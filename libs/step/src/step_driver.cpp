// SPDX-License-Identifier: MIT
#include "vehicle_sil/step/step_driver.hpp"

#include <stdexcept>

namespace vehicle_sil::step {

void StepDriver::add(Host& host, SimDuration step)
{
    if (started_)
    {
        throw std::logic_error("vehicle_sil::step::StepDriver: add() is only allowed before the first run_until()");
    }
    if (step <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::step::StepDriver: the step size must be > 0");
    }
    for (const auto& entry : entries_)
    {
        if (entry.host == &host)
        {
            throw std::logic_error("vehicle_sil::step::StepDriver: a host can be added only once");
        }
    }
    host.require_time_domain(TimeDomain::Virtual);
    host.set_step_size(step);
    entries_.push_back(Entry{&host, step, SimTime{}});
}

SimTime StepDriver::next_step_time() const
{
    if (entries_.empty())
    {
        throw std::logic_error("vehicle_sil::step::StepDriver: no host added");
    }
    SimTime earliest = entries_.front().next;
    for (const auto& entry : entries_)
    {
        if (entry.next < earliest)
        {
            earliest = entry.next;
        }
    }
    return earliest;
}

void StepDriver::run_until(SimTime end)
{
    started_ = true;
    if (entries_.empty())
    {
        return;
    }
    while (true)
    {
        // The first entry with the earliest start time: (start time, registration order).
        Entry* next = &entries_.front();
        for (auto& entry : entries_)
        {
            if (entry.next < next->next)
            {
                next = &entry;
            }
        }
        if (next->next >= end)
        {
            return;
        }
        const SimTime now = next->next;
        next->next = now + next->step; // advance first: a failed step is not retried
        next->host->step(now);
    }
}

} // namespace vehicle_sil::step
