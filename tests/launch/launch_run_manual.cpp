// SPDX-License-Identifier: MIT
//
// The launch_run scenarios about participants started by hand, the network the participants
// listen on, and a registry port that is taken:
//
//   busy_port    the registry's port taken by another program, and by a run that waits: the setup
//                error at once, the first run goes on; a registry_uri of another computer: the
//                error names the host
//   manual       participants started by hand (--manual, manual: true), started by this test from
//                the command the launcher writes, as a person would copy it: the example and its
//                lines in docs/simulation-config.md. The other parts, each a scenario of its own
//                so that they run side by side: manual_late (started long after start_timeout_ms
//                and standing still in a step, as in a debugger), manual_fails (it fails while the
//                simulation runs: the events), manual_stop (a stop before the start ends one that
//                has joined), manual_frozen and manual_thawed (frozen as a debugger would), manual_again
//                (ended before the start), manual_timeout (--timeout-ms while the launcher waits),
//                manual_stranger (started with a mistyped name: the launcher says so; not of an
//                autonomous participant outside the file)
#include "launch_run_support.hpp"

void network(const Options& o)
{
    // Which VEHICLE_SIL_LISTEN_ON_NETWORK the participants get: 1 when other computers may reach
    // the registry (0.0.0.0), so that they listen on every address although they connect to it at
    // 127.0.0.1; else 0, also when the launcher's own environment has 1 (a note says so). A
    // participant started by hand is told the variable in the line and the event.
    const auto config = [&o](const std::string& host) {
        return "vehicle_sil_simulation: 2\n"
               "# run-option --stop-at-ms 1000\n"
               "registry_uri: silkit://" + host + ":0\n" // a free port, chosen by the registry
               "networks: []\n"
               "participants:\n"
               "  - name: Engine\n"
               "    command: [" + ecu(o) + "]\n"
               "  - name: Env\n"
               "    command: [" + helper(o) + ", --env, VEHICLE_SIL_LISTEN_ON_NETWORK, --uri, \"{registry_uri}\"]\n"
               "    describe: false\n"
               "    required: false\n"
               "  - name: Hand\n"
               "    command: [" + helper(o) + ", --uri, \"{registry_uri}\"]\n"
               "    describe: false\n"
               "    required: false\n"
               "    manual: true\n";
    };
    const fs::path dir = scenario_dir(o, config("127.0.0.1"));
    const std::string file = shown_path(dir / "sim.yaml");
    {
        const Captured local = launch(o, {"run", file}, dir);
        expect_code(local, 0, "run with a registry on 127.0.0.1");
        const std::string all = local.standard_output + lf(local.standard_error);
        require(!contains(local.standard_output, "is not a loopback address"), "no note on 127.0.0.1:\n" + local.standard_output);
        require(!contains(all, "listen-variable-ignored") && !contains(all, "in the environment of vehicle_sil_launch"),
                "no note on the variable:\n" + all);
        require(contains(all, "[Env] VEHICLE_SIL_LISTEN_ON_NETWORK=0\n"), "0 on 127.0.0.1:\n" + all);
        require(contains(all, "start it yourself (manual: true, or --manual). In the directory "), "the manual line as it was:\n" + all);
        require(!contains(all, "VEHICLE_SIL_LISTEN_ON_NETWORK=1"), "no 1 anywhere on 127.0.0.1:\n" + all);
        require(!contains(all, "from other computers"), "nothing for other computers on 127.0.0.1:\n" + all);
    }
    // The launcher's own environment has 1: not passed on, and a note.
#ifdef _WIN32
    _putenv_s("VEHICLE_SIL_LISTEN_ON_NETWORK", "1");
#else
    ::setenv("VEHICLE_SIL_LISTEN_ON_NETWORK", "1", 1);
#endif
    {
        const Captured inherited = launch(o, {"run", file}, dir);
        expect_code(inherited, 0, "run with VEHICLE_SIL_LISTEN_ON_NETWORK=1 in the launcher's environment");
        const std::string all = inherited.standard_output + lf(inherited.standard_error);
        require(contains(inherited.standard_output,
                         "note: " + file + ": VEHICLE_SIL_LISTEN_ON_NETWORK=1 in the environment of vehicle_sil_launch is not "
                         "used: the registry is on a loopback address, so run gives the participants it starts "
                         "VEHICLE_SIL_LISTEN_ON_NETWORK=0"),
                "the note on the variable:\n" + inherited.standard_output);
        require(contains(all, "[Env] VEHICLE_SIL_LISTEN_ON_NETWORK=0\n"), "0 although the launcher has 1:\n" + all);
    }
#ifdef _WIN32
    _putenv_s("VEHICLE_SIL_LISTEN_ON_NETWORK", "");
#else
    ::unsetenv("VEHICLE_SIL_LISTEN_ON_NETWORK");
#endif
#ifndef _WIN32
    // Not on Windows: there its firewall would ask whether to let the registry listen on 0.0.0.0.
    write_file(dir / "sim.yaml", config("0.0.0.0"));
    const Captured run = launch(o, {"run", file}, dir);
    expect_code(run, 0, "run with a registry on 0.0.0.0");
    const std::string all = run.standard_output + lf(run.standard_error);
    require(contains(run.standard_output, "note: " + file + ":3: registry_uri silkit://0.0.0.0:"),
            "the note on the registry:\n" + run.standard_output);
    require(contains(run.standard_output, " is not a loopback address: the registry and the participants listen on the network"),
            "the note says what it means:\n" + run.standard_output);
    // Right after the registry's line, where other computers join (this computer's addresses).
    require(contains(run.standard_error, "\n[vehicle_sil_launch] from other computers, join at silkit://") &&
                contains(run.standard_error, ": give it as --registry-uri to a participant started by hand on another "
                                             "computer (instead of the loopback address in its command; it needs no "
                                             "VEHICLE_SIL_LISTEN_ON_NETWORK)\n"),
            "the line for other computers:\n" + run.standard_error);
    require(run.standard_error.find("from other computers") > run.standard_error.find("[vehicle_sil_launch] registry at"),
            "after the registry's line:\n" + run.standard_error);
    // registry_uri has port 0: the lines and the commands have the port the registry took.
    require(!contains(run.standard_error, "registry at silkit://127.0.0.1:0\n") && !contains(run.standard_error, ":0: give it") &&
                !contains(all, "--registry-uri silkit://127.0.0.1:0 ") && contains(all, "--registry-uri silkit://127.0.0.1:"),
            "the port the registry took, not 0:\n" + all);
    // The participant's own line (the note names the variable too).
    require(contains(all, "[Env] VEHICLE_SIL_LISTEN_ON_NETWORK=1\n"), "the participant got the variable:\n" + all);
    require(contains(all, "not starting Hand: start it yourself (manual: true, or --manual). In the directory ") &&
                contains(all, ", with VEHICLE_SIL_LISTEN_ON_NETWORK=1 in its environment, run:\n"),
            "the manual participant is told the variable:\n" + all);
    require(contains(run.standard_output, "participant 'Hand': you start it yourself") &&
                contains(run.standard_output, "; start it with VEHICLE_SIL_LISTEN_ON_NETWORK=1 in its environment, so that the "
                                              "other computers reach it (registry_uri is not a loopback address)"),
            "the manual note tells the variable:\n" + run.standard_output);
    const Captured events = launch(o, {"serve", "--once", file}, dir);
    expect_code(events, 0, "serve --once with a registry on 0.0.0.0");
    require(contains(events.standard_output, "\"environment\":{\"VEHICLE_SIL_LISTEN_ON_NETWORK\":\"1\"}"),
            "the manual event has the variable:\n" + events.standard_output);
    const Captured checked = launch(o, {"check", file}, dir);
    expect_code(checked, 0, "check with a registry on 0.0.0.0");
    require(contains(checked.standard_output, "registry_uri silkit://0.0.0.0:"), "check has the note:\n" + checked.standard_output);
#endif
}

