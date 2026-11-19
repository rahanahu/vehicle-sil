// SPDX-License-Identifier: MIT
#include "run_participants.hpp"

#include "vehicle_sil/runner/detail/run_words.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace vehicle_sil::launch {

std::int64_t detail::ecu_start_timeout_ms(const SimulationConfig& config, bool stands_still_before_start)
{
    // The launcher waits start_timeout_ms for the start and then stops the participants within
    // stop_timeout_ms; the ECU must not give up first (it would end, and the launcher would name it
    // instead of who did not join). 10 s more for a slow machine; never more than ecu_main accepts.
    // A start that waits for someone to start a participant by hand may take any time, and so may
    // one that stands still before the first step until a control client's command: the ECUs are
    // not Running before it, and a limit of theirs would end the run while it stands still.
    if (waits_for_manual(config) || stands_still_before_start)
    {
        return kMaxMillis;
    }
    return std::min(config.start_timeout_ms + config.stop_timeout_ms + 10000, kMaxMillis);
}

detail::StopRequest detail::record_stop_request(ParticipantRun& run, bool sent, const std::optional<process::ExitStatus>& status)
{
    if (sent)
    {
        return StopRequest::Sent;
    }
    if (status)
    {
        run.exit = status;
        run.ending = Ending::Exited;
        run.stopped_after_end = false;
        return StopRequest::HadEnded;
    }
    run.stop_request_failed = true;
    return StopRequest::CannotSend;
}

std::string detail::shell_command_line(const std::string& executable, const std::vector<std::string>& arguments)
{
    const auto quoted = [](const std::string& word) {
#ifdef _WIN32
        // CommandLineToArgvW and the C runtimes: backslashes are literal unless before a quote.
        // Also in quotes: what cmd.exe would read as its own (&, |, <, >, ^), and what PowerShell
        // would (a,b unquoted is an array, given as two arguments; ';' ends a statement); cmd.exe's
        // %...% cannot be kept from it.
        if (!word.empty() && word.find_first_of(" \t\"&|<>^,;") == std::string::npos)
        {
            return word;
        }
        std::string out = "\"";
        std::size_t backslashes = 0;
        for (const char c : word)
        {
            if (c == '\\')
            {
                ++backslashes;
                continue;
            }
            out.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
            backslashes = 0;
            out += c;
        }
        out.append(backslashes * 2, '\\');
        return out + "\"";
#else
        const bool plain = !word.empty() && std::all_of(word.begin(), word.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || std::string_view("%+,-./:=@_").find(static_cast<char>(c)) != std::string_view::npos;
        });
        if (plain)
        {
            return word;
        }
        std::string out = "'";
        for (const char c : word)
        {
            out += c == '\'' ? std::string("'\\''") : std::string(1, c);
        }
        return out + "'";
#endif
    };
    std::string line = quoted(executable);
    for (const auto& argument : arguments)
    {
        line += ' ' + quoted(argument);
    }
    return line;
}

