// SPDX-License-Identifier: MIT
#include "vehicle_sil/core/participant_runtime.hpp"

#include "participant_create.hpp"
#include "participant_runtime_checks.hpp"
#include "participant_runtime_probe.hpp"
#include "vehicle_sil/trace/trace.hpp"

#include <silkit/services/orchestration/all.hpp>
#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

/// Only tests set one (participant_runtime_probe.hpp).
std::atomic<const detail::StartProbe*> g_start_probe{nullptr};

void start_probe(detail::StartProbePoint point)
{
    if (const auto* probe = g_start_probe.load(std::memory_order_acquire))
    {
        (*probe)(point);
    }
}

} // namespace

namespace detail {

void set_start_probe(const StartProbe* probe) noexcept
{
    g_start_probe.store(probe, std::memory_order_release);
}

std::string step_width_error(std::chrono::nanoseconds given, SimDuration own, SimTime now)
{
    if (given == own)
    {
        return {};
    }
    // SIL Kit cut the step (dynamic simulation steps).
    return "SIL Kit gave this participant a step of " + std::to_string(given.count()) + " ns at t=" + std::to_string(to_ns(now)) +
           " ns, not its own " + std::to_string(std::chrono::nanoseconds(own).count()) +
           " ns: this participant's SIL Kit configuration asks for dynamic simulation steps itself, or does not opt out "
           "while another participant asks for them. Set "
           R"("Experimental":{"TimeSynchronization":{"DynamicSimulationStep":false}})"
           " in that configuration, or pass none (the default configuration has it)";
}

} // namespace detail

bool is_running(const PeerStatus& status) noexcept
{
    return status.state == orch::ParticipantState::Running;
}

bool has_finished(const PeerStatus& status) noexcept
{
    return status.state && is_terminal_or_stopping(*status.state);
}