/// A TCP socket that listens on 127.0.0.1 at a free port for as long as it lives (another program
/// at a registry's port).
class Listener
{
public:
    Listener()
    {
#ifdef _WIN32
        WSADATA data;
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(socket_ != INVALID_SOCKET, "cannot make a socket");
        int size = sizeof(sockaddr_in);
#else
        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        require(socket_ >= 0, "cannot make a socket");
        socklen_t size = sizeof(sockaddr_in);
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool listening = ::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                               ::listen(socket_, 4) == 0 &&
                               ::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) == 0;
        require(listening, "cannot listen on 127.0.0.1");
        port_ = ntohs(address.sin_port);
    }
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    ~Listener()
    {
#ifdef _WIN32
        closesocket(socket_);
        WSACleanup();
#else
        ::close(socket_);
#endif
    }
    int port() const { return port_; }

private:
#ifdef _WIN32
    SOCKET socket_ = INVALID_SOCKET;
#else
    int socket_ = -1;
#endif
    int port_ = 0;
};

/// A string value of an event line as JSON writes it (the escapes of a path undone).
std::string json_text(const std::string& line, const std::string& key)
{
    const std::string raw = value_of(line, key);
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i)
    {
        out += raw[i] == '\\' && i + 1 < raw.size() ? raw[++i] : raw[i];
    }
    return out;
}

/// The configuration of a part of the manual scenarios: Auto, started by the launcher, Hand,
/// started by hand, and `more`, at a free port of its own (port 0; not 8500, the port a run with a
/// participant started by hand takes by default, which may be taken here).
std::string hand_config(const Options& o, const std::string& keys, const std::string& more)
{
    return "vehicle_sil_simulation: 2\n" + keys + "registry_uri: silkit://127.0.0.1:0\n"
           "networks: []\n"
           "participants:\n"
           "  - name: Auto\n"
           "    command: [" + ecu(o) + "]\n"
           "  - name: Hand\n"
           "    command: [" + ecu(o) + "]\n"
           "    manual: true\n" + more;
}

/// A third participant started by hand, Late, for hand_config().
std::string late_participant(const Options& o) { return "  - name: Late\n    command: [" + ecu(o) + "]\n    manual: true\n"; }

void manual_late(const Options& o)
{
    {
        // Started long after start_timeout_ms (whose limit begins only once Hand is ready, so it
        // only has to cover the start after that, generous at 2 s scaled), and standing still in
        // its step at 100 ms for 1.5 s, as in a debugger: the run passes.
        const std::chrono::milliseconds startTimeout = scaled_timeout(2000ms);
        const fs::path dir = hand_dir(o, "late",
                                      hand_config(o, "# run-option --stop-at-ms 300\n# run-option --participant-start-timeout-ms " +
                                                         std::to_string(startTimeout.count()) + "\n", ""),
                                      {{"Auto", "pid"}, {"Hand", "slow-at-ms 100 1500"}});
        Live live(o, dir, {"run", "sim.yaml"});
        const std::string err = live.wait_for_error(
            [](const std::string& text) { return contains(text, "[vehicle_sil_launch] waiting for Hand to join (manual): "); });
        const HandCommand hand = hand_command(err, "Hand");
        require(!hand.line.empty() && contains(err, "waiting for Hand to join (manual): start it with the command above"),
                "the launcher waits for Hand:\n" + err);
        // The ECUs it starts wait for the start as long as it takes.
        require(contains(err, "--name Auto --registry-uri silkit://127.0.0.1:") &&
                    contains(err, " --start-timeout-ms 1000000000000\n") &&
                    contains(hand.line, " --start-timeout-ms 1000000000000"),
                "every ECU gets --start-timeout-ms 1000000000000:\n" + err);
        std::this_thread::sleep_for(startTimeout + 500ms);
        ByHand byHand(hand.line, hand.directory);
        const auto [status, out] = live.finish_unchecked();
        require(status && status->code == 0, "the run with Hand started late ends with 0:\n" + out_of(out));
        expect_text(run_part(out_of(out)),
                    "run: the simulation ran to stop_at_ms (300 ms) and ended\n"
                    "\n"
                    "participant  required  ended\n"
                    "Auto         yes       exit code 0\n"
                    "Hand         yes       started by you (manual)\n"
                    "\n"
                    "traffic: no network in use\n"
                    "\n"
                    "passed: 0 errors, 0 warnings, 1 note\n",
                    "a participant started by hand after start_timeout_ms");
        const auto ended = byHand.wait(30s);
        require(ended && ended->code == 0, "Hand ends by itself with 0");
        require(all_gone(dir, {"Auto.pid"}, 30s), "Auto has ended");
    }
}

