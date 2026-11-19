// SPDX-License-Identifier: MIT
//
// The names and the small judgements of a run's outcome that the run itself and the launcher's
// output both use: why the run ended, which participants were ended by force, the names of the
// control link and of the stop request.
#include "vehicle_sil/runner/run_outcome.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::runner {

EndReason end_reason(const RunOutcome& outcome)
{
    if (!outcome.setup.error.empty())
    {
        return EndReason::SetupFailed;
    }
    if (!outcome.end.interrupted_by.empty())
    {
        // What asked first decides, whatever came after it (a Ctrl+C after a stop command is
        // still the operator's stop). An interrupt that was noted does; an operator's stop if it
        // ended the simulation (or the run before the start), or if it came while the launcher
        // waited for the participants to end and cut short the end of a required one (the stop
        // decided how that one ended). One that cut short only the wait for the others leaves
        // the simulation's own end.
        if (outcome.end.interrupted_kind != StopKind::Operator)
        {
            return EndReason::Interrupted;
        }
        const bool cutRequired = std::any_of(outcome.participants.begin(), outcome.participants.end(), [](const ParticipantRun& p) {
            return p.required && p.end_cut_short && (p.ending == Ending::Stopped || p.ending == Ending::Killed);
        });
        if (!outcome.end.interrupted_while_ending || cutRequired)
        {
            return EndReason::Stopped;
        }
    }
    const bool ended = outcome.end.simulation == SimulationEnd::ReachedStopAt || outcome.end.simulation == SimulationEnd::Ended;
    return ended ? EndReason::Completed : EndReason::Failed;
}

std::vector<std::string> ended_by_force(const RunOutcome& outcome)
{
    std::vector<std::string> names;
    const bool ran = outcome.end.simulation != SimulationEnd::NotStarted;
    for (const ParticipantRun& p : outcome.participants)
    {
        // Whatever asked (a stop, a signal, a supervisor that went away): only one whose wait a
        // stop request cut short. Not one that the launcher could not ask to stop, and not one it
        // waited for in full (the finding did-not-end says that).
        if (p.ending == Ending::Killed && p.end_cut_short && !p.stop_request_failed && !(p.stopped_after_end && ran))
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

} // namespace vehicle_sil::runner
