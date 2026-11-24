// SPDX-License-Identifier: MIT
//
// The launch_run scenarios of run --connect: the launcher starts the bridges itself, before the
// ECUs, and a bridge that ends fails the run. The bridges are the production executables on the
// fake pcap library and the fake XL driver (CMake's connect_tools, with a copy of
// vehicle_sil_launch next to them, so that the launcher finds them there); the device fake0 of the
// fake pcap library answers every frame, and the fake XL driver echoes every frame on channel
// virtual:1 (VEHICLE_SIL_FAKE_XL_ECHO).
//
//   connect_roundtrip           Ethernet and CAN connected, registry_uri at port 0, --speed 1
//                               --strict: no finding; the frames go out and the answers come back
//                               (the counts of the result and of the events "outside", the same as
//                               the bridges' last statistics lines in their logs; of the lines there,
//                               only the last is shown, once, as a "log" event); each bridge has joined before any ECU starts; the bridge's
//                               command has the registry's real port; the result has the bridges last
//                               with for_network, and outside; the launcher stops the bridges at the
//                               end (exit code 0).
//   connect_nothing_received    fake1 (no device) and a CAN channel without the echo node: frames go
//                               out and none comes in, a note connect-nothing-received for each
//                               network, and --strict passes; the counts with --json too (no events),
//                               and of each bridge's statistics lines only its last one shown.
//   connect_no_adapter          an adapter and a channel that are not there: connect-failed with
//                               the bridge's own line, no ECU started, the simulation not started.
//                               start_timeout_ms is long: the launcher saw the bridge end (it did not
//                               wait for the time to pass).
//   connect_lost_kill           a bridge killed while the simulation runs: connect-lost, the
//                               simulation aborted.
//   connect_lost_exit0          the same with a bridge that ends with exit code 0 (asked to stop
//                               from outside): connect-lost as well.
//   connect_dies_while_waiting  the bridge ends while the run waits for a participant started by
//                               hand: connect-failed, the simulation does not start.
//   connect_dies_while_held     the bridge ends while the run stands still before its first step
//                               (--start-paused with --control; skipped without the control
//                               server): connect-failed, the simulation does not start, and not
//                               the finding timeout (the hold saw the bridge end).
//   connect_speed               --speed 2 over the file's speed 1, and --start-paused: the error
//                               outside-real-time, and nothing starts; with --time-policy free the
//                               note outside-not-real-time, and it runs (with --strict too). A
//                               file with another speed: the run goes at speed 1 (outside-speed-set).
//   connect_request             the targets from a request file (run --request): the bridges run as
//                               with --connect; the file's network that the configuration lacks is
//                               a note; --connect <network>=none takes the file's target away; the
//                               file's speed and time_policy are named with the file.
//   connect_time_locked         the time commands of a run that keeps real time: a pause and a
//                               speed written at once (before the configuration is read) and
//                               again while it runs are refused with outside-real-time, in their
//                               order, and so are a step and a run_until of standard input; a speed
//                               of 1 and a resume are taken; the state says the same;
//                               no finding. A file that cannot be read: what was written at once
//                               is refused with not-running.
//   connect_time_changed        with --time-policy free: paused while it runs, then a speed, a
//                               resume and a stop: one note outside-time-changed, about the
//                               pause, and the state's caution; with --strict, a speed command
//                               (written at once) is taken and the run passes with the note.
#include "launch_run_support.hpp"

#include <algorithm>
#include <regex>
#include <tuple>
#include <utility>

namespace {

#ifdef _WIN32
const char* const kLibrarySuffix = ".dll";
#else
const char* const kLibrarySuffix = ".so";
#endif

void set_environment(const std::string& name, const std::string& value)
{
#ifdef _WIN32
    const bool ok = _putenv_s(name.c_str(), value.c_str()) == 0;
#else
    const bool ok = ::setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
    require(ok, "cannot set the environment variable " + name);
}

/// The options with the launcher next to the bridges, and the environment the bridges on the fake
/// drivers need (the launcher passes it on to them).
Options connect_options(const Options& o)
{
#if !defined(_WIN32) && !defined(__linux__)
    throw Skip("the Ethernet bridge has no pcap backend for this operating system");
#endif
    require(!o.connect_tools.empty() && !o.bridge_ecu.empty() && !o.fake_pcap_dir.empty(),
            "the scenarios connect_* need --connect-tools, --bridge-ecu and --fake-pcap-dir");
    Options c = o;
#ifdef _WIN32
    c.launch = o.connect_tools / "vehicle_sil_launch.exe";
#else
    c.launch = o.connect_tools / "vehicle_sil_launch";
#endif
    set_environment("VEHICLE_SIL_TEST_PCAP_LIBRARY", shown_path(o.fake_pcap_dir / ("vehicle_sil_fake_pcap" + std::string(kLibrarySuffix))));
    // virtual:1 is the fake driver's channel 0.
    set_environment("VEHICLE_SIL_FAKE_XL_ECHO", "0");
    return c;
}

/// sim.yaml in its own directory: the Ethernet ECU Pinger on ETH_EXT (it reports its MAC
/// address, so the bridge passes its frames by default) and `more` after it. In real time, with
/// a tolerance that a busy machine does not exceed (--strict would fail on behind-real-time).
fs::path connect_dir(const Options& o, const std::string& name, const std::string& top, const std::string& more,
                     const std::vector<std::pair<std::string, std::string>>& behaviours = {})
{
    const fs::path dir = o.work_dir / name;
    fs::create_directories(dir);
    write_file(dir / "sim.yaml", "vehicle_sil_simulation: 2\n"
                                 "# run-option --speed 1\n"
                                 "# run-option --late-tolerance-ms 60000\n"
                                 "registry_uri: silkit://127.0.0.1:0\n" +
                                     top +
                                     "networks:\n"
                                     "  - name: ETH_EXT\n"
                                     "    kind: ethernet\n"
                                     "  - name: CAN\n"
                                     "    kind: can\n"
                                     "participants:\n"
                                     "  - name: Pinger\n"
                                     "    command: [" + yaml_quote(shown_path(o.bridge_ecu)) + "]\n" + more);
    for (const auto& [participant, behaviour] : behaviours)
    {
        write_file(dir / (participant + ".behaviour"), behaviour + "\n");
    }
    return dir;
}

/// The test ECU Talker, which sends a frame on CAN in every step.
std::string talker(const Options& o) { return "  - name: Talker\n    command: [" + ecu(o) + "]\n"; }

/// The index of the first line with every one of `parts`; lines.size() if none.
std::size_t index_of(const std::vector<std::string>& lines, const std::vector<std::string>& parts)
{
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (std::all_of(parts.begin(), parts.end(), [&](const std::string& p) { return contains(lines[i], p); }))
        {
            return i;
        }
    }
    return lines.size();
}

/// The number after "<key>=" in the last statistics line of a bridge's log file (--log-dir), or -1.
long long stat_of(const fs::path& log, const std::string& key)
{
    long long value = -1;
    for (const std::string& line : lines_of(read_text(log)))
    {
        std::smatch match;
        if (contains(line, "] stats ") && std::regex_search(line, match, std::regex(" " + key + "=([0-9]+)")))
        {
            value = std::stoll(match[1].str());
        }
    }
    return value;
}

/// "result":...,"strict":...,"errors":N,"warnings":N,"notes":N of the run in its result event (not
/// those of its check, which the result has at its end in the same form), or "".
std::string run_counts(const std::string& result)
{
    std::smatch match;
    const std::regex head("^\\{\"event\":\"result\",\"seq\":[0-9]+,\"t_ms\":[0-9]+,\"result\":\\{\"vehicle_sil_run\":2,\"vehicle_sil\":\"[^\"]*\","
                          "\"file\":\"[^\"]*\",(\"result\":\"[a-z_]+\"),\"end\":[a-z_\"]+,\"ok\":(?:true|false),\"message\":\"[^\"]*\","
                          "(\"strict\":(?:true|false),\"errors\":[0-9]+,\"warnings\":[0-9]+,\"notes\":[0-9]+)");
    return std::regex_search(result, match, head) ? match[1].str() + "," + match[2].str() : std::string{};
}

/// The last statistics line in a bridge's log file (--log-dir), or "".
std::string last_statistics_line(const fs::path& log)
{
    std::string last;
    for (const std::string& line : lines_of(read_text(log)))
    {
        if (contains(line, "] stats "))
        {
            last = line;
        }
    }
    return last;
}

/// The counts of `network` in the result's outside[] (-1, -1 if they are null).
std::pair<long long, long long> result_counts(const std::string& result, const std::string& network)
{
    std::smatch match;
    // Compact (events) or written out (--json).
    const std::regex counts("\"network\":\\s*\"" + network +
                            "\",\\s*\"target\":\\s*\"[^\"]*\",\\s*\"participant\":\\s*\"[^\"]*\",\\s*\"to_outside\":\\s*([0-9]+),\\s*"
                            "\"from_outside\":\\s*([0-9]+),\\s*\"counts\":\\s*\\{[^}]*\\}\\s*\\}");
    if (!std::regex_search(result, match, counts))
    {
        return {-1, -1};
    }
    return {std::stoll(match[1].str()), std::stoll(match[2].str())};
}