void manual_fails(const Options& o)
{
    {
        // It fails while the simulation runs: the launcher sees it leave the simulation.
        const fs::path dir = hand_dir(o, "fails", hand_config(o, "# run-option --stop-at-ms 1000\n", ""), {{"Auto", ""}, {"Hand", "exit-at-ms 100 3"}});
        Live live(o, dir, {"serve", "--once", "sim.yaml"});
        const std::string event = live.wait_for({"\"name\":\"Hand\"", "\"state\":\"manual\""});
        require(contains(event, "\"command\":[\"") && contains(event, "\"--name\",\"Hand\",\"--registry-uri\","),
                "the event manual has the command:\n" + event);
        ByHand byHand(json_text(event, "command_line"), json_text(event, "working_directory"));
        const auto [status, lines] = live.finish();
        require(status && status->code == 1, "the run ends with 1 when Hand fails");
        require(events_with(lines, {"\"name\":\"Hand\"", "\"state\":\"joined\""}).size() == 1, "Hand joined (the event)");
        require(events_with(lines, {"\"name\":\"Hand\"", "\"state\":\"failed\"", "\"reason\":\"left_simulation\""}).size() == 1,
                "Hand failed: it left the simulation (the event)");
        require(events_with(lines, {"\"name\":\"Hand\"", "\"state\":\"started\""}).empty() &&
                    events_with(lines, {"\"name\":\"Hand\"", "\"state\":\"ended\""}).empty(),
                "no process of Hand in the events");
        const std::string& result = lines.back();
        require(contains(result, "{\"name\":\"Hand\",\"required\":true,\"enabled\":true,\"manual\":true,\"for_network\":null,\"ended\":\"not_started\","
                                 "\"exit_code\":null,"),
                "the result says that Hand was started by hand:\n" + result);
        require(contains(result, "\"code\":\"participant-failed\"") &&
                    contains(result, "participant 'Hand': it failed while the simulation was running") &&
                    contains(result, "you started it (manual), so what it wrote is where you started it"),
                "the finding about Hand:\n" + result);
        const auto ended = byHand.wait(30s);
        require(ended && ended->code == 3, "Hand ended with 3, as its behaviour says");
    }
}

void manual_stop(const Options& o)
{
    {
        // A stop before the start (Late never comes): Hand, which has joined, is told to end.
        const fs::path dir = hand_dir(o, "stop", hand_config(o, "", late_participant(o)), {{"Auto", ""}, {"Hand", ""}, {"Late", ""}});
        Live live(o, dir, {"serve", "--once", "--stdin-commands", "sim.yaml"});
        const std::string event = live.wait_for({"\"name\":\"Hand\"", "\"state\":\"manual\""});
        ByHand byHand(json_text(event, "command_line"), json_text(event, "working_directory"));
        live.wait_for({"\"name\":\"Hand\"", "\"state\":\"joined\""});
        live.write("{\"command\":\"stop\"}\n");
        const auto [status, lines] = live.finish();
        require(status && status->code == 0, "the run stopped before the start ends with 0, not " + code_text(status));
        const auto ended = byHand.wait(30s);
        require(ended && ended->code == 3, "Hand, which waited for the start, ends as a start that failed (3)");
        require(contains(lines.back(), "{\"name\":\"Late\",\"required\":true,\"enabled\":true,\"manual\":true,\"for_network\":null,\"ended\":\"not_started\","),
                "Late in the result:\n" + lines.back());
    }
    if (can_request_stop())
    {
        // A signal must also wake a run that can wait indefinitely for a manual participant.
        const fs::path dir = hand_dir(o, "signal", hand_config(o, "", ""), {{"Auto", ""}, {"Hand", ""}});
        Live live(o, dir, {"serve", "--once", "sim.yaml"});
        live.wait_for({"\"name\":\"Hand\"", "\"state\":\"manual\""});
        live.wait_for({"\"name\":\"Auto\"", "\"state\":\"joined\""});
#ifdef _WIN32
        require(live.process().request_stop(), "Ctrl+Break could not be sent");
#else
        ::kill(static_cast<pid_t>(live.process().id()), SIGINT);
#endif
        const auto [status, lines] = live.finish();
        require(status && status->code == 130, "a signal ends a run waiting for a manual participant");
        require(contains(lines.back(), "\"result\":\"interrupted\""), "the run reports the interrupt");
    }
}

