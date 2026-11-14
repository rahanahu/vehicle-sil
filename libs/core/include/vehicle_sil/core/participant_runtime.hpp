// SPDX-License-Identifier: MIT
//
// RAII owner of one SIL Kit participant and its lifecycle (vehicle_sil::core).
#pragma once

#include "vehicle_sil/core/real_time_pacer.hpp"
#include "vehicle_sil/core/sim_time.hpp"

#include <silkit/SilKit.hpp>
#include <silkit/services/orchestration/OrchestrationDatatypes.hpp>
#include <silkit/services/orchestration/fwd_decl.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil {

/// ParticipantRuntime::start() timed out: the participant did not reach Running in time (in
/// Coordinated mode: no controller started the simulation, or a required participant is
/// missing).
class StartTimeoutError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// As the timeout of ParticipantRuntime::start() and SimulationController::start(): no limit of
/// its own; the caller's `give_up` decides when to stop waiting.
inline constexpr std::chrono::milliseconds kNoStartLimit = std::chrono::milliseconds::max();

/// How the participant's lifecycle is started and stopped.
enum class LifecycleMode
{
    /// Starts on its own, without a system controller (SIL Kit OperationMode::Autonomous).
    Autonomous,
    /// Starts and stops together with the other required participants, coordinated by a system
    /// controller (vehicle_sil::SimulationController). With virtual time, every coordinated
    /// participant starts at T = 0 (SIL Kit OperationMode::Coordinated).
    Coordinated,
};

/// Joins the SIL Kit virtual time synchronisation (ITimeSyncService).
struct VirtualTimeOptions
{
    /// Fixed step size of this participant. Participants may use different step sizes; SIL Kit
    /// lets a participant execute the step for time T only when every other synchronised
    /// participant has finished all its steps before T.
    SimDuration step{std::chrono::milliseconds(1)};

    /// Couples simulation time to the wall clock: simulation seconds per wall-clock second
    /// (1.0 real time, 4.0 four times faster, 0.5 half speed). Empty: as fast as possible.
    /// Implemented by holding each step (RealTimePacer), so it slows the whole simulation down
    /// to at most this speed; it cannot make a slower participant faster.
    std::optional<double> real_time_factor{};

    /// The step handler runs only for steps that start before `end`. Later steps still take
    /// part in the synchronisation (and in pacing), but do nothing. This makes the last handled
    /// step independent of when the system controller's stop reaches the participant. Empty:
    /// every step is handled.
    std::optional<SimTime> end{};
};

/// The environment variable that makes the participants of a process listen on every address
/// even when their registry URI is a loopback address: "1" does, "0", empty or not set does not;
/// any other value is an error (std::invalid_argument when a participant is made). For a registry
/// that other computers reach although this computer's participants connect to it at a loopback
/// address: vehicle_sil_launch run with registry_uri silkit://0.0.0.0:<port> sets it to 1 for the
/// participants it starts (and to 0 when the registry is on a loopback address). Without it, the
/// participants of the other computers are given this computer's loopback acceptors and try them on
/// their own loopback address, where another program may answer: the join then fails.
inline constexpr std::string_view kListenOnNetworkVariable = "VEHICLE_SIL_LISTEN_ON_NETWORK";

/// Whether `registry_uri` (silkit://<host>:<port>...) names its host by a loopback address: an IPv4
/// address in 127.0.0.0/8 in any numeric form that getaddrinfo() reads ("127.0.0.1", "127.1"),
/// ::1 in any IPv6 form ([::1], [0:0:0:0:0:0:0:1]), 127.x.x.x mapped to IPv6
/// ([::ffff:127.0.0.1]), or localhost (any case, also "localhost."). Other names are not resolved:
/// they count as not loopback.
[[nodiscard]] bool is_loopback_registry_uri(std::string_view registry_uri);