/// What run says of the counts of the bridge of `network` (bridge.<network>), which wrote `to_key` and
/// `from_key` in its statistics lines: the result has them (`to`, `from`); its "outside" events
/// come after it started, the last with the result's counts; its log file has every statistics
/// line, the last with the same counts; that last line is its one "log" event with statistics, after
/// the last "outside" and before the result.
void expect_counts(const std::vector<std::string>& lines, const fs::path& logs, const std::string& network,
                   const std::string& to_key, const std::string& from_key, long long& to, long long& from)
{
    const std::string bridge = "bridge." + network;
    std::tie(to, from) = result_counts(lines.back(), network);
    require(to >= 0 && from >= 0, network + ": the result has the counts:\n" + lines.back());
    // Every count of the bridge's last statistics line, with its own keys.
    require(contains(lines.back(), "\"counts\":{") && contains(lines.back(), "\"" + to_key + "\":" + std::to_string(to)) &&
                contains(lines.back(), "\"" + from_key + "\":" + std::to_string(from)),
            network + ": the result's counts are those of the bridge's last statistics line:\n" + lines.back());
    const std::vector<std::string> events = events_with(lines, {"\"event\":\"outside\"", "\"network\":\"" + network + "\""});
    require(!events.empty(), network + ": run --events wrote the event outside:\n" + out_of(lines));
    require(index_of(lines, {"\"name\":\"" + bridge + "\"", "\"state\":\"started\""}) <
                index_of(lines, {"\"event\":\"outside\"", "\"network\":\"" + network + "\""}),
            network + ": the event outside after the bridge started:\n" + out_of(lines));
    require(value_of(events.back(), "to_outside") == std::to_string(to) && value_of(events.back(), "from_outside") == std::to_string(from),
            network + ": the last event outside has the result's counts:\n" + events.back() + "\n" + lines.back());
    const fs::path log = logs / (bridge + ".log");
    require(stat_of(log, to_key) == to && stat_of(log, from_key) == from,
            network + ": the bridge's last statistics line in its log has the result's counts:\n" + read_text(log));
    const std::vector<std::string> shown = events_with(lines, {"\"event\":\"log\"", "\"source\":\"" + bridge + "\"", "] stats "});
    require(shown.size() == 1 && value_of(shown[0], "line") == last_statistics_line(log),
            network + ": the bridge's last statistics line is its one log event with statistics:\n" + out_of(lines));
    const auto at = static_cast<std::size_t>(std::find(lines.begin(), lines.end(), shown[0]) - lines.begin());
    require(at > static_cast<std::size_t>(std::find(lines.begin(), lines.end(), events.back()) - lines.begin()) &&
                at + 1 < lines.size(),
            network + ": the statistics line after the last event outside, before the result:\n" + out_of(lines));
}

/// The process ID in the "started" event of `name`.
long started_pid(const std::string& event)
{
    const auto at = event.find("\"pid\":");
    require(at != std::string::npos, "no pid in " + event);
    return std::stol(event.substr(at + 6));
}

/// The finding `code`: its event (a finding of the run), or else its object in the result (a
/// finding of the check is in the result's check); "" if there is none.
std::string finding_in(const std::vector<std::string>& lines, const std::string& code)
{
    const std::string key = "\"code\":\"" + code + "\"";
    for (const std::string& line : events_with(lines, {"\"event\":\"finding\"", key}))
    {
        return line;
    }
    const std::string& result = lines.back();
    const auto at = result.find(key);
    return at == std::string::npos ? std::string{} : result.substr(at, result.find('}', at) - at);
}

void expect_no_ecu_started(const std::vector<std::string>& lines, const std::vector<std::string>& ecus)
{
    for (const std::string& name : ecus)
    {
        require(events_with(lines, {"\"name\":\"" + name + "\"", "\"state\":\"started\""}).empty(),
                name + " was started although a bridge did not run:\n" + out_of(lines));
    }
}

/// connect_lost_kill and connect_lost_exit0: the bridge of ETH_EXT ended from outside once the
/// simulation runs.
void connect_lost(const Options& base, bool hard)
{
    const Options o = connect_options(base);
    const fs::path dir = connect_dir(o, hard ? "lost_kill" : "lost_exit0", "", "");
    const std::string timeout = std::to_string(scaled_timeout(60s).count());
    Live live(o, dir, {"run", "--events", "--timeout-ms", timeout, "--connect", "ETH_EXT=fake0", "sim.yaml"});
    const long pid = started_pid(live.wait_for({"\"name\":\"bridge.ETH_EXT\"", "\"state\":\"started\""}));
    live.wait_for({"\"event\":\"phase\"", "\"phase\":\"running\""});
    end_process(pid, hard);
    const auto [status, lines] = live.finish();
    require(status && status->code == 1, "a run whose bridge ended fails: exit code " + code_text(status) + "\n" + out_of(lines));
    const std::string lost = finding_in(lines, "connect-lost");
    require(contains(lost, "network 'ETH_EXT': the connection to the real bus (fake0) was lost while the simulation was "
                           "running: bridge.ETH_EXT ended (its process ") &&
                contains(lost, hard ? std::string(
#ifdef _WIN32
                                          "exited with code 1"
#else
                                          "was ended by signal SIGKILL"
#endif
                                          )
                                    : std::string("exited with code 0")),
            "the finding connect-lost:\n" + out_of(lines));
    require(contains(lines.back(), "\"simulation\":\"aborted\"") && contains(lines.back(), "\"result\":\"failed\""),
            "the simulation was aborted:\n" + lines.back());
    require(events_with(lines, {"\"name\":\"bridge.ETH_EXT\"", "\"state\":\"failed\"", "\"reason\":\"process_ended\""}).size() == 1,
            "the bridge's failure is an event:\n" + out_of(lines));
    // Nothing else is said of it.
    for (const char* code : {"participant-failed", "exit-code", "simulation"})
    {
        require(finding_in(lines, code).empty(), std::string("no finding ") + code + ":\n" + out_of(lines));
    }
}

} // namespace

void connect_roundtrip(const Options& base)
{
    const Options o = connect_options(base);
    // Long enough for a statistics line of the bridges while the frames go (once a second): on Windows
    // the launcher may have to end them, and then their last one is what run has.
    const fs::path dir = connect_dir(o, "roundtrip", "# run-option --stop-at-ms 2500\n", talker(o), {{"Talker", "talk CAN"}});
    const Captured run = launch(o, {"run", "--events", "--speed", "1", "--strict", "--log-dir", "logs", "--connect", "ETH_EXT=fake0",
                                    "--connect", "CAN=virtual:1", "sim.yaml"},
                                dir);
    const std::vector<std::string> lines = check_events(run.standard_output);
    expect_code(run, 0, "run --connect with both bridges");
    require(run_counts(lines.back()) == "\"result\":\"passed\",\"strict\":true,\"errors\":0,\"warnings\":0,\"notes\":0",
            "no finding:\n" + lines.back());
    // Each bridge joined the simulation before any ECU was started.
    const std::size_t firstEcu = std::min(index_of(lines, {"\"name\":\"Pinger\"", "\"state\":\"started\""}),
                                          index_of(lines, {"\"name\":\"Talker\"", "\"state\":\"started\""}));
    require(firstEcu < lines.size(), "the ECUs were started:\n" + out_of(lines));
    for (const char* bridge : {"bridge.ETH_EXT", "bridge.CAN"})
    {
        const std::string who = "\"name\":\"" + std::string(bridge) + "\"";
        require(index_of(lines, {who, "\"state\":\"joined\""}) < firstEcu,
                std::string(bridge) + " joined before an ECU was started:\n" + out_of(lines));
        // Started with the registry's real port (registry_uri has port 0).
        const std::string started = lines[index_of(lines, {who, "\"state\":\"started\""})];
        const std::regex port("\"--registry-uri\",\"silkit://127\\.0\\.0\\.1:([0-9]+)\",\"--start-timeout-ms\"");
        std::smatch match;
        require(std::regex_search(started, match, port) && match[1].str() != "0",
                std::string(bridge) + " was given the registry's port:\n" + started);
    }
    // The frames went out and the answers came back: the device of fake0 answers each frame, the
    // echo node each CAN frame.
    long long to = 0;
    long long from = 0;
    expect_counts(lines, dir / "logs", "ETH_EXT", "silkit_to_ext", "ext_from_devices", to, from);
    // A line while it ran (the first after 1 s) and its last one, in its log: only the last is shown.
    const std::vector<std::string> logged = lines_of(read_text(dir / "logs" / "bridge.ETH_EXT.log"));
    require(std::count_if(logged.begin(), logged.end(), [](const std::string& line) { return contains(line, "] stats "); }) >= 2,
            "ETH_EXT: the bridge wrote its statistics while it ran:\n" + read_text(dir / "logs" / "bridge.ETH_EXT.log"));
    require(to >= 1 && from >= 1,
            "ETH_EXT: frames out and answers in: " + std::to_string(to) + ", " + std::to_string(from));
    expect_counts(lines, dir / "logs", "CAN", "silkit_to_ext", "ext_from_devices", to, from);
    require(to >= 1 && from >= 1, "CAN: frames out and echoes in: " + std::to_string(to) + ", " + std::to_string(from));
    // The result: the bridges last, with the network each connects, and where.
    const std::string& result = lines.back();
    require(contains(result, "\"name\":\"Talker\",\"required\":true,\"enabled\":true,\"manual\":false,\"for_network\":null,") &&
                contains(result, "\"name\":\"bridge.ETH_EXT\",\"required\":false,\"enabled\":true,\"manual\":false,"
                                 "\"for_network\":\"ETH_EXT\",") &&
                contains(result, "\"name\":\"bridge.CAN\",\"required\":false,\"enabled\":true,\"manual\":false,"
                                 "\"for_network\":\"CAN\",") &&
                result.find("\"name\":\"Talker\"") < result.find("\"name\":\"bridge.ETH_EXT\"") &&
                result.find("\"name\":\"bridge.ETH_EXT\"") < result.find("\"name\":\"bridge.CAN\""),
            "the result has the bridges after the participants of the file:\n" + result);
    require(std::regex_search(result, std::regex("\"outside\":\\[\\{\"network\":\"ETH_EXT\",\"target\":\"fake0\",\"participant\":"
                                                 "\"bridge\\.ETH_EXT\",\"to_outside\":[0-9]+,\"from_outside\":[0-9]+,\"counts\":\\{[^}]*\\}\\},\\{\"network\":"
                                                 "\"CAN\",\"target\":\"virtual:1\",\"participant\":\"bridge\\.CAN\",\"to_outside\":[0-9]+,"
                                                 "\"from_outside\":[0-9]+,\"counts\":\\{[^}]*\\}\\}\\]")),
            "the result has outside:\n" + result);
#ifndef _WIN32
    // On Windows the launcher can ask only where it shares a console with the bridge, and else ends it.
    require(contains(result, "\"for_network\":\"ETH_EXT\",\"ended\":\"stopped\",\"exit_code\":0,") &&
                contains(result, "\"for_network\":\"CAN\",\"ended\":\"stopped\",\"exit_code\":0,"),
            "the launcher stopped the bridges at the end:\n" + result);
#endif
}