void manual_frozen(const Options& o)
{
    {
        // Frozen (a debugger's pause) as soon as it has joined: the start has no time limit, and
        // a stop command ends the run all the same. Late is the last to join, so the start goes on
        // at once; whether it is frozen before its first step or the simulation runs without it
        // stepping depends on how far it got (logged), and both must end on the stop.
        // No stop_at_ms: if Late got to its first steps before it was frozen, the simulation must not
        // end before the stop command.
        const fs::path dir = hand_dir(o, "frozen", hand_config(o, "# run-option --participant-stop-timeout-ms 1000\n", late_participant(o)),
                                      {{"Auto", ""}, {"Hand", ""}, {"Late", ""}});
        Live live(o, dir, {"serve", "--once", "--stdin-commands", "sim.yaml"});
        const std::string hand = live.wait_for({"\"name\":\"Hand\"", "\"state\":\"manual\""});
        const std::string lateEvent = live.wait_for({"\"name\":\"Late\"", "\"state\":\"manual\""});
        ByHand byHand(json_text(hand, "command_line"), json_text(hand, "working_directory"));
        live.wait_for({"\"name\":\"Hand\"", "\"state\":\"joined\""});
        ByHand lateByHand(json_text(lateEvent, "command_line"), json_text(lateEvent, "working_directory"));
        live.wait_for({"\"name\":\"Late\"", "\"state\":\"joined\""});
        lateByHand.freeze();
        std::this_thread::sleep_for(500ms);
        const bool runningBeforeStop = !events_with(live.lines(), {"\"phase\":\"running\""}).empty();
        live.write("{\"command\":\"stop\"}\n");
        const auto [status, lines] = live.finish_unchecked(60s);
        lateByHand.thaw();
        const bool running = !events_with(lines, {"\"phase\":\"running\""}).empty();
        log(std::string("RESULT Late frozen as it joined: ") +
            (runningBeforeStop ? "the simulation runs (Late got to its first step)"
             : running         ? "the simulation had passed communication-ready and reached running on stop"
                               : "the start waits for Late"));
        // Before the start, the stop ends the run without errors (0). Once the simulation runs, frozen
        // Late keeps it from shutting down, and the participants that wait in their step for Late may
        // not end when asked: the stopped run then reports those errors (1, result failed).
        const bool cleanly = status && status->code == 0;
        require(cleanly || (running && status && status->code == 1),
                "a stop ends a run that waits for a frozen participant (0, or 1 with errors once it ran), not " +
                    (status ? std::to_string(status->code) : std::string("still running")) + ":\n" + out_of(lines));
        check_events(lines);
        require(contains(lines.back(), cleanly ? "\"result\":\"stopped\"" : "\"result\":\"failed\"") &&
                    contains(lines.back(), "\"end\":\"stopped\"") &&
                    contains(lines.back(), "\"interrupted_by\":\"stop command\""),
                "the run was stopped by the command:\n" + lines.back());
        require(!contains(lines.back(), "connection-not-closed"),
                "the launcher closed its SIL Kit connection, with Late frozen:\n" + lines.back());
        // Hand ends when it was told to (the start was aborted); if the simulation ran, it may wait
        // in its step for Late's for ever (the launcher has gone): it is ended with the test.
        if (!running)
        {
            require(byHand.wait(30s).has_value(), "Hand, which waited for the start, has ended:\n" + out_of(lines));
        }
    }
    {
        // Frozen once the simulation runs (a debugger's pause in a step): a stop command ends the
        // run all the same, with Late still frozen (Auto, which waits for Late's step, has to be
        // ended: an error, so the stopped run failed), and the launcher closes its connection to it
        // (on Windows a TCP connection: closing a local-domain socket to a process that stands
        // still may never return there, docs/dependencies.md).
        const fs::path dir = hand_dir(o, "frozen_running", hand_config(o, "# run-option --participant-stop-timeout-ms 1000\n", late_participant(o)),
                                      {{"Auto", ""}, {"Hand", ""}, {"Late", ""}});
        Live live(o, dir, {"serve", "--once", "--stdin-commands", "sim.yaml"});
        const std::string hand = live.wait_for({"\"name\":\"Hand\"", "\"state\":\"manual\""});
        const std::string lateEvent = live.wait_for({"\"name\":\"Late\"", "\"state\":\"manual\""});
        ByHand byHand(json_text(hand, "command_line"), json_text(hand, "working_directory"));
        live.wait_for({"\"name\":\"Hand\"", "\"state\":\"joined\""});
        ByHand lateByHand(json_text(lateEvent, "command_line"), json_text(lateEvent, "working_directory"));
        live.wait_for({"\"phase\":\"running\""});
        std::this_thread::sleep_for(300ms);
        lateByHand.freeze();
        std::this_thread::sleep_for(500ms);
        live.write("{\"command\":\"stop\"}\n");
        const auto [status, lines] = live.finish_unchecked(60s);
        lateByHand.thaw();
        require(status && status->code == 1,
                "a stop ends a run with a participant frozen while the simulation runs (1: Auto did not end), not " +
                    (status ? std::to_string(status->code) : std::string("still running")) + ":\n" + out_of(lines));
        check_events(lines);
        require(contains(lines.back(), "\"result\":\"failed\",\"end\":\"stopped\",\"ok\":false,") &&
                    contains(lines.back(), "\"code\":\"did-not-end\"") && contains(lines.back(), "\"interrupted_by\":\"stop command\""),
                "the run was stopped by the command:\n" + lines.back());
        require(!contains(lines.back(), "connection-not-closed"),
                "the launcher closed its SIL Kit connection, with Late frozen:\n" + lines.back());
    }
}