/// The SIL Kit participant configuration (JSON) for a participant of the registry at
/// `registry_uri` that is given none. It changes two of SIL Kit's defaults:
///
/// - A participant whose registry URI is a loopback address (is_loopback_registry_uri()) listens
///   for the other participants on loopback addresses only (Middleware.AcceptorUris: TCP on
///   127.0.0.1, or ::1 for an IPv6 ::1 registry URI, port 0; and, but on Windows, a local-domain
///   socket of a new name in the temporary directory), instead of on every address. Then no
///   other computer can connect to it, and Windows's firewall does not ask whether to let one.
///   Not when `listen_on_network` is true, or, when it is empty, when kListenOnNetworkVariable is
///   1: a registry that listens at 0.0.0.0 may be reached by other computers while this computer's
///   participants connect to it at 127.0.0.1. A `listen_on_network` that has a value decides alone
///   (the environment is not read).
/// - On Windows, the connections between the participants of one machine are TCP connections
///   (Middleware.EnableDomainSockets: false). With local-domain sockets there, closing the
///   socket to a process whose threads are suspended was seen not to return, and the closing
///   process stayed after it was told to end; with TCP connections that was not seen
///   (docs/dependencies.md says what was tried and what is a guess).
///
/// And for every registry: no dynamic simulation steps
/// (Experimental.TimeSynchronization.DynamicSimulationStep: false). Without the key, SIL Kit cuts
/// the steps of a participant down to the smallest step of the simulation as soon as another
/// participant asks for it, and the fixed step that ParticipantRuntime promises is gone.
///
/// Each call makes a new name for the local-domain socket. Throws std::invalid_argument for a value
/// of kListenOnNetworkVariable other than 1, 0 or empty, when it reads it.
[[nodiscard]] std::string default_participant_configuration(std::string_view registry_uri,
                                                            std::optional<bool> listen_on_network = std::nullopt);

/// default_participant_configuration() with more members of the top level: `members` is their
/// JSON without the braces (`"Logging":{...}`), or empty. For a program that makes a
/// configuration of its own, so that the defaults are made in one place. `members` must not
/// have "Middleware" or "Experimental".
[[nodiscard]] std::string default_participant_configuration_with(std::string_view registry_uri, std::string_view members,
                                                                 std::optional<bool> listen_on_network = std::nullopt);

/// The SIL Kit participant configuration (YAML or JSON text) that the participant
/// `participant_name` of the registry at `registry_uri` is made with, from the `configuration` it
/// is given (YAML or JSON text): default_participant_configuration() if that is "{}" or empty
/// (white space aside, or only comments); otherwise the given text with vehicle-sil's defaults
/// for what it leaves out.
///
/// - SIL Kit reads `configuration` first (its schema, its Includes; an included file is looked
///   for from the working directory and in Includes.SearchPathHints). What SIL Kit refuses is SIL
///   Kit's exception (SilKit::ConfigurationError), with SIL Kit's words; for a text that is not
///   YAML, " (at line <n>, column <n> of the configuration text)" is added to them. What the
///   configuration says is taken from that reading, so a member counts whichever file it is in.
/// - Middleware.AcceptorUris: the given ones; without them, as default_participant_configuration()
///   (loopback only for a loopback registry URI, unless `listen_on_network` or
///   kListenOnNetworkVariable says otherwise).
/// - Middleware.EnableDomainSockets on Windows: false, unless the configuration says false
///   itself. A `true` written there cannot be told from none (it is SIL Kit's default) and
///   becomes false as well.
/// - Experimental.TimeSynchronization.DynamicSimulationStep: false when it is left out.
/// - Everything else (Logging, HealthCheck, the other members of Middleware, ...) stays as it is
///   written: the members above are added to the given text, in its first file (a member with
///   nothing under it, as "Experimental:", is one that is left out). A configuration that leaves
///   none of them out is returned as it is.
///
/// std::invalid_argument, with what to change, for a configuration that would make the
/// participant another one than the program says: a ParticipantName other than
/// `participant_name`, a Middleware.RegistryUri other than `registry_uri` (SIL Kit takes the
/// configuration's value for both; the same value is accepted), and DynamicSimulationStep: true
/// (ParticipantRuntime runs on its own fixed step). An empty `registry_uri` means that it is not
/// known (a check before the run): the configuration's URI is not compared.
///
/// std::runtime_error if the text that SIL Kit accepted cannot be read again to add the members
/// (an error of vehicle-sil; the message says how to go on without it).
///
/// All of this needs the YAML library that SIL Kit builds (rapidyaml). In a build against a SIL
/// Kit that vehicle-sil did not build from source, a configuration that is not "{}" or empty is
/// returned as it is, unread: nothing is added, and none of the checks above is made (SIL Kit
/// reads it when the participant is made). It should then set Middleware.EnableDomainSockets to
/// false on Windows and DynamicSimulationStep to false (a ParticipantRuntime whose step SIL Kit
/// cut fails that step), it listens where it says, and it should not set ParticipantName or
/// Middleware.RegistryUri.
[[nodiscard]] std::string merged_participant_configuration(std::string_view configuration,
                                                           std::string_view participant_name,
                                                           std::string_view registry_uri,
                                                           std::optional<bool> listen_on_network = std::nullopt);

