// SPDX-License-Identifier: MIT
//
// The launch_run scenarios about run --events and --stdin-commands: the event lines, a reader
// that is slow, stalls or goes away, and the stop command:
//
//   events       run --events: the example of the docs byte for byte (after its placeholders), the
//                whole example's events (seq, order, the result as --json writes it), nothing on stderr
//   events_usage run --events with a wrong command line (also one the check finds) or a file that
//                cannot be read: which events come
//   events_findings a participant leaves the simulation and its process ends later: every finding
//                event is in the result, and none comes twice; the state "failed" comes at once; a
//                process that ends in a step: one "failed" (by the end of the process or because it
//                left the simulation), one "ended", one finding with the exit code
//   events_stalled a reader that does not read: the participants are not held up; a stop command, a
//                signal and --timeout-ms work all the same; the launcher gives the reader up after
//                its time limit
//   events_stderr the reader reads neither the events nor stderr (one pipe): the launcher ends
//   events_slow  a reader that falls behind and catches up: log lines are dropped and counted
//                (log_dropped), nothing else, and the result comes last
//   events_nonblocking (POSIX) the standard output is a pipe that does not block: nothing is lost
//   events_output (POSIX) the standard output is a file; there is none (exit 4)
//   stdin_stop   run --stdin-commands: a line that is no command, then {"command":"stop"}: stopped in
//                order (130); the end of input after it is no second stop
//   stdin_eof    without --stdin-commands a closed standard input is never read; with it, its end
//                stops the run in order
//   stdin_twice  a second stop command ends the waiting for a participant that does not end
//   stdin_ending a stop command while the launcher waits for the participants after the simulation
//   output_closed the reader of the events goes away (while the simulation runs, while the launcher
//                waits for the start, while it ends the participants): it stops in order, leaves nothing
//   stop_once    the end of input and a closed output after a stop are no second stop
//   stdin_killed the input closes and the launcher is killed at once: no participant is left
//   no_console_stdin (Windows) the launcher without a console is stopped in order by a command
#include "launch_run_support.hpp"

/// The events of a stream that was cut (the launcher gave its reader up): framed up to the cut,
/// without a result; the last line may be a part of one.
void check_cut_events(const std::vector<std::string>& lines)
{
    for (std::size_t i = 0; i + 1 < lines.size(); ++i)
    {
        check_frame(lines[i], i);
        require(value_of(lines[i], "event") != "result", "no result in a stream that was cut:\n" + lines[i]);
    }
}

/// `lines` without the state events: the history. (How many states are written depends on the
/// reader: one that still waits is replaced by the next.)
std::vector<std::string> history_of(const std::vector<std::string>& lines)
{
    std::vector<std::string> out;
    for (const std::string& line : lines)
    {
        if (value_of(line, "event") != "state")
        {
            out.push_back(line);
        }
    }
    return out;
}

/// `text` (event lines) without the heartbeat events, the state events and the time events between
/// the first and the last (how many there are depends on how long the run takes and on how fast
/// the events are read), "seq" counted without them.
std::string without_heartbeats(const std::string& text)
{
    std::string out;
    std::size_t seq = 0;
    const std::vector<std::string> all = lines_of(text);
    std::vector<std::size_t> times;
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        if (value_of(all[i], "event") == "time")
        {
            times.push_back(i);
        }
    }
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        const std::string& line = all[i];
        if (value_of(line, "event") == "heartbeat" || value_of(line, "event") == "state" ||
            (value_of(line, "event") == "time" && i != times.front() && i != times.back()))
        {
            continue;
        }
        const auto at = line.find(",\"seq\":") + 7;
        out += line.substr(0, at) + std::to_string(seq++) + line.substr(line.find(',', at)) + "\n";
    }
    return out;
}

/// `pretty` (the JSON of --json) without the line ends and spaces outside its strings.
std::string compact(const std::string& pretty)
{
    std::string out;
    bool inString = false;
    for (std::size_t i = 0; i < pretty.size(); ++i)
    {
        const char c = pretty[i];
        if (inString && c == '\\')
        {
            out += c;
            out += pretty[++i];
            continue;
        }
        if (c == '"')
        {
            inString = !inString;
        }
        if (inString || (c != ' ' && c != '\n'))
        {
            out += c;
        }
    }
    return out;
}

/// `text` with what differs from run to run as the docs show it: <t_ms>, <pid>, <port>, <dir>
/// (the launcher's working directory `dir` in each form it may be written), <version>; and the
/// ".exe" of the example's executables.
std::string with_placeholders(std::string text, const fs::path& dir)
{
    const auto replace_all = [&text](const std::string& from, const std::string& to) {
        for (auto at = text.find(from); !from.empty() && at != std::string::npos; at = text.find(from, at + to.size()))
        {
            text.replace(at, from.size(), to);
        }
    };
    std::error_code ec;
    for (const fs::path& form : {fs::canonical(dir, ec), fs::weakly_canonical(fs::absolute(dir), ec),
                                 fs::absolute(dir).lexically_normal()})
    {
        std::string shown = shown_path(form);
        while (!shown.empty() && shown.back() == '/')
        {
            shown.pop_back();
        }
        replace_all(shown + "/", "<dir>/");
    }
    replace_all("vehicle_sil_engine_ecu.exe", "vehicle_sil_engine_ecu");
    for (const std::string key : {"\"t_ms\":", "\"pid\":", "silkit://127.0.0.1:"})
    {
        const std::string placeholder = key == "\"t_ms\":" ? "<t_ms>" : key == "\"pid\":" ? "<pid>" : "<port>";
        for (auto at = text.find(key); at != std::string::npos; at = text.find(key, at + 1))
        {
            const auto from = at + key.size();
            auto to = from;
            while (to < text.size() && text[to] >= '0' && text[to] <= '9')
            {
                ++to;
            }
            text.replace(from, to - from, placeholder);
        }
    }
    const std::string version = "\"vehicle_sil\":\"";
    for (auto at = text.find(version); at != std::string::npos; at = text.find(version, at + 1))
    {
        const auto from = at + version.size();
        text.replace(from, text.find('"', from) - from, "<version>");
    }
    return text;
}