namespace detail {

namespace {

namespace orch = SilKit::Services::Orchestration;

/// `path` without its "." parts ("dir/./engine" as "dir/engine"); ".." stays (it may cross a
/// symbolic link).
std::filesystem::path without_dots(const std::filesystem::path& path)
{
    std::filesystem::path out;
    for (const auto& part : path)
    {
        if (part != ".")
        {
            out /= part;
        }
    }
    return out;
}

std::string command_text(const std::filesystem::path& executable, const std::vector<std::string>& arguments)
{
    std::string out = process::shown_path(executable);
    for (const auto& argument : arguments)
    {
        out += ' ';
        const bool plain = !argument.empty() && argument.find_first_of(" \t\"'") == std::string::npos;
        out += plain ? argument : "'" + argument + "'";
    }
    return out;
}

} // namespace

std::vector<std::string> participant_arguments(const SimulationConfig& config, const ParticipantConfig& participant,
                                               const std::string& registry_uri, bool stands_still_before_start,
                                               bool sample_every_step)
{
    const std::vector<std::string> rest(participant.command.begin() + (participant.command.empty() ? 0 : 1),
                                        participant.command.end());
    std::vector<std::string> arguments;
    if (participant.describe)
    {
        arguments = expand_placeholders(rest, "", "");
        arguments.insert(arguments.end(), {"--name", participant.name, "--registry-uri", registry_uri, "--start-timeout-ms",
                                           std::to_string(ecu_start_timeout_ms(config, stands_still_before_start))});
    }
    else
    {
        arguments = expand_placeholders(rest, registry_uri, participant.name);
    }
    // After the placeholders are replaced: a network's name with "{name}" in it stays as it is.
    const std::vector<std::string> renames = map_network_arguments(participant);
    arguments.insert(arguments.end(), renames.begin(), renames.end());
    const std::vector<std::string> nodes = j1939_node_arguments(participant);
    arguments.insert(arguments.end(), nodes.begin(), nodes.end());
    if (sample_every_step)
    {
        arguments.emplace_back(ecu_executable::kSampleEveryStepArgument);
    }
    return arguments;
}

RunParticipants::RunParticipants(const RunContext& ctx, RunOutput& out, RunOutcome& outcome, std::filesystem::path base) :
  ctx_(ctx), out_(out), outcome_(outcome), base_(std::move(base))
{
    children_.resize(ctx.config.participants.size());
}

template <class Done>
Waited RunParticipants::wait_until(std::chrono::milliseconds timeout, int interrupts_allowed, Done done)
{
    return detail::wait_for(timeout, std::chrono::milliseconds(5), done,
                            [this, interrupts_allowed] { return ctx_.interrupts() > interrupts_allowed; });
}

bool RunParticipants::start_children(const std::string& uri, Which which)
{
    const auto workDir = without_dots(std::filesystem::absolute(base_));
    for (std::size_t i = 0; i < ctx_.config.participants.size(); ++i)
    {
        const ParticipantConfig& p = ctx_.config.participants[i];
        ParticipantRun& run = outcome_.participants[i];
        const std::optional<NetworkOutside>& outside = ctx_.plan.members[i].outside;
        const bool bridge = outside.has_value();
        if (bridge != (which == Which::Bridges))
        {
            continue;
        }
        if (!p.enabled)
        {
            out_.say("not starting " + p.name + ": disabled");
            continue;
        }
        // The one list for the process, the command to start it by hand and the events. A bridge's
        // as the check showed it (no placeholders: an adapter's name may have "{name}" in it).
        std::vector<std::string> arguments;
        if (bridge)
        {
            arguments = bridge_run_arguments(*outside, uri, ctx_.config.start_timeout_ms);
        }
        else
        {
            arguments = participant_arguments(ctx_.config, p, uri, ctx_.holds(), ctx_.plan.members[i].sample_every_step);
        }
        const std::filesystem::path executable = without_dots(std::filesystem::absolute(ctx_.check.participants[i].executable));
        if (p.manual)
        {
            show_manual(p, executable, arguments, workDir);
            continue;
        }
        out_.say("starting " + p.name + ": " + command_text(executable, arguments));
        process::ProcessOptions options;
        // A bridge in its own directory (bridge_working_directory()), a participant of the file in
        // the configuration file's.
        options.working_directory = bridge ? bridge_working_directory(executable) : workDir;
        options.on_line = [this, i](process::Stream, std::string_view line) { out_.on_line(i, line); };
        // Always, so that the value decides and not one that the launcher inherited: "1" if other
        // computers may reach the registry, although these participants connect to it at a loopback
        // address (uri_for_participants()), so that they do not listen on loopback only; else "0".
        options.environment.emplace_back(kListenOnNetworkVariable, listen_variable_value(ctx_.config));
        try
        {
            // Its lines wait (RunOutput::on_line() takes the output's lock) until its "started" event is out.
            const auto held = out_.hold();
            children_[i] = std::make_unique<process::Process>(executable, arguments, std::move(options));
            out_.event([&](RunEvents& e) {
                std::vector<std::string> command{process::shown_path(executable)};
                command.insert(command.end(), arguments.begin(), arguments.end());
                e.participant_started(p.name, children_[i]->id(), command, run.log);
            });
        }
        catch (const std::exception& e)
        {
            run.start_error = one_line(e.what());
            out_.event([&](RunEvents& stream) { stream.participant_not_started(p.name, run.start_error); });
            out_.say(p.name + " could not be started: " + run.start_error);
            if (p.required || bridge)
            {
                return false;
            }
        }
    }
    return true;
}

void RunParticipants::show_manual(const ParticipantConfig& p, const std::filesystem::path& executable,
                                  const std::vector<std::string>& arguments, const std::filesystem::path& workDir)
{
    std::filesystem::path native = executable;
    native.make_preferred();
    std::filesystem::path nativeDir = workDir;
    nativeDir.make_preferred();
    const std::string line = detail::shell_command_line(process::utf8_from_path(native), arguments);
    // As run gives the participants it starts; only if it is 1 (0 is what a participant does
    // without it), so that the command stays as it was for a simulation on this computer.
    std::vector<std::pair<std::string, std::string>> environment;
    if (registry_on_network(ctx_.config))
    {
        environment.emplace_back(kListenOnNetworkVariable, listen_variable_value(ctx_.config));
    }
    const auto held = out_.hold();
    out_.event([&](RunEvents& e) {
        std::vector<std::string> command{process::shown_path(executable)};
        command.insert(command.end(), arguments.begin(), arguments.end());
        e.participant_manual(p.name, command, process::shown_path(workDir), line, environment);
    });
    std::string with;
    for (const auto& [name, value] : environment)
    {
        with += (with.empty() ? ", with " : " and ") + name + "=" + value;
    }
    held.say("not starting " + p.name + ": start it yourself (manual: true, or --manual)" +
             (p.required ? std::string("; the simulation waits for it to join") : std::string{}) + ". In the directory " +
             process::utf8_from_path(nativeDir) + with + (with.empty() ? "" : " in its environment") + ", run:");
    held.say("  " + line);
}

std::vector<OutsideOwn> RunParticipants::own_bridges() const
{
    std::vector<OutsideOwn> out(outcome_.participants.size());
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        const ParticipantRun& run = outcome_.participants[i];
        if (stops_run(run) && children_[i])
        {
            // A process that has ended may still be in another bridge's last report: its id counts as ours.
            out[i] = OutsideOwn{!run.exit.has_value(), children_[i]->id()};
        }
    }
    return out;
}