/// The controllers of `configuration` (a SIL Kit participant configuration, YAML or JSON text,
/// with what it includes) that are given a network: one text for each, e.g.
/// `CanControllers[0] (Name "CAN1"): Network "PowertrainCAN"`. SIL Kit puts such a controller on
/// that network whatever name the program gives when it makes the controller, so whoever renames
/// networks (PeerOptions::network_names) asks this first. Empty if there is none, for a text that
/// SIL Kit does not accept (whoever makes the participant says what is wrong with it), and in a
/// build without rapidyaml (see merged_participant_configuration(): the text is not read).
[[nodiscard]] std::vector<std::string> configured_controller_networks(std::string_view configuration);

/// The text of the SIL Kit participant configuration file `path`, for
/// ParticipantRuntimeOptions::participant_configuration and the like. If the file has Includes,
/// its directory is added to Includes.SearchPathHints, so that the files it includes are found
/// beside it from any working directory (not in a build without rapidyaml, see
/// merged_participant_configuration(): they are then looked for from the working directory).
/// The text is not checked here (merged_participant_configuration() and ParticipantRuntime do):
/// a file that is not YAML, or whose Includes or SearchPathHints is not what SIL Kit wants, is
/// returned as it is, and SIL Kit says what is wrong when the text is used.
/// std::invalid_argument if there is no such file (with the directory a relative path was taken
/// from) or it cannot be read; nothing else is thrown for what the file says.
[[nodiscard]] std::string participant_configuration_from_file(const std::string& path);

/// The SIL Kit configuration (JSON) for a registry that vehicle-sil starts: without local-domain
/// sockets on every system (TCP only, so that StartListening() fails when its port is taken instead
/// of going on with a local-domain socket only), and without acceptors (a registry listens at the
/// URI it is started with, ISilKitRegistry::StartListening()).
[[nodiscard]] std::string_view default_registry_configuration() noexcept;

/// SilKit::CreateParticipant(), tried again for up to `name_in_use_wait` while the registry refuses
/// the name as in use ("A participant with the same name ... already exists"): the registry may not
/// yet have seen the end of an earlier participant with this name (a process restarted at once;
/// over a network it takes longer). After that, SilKit::ProtocolError with SIL Kit's message, the
/// time waited and what it may mean. Other failures are thrown at once; a negative wait is
/// std::invalid_argument. ParticipantRuntime uses it; so can a program that makes its own SIL Kit
/// participants.
[[nodiscard]] std::unique_ptr<SilKit::IParticipant> create_participant(
    std::shared_ptr<SilKit::Config::IParticipantConfiguration> config, const std::string& name,
    const std::string& registry_uri, std::chrono::milliseconds name_in_use_wait);

struct ParticipantRuntimeOptions
{
    std::string participant_name;
    std::string registry_uri = "silkit://localhost:8500";
    /// SIL Kit participant configuration as YAML or JSON text (of a file:
    /// participant_configuration_from_file()). "{}": the defaults. Otherwise the participant
    /// is made with this one and vehicle-sil's defaults for what it leaves out; one that names
    /// another participant or registry, or asks for dynamic simulation steps, is
    /// std::invalid_argument in the constructor (merged_participant_configuration()).
    std::string participant_configuration = "{}";
    /// With the defaults: true listens on every address even when registry_uri is a loopback
    /// address, false does not, empty leaves it to kListenOnNetworkVariable
    /// (default_participant_configuration()).
    std::optional<bool> listen_on_network{};
    /// Upper bound for stop() and the destructor to wait for the lifecycle to shut down.
    std::chrono::milliseconds shutdown_timeout{5000};
    /// How long the constructor tries again while the registry refuses the name as in use ("A
    /// participant with the same name ... already exists"): the registry may not yet have seen the
    /// end of an earlier participant with this name (a process restarted at once). After that, the
    /// refusal is thrown with the time waited. Negative: std::invalid_argument.
    std::chrono::milliseconds name_in_use_wait{5000};
    LifecycleMode lifecycle_mode = LifecycleMode::Autonomous;
    /// Empty: no time synchronisation; the participant runs in wall-clock time.
    std::optional<VirtualTimeOptions> virtual_time{};
};