void events(const Options& o)
{
    const fs::path dir = o.example_dir;
    const Captured alone = launch(o, {"run", "simulation.yaml", "--events", "--disable", "Dashboard"}, dir);
    expect_code(alone, 0, "run simulation.yaml --events --disable Dashboard");
    static_cast<void>(check_events(alone.standard_output));
    // Without the heartbeats: how many there are depends on how long the run takes.
    expect_text(without_timing(with_placeholders(without_heartbeats(alone.standard_output), dir)),
                block_after(o.docs, "<!-- vehicle_sil_launch run simulation.yaml --events --disable Dashboard -->"),
                "run simulation.yaml --events --disable Dashboard");
    expect_text(alone.standard_error, "", "run --events: stderr");

    // Both ECUs: their lines come as events of their own, in no fixed order with the rest.
    const Captured both = launch(o, {"run", "--events", "simulation.yaml"}, dir);
    expect_code(both, 0, "run --events simulation.yaml");
    const std::vector<std::string> lines = check_events(both.standard_output);
    expect_text(both.standard_error, "", "run --events: stderr");
    require(!events_with(lines, {"\"event\":\"log\"", "\"source\":\"Dashboard\"",
                                 "\"line\":\"t=0 ms engine speed 0 rpm (0 messages)\""})
                 .empty(),
            "Dashboard's lines are log events:\n" + both.standard_output);
    for (const std::string name : {"Engine", "Dashboard"})
    {
        const std::string who = "\"name\":\"" + name + "\"";
        for (const std::string state : {"started", "joined", "ended"})
        {
            require(events_with(lines, {"\"event\":\"participant\"", who, "\"state\":\"" + state + "\""}).size() == 1,
                    name + " is " + state + " once:\n" + both.standard_output);
        }
    }
    // The virtual time when it changes: 0 at the start, stop_at_ms at the end, at most every 100 ms
    // in between (more on a slow machine), always further on.
    const auto times = events_with(lines, {"\"event\":\"time\""});
    require(times.size() >= 2 && contains(times.front(), "\"sim_ms\":0,") && contains(times.back(), "\"sim_ms\":1000,"),
            "the virtual time at 0 and at stop_at_ms:\n" + both.standard_output);
    for (std::size_t i = 1; i < times.size(); ++i)
    {
        require(std::stoll(value_of(times[i], "sim_ms")) > std::stoll(value_of(times[i - 1], "sim_ms")) &&
                    (i + 1 == times.size() ||
                     // (t_ms is stamped when the event is written, a moment after the look at
                     // the clock that spaced them: room for that.)
                     std::stoll(value_of(times[i], "t_ms")) - std::stoll(value_of(times[i - 1], "t_ms")) >= 90),
                "the time moves on, at most every 100 ms:\n" + times[i - 1] + "\n" + times[i]);
        require(contains(times[i], "\"speed\":\"max\",\"paused\":false,") && contains(times[i], "\"late_steps\":0,\"max_late_ms\":0,\"slipped_ms\":0,\"late_ms\":0}"),
                "as fast as possible, never late:\n" + times[i]);
    }
    // The result is the JSON of --json.
    const Captured json = launch(o, {"run", "--json", "simulation.yaml"}, dir);
    expect_code(json, 0, "run --json simulation.yaml");
    const std::string& last = lines.back();
    expect_text(without_timing(last.substr(last.find(",\"result\":") + 10)),
                without_timing(compact(json.standard_output.substr(0, json.standard_output.size() - 1)) + "}"),
                "the result event as run --json");
}

fs::path two_ecus(const Options& o);

void events_usage(const Options& o)
{
    const fs::path dir = o.work_dir;
    // hello comes first whatever is wrong with the command line, the result last.
    for (const auto& [args, message] :
         std::vector<std::pair<std::vector<std::string>, std::string>>{
             {{"run", "--events", "--bad", "x.yaml"}, "unknown option --bad"},
             {{"run", "--events", "--json", "x.yaml"}, "--json and --events cannot be used together"},
             {{"run", "--events", "--events-stall-ms", "999", "x.yaml"}, "--events-stall-ms must be from 1000 to 86400000"},
             {{"run", "--events", "--events-stall-ms", "soon", "x.yaml"}, "invalid value for --events-stall-ms: soon"},
             {{"run", "--events"}, "run needs the configuration file"}})
    {
        const Captured run = launch(o, args, dir);
        expect_code(run, 4, args.back());
        const std::vector<std::string> lines = check_events(run.standard_output);
        require(lines.size() == 2 && contains(lines[1], "\"result\":\"usage\"") && contains(lines[1], message),
                "hello and the result of a usage error:\n" + run.standard_output);
        require(contains(lf(run.standard_error), "error: " + message), "the error on stderr:\n" + run.standard_error);
    }
    // The limit is for the events: without them it is a mistake (and no event is written).
    const Captured alone = launch(o, {"run", "--events-stall-ms", "5000", "x.yaml"}, dir);
    expect_code(alone, 4, "run --events-stall-ms without --events");
    require(alone.standard_output.empty() && contains(lf(alone.standard_error), "error: --events-stall-ms is for --events"),
            "the error on stderr:\n" + alone.standard_error);
    const Captured missing = launch(o, {"run", "--events", "missing.yaml"}, dir);
    expect_code(missing, 3, "run --events missing.yaml");
    const std::vector<std::string> lines = history_of(check_events(missing.standard_output));
    require(lines.size() == 5 && value_of(lines[0], "file") == "missing.yaml" && contains(lines[2], "\"event\":\"check\"") &&
                contains(lines[4], "\"result\":\"unreadable\""),
            "hello, checking, check, ended and the result of a file that cannot be read:\n" + missing.standard_output);
    expect_text(missing.standard_error, "", "run --events missing.yaml: stderr");

    // A wrong command line that only the check finds (a name the file does not have): the check
    // is an event, then the result "usage".
    const fs::path sim = two_ecus(o);
    const Captured name = launch(o, {"run", "--events", "--disable", "Nobody", "sim.yaml"}, sim);
    expect_code(name, 4, "run --events --disable Nobody");
    const std::vector<std::string> states = events_with(check_events(name.standard_output), {"\"event\":\"state\""});
    // The state at the check and at the end, each after its phase (the first is not written if
    // the second came before the writer got to it): nothing was set up, nothing was asked to stop.
    require(!states.empty() && states.size() <= 2 && (states.size() == 1 || contains(states[0], "\"rev\":1,\"phase\":\"checking\",")) &&
                contains(states.back(), "\"rev\":2,\"phase\":\"ended\",\"paused\":false,\"holding\":false,\"speed\":null,"
                                        "\"pacing\":\"not-paced\",\"stop\":{\"requests\":0,\"by\":null},\"scenario\":false,") &&
                contains(states.back(), "\"outside\":[]}"),
            "a state after each phase of a run that ends at its check:\n" + name.standard_output);
    const std::vector<std::string> found = history_of(check_events(name.standard_output));
    require(found.size() == 5 && contains(found[1], "\"phase\":\"checking\"") && contains(found[2], "\"event\":\"check\"") &&
                contains(found[2], "\"result\":\"usage\"") && contains(found[3], "\"phase\":\"ended\"") &&
                contains(found[4], "\"event\":\"result\"") && contains(found[4], "\"result\":\"usage\"") &&
                contains(found[4], "Nobody"),
            "hello, checking, check, ended and the result of a usage error the check finds:\n" + name.standard_output);
}

