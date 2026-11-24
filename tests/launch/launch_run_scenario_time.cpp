// SPDX-License-Identifier: MIT
//
// The launch_run scenarios about the time commands while a scenario plays (run --scenario with
// pause, resume, step, run_until and speed):
//
//   scenario_time_commands_same  the promise that the time commands do not change what a scenario
//                gives. One scenario (writes for 0 ms, a write at a time, a wave with an expect
//                beside it, expects with within_ms and hold_for_ms, a wait_until, a ramp) is run
//                once that nobody touches, and then several times with the five time commands from
//                standard input: paused before the first step (it stands still at 0 with the
//                scenario's first writes), at the very times the scenario writes and decides at,
//                and at times of the wall clock that differ from run to run. Each of those runs must give, byte
//                for byte, the same three things as the untouched one:
//                  - the object "scenario" of the result (every step's status, times and message),
//                  - the scenario_step events in their order (without seq and t_ms),
//                  - the timeline file (--timeline).
//                Every command must have been taken, and the simulation must have stood still
//                before the scenario was over (else the run would prove nothing).
//                This is the reference for "the judgement of a scenario depends on the virtual
//                time alone": a change that makes it fail has made the result depend on when the
//                time stood still or on how fast it went.
//   scenario_paused_timeout  a scenario's run that is paused and left so: --timeout-ms ends it
//                (the pause counts), the run fails for the timeout (exit code 1) and the scenario
//                is incomplete, not failed; the same run without the pause passes
#include "launch_run_support.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

/// What a run of a scenario gave: the three things that must not depend on the time commands.
struct Played
{
    std::string scenario; ///< the object "scenario" of the result event, as it was written
    std::string steps;    ///< the scenario_step events, one a line, without seq and t_ms
    std::string timeline; ///< the timeline file
};

/// A run with --events --stdin-commands --timeline, and what is sent to it.
class Run
{
public:
    Run(const Options& o, const fs::path& dir, const std::string& speed) :
      dir_(dir),
      live_(o, dir, {"run", "--events", "--stdin-commands", "--scenario", "s.yaml", "--timeline", "timeline.json", "--speed", speed,
                     "sim.yaml"})
    {
    }

    Live& live() { return live_; }

    static constexpr std::int64_t kAnywhere = -1;

    /// Sends a command line (`members`: after "command", as JSON members), which must be taken.
    void send(const std::string& name, const std::string& members = "")
    {
        from_ = live_.lines().size();
        ++sent_;
        live_.write("{\"command\":\"" + name + "\"" + (members.empty() ? "" : "," + members) + ",\"id\":" + std::to_string(sent_) + "}\n");
        const std::string answer = live_.wait_for({"\"event\":\"command\"", "\"id\":" + std::to_string(sent_) + "}"});
        require(contains(answer, "\"accepted\":true,"), "the command is taken: " + answer);
    }

    /// Waits until a state after the last command says where the simulation stands still, and
    /// returns that time (ms). `at_ms` (unless kAnywhere): waits for a state that names this time
    /// (a state of the time before a step or a run_until can still be on its way when the command
    /// is sent).
    std::int64_t standing(std::int64_t at_ms = kAnywhere)
    {
        const auto deadline = std::chrono::steady_clock::now() + scaled_timeout(120s);
        std::string last = "none";
        for (;;)
        {
            const std::vector<std::string> lines = live_.lines();
            for (std::size_t i = from_; i < lines.size(); ++i)
            {
                if (contains(lines[i], "\"event\":\"state\"") && contains(lines[i], "\"paused\":true,") &&
                    value_of(lines[i], "paused_at_ms") != "null")
                {
                    last = value_of(lines[i], "paused_at_ms");
                    const std::int64_t at = std::stoll(last);
                    if (at_ms == kAnywhere || at == at_ms)
                    {
                        first_stand_ = first_stand_ < 0 ? at : first_stand_;
                        return at;
                    }
                }
            }
            require(std::chrono::steady_clock::now() < deadline && !live_.process().poll().has_value(),
                    "after command " + std::to_string(sent_) + " no state says that the simulation stands still" +
                        (at_ms != kAnywhere ? " at " + std::to_string(at_ms) + " ms" : std::string()) +
                        " (the last state that says where: " + last +
                        ")");
            std::this_thread::sleep_for(2ms);
        }
    }

