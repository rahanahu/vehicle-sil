// SPDX-License-Identifier: MIT
//
// The launch_run scenarios about the simulation time: the speed, a pause and a step, late steps,
// a step that stands still:
//
//   realtime     speed 1 (the file), --speed 2, max and 0.5: the wall-clock time it takes at least, the
//                timing in the result and the line of the time in the text
//   late         an ECU whose steps take longer than real time: the late steps are counted, a warning
//                (not about a participant), --strict fails, the time events count them
//   pause_step   --start-paused and the time commands: step (rounded up to the controller's steps),
//                resume, a refused step, speed, pause, stop; it stands still where it paused
//   run_until_stdin run_until on stdin: runs to the time (rounded up), stands still there, a line without a time
//                is refused
//   state_on_arrival a step and a run_until that the simulation reaches: the look that finds it standing
//                still writes the time event and then the state that says paused, before the next command
//   paused_failures while it stands still: a process that ends fails the run, --timeout-ms aborts it,
//                an interrupt stops it (the last state says paused false each time),
//                start_timeout_ms is not counted
//   time_usage   --start-paused without --stdin-commands, a wrong --speed, the options for check, and
//                wrong time keys in the file
//   stand_still  an ECU that stands still in a step for 11 s: the launcher says that time does not
//                advance, and when it does again
#include "launch_run_support.hpp"

/// One test ECU, Engine (with `behaviour`), and the keys `keys` before networks.
fs::path one_ecu(const Options& o, const std::string& keys, const std::string& behaviour = "")
{
    return scenario_dir(o,
                        "vehicle_sil_simulation: 1\n" + keys +
                            "networks: []\n"
                            "participants:\n"
                            "  - name: Engine\n"
                            "    command: [" + ecu(o) + "]\n",
                        {{"Engine", behaviour}});
}

/// The value of `key` in the pretty JSON of --json (the first), as text.
std::string json_value(const std::string& json, const std::string& key)
{
    const std::string head = "\"" + key + "\": ";
    const auto at = json.find(head);
    require(at != std::string::npos, "no " + key + " in:\n" + json);
    const auto from = at + head.size();
    return json.substr(from, json.find_first_of(",\n", from) - from);
}

double json_number(const std::string& json, const std::string& key) { return std::stod(json_value(json, key)); }

void realtime(const Options& o)
{
    // Speed 1 (from the file): 1000 ms of virtual time take at least 1 s of wall-clock time. A
    // tolerance of 1 s: nothing that runs is that late (the test ECU's step takes no time).
    const fs::path dir = one_ecu(o, "stop_at_ms: 1000\nspeed: 1\nlate_tolerance_ms: 1000\n");
    Captured run = launch(o, {"run", "--json", "sim.yaml"}, dir);
    expect_code(run, 0, "run at speed 1");
    const std::string& json = run.standard_output;
    require(json_value(json, "speed") == "1" && json_value(json, "sim_ms") == "1000" &&
                json_value(json, "time_step_ms") == "10" && json_value(json, "ecu_step_ms") == "10" &&
                json_value(json, "step_delay_ms") == "0" && json_value(json, "late_tolerance_ms") == "1000",
            "the timing of speed 1:\n" + json);
    // From the step at 0 to the step at 1000 ms: never shorter than 1000 ms (only a lower bound).
    require(json_number(json, "wall_ms") >= 999 && json_number(json, "actual_speed") <= 1.002,
            "at speed 1, 1000 ms of virtual time take at least 1000 ms:\n" + json);
    // The steps at 10 .. 1000 ms are paced (the one at 0 is the base), the one at stop_at_ms too.
    require(json_value(json, "paced_steps") == "100" && json_value(json, "late_steps") == "0" &&
                json_value(json, "slipped_ms") == "0" && json_value(json, "late_ms") == "0" &&
                json_value(json, "paused_ms") == "0" && json_number(json, "max_late_ms") <= 1000,
            "nothing late, nothing paused:\n" + json);
    require(contains(json, "  \"findings\": [],\n"), "no finding:\n" + json);

    // --speed replaces the file's speed: twice as fast takes at least half as long.
    run = launch(o, {"run", "--json", "--speed", "2", "sim.yaml"}, dir);
    expect_code(run, 0, "run --speed 2");
    require(json_value(run.standard_output, "speed") == "2" && json_number(run.standard_output, "wall_ms") >= 499,
            "--speed 2:\n" + run.standard_output);
    // As fast as possible: nothing is paced.
    run = launch(o, {"run", "--json", "--speed", "max", "sim.yaml"}, dir);
    expect_code(run, 0, "run --speed max");
    require(json_value(run.standard_output, "speed") == "\"max\"" && json_value(run.standard_output, "paced_steps") == "0",
            "--speed max:\n" + run.standard_output);
    // The text says how the time went, after the run's line.
    run = launch(o, {"run", "--speed", "0.5", "sim.yaml"}, dir);
    expect_code(run, 0, "run --speed 0.5");
    require(contains(run.standard_output, "run: the simulation ran to stop_at_ms (1000 ms) and ended\ntime: speed 0.5, ran at "),
            "the line of the time:\n" + run.standard_output);
    require(contains(run.standard_output, " times real time; 0 of 100 paced steps more than 1000 ms late (the latest by "),
            "the line of the time:\n" + run.standard_output);
    require(contains(run.standard_output, " ms), 0 ms behind in all\n"), "the line of the time:\n" + run.standard_output);
}

void on_time(const Options& o)
{
    // What a user does first: run --speed 1, nothing else. An ECU that does nothing must not be
    // told as late with the default tolerance (10 ms): the launcher's own timer has to be that
    // good on every system. This is a bound on real time, so a machine that stands still for 10 ms
    // fails a run: three runs, and one without a late step is enough (a timer that is too coarse
    // is late in every run).
    const fs::path dir = one_ecu(o, "stop_at_ms: 1000\n");
    std::string seen;
    bool onTime = false;
    for (int i = 0; i < 3 && !onTime; ++i)
    {
        const Captured run = launch(o, {"run", "--json", "--speed", "1", "sim.yaml"}, dir);
        expect_code(run, 0, "run --speed 1");
        const std::string& json = run.standard_output;
        require(json_value(json, "late_tolerance_ms") == "10" && json_value(json, "paced_steps") == "100" &&
                    json_number(json, "wall_ms") >= 999,
                "speed 1 with the default tolerance:\n" + json);
        const std::string numbers = "late_steps " + json_value(json, "late_steps") + ", max_late_ms " + json_value(json, "max_late_ms") +
                                    ", slipped_ms " + json_value(json, "slipped_ms") + ", wall_ms " + json_value(json, "wall_ms");
        log("run --speed 1 with the default tolerance: " + numbers);
        seen += numbers + "\n";
        onTime = json_value(json, "late_steps") == "0" && contains(json, "  \"findings\": [],\n");
    }
    require(onTime, "an ECU that does nothing is late at speed 1 with the default tolerance, three times:\n" + seen);
}

