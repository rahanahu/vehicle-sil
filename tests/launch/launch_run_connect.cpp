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
//                               (the bridges' statistics); each bridge has joined before any ECU
//                               starts; the bridge's command has the registry's real port; the
//                               result has the bridges last with for_network, and outside; the
//                               launcher stops the bridges at the end (exit code 0).
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
//   connect_speed               --speed 2 over the file's speed 1: connect-not-real-time; with
//                               --strict nothing starts; --start-paused: connect-start-paused, and
//                               it runs after a resume (which is no connect-time-changed).
//   connect_time_changed        paused while it runs, then a speed, a resume and a stop: one
//                               warning connect-time-changed, about the pause; with --strict, a
//                               speed command (written at once) fails the run after its end.
#include "launch_run_support.hpp"

#include <regex>

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
    write_file(dir / "sim.yaml", "vehicle_sil_simulation: 1\n"
                                 "speed: 1\n"
                                 "late_tolerance_ms: 60000\n"
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

/// The number after "<key>=" in the lines of `source` (a bridge's statistics), or -1.
long long stat_of(const std::vector<std::string>& lines, const std::string& source, const std::string& key)
{
    const std::regex pattern(key + "=([0-9]+)");
    long long value = -1;
    for (const std::string& line : events_with(lines, {"\"event\":\"log\"", "\"source\":\"" + source + "\""}))
    {
        std::smatch match;
        if (std::regex_search(line, match, pattern))
        {
            value = std::stoll(match[1].str());
        }
    }
    return value;
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
    const fs::path dir = connect_dir(o, "roundtrip", "stop_at_ms: 1500\n", talker(o), {{"Talker", "talk CAN"}});
    const Captured run = launch(o, {"run", "--events", "--speed", "1", "--strict", "--connect", "ETH_EXT=fake0", "--connect",
                                    "CAN=virtual:1", "sim.yaml"},
                                dir);
    const std::vector<std::string> lines = check_events(run.standard_output);
    expect_code(run, 0, "run --connect with both bridges");
    require(contains(lines.back(), "\"errors\":0,\"warnings\":0,\"notes\":0"), "no finding:\n" + lines.back());
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
    // The frames went out and the answers came back.
    for (const auto& [source, key] : std::vector<std::pair<std::string, std::string>>{{"bridge.ETH_EXT", "silkit_to_ext"},
                                                                                       {"bridge.ETH_EXT", "ext_to_silkit"},
                                                                                       {"bridge.CAN", "silkit_to_xl"},
                                                                                       {"bridge.CAN", "xl_to_silkit"}})
    {
        require(stat_of(lines, source, key) >= 1, source + " " + key + " >= 1:\n" + out_of(lines));
    }
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
    require(contains(result, "\"outside\":[{\"network\":\"ETH_EXT\",\"target\":\"fake0\",\"participant\":\"bridge.ETH_EXT\","
                             "\"to_outside\":null,\"from_outside\":null},{\"network\":\"CAN\",\"target\":\"virtual:1\","
                             "\"participant\":\"bridge.CAN\",\"to_outside\":null,\"from_outside\":null}]"),
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
    const fs::path dir = connect_dir(o, "no_adapter", "start_timeout_ms: 120000\n", talker(o), {{"Talker", "talk CAN"}});
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

void connect_speed(const Options& base)
{
    const Options o = connect_options(base);
    const fs::path dir = connect_dir(o, "speed", "stop_at_ms: 300\n", "");
    // --speed over the file's speed 1: a warning about --speed.
    const Captured fast = launch(o, {"run", "--events", "--speed", "2", "--connect", "ETH_EXT=fake0", "sim.yaml"}, dir);
    const std::vector<std::string> fastLines = check_events(fast.standard_output);
    expect_code(fast, 0, "run --connect --speed 2 (a warning)");
    require(contains(finding_in(fastLines, "connect-not-real-time"), "--speed 2 runs the simulation at that speed"),
            "the warning is about --speed:\n" + out_of(fastLines));
    // With --strict nothing starts, not even the bridge.
    const Captured strict =
        launch(o, {"run", "--events", "--strict", "--speed", "2", "--connect", "ETH_EXT=fake0", "sim.yaml"}, dir);
    const std::vector<std::string> strictLines = check_events(strict.standard_output);
    expect_code(strict, 1, "run --strict --connect --speed 2");
    require(contains(strictLines.back(), "\"result\":\"check_failed\"") &&
                events_with(strictLines, {"\"name\":\"bridge.ETH_EXT\"", "\"state\":\"started\""}).empty(),
            "the check failed, and the bridge was not started:\n" + out_of(strictLines));
    // --start-paused: a warning, and it runs after a resume.
    Live live(o, dir, {"run", "--events", "--stdin-commands", "--start-paused", "--connect", "ETH_EXT=fake0", "sim.yaml"});
    live.wait_for({"\"event\":\"time\"", "\"sim_ms\":0,", "\"paused\":true,"});
    live.write("{\"command\":\"resume\"}\n");
    const auto [status, lines] = live.finish();
    require(status && status->code == 0, "exit code " + code_text(status) + "\n" + out_of(lines));
    require(contains(finding_in(lines, "connect-start-paused"), "with --start-paused, the devices there go on"),
            "the warning connect-start-paused:\n" + out_of(lines));
    require(contains(lines.back(), "\"simulation\":\"reached_stop_at\""), "it ran to its end:\n" + lines.back());
    // Starting paused and the resume do not change the time of a run that goes on in real time.
    require(finding_in(lines, "connect-time-changed").empty(), "no connect-time-changed:\n" + out_of(lines));
}

void connect_time_changed(const Options& base)
{
    const Options o = connect_options(base);
    const std::string code = "\"code\":\"connect-time-changed\"";
    // Both networks connected, as connect_roundtrip: the check has no finding, so --strict fails on this one only.
    // No stop_at_ms: the commands do not have to come within a time of the run; a stop ends it.
    {
        const fs::path dir = connect_dir(o, "time_changed", "", talker(o), {{"Talker", "talk CAN"}});
        Live live(o, dir, {"run", "--events", "--stdin-commands", "--connect", "ETH_EXT=fake0", "--connect", "CAN=virtual:1",
                           "sim.yaml"});
        live.wait_for({"\"event\":\"phase\"", "\"phase\":\"running\""});
        live.write("{\"command\":\"pause\"}\n");
        const std::string paused = live.wait_for({"\"event\":\"time\"", "\"paused\":true,"});
        live.write("{\"command\":\"speed\",\"value\":2}\n{\"command\":\"resume\"}\n");
        live.wait_for({"\"event\":\"time\"", "\"speed\":2,", "\"paused\":false,"});
        live.write("{\"command\":\"stop\"}\n");
        const auto [status, lines] = live.finish();
        require(status && status->code == 130, "stopped by a command: exit code " + code_text(status) + "\n" + out_of(lines));
        // One finding, about the first command, at the time it came (not after where it stood still).
        const std::vector<std::string> found = events_with(lines, {"\"event\":\"finding\"", code});
        require(found.size() == 1, "one finding connect-time-changed:\n" + out_of(lines));
        std::smatch match;
        const bool named = std::regex_search(found[0], match,
                                             std::regex("the time command 'pause' from stdin at ([0-9]+) ms: the simulation "
                                                        "stood still while the devices there went on in real time"));
        require(named && std::stoll(match[1].str()) <= std::stoll(value_of(paused, "sim_ms")),
                "the finding names the pause and when it came:\n" + found[0] + "\n" + paused);
        require(contains(lines.back(), "\"errors\":0,\"warnings\":1,"), "this warning only, no error:\n" + lines.back());
    }
    // With --strict the warning fails the run, after it has run to its end. The speed is written at
    // once: a command before the simulation starts is kept and counts as well, so it cannot come too
    // late; stop_at_ms leaves room for a slow reader all the same.
    const fs::path dir = connect_dir(o, "time_changed_strict", "stop_at_ms: " + std::to_string(scaled_timeout(10s).count()) + "\n",
                                     talker(o), {{"Talker", "talk CAN"}});
    Live live(o, dir, {"run", "--events", "--strict", "--stdin-commands", "--connect", "ETH_EXT=fake0", "--connect",
                       "CAN=virtual:1", "sim.yaml"});
    live.write("{\"command\":\"speed\",\"value\":\"max\"}\n");
    const auto [status, lines] = live.finish();
    require(status && status->code == 1, "--strict: exit code " + code_text(status) + "\n" + out_of(lines));
    require(contains(finding_in(lines, "connect-time-changed"), "the time command 'speed max' from stdin at ") &&
                contains(lines.back(), "\"simulation\":\"reached_stop_at\"") && contains(lines.back(), "\"result\":\"failed\""),
            "the run went to its end and failed on the warning:\n" + out_of(lines));
}