void connect_no_adapter(const Options& base)
{
    const Options o = connect_options(base);
    // The launcher waits for the bridges up to start_timeout_ms: long, so that only seeing them end
    // gives the finding in time.
    const fs::path dir = connect_dir(o, "no_adapter", "# run-option --participant-start-timeout-ms 120000\n", talker(o), {{"Talker", "talk CAN"}});
    const Captured run =
        launch(o, {"run", "--events", "--connect", "ETH_EXT=eth9", "--connect", "CAN=physical:9", "sim.yaml"}, dir);
    const std::vector<std::string> lines = check_events(run.standard_output);
    expect_code(run, 1, "run --connect to an adapter that is not there");
    const std::string failed = finding_in(lines, "connect-failed");
    require(contains(failed, "could not be connected to the real bus (") &&
                contains(failed, " ended before the simulation started (its process exited with code ") &&
                contains(failed, "; it wrote: ") && contains(failed, "so nothing else was started"),
            "the finding connect-failed has how the bridge ended and its line:\n" + out_of(lines));
    // The launcher saw a bridge's process end while it waited (not the time limit).
    require(!contains(failed, "within 120000 ms") &&
                !events_with(lines, {"\"event\":\"log\"", " did not start: its process exited with code "}).empty() &&
                events_with(lines, {"\"event\":\"log\"", "not running in SIL Kit within"}).empty(),
            "the launcher saw the bridge end:\n" + out_of(lines));
    expect_no_ecu_started(lines, {"Pinger", "Talker"});
    require(contains(lines.back(), "\"simulation\":\"not_started\""), "the simulation did not start:\n" + lines.back());
    for (const char* code : {"start", "ended-before-start", "not-joined", "exit-code", "participant-failed"})
    {
        require(finding_in(lines, code).empty(), std::string("no finding ") + code + ":\n" + out_of(lines));
    }
}

/// connect_bus_off: the CAN controller goes bus off one second after the bridge started and stays
/// so. The run is short of the 30 s that stop it: it goes on to its end, says the change in a line,
/// and ends with the warning outside-bus-off (the bus-off that is still on counts to the end);
/// --strict would fail on it.
void connect_bus_off(const Options& base)
{
    const Options o = connect_options(base);
    set_environment("VEHICLE_SIL_FAKE_XL_ECHO", "0");
    set_environment("VEHICLE_SIL_FAKE_XL_BUS_OFF_AFTER_MS", "1000");
    const fs::path dir = connect_dir(o, "bus_off", "# run-option --stop-at-ms 4500\n", talker(o), {{"Talker", "talk CAN"}});
    const Captured run = launch(o, {"run", "--events", "--speed", "1", "--log-dir", "logs", "--connect", "CAN=virtual:1", "sim.yaml"}, dir);
    set_environment("VEHICLE_SIL_FAKE_XL_BUS_OFF_AFTER_MS", "");
    const std::vector<std::string> lines = check_events(run.standard_output);
    expect_code(run, 0, "run --connect with a bus that is off");
    require(contains(lines.back(), "\"code\":\"outside-bus-off\"") &&
                contains(lines.back(), "network 'CAN' -> virtual:1: the controller was bus off once, for ") &&
                contains(lines.back(), "\"warnings\":1"),
            "the warning outside-bus-off, in the result (it is made at the end):\n" + lines.back());
    require(finding_in(lines, "connect-lost").empty(), "the run was not stopped for it:\n" + out_of(lines));
    require(index_of(lines, {"\"event\":\"log\"", "outside: CAN -> virtual:1: bus bus-off at "}) < lines.size(),
            "the launcher said that the bus went off:\n" + out_of(lines));
    // The link line is for the launcher: not shown as a line of the bridge.
    require(index_of(lines, {"\"source\":\"bridge.CAN\"", "] link bus="}) == lines.size(), "no link line of the bridge:\n" + out_of(lines));
    require(contains(lines.back(), "\"simulation\":\"reached_stop_at\"") && contains(lines.back(), "\"counts\":{"),
            "the run went to its end, and the result has the counts:\n" + lines.back());
    // The state says it while it lasts: interrupted for bus-off (and ok before, with the bridge there).
    require(index_of(lines, {"\"event\":\"state\"", "\"outside\":[{\"network\":\"CAN\",\"target\":\"virtual:1\",\"participant\":\"bridge.CAN\"",
                             "\"bus\":\"bus-off\"", "\"condition\":\"interrupted\",\"reasons\":[\"bus-off\"]}]"}) < lines.size(),
            "a state event with outside[0].condition interrupted for bus-off:\n" + out_of(lines));
    require(index_of(lines, {"\"event\":\"state\"", "\"network\":\"CAN\"", "\"condition\":\"ok\",\"reasons\":[]}]"}) < lines.size(),
            "a state event with the bridge and condition ok before it:\n" + out_of(lines));
}

/// connect_bus_off_lost: the CAN controller is bus off from the start and stays so (the bridge has
/// no --bitrate, so it does not restart it). The launcher waits 30 s, the real time, and then
/// aborts the run as for a bridge that ended: the error connect-lost with "bus off for 30 s", exit
/// code 1, the simulation aborted, and the ECUs ended.
void connect_bus_off_lost(const Options& base)
{
    const Options o = connect_options(base);
    set_environment("VEHICLE_SIL_FAKE_XL_ECHO", "0");
    set_environment("VEHICLE_SIL_FAKE_XL_BUS_OFF_AFTER_MS", "0");
    const fs::path dir = connect_dir(o, "bus_off_lost", "# run-option --stop-at-ms 300000\n", talker(o), {{"Talker", "talk CAN"}});
    const std::string timeout = std::to_string(scaled_timeout(120s).count());
    const auto began = std::chrono::steady_clock::now();
    const Captured run = launch(o, {"run", "--events", "--speed", "1", "--timeout-ms", timeout, "--connect", "ETH_EXT=fake0",
                          "--connect", "CAN=virtual:1", "sim.yaml"}, dir);
    const auto took = std::chrono::steady_clock::now() - began;
    set_environment("VEHICLE_SIL_FAKE_XL_BUS_OFF_AFTER_MS", "");
    const std::vector<std::string> lines = check_events(run.standard_output);
    expect_code(run, 1, "run --connect whose bus stays off");
    require(took >= 30s && took < 30s + scaled_timeout(30s), "the run was aborted after 30 s, not before and not much later: " +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()) + " ms\n" + out_of(lines));
    const std::string lost = finding_in(lines, "connect-lost");
    require(contains(lost, "network 'CAN': the connection to the real bus (virtual:1) was lost while the simulation was running: "
                           "bridge.CAN reported bus off for 30 s") &&
                contains(lost, "\"severity\":\"error\""),
            "the finding connect-lost:\n" + out_of(lines));
    require(contains(lines.back(), "\"simulation\":\"aborted\"") && contains(lines.back(), "\"result\":\"failed\""),
            "the simulation was aborted:\n" + lines.back());
    for (const char* code : {"participant-failed", "exit-code", "outside-bus-off"})
    {
        require(finding_in(lines, code).empty(), std::string("no finding ") + code + ":\n" + out_of(lines));
    }
    for (const char* ecu : {"Pinger", "Talker"})
    {
        require(events_with(lines, {std::string("\"name\":\"") + ecu + "\"", "\"state\":\"ended\""}).size() == 1,
                std::string(ecu) + " ended:\n" + out_of(lines));
    }
}