void late(const Options& o)
{
    // Each 10 ms step of Engine takes 30 ms of wall-clock time: at speed 1 the simulation runs at
    // a third of the speed or less, its steps begin 20 ms or more after their real time, and it
    // does not catch up. Only lower bounds: a slower machine is later. (Not every step is late: an
    // ECU that goes from one step straight into the next tells the others only after it, so the
    // controller sees every other step one step late, and the one after it on time.)
    const fs::path dir = one_ecu(o, "stop_at_ms: 300\nspeed: 1\n", "slow-ms 30");
    const Captured run = launch(o, {"run", "--json", "sim.yaml"}, dir);
    expect_code(run, 0, "a run that falls behind real time (a warning)");
    const std::string& json = run.standard_output;
    require(json_value(json, "paced_steps") == "30" && json_number(json, "late_steps") >= 10 &&
                json_number(json, "max_late_ms") >= 19 && json_number(json, "slipped_ms") >= 550 &&
                json_number(json, "actual_speed") <= 0.35,
            "the steps that came late:\n" + json);
    // Nothing is caught up: the time it took is the time at the speed and what it slipped (the
    // lateness within the tolerance at the end is not in the slip).
    require(json_number(json, "wall_ms") >= 300 + json_number(json, "slipped_ms") - 2, "wall_ms is 300 ms and the slip:\n" + json);
    require(contains(json, "      \"severity\": \"warning\",\n      \"code\": \"behind-real-time\",\n      \"file\": \"sim.yaml\",\n"
                           "      \"line\": null,\n      \"participant\": null,\n"),
            "a warning about no participant (the launcher cannot tell which one is slow):\n" + json);
    const auto messageAt = json.find("\"message\": \"the simulation fell behind");
    require(messageAt != std::string::npos, "the warning's message:\n" + json);
    const std::string message = json.substr(messageAt + 11, json.find('\n', messageAt) - messageAt - 11);
    const std::string expected = "\"the simulation fell behind real time: " + json_value(json, "late_steps") +
                                 " of 30 paced steps of the launcher's controller (10 ms each) began more than 10 ms "
                                 "(late_tolerance_ms) after the real time they stand for, the latest by " +
                                 json_value(json, "max_late_ms") +
                                 " ms. The launcher does not catch up after a late step (the ECUs' periodic messages would "
                                 "leave in a burst): it goes on at the speed from there, so the simulation came " +
                                 json_value(json, "slipped_ms") + " ms behind in all and ran at " + json_value(json, "actual_speed") +
                                 " times real time (pauses not counted); ";
    require(message.rfind(expected, 0) == 0, "the warning:\n" + message + "\nexpected to start with\n" + expected);
    // --strict fails on it; the events have it before the participants are ended, and the time
    // events count the late steps as they come.
    const Captured strict = launch(o, {"run", "--strict", "--events", "sim.yaml"}, dir);
    expect_code(strict, 1, "--strict fails a run that fell behind");
    const auto lines = check_events(strict.standard_output);
    const auto at = [&lines](const std::string& part) {
        return std::find_if(lines.begin(), lines.end(), [&](const std::string& l) { return contains(l, part); }) - lines.begin();
    };
    require(at("\"code\":\"behind-real-time\"") < at("\"phase\":\"stopping\""), "the warning comes before stopping");
    const auto times = events_with(lines, {"\"event\":\"time\""});
    require(times.size() >= 3 && std::stoll(value_of(times.back(), "late_steps")) >= 10 &&
                std::stoll(value_of(times.back(), "max_late_ms")) >= 19 && std::stoll(value_of(times.back(), "slipped_ms")) >= 550,
            "the time events count the late steps:\n" + strict.standard_output);
    // Last (it writes sim.yaml anew): the tolerance is the file's. As late, but within 100 s: no step is
    // late, nothing slips (the lateness adds up instead), nothing is found.
    const fs::path tolerant = one_ecu(o, "stop_at_ms: 300\nspeed: 1\nlate_tolerance_ms: 100000\n", "slow-ms 30");
    const Captured within = launch(o, {"run", "--json", "sim.yaml"}, tolerant);
    expect_code(within, 0, "a run within late_tolerance_ms");
    require(json_value(within.standard_output, "late_steps") == "0" && json_number(within.standard_output, "max_late_ms") >= 200 &&
                json_value(within.standard_output, "slipped_ms") == "0" && contains(within.standard_output, "  \"findings\": [],\n"),
            "late, but within the tolerance:\n" + within.standard_output);
}

void stall(const Options& o)
{
    // One slow interval: Engine's step at 300 ms takes 500 ms. One late step (more on a machine
    // that is slow elsewhere too), and the simulation goes on from there: it does not hurry
    // through the 490 ms it lost, so the run takes them longer.
    {
        const fs::path dir = one_ecu(o, "stop_at_ms: 1000\nspeed: 1\n", "slow-at-ms 300 500");
        const Captured run = launch(o, {"run", "--json", "sim.yaml"}, dir);
        expect_code(run, 0, "one slow interval (a warning)");
        const std::string& json = run.standard_output;
        require(json_value(json, "paced_steps") == "100" && json_number(json, "late_steps") >= 1 &&
                    json_number(json, "max_late_ms") >= 480 && json_number(json, "slipped_ms") >= 480 &&
                    json_number(json, "wall_ms") >= 1000 + json_number(json, "slipped_ms") - 2 && json_value(json, "late_ms") == "0",
                "one slow interval:\n" + json);
        require(contains(json, "\"code\": \"behind-real-time\""), "the warning:\n" + json);
        log("one slow interval of 500 ms: late_steps " + json_value(json, "late_steps") + ", max_late_ms " +
            json_value(json, "max_late_ms") + ", slipped_ms " + json_value(json, "slipped_ms") + ", wall_ms " +
            json_value(json, "wall_ms"));
    }
    // Only the last interval is slow (the step at 990 ms): the controller's step at stop_at_ms,
    // which nothing holds back, is the late one.
    {
        const fs::path dir = one_ecu(o, "stop_at_ms: 1000\nspeed: 1\n", "slow-at-ms 990 500");
        const Captured run = launch(o, {"run", "--json", "sim.yaml"}, dir);
        expect_code(run, 0, "a slow last interval (a warning)");
        const std::string& json = run.standard_output;
        require(json_value(json, "paced_steps") == "100" && json_number(json, "late_steps") >= 1 &&
                    json_number(json, "max_late_ms") >= 480 && json_number(json, "wall_ms") >= 1480 &&
                    contains(json, "\"code\": \"behind-real-time\""),
                "a slow last interval:\n" + json);
    }
    // stop_at_ms at the controller's first or second step after 0: the steps there are, are
    // measured. (A slow step at 0 would not count: the first step is the base, and the ECU may
    // run its own step at 0 before the controller's.)
    {
        fs::path dir = one_ecu(o, "stop_at_ms: 10\nspeed: 1\n");
        Captured run = launch(o, {"run", "--json", "sim.yaml"}, dir);
        expect_code(run, 0, "a run of one step");
        require(json_value(run.standard_output, "paced_steps") == "1" && json_number(run.standard_output, "wall_ms") >= 9,
                "a run of one step:\n" + run.standard_output);
        dir = one_ecu(o, "stop_at_ms: 20\nspeed: 1\n", "slow-at-ms 10 300");
        run = launch(o, {"run", "--json", "sim.yaml"}, dir);
        expect_code(run, 0, "a run of two steps (a warning)");
        const std::string& json = run.standard_output;
        require(json_value(json, "paced_steps") == "2" && json_number(json, "late_steps") >= 1 &&
                    json_number(json, "max_late_ms") >= 280 && json_number(json, "wall_ms") >= 300,
                "a run of two steps:\n" + json);
    }
    if (!can_request_stop())
    {
        log("no console to send Ctrl+Break through: not the run that ends in a stall");
        return;
    }
    // Engine never returns from its step at 500 ms, and --timeout-ms ends the run 3 s after the
    // start: the step at 510 ms never begins. While it lasts, the time events say how late it is
    // by now; the result has the stall as a late step and in the time the run took.
    {
        const fs::path dir = one_ecu(o, "stop_timeout_ms: 300\nspeed: 1\n", "hang-at-ms 500");
        const Captured events = launch(o, {"run", "--events", "--timeout-ms", "3000", "sim.yaml"}, dir);
        expect_code(events, 1, "a run that ends in a stall");
        const auto lines = check_events(events.standard_output);
        const auto stalled = events_with(lines, {"\"event\":\"time\"", "\"sim_ms\":500,"});
        std::int64_t last = 0;
        std::size_t growing = 0;
        for (const auto& line : stalled)
        {
            const std::int64_t late = std::stoll(value_of(line, "late_ms"));
            require(late >= last, "the lateness of a stall only grows:\n" + events.standard_output);
            growing += late > last ? 1 : 0;
            if (late > 10)
            {
                require(std::stoll(value_of(line, "late_steps")) >= 1 && std::stoll(value_of(line, "max_late_ms")) >= late &&
                            std::stoll(value_of(line, "slipped_ms")) >= late,
                        "a step that is overdue is counted while it is:\n" + line);
            }
            last = late;
        }
        require(growing >= 3 && last >= 1500, "the time events show the stall while it lasts:\n" + events.standard_output);
        const std::string& result = lines.back();
        require(value_of(result, "sim_ms") == "500" && std::stoll(value_of(result, "late_ms")) >= 1500 &&
                    std::stoll(value_of(result, "late_steps")) >= 1 && std::stoll(value_of(result, "max_late_ms")) >= 1500 &&
                    std::stoll(value_of(result, "slipped_ms")) >= 1500 && std::stoll(value_of(result, "wall_ms")) >= 2000 &&
                    std::stod(value_of(result, "actual_speed")) <= 0.25,
                "the result of a run that ended in a stall:\n" + result);
        require(!events_with(lines, {"\"code\":\"behind-real-time\"", "when the run ended, the next step was " +
                                                                         value_of(result, "late_ms") + " ms overdue"})
                     .empty(),
                "the warning says that it ended in a stall:\n" + events.standard_output);
        // The time of the result is the gate's: from the first step to the abort. The launcher's
        // own end (the abort going round, the participants' end: 300 ms and more for the one that
        // hangs) is not in it.
        const std::int64_t ended = std::stoll(value_of(lines.back(), "t_ms"));
        require(std::stoll(value_of(result, "wall_ms")) <= ended - 300,
                "wall_ms ends at the abort, not when the participants have ended:\n" + result);
    }
}

