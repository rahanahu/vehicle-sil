// SPDX-License-Identifier: MIT
//
// The controls of a run: what can be asked of it from outside while it lasts (stop it, move its
// time) and where it stands (its phase), in one object that the run borrows (RunControls). Used by
// the run (run.hpp: RunOptions::controls). Internal.
#pragma once

#include "vehicle_sil/launch/stop_requests.hpp"
#include "vehicle_sil/launch/time_commands.hpp"

#include <atomic>
#include <functional>

namespace vehicle_sil::launch {

/// The stages of a run, in this order (a run may skip some, never goes back).
enum class Phase
{
    Checking, ///< the check (--describe of each executable)
    Starting, ///< the registry, the controller, the participants' processes
    Waiting,  ///< for the required participants to join
    Running,  ///< the simulation runs
    Stopping, ///< the simulation has ended (or did not start): the participants are being ended
    Ended,    ///< every participant has ended; the result follows
};
[[nodiscard]] const char* phase_name(Phase phase);

/// The controls of one run: the stop requests (borrowed), the time commands and the phase.
/// Whoever asks something of the run from outside (the reader of standard input, the threads of a
/// control server) and the run itself meet here; nothing here knows the events or SIL Kit.
///
/// Lifetime: one object serves one run. Its time commands refuse everything once the run has
/// detached them, and nothing attaches them again, so a second run needs controls of its own; the
/// stop requests are the part that several runs share. The caller of run_file() makes it before
/// the run and keeps it for as long as any of its threads may use it (vehicle_sil_launch run:
/// never destroyed, as the reader of standard input uses it until the process ends). Without
/// RunOptions::controls the run makes its own for that run.
///
/// Threads and locks: stops() and time() return the same objects for the whole lifetime, and each
/// keeps itself (StopRequests: atomics; TimeCommands: its mutex). phase() may be read from any
/// thread. set_phase() is the run's alone, one call at a time. This object has no mutex of its
/// own: set_phase() takes none, and the observers are called without one (`on_command` under
/// TimeCommands' mutex, as TimeCommands::OnCommand says).
class RunControls
{
public:
    /// What is told of the controls, fixed when they are made. Either may be empty.
    struct Observers
    {
        /// Each time command as decided, before it is done: TimeCommands::OnCommand (the writer of
        /// the command events).
        TimeCommands::OnCommand on_command{};
        /// Each phase the run enters, on the run's thread, after phase() has it (the writer of the
        /// phase events). Must not call set_phase().
        std::function<void(Phase phase)> on_phase{};
    };

    /// `stops`: the stop requests, the caller's; they must outlive this object. `start_paused`:
    /// run --start-paused (TimeCommands). Without `observers`: none.
    RunControls(StopRequests& stops, bool start_paused, Observers observers);
    explicit RunControls(StopRequests& stops, bool start_paused = false);
    RunControls(const RunControls&) = delete;
    RunControls& operator=(const RunControls&) = delete;

    /// The stop requests this object was made with.
    [[nodiscard]] StopRequests& stops() noexcept { return stops_; }
    [[nodiscard]] const StopRequests& stops() const noexcept { return stops_; }
    /// The time commands of the run.
    [[nodiscard]] TimeCommands& time() noexcept { return time_; }

    /// The phase the run entered last; Phase::Checking before it has said any.
    [[nodiscard]] Phase phase() const noexcept { return phase_.load(); }
    /// The run enters `phase`: phase() has it from now on, then Observers::on_phase is called.
    void set_phase(Phase phase);

private:
    StopRequests& stops_;
    TimeCommands time_;
    std::atomic<Phase> phase_{Phase::Checking};
    const std::function<void(Phase phase)> on_phase_;
};

} // namespace vehicle_sil::launch