void connect_nothing_received(const Options& base)
{
    const Options o = connect_options(base);
    // No echo node on the CAN channel; fake1 has no device behind it.
    set_environment("VEHICLE_SIL_FAKE_XL_ECHO", "");
    const fs::path dir = connect_dir(o, "nothing_received", "# run-option --stop-at-ms 2500\n", talker(o), {{"Talker", "talk CAN"}});
    const std::vector<std::string> args{"--speed", "1", "--strict", "--log-dir", "logs", "--connect", "ETH_EXT=fake1",
                                        "--connect", "CAN=virtual:1", "sim.yaml"};
    std::vector<std::string> withEvents{"run", "--events"};
    withEvents.insert(withEvents.end(), args.begin(), args.end());
    const Captured run = launch(o, withEvents, dir);
    const std::vector<std::string> lines = check_events(run.standard_output);
    expect_code(run, 0, "run --connect --strict with a note");
    require(run_counts(lines.back()) == "\"result\":\"passed\",\"strict\":true,\"errors\":0,\"warnings\":0,\"notes\":2",
            "two notes, and --strict passes:\n" + lines.back());
    long long to = 0;
    long long from = 0;
    expect_counts(lines, dir / "logs", "ETH_EXT", "silkit_to_ext", "ext_from_devices", to, from);
    require(to >= 1 && from == 0, "ETH_EXT: frames out, none in: " + std::to_string(to) + ", " + std::to_string(from));
    require(contains(finding_in(lines, "connect-nothing-received"),
                     "network 'ETH_EXT': nothing came from a device on the real bus (fake1) before the run ended: bridge.ETH_EXT sent " +
                         std::to_string(to) + (to == 1 ? " frame" : " frames") + " there and got none from a device"),
            "the note about ETH_EXT:\n" + out_of(lines));
    expect_counts(lines, dir / "logs", "CAN", "silkit_to_ext", "ext_from_devices", to, from);
    require(to >= 1 && from == 0, "CAN: frames out, none in: " + std::to_string(to) + ", " + std::to_string(from));
    const std::vector<std::string> notes = events_with(lines, {"\"event\":\"finding\"", "\"code\":\"connect-nothing-received\""});
    require(notes.size() == 2 && contains(notes[0] + notes[1], "network 'CAN': nothing came from a device on the real bus (virtual:1) before the run ended") &&
                contains(notes[0] + notes[1], "\"severity\":\"note\""),
            "a note for each network, as events:\n" + out_of(lines));
    // Without events (--json): the counts come from the bridges' lines all the same, and no
    // statistics line but the last of each bridge is shown.
    std::vector<std::string> withJson{"run", "--json"};
    withJson.insert(withJson.end(), args.begin(), args.end());
    const Captured json = launch(o, withJson, dir);
    expect_code(json, 0, "run --connect --json");
    const auto [ethTo, ethFrom] = result_counts(lf(json.standard_output), "ETH_EXT");
    const auto [canTo, canFrom] = result_counts(lf(json.standard_output), "CAN");
    require(ethTo >= 1 && ethFrom == 0 && canTo >= 1 && canFrom == 0 && contains(json.standard_output, "connect-nothing-received"),
            "the result of --json has the counts and the note:\n" + json.standard_output);
    for (const char* network : {"ETH_EXT", "CAN"})
    {
        const std::string bridge = std::string("bridge.") + network;
        const std::vector<std::string> err = lines_of(lf(json.standard_error));
        const auto shown = std::count_if(err.begin(), err.end(), [&](const std::string& line) {
            return line.rfind("[" + bridge + "] ", 0) == 0 && contains(line, "] stats ");
        });
        require(shown == 1 && contains(lf(json.standard_error), "[" + bridge + "] " + last_statistics_line(dir / "logs" / (bridge + ".log")) + "\n"),
                bridge + ": its last statistics line is shown once:\n" + json.standard_error);
    }
}

void connect_lost_kill(const Options& o) { connect_lost(o, true); }

void connect_lost_exit0(const Options& o) { connect_lost(o, false); }

void connect_dies_while_waiting(const Options& base)
{
    const Options o = connect_options(base);
    const fs::path dir = connect_dir(o, "dies_while_waiting", "", "  - name: Late\n    command: [" + ecu(o) + "]\n");
    Live live(o, dir, {"run", "--events", "--manual", "Late", "--connect", "ETH_EXT=fake0", "sim.yaml"});
    const long pid = started_pid(live.wait_for({"\"name\":\"bridge.ETH_EXT\"", "\"state\":\"started\""}));
    live.wait_for({"\"name\":\"bridge.ETH_EXT\"", "\"state\":\"joined\""});
    live.wait_for({"\"event\":\"log\"", "waiting for Late to join"});
    end_process(pid, true);
    const auto [status, lines] = live.finish();
    require(status && status->code == 1, "exit code " + code_text(status) + "\n" + out_of(lines));
    require(contains(finding_in(lines, "connect-failed"), "bridge.ETH_EXT ended before the simulation started") &&
                contains(finding_in(lines, "connect-failed"), "so the simulation was not started"),
            "the finding connect-failed:\n" + out_of(lines));
    require(events_with(lines, {"\"phase\":\"running\""}).empty() && contains(lines.back(), "\"simulation\":\"not_started\""),
            "the simulation did not start:\n" + out_of(lines));
}

void connect_dies_while_held(const Options& base)
{
    const Options o = connect_options(base);
    const fs::path dir = connect_dir(o, "dies_while_held", "", "");
    // The run holds before its first step only with a control server (--start-paused alone pauses
    // at the step at 0, once the simulation has started).
    if (!contains(launch(o, {"version", "--json"}, dir).standard_output, "\"control-server\""))
    {
        throw Skip("this vehicle_sil_launch is built without the control server, and --start-paused holds before the "
                   "first step only with --control (the preset linux-gcc-control builds it)");
    }
    // Long enough not to end the hold before the bridge does; a run that missed the bridge would
    // end with the finding timeout after it, not wait for ever.
    const std::string timeout = std::to_string(scaled_timeout(60s).count());
    Live live(o, dir, {"run", "--events", "--control", "0", "--start-paused", "--time-policy", "free", "--timeout-ms", timeout,
                       "--connect", "ETH_EXT=fake0", "sim.yaml"});
    const long pid = started_pid(live.wait_for({"\"name\":\"bridge.ETH_EXT\"", "\"state\":\"started\""}));
    live.wait_for({"\"event\":\"log\"", "standing still before the first step"});
    end_process(pid, true);
    const auto [status, lines] = live.finish();
    require(status && status->code == 1, "exit code " + code_text(status) + "\n" + out_of(lines));
    require(contains(finding_in(lines, "connect-failed"), "bridge.ETH_EXT ended before the simulation started") &&
                contains(finding_in(lines, "connect-failed"), "so the simulation was not started"),
            "the finding connect-failed:\n" + out_of(lines));
    // The bridge ended the hold, not --timeout-ms.
    require(finding_in(lines, "timeout").empty(), "no finding timeout:\n" + out_of(lines));
    require(events_with(lines, {"\"phase\":\"running\""}).empty() && contains(lines.back(), "\"simulation\":\"not_started\""),
            "the simulation did not start:\n" + out_of(lines));
}