void manual_thawed(const Options& o)
{
    {
        // Frozen for 3 s after it joined, before the first step (Late cannot join while it is), then
        // going on: the run passes. Whether its variables can be used is logged (its control link
        // at the start).
        const fs::path dir = hand_dir(o, "thawed", hand_config(o, "# run-option --stop-at-ms 200\n", late_participant(o)), {{"Auto", ""}, {"Hand", "variables"}, {"Late", ""}});
        Live live(o, dir, {"serve", "--once", "sim.yaml"});
        const std::string hand = live.wait_for({"\"name\":\"Hand\"", "\"state\":\"manual\""});
        const std::string lateEvent = live.wait_for({"\"name\":\"Late\"", "\"state\":\"manual\""});
        ByHand byHand(json_text(hand, "command_line"), json_text(hand, "working_directory"));
        live.wait_for({"\"name\":\"Hand\"", "\"state\":\"joined\""});
        byHand.freeze();
        ByHand lateByHand(json_text(lateEvent, "command_line"), json_text(lateEvent, "working_directory"));
        std::this_thread::sleep_for(3000ms);
        byHand.thaw();
        const auto [status, lines] = live.finish();
        require(status && status->code == 0, "a participant frozen before the first step and thawed: the run passes:\n" +
                                                 lines.back());
        const auto links = events_with(lines, {"\"event\":\"control_link\"", "\"name\":\"Hand\""});
        require(links.size() == 1, "one control_link of Hand");
        log("RESULT the control link of Hand, frozen for 3 s before the start: " + links.front());
        require(byHand.wait(30s).has_value() && lateByHand.wait(30s).has_value(), "Hand and Late have ended");
    }
}

void manual_again(const Options& o)
{
    {
        // Ended after it joined, before the start (to start it again): the start fails, and the
        // finding says to start the launcher again as well.
        const fs::path dir = hand_dir(o, "again", hand_config(o, "", late_participant(o)), {{"Auto", ""}, {"Hand", ""}, {"Late", ""}});
        Live live(o, dir, {"serve", "--once", "sim.yaml"});
        const auto hand = live.wait_for({"\"name\":\"Hand\"", "\"state\":\"manual\""});
        const auto late = live.wait_for({"\"name\":\"Late\"", "\"state\":\"manual\""});
        auto byHand = std::make_unique<ByHand>(json_text(hand, "command_line"), json_text(hand, "working_directory"));
        live.wait_for({"\"name\":\"Hand\"", "\"state\":\"joined\""});
        byHand.reset(); // ends it
        ByHand lateByHand(json_text(late, "command_line"), json_text(late, "working_directory"));
        const auto [status, out] = live.finish_unchecked(60s);
        require(status && status->code == 1 &&
                    contains(out_of(out), "\"code\":\"simulation-start\"") &&
                    contains(out_of(out), "the simulation did not start: ") &&
                    contains(out_of(out), "participant 'Hand' left the simulation before it started") &&
                    contains(out_of(out), "you started it yourself (manual), and it cannot join this run again: to start "
                                          "it again, start vehicle_sil_launch again as well"),
                "a participant started by hand that ended before the start:\n" + out_of(out));
        require(lateByHand.wait(30s).has_value(), "Late, which waited for the start, has ended");
    }
}

void manual_timeout(const Options& o)
{
    {
        // --timeout-ms counts from when the launcher began to wait: Late never comes.
        const fs::path dir = hand_dir(o, "timeout", hand_config(o, "", late_participant(o)), {{"Auto", ""}, {"Hand", ""}, {"Late", ""}});
        Live live(o, dir, {"run", "--timeout-ms", "3000", "sim.yaml"});
        const std::string err = live.wait_for_error([](const std::string& text) { return !hand_command(text, "Hand").line.empty(); });
        const HandCommand hand = hand_command(err, "Hand");
        ByHand byHand(hand.line, hand.directory);
        const auto [status, out] = live.finish_unchecked(60s);
        require(status && status->code == 1, "--timeout-ms ends a run that waits for a participant started by hand:\n" +
                                                 out_of(out) + "--- stderr ---\n" + live.err());
        require(contains(out_of(out), "error: sim.yaml: the simulation did not start within 3000 ms of wall-clock time "
                                      "(--timeout-ms"),
                "the timeout finding:\n" + out_of(out));
        require(contains(out_of(out), "Late (manual) has not joined"), "the finding names Late:\n" + out_of(out));
        require(byHand.wait(30s).has_value(), "Hand, which waited for the start, has ended");
    }
}