/// Two ECUs that run until they are stopped (no stop_at_ms), with their process IDs in files.
fs::path two_ecus(const Options& o)
{
    return scenario_dir(o,
                        "vehicle_sil_simulation: 1\n"
                        "networks: []\n"
                        "participants:\n"
                        "  - name: Engine\n"
                        "    command: [" + ecu(o) + "]\n"
                        "  - name: Brake\n"
                        "    command: [" + ecu(o) + "]\n",
                        {{"Engine", "pid"}, {"Brake", "pid"}});
}

/// The run stopped in order: the result says `by`, both ECUs ended by themselves with 0.
void expect_stopped_in_order(const std::optional<vehicle_sil::process::ExitStatus>& status,
                             const std::vector<std::string>& lines, const std::string& by)
{
    std::string all;
    for (const auto& line : lines)
    {
        all += line + "\n";
    }
    require(status.has_value() && status->code == 130 && !status->signal,
            "the launcher ends with 130 when stopped, not " + (status ? std::to_string(status->code) : std::string("running")) +
                ":\n" + all);
    const std::string& result = lines.back();
    require(contains(result, "\"result\":\"interrupted\"") && contains(result, "\"simulation\":\"interrupted\"") &&
                contains(result, "\"interrupted_by\":\"" + by + "\""),
            "the result of a run stopped by " + by + ":\n" + result);
    for (const std::string name : {"Engine", "Brake"})
    {
        require(contains(result, "{\"name\":\"" + name + "\",\"required\":true,\"enabled\":true,\"manual\":false,\"for_network\":null,\"ended\":\"exited\","
                                                          "\"exit_code\":0,"),
                name + " ended by itself with 0:\n" + result);
    }
    const auto note = events_with(lines, {"\"event\":\"finding\"", "\"code\":\"interrupted\"",
                                          "\"message\":\"interrupted (" + by + "): the launcher stopped the simulation and the "
                                                                               "participants\""});
    require(note.size() == 1, "the note interrupted as a finding:\n" + all);
    // Known when the simulation has stopped: written then, before the participants are ended.
    const auto stopping = events_with(lines, {"\"event\":\"phase\"", "\"phase\":\"stopping\""});
    require(stopping.size() == 1 && std::stoll(value_of(note[0], "seq")) < std::stoll(value_of(stopping[0], "seq")),
            "the note comes before the stopping phase:\n" + all);
}

void stdin_stop(const Options& o)
{
    const fs::path dir = two_ecus(o);
    Live live(o, dir, {"run", "--events", "--stdin-commands", "--start-paused", "sim.yaml"});
    live.wait_for({"\"phase\":\"running\""});
    // Nothing happens (it stands still at 0): a heartbeat every second.
    live.wait_for_count({"{\"event\":\"heartbeat\",\"seq\":"}, 2);
    live.write("not a command\n{\"command\":\"jump\"}\n");
    live.wait_for({"\"command\":\"jump\"", "\"accepted\":false"});
    live.write("{\"command\":\"stop\"}\n");
    live.wait_for({"\"phase\":\"stopping\""});
    // The end of input after a stop is no second stop (the ECUs still end by themselves).
    live.process().close_input();
    const auto [status, lines] = live.finish();
    const bool gone = all_gone(dir, {"Engine.pid", "Brake.pid"}, 30s);
    expect_stopped_in_order(status, lines, "stop command");
    const auto commands = events_with(lines, {"\"event\":\"command\""});
    require(commands.size() == 3, "three commands, no event for the end of input after the stop");
    require(contains(commands[0], "\"command\":null,\"source\":\"stdin\",\"accepted\":false,\"message\":\"the line is "
                                  "not a JSON object: "),
            "a line that is no JSON object:\n" + commands[0]);
    require(contains(commands[1], "\"message\":\"unknown command 'jump': the commands are stop, pause, resume, step, "
                                  "run_until and speed\""),
            "an unknown command:\n" + commands[1]);
    require(contains(commands[2], "\"command\":\"stop\",\"source\":\"stdin\",\"accepted\":true,\"message\":null,\"reason\":null,\"id\":null}"),
            "the stop:\n" + commands[2]);
    // The command comes before what it does.
    const auto at = [&lines](const std::string& part) {
        return std::find_if(lines.begin(), lines.end(), [&](const std::string& l) { return contains(l, part); }) - lines.begin();
    };
    require(at("\"accepted\":true") < at("\"phase\":\"stopping\""), "the stop command comes before the stopping phase");
    require(gone, "no participant is left");
    // The process IDs are the real ones; the time is written when it changes, so only once here.
    require(value_of(lines[0], "pid") == std::to_string(live.process().id()), "hello's pid is the launcher's:\n" + lines[0]);
    for (const std::string name : {"Engine", "Brake"})
    {
        const auto started = events_with(lines, {"\"name\":\"" + name + "\"", "\"state\":\"started\""});
        require(started.size() == 1 && value_of(started[0], "pid") == std::to_string(read_pid((dir / (name + ".pid")).string(), 0ms)),
                name + "'s started event has its process ID");
    }
    // Standing still at 0 from the start; the end (it stands still nowhere then).
    const auto times = events_with(lines, {"\"event\":\"time\""});
    require(times.size() == 2 && contains(times[0], "\"sim_ms\":0,\"speed\":\"max\",\"paused\":true,") &&
                contains(times[1], "\"sim_ms\":0,\"speed\":\"max\",\"paused\":false,"),
            "the time is written when it changes:\n" + (times.empty() ? std::string{} : times[0]));
    const auto beats = events_with(lines, {"\"event\":\"heartbeat\""});
    require(!beats.empty() && members_of(beats[0]).empty(), "a heartbeat has no members:\n" + beats[0]);
}

