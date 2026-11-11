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

namespace vehicle_sil {

/// ParticipantRuntime::start() timed out: the participant did not reach Running in time (in
/// Coordinated mode: no controller started the simulation, or a required participant is
/// missing).
class StartTimeoutError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

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

struct ParticipantRuntimeOptions
{
    std::string participant_name;
    std::string registry_uri = "silkit://localhost:8500";
    /// SIL Kit participant configuration as YAML or JSON text. "{}" uses SIL Kit's defaults.
    std::string participant_configuration = "{}";
    /// Upper bound for stop() and the destructor to wait for the lifecycle to shut down.
    std::chrono::milliseconds shutdown_timeout{5000};
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
/// a SIL Kit error and goes to Error, and step_error() returns the message. Virtual time then
/// stops advancing for everyone until the system controller aborts or stops the simulation
/// (see SimulationController::failed_participant()).
///
/// The destructor stops the lifecycle and disconnects. Anything that holds a pointer to a
/// controller of this participant (e.g. vehicle_sil::can::SilKitCanPort, vehicle_sil::lin::LinMaster)
/// must be closed or destroyed first.
class ParticipantRuntime
{
public:
    /// Connects to the registry. Throws SilKit::SilKitError (or a subclass) on failure.
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
    /// ILinController::Init(). Only before start(); otherwise throws std::logic_error.
    void set_communication_ready_handler(std::function<void()> handler);

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

    /// Completes the step that the StepHold held back; does nothing if none is held. Thread safe
    /// (not from within the StepHold). A stop does not need it: stop() and request_stop() end a
    /// simulation that stands still. Must not be called once the destructor has begun.
    void release_step() noexcept;

    /// Starts the lifecycle and waits until this participant is Running (or was: a short
    /// simulation may have ended already when it looks). In Coordinated mode that requires the
    /// system controller and all other required participants.
    /// Throws StartTimeoutError (a std::runtime_error) on timeout, std::runtime_error if the
    /// lifecycle fails, std::logic_error if virtual time is used without a step handler.
    void start(std::chrono::milliseconds timeout);

    /// Stops the lifecycle and waits (bounded by shutdown_timeout) for it to shut down.
    /// In Coordinated mode this stops the whole simulation. Once it returns, the step handler is
    /// not running and is never called again, so objects it uses may be destroyed; a handler
    /// that is running when stop() is called is waited for. Safe to call more than once, but not
    /// from two threads at the same time; never throws. Must not be called from a SIL Kit
    /// handler (use request_stop() there).
    void stop(std::string_view reason) noexcept;

    /// Asks the lifecycle to stop without waiting. Callable from any thread, including the step
    /// handler. In Coordinated mode this stops the whole simulation. Never throws. Pacing
    /// (real_time_factor) is off from then on. A request made before the lifecycle runs (before
    /// start(), or while it waits for the other participants) is kept and delivered when the
    /// lifecycle can take it: with virtual time at the first step (which then still runs; it needs
    /// every synchronised participant to have started its time), otherwise when start() sees
    /// Running. SIL Kit itself ignores a stop before Running. In Error it is delivered at once.
    /// Only the first reason is kept.
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

    /// true once start() has been called (whether or not it succeeded). Thread safe.
    [[nodiscard]] bool started() const noexcept { return lifecycle_started_.load(std::memory_order_acquire); }

    [[nodiscard]] SilKit::Services::Orchestration::ParticipantState state() const;

    [[nodiscard]] PeerStatus peer_status(const std::string& participant_name) const;

    /// Re-reads the statuses SIL Kit's system monitor knows, so that peer_status() also reports
    /// one whose notification was missed (see resync_peers()). Must not be called from a SIL Kit
    /// handler.
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
    /// request waits for the first step with virtual time unless `now`.
    void deliver_stop(bool now) noexcept;
    /// @param replayed true for statuses re-read by resync_peers(): they never overwrite a newer one.
    void on_status(const SilKit::Services::Orchestration::ParticipantStatus& status, bool replayed);
    /// @returns true to complete the step now, false if the StepHold holds it.
    bool on_step(std::chrono::nanoseconds now, std::chrono::nanoseconds duration);
    /// Runs the step handler; on an exception records step_error_, reports a SIL Kit error and
    /// returns false.
    bool run_step_handler(SimTime now, SimDuration step);
    /// Sleeps until `deadline` unless a stop is requested first.
    void pace_until(std::chrono::steady_clock::time_point deadline);
    /// Re-reads the current status of every connected participant from the system monitor.
    /// Works around a SIL Kit v5.0.8 race in which a status received while
    /// AddParticipantStatusHandler() is registering the handler is never delivered to it.
    void resync_peers();

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
    std::mutex hold_mutex_; ///< guards held_; held while the StepHold decides
    bool held_ = false;     ///< a step is held and not yet completed
    bool hold_disabled_ = false; ///< guarded by hold_mutex_; set by stop()
    std::optional<RealTimePacer> pacer_; // only touched by the step handler thread
    std::atomic<std::int64_t> now_ns_{0};
    std::atomic<std::uint64_t> steps_completed_{0};
    std::atomic<bool> step_entered_{false};
    std::atomic<bool> starting_entered_{false}; ///< without virtual time: SIL Kit's starting handler ran
    std::atomic<bool> lifecycle_started_{false}; // set by start() just before StartLifecycle()

    std::mutex step_gate_;       // held while the step handler runs
    bool steps_disabled_ = false; // guarded by step_gate_; set by stop()
    mutable std::mutex control_mutex_; // guards stop_requested_, and writes of step_error_ (only the step thread writes it)
    std::condition_variable stop_cv_;
    bool stop_requested_ = false;
    std::string stop_reason_;                // guarded by control_mutex_; the first request's
    std::atomic<bool> stop_delivered_{false}; // Stop() was given to the lifecycle (or it ends anyway)
    std::string step_error_;

    mutable std::mutex peers_mutex_;
    mutable std::condition_variable peers_cv_;
    std::map<std::string, PeerRecord, std::less<>> peers_;
};

} // namespace vehicle_sil
