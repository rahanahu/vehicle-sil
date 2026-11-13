// SPDX-License-Identifier: MIT
//
// The participants' processes of a run: starting them, seeing them end, ending them; and the
// participants someone starts by hand (manual), whose processes are not the launcher's. Internal.
#pragma once

#include "run_context.hpp"
#include "run_output.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace vehicle_sil::launch::detail {

/// The processes the launcher started, one per participant (none for a disabled one, for one
/// started by hand and for one that could not be started).
///
/// Threads and locks: the main thread only, for all of it (the processes, and the RunOutcome it
/// writes: each participant's exit, ending, start_error and what the end found). No lock of its
/// own. Each Process has a reader thread, which touches nothing here: it calls
/// RunOutput::on_line(). start_children() holds the output (RunOutput::hold()) while it makes a
/// process and writes its "started" event, so that the process's lines come after it.
///
/// Lifetime: destroyed before the RunOutput (a Process ends its reader thread in its destructor):
/// the Runner declares it last.
class RunParticipants
{
public:
    /// `base`: the directory of the configuration file (the participants' working directory).
    RunParticipants(const RunContext& ctx, RunOutput& out, RunOutcome& outcome, std::filesystem::path base);

    /// Participant `i` is part of this run: the launcher started its process, or someone starts it
    /// by hand (manual).
    [[nodiscard]] bool in_run(std::size_t i) const
    {
        return children_[i] != nullptr || (ctx_.config.participants[i].enabled && ctx_.config.participants[i].manual);
    }

    /// Starts every participant in the file's order (for one started by hand: writes how to start
    /// it). false if a required one could not be started.
    [[nodiscard]] bool start_children(const std::string& uri);

    /// Notes the participants whose process has ended since the last look; returns the first
    /// required one among them (or -1).
    int look_at_children(bool before_start);

    /// After a start that failed: the first required participant whose process has ended; if one
    /// has left the simulation (`left`), waiting a moment for its process (SIL Kit may see the
    /// connection end before the process can be waited for). -1 if none ended.
    [[nodiscard]] int gone_process(bool left);

    /// Ends every participant: after a simulation, first gives the required ones stop_timeout_ms
    /// to end by themselves; then asks the rest to stop, waits stop_timeout_ms, and ends what is
    /// left with the processes it started. `interrupts_at_end`: the stop requests that had come
    /// when the simulation had ended (one more cuts the first wait short).
    void end_children(bool ran, int interrupts_at_end);

    /// The participants someone started by hand: the launcher cannot end their processes. If the
    /// simulation did not start, they still wait for it: the controller aborts it, which ends them
    /// (an ecu_main executable). Then waits up to stop_timeout_ms for each that joined to leave
    /// the simulation, so that the registry and the controller stay until they have.
    void end_manual(SimulationController& controller, bool ran, int interrupts_at_end);

private:
    /// A participant someone starts by hand: the lines (or the event) with where and how to start it.
    void show_manual(const ParticipantConfig& p, const std::filesystem::path& executable,
                     const std::vector<std::string>& arguments, const std::filesystem::path& workDir);
    [[nodiscard]] bool any_running(bool required_only);
    /// Waits up to `timeout` until `done()`; stops waiting once more than `interrupts_allowed`
    /// interrupts have come (detail::wait_for()).
    template <class Done>
    Waited wait_until(std::chrono::milliseconds timeout, int interrupts_allowed, Done done);

    const RunContext& ctx_;
    RunOutput& out_;
    RunOutcome& outcome_; ///< the main thread's
    std::filesystem::path base_;
    std::vector<std::unique_ptr<process::Process>> children_;
};

} // namespace vehicle_sil::launch::detail