void stdin_eof(const Options& o)
{
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 300\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n");
    // Without --stdin-commands, the input is not read: closed at once, it changes nothing.
    {
        Live live(o, dir, {"run", "--events", "sim.yaml"});
        live.process().close_input();
        const auto [status, lines] = live.finish();
        require(status.has_value() && status->code == 0, "the run without --stdin-commands passes");
        require(contains(lines.back(), "\"simulation\":\"reached_stop_at\"") && events_with(lines, {"\"event\":\"command\""}).empty(),
                "it ran to stop_at_ms:\n" + lines.back());
    }
    const fs::path forever = two_ecus(o);
    Live live(o, forever, {"run", "--events", "--stdin-commands", "sim.yaml"});
    live.wait_for({"\"phase\":\"running\""});
    live.process().close_input();
    const auto [status, lines] = live.finish();
    const bool gone = all_gone(forever, {"Engine.pid", "Brake.pid"}, 30s);
    expect_stopped_in_order(status, lines, "end of input");
    require(events_with(lines, {"\"event\":\"command\",", "\"command\":\"stop\",\"source\":\"eof\",\"accepted\":true"}).size() == 1,
            "the end of input is a stop command");
    require(gone, "no participant is left");
}

void stdin_twice(const Options& o)
{
    // Deaf does not end after the simulation and ignores the stop request; stop_timeout_ms would
    // wait 10 minutes twice. A second stop command ends the waiting.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_timeout_ms: 600000\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Deaf\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Deaf", "pid hang-after-end ignore-stop"}});
    Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"});
    live.wait_for({"\"phase\":\"running\""});
    live.write("{\"command\":\"stop\"}\n");
    live.wait_for({"\"phase\":\"stopping\""});
    live.write("{\"command\":\"stop\"}\n");
    const auto [status, lines] = live.finish();
    const bool gone = all_gone(dir, {"Deaf.pid"}, 30s);
    require(status.has_value() && status->code == 130, "the launcher ends with 130 after the second stop");
    require(events_with(lines, {"\"name\":\"Deaf\"", "\"state\":\"killed\""}).size() == 1, "Deaf was killed");
    require(contains(lines.back(), "\"ended\":\"killed\""), "the result says so:\n" + lines.back());
    require(gone, "Deaf has ended");
}

void stdin_ending(const Options& o)
{
    // Stuck does not end after the simulation; the launcher would wait 10 minutes for it. A stop
    // command while it waits: it asks Stuck to stop at once.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 1000\n"
                                      "stop_timeout_ms: 600000\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Stuck\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Stuck", "pid hang-after-end stop-exit 9"}});
    Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"});
    live.wait_for({"\"phase\":\"stopping\""});
    wait_hanging(dir, "Stuck");
    live.write("{\"command\":\"stop\"}\n");
    const auto [status, lines] = live.finish();
    const bool gone = all_gone(dir, {"Stuck.pid"}, 30s);
    require(status.has_value() && status->code == 130, "the launcher ends with 130 when stopped while it waits");
    require(contains(lines.back(), "\"result\":\"interrupted\"") && contains(lines.back(), "\"simulation\":\"reached_stop_at\""),
            "the result:\n" + lines.back());
    require(events_with(lines, {"\"name\":\"Stuck\"", "\"state\":\"stop_requested\""}).size() == 1 ||
                !can_request_stop(),
            "Stuck was asked to stop");
    // A finding known only after the participants have ended is an event too, before the result.
    const auto notes = events_with(lines, {"\"event\":\"finding\"", "\"code\":\"interrupted\""});
    require(notes.size() == 1 && contains(notes[0], "the launcher stopped the participants instead of waiting for them to end"),
            "the note as a finding");
    require(gone, "Stuck has ended");
}

void output_closed(const Options& o)
{
    {
        const fs::path dir = two_ecus(o);
        Live live(o, dir, {"run", "--events", "sim.yaml"});
        live.wait_for({"\"phase\":\"running\""});
        // The reader goes away: the launcher's events have nowhere to go (its stderr closes too).
        live.process().finish_output(0ms);
        const auto status = live.process().wait_for(scaled_timeout(120s));
        const bool gone = all_gone(dir, {"Engine.pid", "Brake.pid"}, 30s);
        require(status.has_value() && status->code == 130 && !status->signal,
                "the launcher stops in order (130) when its output closes, not " +
                    (status ? std::to_string(status->code) + (status->signal ? " (signal)" : "") : std::string("running")));
        require(gone, "no participant is left");
    }
    // While it waits for a participant that never joins (10 minutes): nothing but the heartbeat
    // is written then, and the launcher notices all the same.
    {
        const fs::path dir = scenario_dir(o, "vehicle_sil_simulation: 1\n"
                                             "start_timeout_ms: 600000\n"
                                             "networks: []\n"
                                             "participants:\n"
                                             "  - name: Ghost\n"
                                             "    command: [" + helper(o) + ", --on-stop, \"9\", --pid-file, ghost.pid, --sleep-ms, \"600000\", --uri, \"{registry_uri}\"]\n"
                                             "    describe: false\n");
        fs::remove(dir / "ghost.pid");
        Live live(o, dir, {"run", "--events", "sim.yaml"});
        live.wait_for({"\"phase\":\"waiting\""});
        const long ghost = read_pid((dir / "ghost.pid").string(), 60s);
        const auto closed = std::chrono::steady_clock::now();
        live.process().finish_output(0ms);
        const auto status = live.process().wait_for(scaled_timeout(60s));
        log("closed while waiting: the launcher ended after " +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - closed).count()) + " ms");
        const bool gone = all_gone(dir, {"ghost.pid"}, 30s);
        require(ghost > 0 && status.has_value() && status->code == 130, "the launcher stops (130) when its output closes while it waits for the start");
        require(gone, "no participant is left");
    }
    // While it waits for a participant to end after the simulation (10 minutes).
    {
        const fs::path dir = scenario_dir(o,
                                          "vehicle_sil_simulation: 1\n"
                                          "stop_at_ms: 100\n"
                                          "stop_timeout_ms: 600000\n"
                                          "networks: []\n"
                                          "participants:\n"
                                          "  - name: Stuck\n"
                                          "    command: [" + ecu(o) + "]\n",
                                          {{"Stuck", "pid hang-after-end stop-exit 9"}});
        fs::remove(dir / "Stuck.pid");
        Live live(o, dir, {"run", "--events", "sim.yaml"});
        live.wait_for({"\"phase\":\"stopping\""});
        wait_hanging(dir, "Stuck");
        live.process().finish_output(0ms);
        const auto status = live.process().wait_for(scaled_timeout(60s));
        const bool gone = all_gone(dir, {"Stuck.pid"}, 30s);
        require(status.has_value() && status->code == 130, "the launcher stops (130) when its output closes while it ends the participants");
        require(gone, "no participant is left");
    }
}