namespace detail {

bool is_name_in_use(std::string_view what) noexcept
{
    return what.find(kNameInUseText) != std::string_view::npos;
}

bool is_join_timeout(std::string_view what) noexcept
{
    return what.find(kJoinTimeoutText) != std::string_view::npos;
}

std::unique_ptr<SilKit::IParticipant> create_participant_retrying(const ParticipantFactory& create, const std::string& name,
                                                                  std::chrono::milliseconds name_in_use_wait,
                                                                  const Notice& notice)
{
    if (name_in_use_wait < std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument("name_in_use_wait must not be negative");
    }
    // Fixed here: a join timeout (about 10 s) that is retried does not give the name more time.
    const auto give_up = std::chrono::steady_clock::now() + name_in_use_wait;
    bool join_retried = false;
    for (;;)
    {
        try
        {
            return create();
        }
        catch (const std::exception& e)
        {
            const std::string what = e.what();
            if (is_join_timeout(what))
            {
                // Once in all: the participant that did not reply has usually just ended, and the
                // registry has seen its end by the time the second attempt asks.
                if (join_retried)
                {
                    throw SilKit::ProtocolError(
                        what + " (participant \"" + name +
                        "\" timed out a second time after it was created once more: a participant that the "
                        "registry lists does not reply; check that the participants this message names are "
                        "running and can be reached from this computer, or end them)");
                }
                join_retried = true;
                notice("participant \"" + name +
                       "\" timed out joining the simulation and is created once more (a participant that had "
                       "just ended was probably still on the registry's list): " +
                       what);
            }
            else if (!is_name_in_use(what))
            {
                throw;
            }
            else if (std::chrono::steady_clock::now() >= give_up)
            {
                throw NameInUseError(
                    what + " (the registry refused the name \"" + name + "\" for " + std::to_string(name_in_use_wait.count()) +
                    " ms: another participant of this simulation uses it, or the registry has not yet seen the end of "
                    "an earlier participant with this name; give each participant a name of its own, or end the other "
                    "one)");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

} // namespace detail

std::unique_ptr<SilKit::IParticipant> create_participant(std::shared_ptr<SilKit::Config::IParticipantConfiguration> config,
                                                         const std::string& name, const std::string& registry_uri,
                                                         std::chrono::milliseconds name_in_use_wait)
{
    return detail::create_participant_retrying(
        [&] { return SilKit::CreateParticipant(config, name, registry_uri); }, name, name_in_use_wait,
        [](std::string_view text) {
            // One write: output of other threads (SIL Kit's log, the ECU) does not land inside the line.
            std::string line = "vehicle_sil: ";
            line.append(text).push_back('\n');
            std::cerr << line;
        });
}

ParticipantRuntime::ParticipantRuntime(ParticipantRuntimeOptions options) :
  options_(std::move(options))
{
    if (options_.participant_name.empty())
    {
        throw std::invalid_argument("participant_name must not be empty");
    }
    trace::set_process_name(options_.participant_name);
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

    auto config = SilKit::Config::ParticipantConfigurationFromString(merged_participant_configuration(
        options_.participant_configuration, options_.participant_name, options_.registry_uri, options_.listen_on_network));
    participant_ = create_participant(config, options_.participant_name, options_.registry_uri, options_.name_in_use_wait);

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
        // Always: also a participant that is given no handler has SIL Kit take its errors from
        // here on (open_error_reporting()). set_communication_ready_handler() and
        // set_communication_ready_handler_async() replace this one.
        lifecycle_->SetCommunicationReadyHandler([this] {
            open_error_reporting();
            const std::lock_guard gate(communication_ready_gate_);
            if (!communication_ready_disabled_)
            {
                run_added_communication_ready();
            }
        });

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
            lifecycle_->SetStartingHandler([this] {
                starting_entered_.store(true, std::memory_order_release);
                wake_waiters();
                start_probe(detail::StartProbePoint::StartingHandlerEntered);
            });
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
    lifecycle_->SetCommunicationReadyHandler([this, handler = std::move(handler)] {
        open_error_reporting();
        const std::lock_guard gate(communication_ready_gate_);
        if (communication_ready_disabled_)
        {
            return; // stop() has returned: what the handlers use may be gone
        }
        run_added_communication_ready();
        if (handler)
        {
            handler();
        }
    });
}

void ParticipantRuntime::add_communication_ready_handler(std::function<void()> handler)
{
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::add_communication_ready_handler() must be called before start()");
    }
    // Run by the handler that SIL Kit has (the constructor's, or the one that was set).
    added_communication_ready_.push_back(std::move(handler));
}

void ParticipantRuntime::run_added_communication_ready()
{
    for (const auto& handler : added_communication_ready_)
    {
        std::string error;
        try
        {
            if (handler)
            {
                handler();
            }
            continue;
        }
        catch (const std::exception& e)
        {
            error = *e.what() != '\0' ? e.what() : "exception without message";
        }
        catch (...)
        {
            error = "unknown exception";
        }
        report_failure("CommunicationReady handler failed: " + error);
    }
}

void ParticipantRuntime::set_communication_ready_handler_async(std::function<void()> handler)
{
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::set_communication_ready_handler_async() must be called before start()");
    }
    communication_ready_ = std::move(handler);
    lifecycle_->SetCommunicationReadyHandlerAsync([this] {
        open_error_reporting();
        const std::lock_guard gate(communication_ready_gate_);
        const bool disabled = communication_ready_disabled_;
        if (!disabled)
        {
            run_added_communication_ready();
        }
        {
            // Before the handler: a thread it wakes may complete at once. The lock also orders
            // SIL Kit's own (unsynchronised) record of the call before the completing thread's
            // read of it.
            const std::lock_guard lock(communication_ready_mutex_);
            communication_ready_state_ = CommunicationReady::Open;
        }
        if (disabled)
        {
            // stop() has returned: nobody is told, so nobody would complete. Completed here, the
            // lifecycle goes on as after a handler that is not asynchronous, to where the stop
            // can reach it. SIL Kit only queues the transition, so this is fine from its handler.
            complete_communication_ready();
            return;
        }
        if (communication_ready_)
        {
            communication_ready_();
        }
    });
}

bool ParticipantRuntime::complete_communication_ready() noexcept
{
    const std::lock_guard lock(communication_ready_mutex_);
    if (communication_ready_state_ != CommunicationReady::Open)
    {
        return false;
    }
    communication_ready_state_ = CommunicationReady::Completed;
    try
    {
        lifecycle_->CompleteCommunicationReadyHandlerAsync(); // queues the transition on the SIL Kit thread
    }
    catch (const std::exception&)
    {
        // Only queues a task; nothing to recover if even that fails.
    }
    return true;
}

void ParticipantRuntime::set_peer_status_observer(std::function<void(const std::string&, const PeerStatus&)> observer)
{
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::set_peer_status_observer() must be called before start()");
    }
    // The SIL Kit thread may deliver a status (on_status()) already.
    const std::lock_guard lock(peer_observer_mutex_);
    peer_observer_ = std::move(observer);
}

void ParticipantRuntime::start(std::chrono::milliseconds timeout, const std::function<std::string()>& give_up)
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
    // SIL Kit takes a reported error from StartLifecycle() on. report_failure() gives it one from
    // here or from the head of the CommunicationReady handler on, whichever comes first.
    // control_mutex_ is not held across StartLifecycle(): that registers status handlers under SIL
    // Kit's handler lock, which the SIL Kit thread holds while it calls on_status(), which takes
    // control_mutex_.
    start_probe(detail::StartProbePoint::AfterStartLifecycle);
    open_error_reporting();

