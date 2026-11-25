// SPDX-License-Identifier: MIT
//
// What a run's outcome means: the findings (assess_run()). Pure functions of the configuration and
// the RunOutcome: no state, no thread, no lock. The counts, the result and the exit code of the
// report are in run_report.cpp.
#include "vehicle_sil/runner/run_outcome.hpp"

#include "vehicle_sil/config/detail/check_words.hpp"
#include "vehicle_sil/runner/detail/run_words.hpp"
#include "vehicle_sil/config/detail/seen_ecu.hpp"

#include "vehicle_sil/control/wire.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/ecu/executable.hpp"
#include "vehicle_sil/config/connect.hpp"
#include "vehicle_sil/runner/detail/run_detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <numeric>
#include <utility>

namespace vehicle_sil::runner {

namespace {

using detail::ended_by_itself;
using detail::exit_text;
using detail::failed_exit;
using config::detail::kNanosPerMilli;
using config::detail::millis_text;

std::string prefix_of(const ParticipantRun& p) { return "participant '" + p.name + "': "; }

/// "its process exited with code 2", or what is known without its status.
std::string process_text(const ParticipantRun& p)
{
    return p.exit ? "its process " + exit_text(*p.exit) : std::string("its process ended");
}

std::string lines_hint(const ParticipantRun& p)
{
    return p.manual ? "you started it (manual), so what it wrote is where you started it"
                    : "the lines [" + p.name + "] above show what it wrote";
}

/// outside-device-shared of the bridge of `o`: another vehicle-sil bridge that is not this run's used its
/// device. A warning; an error where the launcher stopped the run, or did not start it, for it
/// (--strict).
config::Finding shared_finding(const RunOutside& o, const ParticipantRun& bridge)
{
    const OutsideLink& link = o.link;
    detail::OutsideForeign foreign{std::max<std::uint32_t>(link.shared_most, static_cast<std::uint32_t>(link.shared_peers.size())),
                                   link.shared_peers};
    const std::string who = detail::shared_text(foreign) + " that is not part of this run";
    const std::string where = "network '" + o.network + "' -> " + o.target + ": ";
    char at[32];
    std::snprintf(at, sizeof at, "%.2f", link.shared_first ? static_cast<double>(link.shared_first->count()) / 1000.0 : 0.0);
    const std::string mixed =
        "the frames of both mix there: this run's devices get frames meant for the other and the other's get this run's, so "
        "what this run says of them cannot be trusted";
    const std::string fix =
        ". Connect each run to a device of its own, or stop the other bridge (one left over from an earlier run, or "
        "another run on this computer)";
    const std::string limit = std::to_string(kOutsideSharedAfter.count() / 1000) + " s";
    if (link.shared_stop == OutsideLink::SharedStop::AtStart)
    {
        return config::Finding{config::Severity::Error, "outside-device-shared",
                       where + "the device is also used by " + who + ", and --strict does not start a run on it: " + mixed + fix +
                           " and run again; without --strict the run starts and warns",
                       bridge.line, bridge.name, o.network};
    }
    if (link.shared_stop == OutsideLink::SharedStop::AfterDelay)
    {
        return config::Finding{config::Severity::Error, "outside-device-shared",
                       where + "the device was also used by " + who + " from " + at + " s on, and --strict stopped the run after " +
                           limit + " of it: " + mixed + fix + ", then run again",
                       bridge.line, bridge.name, o.network};
    }
    return config::Finding{config::Severity::Warning, "outside-device-shared",
                   where + "the device was also used by " + who + " from " + at + " s on: " + mixed + fix +
                       "; --strict does not start a run on a device that is shared, and stops one that shares it for " + limit,
                   bridge.line, bridge.name, o.network};
}

/// Whether `p` ended by itself with the stop request that reached it too (the console was
/// closed: OutcomeEnd::stop_reached_participants), and so did not fail: Windows ended it when its
/// handler returned (0xC000013A, STATUS_CONTROL_C_EXIT), ecu_main stopped on it (exit code 130), or
/// a bridge of --connect stopped on it (exit code 0). Another exit code is judged as usual.
bool ended_with_the_console(const RunOutcome& outcome, const ParticipantRun& p)
{
    if (!outcome.end.stop_reached_participants || p.missing_at_start || !p.exit || p.exit->signal ||
        !detail::ended_by_itself(p))
    {
        return false;
    }
    const int code = p.exit->code;
    return static_cast<std::uint32_t>(code) == 0xC000013Au || code == ecu_executable::kExitInterrupted ||
           (detail::stops_run(p) && code == 0);
}

/// connect-failed or connect-lost for the bridge `p` of --connect, or nothing: it did not fail the run.
std::optional<config::Finding> bridge_failure(const RunOutcome& outcome, const ParticipantRun& p)
{
    const bool ran = outcome.end.simulation != SimulationEnd::NotStarted;
    const bool blamed = !outcome.end.failed_participant.empty() && p.name == outcome.end.failed_participant;
    const auto outside = std::find_if(outcome.outside.begin(), outcome.outside.end(),
                                      [&p](const RunOutside& o) { return o.network == p.for_network; });
    const std::string target = outside != outcome.outside.end() ? outside->target : std::string{};
    // Only a line that says why: a bridge that was killed, or ended without saying why, wrote last about
    // something else (its start, its statistics, "stopped").
    const std::string said =
        p.last_line.empty() ? std::string("it wrote no line that says why") : "it wrote: " + p.last_line;
    // What it ended with: its process, or what the launcher saw.
    const std::string how = p.exit && ended_by_itself(p) ? "its process " + exit_text(*p.exit)
                            : !outcome.end.failure_reason.empty() && blamed ? "it " + outcome.end.failure_reason
                                                                        : std::string("it left the simulation");
    // The run's own stop: the device is shared with a bridge outside the run (--strict).
    if (outside != outcome.outside.end() && outside->link.shared_stop != OutsideLink::SharedStop::None)
    {
        return shared_finding(*outside, p);
    }
    // The run's own stop: the bridge went on, but its link or bus stayed down (OutsideState::lost()).
    if (blamed && ran && outside != outcome.outside.end() && !outside->link.lost.empty())
    {
        return config::Finding{config::Severity::Error, "connect-lost",
                       "network '" + p.for_network + "': the connection to the real bus (" + target +
                           ") was lost while the simulation was running: " + p.name + " reported " + outside->link.lost +
                           " (the longest the launcher waits for it to come back), so the launcher aborted the "
                           "simulation. The devices on the bus missed what came after. Check the cable, the device on the "
                           "other end and the adapter or the bus it is on, then run again; a link that is down for a few "
                           "seconds only (a cable plugged in again) does not stop the run",
                       p.line, p.name, p.for_network};
    }
    if (ended_with_the_console(outcome, p))
    {
        return std::nullopt;
    }
    if (blamed && ran)
    {
        return config::Finding{config::Severity::Error, "connect-lost",
                       "network '" + p.for_network + "': the connection to the real bus (" + target +
                           ") was lost while the simulation was running: " + p.name + " ended (" + how + "; " + said +
                           "), so the launcher aborted the simulation. The devices on the bus missed what came after; "
                           "the lines [" + p.name + "] above show what the bridge wrote",
                       p.line, p.name, p.for_network};
    }
    if (p.ended_with_run)
    {
        return config::Finding{config::Severity::Error, "connect-lost",
                       "network '" + p.for_network + "': the connection to the real bus (" + target +
                           ") was lost at about the time the simulation ended: " + p.name + " ended (" + how + "; " +
                           said + ") before the launcher asked it to stop. The devices on the bus may have missed the "
                           "last frames; the lines [" + p.name + "] above show what the bridge wrote",
                       p.line, p.name, p.for_network};
    }
    std::string what;
    if (!p.start_error.empty())
    {
        what = "could not be started (" + p.start_error + ")";
    }
    else if (p.ending == Ending::ExitedBeforeStart || blamed)
    {
        what = "ended before the simulation started (" + how + "; " + said + ")";
    }
    else if (p.missing_at_start)
    {
        what = "did not run in SIL Kit within " + std::to_string(outcome.start.timeout_ms) +
               " ms (start_timeout_ms; " + said + ")";
    }
    // One that ended by itself while the launcher was ending the run that did not start (the other
    // of two bridges that failed at once is seen only then).
    else if (!ran && p.exit && ended_by_itself(p))
    {
        what = "ended before the simulation started (" + how + "; " + said + ")";
    }
    else
    {
        return std::nullopt;
    }
    // Whether the participants of the file had been started already (the bridge failed while the run
    // waited for the start, not before the launcher started them).
    const bool othersStarted = std::any_of(outcome.participants.begin(), outcome.participants.end(), [](const ParticipantRun& o) {
        return !detail::stops_run(o) && o.ending != Ending::NotStarted;
    });
    return config::Finding{config::Severity::Error, "connect-failed",
                   "network '" + p.for_network + "' could not be connected to the real bus (" + target + "): " + p.name +
                       " " + what + (othersStarted ? ", so the simulation was not started" : ", so nothing else was started") +
                       ". Check that " + target +
                       " is there and free on this computer (vehicle_sil_launch connections <file> lists where each "
                       "network can be connected), or give --connect " +
                       p.for_network + "=none to run the network on SIL Kit only",
                   p.line, p.name, p.for_network};
}

/// What a time command that changed how the time goes (the first such command of the run) means
/// for what does not follow the virtual time; none without such a command:
/// - the note outside-time-changed, in a run that is connected outside the simulation (it took
///   the command, so its --time-policy is free);
/// - the note unsynchronized-time-changed, in a run with participants that run in real time.
std::vector<config::Finding> time_changed_findings(const RunOutcome& outcome)
{
    std::vector<config::Finding> out;
    const std::string outside = config::detail::connected_outside(outcome.time_lock);
    if (!outcome.time_changed || (outside.empty() && outcome.real_time_peers.empty()))
    {
        return out;
    }
    const TimeChange& t = *outcome.time_changed;
    // The command as its event says it.
    std::string command = t.command;
    if (t.command == "step" && t.ms)
    {
        command += " " + std::to_string(*t.ms) + " ms";
    }
    else if (t.command == "run_until" && t.at_ms)
    {
        command += " " + std::to_string(*t.at_ms) + " ms";
    }
    else if (t.command == "pause" && t.at_ms)
    {
        command += " at " + std::to_string(*t.at_ms) + " ms";
    }
    else if (t.command == "speed")
    {
        command += " " + config::speed_text(t.speed);
    }
    const std::string effect = t.command != "speed" ? std::string("the simulation stood still")
                               : t.speed            ? "the simulation ran at speed " + config::speed_text(t.speed)
                                                    : std::string("the simulation ran as fast as possible");
    const std::string took = "the run took the time command '" + command + "' from " + t.source + " at " +
                             std::to_string(t.sim_ms) + " ms";
    const std::string first = "It is the first command of the run that paused, stepped or changed the speed (the command "
                              "events show each one)";
    if (!outside.empty())
    {
        out.push_back(config::Finding{config::Severity::Note, "outside-time-changed",
                              outside + ", which does not wait for the virtual time, and with --time-policy free " + took +
                                  ": " + effect +
                                  " while what is connected there went on in real time, so from then on it got the messages "
                                  "at the wrong times, and the bridge carried frames both ways all the while (frames from "
                                  "outside pile up while the simulation stands still). " +
                                  first + ". To keep the run to real time, leave --time-policy free out; to pause, step or "
                                          "change the speed with nothing outside, run " +
                                  config::detail::not_connected(outcome.time_lock),
                              0, "", ""});
    }
    if (!outcome.real_time_peers.empty())
    {
        std::vector<std::string> quoted;
        for (const std::string& name : outcome.real_time_peers)
        {
            quoted.push_back("'" + name + "'");
        }
        const bool one = quoted.size() == 1;
        out.push_back(config::Finding{config::Severity::Note, "unsynchronized-time-changed",
                              std::string(one ? "participant " : "participants ") + config::detail::and_list(quoted) +
                                  (one ? " runs" : " run") + " in real time (no step: the virtual time does not move " +
                                  (one ? "it" : "them") + "), and " + took + ": " + effect + " while " +
                                  (one ? "that participant" : "those participants") +
                                  " went on, so what " + (one ? "it" : "they") +
                                  " sent and saw from then on does not fit the virtual time, and another run with the same "
                                  "commands may go differently. " +
                                  first + ". For a run that repeats, give " + (one ? "the participant" : "each of them") +
                                  " a step (so that it follows the virtual time), or do not pause, step or change the speed",
                              0, "", ""});
    }
    return out;
}

/// connect-nothing-received: a bridge of --connect that sent frames to the real bus while the
/// simulation ran and got none from a device there, for each such network. Not for a bridge that
/// failed the run (connect-failed, connect-lost say more), nor without counts (none could be read).
std::vector<config::Finding> nothing_received_findings(const RunOutcome& outcome)
{
    std::vector<config::Finding> out;
    if (outcome.end.simulation == SimulationEnd::NotStarted)
    {
        return out;
    }
    for (const RunOutside& o : outcome.outside)
    {
        if (!detail::sent_without_answer(o.to_outside, o.from_outside))
        {
            continue;
        }
        const auto bridge = std::find_if(outcome.participants.begin(), outcome.participants.end(),
                                         [&o](const ParticipantRun& p) { return p.for_network == o.network; });
        if (bridge == outcome.participants.end() || bridge_failure(outcome, *bridge))
        {
            continue;
        }
        const bool can = config::is_can_bridge_tool(o.tool);
        out.push_back(config::Finding{
            config::Severity::Note, "connect-nothing-received",
            "network '" + o.network + "': nothing came from a device on the real bus (" + o.target + ") before the run ended: " + o.participant +
                " sent " + std::to_string(*o.to_outside) + (*o.to_outside == 1 ? " frame" : " frames") +
                " there and got none from a device" +
                (can ? std::string{} : std::string(" (frames that this computer sent do not count)")) +
                ". If a device is meant to answer, check that it is on, " +
                (can ? "its cable and termination, the bit rate, and that --connect names the channel it is on"
                     : "its cable, and that --connect names the adapter it is on") +
                "; a short run can end before a device answers",
            bridge->line, bridge->name, o.network});
    }
    return out;
}

std::string seconds_text(std::chrono::milliseconds ms)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.2f", static_cast<double>(ms.count()) / 1000.0);
    return text;
}

/// outside-link-down, outside-bus-off, outside-bus-errors, outside-frames-dropped and
/// outside-loop-suspected: what the bridges of --connect saw of the real bus while the simulation
/// ran, for each network. A warning each (--strict fails on it): the run went on, but what it says of
/// the devices outside may be wrong. Not for a bridge that failed the run (connect-lost says more).
std::vector<config::Finding> outside_findings(const RunOutcome& outcome)
{
    std::vector<config::Finding> out;
    if (outcome.end.simulation == SimulationEnd::NotStarted)
    {
        return out;
    }
    // Two networks of this run whose bridges report one device although --connect named it two
    // ways: the check did not see it (it compares the names), so it is said here. The same name
    // twice is the check's.
    for (std::size_t i = 0; i < outcome.outside.size(); ++i)
    {
        for (std::size_t j = i + 1; j < outcome.outside.size(); ++j)
        {
            const RunOutside& a = outcome.outside[i];
            const RunOutside& b = outcome.outside[j];
            if (a.link.device.empty() || a.link.device != b.link.device || a.tool != b.tool)
            {
                continue;
            }
            const NetworkKind kind = config::is_can_bridge_tool(a.tool) ? NetworkKind::Can : NetworkKind::Ethernet;
            if (config::connect_device_key(kind, a.target) == config::connect_device_key(kind, b.target))
            {
                continue;
            }
            out.push_back(config::Finding{config::Severity::Warning, "connect-target-shared",
                                  "networks '" + a.network + "' and '" + b.network + "': --connect gives them the same device under two names ('" +
                                      a.target + "' and '" + b.target +
                                      "'; the bridges report one device): the frames of both networks are mixed there: connect them to different "
                                      "channels (adapters) if they must stay apart, or make them one network in the file if they are one bus",
                                  0, "", a.network});
        }
    }
    for (const RunOutside& o : outcome.outside)
    {
        const auto bridge = std::find_if(outcome.participants.begin(), outcome.participants.end(),
                                         [&o](const ParticipantRun& p) { return p.for_network == o.network; });
        if (bridge == outcome.participants.end() || bridge_failure(outcome, *bridge))
        {
            continue;
        }
        const OutsideLink& link = o.link;
        const bool can = config::is_can_bridge_tool(o.tool);
        if (link.shared_most > 0)
        {
            out.push_back(shared_finding(o, *bridge));
        }
        const std::string where = "network '" + o.network + "' -> " + o.target + ": ";
        const auto add = [&](const char* code, std::string message) {
            out.push_back(config::Finding{config::Severity::Warning, code, where + message, bridge->line, bridge->name, o.network});
        };
        const auto times = [](std::uint32_t n) { return n == 1 ? std::string("once") : std::to_string(n) + " times"; };
        if (link.down_count > 0)
        {
            add("outside-link-down",
                "the link was down " + times(link.down_count) + ", for " + seconds_text(link.down_total) +
                    " s in all. The run went on, but frames sent meanwhile did not reach the devices, and the devices' did "
                    "not reach the simulation, so what this run says of them may be wrong. Check the cable, the device or "
                    "switch on the other end and the adapter; a run that must not lose the link is run with --strict");
        }
        if (link.bus_off_count > 0)
        {
            add("outside-bus-off",
                "the controller was bus off " + times(link.bus_off_count) + ", for " + seconds_text(link.bus_off_total) +
                    " s in all. The run went on, but no frame passed while it was, so what this run says of the devices "
                    "may be wrong. A bus off comes from too many errors: check the bit rate (it must be the bus's), "
                    "the termination and the cable, and that another device is on the bus");
        }
        const auto errors = link.counts.find(std::string(detail::kOutsideBusErrorsKey));
        const std::uint64_t errorFrames = errors != link.counts.end() ? errors->second : 0;
        if (link.passive_total.count() > 0)
        {
            add("outside-bus-errors",
                "the controller was error passive for " + seconds_text(link.passive_total) + " s in all (" +
                    std::string(detail::kOutsideBusErrorsKey) + "=" + std::to_string(errorFrames) +
                    "). Frames may have been lost or repeated on the bus. Check the bit rate, the termination and the "
                    "cable");
        }
        if (const auto dropped = detail::dropped_counts(link.counts); !dropped.empty())
        {
            std::string what;
            for (const auto& [key, count] : dropped)
            {
                what += (what.empty() ? "" : ", ") + key + "=" + std::to_string(count);
            }
            add("outside-frames-dropped",
                "the bridge dropped frames (" + what + "): the devices or the simulation did not get them, so what this "
                    "run says of them may be wrong. A full queue means the bus or the simulation sent faster than the "
                    "bridge could pass; a failed send means the adapter or the bus refused it" +
                    (can ? "; --tx-timeout-ms of the bridge decides how long a congested bus is waited for" : std::string{}));
        }
        if (const auto loop = link.counts.find(std::string(detail::kOutsideLoopKey)); loop != link.counts.end() && loop->second > 0)
        {
            add("outside-loop-suspected",
                "the bridge stopped " + std::to_string(loop->second) + (loop->second == 1 ? " frame" : " frames") +
                    " whose source MAC address was on both sides (" + std::string(detail::kOutsideLoopKey) +
                    "), and from then on drops every frame from such an address in both directions. Two bridges between "
                    "the same networks, or one MAC address on the bus and in the simulation, cause it: give every device "
                    "one address, and connect a network to one bus only");
        }
    }
    return out;
}

} // namespace