void stand_still(const Options& o)
{
    // An ECU that stands still in its step at 100 ms for 11 s (as one that lost its network
    // connection, or hangs): after 10 s the launcher says that virtual time does not advance, and
    // what may cause it; when it moves again, it says so. The run passes.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 300\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Auto\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Auto", "slow-at-ms 100 11000"}});
    const Captured run = launch(o, {"run", "sim.yaml"}, dir);
    expect_code(run, 0, "a run with a step of 11 s");
    require(contains(run.standard_error, "[vehicle_sil_launch] virtual time has not advanced for 10 s (at t=") &&
                contains(run.standard_error, " ms, not paused): a participant may have lost its network connection (on "
                                             "another computer), stand still in a debugger, or hang in a step; the run "
                                             "waits on (a run with --timeout-ms ends when that has passed)\n"),
            "the line after 10 s:\n" + run.standard_error);
    require(contains(run.standard_error, "[vehicle_sil_launch] virtual time advances again: it stood still for 1"),
            "the line when it moves again:\n" + run.standard_error);
    require(!contains(run.standard_error, "has not advanced for 60 s"), "nothing more:\n" + run.standard_error);
}

void run_until_stdin(const Options& o)
{
    // No scenario, no control server: started paused, a run_until runs to its time (rounded up to
    // the controller's 10 ms) and stands still there; the state says the command is taken, and a
    // line without a time is refused with the form of a step's.
    const fs::path dir = one_ecu(o, "", "pid");
    Live live(o, dir, {"run", "--events", "--stdin-commands", "--start-paused", "sim.yaml"});
    live.wait_standing(0);
    live.wait_for({"\"event\":\"state\"", "\"paused\":true,", "\"run_until\":{\"ok\":true,\"reason\":null,"});
    live.write("{\"command\":\"run_until\",\"at_ms\":95,\"id\":5}\n");
    live.wait_for({"\"command\":\"run_until\"", "\"source\":\"stdin\"", "\"accepted\":true,\"message\":null,\"reason\":null,"
                   "\"at_ms\":100,\"id\":5}"});
    live.wait_standing(100);
    std::this_thread::sleep_for(300ms);
    require(events_with(live.lines(), {"\"sim_ms\":110,"}).empty(), "it stands still at 100 ms");
    // A time that has passed while it stands still at 100: accepted, and nothing moves (the at_ms is
    // the time asked for, rounded up, not a time it stops at).
    live.write("{\"command\":\"run_until\",\"at_ms\":50,\"id\":7}\n");
    live.wait_for({"\"command\":\"run_until\"", "\"accepted\":true,\"message\":null,\"reason\":null,"
                   "\"at_ms\":50,\"id\":7}"});
    std::this_thread::sleep_for(300ms);
    require(events_with(live.lines(), {"\"sim_ms\":110,"}).empty(), "a time that has passed moves nothing");
    live.write("{\"command\":\"run_until\",\"id\":6}\n");
    live.wait_for({"\"command\":\"run_until\"", "\"accepted\":false", "\"reason\":\"invalid-argument\",\"id\":6}"});
    live.write("{\"command\":\"stop\"}\n");
    const auto [status, lines] = live.finish();
    const bool gone = all_gone(dir, {"Engine.pid"}, 30s);
    require(status && status->code == 0, "stopped while paused: 0, not " + code_text(status));
    require(gone, "no participant is left");
    // A time that has passed while it runs as fast as it can: the simulation stands still at its next
    // step, not where it was when the command came. The state says paused at once and where only once it
    // stands (paused_at_ms is null until then): no state names a time it does not stand at, and the last
    // time event is of the time the state names. (At a speed, where a step waits for its real time, the
    // simulation stands still at once and the command's own state says where.)
    {
        const fs::path fast = one_ecu(o, "", "pid");
        Live run(o, fast, {"run", "--events", "--stdin-commands", "sim.yaml"});
        run.wait_for_count({"\"event\":\"time\""}, 2);
        run.write("{\"command\":\"run_until\",\"at_ms\":0,\"id\":3}\n");
        const std::vector<std::string> sent{"\"command\":\"run_until\"", "\"accepted\":true,", "\"id\":3}"};
        static_cast<void>(run.wait_for(sent));
        const std::vector<std::string> standing{"\"event\":\"state\"", "\"paused\":true,\"paused_at_ms\":"};
        // (the first state after the command may be the one that does not say where yet)
        std::string stands;
        const auto deadline = std::chrono::steady_clock::now() + scaled_timeout(120s);
        while (stands.empty() || value_of(stands, "paused_at_ms") == "null")
        {
            require(std::chrono::steady_clock::now() < deadline, "no state says where it stands still:\n" + stands);
            std::this_thread::sleep_for(5ms);
            const std::vector<std::string> states = events_with(run.lines(), standing);
            stands = states.empty() ? std::string{} : states.back();
        }
        std::this_thread::sleep_for(300ms);
        run.write("{\"command\":\"stop\"}\n");
        const auto [ended, out] = run.finish();
        require(ended && ended->code == 0, "stopped while paused: 0, not " + code_text(ended));
        const auto command = std::find_if(out.begin(), out.end(), [&](const std::string& l) {
            return std::all_of(sent.begin(), sent.end(), [&](const std::string& p) { return contains(l, p); });
        });
        const std::string where = value_of(stands, "paused_at_ms");
        for (auto line = command; line != out.end(); ++line)
        {
            if (std::all_of(standing.begin(), standing.end(), [&](const std::string& p) { return contains(*line, p); }))
            {
                const std::string at = value_of(*line, "paused_at_ms");
                require(at == "null" || at == where, "a state names a time (" + at + ") that it does not stand at (" + where + "):\n" + *line);
            }
        }
        const std::vector<std::string> times = events_with(out, {"\"event\":\"time\""});
        require(!times.empty() && value_of(times.back(), "sim_ms") == where,
                "the last time event is of the time the state says it stands at (" + where + "):\n" + (times.empty() ? "" : times.back()));
    }
}