    const auto deadline = timeout == kNoStartLimit ? std::chrono::steady_clock::time_point::max()
                                                   : std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        // Capture generations before reading flags and state so a concurrent transition on the
        // SIL Kit thread cannot be missed between checking and waiting on peers_cv_.
        std::uint64_t peerGen = 0;
        std::uint64_t wakeGen = 0;
        {
            const std::lock_guard lock(peers_mutex_);
            peerGen = peer_generation_;
            wakeGen = wake_generation_;
        }
        // A short simulation can pass through Running between two polls (to Stopping or on to
        // Shutdown); a step handler call (virtual time) or the starting handler (otherwise)
        // proves that it got there. The starting handler runs in ReadyToRun, and SIL Kit sets
        // Running only after it returned: until then, state() would still say ReadyToRun.
        //
        // The order of the reads matters: the flags first, the state after them. SIL Kit sets
        // ReadyToRun before it calls the starting handler and never goes back from there, so a
        // state read after the flag was seen set is ReadyToRun or a later one, and "not
        // ReadyToRun" then means Running or beyond. With the state read first, the SIL Kit thread
        // could set ReadyToRun and the flag between the two reads, and start() would return on
        // an older state while state() says ReadyToRun.
        const bool runningEntered = running_entered_.load(std::memory_order_acquire);
        const bool stepEntered = step_entered_.load(std::memory_order_acquire);
        const bool startingEntered = starting_entered_.load(std::memory_order_acquire);
        start_probe(detail::StartProbePoint::BetweenFlagsAndState);
        const auto current = lifecycle_->State();
        if (failed_before_start_.load(std::memory_order_acquire))
        {
            // SIL Kit put this participant into Error before it ran (on_status()). With a SIL Kit
            // that lacks patches/sil-kit/0006 (the consumer's own SilKit::SilKit), the state may
            // say something else by now: a step of the start that was under way when the error
            // came (the wait for the other participants' replies, for the subscriptions) sets the
            // next state when it completes, whatever the state is then, and the lifecycle goes on
            // from there, to Running if the simulation starts, or it stays where the simulation
            // has passed already. Either way this participant failed: it must not take part, and
            // must not keep the others waiting for it. The patched SIL Kit stays in Error, and
            // this is the same as the check of the state below.
            throw std::runtime_error("SIL Kit lifecycle of '" + options_.participant_name + "' failed to start, state: " +
                                     orch::to_string(orch::ParticipantState::Error) +
                                     (current == orch::ParticipantState::Error
                                          ? std::string()
                                          : " (the state is " + orch::to_string(current) + " now)"));
        }
        if (current == orch::ParticipantState::Running || runningEntered || stepEntered
            || (startingEntered && current != orch::ParticipantState::ReadyToRun))
        {
            deliver_stop(false); // a stop requested before Running
            return;
        }
        if (is_terminal_or_stopping(current))
        {
            // The other side of the same order: the lifecycle can have gone through Running and
            // on to this state after the flags were read (or been aborted right after Running,
            // before any step ran). They never go back, so they say now whether it did: that is a
            // start that succeeded, not one that failed.
            if (running_entered_.load(std::memory_order_acquire) || step_entered_.load(std::memory_order_acquire)
                || starting_entered_.load(std::memory_order_acquire))
            {
                deliver_stop(false);
                return;
            }
            throw std::runtime_error("SIL Kit lifecycle of '" + options_.participant_name
                                     + "' failed to start, state: " + orch::to_string(current));
        }
        if (give_up)
        {
            // give_up() looks at the simulation after the looks above, and the simulation goes on
            // meanwhile: a short one can have begun and ended, and a participant that is stopping
            // then looks to the caller like one that left before the start. The flags say whether
            // this one began: if so, the next round decides (it returns, or waits for Running
            // while the starting handler is still there), and "give up" is not taken.
            if (std::string why = give_up();
                !why.empty() && !running_entered_.load(std::memory_order_acquire)
                && !step_entered_.load(std::memory_order_acquire)
                && !starting_entered_.load(std::memory_order_acquire))
            {
                throw std::runtime_error("SIL Kit lifecycle of '" + options_.participant_name +
                                         "': the start was given up, state: " + orch::to_string(current) + ": " + why);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            throw StartTimeoutError("SIL Kit lifecycle of '" + options_.participant_name
                                     + "' did not reach Running within " + std::to_string(timeout.count())
                                     + " ms, state: " + orch::to_string(current));
        }
        // Wake immediately on own lifecycle transitions; bound the slice only while give_up needs
        // periodic rechecks or while SIL Kit's starting handler is running in ReadyToRun. Otherwise
        // a coarse 1 s cap: kNoStartLimit makes `deadline` time_point::max(), which a libstdc++
        // without pthread_cond_clockwait (MinGW) converts to system_clock and could overflow.
        const auto waitUntil = (give_up || (startingEntered && current == orch::ParticipantState::ReadyToRun))
                                   ? std::min(deadline, now + std::chrono::milliseconds(5))
                                   : std::min(deadline, now + std::chrono::seconds(1));
        std::unique_lock lock(peers_mutex_);
        peers_cv_.wait_until(lock, waitUntil, [&] {
            return peer_generation_ != peerGen || wake_generation_ != wakeGen;
        });
    }
}