void stop_once(const Options& o)
{
    // Deaf does not end after the simulation and ignores the stop request: after a stop command the
    // launcher waits stop_timeout_ms (10 minutes) for it. The end of input and a closed output
    // after that stop are no second stop: it still waits.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_timeout_ms: 600000\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Deaf\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Deaf", "pid hang-after-end ignore-stop"}});
    Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"});
    live.wait_for({"\"phase\":\"running\""});
    const long deaf = read_pid((dir / "Deaf.pid").string(), 60s);
    live.write("{\"command\":\"stop\"}\n");
    live.wait_for({"\"phase\":\"stopping\""});
    live.process().close_input();
    std::this_thread::sleep_for(1500ms);
    const bool waitsAfterEof = !live.process().poll().has_value() && process_alive(deaf) &&
                               events_with(live.lines(), {"\"state\":\"killed\""}).empty();
    // A closed output is seen within a second (the heartbeat).
    live.process().finish_output(0ms);
    std::this_thread::sleep_for(3000ms);
    const bool waitsAfterClose = !live.process().poll().has_value() && process_alive(deaf);
    // The launcher's death ends Deaf.
    kill_one(live.process().id());
    static_cast<void>(live.process().wait_for(scaled_timeout(30s)));
    const bool gone = all_gone(dir, {"Deaf.pid"}, 30s);
    require(deaf > 0 && waitsAfterEof, "the end of input after a stop is no second stop: the launcher still waits for Deaf");
    require(waitsAfterClose, "a closed output after a stop is no second stop: the launcher still waits for Deaf");
    require(gone, "Deaf has ended");
}

/// Engine writes `lines` lines in every step, Brake nothing; they run until they are stopped
/// (or to `stop_at_ms`). Their process IDs are in files; nothing of an earlier run is left.
fs::path chatty(const Options& o, int lines, const std::string& more = "")
{
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n" + more +
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n"
                                      "  - name: Brake\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Engine", "pid chatter " + std::to_string(lines)}, {"Brake", "pid"}});
    std::error_code ec;
    fs::remove(dir / "Engine.pid", ec);
    fs::remove(dir / "Brake.pid", ec);
    fs::remove_all(dir / "logs", ec);
    return dir;
}

/// The size of `file` as a reader sees it now (0 if it cannot be opened): not the size in its
/// directory, which Windows brings up to date only when the writer closes the file.
std::uintmax_t size_now(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    const auto at = in ? static_cast<std::streamoff>(in.tellg()) : std::streamoff{0};
    return at > 0 ? static_cast<std::uintmax_t>(at) : 0;
}

/// Waits up to `timeout` (scaled) until `file` has grown by `bytes`; how much it grew.
std::uintmax_t growth(const fs::path& file, std::uintmax_t bytes, std::chrono::milliseconds timeout)
{
    const std::uintmax_t from = size_now(file);
    const auto deadline = std::chrono::steady_clock::now() + scaled_timeout(timeout);
    std::uintmax_t now = from;
    while (now < from + bytes && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(20ms);
        now = std::max(now, size_now(file));
    }
    return now - from;
}

/// What a scenario found, so that it can say everything that is wrong at its end (after it has
/// cleaned up).
struct Checks
{
    std::vector<std::string> failed;

    void expect(bool condition, const std::string& what)
    {
        log(std::string(condition ? "CHECK " : "FAILED ") + what);
        if (!condition)
        {
            failed.push_back(what);
        }
    }

    void done() const
    {
        std::string all;
        for (const auto& what : failed)
        {
            all += what + "\n";
        }
        require(failed.empty(), all);
    }
};

long long ms_since(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count();
}

/// A chatty run whose reader has stopped reading, once Engine has written 1 MiB more (the pipe
/// of the events is full by then, and what the launcher keeps in memory too).
struct Stalled
{
    fs::path dir;
    std::unique_ptr<Live> live;
    long engine = 0;
    long brake = 0;

    /// `grow`: false if the launcher may give the reader up before Engine has written that much.
    Stalled(const Options& o, const std::vector<std::string>& args, Checks& checks, bool grow = true) : dir(chatty(o, 20))
    {
        live = std::make_unique<Live>(o, dir, args);
        live->wait_for({"\"phase\":\"running\""});
        engine = read_pid((dir / "Engine.pid").string(), 60s);
        brake = read_pid((dir / "Brake.pid").string(), 60s);
        live->hold();
        if (!grow)
        {
            return;
        }
        const auto grown = growth(dir / "logs" / "Engine.log", 1 << 20, 30s);
        checks.expect(grown >= (1 << 20), "Engine goes on writing while the reader of the events does not read (its log grew by " +
                                              std::to_string(grown) + " bytes in at most 30 s)");
    }

    /// Both ECUs have ended within `timeout`.
    bool participants_gone(std::chrono::milliseconds timeout) const { return wait_gone(engine, timeout) && wait_gone(brake, timeout); }

    /// Ends what is left; the launcher's status after `timeout` (scaled) and its lines.
    std::pair<std::optional<vehicle_sil::process::ExitStatus>, std::vector<std::string>> end(std::chrono::milliseconds timeout)
    {
        auto status = live->process().wait_for(scaled_timeout(timeout));
        if (!status)
        {
            kill_one(live->process().id());
        }
        auto [last, lines] = live->finish_unchecked(30s);
        for (const long pid : {engine, brake})
        {
            if (process_alive(pid))
            {
                kill_one(pid);
            }
        }
        return {status, lines};
    }
};