    /// Sends a step or a run_until, and waits until the simulation stands still at `at_ms`.
    void send_and_stand(const std::string& name, const std::string& members, std::int64_t at_ms)
    {
        send(name, members);
        static_cast<void>(standing(at_ms));
    }

    /// Waits for the end of the run, which must pass with every command taken and with the
    /// simulation having stood still before `over_at_ms`.
    Played finish(const std::string& what, std::int64_t over_at_ms)
    {
        const auto [status, lines] = live_.finish();
        require(status && status->code == 0, what + ": the run passes, not " + code_text(status));
        const std::vector<std::string> commands = events_with(lines, {"\"event\":\"command\""});
        require(commands.size() == sent_ && events_with(commands, {"\"accepted\":true,"}).size() == sent_,
                what + ": every one of the " + std::to_string(sent_) + " commands is taken");
        require(sent_ == 0 || (first_stand_ >= 0 && first_stand_ < over_at_ms),
                what + ": the simulation stood still before the scenario was over (at " + std::to_string(first_stand_) + " ms)");
        Played out;
        const std::string& result = lines.back();
        const auto at = result.rfind("\"scenario\":{\"file\":");
        require(contains(result, "\"event\":\"result\"") && at != std::string::npos, what + ": the result has the scenario:\n" + result);
        out.scenario = result.substr(at);
        for (const std::string& line : events_with(lines, {"\"event\":\"scenario_step\""}))
        {
            out.steps += members_of(line) + "\n";
        }
        out.timeline = read_text(dir_ / "timeline.json");
        log("CHECK " + what + ": " + std::to_string(sent_) + " commands taken, first stood still at " + std::to_string(first_stand_) +
            " ms -> ok");
        return out;
    }

private:
    fs::path dir_;
    Live live_;
    std::size_t from_ = 0;
    std::size_t sent_ = 0;
    std::int64_t first_stand_ = -1;
};

void expect_same(const Played& with, const Played& without, const std::string& what)
{
    expect_text(with.scenario, without.scenario, "the scenario of the result, " + what);
    expect_text(with.steps, without.steps, "the scenario_step events, " + what);
    expect_text(with.timeline, without.timeline, "the timeline file, " + what);
}

} // namespace