void busy_port(const Options& o)
{
    // The registry's port is taken: run fails at once with the setup error, before it starts
    // anything (the registry does not go on with a local-domain socket only).
    const auto config = [&o](int port) {
        return "vehicle_sil_simulation: 2\n"
               "# run-option --stop-at-ms 300\n"
               "registry_uri: silkit://127.0.0.1:" + std::to_string(port) + "\n"
               "networks: []\n"
               "participants:\n"
               "  - name: Auto\n"
               "    command: [" + ecu(o) + "]\n"
               "  - name: Hand\n"
               "    command: [" + ecu(o) + "]\n"
               "    manual: true\n";
    };
    const auto refused = [](const Captured& run, int port, const std::string& what) {
        expect_code(run, 3, what);
        const std::string uri = "silkit://127.0.0.1:" + std::to_string(port);
        require(contains(run.standard_output, "error: sim.yaml: cannot set up the simulation: the registry cannot listen at " +
                                                  uri + ": ") &&
                    contains(run.standard_output, "(does another registry run there? choose another port in registry_uri, "
                                                  "or port 0 for a free one)"),
                what + ": the setup error names the port:\n" + run.standard_output);
        require(!contains(run.standard_error, "[vehicle_sil_launch] registry at") &&
                    !contains(run.standard_error, "[vehicle_sil_launch] starting Auto"),
                what + ": no registry, nothing started:\n" + run.standard_error);
    };
    {
        const Listener other;
        const fs::path dir = scenario_dir(o, config(other.port()));
        refused(launch(o, {"run", "sim.yaml"}, dir), other.port(), "another program at the registry's port");
    }
    {
        // A host that is not this computer's (192.0.2.1 is for documentation, never assigned), at
        // port 0: nothing has the port, and the error says what is wrong with the host.
        std::string text = config(1);
        text.replace(text.find("silkit://127.0.0.1:1\n"), std::string("silkit://127.0.0.1:1").size(), "silkit://192.0.2.1:0");
        const Captured run = launch(o, {"run", "sim.yaml"}, scenario_dir(o, text));
        expect_code(run, 3, "a registry_uri of another computer");
        require(contains(run.standard_output, "error: sim.yaml: cannot set up the simulation: the registry cannot listen at "
                                              "silkit://192.0.2.1:0: ") &&
                    contains(run.standard_output, " (192.0.2.1 is not an address of this computer, and the registry listens on "
                                                  "this computer: write in registry_uri ") &&
                    !contains(run.standard_output, "another registry"),
                "a registry_uri of another computer: the setup error names the host:\n" + run.standard_output);
        require(!contains(run.standard_error, "[vehicle_sil_launch] starting Auto"),
                "a registry_uri of another computer: nothing started:\n" + run.standard_error);
    }
    // A second run at the port of a run that waits for a participant started by hand: refused, and
    // the first one goes on with its own registry (the participant started by hand joins it).
    const int port = free_port();
    const fs::path dir = hand_dir(o, "busy", config(port), {{"Auto", ""}, {"Hand", ""}});
    Live first(o, dir, {"run", "sim.yaml"});
    const std::string err = first.wait_for_error(
        [](const std::string& text) { return contains(text, "[vehicle_sil_launch] waiting for Hand to join (manual): "); });
    const HandCommand hand = hand_command(err, "Hand");
    require(!hand.line.empty(), "the first run waits for Hand:\n" + err);
    refused(launch(o, {"run", "sim.yaml"}, dir), port, "a second run at the port of the first");
    ByHand byHand(hand.line, hand.directory);
    const auto [status, out] = first.finish_unchecked();
    require(status && status->code == 0, "the first run, with Hand, ends with 0:\n" + out_of(out) + "--- stderr ---\n" + first.err());
    const auto ended = byHand.wait(30s);
    require(ended && ended->code == 0, "Hand, started by hand, ends with 0");
}

void manual_stranger(const Options& o)
{
    // Started by hand with a mistyped name: it waits for the start (in ServicesCreated), and the
    // launcher says so once, and waits on for Hand. A participant
    // outside the file that runs on its own (autonomous) is not named: it is one of the simulation.
    const fs::path dir = hand_dir(o, "stranger", hand_config(o, "# run-option --stop-at-ms 300\n", ""), {{"Auto", ""}, {"Hand", ""}});
    Live live(o, dir, {"run", "sim.yaml"});
    const std::string err = live.wait_for_error(
        [](const std::string& text) { return contains(text, "[vehicle_sil_launch] waiting for Hand to join (manual): "); });
    const HandCommand hand = hand_command(err, "Hand");
    require(!hand.line.empty(), "the launcher waits for Hand:\n" + err);
    const auto uriAt = hand.line.find("--registry-uri ");
    require(uriAt != std::string::npos, "the command has --registry-uri: " + hand.line);
    const std::string uri = hand.line.substr(uriAt + 15, hand.line.find(' ', uriAt + 15) - uriAt - 15);
    ByHand sender("\"" + shown_path(o.test_ecu) + "\" --autonomous Sender " + uri, hand.directory);
    const fs::path running = fs::path(hand.directory) / "Sender.running";
    for (int i = 0; i < 300 && !fs::exists(running); ++i)
    {
        std::this_thread::sleep_for(100ms);
    }
    require(fs::exists(running), "the autonomous participant Sender runs");
    std::string mistyped = hand.line;
    const auto at = mistyped.find("--name Hand ");
    require(at != std::string::npos, "the command has --name Hand: " + mistyped);
    mistyped.replace(at, 12, "--name hand ");
    ByHand stranger(mistyped, hand.directory);
    const std::string line = "[vehicle_sil_launch] participant 'hand' joined the simulation, but this run has no participant "
                             "of that name (did you mean 'Hand'?): it waits for the simulation's start, which only the "
                             "participants of the file get; start it with the name the file gives it (--name)\n";
    const std::string told = live.wait_for_error([&](const std::string& text) { return contains(text, line); });
    require(contains(told, line), "the launcher names the stranger:\n" + told);
    // The launcher looks again meanwhile (several times): it has said it once.
    std::this_thread::sleep_for(1500ms);
    ByHand byHand(hand.line, hand.directory);
    const auto [status, out] = live.finish_unchecked();
    // When the virtual time stands still, whether the stranger still runs tells whether the others
    // wait for it (SIL Kit adds every participant with virtual time to the synchronisation when it
    // is discovered, and takes it out only when it leaves).
    require(status && status->code == 0, "the run with Hand ends with 0:\n" + out_of(out) + "--- stderr ---\n" + live.err() +
                                             stranger.report("hand (the mistyped name)") + sender.report("Sender") +
                                             byHand.report("Hand"));
    const std::string all = live.err();
    require(all.find(line) == all.rfind(line), "said once:\n" + all);
    require(!contains(all, "'Sender'"), "nothing of the autonomous participant:\n" + all);
    require(byHand.wait(30s).has_value(), "Hand ends by itself");
    // The stranger did not run with the others: its start failed when the simulation started (SIL
    // Kit refuses a coordinated participant that is not required), and it left. An exit code 0
    // would be a stranger that ran the simulation's steps as one of its participants. It left at
    // the start, so it is gone by the time the run and Hand have ended: one that still waits for
    // its own stop (SIL Kit not taking the stop of a participant that left Error again) is late.
    const auto strangerEnd = stranger.wait(3s);
    require(strangerEnd && strangerEnd->code == 3,
            "the participant with the mistyped name ends with 3 (its start failed, it did not take part) at the "
            "simulation's start, not later:\n" +
                stranger.report("hand (the mistyped name)"));
}