/// Called once per simulation step with the step's start time and size. It runs on a SIL Kit
/// thread and blocks the time synchronisation: virtual time does not advance until it returns,
/// and no other SIL Kit handler of this participant runs meanwhile. Messages it sends are
/// stamped with `now` and delivered when it returns.
using StepHandler = std::function<void(SimTime now, SimDuration step)>;

/// Decides, once per step after the step handler (and pacing), whether the participant holds
/// back the completion of the step: true holds it until ParticipantRuntime::release_step().
using StepHold = std::function<bool(SimTime now, SimDuration step)>;

/// What this participant currently knows about another participant.
struct PeerStatus
{
    /// Last lifecycle state the peer published. SIL Kit keeps the last status as history, so
    /// a participant that joins late still learns the current state of earlier participants.
    /// If a peer with a started lifecycle disconnects without shutting down, SIL Kit reports it
    /// as ParticipantState::Error ("Connection Lost").
    std::optional<SilKit::Services::Orchestration::ParticipantState> state;
};

/// true when the peer is running.
[[nodiscard]] bool is_running(const PeerStatus& status) noexcept;

/// true when the peer has left the simulation: it reached a stopping or terminal lifecycle state
/// (Stopping, Stopped, Error, ShuttingDown, Shutdown, Aborting), including a lost connection.
[[nodiscard]] bool has_finished(const PeerStatus& status) noexcept;

/// Owns a SIL Kit participant, its lifecycle and, optionally, its time synchronisation.
///
/// This class is protocol independent: it knows nothing about CAN, LIN or J1939. Bus controllers
/// are created by the caller through participant() (e.g. IParticipant::CreateCanController).
///
/// Usage: construct (connects to the registry), create the bus controllers the participant needs
/// through participant(), then call start(). Controllers created before start() have their
/// subscriptions acknowledged by connected peers once start() returns, so traffic sent after
/// start() is not lost to a late subscription.
///
/// With ParticipantRuntimeOptions::virtual_time, the participant joins the SIL Kit time
/// synchronisation with a fixed step. set_step_handler() must be called before start(). The
/// simulation logic then runs in the step handler; wait_until_finished() waits for the end of
/// the simulation. Exceptions thrown by the step handler are caught: the participant reports
/// a SIL Kit error and goes to Error, and step_error() returns the message. The same happens,
/// without the handler being called, when SIL Kit gives a step of another size than
/// VirtualTimeOptions::step (dynamic simulation steps, which the default configuration turns
/// off). Virtual time then
/// stops advancing for everyone until the system controller aborts or stops the simulation
/// (see SimulationController::failed_participant()).
///
/// The destructor stops the lifecycle and disconnects. Anything that holds a pointer to a
/// controller of this participant (e.g. vehicle_sil::can::SilKitCanPort, vehicle_sil::lin::LinMaster)
/// must be closed or destroyed first.
class ParticipantRuntime
{
public:
    /// Connects to the registry (see ParticipantRuntimeOptions::name_in_use_wait). Throws
    /// SilKit::SilKitError (or a subclass) on failure.
    explicit ParticipantRuntime(ParticipantRuntimeOptions options);
    ~ParticipantRuntime();

    ParticipantRuntime(const ParticipantRuntime&) = delete;
    ParticipantRuntime& operator=(const ParticipantRuntime&) = delete;
    ParticipantRuntime(ParticipantRuntime&&) = delete;
    ParticipantRuntime& operator=(ParticipantRuntime&&) = delete;

    [[nodiscard]] const std::string& name() const noexcept { return options_.participant_name; }
    [[nodiscard]] SilKit::IParticipant& participant() noexcept { return *participant_; }

    /// Sets the step handler. Only with virtual time, and only before start(); otherwise
    /// throws std::logic_error.
    void set_step_handler(StepHandler handler);