void state_on_arrival(const Options& o)
{
    // As fast as possible, started paused. The simulation reaches the end of a step, and the time of a
    // run_until, between two looks of the launcher. The look that finds it standing still writes the
    // time event and, right after it, the state that says paused: a reader that sends its next command
    // once the latest state says paused has both before that command's event.
    const fs::path dir = one_ecu(o, "", "pid");
    Live live(o, dir, {"run", "--events", "--stdin-commands", "--start-paused", "sim.yaml"});
    const std::string stateEvent = "\"event\":\"state\"";
    const auto says_paused = [](const std::string& state) { return contains(state, "\"paused\":true,\"paused_at_ms\":"); };
    // Waits for the time event that stands still at `ms`, then until the latest state says paused.
    const auto stands_at = [&](std::int64_t ms) {
        live.wait_for({"\"event\":\"time\"", "\"sim_ms\":" + std::to_string(ms) + ","});
        const auto deadline = std::chrono::steady_clock::now() + scaled_timeout(120s);
        while (true)
        {
            const std::vector<std::string> states = events_with(live.lines(), {stateEvent});
            if (!states.empty() && says_paused(states.back()))
            {
                return;
            }
            require(std::chrono::steady_clock::now() < deadline,
                    "the state does not say paused although the time event at " + std::to_string(ms) + " ms does");
            std::this_thread::sleep_for(2ms);
        }
    };
    stands_at(0);
    live.write("{\"command\":\"step\",\"ms\":30,\"id\":1}\n");
    stands_at(30);
    live.write("{\"command\":\"run_until\",\"at_ms\":95,\"id\":2}\n");
    stands_at(100);
    live.write("{\"command\":\"resume\",\"id\":3}\n");
    live.wait_for({"\"command\":\"resume\"", "\"accepted\":true,", "\"id\":3}"});
    live.write("{\"command\":\"stop\"}\n");
    const auto [status, lines] = live.finish();
    const bool gone = all_gone(dir, {"Engine.pid"}, 30s);
    require(status && status->code == 0, "stopped by a command: 0, not " + code_text(status));
    require(gone, "no participant is left");
    std::string all;
    for (const auto& line : lines)
    {
        all += line.substr(0, 200) + "\n";
    }
    const auto at = [&lines](const std::vector<std::string>& parts, std::size_t from = 0) {
        for (std::size_t i = from; i < lines.size(); ++i)
        {
            if (std::all_of(parts.begin(), parts.end(), [&](const std::string& p) { return contains(lines[i], p); }))
            {
                return i;
            }
        }
        return lines.size();
    };
    const auto arrival = [&](const std::string& id, std::int64_t ms, const std::string& next) {
        const std::size_t sent = at({"\"event\":\"command\"", "\"accepted\":true,", "\"id\":" + id + "}"});
        const std::size_t time = at({"\"event\":\"time\"", "\"sim_ms\":" + std::to_string(ms) + ","}, sent);
        const std::size_t after = at({"\"event\":\"command\"", "\"id\":" + next + "}"}, sent);
        require(sent < time && time < after && after < lines.size(),
                "command " + id + ", the time event that stands still at " + std::to_string(ms) + " ms, command " + next + ":\n" + all);
        // The states the command and the arrival made (none, if the simulation was there before the
        // command's state was put together: the state before the command said paused, and still does).
        std::vector<std::size_t> states;
        for (std::size_t i = sent; i < after; ++i)
        {
            if (contains(lines[i], stateEvent))
            {
                states.push_back(i);
            }
        }
        if (states.empty())
        {
            return;
        }
        // It goes (the command's state), then it stands still (the look's state, after the look's time
        // event): one change each, so the rev goes up by one.
        require(states.size() == 2 && !says_paused(lines[states[0]]) && states[0] < time && says_paused(lines[states[1]]) &&
                    time < states[1],
                "after command " + id + ": a state that goes, the time event at " + std::to_string(ms) +
                    " ms, a state that says paused:\n" + all);
        require(std::stoll(value_of(lines[states[1]], "rev")) == std::stoll(value_of(lines[states[0]], "rev")) + 1,
                "the state that says paused is the next one (rev) after the state that goes:\n" + all);
    };
    arrival("1", 30, "2");
    arrival("2", 100, "3");
    // Whatever came between: the state before each next command says paused.
    for (const std::string id : {"2", "3"})
    {
        const std::size_t next = at({"\"event\":\"command\"", "\"id\":" + id + "}"});
        std::size_t last = lines.size();
        for (std::size_t i = 0; i < next; ++i)
        {
            if (contains(lines[i], stateEvent))
            {
                last = i;
            }
        }
        require(last < lines.size() && says_paused(lines[last]), "the state before command " + id + " says paused:\n" + all);
    }
}

void speed_change(const Options& o)
{
    // Speed 1 up to 200 ms or a little further, then speed 4: the 800 ms (or a little less) that
    // are left take a quarter of their time, counted from where the change came. Lower bounds:
    // 1000 ms of virtual time never take less than 200 + 800 / 4 ms.
    const fs::path dir = one_ecu(o, "stop_at_ms: 1000\nspeed: 1\nlate_tolerance_ms: 100000\n");
    Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"});
    // The third time event after the one at 0: 200 ms or more have passed (they are 100 ms apart).
    live.wait_for_count({"\"event\":\"time\""}, 4);
    const std::int64_t reached = std::stoll(value_of(events_with(live.lines(), {"\"event\":\"time\""})[3], "sim_ms"));
    live.write("{\"command\":\"speed\",\"value\":4}\n");
    const auto [status, lines] = live.finish();
    require(status && status->code == 0, "a run whose speed changes on the way: 0");
    require(reached >= 200 && reached < 1000, "200 ms or more at speed 1: " + std::to_string(reached));
    // The change came at `reached` or later: the rest takes a quarter of its time at least.
    const std::int64_t least = reached + (1000 - reached) / 4;
    const std::string& result = lines.back();
    require(value_of(result, "sim_ms") == "1000" && contains(result, "\"speed\":4,") &&
                std::stoll(value_of(result, "wall_ms")) >= least - 1 &&
                std::stod(value_of(result, "actual_speed")) <= 1000.0 / static_cast<double>(least - 1) + 0.001 &&
                value_of(result, "paced_steps") == "100" && value_of(result, "slipped_ms") == "0",
            "the time of a run that went on four times as fast from " + std::to_string(reached) + " ms or later:\n" + result);
}

