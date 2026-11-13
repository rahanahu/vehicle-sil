// SPDX-License-Identifier: MIT
#include "vehicle_sil/core/participant_runtime.hpp"

#include <silkit/services/orchestration/all.hpp>
#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <system_error>
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

} // namespace

namespace {

// The one place that says what vehicle-sil changes of SIL Kit's defaults (participant_runtime.hpp
// says why).

/// The host of "silkit://<host>:<port>...", without the brackets of an IPv6 address; empty if
/// `uri` is not of that form.
std::string_view registry_host(std::string_view uri) noexcept
{
    constexpr std::string_view scheme = "silkit://";
    if (uri.substr(0, scheme.size()) != scheme)
    {
        return {};
    }
    uri.remove_prefix(scheme.size());
    if (!uri.empty() && uri.front() == '[')
    {
        const auto end = uri.find(']');
        return end == std::string_view::npos ? std::string_view{} : uri.substr(1, end - 1);
    }
    return uri.substr(0, uri.find_first_of(":/"));
}

bool equals_ignoring_case(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

/// 127.x.x.x in dotted decimal (four numbers from 0 to 255).
bool is_loopback_ipv4(std::string_view host) noexcept
{
    if (std::count(host.begin(), host.end(), '.') != 3)
    {
        return false;
    }
    int first = -1;
    while (true)
    {
        const auto dot = host.find('.');
        const std::string_view part = host.substr(0, dot);
        int value = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || part.size() > 3 || error != std::errc{} || end != part.data() + part.size() || value < 0 || value > 255)
        {
            return false;
        }
        first = first < 0 ? value : first;
        if (dot == std::string_view::npos)
        {
            return first == 127;
        }
        host.remove_prefix(dot + 1);
    }
}

bool listen_on_network_from_environment()
{
    const std::string name(kListenOnNetworkVariable);
    const char* value = std::getenv(name.c_str());
    return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

std::string json_string(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (static_cast<unsigned char>(c) < 0x20)
        {
            constexpr char kHex[] = "0123456789abcdef";
            out += "\\u00";
            out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
            out += kHex[static_cast<unsigned char>(c) & 0xF];
        }
        else
        {
            out += c;
        }
    }
    out += '"';
    return out;
}

#ifndef _WIN32
/// "local://<temporary directory>/vsil-<16 random hex digits>.silkit", or empty if there is no
/// temporary directory. SIL Kit's own name for it is made from the participant's name and the
/// registry URI, which this does not have; a random one is as unique.
std::string new_local_acceptor_uri()
{
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    if (error || directory.empty())
    {
        return {};
    }
    std::random_device device;
    const std::uint64_t value = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    char name[32];
    std::snprintf(name, sizeof name, "vsil-%016llx.silkit", static_cast<unsigned long long>(value));
    return "local://" + (directory / name).string();
}
#endif

/// The members of "Middleware", without the braces.
std::string middleware_members(std::string_view registry_uri, bool listen_on_network)
{
    std::string members;
#ifdef _WIN32
    members = R"("EnableDomainSockets":false)";
#endif
    if (listen_on_network || !is_loopback_registry_uri(registry_uri) || listen_on_network_from_environment())
    {
        return members;
    }
    std::vector<std::string> acceptors;
#ifndef _WIN32
    if (std::string local = new_local_acceptor_uri(); !local.empty())
    {
        acceptors.push_back(std::move(local));
    }
#endif
    // The registry's address family: an IPv6 registry URI may be all that this computer has.
    acceptors.emplace_back(registry_host(registry_uri).find(':') != std::string_view::npos ? "tcp://[::1]:0"
                                                                                           : "tcp://127.0.0.1:0");
    members += members.empty() ? R"("AcceptorUris":[)" : R"(,"AcceptorUris":[)";
    for (std::size_t i = 0; i < acceptors.size(); ++i)
    {
        members += (i == 0 ? "" : ",") + json_string(acceptors[i]);
    }
    members += ']';
    return members;
}

} // namespace

bool is_loopback_registry_uri(std::string_view registry_uri) noexcept
{
    const std::string_view host = registry_host(registry_uri);
    return is_loopback_ipv4(host) || host == "::1" || equals_ignoring_case(host, "localhost");
}

std::string default_participant_configuration(std::string_view registry_uri, bool listen_on_network)
{
    return default_participant_configuration_with(registry_uri, "", listen_on_network);
}

