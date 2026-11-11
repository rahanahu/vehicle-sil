// SPDX-License-Identifier: MIT
#include "vehicle_sil/core/participant_runtime.hpp"

#include <silkit/services/orchestration/all.hpp>
#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

namespace vehicle_sil {
namespace {

namespace orch = SilKit::Services::Orchestration;

bool is_terminal_or_stopping(orch::ParticipantState state) noexcept
{
    switch (state)
    {
    case orch::ParticipantState::Stopping:
    case orch::ParticipantState::Stopped:
    case orch::ParticipantState::Error:
    case orch::ParticipantState::ShuttingDown:
    case orch::ParticipantState::Shutdown:
    case orch::ParticipantState::Aborting:
        return true;
    default:
        return false;
    }
}

} // namespace

bool is_running(const PeerStatus& status) noexcept
{
    return status.state == orch::ParticipantState::Running;
}

bool has_finished(const PeerStatus& status) noexcept
{
    return status.state && is_terminal_or_stopping(*status.state);
}

ParticipantRuntime::ParticipantRuntime(ParticipantRuntimeOptions options) :
  options_(std::move(options))
{
    if (options_.participant_name.empty())
    {
        throw std::invalid_argument("participant_name must not be empty");
    }
    // Validate before CreateParticipant(): once handlers that capture `this` are registered, a
    // throwing constructor would destroy members the SIL Kit threads may still use.
    if (options_.virtual_time)
    {
        const auto& vt = *options_.virtual_time;
        if (vt.step <= SimDuration::zero())
        {
            throw std::invalid_argument("virtual_time.step must be > 0");
        }
        if (vt.real_time_factor)
        {
            pacer_.emplace(*vt.real_time_factor); // validates the factor
        }
    }

    auto config = SilKit::Config::ParticipantConfigurationFromString(options_.participant_configuration);
    participant_ = SilKit::CreateParticipant(config, options_.participant_name, options_.registry_uri);

    // Handlers registered below capture `this`. If a later call throws, destroy the
    // participant (which removes them) before the members they use go away.
    try
    {
        monitor_ = participant_->CreateSystemMonitor();
        // Only the participant status handler is used. ISystemMonitor::SetParticipantConnectedHandler
        // and SetParticipantDisconnectedHandler are not: in SIL Kit v5.0.8 they assign the handler
        // without synchronisation while the I/O thread may already be calling it (a data race found
        // with ThreadSanitizer, see docs/dependencies.md). A lost connection still shows up here as
        // ParticipantState::Error.
        monitor_->AddParticipantStatusHandler([this](const orch::ParticipantStatus& status) { on_status(status, false); });

        const auto mode = options_.lifecycle_mode == LifecycleMode::Coordinated ? orch::OperationMode::Coordinated
                                                                                 : orch::OperationMode::Autonomous;
        lifecycle_ = participant_->CreateLifecycleService({mode});

        if (options_.virtual_time)
        {
            time_sync_ = lifecycle_->CreateTimeSyncService();
            // The asynchronous variant, completed at the end of the handler. SIL Kit runs no
            // other handler of this participant while the step runs (it runs the handlers on its
            // I/O thread). CompleteSimulationStep() defers the completion on that thread, behind
            // everything the step queued: the messages sent in the step, and also the messages
            // SIL Kit sends while it delivers those back to this participant (a LIN master's own
            // frame is generated when its header comes back to it). All of them go out before the
            // participant announces its next step. The blocking variant announces the next step
            // before that second round, and may even run the next step from a batch of received
            // messages before it.
            time_sync_->SetSimulationStepHandlerAsync(
                [this](std::chrono::nanoseconds now, std::chrono::nanoseconds duration) {
                    struct CompleteOnExit
                    {
                        SilKit::Services::Orchestration::ITimeSyncService* time_sync;
                        bool enabled = true; // false: a StepHold keeps the step until release_step()
                        ~CompleteOnExit()
                        {
                            if (!enabled)
                            {
                                return;
                            }
                            try
                            {
                                time_sync->CompleteSimulationStep();
                            }
                            catch (const std::exception&)
                            {
                                // Only queues a task; nothing to recover if even that fails.
                            }
                        }
                    } complete{time_sync_};
                    complete.enabled = on_step(now, duration);
                },
                options_.virtual_time->step);
        }
        else
        {
            // Without time synchronisation no step proves that the lifecycle got to Running;
            // this handler does (start()). SIL Kit calls it only without a TimeSyncService.
            lifecycle_->SetStartingHandler([this] { starting_entered_.store(true, std::memory_order_release); });
        }
    }
    catch (...)
    {
        participant_.reset();
        throw;
    }
}

ParticipantRuntime::~ParticipantRuntime()
{
    stop("ParticipantRuntime destroyed");
    // Destroying the participant removes all handlers registered above before `this` dies.
    participant_.reset();
}

void ParticipantRuntime::set_step_handler(StepHandler handler)
{
    if (!time_sync_)
    {
        throw std::logic_error("ParticipantRuntime::set_step_handler() needs virtual_time");
    }
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::set_step_handler() must be called before start()");
    }
    step_handler_ = std::move(handler);
}