/// A participant with role: bridge in the file: the run is connected outside the simulation, so it
/// keeps real time (speed 1) whatever speed the file has, in test too; --time-policy free and
/// --disable lift that.
void own_bridge(const Options& o)
{
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\nstop_at_ms: 200\nspeed: 4\nnetworks: []\nparticipants:\n"
                                      "  - name: Body\n    command: [" + ecu(o) + "]\n"
                                      "  - name: Gateway\n    command: [" + helper(o) + ", --pid-file, gateway.pid]\n"
                                      "    role: bridge\n    describe: false\n    required: false\n",
                                      {{"Body", "scenario-variables"}});
    const auto ran = [&](std::vector<std::string> args, int code, const std::string& what) {
        fs::remove(dir / "gateway.pid");
        const Captured run = launch(o, args, dir);
        const bool gone = all_gone(dir, {"gateway.pid"}, 30s);
        expect_code(run, code, what);
        require(gone, what + ": Gateway has ended");
        return lf(run.standard_output);
    };
    std::string out = ran({"run", "--json", "sim.yaml"}, 0, "a run with a role: bridge participant");
    require(contains(out, "\"timing\": {\n    \"speed\": 1,\n") && contains(out, "\"code\": \"outside-speed-set\"") &&
                contains(out, "participant 'Gateway' is a bridge (role: bridge) to something outside the simulation") &&
                contains(out, "not at the speed 4 of the file") &&
                contains(out, "\"time_lock\": {\n      \"locked\": true,\n      \"because\": [\n        \"Gateway\"\n      ]\n"),
            "the run went at speed 1, and a note says so:\n" + out);
    out = ran({"run", "--json", "--time-policy", "free", "sim.yaml"}, 0, "the same with --time-policy free");
    require(contains(out, "\"timing\": {\n    \"speed\": 4,\n") && contains(out, "\"code\": \"outside-not-real-time\"") &&
                !contains(out, "outside-speed-set") &&
                contains(out, "\"time_lock\": {\n      \"locked\": false,\n      \"because\": [\n        \"Gateway\"\n      ]\n"),
            "--time-policy free: the file's speed, and a note:\n" + out);
    out = ran({"run", "--json", "--disable", "Gateway", "sim.yaml"}, 0, "the same without the bridge");
    require(contains(out, "\"timing\": {\n    \"speed\": 4,\n") && !contains(out, "\"outside-") &&
                contains(out, "\"time_lock\": {\n      \"locked\": false,\n      \"because\": []\n"),
            "--disable Gateway: nothing connects the run outside:\n" + out);
    fs::remove(dir / "gateway.pid");
    const Captured refused = launch(o, {"run", "--json", "--speed", "2", "sim.yaml"}, dir);
    expect_code(refused, 1, "run --speed 2 with a role: bridge participant");
    require(contains(refused.standard_output, "\"result\": \"check_failed\"") &&
                contains(refused.standard_output, "\"code\": \"outside-real-time\"") &&
                contains(refused.standard_output, "--speed 2 is refused") && !fs::exists(dir / "gateway.pid"),
            "outside-real-time, and nothing started:\n" + refused.standard_output);
    expect_code(launch(o, {"run", "--speed", "1", "sim.yaml"}, dir), 0, "run --speed 1 with a role: bridge participant");
    // test: each run at speed 1 too (it is as fast as possible otherwise), and the command to run
    // one alone is one that run takes.
    fs::create_directories(dir / "scn");
    write_file(dir / "scn" / "a.yaml", "vehicle_sil_scenario: 1\nsteps:\n  - set: { Body.level: 5 }\n"
                                       "  - wait_until: { Body.level: 6 }\n    timeout_ms: 50\n");
    out = ran({"test", "sim.yaml", "--scenarios", "scn"}, 1, "test with a role: bridge participant");
    require(contains(out, "run it alone: vehicle_sil_launch run sim.yaml --scenario scn/a.yaml\n") &&
                contains(out, "note: sim.yaml: 1 of 1 runs went at speed 1 (real time), not as fast as possible: ") &&
                contains(out, "The first of them (scenario scn/a.yaml) says why: participant 'Gateway' is a bridge") &&
                contains(out, "pass --time-policy free if ") && contains(out, " [test-speed-set]\n"),
            "test at speed 1: the run alone takes no --speed max, and a note says why the test is not as fast as possible:\n" + out);
    out = ran({"test", "sim.yaml", "--scenarios", "scn", "--json"}, 1, "test --json with a role: bridge participant");
    require(contains(out, "\"speed\": 1,") && !contains(out, "\"speed\": \"max\",") && contains(out, "\"code\": \"outside-speed-set\"") &&
                contains(out, "not as fast as possible"),
            "test --json at speed 1:\n" + out);
    out = ran({"test", "sim.yaml", "--time-policy", "free", "--scenarios", "scn"}, 1, "test --time-policy free");
    require(contains(out, "run it alone: vehicle_sil_launch run sim.yaml --speed max --time-policy free --scenario scn/a.yaml\n"),
            "test --time-policy free: as fast as possible:\n" + out);
    require(!contains(out, "test-speed-set"), "test --time-policy free: no note about speed 1:\n" + out);
    out = ran({"test", "sim.yaml", "--time-policy", "free", "--scenarios", "scn", "--json"}, 1, "test --json --time-policy free");
    require(contains(out, "\"speed\": \"max\",") && contains(out, "\"code\": \"outside-not-real-time\"") &&
                contains(out, "the simulation runs as fast as possible"),
            "test --json --time-policy free:\n" + out);
}