void connect_speed(const Options& base)
{
    const Options o = connect_options(base);
    // Both networks connected, as connect_roundtrip: the check has no other finding, so --strict
    // judges these alone.
    const auto dir_of = [&o](const std::string& name) {
        return connect_dir(o, name, "# run-option --stop-at-ms 300\n", talker(o), {{"Talker", "talk CAN"}});
    };
    const auto run_with = [](std::vector<std::string> args) {
        args.insert(args.begin(), {"run", "--events"});
        args.insert(args.end(), {"--connect", "ETH_EXT=fake0", "--connect", "CAN=virtual:1", "sim.yaml"});
        return args;
    };
    const fs::path dir = dir_of("speed");
    /// The check failed with outside-real-time about `what`, and nothing was started.
    const auto refused = [](const Captured& run, const std::string& what) {
        const std::vector<std::string> lines = check_events(run.standard_output);
        expect_code(run, 1, "run --connect " + what);
        require(contains(lines.back(), "\"severity\":\"error\",\"code\":\"outside-real-time\"") &&
                    contains(lines.back(), what + " is refused") && contains(lines.back(), "\"result\":\"check_failed\"") &&
                    contains(lines.back(), "\"errors\":1,\"warnings\":0,") &&
                    events_with(lines, {"\"event\":\"participant\"", "\"state\":\"started\""}).empty(),
                "the error outside-real-time is about " + what + ", and nothing was started:\n" + out_of(lines));
    };
    // --speed 2 over the file's speed 1: the run keeps real time, so it is an error.
    refused(launch(o, run_with({"--speed", "2"}), dir), "--speed 2");
    refused(launch(o, run_with({"--time-policy", "realtime", "--speed", "max"}), dir), "--speed max");
    // --time-policy free: a note, so it runs with --strict too.
    const Captured free = launch(o, run_with({"--strict", "--time-policy", "free", "--speed", "2"}), dir);
    const std::vector<std::string> freeLines = check_events(free.standard_output);
    expect_code(free, 0, "run --strict --time-policy free --connect --speed 2 (a note)");
    require(contains(finding_in(freeLines, "outside-not-real-time"), "--speed 2 runs the simulation at that speed") &&
                contains(freeLines.back(), "\"severity\":\"note\",\"code\":\"outside-not-real-time\"") &&
                contains(finding_in(freeLines, "outside-not-real-time"), "the bridge still carries frames both ways") &&
                !events_with(freeLines, {"\"event\":\"state\"", "\"speed\":2,"}).empty() &&
                contains(freeLines.back(), "\"simulation\":\"reached_stop_at\""),
            "the note is about --speed, and the run went at speed 2:\n" + out_of(freeLines));
    // No requested speed: the connected run goes at speed 1, and a note says so.
    {
        const fs::path other = dir_of("speed_set");
        std::string text = read_text(other / "sim.yaml");
        require(contains(text, "# run-option --speed 1\n"), "the file of connect_dir() has speed: 1");
        text.erase(text.find("# run-option --speed 1\n"), std::string_view("# run-option --speed 1\n").size());
        write_file(other / "sim.yaml", text);
        const Captured set = launch(o, run_with({"--strict"}), other);
        const std::vector<std::string> setLines = check_events(set.standard_output);
        expect_code(set, 0, "run --strict --connect without a speed (a note)");
        require(contains(finding_in(setLines, "outside-speed-set"), "not as fast as possible") &&
                    !events_with(setLines, {"\"event\":\"state\"", "\"speed\":1,"}).empty() &&
                    events_with(setLines, {"\"event\":\"state\"", "\"speed\":8,"}).empty() &&
                    contains(setLines.back(), "\"timing\":{\"speed\":1,") &&
                    contains(setLines.back(), "\"simulation\":\"reached_stop_at\""),
                "the run went at speed 1, and outside-speed-set says so:\n" + out_of(setLines));
        // With --time-policy free, an explicitly requested speed is accepted.
        const Captured kept = launch(o, run_with({"--strict", "--time-policy", "free", "--speed", "8"}), other);
        const std::vector<std::string> keptLines = check_events(kept.standard_output);
        expect_code(kept, 0, "run --strict --time-policy free --connect of a file with speed: 8");
        require(contains(finding_in(keptLines, "outside-not-real-time"), "--speed 8 runs the simulation at that speed") &&
                    contains(keptLines.back(), "\"timing\":{\"speed\":8,"),
                "the run went at the file's speed:\n" + out_of(keptLines));
        // Nothing connected (none): nothing is locked: the file's speed, and no finding about it.
        const Captured alone =
            launch(o, {"run", "--events", "--speed", "8", "--connect", "ETH_EXT=none", "--connect", "CAN=none", "sim.yaml"}, other);
        const std::vector<std::string> aloneLines = check_events(alone.standard_output);
        expect_code(alone, 0, "run --connect <network>=none of a file with speed: 8");
        require(finding_in(aloneLines, "outside-speed-set").empty() && finding_in(aloneLines, "outside-not-real-time").empty() &&
                    contains(aloneLines.back(), "\"timing\":{\"speed\":8,"),
                "nothing connected, nothing locked:\n" + out_of(aloneLines));
    }
    // The speed 1 of the time lock is the speed the controller's step is planned for: a file
    // without a speed and with a time_step_ms coarser than the ECUs' steps gets coarse-time-step
    // from the check, as a file with speed: 1 does (and not with --time-policy free: no speed).
    {
        const fs::path coarse = connect_dir(o, "speed_step", "# run-option --stop-at-ms 300\ntime_step_ms: 30\n", talker(o), {{"Talker", "talk CAN"}});
        std::string text = read_text(coarse / "sim.yaml");
        text.erase(text.find("# run-option --speed 1\n"), std::string_view("# run-option --speed 1\n").size());
        write_file(coarse / "sim.yaml", text);
        const std::vector<std::string> both{"--connect", "ETH_EXT=fake0", "--connect", "CAN=virtual:1", "sim.yaml"};
        const auto with = [&both](std::vector<std::string> args) {
            args.insert(args.end(), both.begin(), both.end());
            return args;
        };
        const Captured locked = launch(o, with({"check", "--json"}), coarse);
        expect_code(locked, 0, "check --connect of a file with a coarse time_step_ms and no speed");
        require(contains(locked.standard_output, "\"code\": \"coarse-time-step\"") &&
                    contains(locked.standard_output, "\"code\": \"outside-speed-set\"") &&
                    contains(locked.standard_output, "not as fast as possible"),
                "at speed 1 the step is too coarse:\n" + locked.standard_output);
        const Captured unlocked = launch(o, with({"check", "--json", "--time-policy", "free"}), coarse);
        expect_code(unlocked, 0, "the same with --time-policy free");
        require(!contains(unlocked.standard_output, "coarse-time-step") &&
                    contains(unlocked.standard_output, "\"code\": \"outside-not-real-time\""),
                "as fast as possible, the step is not too coarse:\n" + unlocked.standard_output);
        const Captured run = launch(o, with({"run", "--json"}), coarse);
        expect_code(run, 0, "run --connect of a file with a coarse time_step_ms and no speed");
        const std::string out = lf(run.standard_output);
        std::size_t count = 0;
        for (auto at = out.find("\"code\": \"coarse-time-step\""); at != std::string::npos; at = out.find("\"code\": \"coarse-time-step\"", at + 1))
        {
            ++count;
        }
        require(count == 1 && contains(out, "\"timing\": {\n    \"speed\": 1,\n    \"time_step_ms\": 30,"),
                "the run went at speed 1 with the file's step, and coarse-time-step is the check's:\n" + out);
    }
    // --start-paused: an error too.
    refused(launch(o, run_with({"--stdin-commands", "--start-paused"}), dir), "--start-paused");
    // With --time-policy free it is a note, and it runs after a resume.
    Live live(o, dir, run_with({"--strict", "--stdin-commands", "--time-policy", "free", "--start-paused"}));
    live.wait_standing(0);
    live.write("{\"command\":\"resume\"}\n");
    const auto [status, lines] = live.finish();
    require(status && status->code == 0, "exit code " + code_text(status) + "\n" + out_of(lines));
    require(contains(finding_in(lines, "outside-not-real-time"), "--start-paused makes the simulation wait before its first step"),
            "the note outside-not-real-time:\n" + out_of(lines));
    require(contains(lines.back(), "\"simulation\":\"reached_stop_at\""), "it ran to its end:\n" + lines.back());
    // Starting paused and the resume do not change the time of a run that goes on in real time.
    require(finding_in(lines, "outside-time-changed").empty(), "no outside-time-changed:\n" + out_of(lines));
}

void connect_request(const Options& base)
{
    const Options o = connect_options(base);
    const fs::path dir = connect_dir(o, "request", "# run-option --stop-at-ms 300\n", talker(o), {{"Talker", "talk CAN"}});
    const auto bench = [&dir](const std::string& more) {
        write_file(dir / "bench.json", "{ \"vehicle_sil_run_request\": 1,\n"
                                       "  \"connect\": { \"ETH_EXT\": \"fake0\", \"CAN\": \"virtual:1\", \"LIN_BODY\": \"1\" }" +
                                           more + " }\n");
    };
    const std::string unused = "the network LIN_BODY of connect in bench.json is not in this configuration: it is left out";
    // The file's targets: both bridges run; the network that this configuration lacks is a note,
    // which --strict lets pass.
    bench("");
    {
        const Captured run = launch(o, {"run", "--events", "--strict", "--request", "bench.json", "sim.yaml"}, dir);
        const std::vector<std::string> lines = check_events(run.standard_output);
        expect_code(run, 0, "run --request with connect");
        require(run_counts(lines.back()) == "\"result\":\"passed\",\"strict\":true,\"errors\":0,\"warnings\":0,\"notes\":1",
                "one note:\n" + lines.back());
        require(contains(lines.back(), "\"severity\":\"note\",\"code\":\"connect-network-unused\"") && contains(lines.back(), unused),
                "the note about the network of the file:\n" + lines.back());
        for (const char* bridge : {"bridge.ETH_EXT", "bridge.CAN"})
        {
            require(index_of(lines, {"\"name\":\"" + std::string(bridge) + "\"", "\"state\":\"joined\""}) < lines.size(),
                    std::string(bridge) + " joined:\n" + out_of(lines));
        }
        require(contains(lines.back(), "\"outside\":[{\"network\":\"ETH_EXT\",\"target\":\"fake0\",") &&
                    contains(lines.back(), "{\"network\":\"CAN\",\"target\":\"virtual:1\",") &&
                    !contains(lines.back(), "bridge.LIN_BODY"),
                "the result has the two networks outside:\n" + lines.back());
    }
    // --connect <network>=none takes the file's target away: one bridge, and none.
    {
        const Captured run = launch(o, {"run", "--events", "--request", "bench.json", "--connect", "ETH_EXT=none", "sim.yaml"}, dir);
        const std::vector<std::string> lines = check_events(run.standard_output);
        expect_code(run, 0, "run --request --connect ETH_EXT=none");
        require(index_of(lines, {"\"name\":\"bridge.CAN\"", "\"state\":\"joined\""}) < lines.size() &&
                    events_with(lines, {"\"name\":\"bridge.ETH_EXT\""}).empty(),
                "only the CAN bridge ran:\n" + out_of(lines));
        const Captured alone = launch(
            o, {"run", "--events", "--request", "bench.json", "--connect", "ETH_EXT=none", "--connect", "CAN=none", "sim.yaml"}, dir);
        const std::vector<std::string> aloneLines = check_events(alone.standard_output);
        expect_code(alone, 0, "run --request --connect <network>=none for both");
        require(events_with(aloneLines, {"\"name\":\"bridge."}).empty() && contains(aloneLines.back(), unused),
                "no bridge ran:\n" + out_of(aloneLines));
    }
    // The file's speed is refused as --speed is, and named with the file; --speed 1 wins over it.
    {
        std::string text = read_text(dir / "sim.yaml");
        const std::string speed = "# run-option --speed 1\n";
        text.erase(text.find(speed), speed.size());
        write_file(dir / "sim.yaml", text);
    }
    bench(", \"speed\": 2");
    {
        const Captured run = launch(o, {"run", "--events", "--request", "bench.json", "sim.yaml"}, dir);
        const std::vector<std::string> lines = check_events(run.standard_output);
        expect_code(run, 1, "run --request with speed 2 and connect");
        require(contains(lines.back(), "\"severity\":\"error\",\"code\":\"outside-real-time\"") &&
                    contains(lines.back(), "speed 2 in bench.json is refused. Take speed out of bench.json, or pass --speed 1") &&
                    contains(lines.back(), "\"result\":\"check_failed\"") &&
                    events_with(lines, {"\"event\":\"participant\"", "\"state\":\"started\""}).empty(),
                "the file's speed is refused, and nothing was started:\n" + out_of(lines));
        const Captured one = launch(o, {"run", "--events", "--request", "bench.json", "--speed", "1", "sim.yaml"}, dir);
        expect_code(one, 0, "run --request with speed 2, --speed 1");
    }
    // The file's time_policy free: the note names the file; --time-policy realtime wins over it.
    bench(", \"speed\": 2, \"time_policy\": \"free\"");
    {
        const Captured run = launch(o, {"run", "--events", "--strict", "--request", "bench.json", "sim.yaml"}, dir);
        const std::vector<std::string> lines = check_events(run.standard_output);
        expect_code(run, 0, "run --request with time_policy free");
        require(contains(finding_in(lines, "outside-not-real-time"),
                         "with time_policy free in bench.json speed 2 in bench.json runs the simulation at that speed") &&
                    contains(finding_in(lines, "outside-not-real-time"),
                             "leave time_policy free in bench.json and speed in bench.json out") &&
                    contains(lines.back(), "\"timing\":{\"speed\":2,"),
                "the note names the file:\n" + out_of(lines));
        const Captured locked = launch(o, {"run", "--events", "--time-policy", "realtime", "--request", "bench.json", "sim.yaml"}, dir);
        const std::vector<std::string> lockedLines = check_events(locked.standard_output);
        expect_code(locked, 1, "run --request with time_policy free, --time-policy realtime");
        require(contains(lockedLines.back(), "speed 2 in bench.json is refused"),
                "--time-policy realtime wins over the file:\n" + out_of(lockedLines));
    }
}