void scenario_time_commands_same(const Options& o)
{
    // The ECUs step every 10 ms. What the scenario writes and when it decides (ms):
    //   0 the first values; 30 a write; 30..50 an expect within; 70..170 a square wave with an
    //   expect beside it; 170 an expect; 170..220 a wait (the pulse is in the sample of 200, known
    //   at 210, the next step is at 220); 220 a write; 220..240 an expect within; 240..270 a ramp;
    //   270..300 an expect that holds. Over at 310 ms (the step after the last sample it needs).
    const std::int64_t over_at_ms = 310;
    const std::string file = "vehicle_sil_scenario: 1\n"
                             "name: time commands\n"
                             "steps:\n"
                             "  - set: { Body.level: 5, Body.gain: 3, Door.mode: auto }\n"
                             "  - expect: { Body.gain_at_0: 3, Body.level_changed_at_ms: 0, Door.mode_now: auto }\n"
                             "  - at_ms: 30\n"
                             "    set: { Body.level: 6 }\n"
                             "  - expect: { Body.level_changed_at_ms: 30 }\n"
                             "    within_ms: 40\n"
                             "  - after_ms: 20\n"
                             "    wave: { Body.level: { square: { low: 20, high: 30, period_ms: 40 }, for_ms: 100 } }\n"
                             "    expect: { Body.level: { ge: 20, le: 30 } }\n"
                             "  - expect: { Body.level_changed_at_ms: 170, Door.mode_now: auto }\n"
                             "  - wait_until: { Body.pulse: true }\n"
                             "    timeout_ms: 500\n"
                             "  - set: { Body.level: 7 }\n"
                             "  - expect: { Body.level_changed_at_ms: 220 }\n"
                             "    within_ms: 30\n"
                             "  - ramp: { Door.level: { to: 60, over_ms: 30 } }\n"
                             "  - expect: { Door.level: 60, Door.level_changed_at_ms: 270 }\n"
                             "    hold_for_ms: 30\n";
    const fs::path dir = scenario_ecus(o, "", file);

    // The run that nobody touches, as fast as possible.
    Played untouched;
    {
        Run run(o, dir, "max");
        untouched = run.finish("no time command", over_at_ms);
        require(contains(untouched.scenario, "\"status\":\"passed\",\"over_at_ms\":" + std::to_string(over_at_ms) + ",") &&
                    !contains(untouched.scenario, "\"status\":\"failed\"") && !contains(untouched.scenario, "\"status\":\"not_run\""),
                "the scenario passes, and is over at " + std::to_string(over_at_ms) + " ms:\n" + untouched.scenario);
        require(contains(untouched.scenario, "\"start_ms\":170,\"at_ms\":200,\"end_ms\":220,"), "the wait:\n" + untouched.scenario);
        require(contains(untouched.timeline, "\"vehicle_sil_timeline\": 1,") && contains(untouched.timeline, "\"source\": \"sampled\""),
                "the timeline has what the scenario saw:\n" + untouched.timeline);
    }

    // No command below has to come within a time of the wall clock that a slow computer could
    // miss. The first pause of each run is sent before the simulation runs (it is taken then, and
    // the simulation stands still at 0), and from where it stands the simulation is moved on with
    // step and run_until. Where a run is left to go on with a pause to follow at a time of the
    // wall clock, its speed is so low that the scenario is seconds from its end (and lower by the
    // factor of the test timeouts).
    const double scale = static_cast<double>(scaled_timeout(1000ms).count()) / 1000.0;
    const auto slow = [scale](double speed) { return "\"value\":" + std::to_string(speed / scale); };

    // 1. Paused before the first step: the scenario's writes for 0 and its hold of the step at 0
    //    are there together with the pause. Then the simulation is made to stand still at the
    //    very times the scenario writes and decides at, and one step before and after them.
    {
        Run run(o, dir, "0.2");
        run.send("pause");
        std::int64_t now = run.standing();
        require(now == 0, "a pause before the simulation runs: it stands still at 0 ms, not at " + std::to_string(now) + " ms");
        run.send_and_stand("step", "\"ms\":10", 10);
        run.send("speed", "\"value\":2");
        now = 10;
        for (const std::int64_t at : {20, 30, 31, 50, 70, 90, 110, 170, 195, 200, 210, 220, 230, 240, 250, 270, 290, 300})
        {
            if (at > now) // (a run_until to where it stands moves nothing)
            {
                now = (at + 9) / 10 * 10; // a run_until stands still at the first step at or after its time
                run.send_and_stand("run_until", "\"at_ms\":" + std::to_string(at), now);
            }
        }
        run.send("speed", "\"value\":\"max\"");
        run.send("resume");
        expect_same(run.finish("paused before the first step, then standing still at the scenario's own times", over_at_ms),
                    untouched, "paused before the first step, then standing still at the scenario's own times");
    }

    // 2. A pause at a time of the wall clock: from the beginning of the wave (70 ms) the
    //    simulation goes on slowly, and the pause comes `wait_ms` later, in whatever step that is.
    //    Then steps of 10 ms (one step of the ECUs) and of 30 ms, a run_until to a time between
    //    two steps, another speed, a run_until into the wait for the pulse (between its sample and
    //    the step the scenario goes on at), and the rest as fast as possible.
    for (const int wait_ms : {0, 23, 61, 137})
    {
        const std::string what = "a pause " + std::to_string(wait_ms) + " ms into the wave, steps and a run_until";
        Run run(o, dir, "0.2");
        run.send("pause");
        static_cast<void>(run.standing());
        run.send_and_stand("run_until", "\"at_ms\":70", 70);
        run.send("speed", slow(0.02)); // 240 ms to the end: 12 s
        run.send("resume");
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        run.send("pause");
        const std::int64_t at = run.standing();
        require(at >= 70 && at + 60 < 210, what + ": the pause came in the wave, not at " + std::to_string(at) + " ms");
        run.send("speed", "\"value\":1");
        run.send_and_stand("step", "\"ms\":10", at + 10);
        run.send_and_stand("step", "\"ms\":30", at + 40);
        run.send_and_stand("run_until", "\"at_ms\":" + std::to_string(at + 55), at + 60);
        run.send("speed", "\"value\":2");
        run.send_and_stand("run_until", "\"at_ms\":205", 210);
        run.send("speed", "\"value\":\"max\"");
        run.send("resume");
        expect_same(run.finish(what, over_at_ms), untouched, what);
    }

    // 3. Speeds while it goes, and pauses that last for a time of the wall clock (the scenario's
    //    waits and bounds are of the virtual time: a pause is not in them).
    for (const int wait_ms : {11, 97})
    {
        const std::string what = "speeds and pauses of " + std::to_string(wait_ms) + " ms and more";
        Run run(o, dir, "0.2");
        run.send("pause");
        static_cast<void>(run.standing());
        run.send("speed", slow(0.02)); // 310 ms to the end: 15 s
        run.send("resume");
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        run.send("speed", slow(0.03));
        run.send("pause");
        const std::int64_t stood = run.standing();
        run.send("speed", "\"value\":2");
        if (stood < 45)
        {
            run.send_and_stand("run_until", "\"at_ms\":45", 50); // in the expect within 40 ms of 30 ms
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3 * wait_ms));
        // In the wait for the pulse: longer on the wall clock than anything in the scenario allows for.
        run.send_and_stand("run_until", "\"at_ms\":185", 190);
        std::this_thread::sleep_for(std::chrono::milliseconds(300 + wait_ms));
        run.send("speed", "\"value\":4");
        run.send("resume");
        expect_same(run.finish(what, over_at_ms), untouched, what);
    }
}