void manual(const Options& o)
{
    {
        // The example of the docs: Engine by hand, Dashboard by the launcher.
        Live live(o, o.example_dir, {"run", "simulation.yaml", "--stop-at-ms", "1000", "--manual", "Engine"});
        HandCommand hand;
        static_cast<void>(live.wait_for_error([&](const std::string& text) {
            hand = hand_command(text, "Engine");
            return !hand.line.empty();
        }));
        if (hand.line.empty())
        {
            // The launcher ended: its error is in its output (standard output).
            const auto [status, out] = live.finish_unchecked();
            const std::string all = out_of(out) + "--- stderr ---\n" + live.err();
            if (contains(all, "the registry cannot listen at silkit://127.0.0.1:8500"))
            {
                throw Skip("another program listens at 127.0.0.1:8500: the example of the docs was not run (the other "
                           "manual_* scenarios run at ports of their own)");
            }
            require(false, "no lines about Engine:\n" + all);
        }
        require(fs::equivalent(vehicle_sil::process::path_from_utf8(hand.directory), o.example_dir),
                "the directory is the example's: " + hand.directory);
        // As the docs show them: the directory as <dir>, paths with '/' and without ".exe".
        std::string text = hand.text;
        for (auto at = text.find(hand.directory); at != std::string::npos; at = text.find(hand.directory, at))
        {
            text.replace(at, hand.directory.size(), "<dir>");
        }
#ifdef _WIN32
        std::replace(text.begin(), text.end(), '\\', '/');
        for (auto at = text.find(".exe"); at != std::string::npos; at = text.find(".exe", at))
        {
            text.erase(at, 4);
        }
#endif
        expect_text(text, block_after(o.docs, "<!-- vehicle_sil_launch run simulation.yaml --manual Engine: the lines about Engine -->"),
                    "the lines about Engine started by hand");
        ByHand engine(hand.line, hand.directory);
        const auto [status, out] = live.finish_unchecked();
        require(status && status->code == 0, "run simulation.yaml --manual Engine ends with 0:\n" + out_of(out));
        expect_text(out_of(out), block_after(o.docs, "<!-- vehicle_sil_launch run simulation.yaml --manual Engine -->"),
                    "run simulation.yaml --manual Engine");
        const auto ended = engine.wait(30s);
        require(ended && ended->code == 0 && !ended->signal, "Engine, started by hand, ends by itself with 0");
    }
}