void ParticipantRuntime::set_communication_ready_handler(std::function<void()> handler)
{
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::set_communication_ready_handler() must be called before start()");
    }
    lifecycle_->SetCommunicationReadyHandler(std::move(handler));
}

void ParticipantRuntime::start(std::chrono::milliseconds timeout)
{
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::start() called twice");
    }
    if (time_sync_ && !step_handler_)
    {
        throw std::logic_error("ParticipantRuntime with virtual_time needs set_step_handler() before start()");
    }
    // Set first: the first step (and a request_stop() from it) can run before StartLifecycle()
    // returns.
    lifecycle_started_.store(true, std::memory_order_release);
    lifecycle_done_ = lifecycle_->StartLifecycle().share();

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        const auto current = lifecycle_->State();
        // A short simulation can pass through Running between two polls (to Stopping or on to
        // Shutdown); a step handler call (virtual time) or the starting handler (otherwise)
        // proves that it got there.
        if (current == orch::ParticipantState::Running || step_entered_.load(std::memory_order_acquire)
            || starting_entered_.load(std::memory_order_acquire))
        {
            deliver_stop(false); // a stop requested before Running
            return;
        }
        if (is_terminal_or_stopping(current))
        {
            throw std::runtime_error("SIL Kit lifecycle of '" + options_.participant_name
                                     + "' failed to start, state: " + orch::to_string(current));
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            throw StartTimeoutError("SIL Kit lifecycle of '" + options_.participant_name
                                     + "' did not reach Running within " + std::to_string(timeout.count())
                                     + " ms, state: " + orch::to_string(current));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void ParticipantRuntime::stop(std::string_view reason) noexcept
{
    if (std::exchange(stopped_, true))
    {
        return;
    }
    // Whatever happens below, the step handler must not run once stop() returns: callers
    // destroy what it uses. A handler that is running is waited for (a long tick() delays
    // stop() accordingly).
    struct DisableSteps
    {
        ParticipantRuntime& self;
        ~DisableSteps()
        {
            {
                const std::lock_guard gate(self.step_gate_);
                self.steps_disabled_ = true;
            }
            // Waits for a StepHold that is deciding, and keeps later ones from running.
            const std::lock_guard hold(self.hold_mutex_);
            self.hold_disabled_ = true;
        }
    } disableSteps{*this};
    if (!lifecycle_done_)
    {
        return;
    }
    {
        const std::lock_guard lock(control_mutex_);
        if (!stop_requested_)
        {
            stop_requested_ = true;
            try
            {
                stop_reason_ = reason;
            }
            catch (const std::exception&)
            {
            }
        }
    }
    stop_cv_.notify_all();
    deliver_stop(true); // this participant ends now: no waiting for a first step
    // The lifecycle proceeds to Shutdown on its own after Stop(), in both modes and from Error.
    const auto done = *lifecycle_done_; // own copy: wait_until_finished() may use it meanwhile
    try
    {
        if (!final_state() && done.wait_for(options_.shutdown_timeout) == std::future_status::ready)
        {
            set_final_state(done.get());
        }
    }
    catch (const std::exception&)
    {
        // The lifecycle ended by an error or abort.
        // Shutdown is best effort: the participant is destroyed (and disconnected) afterwards.
        set_final_state(lifecycle_->State());
    }
}

std::optional<orch::ParticipantState> ParticipantRuntime::final_state() const
{
    const std::lock_guard lock(final_mutex_);
    return final_state_;
}

void ParticipantRuntime::set_final_state(orch::ParticipantState state)
{
    const std::lock_guard lock(final_mutex_);
    if (!final_state_)
    {
        final_state_ = state;
    }
}

void ParticipantRuntime::request_stop(std::string_view reason) noexcept
{
    {
        const std::lock_guard lock(control_mutex_);
        if (!stop_requested_)
        {
            stop_requested_ = true;
            try
            {
                stop_reason_ = reason;
            }
            catch (const std::exception&)
            {
                // The reason is informative; an empty one is fine.
            }
        }
    }
    stop_cv_.notify_all(); // ends a pacing wait early
    deliver_stop(false);
}

void ParticipantRuntime::deliver_stop(bool now) noexcept
{
    if (stop_delivered_.load(std::memory_order_acquire) || !stop_requested())
    {
        return;
    }
    // lifecycle_done_ may still be being assigned by start() on another thread.
    if (!started())
    {
        return;
    }
    try
    {
        const auto state = lifecycle_->State();
        const bool running = state == orch::ParticipantState::Running || state == orch::ParticipantState::Paused;
        // With virtual time, a request waits for the first step: a participant can be Running
        // while others (the system controller among them) are not there yet, and a stop then
        // can leave those stuck before Running; the first step needs every synchronised
        // participant to have started its time. stop() does not wait.
        if (running && !now && time_sync_ && !step_entered_.load(std::memory_order_acquire))
        {
            return;
        }
        // Error is included: SIL Kit keeps a participant in Error until it is stopped or
        // aborted, and Stop() takes it from Error to Shutdown.
        if (running || state == orch::ParticipantState::Error)
        {
            if (stop_delivered_.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }
            std::string reason;
            {
                const std::lock_guard lock(control_mutex_);
                reason = stop_reason_;
            }
            lifecycle_->Stop(std::move(reason));
        }
        else if (is_terminal_or_stopping(state))
        {
            stop_delivered_.store(true, std::memory_order_release); // it ends anyway
        }
        // Before Running, SIL Kit ignores Stop() (with a warning): the request is kept and
        // delivered by start() once Running is reached or, with virtual time, by the first step.
    }
    catch (const std::exception&)
    {
        // Stop() may throw if the state machine cannot stop from its current state; the
        // caller then waits or destroys the participant, which disconnects it.
    }
}

std::optional<orch::ParticipantState> ParticipantRuntime::wait_until_finished(std::chrono::milliseconds timeout)
{
    if (auto state = final_state())
    {
        return state;
    }
    if (!lifecycle_done_)
    {
        return std::nullopt;
    }
    const auto done = *lifecycle_done_; // own copy: stop() may use it meanwhile
    try
    {
        // The future completes at Shutdown. A participant in Error stays there until someone
        // stops or aborts it, so Error also counts as finished (stop() then shuts it down).
        constexpr auto kPoll = std::chrono::milliseconds(20);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        // wait_for() returns at once for a non-positive duration.
        while (done.wait_for(std::min<std::chrono::steady_clock::duration>(
                   kPoll, deadline - std::chrono::steady_clock::now()))
               != std::future_status::ready)
        {
            if (lifecycle_->State() == orch::ParticipantState::Error)
            {
                return orch::ParticipantState::Error;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return std::nullopt;
            }
        }
        set_final_state(done.get());
    }
    catch (const std::exception&)
    {
        // The lifecycle ended by an error or abort.
        set_final_state(lifecycle_->State());
    }
    return final_state();
}

bool ParticipantRuntime::stop_requested() const
{
    const std::lock_guard lock(control_mutex_);
    return stop_requested_;
}

std::string ParticipantRuntime::step_error() const
{
    const std::lock_guard lock(control_mutex_);
    return step_error_;
}

bool ParticipantRuntime::on_step(std::chrono::nanoseconds now, std::chrono::nanoseconds duration)
{
    const auto wallStart = pacer_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const SimTime time = from_silkit_time(now);
    now_ns_.store(now.count(), std::memory_order_release);
    step_entered_.store(true, std::memory_order_release);
    if (!stop_delivered_.load(std::memory_order_acquire))
    {
        deliver_stop(false); // a stop requested before Running: this step is the first chance
    }
    {
        const std::lock_guard gate(step_gate_);
        // Only this thread writes step_error_, so reading it here needs no lock.
        if (steps_disabled_ || !step_error_.empty())
        {
            return true;
        }
        if (const auto& end = options_.virtual_time->end; !end || time < *end)
        {
            if (!run_step_handler(time, duration))
            {
                return true;
            }
            steps_completed_.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    if (pacer_)
    {
        pace_until(pacer_->step_deadline(time, duration, wallStart));
    }
    if (!step_hold_ || stop_requested())
    {
        return true;
    }
    // held_ is set before the hold decides, under hold_mutex_: a release_step() that the hold
    // triggers on another thread waits for the decision and then finds the step held.
    const std::lock_guard lock(hold_mutex_);
    if (hold_disabled_)
    {
        return true;
    }
    held_ = true;
    bool hold = false;
    try
    {
        hold = step_hold_(time, duration);
    }
    catch (...)
    {
        hold = false; // a failing hold must not stall the simulation
    }
    held_ = hold;
    return !hold;
}

void ParticipantRuntime::set_step_hold(StepHold hold)
{
    if (!time_sync_)
    {
        throw std::logic_error("ParticipantRuntime::set_step_hold() needs virtual_time");
    }
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::set_step_hold() must be called before start()");
    }
    step_hold_ = std::move(hold);
}

void ParticipantRuntime::release_step() noexcept
{
    const std::lock_guard lock(hold_mutex_);
    if (!std::exchange(held_, false))
    {
        return;
    }
    try
    {
        time_sync_->CompleteSimulationStep(); // queues the completion on the SIL Kit thread
    }
    catch (const std::exception&)
    {
        // Only queues a task; nothing to recover if even that fails.
    }
}

bool ParticipantRuntime::run_step_handler(SimTime now, SimDuration step)
{
    std::string error;
    try
    {
        step_handler_(now, step);
        return true;
    }
    catch (const std::exception& e)
    {
        error = *e.what() != '\0' ? e.what() : "exception without message";
    }
    catch (...)
    {
        error = "unknown exception";
    }
    error = "step handler at t=" + std::to_string(to_ns(now)) + " ns failed: " + error;
    {
        const std::lock_guard lock(control_mutex_);
        step_error_ = error;
    }
    try
    {
        lifecycle_->ReportError(error);
    }
    catch (const std::exception&)
    {
        // Already in a state that cannot report errors; the simulation is ending anyway.
    }
    return false;
}

void ParticipantRuntime::pace_until(std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock(control_mutex_);
    stop_cv_.wait_until(lock, deadline, [this] { return stop_requested_; });
}

orch::ParticipantState ParticipantRuntime::state() const
{
    return lifecycle_->State();
}

PeerStatus ParticipantRuntime::peer_status(const std::string& participant_name) const
{
    const std::lock_guard lock(peers_mutex_);
    const auto it = peers_.find(participant_name);
    return it == peers_.end() ? PeerStatus{} : it->second.status;
}

bool ParticipantRuntime::wait_for_peer(const std::string& participant_name,
                                       const std::function<bool(const PeerStatus&)>& predicate,
                                       std::chrono::milliseconds timeout)
{
    constexpr auto kResyncInterval = std::chrono::milliseconds(200);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        {
            std::unique_lock lock(peers_mutex_);
            const auto now = std::chrono::steady_clock::now();
            const auto until = std::min(deadline, now + kResyncInterval);
            const bool holds = peers_cv_.wait_until(lock, until, [&] {
                const auto it = peers_.find(participant_name);
                return predicate(it == peers_.end() ? PeerStatus{} : it->second.status);
            });
            if (holds)
            {
                return true;
            }
            if (now >= deadline)
            {
                return false;
            }
        }
        resync_peers(); // outside peers_mutex_: the replay calls on_status()
    }
}

void ParticipantRuntime::resync_peers()
{
    // AddParticipantStatusHandler() replays the current status of every connected participant to
    // the new handler synchronously, under SIL Kit's own lock.
    const auto id =
        monitor_->AddParticipantStatusHandler([this](const orch::ParticipantStatus& status) { on_status(status, true); });
    monitor_->RemoveParticipantStatusHandler(id);
}

void ParticipantRuntime::on_status(const orch::ParticipantStatus& status, bool replayed)
{
    {
        const std::lock_guard lock(peers_mutex_);
        auto& record = peers_[status.participantName];
        // A replayed status may be older than one delivered meanwhile; keep the newer one. Live
        // statuses always apply: a "Connection Lost" Error is stamped with the local clock, which
        // must not be compared with a remote participant's clock.
        if (replayed && record.status.state && status.enterTime < record.enter_time)
        {
            return;
        }
        record.status.state = status.state;
        record.enter_time = status.enterTime;
    }
    peers_cv_.notify_all();
}

} // namespace vehicle_sil