void step_width(const Options& o)
{
    const auto step_file = [](const fs::path& dir, const std::string& ms) { write_file(dir / "test-ecu-step-ms", ms + "\n"); };
    // An ECU that steps every millisecond. As fast as possible, the controller steps every 10 ms
    // as ever; with a speed, as finely as the ECU, so that every ECU step runs at its real time.
    fs::path dir = one_ecu(o, "stop_at_ms: 200\n");
    step_file(dir, "1");
    Captured run = launch(o, {"run", "--json", "sim.yaml"}, dir);
    expect_code(run, 0, "1 ms steps as fast as possible");
    require(json_value(run.standard_output, "time_step_ms") == "10" && json_value(run.standard_output, "ecu_step_ms") == "1" &&
                json_value(run.standard_output, "step_delay_ms") == "9" && contains(run.standard_output, "  \"findings\": [],\n"),
            "as fast as possible, the controller's step is 10 ms and nothing is found:\n" + run.standard_output);
    run = launch(o, {"run", "--json", "--speed", "2", "sim.yaml"}, dir);
    expect_code(run, 0, "1 ms steps at speed 2");
    require(json_value(run.standard_output, "time_step_ms") == "1" && json_value(run.standard_output, "ecu_step_ms") == "1" &&
                json_value(run.standard_output, "step_delay_ms") == "0" && json_value(run.standard_output, "paced_steps") == "200" &&
                json_number(run.standard_output, "wall_ms") >= 99 && !contains(run.standard_output, "coarse-time-step"),
            "with a speed, the controller steps as finely as the ECU:\n" + run.standard_output);
    // time_step_ms as written stays: 10 ms over 1 ms steps at a speed is a warning that says by
    // how much the ECU's steps run late, and what to write.
    dir = one_ecu(o, "stop_at_ms: 200\ntime_step_ms: 10\n");
    run = launch(o, {"run", "--json", "--speed", "2", "sim.yaml"}, dir);
    expect_code(run, 0, "time_step_ms coarser than the ECU's step (a warning)");
    require(json_value(run.standard_output, "time_step_ms") == "10" && json_value(run.standard_output, "step_delay_ms") == "9" &&
                json_value(run.standard_output, "paced_steps") == "20",
            "time_step_ms as written:\n" + run.standard_output);
    require(contains(run.standard_output,
                     "\"code\": \"coarse-time-step\",\n      \"file\": \"sim.yaml\",\n      \"line\": null,\n"
                     "      \"participant\": null,\n      \"network\": null,\n      \"message\": \"the launcher's controller steps "
                     "every 10 ms "
                     "(time_step_ms), but the ECUs' steps are as small as 1 ms: with a speed, the launcher keeps real time "
                     "only at the controller's steps. An ECU step that starts between two of them runs at the later one, up "
                     "to 9 ms after the real time it stands for (never before it), together with the other steps in between; "
                     "a device attached in real time gets the messages of those steps up to 9 ms late and in a burst. Write "
                     "time_step_ms: 1 (or leave time_step_ms out: with a speed, the default is that)\""),
            "the warning:\n" + run.standard_output);
    expect_code(launch(o, {"run", "--strict", "--speed", "2", "sim.yaml"}, dir), 1, "--strict fails on coarse-time-step");
    expect_code(launch(o, {"run", "--strict", "sim.yaml"}, dir), 0, "as fast as possible, a coarse step is nothing to warn of");
    // With the speed in the file too, check knows it before the run, and run reports it once (as
    // the check's); run --speed max has no speed to warn of.
    dir = one_ecu(o, "stop_at_ms: 200\ntime_step_ms: 10\nspeed: 2\n");
    step_file(dir, "1");
    const auto warnings = [](const std::string& text) {
        std::size_t n = 0;
        for (auto at = text.find("\"code\": \"coarse-time-step\""); at != std::string::npos;
             at = text.find("\"code\": \"coarse-time-step\"", at + 1))
        {
            ++n;
        }
        return n;
    };
    Captured checked = launch(o, {"check", "--json", "sim.yaml"}, dir);
    expect_code(checked, 0, "check of a file with speed and a coarse time_step_ms");
    require(warnings(checked.standard_output) == 1 &&
                contains(checked.standard_output, "but the ECUs' steps are as small as 1 ms: with a speed, the launcher keeps "
                                                  "real time only at the controller's steps."),
            "check warns of the coarse time_step_ms:\n" + checked.standard_output);
    expect_code(launch(o, {"check", "--strict", "sim.yaml"}, dir), 1, "check --strict fails on coarse-time-step");
    run = launch(o, {"run", "--json", "sim.yaml"}, dir);
    expect_code(run, 0, "a run at the file's speed with a coarse time_step_ms");
    require(warnings(run.standard_output) == 1, "run reports coarse-time-step once:\n" + run.standard_output);
    run = launch(o, {"run", "--json", "--speed", "max", "sim.yaml"}, dir);
    expect_code(run, 0, "the same file as fast as possible");
    require(warnings(run.standard_output) == 0, "as fast as possible, nothing to warn of:\n" + run.standard_output);
    expect_code(launch(o, {"run", "--strict", "--speed", "max", "sim.yaml"}, dir), 0,
                "run --strict --speed max of a file with a speed and a coarse time_step_ms");
    // A participant that does not describe itself: with a speed, 1 ms to be safe.
    dir = scenario_dir(o,
                       "vehicle_sil_simulation: 1\nstop_at_ms: 200\nspeed: 4\nnetworks: []\nparticipants:\n"
                       "  - name: Engine\n    command: [" + ecu(o) + "]\n"
                       "  - name: Logger\n    command: [" + helper(o) + ", --pid-file, logger.pid]\n"
                       "    describe: false\n    required: false\n",
                       {{"Engine", ""}});
    step_file(dir, "10");
    run = launch(o, {"run", "--json", "sim.yaml"}, dir);
    const bool gone = all_gone(dir, {"logger.pid"}, 30s);
    expect_code(run, 0, "a participant that does not describe itself, at a speed");
    require(gone, "Logger has ended");
    require(json_value(run.standard_output, "time_step_ms") == "1" && json_value(run.standard_output, "ecu_step_ms") == "null" &&
                json_value(run.standard_output, "step_delay_ms") == "null" && !contains(run.standard_output, "coarse-time-step"),
            "an unknown step: 1 ms\n" + run.standard_output);
    own_bridge(o);
}

/// A run that has ended stands still nowhere, however it ended: its last "state" says `paused`
/// false. Its last "time" event is at `sim_ms` (where it ended).
void require_ended_not_paused(const std::vector<std::string>& lines, std::int64_t sim_ms, const std::string& how)
{
    const auto times = events_with(lines, {"\"event\":\"time\""});
    require(!times.empty() && contains(times.back(), "\"sim_ms\":" + std::to_string(sim_ms) + ","),
            "the last time event of a run that " + how + " while it stood still is at " + std::to_string(sim_ms) + " ms" +
                (times.empty() ? ": there is none" : ":\n" + times.back()));
    const auto states = events_with(lines, {"\"event\":\"state\""});
    require(!states.empty() && contains(states.back(), "\"phase\":\"ended\",\"paused\":false,"),
            "the last state of a run that " + how + " while it stood still says ended and paused false" +
                (states.empty() ? ": there is none" : ":\n" + states.back()));
}