void manual_external(const Options& o)
{
    // A manual participant with describe: false and no local executable (either omitted command or
    // a remote-only command path): check, run, and serve --once succeed without a bogus cwd path,
    // while normal local missing-executable checks still fail strictly.
    const std::string autoPart =
        "  - name: Auto\n"
        "    command: [" + ecu(o) + "]\n";
    const auto join_cmd = [&o](const std::string& name, const std::string& uri) {
        return "\"" + shown_path(o.test_ecu) + "\" --name " + name + " --registry-uri " + uri +
               " --start-timeout-ms 1000000000000";
    };
    const auto registry_uri_from_err = [](const std::string& err) -> std::string {
        const std::string marker = "[vehicle_sil_launch] registry at ";
        const auto pos = err.find(marker);
        if (pos == std::string::npos)
        {
            return {};
        }
        const auto start = pos + marker.size();
        const auto end = err.find('\n', start);
        std::string uri = end == std::string::npos ? err.substr(start) : err.substr(start, end - start);
        if (!uri.empty() && uri.back() == '\r')
        {
            uri.pop_back();
        }
        return uri;
    };

    // 1. Omitted command (manual: true, describe: false) under serve --once.
    {
        const std::string cfg =
            "vehicle_sil_simulation: 2\n"
            "# run-option --stop-at-ms 200\n"
            "registry_uri: silkit://127.0.0.1:0\n"
            "networks: []\n"
            "participants:\n" +
            autoPart +
            "  - name: ExternalPeer\n"
            "    describe: false\n"
            "    manual: true\n";
        const fs::path dir = hand_dir(o, "ext_omit_serve", cfg, {{"Auto", ""}, {"ExternalPeer", ""}});
        Live live(o, dir, {"serve", "--once", "sim.yaml"});
        const std::string reg = live.wait_for({"\"event\":\"registry\""});
        const std::string ev = live.wait_for({"\"name\":\"ExternalPeer\"", "\"state\":\"manual\""});
        require(contains(ev, "\"command\":[],\"working_directory\":null,\"command_line\":null"),
                "omitted command manual event has null working_directory and command_line (no bogus cwd):\n" + ev);
        const std::string uri = json_text(reg, "uri");
        require(!uri.empty(), "registry uri present:\n" + reg);
        ByHand peer(join_cmd("ExternalPeer", uri), shown_path(dir));
        const auto [status, lines] = live.finish();
        require(status && status->code == 0, "serve --once with omitted command passes:\n" + out_of(lines));
        require(events_with(lines, {"\"name\":\"ExternalPeer\"", "\"state\":\"joined\""}).size() == 1,
                "ExternalPeer joined event");
        const auto ended = peer.wait(30s);
        require(ended && ended->code == 0, "ExternalPeer ended cleanly");
    }

    // 2. Omitted command under CLI run (verify stderr has no bogus cwd path and no "with the command above").
    {
        const std::string cfg =
            "vehicle_sil_simulation: 2\n"
            "# run-option --stop-at-ms 200\n"
            "registry_uri: silkit://127.0.0.1:0\n"
            "networks: []\n"
            "participants:\n" +
            autoPart +
            "  - name: ExternalPeer\n"
            "    describe: false\n"
            "    manual: true\n";
        const fs::path dir = hand_dir(o, "ext_omit_run", cfg, {{"Auto", ""}, {"ExternalPeer", ""}});
        Live live(o, dir, {"run", "sim.yaml"});
        const std::string err = live.wait_for_error([](const std::string& text) {
            return contains(text, "[vehicle_sil_launch] waiting for ExternalPeer to join (manual):");
        });
        require(contains(err, "No command is configured in the file; connect it to the simulation's registry as 'ExternalPeer'"),
                "omitted command stderr text:\n" + err);
        require(contains(err, "waiting for ExternalPeer to join (manual): start it; Ctrl+C stops the run"),
                "waiting line omits 'with the command above':\n" + err);
        require(!contains(err, "In the directory "), "no bogus local directory printed for omitted command:\n" + err);
        const std::string uri = registry_uri_from_err(err);
        require(!uri.empty(), "registry uri in stderr:\n" + err);
        ByHand peer(join_cmd("ExternalPeer", uri), shown_path(dir));
        const auto [status, out] = live.finish_unchecked();
        require(status && status->code == 0, "CLI run with omitted command passes:\n" + out_of(out));
        require(peer.wait(30s).has_value(), "ExternalPeer ended");
    }

    // 3. Non-existent remote command (manual: true, describe: false) under serve --once.
    {
        const std::string cfg =
            "vehicle_sil_simulation: 2\n"
            "# run-option --stop-at-ms 200\n"
            "registry_uri: silkit://127.0.0.1:0\n"
            "networks: []\n"
            "participants:\n" +
            autoPart +
            "  - name: RemotePeer\n"
            "    command: [remote_only_peer_bin, --name, \"{name}\", --registry-uri, \"{registry_uri}\"]\n"
            "    describe: false\n"
            "    manual: true\n";
        const fs::path dir = hand_dir(o, "ext_remote_serve", cfg, {{"Auto", ""}, {"RemotePeer", ""}});
        Live live(o, dir, {"serve", "--once", "sim.yaml"});
        const std::string reg = live.wait_for({"\"event\":\"registry\""});
        const std::string ev = live.wait_for({"\"name\":\"RemotePeer\"", "\"state\":\"manual\""});
        const std::string uri = json_text(reg, "uri");
        require(contains(ev, "\"working_directory\":null"), "remote command has null working_directory:\n" + ev);
        require(contains(ev, "\"command\":[\"remote_only_peer_bin\",\"--name\",\"RemotePeer\",\"--registry-uri\",\"" + uri + "\"]"),
                "remote command keeps verbatim executable and expanded placeholders:\n" + ev);
        require(contains(ev, "\"command_line\":\"remote_only_peer_bin --name RemotePeer --registry-uri " + uri + "\""),
                "remote command_line has no bogus cwd prefix:\n" + ev);
        ByHand peer(join_cmd("RemotePeer", uri), shown_path(dir));
        const auto [status, lines] = live.finish();
        require(status && status->code == 0, "serve --once with remote command passes:\n" + out_of(lines));
        require(peer.wait(30s).has_value(), "RemotePeer ended");
    }

    // 4. --manual override on describe: false with non-existent remote command passes run,
    //    while without --manual it fails strictly with [executable].
    {
        const std::string cfg =
            "vehicle_sil_simulation: 2\n"
            "# run-option --stop-at-ms 200\n"
            "registry_uri: silkit://127.0.0.1:0\n"
            "networks: []\n"
            "participants:\n" +
            autoPart +
            "  - name: RemotePeer\n"
            "    command: [remote_only_peer_bin, --name, \"{name}\", --registry-uri, \"{registry_uri}\"]\n"
            "    describe: false\n";
        const fs::path dir = hand_dir(o, "ext_override", cfg, {{"Auto", ""}, {"RemotePeer", ""}});
        const Captured failedCheck = launch(o, {"check", "sim.yaml"}, dir);
        expect_code(failedCheck, 1, "describe: false without manual fails on missing executable");
        require(contains(failedCheck.standard_output, "[executable]"), failedCheck.standard_output);
        const Captured failedRun = launch(o, {"run", "sim.yaml"}, dir);
        expect_code(failedRun, 1, "run without manual fails on missing executable");
        require(contains(failedRun.standard_output, "[executable]"), failedRun.standard_output);

        const Captured passedCheck = launch(o, {"check", "--manual", "RemotePeer", "sim.yaml"}, dir);
        expect_code(passedCheck, 0, "check --manual passes for describe: false with remote command");

        Live live(o, dir, {"run", "--manual", "RemotePeer", "sim.yaml"});
        const std::string err = live.wait_for_error([](const std::string& text) {
            return contains(text, "[vehicle_sil_launch] waiting for RemotePeer to join (manual):");
        });
        require(contains(err, "(executable not found on this computer)"), "stderr explains remote executable:\n" + err);
        require(!contains(err, "In the directory "), "no local working directory for unresolved remote command:\n" + err);
        const std::string uri = registry_uri_from_err(err);
        ByHand peer(join_cmd("RemotePeer", uri), shown_path(dir));
        const auto [status, out] = live.finish_unchecked();
        require(status && status->code == 0, "run --manual with remote command passes:\n" + out_of(out));
        require(peer.wait(30s).has_value(), "RemotePeer ended");
    }

    // 5. manual: true + describe: true with missing executable still fails strictly.
    {
        const std::string cfg =
            "vehicle_sil_simulation: 2\n"
            "networks: []\n"
            "participants:\n" +
            autoPart +
            "  - name: MissingEcu\n"
            "    command: [missing_ecu_bin]\n"
            "    describe: true\n"
            "    manual: true\n";
        const fs::path dir = scenario_dir(o, cfg);
        const Captured failed = launch(o, {"check", "sim.yaml"}, dir);
        expect_code(failed, 1, "manual: true with describe: true still fails on missing executable");
        require(contains(failed.standard_output, "[executable]"), failed.standard_output);
    }
}