void ParticipantRuntime::stop(std::string_view reason) noexcept
{
    if (std::exchange(stopped_, true))
    {
        return;
    }
    // Whatever happens below, the step handler and the CommunicationReady handlers must not run
    // once stop() returns: callers destroy what they use. A handler that is running is waited
    // for (a long tick() delays stop() accordingly).
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
            {
                const std::lock_guard hold(self.hold_mutex_);
                self.hold_disabled_ = true;
            }
            // Last, when the wait for the shutdown is over: a lifecycle that gets to
            // CommunicationReady while stop() waits (a required participant that came late) still
            // has its controllers started and its start window opened, and goes on to where the
            // stop reaches it. Only after stop() can it come too late: after a start that timed
            // out, SIL Kit takes no stop and the lifecycle lives until the participant is
            // destroyed.
            const std::lock_guard ready(self.communication_ready_gate_);
            self.communication_ready_disabled_ = true;
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
    notify_stop_observer();
    {
        // A step that is failing now reports its error before the stop goes out (see
        // deliver_stop()). The caller may have seen the failure before the step handler
        // returned: Peer::failed() looks at the Host, which fails inside the handler. No
        // handler starts afterwards, so no failure can begin after this point either: without
        // that, a step that started right after the gate was let go and failed would report
        // its error behind the Stop() that deliver_stop() queued from Running.
        const std::lock_guard gate(step_gate_);
        steps_disabled_ = true;
    }
    // One deadline for the Error (deliver_stop()) and the Shutdown below.
    const auto deadline = std::chrono::steady_clock::now() + options_.shutdown_timeout;
    deliver_stop(true, deadline); // this participant ends now: no waiting for a first step
    // Before Running (and with no failure pending), SIL Kit ignores Stop(), so deliver_stop()
    // sent nothing and waiting out shutdown_timeout would only stall until the participant is
    // destroyed. When an asynchronous CommunicationReady handler is registered (a controller's
    // start window), wait briefly while lifecycle/peer activity continues so an in-flight start
    // can open and complete the window, reach Running, and shut down peers cleanly; break early
    // once the lifecycle goes idle before Running.
    const auto pre_running_without_stop = [this] {
        return !stop_delivered_.load(std::memory_order_acquire) && !has_failed()
               && !failed_before_start_.load(std::memory_order_acquire);
    };
    if (pre_running_without_stop() && !communication_ready_)
    {
        return;
    }
    const auto read_activity = [this] {
        const std::lock_guard lock(peers_mutex_);
        return std::pair{peer_generation_, wake_generation_};
    };
    // The lifecycle proceeds to Shutdown on its own after Stop(), in both modes and from Error.
    const auto done = *lifecycle_done_; // own copy: wait_until_finished() may use it meanwhile
    try
    {
        // SIL Kit sets the future at Shutdown, once the others have replied. After a start that
        // SIL Kit failed (failed_before_start_), it may never set it: a Stop() that comes while
        // the start still waits for the others' replies has its own wait for them dropped (SIL
        // Kit takes one such wait at a time), and the lifecycle stays in Shutdown. Shutdown then
        // ends the wait too. Otherwise only the future does: a normal stop still waits for the
        // replies, which make sure the others got this participant's last messages.
        constexpr auto kIdlePreRunningLimit = std::chrono::milliseconds(500);
        auto lastActivityGen = read_activity();
        auto lastActivityAt = std::chrono::steady_clock::now();
        while (!final_state())
        {
            const auto now = std::chrono::steady_clock::now();
            if (done.wait_until(std::min(deadline, now + std::chrono::milliseconds(5))) == std::future_status::ready)
            {
                set_final_state(done.get());
            }
            else if (failed_before_start_.load(std::memory_order_acquire)
                     && lifecycle_->State() == orch::ParticipantState::Shutdown)
            {
                set_final_state(orch::ParticipantState::Shutdown); // the value the future has at Shutdown
            }
            else if (std::chrono::steady_clock::now() >= deadline)
            {
                break;
            }
            else if (pre_running_without_stop())
            {
                const auto gen = read_activity();
                if (gen != lastActivityGen)
                {
                    lastActivityGen = gen;
                    lastActivityAt = std::chrono::steady_clock::now();
                }
                else if (std::chrono::steady_clock::now() - lastActivityAt >= kIdlePreRunningLimit)
                {
                    break;
                }
            }
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
    notify_stop_observer();
    deliver_stop(false);
}

void ParticipantRuntime::deliver_stop(bool now, std::chrono::steady_clock::time_point deadline) noexcept
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
        const auto is_running_or_paused = [](orch::ParticipantState s) {
            return s == orch::ParticipantState::Running || s == orch::ParticipantState::Paused;
        };
        auto state = lifecycle_->State();
        // A participant that failed (its step handler, or report_failure()) leaves through
        // Error. ReportError() and Stop() are both carried out later on the SIL Kit thread; a
        // Stop() taken there while still Running would be an ordinary stop (in Coordinated
        // mode, of the whole simulation), and nobody would see the failure. So no Stop() from
        // Running after a failure: a request waits for the Error (on_status() delivers it),
        // stop() waits here until `deadline`. Nor while a step handler runs, which may be
        // failing (the caller may have seen it fail): a request then goes out when the handler
        // has returned (on_step()); stop() waited for the handler already.
        //
        // The failure flag is set and ReportError() queued under one lock, so the error is
        // queued before any Stop() that this function could queue after seeing the flag. The
        // two branches under has_failed() are what makes the result independent of the order
        // in which SIL Kit carries out queued calls: asio does not promise the order of
        // post(), and it was not confirmed for Windows. Where queued calls run in order (seen
        // on Linux), removing the branches changes nothing that a test can observe.
        if (!now && is_running_or_paused(state) && handler_running_.load(std::memory_order_acquire))
        {
            return;
        }
        if (is_running_or_paused(state) && has_failed())
        {
            if (!now)
            {
                return;
            }
            while (is_running_or_paused(state))
            {
                std::uint64_t peerGen = 0;
                std::uint64_t wakeGen = 0;
                {
                    const std::lock_guard lock(peers_mutex_);
                    peerGen = peer_generation_;
                    wakeGen = wake_generation_;
                }
                state = lifecycle_->State();
                if (!is_running_or_paused(state) || std::chrono::steady_clock::now() >= deadline)
                {
                    break;
                }
                std::unique_lock lock(peers_mutex_);
                peers_cv_.wait_until(lock, deadline, [&] {
                    return peer_generation_ != peerGen || wake_generation_ != wakeGen;
                });
                lock.unlock();
                state = lifecycle_->State();
            }
        }
        const bool running = is_running_or_paused(state);
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
        // delivered once Running is reached (by start(), or by on_status() when start() does not
        // wait any more) or, with virtual time, by the first step.
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

std::optional<orch::ParticipantState> ParticipantRuntime::wait_until_finished(
    std::chrono::milliseconds timeout, const std::function<bool()>& should_wake)
{
    if (!should_wake)
    {
        return wait_until_finished(timeout);
    }
    if (auto state = final_state())
    {
        return state;
    }
    if (!lifecycle_done_)
    {
        return std::nullopt;
    }
    const auto done = *lifecycle_done_; // own copy: stop() may use it meanwhile
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    try
    {
        while (true)
        {
            std::uint64_t peer_gen = 0;
            std::uint64_t wake_gen = 0;
            {
                const std::lock_guard lock(peers_mutex_);
                peer_gen = peer_generation_;
                wake_gen = wake_generation_;
            }
            if (done.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            {
                set_final_state(done.get());
                return final_state();
            }
            const auto current = lifecycle_->State();
            if (current == orch::ParticipantState::Error)
            {
                return orch::ParticipantState::Error;
            }
            if (should_wake())
            {
                return std::nullopt;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                return std::nullopt;
            }
            // Once the lifecycle is already stopping or shut down, wait directly on the future
            // for its completion instead of sleeping on peers_cv_.
            if (is_terminal_or_stopping(current))
            {
                constexpr auto kPoll = std::chrono::milliseconds(20);
                if (done.wait_for(std::min<std::chrono::steady_clock::duration>(kPoll, deadline - now))
                    == std::future_status::ready)
                {
                    set_final_state(done.get());
                    return final_state();
                }
                continue;
            }
            std::unique_lock lock(peers_mutex_);
            peers_cv_.wait_until(lock, deadline, [&] {
                return peer_generation_ != peer_gen || wake_generation_ != wake_gen;
            });
        }
    }
    catch (const std::exception&)
    {
        set_final_state(lifecycle_->State());
    }
    return final_state();
}

void ParticipantRuntime::wake_waiters() noexcept
{
    {
        const std::lock_guard lock(peers_mutex_);
        ++wake_generation_;
    }
    peers_cv_.notify_all();
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

bool ParticipantRuntime::has_failed() const
{
    const std::lock_guard lock(control_mutex_);
    return failed_;
}

void ParticipantRuntime::open_error_reporting() noexcept
{
    try
    {
        const std::lock_guard lock(control_mutex_);
        error_reportable_ = true;
        if (early_failure_ && !failed_)
        {
            // Something failed while start() was in StartLifecycle(): report it now. As in
            // report_failure(): ReportError() only queues a call, and the flag is set after it.
            lifecycle_->ReportError(*early_failure_);
            failed_ = true;
        }
        early_failure_.reset(); // reported, or a failed step was reported before it (only the first is)
    }
    catch (const std::exception&)
    {
        // As in report_failure().
    }
}

void ParticipantRuntime::report_failure(std::string_view reason) noexcept
{
    try
    {
        std::string text(reason);
        // The flag and the queued error together (deliver_stop()): whoever sees the flag can
        // count on the Error to come. ReportError() only queues a call for the SIL Kit thread
        // (Participant::ExecuteDeferred() is asio::post()), so nothing runs under the lock.
        const std::lock_guard lock(control_mutex_);
        if (!error_reportable_)
        {
            // SIL Kit ignores an error before StartLifecycle(); so does the stop logic. One that
            // comes while start() is in StartLifecycle() is kept for open_error_reporting().
            if (lifecycle_started_.load(std::memory_order_acquire) && !early_failure_)
            {
                early_failure_ = std::move(text);
            }
            return;
        }
        if (failed_)
        {
            return; // only the first failure is reported
        }
        lifecycle_->ReportError(std::move(text));
        failed_ = true;
    }
    catch (const std::exception&)
    {
        // Already in a state that cannot report errors; the simulation is ending anyway.
    }
}

bool ParticipantRuntime::on_step(std::chrono::nanoseconds now, std::chrono::nanoseconds duration)
{
    const auto wallStart = pacer_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const SimTime time = from_silkit_time(now);
    // The profiling trace (docs/internals/trace.md): the whole step on SIL Kit's thread, to the
    // hold's decision; its argument says whether the step was held.
    trace::Span traceStep("step", now.count());
    trace::set_thread_name("silkit");
    now_ns_.store(now.count(), std::memory_order_release);
    if (!step_entered_.load(std::memory_order_relaxed))
    {
        step_entered_.store(true, std::memory_order_release);
        wake_waiters();
    }
    if (!stop_delivered_.load(std::memory_order_acquire))
    {
        deliver_stop(false); // a stop requested before Running: this step is the first chance
    }
    {
        const std::lock_guard gate(step_gate_);
        // Only this thread writes step_error_, so reading it here needs no lock.
        // A participant that failed before it began (start() throws) does not run its steps if
        // SIL Kit lets it go on meanwhile.
        if (steps_disabled_ || !step_error_.empty() || failed_before_start_.load(std::memory_order_acquire))
        {
            return true;
        }
        if (const auto& end = options_.virtual_time->end; !end || time < *end)
        {
            struct Running
            {
                std::atomic<bool>& flag;
                explicit Running(std::atomic<bool>& f) : flag(f) { flag.store(true, std::memory_order_release); }
                ~Running() { flag.store(false, std::memory_order_release); }
                Running(const Running&) = delete;
                Running& operator=(const Running&) = delete;
            };
            bool ok = false;
            if (std::string cut = detail::step_width_error(duration, options_.virtual_time->step, time); !cut.empty())
            {
                // Everything above assumes the fixed step that was asked for (the grid of the
                // steps, TickScheduler, the time gate).
                fail_step(std::move(cut));
            }
            else
            {
                const Running running(handler_running_);
                const trace::Span traceHandler("step.handler", now.count());
                ok = run_step_handler(time, duration);
            }
            if (!stop_delivered_.load(std::memory_order_acquire))
            {
                deliver_stop(false); // a request made while the handler ran (deliver_stop())
            }
            if (!ok)
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
    traceStep.set_arg(hold ? 1 : 0);
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

void ParticipantRuntime::set_stop_observer(std::function<void()> observer)
{
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::set_stop_observer() must be called before start()");
    }
    // The SIL Kit thread may deliver a status (on_status()) already.
    const std::lock_guard lock(stop_observer_mutex_);
    stop_observer_ = std::move(observer);
}

void ParticipantRuntime::notify_stop_observer() noexcept
{
    const std::lock_guard lock(stop_observer_mutex_);
    if (stop_observer_)
    {
        try
        {
            stop_observer_();
        }
        catch (...)
        {
            // An observer must not keep the stop from going on.
        }
    }
}

void ParticipantRuntime::release_step() noexcept
{
    const std::lock_guard lock(hold_mutex_);
    if (!std::exchange(held_, false))
    {
        return;
    }
    if (trace::enabled())
    {
        trace::instant("step.release", now_ns_.load(std::memory_order_acquire));
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
    fail_step("step handler at t=" + std::to_string(to_ns(now)) + " ns failed: " + error);
    return false;
}

void ParticipantRuntime::fail_step(std::string error)
{
    // The flag and the queued error under one lock, as in report_failure(). A step runs only
    // after StartLifecycle(), so SIL Kit takes the error.
    const std::lock_guard lock(control_mutex_);
    step_error_ = error;
    if (std::exchange(failed_, true))
    {
        return; // report_failure() reported a failure already
    }
    try
    {
        lifecycle_->ReportError(std::move(error));
    }
    catch (const std::exception&)
    {
        // Already in a state that cannot report errors; the simulation is ending anyway.
    }
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

std::vector<std::string> ParticipantRuntime::peer_names() const
{
    const std::lock_guard lock(peers_mutex_);
    std::vector<std::string> names;
    for (const auto& [name, record] : peers_)
    {
        if (record.status.state)
        {
            names.push_back(name);
        }
    }
    return names;
}

std::uint64_t ParticipantRuntime::peer_generation() const
{
    const std::lock_guard lock(peers_mutex_);
    return peer_generation_;
}

bool ParticipantRuntime::wait_for_peer_change(std::uint64_t generation,
                                              std::chrono::steady_clock::time_point deadline) const
{
    std::unique_lock lock(peers_mutex_);
    return peers_cv_.wait_until(lock, deadline, [this, generation] { return peer_generation_ != generation; });
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
    // One thread at a time: SIL Kit's C++ API keeps a system monitor's handlers in a table
    // without a lock, which adding and removing change. Not peers_mutex_: the replay calls
    // on_status(), which takes that.
    const std::lock_guard lock(resync_mutex_);
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
        if (record.status.state != status.state)
        {
            ++peer_generation_;
        }
        record.status.state = status.state;
        record.enter_time = status.enterTime;
    }
    peers_cv_.notify_all();
    {
        const std::lock_guard lock(peer_observer_mutex_);
        if (peer_observer_)
        {
            try
            {
                peer_observer_(status.participantName, PeerStatus{status.state});
            }
            catch (...)
            {
                // An observer must not disturb the status handling.
            }
        }
    }
    // This participant's own status comes here too: a stop requested after a step failure
    // waited for the Error (deliver_stop()), and one requested before Running waits for Running.
    // start() passes that one on while it waits, and the first step does with virtual time; when
    // start() has given up (a timeout) and there are no steps, this is the only place left.
    // deliver_stop() keeps a request for the first step where it has to, and passes it on once.
    if (status.participantName == options_.participant_name)
    {
        // An Error before this participant began to run is a start that failed, also if SIL Kit
        // leaves Error again (start()). Only a status as it comes (on the SIL Kit thread, which
        // also sets the flags): a replay may bring an old Error after a run has begun.
        if (!replayed && status.state == orch::ParticipantState::Error && started()
            && !running_entered_.load(std::memory_order_acquire)
            && !step_entered_.load(std::memory_order_acquire)
            && !starting_entered_.load(std::memory_order_acquire))
        {
            failed_before_start_.store(true, std::memory_order_release);
        }
        if (!replayed && status.state == orch::ParticipantState::Running && started()
            && !failed_before_start_.load(std::memory_order_acquire))
        {
            running_entered_.store(true, std::memory_order_release);
        }
        wake_waiters();
        if (has_finished(PeerStatus{status.state}))
        {
            notify_stop_observer(); // also a stop that another participant asked for
        }
        if (status.state == orch::ParticipantState::Error || status.state == orch::ParticipantState::Running)
        {
            deliver_stop(false);
        }
    }
}

} // namespace vehicle_sil