void connect_time_locked(const Options& base)
{
    const Options o = connect_options(base);
    const std::vector<std::string> run{"run", "--events", "--stdin-commands", "--connect", "ETH_EXT=fake0", "--connect",
                                       "CAN=virtual:1", "sim.yaml"};
    /// The command events of standard input, in their order, each as "<command> <id> <accepted or the reason>".
    const auto answers = [](const std::vector<std::string>& lines) {
        std::vector<std::string> out;
        for (const std::string& line : events_with(lines, {"\"event\":\"command\"", "\"source\":\"stdin\""}))
        {
            out.push_back(value_of(line, "command") + " " + value_of(line, "id") + " " +
                          (contains(line, "\"accepted\":true") ? "accepted" : value_of(line, "reason")));
        }
        return out;
    };
    {
        // No stop_at_ms: the commands do not have to come within a time of the run; a stop ends it.
        const fs::path dir = connect_dir(o, "time_locked", "", talker(o), {{"Talker", "talk CAN"}});
        Live live(o, dir, run);
        // At once: most likely before the launcher has read the configuration, and whenever they
        // come, the answer is the lock's. In the order they were written.
        live.write("{\"command\":\"pause\",\"id\":1}\n{\"command\":\"speed\",\"value\":5,\"id\":2}\n"
                   "{\"command\":\"speed\",\"value\":1,\"id\":3}\n{\"command\":\"resume\",\"id\":4}\n");
        live.wait_for({"\"event\":\"phase\"", "\"phase\":\"running\""});
        // While it runs: the same answers.
        live.write("{\"command\":\"pause\",\"id\":5}\n{\"command\":\"speed\",\"value\":\"max\",\"id\":6}\n"
                   "{\"command\":\"step\",\"ms\":10,\"id\":7}\n{\"command\":\"run_until\",\"at_ms\":60000,\"id\":8}\n"
                   "{\"command\":\"speed\",\"value\":1,\"id\":9}\n");
        live.wait_for({"\"event\":\"command\"", "\"id\":9"});
        live.write("{\"command\":\"stop\"}\n");
        const auto [status, lines] = live.finish();
        require(status && status->code == 0, "stopped by a command: 0, not exit code " + code_text(status) + "\n" + out_of(lines));
        const std::vector<std::string> expected{"pause 1 outside-real-time", "speed 2 outside-real-time", "speed 3 accepted",
                                                "resume 4 accepted",         "pause 5 outside-real-time", "speed 6 outside-real-time",
                                                "step 7 outside-real-time",  "run_until 8 outside-real-time",
                                                "speed 9 accepted",          "stop null accepted"};
        require(answers(lines) == expected, "each command has its answer, in the order they were written:\n" + out_of(lines));
        const std::string pause = events_with(lines, {"\"event\":\"command\"", "\"id\":1}"}).front();
        require(contains(pause, "\"accepted\":false,") && contains(pause, "\"reason\":\"outside-real-time\"") &&
                    contains(pause, "pause is refused, because what is connected does not wait"),
                "the refusal says why:\n" + pause);
        // Nothing of it reached the simulation: it never stood still and went at speed 1.
        require(events_with(lines, {"\"event\":\"state\"", "\"paused\":true"}).empty() &&
                    events_with(lines, {"\"event\":\"state\"", "\"speed\":5,"}).empty() &&
                    events_with(lines, {"\"event\":\"state\"", "\"speed\":\"max\","}).empty() &&
                    contains(lines.back(), "\"timing\":{\"speed\":1,"),
                "the simulation went on at speed 1:\n" + out_of(lines));
        // The state says what the commands were told: from the first one after the configuration.
        const std::vector<std::string> states = events_with(lines, {"\"event\":\"state\"", "\"phase\":\"running\""});
        require(!states.empty(), "a state while it runs:\n" + out_of(lines));
        const std::string& state = states.front();
        require(contains(state, "\"pause\":{\"ok\":false,\"reason\":\"outside-real-time\"") &&
                    contains(state, "\"step\":{\"ok\":false,\"reason\":\"outside-real-time\"") &&
                    contains(state, "\"run_until\":{\"ok\":false,\"reason\":\"outside-real-time\"") &&
                    contains(state, "\"resume\":{\"ok\":true,") && contains(state, "\"stop\":{\"ok\":true,") &&
                    contains(state, "\"speed\":{\"ok\":true,\"reason\":null,\"message\":null,\"caution\":null,\"min\":1,\"max\":1}"),
                "the state refuses a pause, a step and a run_until, and takes the speed 1 only:\n" + state);
        for (const std::string& line : events_with(lines, {"\"event\":\"state\""}))
        {
            // The first state of a run comes with its first phase, before the configuration is
            // read; every one after the check has the lock.
            require(contains(line, "\"phase\":\"checking\"") || contains(line, "\"pause\":{\"ok\":false,"),
                    "no state after the check takes a pause:\n" + line);
        }
        require(finding_in(lines, "outside-time-changed").empty() && contains(lines.back(), "\"errors\":0,\"warnings\":0,"),
                "nothing changed the time, so no finding says so:\n" + lines.back());
    }
    {
        // Without --events: the answers of the commands that were refused are lines on stderr,
        // those written at once too.
        const fs::path dir = connect_dir(o, "time_locked_text", "# run-option --stop-at-ms 300\n", talker(o), {{"Talker", "talk CAN"}});
        Live live(o, dir, {"run", "--stdin-commands", "--connect", "ETH_EXT=fake0", "--connect", "CAN=virtual:1", "sim.yaml"});
        live.write("{\"command\":\"pause\"}\n{\"command\":\"resume\"}\n");
        const auto [status, lines] = live.finish_unchecked();
        require(status && status->code == 0, "exit code " + code_text(status) + "\n" + out_of(lines));
        const std::string err = lf(live.err());
        const std::string line = "vehicle_sil_launch: a line of the standard input was ignored: this run is connected outside "
                                 "the simulation and keeps to real time: pause is refused";
        require(contains(err, line) && err.find(line) == err.rfind(line), "one line on stderr about the pause:\n" + err);
    }
    {
        // A file that cannot be read: nothing starts, and the commands that waited for the
        // configuration are told so, in their order, before the result.
        const fs::path dir = connect_dir(o, "time_locked_no_file", "", talker(o), {{"Talker", "talk CAN"}});
        std::vector<std::string> args = run;
        args.back() = "no-such.yaml";
        Live live(o, dir, args);
        live.write("{\"command\":\"pause\",\"id\":1}\n{\"command\":\"speed\",\"value\":5,\"id\":2}\n");
        const auto [status, lines] = live.finish();
        require(status && status->code == 3, "a file that cannot be read: " + code_text(status) + "\n" + out_of(lines));
        const std::vector<std::string> got = answers(lines);
        // The launcher may have ended before it read a line (then there is none): what it read
        // is refused, never accepted, and in order.
        require(got.empty() || got == std::vector<std::string>{"pause 1 not-running"} ||
                    got == std::vector<std::string>{"pause 1 not-running", "speed 2 not-running"},
                "the commands of a run that does not start are refused:\n" + out_of(lines));
        require(contains(lines.back(), "\"result\":\"unreadable\""), "the file could not be read:\n" + lines.back());
    }
}

void connect_time_changed(const Options& base)
{
    const Options o = connect_options(base);
    const std::string code = "\"code\":\"outside-time-changed\"";
    // Both networks connected, as connect_roundtrip: the check has no finding of its own.
    // No stop_at_ms: the commands do not have to come within a time of the run; a stop ends it.
    {
        const fs::path dir = connect_dir(o, "time_changed", "", talker(o), {{"Talker", "talk CAN"}});
        Live live(o, dir, {"run", "--events", "--stdin-commands", "--time-policy", "free", "--connect", "ETH_EXT=fake0",
                           "--connect", "CAN=virtual:1", "sim.yaml"});
        live.wait_for({"\"event\":\"phase\"", "\"phase\":\"running\""});
        live.write("{\"command\":\"pause\"}\n");
        live.wait_paused();
        live.write("{\"command\":\"speed\",\"value\":2}\n{\"command\":\"resume\"}\n");
        live.wait_for({"\"event\":\"state\"", "\"paused\":false,", "\"speed\":2,"});
        // A time after it went on: not before where it stood still.
        const std::string paused = live.wait_for_after({"\"command\":\"resume\"", "\"accepted\":true,"}, {"\"event\":\"time\""});
        live.write("{\"command\":\"stop\"}\n");
        const auto [status, lines] = live.finish();
        require(status && status->code == 0, "stopped by a command: 0, not exit code " + code_text(status) + "\n" + out_of(lines));
        require(events_with(lines, {"\"event\":\"command\"", "\"accepted\":false"}).empty(),
                "every command was accepted:\n" + out_of(lines));
        // One finding, about the first command, at the time it came (not after where it stood still).
        const std::vector<std::string> found = events_with(lines, {"\"event\":\"finding\"", code});
        require(found.size() == 1 && contains(found[0], "\"severity\":\"note\""), "one note outside-time-changed:\n" + out_of(lines));
        std::smatch match;
        const bool named = std::regex_search(found[0], match,
                                             std::regex("with --time-policy free the run took the time command 'pause' from "
                                                        "stdin at ([0-9]+) ms: the simulation stood still while what is "
                                                        "connected there went on in real time"));
        require(named && std::stoll(match[1].str()) <= std::stoll(value_of(paused, "sim_ms")),
                "the finding names the pause and when it came:\n" + found[0] + "\n" + paused);
        require(contains(lines.back(), "\"errors\":0,\"warnings\":0,"), "notes only:\n" + lines.back());
        // The state takes the commands, and says what there is to know about them.
        const std::vector<std::string> states = events_with(lines, {"\"event\":\"state\"", "\"phase\":\"running\""});
        require(!states.empty() &&
                    contains(states.front(), "\"pause\":{\"ok\":true,\"reason\":null,\"message\":null,\"caution\":{\"code\":"
                                             "\"outside-not-real-time\",\"message\":\"") &&
                    contains(states.front(), "\"resume\":{\"ok\":true,\"reason\":null,\"message\":null,\"caution\":null") &&
                    contains(states.front(), "\"stop\":{\"ok\":true,\"reason\":null,\"message\":null,\"caution\":null"),
                "the state's caution:\n" + (states.empty() ? out_of(lines) : states.front()));
    }
    // With --strict the note does not fail the run. The speed is written at once: a command before
    // the simulation starts waits for the configuration, is taken and kept, and counts as well.
    const fs::path dir = connect_dir(o, "time_changed_strict", "# run-option --stop-at-ms 300\n", talker(o), {{"Talker", "talk CAN"}});
    Live live(o, dir, {"run", "--events", "--strict", "--stdin-commands", "--time-policy", "free", "--connect", "ETH_EXT=fake0",
                       "--connect", "CAN=virtual:1", "sim.yaml"});
    live.write("{\"command\":\"speed\",\"value\":\"max\"}\n");
    const auto [status, lines] = live.finish();
    require(status && status->code == 0, "--strict: exit code " + code_text(status) + "\n" + out_of(lines));
    require(contains(finding_in(lines, "outside-time-changed"), "the time command 'speed max' from stdin at ") &&
                contains(lines.back(), "\"simulation\":\"reached_stop_at\"") && contains(lines.back(), "\"result\":\"passed\""),
            "the run went to its end and passed with the note:\n" + out_of(lines));
}