    /// Sets a handler that SIL Kit calls once, on its own thread, when this participant's
    /// subscriptions with all connected participants are established and before the first step.
    /// Use it for controller initialisation that the other participants must see, such as
    /// ILinController::Init(). Not called once stop() has returned (see stop()). Only before
    /// start(); otherwise throws std::logic_error.
    void set_communication_ready_handler(std::function<void()> handler);

    /// Adds a handler for the same moment: SIL Kit's CommunicationReady, on its own thread, before
    /// the first step. The added handlers run in the order added, before the one of
    /// set_communication_ready_handler() or set_communication_ready_handler_async() (which this
    /// does not replace, nor they it). For what each controller needs there, without one place
    /// that knows them all: `runtime.add_communication_ready_handler([&port] { port.start(); })`
    /// for a vehicle_sil::can::SilKitCanPort. What the handler uses must live until stop() has
    /// returned (or the ParticipantRuntime is destroyed): no handler is called after that, also
    /// when the lifecycle lives on (see stop()). An exception from an added handler is
    /// reported as a failure of the participant (report_failure(), "CommunicationReady handler
    /// failed: ..."); the handlers after it still run. Only before start(); otherwise throws
    /// std::logic_error.
    void add_communication_ready_handler(std::function<void()> handler);

    /// Like set_communication_ready_handler(), but the participant stays where the handler found
    /// it (CommunicationInitialized) after the handler has returned, until
    /// complete_communication_ready(). A Coordinated participant gets there when every required
    /// participant has: all of them then wait before their first step, their publish/subscribe
    /// services are connected, and this participant can exchange messages with them (the handler
    /// itself must return at once: SIL Kit delivers nothing while it runs). Nobody's first step
    /// runs before the completion, so start() of every participant waits for it: complete in
    /// every case, also when what the window was for failed. If the window would open once stop()
    /// has returned, the handler is not called and the runtime completes the window itself.
    /// Replaces set_communication_ready_handler(). Only before start(); otherwise throws
    /// std::logic_error.
    void set_communication_ready_handler_async(std::function<void()> handler);

    /// Lets the lifecycle go on after the handler of set_communication_ready_handler_async().
    /// Thread safe; not from within that handler (from the thread it woke instead).
    /// @returns false if there is nothing to complete: the handler has not been called (yet), or
    /// it was completed already.
    bool complete_communication_ready() noexcept;

    /// Sets an observer of the other participants' (and this one's) lifecycle states: called with
    /// the participant's name and its status whenever peer_status() of it changes, on the thread
    /// where that is learnt (the SIL Kit thread, or a thread that re-reads the statuses:
    /// wait_for_peer(), refresh_peer_statuses()), one call at a time, possibly again with the
    /// same state. It must be short, must not call into this runtime except peer_status(), and
    /// must not throw. Only before start(); otherwise throws std::logic_error.
    void set_peer_status_observer(std::function<void(const std::string& participant, const PeerStatus& status)> observer);

    /// Sets a StepHold. While this participant holds a step that starts at P, it does not
    /// announce its next step, so every other synchronised participant runs its steps that start
    /// at or before P and none that starts after P: the simulation stands still at P. Holding
    /// costs no thread: the SIL Kit thread stays free (statuses and a stop still arrive), and
    /// SIL Kit's health check does not time a held step.
    /// The hold runs on the SIL Kit thread, like the step handler; it must not call
    /// release_step() or stop() itself (request_stop() is fine). It is not called once a stop was requested, after a step handler
    /// failure, or once stop() has returned (stop() waits for a hold that is deciding); the step
    /// completes then. If it throws, the step completes. It is called for the steps at or after
    /// VirtualTimeOptions::end as well (they still take part in the synchronisation).
    /// Only with virtual time, and only before start(); otherwise throws std::logic_error.
    void set_step_hold(StepHold hold);

    /// Sets an observer of the end of this participant's run. It is called when a stop is
    /// requested (stop(), request_stop()) and when this participant's lifecycle is seen in a state
    /// after Running (has_finished()), on the thread where that happens (the SIL Kit thread too),
    /// possibly more than once, and one call at a time. It must not call into this runtime and
    /// must not throw. Only before start(); otherwise throws std::logic_error.
    void set_stop_observer(std::function<void()> observer);