std::string default_participant_configuration_with(std::string_view registry_uri, std::string_view members,
                                                   bool listen_on_network)
{
    const std::string middleware = middleware_members(registry_uri, listen_on_network);
    std::string text = "{";
    if (!middleware.empty())
    {
        text += R"("Middleware":{)" + middleware + "}";
    }
    if (!middleware.empty() && !members.empty())
    {
        text += ',';
    }
    text += members;
    text += '}';
    return text;
}

std::string participant_configuration_or_default(std::string_view configuration, std::string_view registry_uri,
                                                 bool listen_on_network)
{
    constexpr std::string_view space = " \t\r\n";
    const auto first = configuration.find_first_not_of(space);
    const std::string_view text =
        first == std::string_view::npos ? std::string_view{} : configuration.substr(first, configuration.find_last_not_of(space) - first + 1);
    return text.empty() || text == "{}" ? default_participant_configuration(registry_uri, listen_on_network)
                                        : std::string(configuration);
}

std::string_view default_registry_configuration() noexcept
{
#ifdef _WIN32
    return R"({"Middleware":{"EnableDomainSockets":false}})";
#else
    return "{}";
#endif
}

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

    auto config = SilKit::Config::ParticipantConfigurationFromString(participant_configuration_or_default(
        options_.participant_configuration, options_.registry_uri, options_.listen_on_network));
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

void ParticipantRuntime::set_communication_ready_handler_async(std::function<void()> handler)
{
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::set_communication_ready_handler_async() must be called before start()");
    }
    communication_ready_ = std::move(handler);
    lifecycle_->SetCommunicationReadyHandlerAsync([this] {
        {
            // Before the handler: a thread it wakes may complete at once. The lock also orders
            // SIL Kit's own (unsynchronised) record of the call before the completing thread's
            // read of it.
            const std::lock_guard lock(communication_ready_mutex_);
            communication_ready_state_ = CommunicationReady::Open;
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
    {
        // Only now does SIL Kit take a reported error (report_failure()). Not held across
        // StartLifecycle(): that registers status handlers under SIL Kit's handler lock, which
        // the SIL Kit thread holds while it calls on_status(), which takes this lock.
        const std::lock_guard lock(control_mutex_);
        error_reportable_ = true;
    }

    const auto deadline = timeout == kNoStartLimit ? std::chrono::steady_clock::time_point::max()
                                                   : std::chrono::steady_clock::now() + timeout;
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
        if (give_up)
        {
            if (std::string why = give_up(); !why.empty())
            {
                throw std::runtime_error("SIL Kit lifecycle of '" + options_.participant_name +
                                         "': the start was given up, state: " + orch::to_string(current) + ": " + why);
            }
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
    // The lifecycle proceeds to Shutdown on its own after Stop(), in both modes and from Error.
    const auto done = *lifecycle_done_; // own copy: wait_until_finished() may use it meanwhile
    try
    {
        if (!final_state() && done.wait_until(deadline) == std::future_status::ready)
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
            while (is_running_or_paused(state) && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
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

bool ParticipantRuntime::has_failed() const
{
    const std::lock_guard lock(control_mutex_);
    return failed_;
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
            return; // SIL Kit ignores an error before StartLifecycle(); so does the stop logic
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
            struct Running
            {
                std::atomic<bool>& flag;
                explicit Running(std::atomic<bool>& f) : flag(f) { flag.store(true, std::memory_order_release); }
                ~Running() { flag.store(false, std::memory_order_release); }
                Running(const Running&) = delete;
                Running& operator=(const Running&) = delete;
            };
            bool ok = false;
            {
                const Running running(handler_running_);
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
    // The flag and the queued error under one lock, as in report_failure(). A step runs only
    // after StartLifecycle(), so SIL Kit takes the error.
    const std::lock_guard lock(control_mutex_);
    step_error_ = error;
    if (std::exchange(failed_, true))
    {
        return false; // report_failure() reported a failure already
    }
    try
    {
        lifecycle_->ReportError(std::move(error));
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
    // waited for the Error (deliver_stop()).
    if (status.participantName == options_.participant_name)
    {
        if (has_finished(PeerStatus{status.state}))
        {
            notify_stop_observer(); // also a stop that another participant asked for
        }
        if (status.state == orch::ParticipantState::Error)
        {
            deliver_stop(false);
        }
    }
}

} // namespace vehicle_sil
