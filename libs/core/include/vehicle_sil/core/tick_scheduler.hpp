// SPDX-License-Identifier: MIT
//
// Fixed-period tasks (e.g. an existing ECU's tick()) driven by simulation steps (vehicle_sil::core).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vehicle_sil {

/// Runs fixed-period tasks from a participant's simulation step handler.
///
/// A SIL Kit participant has exactly one step size. TickScheduler lets one participant host
/// several fixed-period tasks, for example an existing ECU's 10 ms tick() next to a 20 ms
/// diagnostic task, on a 10 ms (or finer) step. Every period and offset must be a multiple of
/// the participant's step; add() throws std::invalid_argument otherwise, so a task can never
/// silently drift off its period.
///
/// run_due(now) calls every task with `now >= offset` and `(now - offset) % period == 0`, in the
/// order the tasks were added. Call it once per step from the step handler, with the step's
/// time. Because the step handler blocks the SIL Kit time synchronisation, a task that runs long
/// in wall-clock time delays the whole simulation instead of being skipped: virtual time does
/// not advance until the slowest participant has finished its step.
///
/// Not thread safe: add() and run_due() belong to the thread that runs the step handler (add
/// tasks before the lifecycle starts).
class TickScheduler
{
public:
    using Task = std::function<void(SimTime now)>;

    /// @param step The step size of the participant that calls run_due(); must be > 0.
    explicit TickScheduler(SimDuration step);

    /// Adds a task. Returns its index (for tick_count()).
    std::size_t add(std::string name, SimDuration period, Task task, SimDuration offset = SimDuration::zero());

    /// Runs the tasks that are due at `now`. `now` must be a multiple of the step; otherwise
    /// throws std::invalid_argument.
    void run_due(SimTime now);

    [[nodiscard]] SimDuration step() const noexcept { return step_; }
    [[nodiscard]] std::size_t task_count() const noexcept { return tasks_.size(); }
    [[nodiscard]] const std::string& name(std::size_t index) const { return tasks_.at(index).name; }
    /// How often the task at `index` has run.
    [[nodiscard]] std::uint64_t tick_count(std::size_t index) const { return tasks_.at(index).count; }

private:
    struct Entry
    {
        std::string name;
        SimDuration period;
        SimDuration offset;
        Task task;
        std::uint64_t count = 0;
    };

    SimDuration step_;
    std::vector<Entry> tasks_;
};

} // namespace vehicle_sil