void pause_step(const Options& o)
{
    // No stop_at_ms: it runs until it is stopped. Started paused, stepped, resumed, paced, paused.
    const fs::path dir = one_ecu(o, "", "pid");
    Live live(o, dir, {"run", "--events", "--stdin-commands", "--start-paused", "sim.yaml"});
    live.wait_standing(0);
    // The state says the same, and that a step is taken now.
    live.wait_for({"\"event\":\"state\"", "\"phase\":\"running\",\"paused\":true,\"paused_at_ms\":0,\"holding\":false,\"speed\":\"max\","
                                        "\"pacing\":\"not-paced\",\"stop\":{\"requests\":0,\"by\":null}",
                   "\"step\":{\"ok\":true,\"reason\":null,\"message\":null,\"caution\":null,\"min_ms\":1,"});
    // A step goes the time given, in whole steps of the controller (10 ms), and stands still.
    live.write("{\"command\":\"step\",\"ms\":50}\n");
    // Its event says where it will stand ("at_ms"), before it is there.
    live.wait_for({"\"command\":\"step\"", "\"accepted\":true,\"message\":null,\"reason\":null,\"ms\":50,\"at_ms\":50,\"id\":null}"});
    live.wait_standing(50);
    std::this_thread::sleep_for(300ms);
    require(events_with(live.lines(), {"\"sim_ms\":60,"}).empty(), "it stands still at 50 ms");
    // With the sender's number, which comes back.
    live.write("{\"command\":\"step\",\"ms\":15,\"id\":7}\n");
    live.wait_for({"\"command\":\"step\"", "\"ms\":20,\"at_ms\":70,\"id\":7}"});
    live.wait_standing(70);
    // Two steps at once: the second counts from where the first ends, and says so when it is taken.
    live.write("{\"command\":\"step\",\"ms\":100,\"id\":11}\n{\"command\":\"step\",\"ms\":30,\"id\":12}\n");
    live.wait_for({"\"command\":\"step\"", "\"ms\":100,\"at_ms\":170,\"id\":11}"});
    live.wait_for({"\"command\":\"step\"", "\"ms\":30,\"at_ms\":200,\"id\":12}"});
    live.wait_standing(200);
    // Running: a step is refused; the speed changes.
    live.write("{\"command\":\"resume\"}\n");
    // The state: it goes, and a step is refused with the reason the command would get.
    live.wait_for({"\"event\":\"state\"", "\"paused\":false,", "\"step\":{\"ok\":false,\"reason\":\"not-paused\","});
    // A pause and a step at once, while it runs as fast as it can: the step is taken whether the
    // simulation has paused yet or not, and its "at_ms" is where it stands still after both.
    live.write("{\"command\":\"pause\",\"id\":20}\n{\"command\":\"step\",\"ms\":30,\"id\":21}\n");
    const std::string stepped = live.wait_for({"\"command\":\"step\"", "\"accepted\":true,", "\"ms\":30,\"at_ms\":", "\"id\":21}"});
    const std::int64_t to = std::stoll(value_of(stepped, "at_ms"));
    require(to >= 230 && to % 10 == 0, "the step after a pause ends at a later step of the controller:\n" + stepped);
    live.wait_standing(to);
    std::this_thread::sleep_for(300ms);
    for (const auto& time : events_with(live.lines(), {"\"event\":\"time\""}))
    {
        require(std::stoll(value_of(time, "sim_ms")) <= to, "it stands still at the step's at_ms (" + std::to_string(to) + "):\n" + time);
    }
    live.write("{\"command\":\"step\",\"ms\":10,\"id\":22}\n");
    live.wait_for({"\"command\":\"step\"", "\"ms\":10,\"at_ms\":" + std::to_string(to + 10) + ",\"id\":22}"});
    live.wait_standing(to + 10);
    live.write("{\"command\":\"resume\",\"id\":23}\n");
    live.wait_for({"\"command\":\"resume\"", "\"accepted\":true,", "\"id\":23}"});
    live.write("{\"command\":\"step\",\"ms\":10,\"id\":8}\n{\"command\":\"speed\",\"value\":1}\n");
    live.wait_for({"\"command\":\"step\"", "\"accepted\":false", "\"reason\":\"not-paused\",\"id\":8}"});
    // The speed set, and whether the run keeps up with it (on time, unless this computer is busy).
    const std::string paced = live.wait_for({"\"event\":\"state\"", "\"paused\":false,\"paused_at_ms\":null,\"holding\":false,\"speed\":1,\"pacing\":\""});
    require(contains(paced, "\"pacing\":\"on-time\"") || contains(paced, "\"pacing\":\"late\""),
            "a run that goes at a speed is on time or late:\n" + paced);
    live.write("{\"command\":\"pause\"}\n");
    // The state says that it stands still, and where (the time event of that time may come after it).
    const std::string paused = live.wait_for({"\"event\":\"state\"", "\"paused\":true,\"paused_at_ms\":", "\"holding\":false,\"speed\":1,"
                                                                    "\"pacing\":\"not-paced\","});
    require(value_of(paused, "paused_at_ms") != "null", "the state that says paused at a speed says where:\n" + paused);
    std::this_thread::sleep_for(600ms);
    live.write("{\"command\":\"stop\"}\n");
    // The stop request is in the state at once, by its name in the result.
    live.wait_for({"\"event\":\"state\"", "\"stop\":{\"requests\":1,\"by\":\"stop command\"}"});
    const auto [status, lines] = live.finish();
    const bool gone = all_gone(dir, {"Engine.pid"}, 30s);
    require(status && status->code == 0, "stopped while paused: 0, not " + code_text(status));
    require(gone, "no participant is left");
    require_ended_not_paused(lines, std::stoll(value_of(paused, "paused_at_ms")), "was stopped by a command");
    std::string all;
    for (const auto& line : lines)
    {
        all += line + "\n";
    }
    // It stood still where it paused.
    const std::int64_t where = std::stoll(value_of(paused, "paused_at_ms"));
    for (const auto& time : events_with(lines, {"\"event\":\"time\""}))
    {
        require(std::stoll(value_of(time, "sim_ms")) <= where, "nothing moved after the pause:\n" + all);
    }
    // Each command comes before what it does; the refused step says why.
    const auto at = [&lines](const std::vector<std::string>& parts) {
        return std::find_if(lines.begin(), lines.end(),
                            [&](const std::string& l) { return std::all_of(parts.begin(), parts.end(), [&](const std::string& p) { return contains(l, p); }); }) -
               lines.begin();
    };
    require(at({"\"ms\":50,\"at_ms\":50,\"id\":null}"}) < at({"\"sim_ms\":50,"}), "the step comes before the time it reaches:\n" + all);
    require(!events_with(lines, {"\"command\":\"step\"", "\"accepted\":false,\"message\":\"step works while the simulation is "
                                                         "paused: send {\\\"command\\\":\\\"pause\\\"} first (or run with "
                                                         "--start-paused)\",\"reason\":\"not-paused\",\"id\":8}"})
                 .empty(),
            "a step while it runs is refused:\n" + all);
    // The states: each "rev" higher than the one before; the state a command made comes after
    // the command's event; the last one is the end, where no time command is taken any more.
    const auto states = events_with(lines, {"\"event\":\"state\""});
    for (std::size_t i = 1; i < states.size(); ++i)
    {
        require(std::stoll(value_of(states[i], "rev")) > std::stoll(value_of(states[i - 1], "rev")),
                "rev goes up from state to state:\n" + states[i - 1] + "\n" + states[i]);
    }
    // (A step goes too, and is still taken: the first state that goes is the first step's.)
    require(at({"\"ms\":50,\"at_ms\":50,\"id\":null}"}) < at({"\"event\":\"state\"", "\"phase\":\"running\",\"paused\":false,"}) &&
                at({"\"command\":\"resume\""}) < at({"\"event\":\"state\"", "\"step\":{\"ok\":false,\"reason\":\"not-paused\","}),
            "the step and the resume come before the states they made:\n" + all);
    require(at({"\"command\":\"speed\""}) < at({"\"event\":\"state\"", "\"speed\":1,"}),
            "the speed command comes before the state with that speed:\n" + all);
    require(at({"\"command\":\"stop\""}) < at({"\"event\":\"state\"", "\"stop\":{\"requests\":1,"}),
            "the stop command comes before the state with the request:\n" + all);
    require(!states.empty() &&
                contains(states.back(), "\"phase\":\"ended\",\"paused\":false,\"paused_at_ms\":null,\"holding\":false,\"speed\":1,\"pacing\":\"not-paced\","
                                        "\"stop\":{\"requests\":1,\"by\":\"stop command\"}") &&
                contains(states.back(), "\"pause\":{\"ok\":false,\"reason\":\"not-running\",") &&
                contains(states.back(), "\"stop\":{\"ok\":true,\"reason\":null,"),
            "the last state is the end:\n" + all);
    // This run has no control server: no state says that a write would be taken, and run_until is
    // standard input's, so it is never refused for that.
    for (const std::string& state : states)
    {
        require(contains(state, "\"write\":{\"ok\":false,\"reason\":\"no-control-server\",") &&
                    !contains(state, "\"run_until\":{\"ok\":false,\"reason\":\"no-control-server\","),
                "a run without --control refuses write in every state, and not run_until for that reason:\n" + state);
    }
    // The result: stood still for at least the waits above.
    const std::string& result = lines.back();
    require(std::stoll(value_of(result, "paused_ms")) >= 600 && contains(result, "\"speed\":1,"),
            "the result counts the time it stood still:\n" + result);
}

