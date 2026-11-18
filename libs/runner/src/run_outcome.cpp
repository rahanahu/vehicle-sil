// SPDX-License-Identifier: MIT
//
// The names and the small judgements of a run's outcome that the run itself and the launcher's
// output both use: why the run ended, which participants were ended by force, the names of the
// control link and of the stop request.
#include "vehicle_sil/runner/run_outcome.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch {

EndReason end_reason(const RunOutcome& outcome)
{
    if (!outcome.setup.error.empty())
    {
        return EndReason::SetupFailed;
    }
    if (!outcome.end.interrupted_by.empty())
    {
        // What asked first decides, whatever came after it (a Ctrl+C after a stop command is
        // still the operator's stop).
        return outcome.end.interrupted_kind == StopKind::Operator ? EndReason::Stopped : EndReason::Interrupted;
    }
    const bool ended = outcome.end.simulation == SimulationEnd::ReachedStopAt || outcome.end.simulation == SimulationEnd::Ended;
    return ended ? EndReason::Completed : EndReason::Failed;
}

std::vector<std::string> ended_by_force(const RunOutcome& outcome)
{
    std::vector<std::string> names;
    if (end_reason(outcome) != EndReason::Stopped)
    {
        return names;
    }
    const bool ran = outcome.end.simulation != SimulationEnd::NotStarted;
    for (const ParticipantRun& p : outcome.participants)
    {
        // Not one that the launcher could not ask to stop, and not one it waited for in full (the
        // finding did-not-end says that).
        if (p.ending == Ending::Killed && !p.stop_request_failed && !(p.stopped_after_end && ran))
        {
            names.push_back(p.name);
        }
    }
    return names;
}

const char* end_reason_name(EndReason reason) noexcept
{
    switch (reason)
    {
    case EndReason::Completed: return "completed";
    case EndReason::Stopped: return "stopped";
    case EndReason::Interrupted: return "interrupted";
    case EndReason::Failed: return "failed";
    case EndReason::SetupFailed: return "setup_failed";
    }
    return "failed";
}

const char* control_link_name(ControlLink link)
{
    switch (link)
    {
    case ControlLink::None:
        return "none";
    case ControlLink::Connected:
        return "connected";
    case ControlLink::NoHello:
        return "no_hello";
    case ControlLink::VersionMismatch:
        return "version_mismatch";
    case ControlLink::NotOffered:
        return "not_offered";
    }
    return "none";
}

std::string_view stop_request_name()
{
#ifdef _WIN32
    return "Ctrl+Break";
#else
    return "SIGTERM";
#endif
}

} // namespace vehicle_sil::launch