    /// Completes the step that the StepHold held back; does nothing if none is held. Thread safe
    /// (not from within the StepHold). A stop does not need it: stop() and request_stop() end a
    /// simulation that stands still. Must not be called once the destructor has begun.
    void release_step() noexcept;

    /// Starts the lifecycle and waits until this participant is Running (or was: a short
    /// simulation may have ended already when it looks). When it returns, state() says Running
    /// or a later state, never one before Running. In Coordinated mode that requires the
    /// system controller and all other required participants.
    /// Throws StartTimeoutError (a std::runtime_error) on timeout, std::runtime_error if the
    /// lifecycle fails, std::logic_error if virtual time is used without a step handler.
    /// `give_up`, if set, is asked while it waits (on the calling thread, every few milliseconds):
    /// a non-empty answer ends the wait at once with std::runtime_error that has the answer (for
    /// a required participant that has gone, which the lifecycle would wait for until the
    /// timeout, or for limits of the caller's own). `timeout` kNoStartLimit: no limit but
    /// `give_up`.
    void start(std::chrono::milliseconds timeout, const std::function<std::string()>& give_up = {});

    /// Stops the lifecycle and waits (bounded by shutdown_timeout) for it to shut down.
    /// In Coordinated mode this stops the whole simulation. Once it returns, the step handler is
    /// not running and is never called again, so objects it uses may be destroyed; a handler
    /// that is running when stop() is called is waited for (without a bound), and no handler
    /// starts after that. The same holds for the CommunicationReady handlers (set, added and
    /// asynchronous): one that runs is waited for, and none is called once stop() has returned.
    /// That matters after a start that failed before Running (a timeout, `give_up`): SIL Kit takes
    /// no stop there, so the lifecycle lives on until the ParticipantRuntime is destroyed and gets
    /// to CommunicationReady if the participants it waited for come after all. It then goes on
    /// without the handlers (no controller is started; no step runs either) to where the stop
    /// reaches it. While stop() still waits for the shutdown, the handlers are called as usual.
    /// Safe to call more than once, but not from two threads at the same time; never throws. Must
    /// not be called from a SIL Kit handler, the CommunicationReady handlers among them: it would
    /// wait for itself (use request_stop() there).
    /// After a failure (of the step handler, or report_failure()), the stop goes to the lifecycle
    /// only once the participant is in Error, so that the others see the failure and not an
    /// ordinary stop. The wait for the Error and the wait for the shutdown share one
    /// shutdown_timeout; if the Error has not come by then, the stop goes out anyway. A stop or
    /// abort that another participant (the controller) gives first is not held back: the
    /// participant then may stop (Stopping, Stopped) before it is in Error, or never be in Error.
    void stop(std::string_view reason) noexcept;

    /// Asks the lifecycle to stop without waiting. Callable from any thread, including the step
    /// handler. In Coordinated mode this stops the whole simulation. Never throws. Pacing
    /// (real_time_factor) is off from then on. A request made before the lifecycle runs (before
    /// start(), or while it waits for the other participants) is kept and delivered when the
    /// lifecycle can take it: with virtual time at the first step (which then still runs; it needs
    /// every synchronised participant to have started its time), otherwise when start() sees
    /// Running. SIL Kit itself ignores a stop before Running. A request made while the step
    /// handler runs (from the handler too) is delivered when the handler has returned. In Error
    /// it is delivered at once; after a failure (see stop()) it is delivered only once the
    /// participant is in Error. A request that arrives before a failure (before the failing step
    /// handler started, before report_failure()) is an ordinary stop. Only the first reason is
    /// kept.
    void request_stop(std::string_view reason) noexcept;

    /// Waits until the lifecycle has shut down or is in Error, or the timeout expires.
    /// SIL Kit keeps a participant in Error until it is stopped; stop() (or the destructor)
    /// then shuts it down.
    /// @returns Shutdown or Error, or nullopt on timeout or if start() was never called.
    /// Thread safe against stop() on another thread.
    std::optional<SilKit::Services::Orchestration::ParticipantState> wait_until_finished(std::chrono::milliseconds timeout);

    [[nodiscard]] bool uses_virtual_time() const noexcept { return time_sync_ != nullptr; }

