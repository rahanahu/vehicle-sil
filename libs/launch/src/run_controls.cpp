// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/run_controls.hpp"

#include <utility>

namespace vehicle_sil::launch {

const char* phase_name(Phase phase)
{
    switch (phase)
    {
    case Phase::Checking: return "checking";
    case Phase::Starting: return "starting";
    case Phase::Waiting: return "waiting";
    case Phase::Running: return "running";
    case Phase::Stopping: return "stopping";
    case Phase::Ended: return "ended";
    }
    return "ended";
}

RunControls::RunControls(StopRequests& stops, bool start_paused, Observers observers) :
  stops_(stops), time_(start_paused, observers.on_command), on_phase_(std::move(observers.on_phase)),
  has_command_observer_(static_cast<bool>(observers.on_command))
{
}

RunControls::RunControls(StopRequests& stops, bool start_paused) : RunControls(stops, start_paused, Observers{}) {}

void RunControls::set_phase(Phase phase)
{
    phase_.store(phase);
    if (on_phase_)
    {
        on_phase_(phase);
    }
}

} // namespace vehicle_sil::launch