namespace {

/// Sets an environment variable for as long as it lives, then clears it: a scenario that fails
/// leaves the next one the environment it had.
class ScopedEnvironment
{
public:
    ScopedEnvironment(std::string name, const std::string& value) : name_(std::move(name)) { set_environment(name_, value); }
    ~ScopedEnvironment()
    {
        try
        {
            set_environment(name_, "");
        }
        catch (...)
        {
        }
    }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    std::string name_;
};

/// A bridge on the channel virtual:1 outside the run, in a registry of its own: the vehicle-sil
/// bridge that another run (or a person) left running. It is ended when this goes, whether the
/// scenario passed or failed, so that no process stays to be found by another test.
class OutsideBridge
{
public:
    OutsideBridge(const Options& o, const std::string& registry_uri)
    {
        vehicle_sil::process::ProcessOptions options;
        options.on_line = [this](vehicle_sil::process::Stream, std::string_view line) {
            const std::lock_guard lock(mutex_);
            lines_ += std::string(line) + "\n";
        };
#ifdef _WIN32
        const fs::path bridge = o.connect_tools / "vehicle_sil_vector_xl_bridge.exe";
#else
        const fs::path bridge = o.connect_tools / "vehicle_sil_vector_xl_bridge";
#endif
        process_ = std::make_unique<vehicle_sil::process::Process>(
            bridge,
            std::vector<std::string>{"--registry-uri", registry_uri, "--participant", "OutsideBridge", "--network", "OUTSIDE_CAN",
                                     "--xl-target", "virtual", "--xl-channel", "1"},
            std::move(options));
    }
    ~OutsideBridge()
    {
        process_->kill_tree();
        static_cast<void>(process_->wait_for(10s));
        process_->finish_output(10s);
    }
    OutsideBridge(const OutsideBridge&) = delete;
    OutsideBridge& operator=(const OutsideBridge&) = delete;