    /// Start time of the step that is running or ran last; SimTime{} before the first step.
    [[nodiscard]] SimTime now() const noexcept { return SimTime{SimDuration{now_ns_.load(std::memory_order_acquire)}}; }

    /// Number of steps whose handler completed without an exception (steps at or after
    /// VirtualTimeOptions::end are not counted).
    [[nodiscard]] std::uint64_t steps_completed() const noexcept { return steps_completed_.load(std::memory_order_acquire); }

    /// true once request_stop() has been called, or stop() after start(). Thread safe.
    [[nodiscard]] bool stop_requested() const;

    /// Message of the exception that the step handler threw, empty if none.
    [[nodiscard]] std::string step_error() const;

    /// Reports a failure of this participant found outside the step handler (a real-time
    /// driver's step, for example): the lifecycle goes to Error, as after a failed step handler
    /// (SIL Kit does not end the simulation; the system controller has to abort it), and stop()
    /// and request_stop() wait for that Error. Only the first failure (this or the step
    /// handler's) is reported. Callable from any thread, but not once the destructor has begun.
    /// Before start() it does nothing (SIL Kit would ignore the error). A call made while start()
    /// is starting the lifecycle (from a CommunicationReady handler of an autonomous participant,
    /// which SIL Kit can call before start() is back from it) is kept, and start() reports it.
    /// Never throws.
    /// step_error() does not change.
    void report_failure(std::string_view reason) noexcept;

    /// true once start() has been called (whether or not it succeeded). Thread safe.
    [[nodiscard]] bool started() const noexcept { return lifecycle_started_.load(std::memory_order_acquire); }

    [[nodiscard]] SilKit::Services::Orchestration::ParticipantState state() const;

    [[nodiscard]] PeerStatus peer_status(const std::string& participant_name) const;
    /// The names of the participants peer_status() knows a state of (this one's too), in order.
    [[nodiscard]] std::vector<std::string> peer_names() const;

    /// Re-reads the statuses SIL Kit's system monitor knows, so that peer_status() also reports
    /// one whose notification was missed (see resync_peers()). Thread safe (callers take turns,
    /// also with wait_for_peer()). Must not be called from a SIL Kit handler.
    void refresh_peer_statuses() { resync_peers(); }
    /// Blocks until `predicate(peer_status(name))` holds or the timeout expires.
    /// While waiting it periodically re-reads the statuses SIL Kit knows (see resync_peers()).
    /// Must not be called from a SIL Kit handler.
    /// @returns true if the predicate holds.
    bool wait_for_peer(const std::string& participant_name,
                       const std::function<bool(const PeerStatus&)>& predicate,
                       std::chrono::milliseconds timeout);

private:
    /// Gives a requested stop to the lifecycle once it can take it (Running, Paused, Error). A
    /// request waits for the first step with virtual time unless `now`. After a failure, `now`
    /// waits for the Error until `deadline`; otherwise it keeps the request.
    void deliver_stop(bool now, std::chrono::steady_clock::time_point deadline = {}) noexcept;
    /// true once the step handler failed or report_failure() was called.
    [[nodiscard]] bool has_failed() const;
    /// @param replayed true for statuses re-read by resync_peers(): they never overwrite a newer one.
    void on_status(const SilKit::Services::Orchestration::ParticipantStatus& status, bool replayed);
    /// @returns true to complete the step now, false if the StepHold holds it.
    bool on_step(std::chrono::nanoseconds now, std::chrono::nanoseconds duration);
    /// Runs the step handler; on an exception records step_error_, reports a SIL Kit error and
    /// returns false.
    bool run_step_handler(SimTime now, SimDuration step);
    /// Records step_error_ and reports a SIL Kit error (once). Only from the step.
    void fail_step(std::string error);
    /// Sleeps until `deadline` unless a stop is requested first.
    void pace_until(std::chrono::steady_clock::time_point deadline);
    /// Re-reads the current status of every connected participant from the system monitor.
    /// Works around a SIL Kit v5.0.8 race in which a status received while
    /// AddParticipantStatusHandler() is registering the handler is never delivered to it.
    /// One caller at a time (resync_mutex_).
    void resync_peers();
    std::mutex resync_mutex_; ///< guards the adding and removing of status handlers in resync_peers()