void scenario_paused_timeout(const Options& o)
{
    const std::string file = "vehicle_sil_scenario: 1\n"
                             "name: left paused\n"
                             "steps:\n"
                             "  - set: { Body.level: 5 }\n"
                             "  - at_ms: 100\n"
                             "    expect: { Body.level: 5 }\n";
    const fs::path dir = scenario_ecus(o, "", file);
    const std::string timeout = std::to_string(scaled_timeout(1200ms).count());
    const std::vector<std::string> args{"run", "--events", "--stdin-commands", "--scenario", "s.yaml", "--timeout-ms", timeout, "sim.yaml"};
    // Without a pause it is over long before the timeout (as fast as possible).
    {
        Live live(o, dir, args);
        const auto [status, lines] = live.finish();
        require(status && status->code == 0, "the scenario passes within --timeout-ms, not " + code_text(status));
        require(contains(lines.back(), "\"scenario\":{\"file\":\"s.yaml\",\"name\":\"left paused\",\"status\":\"passed\","),
                "the scenario passes:\n" + lines.back());
    }
    // Paused and left so: the timeout counts the time it stands still, and ends the run. (The
    // pause is sent before the simulation runs: it stands still at 0, however slow this computer is.)
    Live live(o, dir, args);
    live.write("{\"command\":\"pause\"}\n");
    static_cast<void>(live.wait_for({"\"event\":\"command\"", "\"command\":\"pause\"", "\"accepted\":true,"}));
    static_cast<void>(live.wait_standing(0));
    const auto [status, lines] = live.finish();
    require(status && status->code == 1, "a scenario's run left paused until --timeout-ms: 1, not " + code_text(status));
    const std::string& result = lines.back();
    // The run failed for the timeout; the scenario did not fail, it did not get to its end.
    require(contains(result, "\"result\":\"failed\",\"end\":\"failed\",\"ok\":false,") &&
                contains(result, "\"scenario\":{\"file\":\"s.yaml\",\"name\":\"left paused\",\"status\":\"incomplete\",") &&
                !contains(result.substr(result.rfind("\"scenario\":{\"file\":")), "\"status\":\"failed\""),
            "the result of a run left paused until --timeout-ms:\n" + result);
    require(events_with(lines, {"\"event\":\"finding\"", "\"code\":\"timeout\""}).size() == 1 &&
                events_with(lines, {"\"event\":\"finding\"", "\"code\":\"scenario-incomplete\""}).size() == 1 &&
                events_with(lines, {"\"event\":\"finding\"", "\"code\":\"scenario-failed\""}).empty(),
            "the findings of a run left paused until --timeout-ms:\n" + result);
    log("CHECK a scenario's run left paused until --timeout-ms: the run fails, the scenario is incomplete -> ok");
}