    /// Waits until it has said it runs (its link line, with the device it shares, is before that).
    void wait_ready()
    {
        const auto deadline = std::chrono::steady_clock::now() + scaled_timeout(60s);
        while (!contains(lines(), "Ctrl+C to stop"))
        {
            require(!process_->poll().has_value() && std::chrono::steady_clock::now() < deadline,
                    "the bridge outside the run did not start:\n" + lines());
            std::this_thread::sleep_for(10ms);
        }
    }
    long id() const { return process_->id(); }
    std::string lines()
    {
        const std::lock_guard lock(mutex_);
        return lines_;
    }
    bool running() { return !process_->poll().has_value(); }

private:
    std::mutex mutex_;
    std::string lines_;
    std::unique_ptr<vehicle_sil::process::Process> process_;
};

/// What the bridges of this process say of the pid of a bridge outside the run (Linux), else nothing.
std::string who_text(long pid)
{
#ifdef __linux__
    return " (process " + std::to_string(pid) + ")";
#else
    static_cast<void>(pid);
    return {};
#endif
}

/// The scope that makes the fake devices of the bridges of this scenario the same device (one per
/// process otherwise, so that tests that run at the same time do not find each other).
std::string device_scope(const std::string& name)
{
    return "launch-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

/// Whether a bridge of this run wrote `part` as a line (a "log" event of `network`'s bridge).
bool bridge_wrote(const std::vector<std::string>& lines, const std::string& network, const std::string& part)
{
    return index_of(lines, {"\"source\":\"bridge." + network + "\"", part}) < lines.size();
}

} // namespace

/// connect_device_shared: another vehicle-sil bridge, which is not of the run, is on virtual:1
/// before the run starts. Without --strict the run goes to its end and tells it as the warning
/// outside-device-shared with that bridge's process id (the bridge's own warning is not shown, but
/// its log has it); with --strict it is an error and the run does not start the ECUs.
void connect_device_shared(const Options& base)
{
    const Options o = connect_options(base);
    require(!o.registry.empty(), "the scenarios connect_device_shared* need --registry");
    set_environment("VEHICLE_SIL_FAKE_XL_ECHO", "0");
    const ScopedEnvironment scope("VEHICLE_SIL_FAKE_DEVICE_SCOPE", device_scope("device-shared"));
    fs::create_directories(o.work_dir / "registry");
    vehicle_sil::test::RegistryProcess registry(o.registry, o.work_dir / "registry");
    OutsideBridge outside(o, registry.uri());
    outside.wait_ready();
    {
        const fs::path dir = connect_dir(o, "device_shared", "# run-option --stop-at-ms 2500\n", talker(o), {{"Talker", "talk CAN"}});
        const Captured run = launch(o, {"run", "--events", "--speed", "1", "--log-dir", "logs", "--connect", "CAN=virtual:1", "sim.yaml"}, dir);
        const std::vector<std::string> lines = check_events(run.standard_output);
        expect_code(run, 0, "run --connect on a device that a bridge outside the run uses (a warning)");
        const std::string found = finding_in(lines, "outside-device-shared");
        require(contains(found, "\"severity\":\"warning\"") &&
                    contains(found, "network 'CAN' -> virtual:1: the device was also used by another vehicle-sil bridge" + who_text(outside.id()) +
                                        " that is not part of this run from ") &&
                    contains(found, "\"warnings\":") == false,
                "the warning outside-device-shared, with the pid of the bridge outside:\n" + out_of(lines));
        require(index_of(lines, {"\"event\":\"log\"", "outside: CAN -> virtual:1: also used by another vehicle-sil bridge" + who_text(outside.id()) +
                                                     " at "}) < lines.size(),
                "the launcher said it when it found it:\n" + out_of(lines));
        // It is said once, and before the ECUs start (the first link line of the bridge tells it), not
        // only when the run is already going.
        const std::vector<std::string> said = events_with(lines, {"\"event\":\"log\"", "outside: CAN -> virtual:1: also used by another vehicle-sil bridge"});
        require(said.size() == 1, "the launcher said it once:\n" + out_of(lines));
        require(index_of(lines, {"\"event\":\"log\"", "outside: CAN -> virtual:1: also used by another vehicle-sil bridge"}) <
                    index_of(lines, {"\"name\":\"Talker\"", "\"state\":\"started\""}),
                "the launcher said it before the ECUs started:\n" + out_of(lines));
        // The bridge's own warning and its link line are for the launcher: not shown; the log keeps them.
        require(!bridge_wrote(lines, "CAN", "is also used by") && !bridge_wrote(lines, "CAN", "] link "),
                "the bridge's warning about the device is not shown:\n" + out_of(lines));
        require(contains(read_text(dir / "logs" / "bridge.CAN.log"), " shared=1"), "the bridge's log has its link line with shared=1");
        require(finding_in(lines, "connect-target-shared").empty(), "nothing is said of the networks of this run:\n" + out_of(lines));
        require(contains(lines.back(), "\"simulation\":\"reached_stop_at\"") && contains(lines.back(), "\"result\":\"passed\""),
                "the run went to its end:\n" + lines.back());
        require(outside.running(), "the bridge outside the run was not touched");
    }
    // A manual participant that has not come: the run waits for it, and has said it by then (from
    // the bridge's first link line, not only when the run is going), once.
    {
        const fs::path dir = connect_dir(o, "device_shared_manual", "# run-option --stop-at-ms 2500\n", talker(o) + "  - name: Late\n    command: [" + ecu(o) + "]\n",
                                         {{"Talker", "talk CAN"}});
        Live live(o, dir, {"run", "--events", "--manual", "Late", "--speed", "1", "--connect", "CAN=virtual:1", "sim.yaml"});
        live.wait_for({"\"event\":\"log\"", "outside: CAN -> virtual:1: also used by another vehicle-sil bridge" + who_text(outside.id())});
        const long pid = started_pid(live.wait_for({"\"name\":\"bridge.CAN\"", "\"state\":\"started\""}));
        end_process(pid, true);
        const auto [status, lines] = live.finish();
        require(status && status->code == 1, "exit code " + code_text(status) + "\n" + out_of(lines));
        require(events_with(lines, {"\"event\":\"log\"", "outside: CAN -> virtual:1: also used by another vehicle-sil bridge"}).size() == 1,
                "the launcher said it once:\n" + out_of(lines));
    }
    // --strict: an error, and the ECUs are not started.
    const fs::path dir = connect_dir(o, "device_shared_strict", "# run-option --stop-at-ms 2500\n", talker(o), {{"Talker", "talk CAN"}});
    const Captured run =
        launch(o, {"run", "--events", "--strict", "--speed", "1", "--connect", "ETH_EXT=fake0", "--connect", "CAN=virtual:1", "sim.yaml"}, dir);
    const std::vector<std::string> lines = check_events(run.standard_output);
    expect_code(run, 1, "run --strict --connect on a device that a bridge outside the run uses");
    const std::string found = finding_in(lines, "outside-device-shared");
    require(contains(found, "\"severity\":\"error\"") &&
                contains(found, "the device is also used by another vehicle-sil bridge" + who_text(outside.id()) +
                                    " that is not part of this run, and --strict does not start a run on it: "),
            "the error outside-device-shared:\n" + out_of(lines));
    require(contains(lines.back(), "\"simulation\":\"not_started\"") && contains(lines.back(), "\"result\":\"failed\""),
            "the simulation did not start:\n" + lines.back());
    for (const char* ecu : {"Pinger", "Talker"})
    {
        require(events_with(lines, {std::string("\"name\":\"") + ecu + "\"", "\"state\":\"started\""}).empty() &&
                    events_with(lines, {std::string("\"name\":\"") + ecu + "\"", "\"state\":\"ended\""}).empty(),
                std::string(ecu) + " was not started:\n" + out_of(lines));
    }
    for (const char* code : {"connect-failed", "participant-failed", "connect-lost"})
    {
        require(finding_in(lines, code).empty(), std::string("no finding ") + code + ":\n" + out_of(lines));
    }
    require(outside.running(), "the bridge outside the run was not touched");
}

/// connect_device_shared_later: a bridge outside the run comes to virtual:1 while a --strict run
/// goes on. The launcher says it, waits 3 s, the real time (a bridge that another run started and
/// that ends before its ECUs start must not take this run down), and then aborts the run: the
/// error outside-device-shared, exit code 1, the simulation aborted. A bridge that is gone again
/// within the 3 s does not stop the run: the warning, and --strict fails on it at the end.
void connect_device_shared_later(const Options& base)
{
    const Options o = connect_options(base);
    require(!o.registry.empty(), "the scenarios connect_device_shared* need --registry");
    set_environment("VEHICLE_SIL_FAKE_XL_ECHO", "0");
    const ScopedEnvironment scope("VEHICLE_SIL_FAKE_DEVICE_SCOPE", device_scope("device-shared-later"));
    fs::create_directories(o.work_dir / "registry");
    vehicle_sil::test::RegistryProcess registry(o.registry, o.work_dir / "registry");
    const std::string timeout = std::to_string(scaled_timeout(120s).count());
    {
        const fs::path dir = connect_dir(o, "device_shared_later", "# run-option --stop-at-ms 300000\n", talker(o), {{"Talker", "talk CAN"}});
        Live live(o, dir, {"run", "--events", "--strict", "--speed", "1", "--timeout-ms", timeout, "--connect", "ETH_EXT=fake0",
                          "--connect", "CAN=virtual:1", "sim.yaml"});
        live.wait_for({"\"event\":\"phase\"", "\"phase\":\"running\""});
        OutsideBridge outside(o, registry.uri());
        outside.wait_ready();
        live.wait_for({"\"event\":\"log\"", "outside: CAN -> virtual:1: also used by another vehicle-sil bridge" + who_text(outside.id()) + " at "});
        const auto found_at = std::chrono::steady_clock::now();
        const auto [status, lines] = live.finish();
        const auto took = std::chrono::steady_clock::now() - found_at;
        require(status && status->code == 1, "exit code " + code_text(status) + "\n" + out_of(lines));
        // The 3 s count from when the launcher found it, which the line says: not before, and not much later.
        require(took >= 2900ms && took < 3s + scaled_timeout(30s),
                "the run was aborted 3 s after the launcher found the bridge: " +
                    std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()) + " ms\n" + out_of(lines));
        const std::string found = finding_in(lines, "outside-device-shared");
        require(contains(found, "\"severity\":\"error\"") && contains(found, "network 'CAN' -> virtual:1: the device was also used by another "
                                                                              "vehicle-sil bridge" + who_text(outside.id()) +
                                                                              " that is not part of this run from ") &&
                    contains(found, ", and --strict stopped the run after 3 s of it: "),
                "the error outside-device-shared:\n" + out_of(lines));
        require(contains(lines.back(), "\"simulation\":\"aborted\"") && contains(lines.back(), "\"result\":\"failed\""),
                "the simulation was aborted:\n" + lines.back());
        for (const char* code : {"participant-failed", "connect-lost", "exit-code"})
        {
            require(finding_in(lines, code).empty(), std::string("no finding ") + code + ":\n" + out_of(lines));
        }
        for (const char* ecu : {"Pinger", "Talker"})
        {
            require(events_with(lines, {std::string("\"name\":\"") + ecu + "\"", "\"state\":\"ended\""}).size() == 1,
                    std::string(ecu) + " ended:\n" + out_of(lines));
        }
    }
    // Gone again within 3 s: the run is not stopped.
    const fs::path dir = connect_dir(o, "device_shared_gone", "# run-option --stop-at-ms 4000\n", talker(o), {{"Talker", "talk CAN"}});
    Live live(o, dir, {"run", "--events", "--strict", "--speed", "1", "--timeout-ms", timeout, "--connect", "ETH_EXT=fake0",
                          "--connect", "CAN=virtual:1", "sim.yaml"});
    live.wait_for({"\"event\":\"phase\"", "\"phase\":\"running\""});
    {
        OutsideBridge outside(o, registry.uri());
        outside.wait_ready();
        live.wait_for({"\"event\":\"log\"", "outside: CAN -> virtual:1: also used by another vehicle-sil bridge"});
        // The launcher has recorded the sharing (the line is written when it does): the bridge goes now.
    }
    const auto [status, lines] = live.finish();
    require(status && status->code == 1, "--strict fails on the warning: exit code " + code_text(status) + "\n" + out_of(lines));
    require(contains(finding_in(lines, "outside-device-shared"), "\"severity\":\"warning\""),
            "the warning outside-device-shared:\n" + out_of(lines));
    require(contains(lines.back(), "\"simulation\":\"reached_stop_at\"") && contains(lines.back(), "\"result\":\"failed\"") &&
                contains(lines.back(), "\"errors\":0,"),
            "the run went to its end:\n" + lines.back());
}

/// connect_device_shared_own: two networks of the run on Vector Virtual CAN, which is one bus, and
/// so one device: the check says it (connect-target-shared) and the run goes on; the launcher
/// takes the other bridge of the run out of what each bridge reports, so there is no
/// outside-device-shared, and no line of the bridges' warnings.
void connect_device_shared_own(const Options& base)
{
    const Options o = connect_options(base);
    set_environment("VEHICLE_SIL_FAKE_XL_ECHO", "0");
    const ScopedEnvironment scope("VEHICLE_SIL_FAKE_DEVICE_SCOPE", device_scope("device-shared-own"));
    const fs::path dir = o.work_dir / "device_shared_own";
    fs::create_directories(dir);
    write_file(dir / "sim.yaml", "vehicle_sil_simulation: 2\n"
                                 "# run-option --speed 1\n"
                                 "# run-option --late-tolerance-ms 60000\n"
                                 "registry_uri: silkit://127.0.0.1:0\n"
                                 "# run-option --stop-at-ms 2500\n"
                                 "networks:\n"
                                 "  - name: CAN\n"
                                 "    kind: can\n"
                                 "  - name: CAN2\n"
                                 "    kind: can\n"
                                 "participants:\n"
                                 "  - name: Talker\n"
                                 "    command: [" + ecu(o) + "]\n");
    write_file(dir / "Talker.behaviour", "talk CAN\n");
    const Captured run =
        launch(o, {"run", "--events", "--speed", "1", "--connect", "CAN=virtual:1", "--connect", "CAN2=virtual:2", "sim.yaml"}, dir);
    const std::vector<std::string> lines = check_events(run.standard_output);
    expect_code(run, 0, "run --connect with two networks on one device (a warning)");
    require(finding_in(lines, "outside-device-shared").empty(), "no outside-device-shared:\n" + out_of(lines));
    // The check says it (in its event, and in the result's copy of it), the run does not say it again.
    const std::string& result = lines.back();
    const auto begin = result.find("\"findings\":[");
    const auto end = result.find("\"check\":{", begin);
    require(index_of(lines, {"\"event\":\"check\"", "\"code\":\"connect-target-shared\""}) < lines.size() && begin != std::string::npos &&
                end != std::string::npos && !contains(result.substr(begin, end - begin), "connect-target-shared"),
            "the check's warning connect-target-shared, once:\n" + out_of(lines));
    require(!bridge_wrote(lines, "CAN", "is also used by") && !bridge_wrote(lines, "CAN2", "is also used by"),
            "the bridges' warnings about the device are not shown:\n" + out_of(lines));
    require(index_of(lines, {"\"event\":\"log\"", "also used by another vehicle-sil bridge"}) == lines.size(),
            "the launcher says nothing of it:\n" + out_of(lines));
    require(contains(lines.back(), "\"simulation\":\"reached_stop_at\""), "the run went to its end:\n" + lines.back());
}