void events_stalled(const Options& o)
{
    Checks checks;
    // The limit (--events-stall-ms) is far away: a stop command works while the reader does not
    // read, and a second one ends the launcher's wait for the reader (it cannot write its result).
    const std::vector<std::string> kRun{"run", "--events", "--events-stall-ms", "600000", "--stdin-commands", "--log-dir", "logs", "sim.yaml"};
    {
        Stalled run(o, kRun, checks);
        const auto asked = std::chrono::steady_clock::now();
        run.live->write("{\"command\":\"stop\"}\n");
        const bool gone = run.participants_gone(30s);
        checks.expect(gone, "a stop command stops the participants while the reader does not read (" + std::to_string(ms_since(asked)) + " ms)");
        checks.expect(!run.live->process().poll().has_value(), "the launcher waits for its reader to write the result");
        run.live->write("{\"command\":\"stop\"}\n");
        const auto [status, lines] = run.end(30s);
        checks.expect(status.has_value() && status->code == 130,
                      "a second stop command ends the wait: 130, " + code_text(status) + " after " + std::to_string(ms_since(asked)) + " ms");
        check_cut_events(lines);
    }
    // --timeout-ms aborts the simulation while the reader does not read. The run has to be under
    // way and its reader stalled before then (scaled: that takes longer on a slow machine).
    {
        std::vector<std::string> args = kRun;
        args.insert(args.begin() + 1, {"--timeout-ms", std::to_string(scaled_timeout(5s).count())});
        Stalled run(o, args, checks);
        checks.expect(run.participants_gone(60s), "--timeout-ms ends the run while the reader does not read");
        // Gone here includes a process that is ending but that the launcher cannot reap yet (its
        // first thread has ended, others have not: under a sanitizer that takes a while). A stop
        // then comes while the launcher waits for the participants to end, which is an interrupted
        // run (130), rightly. So wait until the launcher has reaped them: their process IDs are
        // free then. Windows shows no such thing from outside: a second is room there.
#ifdef _WIN32
        std::this_thread::sleep_for(1000ms);
#else
        for (const long pid : {run.engine, run.brake})
        {
            const auto limit = std::chrono::steady_clock::now() + scaled_timeout(30s);
            while (!(::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) && std::chrono::steady_clock::now() < limit)
            {
                std::this_thread::sleep_for(5ms);
            }
        }
#endif
        // Two: the launcher may still be ending the participants (their output), where the first
        // only ends that; a stop while it waits for the reader, or a second one, ends the wait.
        run.live->write("{\"command\":\"stop\"}\n");
        std::this_thread::sleep_for(200ms);
        static_cast<void>(run.live->process().write_input("{\"command\":\"stop\"}\n")); // it may have ended
        const auto [status, lines] = run.end(30s);
        // The run failed (the time-out) before the stop came; its result could not be written.
        checks.expect(status.has_value() && status->code == 3, "the result of the timed-out run cannot be written: 3, " + code_text(status));
        checks.expect(contains(run.live->err(), "error: cannot write the result"), "the launcher says so on stderr:\n" + run.live->err());
    }
    // A signal (Windows: Ctrl+Break, if there is a console to send it through).
    if (can_request_stop())
    {
        Stalled run(o, {"run", "--events", "--events-stall-ms", "600000", "--log-dir", "logs", "sim.yaml"}, checks);
        const auto interrupt = [&run] {
#ifdef _WIN32
            static_cast<void>(run.live->process().request_stop());
#else
            ::kill(static_cast<pid_t>(run.live->process().id()), SIGINT);
#endif
        };
        interrupt();
        checks.expect(run.participants_gone(30s), "an interrupt stops the participants while the reader does not read");
        interrupt();
        const auto [status, lines] = run.end(30s);
        checks.expect(status.has_value() && status->code == 130, "a second interrupt ends the wait: 130, " + code_text(status));
    }
    // The limit itself: the launcher gives the reader up, stops the run and says why.
    {
        Stalled run(o, {"run", "--events", "--events-stall-ms", "1500", "--log-dir", "logs", "sim.yaml"}, checks, false);
        const auto held = std::chrono::steady_clock::now();
        const bool ended = run.live->process().wait_for(scaled_timeout(30s)).has_value();
        const long long after = ms_since(held);
        const bool gone = run.participants_gone(10s);
        const auto [status, lines] = run.end(0ms);
        checks.expect(ended && status.has_value() && status->code == 130,
                      "the launcher gives up a reader that does not read: 130, " + code_text(status) + " after " +
                          std::to_string(after) + " ms");
        checks.expect(gone, "it has ended the participants");
        const std::string err = run.live->err();
        checks.expect(contains(err, "vehicle_sil_launch: cannot write the events (their reader has not read for 1500 ms") &&
                          contains(err, "stopping the run"),
                      "it says why on stderr:\n" + err);
        check_cut_events(lines);
    }
    checks.done();
}

void events_slow(const Options& o)
{
    // The reader stops for a while and reads again: the participants' lines that did not fit are
    // dropped and counted; everything else is written, and the stream ends with the result.
    Checks checks;
    Stalled run(o, {"run", "--events", "--events-stall-ms", "600000", "--stdin-commands", "--log-dir", "logs", "sim.yaml"}, checks);
    run.live->release();
    run.live->wait_for({"\"event\":\"log_dropped\""});
    run.live->write("{\"command\":\"stop\"}\n");
    const auto [status, lines] = run.live->finish();
    const bool gone = run.participants_gone(30s);
    checks.expect(status.has_value() && status->code == 130 && contains(lines.back(), "\"interrupted_by\":\"stop command\""),
                  "the run goes on and is stopped in order: " + code_text(status));
    // Every line of Engine is an event or is counted in a log_dropped event: as many as its log has.
    std::size_t events = 0;
    std::size_t dropped = 0;
    for (const auto& line : lines)
    {
        const std::string event = value_of(line, "event");
        events += event == "log" && value_of(line, "source") == "Engine" ? 1 : 0;
        if (event == "log_dropped")
        {
            const std::string engine = value_of(line.substr(line.find("\"sources\":")), "Engine");
            dropped += engine.empty() ? 0 : std::stoul(engine);
            checks.expect(std::stoul(value_of(line, "count")) >= (engine.empty() ? 0 : std::stoul(engine)) && members_of(line).rfind("\"count\":", 0) == 0,
                          "log_dropped has count and sources: " + line);
        }
    }
    std::size_t written = 0;
    {
        std::ifstream log(run.dir / "logs" / "Engine.log", std::ios::binary);
        for (std::string line; std::getline(log, line);)
        {
            ++written;
        }
    }
    checks.expect(dropped > 0, "lines were dropped while the reader did not read");
    checks.expect(events + dropped == written, "Engine wrote " + std::to_string(written) + " lines: " + std::to_string(events) +
                                                   " are events, " + std::to_string(dropped) + " are counted as dropped");
    checks.expect(gone, "no participant is left");
    checks.done();
}