int RunParticipants::look_at_children(bool before_start, bool bridges_only)
{
    int ended = -1;
    for (std::size_t i = 0; i < children_.size(); ++i)
    {
        ParticipantRun& run = outcome_.participants[i];
        if (!children_[i] || run.exit || (bridges_only && !stops_run(run)))
        {
            continue;
        }
        if (auto status = children_[i]->poll())
        {
            run.exit = status;
            run.ending = before_start ? Ending::ExitedBeforeStart : Ending::Exited;
            out_.say(run.name + " " + exit_text(*status));
            out_.event([&](RunEvents& e) { e.participant_ended(run.name, *status); });
            if ((run.required || stops_run(run)) && ended < 0)
            {
                ended = static_cast<int>(i);
            }
        }
    }
    return ended;
}

int RunParticipants::gone_process(bool left)
{
    int ended = -1;
    static_cast<void>(wait_until(std::chrono::milliseconds(left ? 2000 : 0), 1, [&] {
        ended = look_at_children(true);
        return ended >= 0;
    }));
    return ended;
}

bool RunParticipants::any_running(bool required_only)
{
    static_cast<void>(look_at_children(false));
    for (std::size_t i = 0; i < children_.size(); ++i)
    {
        const ParticipantRun& run = outcome_.participants[i];
        if (children_[i] && !run.exit && run.ending != Ending::Killed && (!required_only || run.required))
        {
            return true;
        }
    }
    return false;
}

void RunParticipants::end_children(bool ran, int interrupts_at_end)
{
    const std::chrono::milliseconds stopTimeout(ctx_.config.stop_timeout_ms);
    // An interrupt while the launcher waits for the participants to end by themselves stops
    // the waiting (a first one too: the simulation has ended already); a second interrupt in
    // all skips the wait after the stop requests as well.
    bool waitedInFull = false;
    if (ran)
    {
        const Waited waited = wait_until(stopTimeout, interrupts_at_end, [this] { return !any_running(true); });
        waitedInFull = waited == Waited::TimedOut;
        if (waited == Waited::Interrupted && outcome_.end.interrupted_by.empty())
        {
            ctx_.note_interrupt(outcome_.end);
            outcome_.end.interrupted_while_ending = true;
            out_.say(outcome_.end.stop_text() + ": no more waiting for the participants to end");
        }
    }
    std::vector<std::size_t> asked;
    for (std::size_t i = 0; i < children_.size(); ++i)
    {
        ParticipantRun& run = outcome_.participants[i];
        if (!children_[i] || run.exit)
        {
            continue;
        }
        run.stopped_after_end = waitedInFull && run.required;
        const bool sent = children_[i]->request_stop();
        switch (detail::record_stop_request(run, sent, children_[i]->poll()))
        {
        case detail::StopRequest::Sent:
            out_.say("asking " + run.name + " to stop (" + std::string(stop_request_name()) + ")");
            out_.event([&run](RunEvents& e) { e.participant(run.name, ParticipantState::StopRequested); });
            asked.push_back(i);
            break;
        case detail::StopRequest::HadEnded:
            out_.say(run.name + " " + exit_text(*run.exit));
            out_.event([&run](RunEvents& e) { e.participant_ended(run.name, *run.exit); });
            break;
        case detail::StopRequest::CannotSend:
            out_.say("cannot ask " + run.name + " to stop: ending it");
            children_[i]->kill_tree();
            run.ending = Ending::Killed;
            out_.event([&run](RunEvents& e) { e.participant(run.name, ParticipantState::Killed); });
            break;
        }
    }
    if (!asked.empty())
    {
        // The first process of each and what it started: a wrapper (sh -c) may end before the
        // program it started has cleaned up.
        static_cast<void>(wait_until(stopTimeout, 1, [this, &asked] {
            return std::all_of(asked.begin(), asked.end(), [this](std::size_t i) {
                return children_[i]->poll().has_value() && !children_[i]->others_running();
            });
        }));
    }
    for (const std::size_t i : asked)
    {
        ParticipantRun& run = outcome_.participants[i];
        if (auto status = children_[i]->poll())
        {
            run.exit = status;
            run.ending = Ending::Stopped;
            out_.event([&run](RunEvents& e) { e.participant_ended(run.name, *run.exit); });
        }
        else
        {
            out_.say("ending " + run.name + " (it did not stop)");
            children_[i]->kill_tree();
            run.ending = Ending::Killed;
            out_.event([&run](RunEvents& e) { e.participant(run.name, ParticipantState::Killed); });
        }
    }
    for (std::size_t i = 0; i < children_.size(); ++i)
    {
        if (!children_[i])
        {
            continue;
        }
        ParticipantRun& run = outcome_.participants[i];
        if (run.ending != Ending::Killed && children_[i]->others_running())
        {
            run.left_processes = true;
            out_.say(run.name + " left processes running: ending them");
            children_[i]->kill_tree();
        }
    }
    for (auto& child : children_)
    {
        if (child)
        {
            static_cast<void>(child->finish_output(std::chrono::milliseconds(2000)));
        }
    }
}