// A finding that is added here and that the end of the participants can still change
// (RunParticipants::end_children(): exit, ending, stopped_after_end, stop_request_failed, left_processes,
// interrupted_by, interrupted_while_ending) must be left out by detail::settled_findings() until it cannot.
std::vector<config::Finding> assess_run(const RunOutcome& outcome)
{
    std::vector<config::Finding> out;
    const auto add = [&out](config::Severity severity, std::string code, std::string message, const ParticipantRun* p) {
        out.push_back(config::Finding{severity, std::move(code), std::move(message), p != nullptr ? p->line : 0,
                              p != nullptr ? p->name : std::string{}, ""});
    };
    if (!outcome.setup.error.empty())
    {
        add(config::Severity::Error, "setup", "cannot set up the simulation: " + outcome.setup.error, nullptr);
    }
    const bool ran = outcome.end.simulation != SimulationEnd::NotStarted;
    for (const ParticipantRun& p : outcome.participants)
    {
        const std::string pre = prefix_of(p);
        const config::Severity weight = p.required ? config::Severity::Error : config::Severity::Warning;
        // A bridge of --connect that ended before its time says so in one finding, instead of
        // start, ended-before-start, participant-failed, not-joined or exit-code.
        if (detail::stops_run(p))
        {
            if (std::optional<config::Finding> f = bridge_failure(outcome, p))
            {
                out.push_back(std::move(*f));
                continue;
            }
            // When the launcher aborts the simulation (for another participant, or --timeout-ms),
            // SIL Kit tells the bridges too, and one may end by itself with exit code 1 before the
            // launcher asks it to stop: it followed the abort, which another finding explains.
            if (outcome.end.simulation == SimulationEnd::Aborted)
            {
                continue;
            }
        }
        if (!p.start_error.empty())
        {
            add(weight, "start", pre + p.start_error + ": check the command in the configuration file", &p);
            continue;
        }
        const bool blamed = !outcome.end.failed_participant.empty() && p.name == outcome.end.failed_participant;
        // Closing the console reached it too, and it ended with it: the run was interrupted, which
        // the result says; the participants' table has its exit code.
        if (ended_with_the_console(outcome, p) && (!blamed || outcome.end.failure_reason.empty()))
        {
        }
        // An optional participant may end before or after the start (it does not hold the start
        // up): only its exit code counts, so that the same run gives the same result.
        else if (p.ending == Ending::ExitedBeforeStart && p.required)
        {
            add(config::Severity::Error, "ended-before-start",
                pre + "it ended before the simulation started: " + process_text(p) +
                    ", so the simulation could not start; " + lines_hint(p),
                &p);
        }
        else if (blamed)
        {
            std::string what;
            if (p.exit && ended_by_itself(p))
            {
                what = "its process " + exit_text(*p.exit);
            }
            else if (!outcome.end.failure_reason.empty())
            {
                what = "it " + outcome.end.failure_reason;
            }
            else
            {
                what = "it left the simulation";
            }
            add(config::Severity::Error, "participant-failed",
                pre + "it failed while the simulation was running, and the launcher aborted the simulation: " + what +
                    "; " + lines_hint(p),
                &p);
        }
        else if (p.missing_at_start)
        {
            add(config::Severity::Error, "not-joined",
                pre + "it did not join the simulation within " + std::to_string(outcome.start.timeout_ms) +
                    " ms (start_timeout_ms), although its process was running: " +
                    (p.describe ? "it did not connect to the registry; " + lines_hint(p)
                                : "it must connect to the registry given with {registry_uri} as the participant '" +
                                      p.name + "' with a coordinated lifecycle (or set required: false)") +
                    "; if it is only slow, raise start_timeout_ms",
                &p);
        }
        else if (failed_exit(p))
        {
            add(weight, "exit-code", pre + "its process " + exit_text(*p.exit) + "; " + lines_hint(p), &p);
        }
        // Only a participant the launcher had to stop or end: one that ended by itself just
        // before the request did end.
        if (p.stopped_after_end && ran && (p.ending == Ending::Stopped || p.ending == Ending::Killed))
        {
            std::string how;
            if (p.stop_request_failed)
            {
                how = "could not ask it to stop (no console to send " + std::string(stop_request_name()) +
                      " through) and ended it with the processes it started";
            }
            else if (p.ending == Ending::Stopped)
            {
                how = "asked it to stop (" + std::string(stop_request_name()) + "), and " + process_text(p);
            }
            else
            {
                how = "asked it to stop (" + std::string(stop_request_name()) + "), and as it did not end within " +
                      std::to_string(outcome.end.stop_timeout_ms) + " ms either, ended it with the processes it started";
            }
            add(weight, "did-not-end",
                pre + "it was still running " + std::to_string(outcome.end.stop_timeout_ms) +
                    " ms (stop_timeout_ms) after the simulation ended: the launcher " + how +
                    "; a participant should end when the simulation ends (" + lines_hint(p) + ")",
                &p);
        }
        if (p.control == ControlLink::NoHello)
        {
            add(config::Severity::Warning, "control-link-no-hello",
                pre + "it offers its variables (--describe), but " +
                    (p.control_hello_seen ? "its control agent said hello and then did not answer that it receives the "
                                            "launcher's commands within "
                                          : "its control agent did not say hello within ") +
                    std::to_string(outcome.start.control_hello_timeout_ms) +
                    " ms before the simulation started, so its variables cannot be read or written in this run. " +
                    (p.control_hello_seen
                         ? "Its transport did not answer the launcher's probe: look in the ECU's output for an error of "
                           "its control transport (an application with a transport of its own answers the probe, or gives "
                           "--no-control)"
                         : "Either the executable did not make the agent's transport that --describe announces (an "
                           "application with a transport of its own gives --no-control), or the launcher did not take its "
                           "hello (the finding control-link-dropped says so, if it is there)") +
                    "; " + lines_hint(p),
                &p);
        }
        // Said by the check already if --describe announced the version.
        else if (p.control == ControlLink::VersionMismatch && !p.control_version_described)
        {
            add(config::Severity::Warning, "control-link-version",
                pre + "its control agent speaks version " + std::to_string(p.control_version) +
                    " of the control messages and this launcher version " + std::to_string(control::wire::kVersion) +
                    ", so its variables cannot be read or written in this run: " + config::detail::control_version_fix(p.control_version),
                &p);
        }
        if (!p.variable_map_problems.empty())
        {
            std::string problems;
            for (const std::string& problem : p.variable_map_problems)
            {
                problems += (problems.empty() ? "" : "; ") + problem;
            }
            add(config::Severity::Error, "variable-map-not-applied",
                pre + "variables: " + std::to_string(p.variable_map_problems.size()) +
                    " of its mappings do not fit the variables its control agent announced and were not applied, so "
                    "those variables were seen under the executable's names and values in this run and writes to the names "
                    "of these mappings were refused: " + problems +
                    ". Correct variables in the configuration (vehicle_sil_launch check compares them with what an "
                    "ecu_main executable describes)",
                &p);
        }
        if (p.control_late_messages > 0)
        {
            add(config::Severity::Warning, "control-link-late-values",
                pre + std::to_string(p.control_late_messages) +
                    " messages of its control agent's steps reached the launcher after the launcher's controller had "
                    "begun a later step: the values and results that the launcher had at a pause were not complete. Its "
                    "agent must publish within its steps (an executable built with ecu_main() does; a transport of its "
                    "own must send from control::Agent::set_step_sink(), not from another thread)",
                &p);
        }
        if (p.left_processes)
        {
            add(config::Severity::Warning, "left-processes",
                pre + "processes it started were still running after it had ended: the launcher ended them; end them "
                      "in the participant before it exits",
                &p);
        }
    }
    if (!outcome.start.error.empty())
    {
        add(config::Severity::Error, "simulation-start", "the simulation did not start: " + outcome.start.error, nullptr);
    }
    if (outcome.close.not_closed_ms && !outcome.close.interrupted)
    {
        // Not when one more interrupt cut the wait: then the launcher did what it was asked to.
        add(config::Severity::Warning, "connection-not-closed",
            "the launcher's own connection to the simulation (SIL Kit) had not closed " +
                std::to_string(*outcome.close.not_closed_ms) +
                " ms after the participants had ended: the launcher ended without closing it. The result is complete. "
                "This is not expected of any configuration: it is a defect of the launcher or of SIL Kit. Report it "
                "with this result, the launcher's output and what the participants were doing (was one stopped in a "
                "debugger?)",
            nullptr);
    }
    if (outcome.control.refused_hellos + outcome.control.undecodable + outcome.control.unknown > 0)
    {
        add(config::Severity::Warning, "control-link-dropped",
            "the launcher dropped messages of control agents: " + std::to_string(outcome.control.refused_hellos) +
                " hellos it did not take, " + std::to_string(outcome.control.undecodable) + " messages it could not read and " +
                std::to_string(outcome.control.unknown) +
                " messages (or samples) that fit no agent it knows. Hellos are not taken beyond " +
                std::to_string(control_silkit::HubTransportOptions{}.max_links) +
                " participants with control agents, or with the session number of another participant; a message cannot be "
                "read if it is of another version of vehicle-sil (the participant's control-link-version says so) or not a "
                "control message at all (another program publishes on the topics vehicle_sil.control.*); a message fits "
                "no agent if its agent's hello was not taken, is of another version or has not arrived yet (a "
                "participant that joined while the simulation ran), and a sample if it does not match the variables its "
                "agent announced. The variables of the participants concerned may be missing or stale",
            nullptr);
    }
    if (!outcome.start.before_start_error.empty())
    {
        add(config::Severity::Error, "before-start",
            "what was to be done before the simulation started failed, and the launcher aborted the simulation at once: " +
                outcome.start.before_start_error,
            nullptr);
    }
    if (!outcome.end.controller_state.empty())
    {
        add(config::Severity::Error, "simulation",
            "the simulation ended in state " + outcome.end.controller_state +
                " instead of shutting down normally: the lines above show what the participants wrote",
            nullptr);
    }
    if (outcome.end.timed_out && outcome.start.timed_out_before_start)
    {
        add(config::Severity::Error, "timeout",
            "the simulation did not start within " + std::to_string(outcome.end.timeout_ms.value_or(0)) +
                " ms of wall-clock time (--timeout-ms, which counts from when the launcher began to wait for the "
                "participants, as the run waits for a participant you start yourself), so the launcher gave up: " +
                outcome.start.timed_out_waiting_for +
                "; start the participants started by hand (manual) in time, or leave --timeout-ms out to wait for them",
            nullptr);
    }
    else if (outcome.end.timed_out)
    {
        add(config::Severity::Error, "timeout",
            "the simulation did not end within " + std::to_string(outcome.end.timeout_ms.value_or(0)) +
                " ms of wall-clock time (--timeout-ms), so the launcher aborted it: " +
                (outcome.end.timed_out_while_holding
                     ? std::string("it stood still before its first step all that time, waiting for a resume, step or "
                                   "run_until command (--start-paused with a control server): no client let it go on")
                     : std::string("a participant may hang in a step (the lines above show what the participants "
                                   "wrote); if it is only slow, raise --timeout-ms") +
                           (outcome.start.timeout_from_wait
                                ? ". Those " + std::to_string(outcome.end.timeout_ms.value_or(0)) +
                                      " ms count from when the launcher began to wait for the participants, as the run "
                                      "waits for a participant you start yourself (manual)"
                            : outcome.start.held_before_start_ms
                                ? ". Those " + std::to_string(outcome.end.timeout_ms.value_or(0)) +
                                      " ms count from when the run began to stand still before its first step "
                                      "(--start-paused with a control server), and " +
                                      std::to_string(*outcome.start.held_before_start_ms) +
                                      " ms of them passed before a client let it go on: allow for that time in --timeout-ms"
                                : std::string{})),
            nullptr);
    }
    else if (outcome.end.simulation == SimulationEnd::Aborted && outcome.end.failed_participant.empty() &&
             outcome.start.before_start_error.empty())
    {
        add(config::Severity::Error, "simulation", "the simulation was aborted: the lines above show what the participants wrote",
            nullptr);
    }
    if (!outcome.end.interrupted_by.empty())
    {
        // An operator's stop is "stopped"; a signal, or a supervisor that went away, "interrupted".
        // Closing the console reached the participants too: they stopped by themselves with it.
        add(config::Severity::Note, outcome.end.interrupted_kind == StopKind::Operator ? "stopped" : "interrupted",
            outcome.end.stop_text() + ": " +
                (outcome.end.stop_reached_participants
                     ? std::string("it reached the participants too, and they stopped with it")
                 : outcome.end.interrupted_while_ending ? std::string("the launcher stopped the participants instead of waiting for them to end")
                 : ran                                  ? std::string("the launcher stopped the simulation and the participants")
                                                        : std::string("the launcher stopped the participants before the simulation started")),
            nullptr);
    }
    if (const std::vector<std::string> forced = ended_by_force(outcome); !forced.empty())
    {
        add(config::Severity::Note, "ended-by-force",
            "the launcher did not wait for " + config::detail::and_list(forced) + " to end, because it was asked to stop again (or while it "
            "waited for the participants to end): it ended " + (forced.size() == 1 ? "it" : "them") +
                " by force, with the processes started. This is no error: whoever asked chose not to wait; whether " +
                (forced.size() == 1 ? "it ends" : "they end") + " when asked to is not known from this run (stop once and wait to see)",
            nullptr);
    }
    if (const config::RunTiming& t = outcome.timing; t.late_steps > 0)
    {
        const auto actual = t.actual_speed();
        add(config::Severity::Warning, "behind-real-time",
            "the simulation fell behind real time: " + std::to_string(t.late_steps) + " of " + std::to_string(t.paced_steps) +
                " paced steps of the launcher's controller (" + std::to_string(t.time_step_ms) + " ms each) began more than " +
                std::to_string(t.late_tolerance_ms) + " ms (late_tolerance_ms) after the real time they stand for, the latest by " +
                std::to_string(t.max_late_ms) + " ms" +
                (t.late_ms > 0 ? "; when the run ended, the next step was " + std::to_string(t.late_ms) +
                                     " ms overdue (the simulation was not moving)"
                               : std::string{}) +
                ". The launcher does not catch up after a late step (the ECUs' periodic messages would leave in a burst): it "
                "goes on at the speed from there, so the simulation came " +
                std::to_string(t.slipped_ms) + " ms behind in all" +
                (actual ? " and ran at " + config::speed_text(actual) + " times real time (pauses not counted)" : std::string{}) +
                "; a device attached in real time got the ECUs' messages late by up to that" +
                (outcome.time_lock.locked
                     ? " (" + config::detail::connected_outside(outcome.time_lock) +
                           ": what is connected there does not wait, so the run was that far off from it)"
                     : std::string{}) +
                ". Lower the speed (speed, run "
                "--speed), or find what is slow: an ECU whose step takes long needs a lighter step (a larger step of that "
                "ECU does not help), and many ECUs with small steps cost a round of synchronisation per step (larger ECU "
                "steps and a larger time_step_ms help then). The launcher cannot tell which participant is slow: leave ECUs "
                "out one at a time (--disable <name>) and compare",
            nullptr);
    }
    std::ranges::move(time_changed_findings(outcome), std::back_inserter(out));
    std::ranges::move(nothing_received_findings(outcome), std::back_inserter(out));
    if (!outcome.timing.coarse_step_checked)
    {
        if (auto f = config::detail::coarse_time_step(outcome.timing))
        {
            out.push_back(std::move(*f));
        }
    }
    if (outcome.scenario.result)
    {
        const scenario::ScenarioRun& scenario = *outcome.scenario.result;
        const auto add_scenario = [&](std::string code, std::string message, std::size_t line, config::Severity severity = config::Severity::Error) {
            out.push_back(config::Finding{severity, std::move(code), std::move(message), line, "", "", "", scenario.file});
        };
        for (const scenario::ScenarioStepRun& step : scenario.steps)
        {
            if (step.status == scenario::StepStatus::Failed)
            {
                add_scenario(step.code, step.text + ": " + step.message, step.line);
            }
        }
        for (const ScenarioNotObserved& coarse : outcome.scenario.not_observed)
        {
            // Not a step's failure: nothing was judged. Named so that it is told apart from a
            // condition that was false ("scenario-failed").
            out.push_back(config::Finding{
                config::Severity::Error, "scenario-not-observed",
                "the scenario was not begun: it reads values of participant '" + coarse.participant +
                    "' and judges with every step of the ECU, and '" + coarse.participant + "' sends its values every " +
                    millis_text(coarse.sample_period.count()) + " ms (its sample_period) while its step is " +
                    millis_text(coarse.step.count()) + " ms, so a condition would not see what a variable does between two samples. " +
                    (coarse.asked ? "It was started with --sample-every-step and did not follow it: an executable that makes its "
                                    "own control agent instead of using the one of ecu_main() must set "
                                    "AgentOptions::sample_period to 0 itself. Change the executable so"
                                  : "Its executable does not take --sample-every-step (no \"sample-every-step\" in the features "
                                    "of its --describe), so the launcher could not ask for more. Build the executable again "
                                    "with this vehicle-sil") +
                    ", or write samples: as_sent at the top of the scenario to judge with the values as the ECU sends them",
                0, coarse.participant, "", "", scenario.file});
        }
        if (scenario.status == scenario::ScenarioStatus::Incomplete)
        {
            const auto reached = std::find_if(scenario.steps.rbegin(), scenario.steps.rend(), [](const scenario::ScenarioStepRun& step) {
                return step.status != scenario::StepStatus::NotRun;
            });
            const auto passed = static_cast<std::size_t>(std::count_if(
                scenario.steps.begin(), scenario.steps.end(), [](const scenario::ScenarioStepRun& step) { return step.status == scenario::StepStatus::Passed; }));
            const EndReason end = end_reason(outcome);
            // The stop ended the simulation (one that came while the participants were ending did
            // not, unless it reached the participants too so one of them stopped the simulation).
            const bool cutShort = (end == EndReason::Stopped || end == EndReason::Interrupted) &&
                                  (!outcome.end.interrupted_while_ending || outcome.end.stop_reached_participants) &&
                                  outcome.scenario.error.empty();
            const bool reachedLimit = outcome.end.simulation == SimulationEnd::ReachedStopAt &&
                                      outcome.scenario.error.empty() && outcome.scenario.not_observed.empty();
            add_scenario("scenario-incomplete",
                         "the scenario did not get to its end: the simulation ended at " + std::to_string(outcome.timing.sim_ms) +
                             " ms when " + std::to_string(passed) + " of " + std::to_string(scenario.steps.size()) +
                             " steps had passed" +
                             (reached != scenario.steps.rend() && reached->status == scenario::StepStatus::Unfinished
                                  ? " and the step at line " + std::to_string(reached->line) + " (" + reached->text + ") was not decided yet"
                                  : std::string{}) +
                             (!outcome.scenario.error.empty()
                                  ? "; the launcher's scenario thread failed (" + outcome.scenario.error +
                                        "): this is an error of vehicle_sil_launch"
                              : reachedLimit ? "; the run reached the requested stop_at_ms"
                              : cutShort
                                  ? "; the run was " + outcome.end.stop_text() + " before the scenario's end"
                                  : std::string("; the other findings and the lines above say why the simulation ended")),
                         reached != scenario.steps.rend() ? reached->line : 0,
                         // Cut short by a stop or an interrupt: that is why, and it is not an error
                         // of the run (the result says that the scenario has no verdict).
                         (cutShort || reachedLimit) ? config::Severity::Note : config::Severity::Error);
        }
    }
    if (!outcome.scenario.timeline.empty() && outcome.scenario.timeline_record && outcome.scenario.timeline_record->dropped() > 0)
    {
        const std::uint64_t points = outcome.scenario.timeline_record->dropped();
        add(config::Severity::Note, "timeline-limit",
            "the timeline " + outcome.scenario.timeline + " does not have the whole run: " + std::to_string(points) +
                (points == 1 ? " older point was" : " older points were") +
                " dropped at its limits (the newest points, up to the end of the scenario, are all there). limits.dropped in "
                "the file says of which series and of which time; for a file with every point, record a shorter scenario",
            nullptr);
    }
    if (!outcome.scenario.timeline_error.empty())
    {
        add(config::Severity::Error, "timeline-write",
            "the timeline " + outcome.scenario.timeline + " could not be written: " + outcome.scenario.timeline_error +
                "; what is in the file is not complete: do not use it. The scenario's result is not changed by this. Make "
                "room on the disk, or name another file with --timeline, and run again",
            nullptr);
    }
    // Not when closing the console stopped it: a participant that got it too stopped the simulation
    // first, and the result says that the run was interrupted.
    if (outcome.end.simulation == SimulationEnd::Ended && outcome.end.stop_at_ms && outcome.end.controller_state.empty() &&
        !outcome.end.stopped_by_scenario && !outcome.end.stop_reached_participants)
    {
        add(config::Severity::Note, "ended-before-stop-at",
            "the simulation ended before stop_at_ms (" + std::to_string(*outcome.end.stop_at_ms) +
                " ms): a participant stopped it; if that is not expected, the lines above show which one",
            nullptr);
    }
    for (config::Finding& f : outside_findings(outcome))
    {
        out.push_back(std::move(f));
    }
    config::sort_findings(out);
    return out;
}