    struct PeerRecord
    {
        PeerStatus status;
        std::chrono::system_clock::time_point enter_time{};
    };

    [[nodiscard]] std::optional<SilKit::Services::Orchestration::ParticipantState> final_state() const;
    void set_final_state(SilKit::Services::Orchestration::ParticipantState state); // the first one stays

    ParticipantRuntimeOptions options_;
    std::unique_ptr<SilKit::IParticipant> participant_;
    SilKit::Services::Orchestration::ILifecycleService* lifecycle_ = nullptr;
    SilKit::Services::Orchestration::ISystemMonitor* monitor_ = nullptr;
    SilKit::Services::Orchestration::ITimeSyncService* time_sync_ = nullptr;
    // Assigned once by start(); stop() and wait_until_finished() may run on two threads, so
    // each uses its own copy (shared_future) and final_state_ is guarded.
    std::optional<std::shared_future<SilKit::Services::Orchestration::ParticipantState>> lifecycle_done_;
    mutable std::mutex final_mutex_;
    std::optional<SilKit::Services::Orchestration::ParticipantState> final_state_; // guarded by final_mutex_
    bool stopped_ = false;

    StepHandler step_handler_;
    StepHold step_hold_;
    /// Runs added_communication_ready_ (the SIL Kit thread).
    void run_added_communication_ready();
    std::vector<std::function<void()>> added_communication_ready_; ///< add_communication_ready_handler()
    bool communication_ready_registered_ = false; ///< SIL Kit has a handler that runs the added ones
    std::function<void()> communication_ready_; ///< set_communication_ready_handler_async()
    std::mutex communication_ready_mutex_;      ///< guards communication_ready_state_
    enum class CommunicationReady
    {
        NotCalled, ///< the asynchronous handler has not been called
        Open,      ///< it was called and is not completed
        Completed,
    } communication_ready_state_ = CommunicationReady::NotCalled;
    std::mutex peer_observer_mutex_; ///< guards peer_observer_; held while it is called
    std::function<void(const std::string&, const PeerStatus&)> peer_observer_;
    std::mutex stop_observer_mutex_; ///< guards stop_observer_; held while it is called
    std::function<void()> stop_observer_;
    void notify_stop_observer() noexcept;
    std::mutex hold_mutex_; ///< guards held_; held while the StepHold decides
    bool held_ = false;     ///< a step is held and not yet completed
    bool hold_disabled_ = false; ///< guarded by hold_mutex_; set by stop()
    std::optional<RealTimePacer> pacer_; // only touched by the step handler thread
    std::atomic<std::int64_t> now_ns_{0};
    std::atomic<std::uint64_t> steps_completed_{0};
    std::atomic<bool> step_entered_{false};
    /// The step handler runs, until its error (if any) is reported: a stop request waits.
    std::atomic<bool> handler_running_{false};
    std::atomic<bool> starting_entered_{false}; ///< without virtual time: SIL Kit's starting handler ran
    std::atomic<bool> lifecycle_started_{false}; // set by start() just before StartLifecycle()

    std::mutex communication_ready_gate_;      // held while the CommunicationReady handlers run
    bool communication_ready_disabled_ = false; // guarded by communication_ready_gate_; set by stop()
    std::mutex step_gate_;       // held while the step handler runs
    bool steps_disabled_ = false; // guarded by step_gate_; set by stop()
    mutable std::mutex control_mutex_; // guards stop_requested_, and writes of step_error_ (only the step thread writes it)
    std::condition_variable stop_cv_;
    bool stop_requested_ = false;
    std::string stop_reason_;                // guarded by control_mutex_; the first request's
    std::atomic<bool> stop_delivered_{false}; // Stop() was given to the lifecycle (or it ends anyway)
    std::string step_error_;
    /// Guarded by control_mutex_; a failure was reported: ReportError() is queued (both happen
    /// under the lock).
    bool failed_ = false;
    bool error_reportable_ = false; ///< guarded by control_mutex_; start() is past StartLifecycle()
    /// guarded by control_mutex_; a failure reported while start() was in StartLifecycle()
    std::optional<std::string> early_failure_;

    mutable std::mutex peers_mutex_;
    mutable std::condition_variable peers_cv_;
    std::map<std::string, PeerRecord, std::less<>> peers_;
};

} // namespace vehicle_sil