void paused_failures(const Options& o)
{
    // While it stands still, a participant whose process ends fails the run (as while it runs).
    {
        const fs::path dir = scenario_dir(o,
                                          "vehicle_sil_simulation: 1\n"
                                          "networks: []\n"
                                          "participants:\n"
                                          "  - name: Engine\n"
                                          "    command: [" + ecu(o) + "]\n"
                                          "  - name: Brake\n"
                                          "    command: [" + ecu(o) + "]\n",
                                          {{"Engine", "exit-after-wall-ms 1500 3"}, {"Brake", "pid"}});
        Live live(o, dir, {"run", "--events", "--stdin-commands", "--start-paused", "--timeout-ms", "60000", "sim.yaml"});
        const auto [status, lines] = live.finish();
        const bool gone = all_gone(dir, {"Brake.pid"}, 30s);
        require(status && status->code == 1, "a participant that fails while it stands still fails the run (1)");
        require(contains(lines.back(), "participant 'Engine': it failed while the simulation was running, and the launcher "
                                       "aborted the simulation: its process exited with code 3"),
                "the failure, not the timeout:\n" + lines.back());
        // The launcher learns of it in two ways, and either may be first: its look at the process
        // (process_ended), or the controller, which sees the connection go as the process ends, a
        // moment before the process can be waited for (left_simulation). The result names the
        // exit code in both (checked above).
        const auto failed = events_with(lines, {"\"name\":\"Engine\"", "\"state\":\"failed\""});
        require(failed.size() == 1 && (contains(failed[0], "\"reason\":\"process_ended\"") ||
                                       contains(failed[0], "\"reason\":\"left_simulation\"")),
                "Engine is failed at once, with one event" + (failed.empty() ? std::string() : ":\n" + failed[0]));
        require_ended_not_paused(lines, 0, "was aborted for a failed participant");
        require(gone, "no participant is left");
    }
    // --timeout-ms counts the time it stands still (wall-clock time).
    {
        const fs::path dir = one_ecu(o, "", "pid");
        Live live(o, dir, {"run", "--events", "--stdin-commands", "--start-paused", "--timeout-ms", "1500", "sim.yaml"});
        const auto [status, lines] = live.finish();
        const bool gone = all_gone(dir, {"Engine.pid"}, 30s);
        require(status && status->code == 1 && contains(lines.back(), "\"code\":\"timeout\""),
                "a run that stands still past --timeout-ms is aborted:\n" + lines.back());
        require_ended_not_paused(lines, 0, "was aborted for --timeout-ms");
        require(gone, "no participant is left");
    }
    // An interrupt while it stands still (Windows: Ctrl+Break, if there is a console to send it through).
    if (can_request_stop())
    {
        const fs::path dir = one_ecu(o, "", "pid");
        Live live(o, dir, {"run", "--events", "--stdin-commands", "--start-paused", "sim.yaml"});
        live.wait_standing(0);
#ifdef _WIN32
        static_cast<void>(live.process().request_stop());
#else
        ::kill(static_cast<pid_t>(live.process().id()), SIGINT);
#endif
        const auto [status, lines] = live.finish();
        const bool gone = all_gone(dir, {"Engine.pid"}, 30s);
        require(status && status->code == 130, "a run interrupted while it stands still: 130");
        require_ended_not_paused(lines, 0, "was interrupted");
        require(gone, "no participant is left");
    }
    // start_timeout_ms is for the start: standing still at 0 longer than it is fine.
    {
        const fs::path dir = one_ecu(o, "stop_at_ms: 100\nstart_timeout_ms: 1000\n");
        Live live(o, dir, {"run", "--events", "--stdin-commands", "--start-paused", "sim.yaml"});
        // The event comes once the simulation stands still: the launcher's count of that time has begun.
        // A little longer than the 2000 ms asked for below all the same (a sleep that returns a moment
        // early, a launcher that gave up waiting for the standstill before it wrote the event).
        live.wait_standing(0);
        std::this_thread::sleep_for(2200ms);
        live.write("{\"command\":\"resume\"}\n");
        const auto [status, lines] = live.finish();
        require(status && status->code == 0 && contains(lines.back(), "\"simulation\":\"reached_stop_at\""),
                "a start paused for longer than start_timeout_ms runs to its end:\n" + lines.back());
        require(std::stoll(value_of(lines.back(), "paused_ms")) >= 2000, "the time it stood still:\n" + lines.back());
    }
}

void time_usage(const Options& o)
{
    const fs::path dir = one_ecu(o, "stop_at_ms: 100\n");
    Captured run = launch(o, {"run", "--start-paused", "sim.yaml"}, dir);
    expect_code(run, 4, "--start-paused without --stdin-commands");
    require(contains(run.standard_error, "error: --start-paused needs --stdin-commands or --control: without commands nothing "
                                         "could make the simulation go on (resume, step)\n"),
            run.standard_error);
    // The options of the control server: without --control, and --control in a build without the
    // server (with it: tests/control_server/test_launcher.py).
    run = launch(o, {"run", "--control-credentials", "c.txt", "sim.yaml"}, dir);
    expect_code(run, 4, "--control-credentials without --control");
    require(contains(run.standard_error, "error: --control-credentials is for --control: without it there is no control "
                                         "server\n"),
            run.standard_error);
    run = launch(o, {"check", "--control", "0", "sim.yaml"}, dir);
    expect_code(run, 4, "check --control");
    require(contains(run.standard_error, "error: --control is for run: check starts no participant\n"), run.standard_error);
    const Captured version = launch(o, {"version", "--json"}, dir);
    if (!contains(version.standard_output, "\"control-server\""))
    {
        run = launch(o, {"run", "--control", "0", "sim.yaml"}, dir);
        expect_code(run, 4, "--control without the control server");
        require(contains(run.standard_error, "error: --control needs a vehicle_sil_launch built with the control server: "
                                             "configure with -DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON (it needs gRPC) and build "
                                             "again"),
                run.standard_error);
    }
    run = launch(o, {"run", "--speed", "fast", "sim.yaml"}, dir);
    expect_code(run, 4, "--speed fast");
    require(contains(run.standard_error, "error: --speed must be a number of simulation seconds per wall-clock second from "
                                         "0.000001 to 1000000 (1: real time, 2: twice as fast) or max (as fast as possible), "
                                         "not 'fast'\n"),
            run.standard_error);
    for (const std::string option : {"--speed", "--start-paused"})
    {
        std::vector<std::string> args{"check", option};
        if (option == "--speed")
        {
            args.push_back("1");
        }
        args.push_back("sim.yaml");
        run = launch(o, args, dir);
        expect_code(run, 4, "check " + option);
        require(contains(run.standard_error, "error: " + option + " is for run: check starts no participant\n"), run.standard_error);
    }
    // In the file: an error of the check, nothing is started.
    const fs::path bad = one_ecu(o, "stop_at_ms: 105\ntime_step_ms: 10\nspeed: 0\n");
    run = launch(o, {"run", "sim.yaml"}, bad);
    expect_code(run, 1, "a speed of 0 and a step that does not divide stop_at_ms");
    require(contains(run.standard_output, "error: sim.yaml:3: stop_at_ms (105) is not a multiple of time_step_ms (10)") &&
                contains(run.standard_output, "error: sim.yaml:4: speed must be ") &&
                contains(run.standard_output, "nothing was started"),
            run.standard_output);
}