void events_findings(const Options& o)
{
    // Engine's step fails at 200 ms; its process ends by itself 1.5 s after it has left the
    // simulation, while the launcher ends the participants. What the launcher knows about Engine
    // changes in between (first that it left, then how its process ended).
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 1000\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n"
                                      "  - name: Brake\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Engine", "throw-at-ms 200 linger-ms 1500"}});
    const Captured run = launch(o, {"run", "--events", "sim.yaml"}, dir);
    expect_code(run, 1, "run --events with an ECU whose step fails");
    std::string findings;
    for (const auto& line : lines_of(run.standard_output))
    {
        findings += value_of(line, "event") == "finding" ? line + "\n" : std::string{};
    }
    log("the finding events:\n" + findings);
    // check_events(): every finding event is in the result as written, and none comes twice.
    const std::vector<std::string> lines = check_events(run.standard_output);
    const auto failed = events_with(lines, {"\"event\":\"finding\"", "\"code\":\"participant-failed\""});
    require(failed.size() == 1 && contains(failed[0], "its process exited with code 1"),
            "one participant-failed finding, with how the process ended:\n" + findings);
    // That Engine failed is an event at once (never dropped, unlike the launcher's line): before
    // the others are stopped, before Engine's process has ended, before the finding.
    const auto seq = [&lines](const std::vector<std::string>& parts) {
        const auto found = events_with(lines, parts);
        require(found.size() == 1, "one event with " + parts.back() + ", not " + std::to_string(found.size()));
        log(found[0].substr(0, 400));
        return std::stoll(value_of(found[0], "seq"));
    };
    const std::string engine = "\"name\":\"Engine\"";
    const auto failedAt = seq({"\"event\":\"participant\"", engine, "\"state\":\"failed\",\"reason\":\"left_simulation\"}"});
    const auto stoppingAt = seq({"\"event\":\"phase\"", "\"phase\":\"stopping\""});
    const auto endedAt = seq({"\"event\":\"participant\"", engine, "\"state\":\"ended\""});
    const auto findingAt = seq({"\"event\":\"finding\"", "\"code\":\"participant-failed\""});
    require(failedAt < stoppingAt && stoppingAt < endedAt && endedAt < findingAt,
            "failed, then stopping, then Engine's ended, then the finding:\n" + run.standard_output);
    // A reader that does not know the state "failed" misses nothing else: the states it knows
    // come once each, as before.
    for (const std::string state : {"started", "joined", "ended"})
    {
        static_cast<void>(seq({"\"event\":\"participant\"", engine, "\"state\":\"" + state + "\""}));
    }
    // The same result as without --events.
    const Captured json = launch(o, {"run", "--json", "sim.yaml"}, dir);
    const std::string& last = lines.back();
    // (But for how long the two runs took, and for the controller's last step: the launcher
    // notices the failed participant either before or after the controller begins its step at
    // the time of the failure, so a run that a failure ends reaches that step or the one before.)
    const auto comparable = [](std::string text) {
        const std::string head = "\"sim_ms\":";
        const auto at = text.find(head);
        require(at != std::string::npos, "the result has sim_ms:\n" + text);
        const auto to = text.find_first_of(",}", at);
        const std::string value = text.substr(at + head.size(), to - at - head.size());
        require(value == "190" || value == "200", "the simulation got to the step of the failure (200 ms) or the one before, not to " + value);
        text.replace(at + head.size(), value.size(), "<sim_ms>");
        // The traffic is of the steps before that step (no network here: nothing else in it).
        const std::string before = "\"traffic\":{\"before_ms\":" + value + ",";
        const auto traffic = text.find(before);
        require(traffic != std::string::npos, "the result's traffic is of the steps before sim_ms:\n" + text);
        text.replace(traffic, before.size(), "\"traffic\":{\"before_ms\":<sim_ms>,");
        return without_timing(text);
    };
    expect_text(comparable(last.substr(last.find(",\"result\":") + 10)),
                comparable(compact(json.standard_output.substr(0, json.standard_output.size() - 1)) + "}"),
                "the result event as run --json");

    // Engine's process ends in a step (exit code 7). The launcher learns of the failure either by
    // the end of the process ("process_ended": "ended" comes first, and "failed" has the finding's
    // message) or, when its connection is gone first, because it left the simulation
    // ("left_simulation": "failed" comes before "ended", without a message). Either way: one
    // "failed", one "ended", one finding, which says how the process ended.
    write_file(dir / "Engine.behaviour", "exit-at-ms 200 7\n");
    const Captured crash = launch(o, {"run", "--events", "sim.yaml"}, dir);
    expect_code(crash, 1, "run --events with an ECU whose process ends");
    const std::vector<std::string> crashed = check_events(crash.standard_output);
    const auto state = events_with(crashed, {"\"event\":\"participant\"", "\"name\":\"Engine\"", "\"state\":\"failed\""});
    const auto ended = events_with(crashed, {"\"event\":\"participant\"", "\"name\":\"Engine\"", "\"state\":\"ended\""});
    const auto finding = events_with(crashed, {"\"event\":\"finding\"", "\"code\":\"participant-failed\""});
    require(state.size() == 1 && ended.size() == 1 && finding.size() == 1, "failed, ended and the finding once each:\n" + crash.standard_output);
    log(state[0]);
    require(contains(finding[0], "its process exited with code 7"), "the finding says how the process ended:\n" + finding[0]);
    const auto seqOf = [](const std::string& line) { return std::stoll(value_of(line, "seq")); };
    require(seqOf(state[0]) < seqOf(finding[0]), "failed before the finding:\n" + crash.standard_output);
    const std::string reason = value_of(state[0], "reason");
    if (reason == "process_ended")
    {
        require(seqOf(ended[0]) < seqOf(state[0]), "process_ended: ended, then failed:\n" + crash.standard_output);
        require(value_of(state[0], "message") == value_of(finding[0], "message"),
                "failed has the finding's message:\n" + state[0] + "\n" + finding[0]);
    }
    else
    {
        require(reason == "left_simulation", "failed with the reason process_ended or left_simulation, not " + reason + ":\n" + state[0]);
        require(seqOf(state[0]) < seqOf(ended[0]), "left_simulation: failed, then ended:\n" + crash.standard_output);
        require(!contains(state[0], "\"message\":"), "left_simulation: failed has no message:\n" + state[0]);
    }
}

void events_stderr(const Options& o)
{
    // The launcher's standard output and standard error are one pipe (2>&1 of a shell), and
    // nothing reads it: the line that says why the events closed has nowhere to go either. The
    // launcher ends all the same.
    const fs::path dir = chatty(o, 20);
    std::mutex mutex;
    std::condition_variable changed;
    bool held = true;
    vehicle_sil::process::ProcessOptions options;
    options.working_directory = dir;
    options.on_line = [&](vehicle_sil::process::Stream, std::string_view) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&held] { return !held; });
    };
    const std::vector<std::string> run{"run", "--events", "--events-stall-ms", "1500", "sim.yaml"};
