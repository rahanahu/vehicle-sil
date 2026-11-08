// SPDX-License-Identifier: MIT
#include "vehicle_sil/core/tick_scheduler.hpp"

#include <stdexcept>
#include <utility>

namespace vehicle_sil {

TickScheduler::TickScheduler(SimDuration step) : step_(step)
{
    if (step_ <= SimDuration::zero())
    {
        throw std::invalid_argument("TickScheduler step must be > 0");
    }
}

std::size_t TickScheduler::add(std::string name, SimDuration period, Task task, SimDuration offset)
{
    if (!task)
    {
        throw std::invalid_argument("TickScheduler task '" + name + "' has no function");
    }
    if (period <= SimDuration::zero() || !on_step_grid(period, step_))
    {
        throw std::invalid_argument("TickScheduler task '" + name + "': period " + std::to_string(period.count()) +
                                    " ns is not a positive multiple of the step " + std::to_string(step_.count()) + " ns");
    }
    if (!on_step_grid(offset, step_))
    {
        throw std::invalid_argument("TickScheduler task '" + name + "': offset " + std::to_string(offset.count()) +
                                    " ns is not a non-negative multiple of the step " + std::to_string(step_.count()) +
                                    " ns");
    }
    tasks_.push_back(Entry{std::move(name), period, offset, std::move(task)});
    return tasks_.size() - 1;
}

void TickScheduler::run_due(SimTime now)
{
    const SimDuration sinceStart = now.time_since_epoch();
    if (!on_step_grid(sinceStart, step_))
    {
        throw std::invalid_argument("TickScheduler::run_due: time " + std::to_string(sinceStart.count()) +
                                    " ns is not on the step grid of " + std::to_string(step_.count()) + " ns");
    }
    for (auto& entry : tasks_)
    {
        if (sinceStart >= entry.offset && ((sinceStart - entry.offset) % entry.period) == SimDuration::zero())
        {
            entry.task(now);
            ++entry.count;
        }
    }
}

} // namespace vehicle_sil