void RunParticipants::note_bridges_ended_with_run()
{
    // Only the bridges: the end of the other participants is noted, and written, when they are
    // ended, after the phase "stopping".
    static_cast<void>(look_at_children(false, true));
    for (ParticipantRun& run : outcome_.participants)
    {
        if (stops_run(run) && run.exit && ended_by_itself(run))
        {
            run.ended_with_run = true;
        }
    }
}

void RunParticipants::note_last_lines()
{
    for (std::size_t i = 0; i < outcome_.participants.size(); ++i)
    {
        ParticipantRun& run = outcome_.participants[i];
        if (stops_run(run))
        {
            run.last_line = out_.last_line(i);
            // Its last counts (also of one that was killed: those of its last line while it ran).
            const std::optional<OutsideCounts> counts = out_.take_counts(i);
            for (RunOutside& o : outcome_.outside)
            {
                if (o.network == run.for_network && counts)
                {
                    o.to_outside = counts->to_outside;
                    o.from_outside = counts->from_outside;
                }
                if (o.network == run.for_network)
                {
                    // The run's own stop for it (RunWatch) stays.
                    std::string lost = std::move(o.link.lost);
                    o.link = out_.take_link(i, std::chrono::steady_clock::now());
                    o.link.lost = std::move(lost);
                }
            }
        }
    }
}

void RunParticipants::end_manual(SimulationController& controller, bool ran, int interrupts_at_end)
{
    std::vector<std::size_t> joined;
    for (std::size_t i = 0; i < outcome_.participants.size(); ++i)
    {
        ParticipantRun& run = outcome_.participants[i];
        if (run.enabled && run.manual && controller.runtime().peer_status(run.name).state)
        {
            run.manual_joined = true;
            joined.push_back(i);
        }
    }
    if (joined.empty())
    {
        return;
    }
    if (!ran)
    {
        controller.abort();
    }
    const auto gone = [&](std::size_t i) {
        const auto state = controller.runtime().peer_status(outcome_.participants[i].name).state;
        return state && (*state == orch::ParticipantState::Shutdown || *state == orch::ParticipantState::Error);
    };
    // As for the processes: an interrupt (one more than had come when the simulation ended)
    // cuts the wait short.
    static_cast<void>(wait_until(std::chrono::milliseconds(ctx_.config.stop_timeout_ms), interrupts_at_end,
                                 [&] { return std::all_of(joined.begin(), joined.end(), gone); }));
    for (const std::size_t i : joined)
    {
        if (!gone(i))
        {
            out_.say(outcome_.participants[i].name + " (started by you, manual) has not left the simulation " +
                std::to_string(ctx_.config.stop_timeout_ms) +
                " ms after it ended: the launcher cannot end it, so end it yourself");
        }
    }
}

} // namespace detail

} // namespace vehicle_sil::launch
