// SPDX-License-Identifier: MIT
//
// run --events: the events about how the run goes that the launcher writes when it looks (who has
// joined, the virtual time, who failed, the findings), and what it remembers to write each of
// them once. Internal.
#pragma once

#include "run_context.hpp"
#include "run_participants.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vehicle_sil::launch::detail {

/// When RunNotes::note_time() is called.
enum class TimeNote
{
    Start, ///< the simulation runs: virtual time 0
    Poll,  ///< a look of the watch
    Last,  ///< the simulation is over
};

/// The controller's virtual time: the start of its step that runs or ran last (the gate's
/// time, or the runtime's if that has gone on since the gate's numbers stopped).
[[nodiscard]] inline std::int64_t sim_ms(SimulationController& controller, const TimeState& s)
{
    const SimTime t = std::max(s.now, controller.runtime().now());
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

/// What a "time" event says in `paused`, from one look at the gate. It stands still, or is at
/// (or beyond) the time it is to stand still at and does so with its next step; an ended
/// simulation stands still nowhere. The event at the start is the one of virtual time 0 (its
/// sim_ms is 0 whatever the gate says): a simulation that has gone on by then (a step or
/// run_until from the hold before the first step, as fast as possible) and stands still at a
/// later time did not stand still at 0, and the next look writes where it does.
[[nodiscard]] inline bool noted_paused(const TimeState& s, TimeNote note) noexcept
{
    if (note == TimeNote::Start && s.now > SimTime{})
    {
        return false;
    }
    return s.paused || (note != TimeNote::Last && s.pause_target && *s.pause_target <= s.now);
}

[[nodiscard]] inline std::int64_t ceil_ms(std::chrono::nanoseconds d)
{
    return std::chrono::ceil<std::chrono::milliseconds>(d).count();
}

/// What has been written as events of run --events, so that each thing is written once and the
/// time not too often. Without --events (RunOptions::events is null) every call does nothing.
///
/// Threads and locks: the main thread only, for all of it (the state below and the RunOutcome
/// it is shown). No lock of its own; the EventStream keeps itself.
class RunNotes
{
public:
    RunNotes(const RunContext& ctx, const RunParticipants& participants);

    /// The run's findings that have not been written yet. Before the participants have ended
    /// (`final` false) only those that their end cannot change (settled_findings()): what is
    /// written as an event is in the result as it was written.
    void publish_findings(const RunOutcome& outcome, bool final);

    /// The launcher has decided that `name` failed and ends the run because of it (`reason`: one
    /// word that stays). Written at once, as an event that is never dropped; with the message of
    /// its finding if that is settled already (the finding itself is written when it is settled:
    /// publish_findings()).
    void note_failed(const RunOutcome& outcome, const std::string& name, std::string_view reason);

    /// "joined" for each participant of the run whose lifecycle the controller has seen since the
    /// last look (also an optional one; not after its process has ended).
    void note_joined(SimulationController& controller, const RunOutcome& outcome);

    /// The controller's virtual time and how it goes. At the start 0; then when it stands still
    /// or runs on, or the speed changes, at once; when it has moved, at most every 100 ms; at the
    /// end, if anything changed.
    void note_time(SimulationController& controller, TimeNote note, const RunOutcome& outcome);

private:
    const RunContext& ctx_;
    const RunParticipants& participants_; ///< who is part of the run (in_run())
    std::vector<bool> joined_;            ///< per participant: its "joined" has been written
    // The last "time" event written.
    std::int64_t last_time_ms_ = -1;
    std::chrono::steady_clock::time_point last_time_wall_{};
    bool last_paused_ = false;
    std::int64_t last_late_ms_ = 0;
    std::uint64_t last_late_steps_ = 0;
    std::optional<double> last_speed_{};
    std::deque<std::pair<std::chrono::steady_clock::time_point, std::int64_t>> samples_; ///< (wall, sim_ms)
    std::vector<Finding> published_; ///< the findings written
};

} // namespace vehicle_sil::launch::detail