std::vector<config::Finding> detail::settled_findings(const RunOutcome& outcome)
{
    std::vector<config::Finding> out;
    for (config::Finding& f : assess_run(outcome))
    {
        // A bridge's quotes its last line, or its counts (and the outside-* findings are made of them), which are known once its output has been
        // read to the end (after the participants have ended).
        if (f.code == "connect-failed" || f.code == "connect-lost" || f.code == "connect-nothing-received" ||
            f.code == "outside-link-down" || f.code == "outside-bus-off" || f.code == "outside-bus-errors" ||
            f.code == "outside-frames-dropped" || f.code == "outside-loop-suspected" || f.code == "outside-device-shared" ||
            f.code == "connect-target-shared")
        {
            continue;
        }
        // Whom the launcher ended by force is known once the participants have ended.
        if (f.code == "ended-by-force")
        {
            continue;
        }
        // What a participant that failed in the simulation did is said with how its process ended,
        // once it has: until then the finding would still change.
        if (f.code == "participant-failed")
        {
            const auto p = std::find_if(outcome.participants.begin(), outcome.participants.end(),
                                        [&f](const ParticipantRun& run) { return run.name == f.participant; });
            if (p == outcome.participants.end() || !(p->exit && ended_by_itself(*p)))
            {
                continue;
            }
        }
        out.push_back(std::move(f));
    }
    return out;
}

} // namespace vehicle_sil::runner