#ifdef _WIN32
    const char* shell = std::getenv("ComSpec");
    require(shell != nullptr, "ComSpec is not set");
    std::vector<std::string> args{"/c", shown_path(o.launch)};
    args.insert(args.end(), run.begin(), run.end());
    args.push_back("2>&1");
#else
    const char* shell = "/bin/sh";
    std::vector<std::string> args{"-c", "exec \"$0\" \"$@\" 2>&1", o.launch.string()};
    args.insert(args.end(), run.begin(), run.end());
#endif
    const auto from = std::chrono::steady_clock::now();
    vehicle_sil::process::Process wrapper(shell, args, std::move(options));
    const long engine = read_pid((dir / "Engine.pid").string(), 60s);
    const long brake = read_pid((dir / "Brake.pid").string(), 60s);
    const auto status = wrapper.wait_for(scaled_timeout(60s));
    const long long after = ms_since(from);
    const bool gone = wait_gone(engine, 10s) && wait_gone(brake, 10s);
    if (!status)
    {
        wrapper.kill_tree();
    }
    for (const long pid : {engine, brake})
    {
        if (process_alive(pid))
        {
            kill_one(pid);
        }
    }
    {
        const std::lock_guard lock(mutex);
        held = false;
        changed.notify_all();
    }
    wrapper.finish_output(10s);
    require(engine > 0 && status.has_value() && status->code == 130,
            "the launcher gives the reader up and ends (130) although its stderr is not read either: " + code_text(status) +
                " after " + std::to_string(after) + " ms");
    log("ended after " + std::to_string(after) + " ms");
    require(gone, "no participant is left");
}

void events_nonblocking(const Options& o)
{
#ifdef _WIN32
    static_cast<void>(o);
    throw Skip("a standard output with O_NONBLOCK is a POSIX case");
#else
    // The launcher's standard output is a pipe whose writes do not block (whoever started it set
    // O_NONBLOCK on it), and its reader reads late: a full pipe is no closed pipe.
    const fs::path dir = chatty(o, 20, "stop_at_ms: 3000\n");
    int fds[2];
    require(::pipe(fds) == 0 && ::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL) | O_NONBLOCK) == 0, "a pipe");
    const std::string program = o.launch.string();
    const std::string in = dir.string();
    const pid_t pid = ::fork();
    if (pid == 0)
    {
        if (::chdir(in.c_str()) != 0 || ::dup2(fds[1], STDOUT_FILENO) < 0)
        {
            ::_exit(126);
        }
        ::close(fds[0]);
        ::close(fds[1]);
        ::execl(program.c_str(), program.c_str(), "run", "--events", "--log-dir", "logs", "sim.yaml", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    require(pid > 0, "fork");
    ::close(fds[1]);
    // The whole run writes about 6 MiB; the pipe holds 64 KiB.
    const auto grown = growth(dir / "logs" / "Engine.log", 512 * 1024, 60s);
    std::string out;
    char buffer[65536];
    for (ssize_t got = 0; (got = ::read(fds[0], buffer, sizeof(buffer))) != 0;)
    {
        require(got > 0 || errno == EINTR, "read");
        out.append(buffer, got > 0 ? static_cast<std::size_t>(got) : 0);
    }
    ::close(fds[0]);
    int status = 0;
    require(::waitpid(pid, &status, 0) == pid, "waitpid");
    require(grown >= 512 * 1024, "Engine wrote before the events were read");
    const std::vector<std::string> all = lines_of(out);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "the run passes although its output was full for a while: status " + std::to_string(status) + ", " +
                std::to_string(all.size()) + " lines, the last:\n" + (all.empty() ? std::string{} : all.back().substr(0, 300)));
    const std::vector<std::string> lines = check_events(all);
    require(contains(lines.back(), "\"result\":\"passed\""), "the result is the last line:\n" + lines.back().substr(0, 300));
#endif
}

void events_output(const Options& o)
{
#ifdef _WIN32
    static_cast<void>(o);
    throw Skip("the shell redirections of this scenario are POSIX");
#else
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 300\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n");
    vehicle_sil::process::CaptureOptions options;
    options.timeout = scaled_timeout(120s);
    options.working_directory = dir;
    // A file: every event is in it.
    const Captured file = vehicle_sil::process::run_captured(
        "/bin/sh", {"-c", "exec \"$0\" run --events sim.yaml > events.ndjson", o.launch.string()}, options);
    expect_code(file, 0, "run --events > file");
    const std::vector<std::string> lines = check_events(read_text(dir / "events.ndjson"));
    require(contains(lines.back(), "\"result\":\"passed\""), "the result is in the file");
    // No standard output at all: a mistake of whoever started it, said at once; nothing starts.
    const Captured none = vehicle_sil::process::run_captured(
        "/bin/sh", {"-c", "exec \"$0\" run --events sim.yaml >&-", o.launch.string()}, options);
    expect_code(none, 4, "run --events without a standard output");
    require(contains(none.standard_error, "error: --events needs a standard output to write the events to"),
            "the error on stderr:\n" + none.standard_error);
#endif
}

void stdin_killed(const Options& o)
{
    // The last resort of a program that runs the launcher: its input closes, and at once it is
    // killed (not its process group or job). Its participants end all the same.
    const fs::path dir = two_ecus(o);
    Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"});
    live.wait_for({"\"phase\":\"running\""});
    const std::vector<long> pids{read_pid((dir / "Engine.pid").string(), 60s), read_pid((dir / "Brake.pid").string(), 60s)};
    live.process().close_input();
    kill_one(live.process().id());
    require(live.process().wait_for(scaled_timeout(120s)).has_value(), "the launcher has ended");
    const bool gone = wait_gone(pids[0], 60s) && wait_gone(pids[1], 60s);
    for (const long pid : pids)
    {
        if (process_alive(pid))
        {
            kill_one(pid);
        }
    }
    require(gone, "no participant is left after the launcher was killed");
}

void no_console_stdin(const Options& o)
{
#ifdef _WIN32
    // No console: neither Ctrl+C nor Ctrl+Break reaches the launcher. A stop command does, and
    // the ECUs end by themselves when the simulation stops.
    const fs::path dir = two_ecus(o);
    Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"}, true);
    live.wait_for({"\"phase\":\"running\""});
    live.write("{\"command\":\"stop\"}\n");
    const auto [status, lines] = live.finish();
    const bool gone = all_gone(dir, {"Engine.pid", "Brake.pid"}, 30s);
    expect_stopped_in_order(status, lines, "stop command");
    require(gone, "no participant is left");
#else
    static_cast<void>(o);
    throw Skip("a launcher without a console is a Windows case");
#endif
}
