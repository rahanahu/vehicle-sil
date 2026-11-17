// SPDX-License-Identifier: MIT
//
// The rules of a run's controls (launch::decide(), control_rules.hpp): every row of the table with
// its code and its message, the order of the rows, where the two entries differ, the answer
// without arguments (allowed()) and the server that only shows the run (always_refused()). Then
// the same rules where they are used: what TimeCommands::allowed() says and what apply() does
// agree, in every state of the time commands.

#include "test_harness.hpp"

#include "launch_test_util.hpp"
#include "silkit_test_util.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/launch/config.hpp"
#include "vehicle_sil/launch/control_rules.hpp"
#include "vehicle_sil/launch/run_controls.hpp"
#include "vehicle_sil/launch/time_commands.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using vehicle_sil::launch::test::check_text;

constexpr Operation kAllOperations[] = {Operation::Pause, Operation::Resume, Operation::Step, Operation::RunUntil,
                                        Operation::Speed, Operation::Stop,   Operation::Write, Operation::Unknown};
constexpr Operation kTimeOperations[] = {Operation::Pause, Operation::Resume, Operation::Step, Operation::RunUntil,
                                         Operation::Speed};
constexpr Entry kEntries[] = {Entry::Stdin, Entry::ControlServer};
constexpr ControlStage kStages[] = {ControlStage::SettingUp, ControlStage::Attached, ControlStage::Detached};

ControlSituation situation(ControlStage stage, bool paused = false, bool scenario = false, bool locked = false)
{
    return ControlSituation{stage, paused, scenario, locked};
}

/// A command of `operation` as it comes, with arguments that are in range.
Command command(Operation operation)
{
    Command c;
    c.name = operation == Operation::Unknown ? "jump" : operation_name(operation);
    c.accepted = true;
    if (operation == Operation::Step)
    {
        c.ms = 10;
    }
    else if (operation == Operation::RunUntil)
    {
        c.at_ms = 50;
    }
    else if (operation == Operation::Speed)
    {
        c.speed = 2.0;
    }
    return c;
}

/// decide() for `c`, by its own name.
Decision decide_command(const ControlSituation& s, Entry entry, const Command& c)
{
    const Operation operation = *c.name == "write" ? Operation::Write : command_operation(*c.name);
    return decide(s, entry, operation, *c.name, &c);
}

void check_refused(const Decision& d, const char* reason, const std::string& message)
{
    CHECK(!d.ok);
    check_text(d.reason, reason, "reason");
    check_text(d.message, message, "message");
}

const std::string kScenarioMessage =
    "this run plays a scenario (run --scenario), and until it ends only the scenario writes variables and moves the "
    "time, so that the same scenario always gives the same result; a client can read (ListPeers, GetValues, "
    "GetSnapshot, Watch, GetTime, WaitPaused) and Stop; to write or to move the time from a client, run without "
    "--scenario";

} // namespace

TEST_CASE(control_rules_name_the_operations_and_the_entries)
{
    for (const Operation o : {Operation::Pause, Operation::Resume, Operation::Step, Operation::RunUntil, Operation::Speed,
                              Operation::Stop})
    {
        CHECK(command_operation(operation_name(o)) == o);
    }
    check_text(operation_name(Operation::RunUntil), "run_until");
    check_text(operation_name(Operation::Write), "write");
    // A write is no command of the time commands, and neither is anything else.
    CHECK(command_operation("write") == Operation::Unknown);
    CHECK(command_operation("") == Operation::Unknown);
    CHECK(command_operation("Pause") == Operation::Unknown);
    check_text(entry_name(Entry::Stdin), "stdin");
    check_text(entry_name(Entry::ControlServer), "control_server");
    CHECK(entry_of("control_server") == Entry::ControlServer);
    CHECK(entry_of("stdin") == Entry::Stdin);
    // No other text is an entry: nothing falls to the entry with the fewer rules.
    CHECK(!entry_of("anything else"));
    CHECK(!entry_of("Control_Server"));
    CHECK(!entry_of(""));
}

TEST_CASE(control_rules_take_what_no_row_refuses)
{
    // Attached and paused, no scenario, no lock: every operation there is, from both entries.
    const ControlSituation s = situation(ControlStage::Attached, true);
    for (const Entry entry : kEntries)
    {
        for (const Operation o : {Operation::Pause, Operation::Resume, Operation::Step, Operation::RunUntil,
                                  Operation::Speed, Operation::Stop})
        {
            const Command c = command(o);
            const Decision d = decide_command(s, entry, c);
            CHECK(d.ok);
            CHECK(d.reason.empty());
            CHECK(d.message.empty());
        }
    }
    CHECK(decide(s, Entry::ControlServer, Operation::Write, "write", nullptr).ok);
    // Setting up: what needs no step of the controller is kept for it.
    for (const Operation o : {Operation::Pause, Operation::Resume, Operation::Speed, Operation::Stop})
    {
        const Command c = command(o);
        CHECK(decide_command(situation(ControlStage::SettingUp), Entry::Stdin, c).ok);
        CHECK(decide_command(situation(ControlStage::SettingUp), Entry::ControlServer, c).ok);
    }
    // A speed of max (no value), a step without "ms" (one step), the ends of the ranges.
    Command c = command(Operation::Speed);
    c.speed.reset();
    CHECK(decide_command(s, Entry::Stdin, c).ok);
    for (const double speed : {kMinSpeed, kMaxSpeed})
    {
        c.speed = speed;
        CHECK(decide_command(s, Entry::Stdin, c).ok);
    }
    c = command(Operation::Step);
    c.ms.reset();
    CHECK(decide_command(s, Entry::Stdin, c).ok);
    for (const std::int64_t ms : {std::int64_t{1}, kMaxMillis})
    {
        c.ms = ms;
        CHECK(decide_command(s, Entry::Stdin, c).ok);
    }
    for (const Operation o : {Operation::Pause, Operation::RunUntil})
    {
        c = command(o);
        for (const std::int64_t at : {std::int64_t{0}, kMaxMillis})
        {
            c.at_ms = at;
            CHECK(decide_command(s, Entry::ControlServer, c).ok);
        }
    }
}

TEST_CASE(control_rules_row_scenario_running_refuses_a_control_server_what_changes_the_run)
{
    for (const ControlStage stage : kStages)
    {
        for (const bool paused : {false, true})
        {
            const ControlSituation s = situation(stage, paused, true);
            for (const Operation o : kTimeOperations)
            {
                const Command c = command(o);
                check_refused(decide_command(s, Entry::ControlServer, c), "scenario-running", kScenarioMessage);
                check_refused(decide(s, Entry::ControlServer, o, operation_name(o), nullptr), "scenario-running",
                              kScenarioMessage);
            }
            check_refused(decide(s, Entry::ControlServer, Operation::Write, "write", nullptr), "scenario-running",
                          kScenarioMessage);
        }
    }
    // Not its stop (the run can always be stopped), and nothing of standard input.
    const ControlSituation s = situation(ControlStage::Attached, true, true);
    const Command stop = command(Operation::Stop);
    CHECK(decide_command(s, Entry::ControlServer, stop).ok);
    for (const Operation o : {Operation::Pause, Operation::Resume, Operation::Step, Operation::Speed, Operation::Stop})
    {
        const Command c = command(o);
        CHECK(decide_command(s, Entry::Stdin, c).ok);
    }
    // Without a scenario the row refuses nothing.
    const Command pause = command(Operation::Pause);
    CHECK(decide_command(situation(ControlStage::Attached), Entry::ControlServer, pause).ok);
}

TEST_CASE(control_rules_row_not_running_refuses_everything_once_detached_but_a_stop_of_stdin)
{
    const ControlSituation s = situation(ControlStage::Detached, true);
    for (const Entry entry : kEntries)
    {
        for (const Operation o : kAllOperations)
        {
            if (o == Operation::Write || (o == Operation::Stop && entry == Entry::Stdin))
            {
                continue;
            }
            const Command c = command(o);
            check_refused(decide_command(s, entry, c), "not-running",
                          "the simulation is not running (it is over, or it did not start): " + *c.name +
                              " works from the start of the run until the simulation ends");
        }
    }
    // The difference of the entries: a stop from standard input is a stop request whenever it
    // comes; a control server's is refused once the simulation is over (its result stays).
    const Command stop = command(Operation::Stop);
    CHECK(decide_command(s, Entry::Stdin, stop).ok);
    check_refused(decide_command(s, Entry::ControlServer, stop), "not-running",
                  "the simulation is not running (it is over, or it did not start): stop works from the start of the "
                  "run until the simulation ends");
    // A write does not go through the time commands: the row does not look at it. (Standard
    // input has no write: there the name is refused as any other, as before the end.)
    CHECK(decide(s, Entry::ControlServer, Operation::Write, "write", nullptr).ok);
    check_refused(decide(s, Entry::Stdin, Operation::Write, "write", nullptr), "not-running",
                  "the simulation is not running (it is over, or it did not start): write works from the start of the "
                  "run until the simulation ends");
    // Before a command's arguments: a step out of range is told that the run is over.
    Command step = command(Operation::Step);
    step.ms = 0;
    CHECK(decide_command(s, Entry::Stdin, step).reason == "not-running");
}

TEST_CASE(control_rules_row_not_a_time_command_refuses_other_names)
{
    for (const ControlStage stage : {ControlStage::SettingUp, ControlStage::Attached})
    {
        for (const Entry entry : kEntries)
        {
            const Command c = command(Operation::Unknown);
            check_refused(decide_command(situation(stage, true), entry, c), "not-a-time-command",
                          "'jump' is no time command: the time commands are pause, resume, step, run_until and speed");
        }
        check_refused(decide(situation(stage), Entry::Stdin, Operation::Write, "write", nullptr), "not-a-time-command",
                      "'write' is no time command: the time commands are pause, resume, step, run_until and speed");
    }
    Command none;
    none.name = "";
    check_refused(decide_command(situation(ControlStage::Attached), Entry::Stdin, none), "not-a-time-command",
                  "'' is no time command: the time commands are pause, resume, step, run_until and speed");
}

TEST_CASE(control_rules_row_outside_real_time_refuses_what_moves_the_time_away_from_real_time)
{
    for (const Entry entry : kEntries)
    {
        for (const ControlStage stage : {ControlStage::SettingUp, ControlStage::Attached})
        {
            const ControlSituation s = situation(stage, true, false, true);
            for (const Operation o : {Operation::Pause, Operation::Step, Operation::RunUntil, Operation::Speed})
            {
                const Command c = command(o); // the speed: 2
                check_refused(decide_command(s, entry, c), "outside-real-time",
                              "this run keeps to real time, because something connected to it does not wait for the "
                              "virtual time: " +
                                  *c.name + " is refused (a resume, a speed of 1 and a stop are taken)");
            }
            // A speed of max is refused, a speed of 1 is taken; so are a resume and a stop.
            Command speed = command(Operation::Speed);
            speed.speed.reset();
            CHECK(decide_command(s, entry, speed).reason == "outside-real-time");
            speed.speed = 1.0;
            CHECK(decide_command(s, entry, speed).ok);
            const Command resume = command(Operation::Resume);
            CHECK(decide_command(s, entry, resume).ok);
            const Command stop = command(Operation::Stop);
            CHECK(decide_command(s, entry, stop).ok);
        }
        // The lock is not about writes.
        CHECK(decide(situation(ControlStage::Attached, false, false, true), Entry::ControlServer, Operation::Write, "write",
                     nullptr)
                  .ok);
        // Before the rows that tell how to get the operation (there is no way while locked), and
        // before the ranges; after the end of the run and after a control server's scenario.
        Command step = command(Operation::Step);
        CHECK(decide_command(situation(ControlStage::SettingUp, false, false, true), entry, step).reason == "outside-real-time");
        step.ms = 0;
        CHECK(decide_command(situation(ControlStage::Attached, true, false, true), entry, step).reason == "outside-real-time");
        CHECK(decide_command(situation(ControlStage::Detached, true, false, true), entry, step).reason == "not-running");
    }
    const Command pause = command(Operation::Pause);
    CHECK(decide_command(situation(ControlStage::Attached, false, true, true), Entry::ControlServer, pause).reason ==
          "scenario-running");
    CHECK(decide_command(situation(ControlStage::Attached, false, true, true), Entry::Stdin, pause).reason ==
          "outside-real-time");
    // Unlocked, the row refuses nothing: the same commands are taken.
    CHECK(decide_command(situation(ControlStage::Attached, true), Entry::Stdin, pause).ok);
}

TEST_CASE(control_rules_rows_of_the_arguments_refuse_what_is_out_of_range_or_missing)
{
    const ControlSituation s = situation(ControlStage::Attached, true);
    for (const Entry entry : kEntries)
    {
        Command speed = command(Operation::Speed);
        for (const double bad : {0.0, -1.0, 1e-7, 1.0000001e6, std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity()})
        {
            speed.speed = bad;
            check_refused(decide_command(s, entry, speed), "out-of-range",
                          "the speed must be from 0.000001 to 1000000 simulation seconds per wall-clock second, or max");
        }
        Command step = command(Operation::Step);
        for (const std::int64_t bad : {std::int64_t{0}, std::int64_t{-5}, kMaxMillis + 1})
        {
            step.ms = bad;
            check_refused(decide_command(s, entry, step), "out-of-range", "step needs a time from 1 to 1000000000000 ms");
        }
        Command until = command(Operation::RunUntil);
        until.at_ms.reset();
        check_refused(decide_command(s, entry, until), "missing-argument", "run_until needs the time to stand still at");
        for (const Operation o : {Operation::Pause, Operation::RunUntil})
        {
            Command at = command(o);
            for (const std::int64_t bad : {std::int64_t{-1}, kMaxMillis + 1})
            {
                at.at_ms = bad;
                check_refused(decide_command(s, entry, at), "out-of-range",
                              *at.name + " at a time needs a time from 0 to 1000000000000 ms");
            }
        }
        // An "at_ms" on another command is not looked at.
        Command resume = command(Operation::Resume);
        resume.at_ms = -1;
        CHECK(decide_command(s, entry, resume).ok);
        // Before the rows of the state: a step out of range is told so also while not paused
        // and while setting up.
        step.ms = 0;
        CHECK(decide_command(situation(ControlStage::Attached, false), entry, step).reason == "out-of-range");
        CHECK(decide_command(situation(ControlStage::SettingUp, true), entry, step).reason == "out-of-range");
        until.at_ms = -1;
        CHECK(decide_command(situation(ControlStage::SettingUp), entry, until).reason == "out-of-range");
    }
}

TEST_CASE(control_rules_rows_not_set_up_and_not_paused_refuse_a_step_or_a_time_too_early)
{
    for (const Entry entry : kEntries)
    {
        // Setting up (paused or not): a step, a pause at a time, a run_until.
        for (const bool paused : {false, true})
        {
            const ControlSituation s = situation(ControlStage::SettingUp, paused);
            const Command step = command(Operation::Step);
            check_refused(decide_command(s, entry, step), "not-set-up",
                          "the simulation has not been set up yet: step works once it stands still (a time event with "
                          "paused true), with --start-paused from the start");
            for (const Operation o : {Operation::Pause, Operation::RunUntil})
            {
                Command at = command(o);
                at.at_ms = 50;
                check_refused(decide_command(s, entry, at), "not-set-up",
                              "the simulation has not been set up yet: " + *at.name +
                                  " at a time works once the participants have been started");
            }
            // A pause without a time is kept for the controller.
            const Command pause = command(Operation::Pause);
            CHECK(decide_command(s, entry, pause).ok);
        }
        // Attached and not paused: only the step.
        const ControlSituation s = situation(ControlStage::Attached, false);
        const Command step = command(Operation::Step);
        check_refused(decide_command(s, entry, step), "not-paused",
                      "step works while the simulation is paused: send {\"command\":\"pause\"} first (or run with "
                      "--start-paused)");
        const Command until = command(Operation::RunUntil);
        CHECK(decide_command(s, entry, until).ok);
    }
}

TEST_CASE(control_rules_allowed_answers_without_arguments_and_gives_the_ranges)
{
    const ControlSituation paused = situation(ControlStage::Attached, true);
    for (const Entry entry : kEntries)
    {
        const Allowed step = allowed(paused, entry, Operation::Step);
        CHECK(step.ok);
        CHECK(step.reason.empty());
        CHECK(step.message.empty());
        CHECK(step.range.has_value());
        CHECK_EQ(step.range->min, 1.0);
        CHECK_EQ(step.range->max, static_cast<double>(kMaxMillis));
        const Allowed speed = allowed(paused, entry, Operation::Speed);
        CHECK(speed.ok);
        CHECK(speed.range.has_value());
        CHECK_EQ(speed.range->min, kMinSpeed);
        CHECK_EQ(speed.range->max, kMaxSpeed);
        for (const Operation o : {Operation::Pause, Operation::Resume, Operation::RunUntil, Operation::Stop})
        {
            const Allowed a = allowed(paused, entry, o);
            CHECK(a.ok);
            CHECK(!a.range);
        }
        // Refused: the code and the message of the row, by the operation's name; the range all
        // the same (what the argument would have to be), and no caution anywhere yet.
        const Allowed early = allowed(situation(ControlStage::Attached, false), entry, Operation::Step);
        CHECK(!early.ok);
        check_text(early.reason, "not-paused");
        check_text(early.message, "step works while the simulation is paused: send {\"command\":\"pause\"} first (or run "
                                  "with --start-paused)");
        CHECK(early.range.has_value());
        CHECK_EQ(early.range->min, 1.0);
        CHECK_EQ(early.range->max, static_cast<double>(kMaxMillis));
        CHECK(!early.caution && !step.caution && !speed.caution);
        CHECK(allowed(situation(ControlStage::Detached), entry, Operation::Speed).range.has_value());
        CHECK(!allowed(situation(ControlStage::Detached), entry, Operation::Pause).range);
        // Without arguments a run_until has a time and a pause has none.
        const Allowed until = allowed(situation(ControlStage::SettingUp), entry, Operation::RunUntil);
        check_text(until.reason, "not-set-up");
        check_text(until.message, "the simulation has not been set up yet: run_until at a time works once the participants "
                                  "have been started");
        CHECK(allowed(situation(ControlStage::SettingUp), entry, Operation::Pause).ok);
        check_text(allowed(situation(ControlStage::SettingUp), entry, Operation::Step).reason, "not-set-up");
        // Locked: the speed is taken, at 1 only; what stops the time is not.
        const ControlSituation locked = situation(ControlStage::Attached, true, false, true);
        const Allowed one = allowed(locked, entry, Operation::Speed);
        CHECK(one.ok);
        CHECK(one.range.has_value());
        CHECK_EQ(one.range->min, 1.0);
        CHECK_EQ(one.range->max, 1.0);
        for (const Operation o : {Operation::Pause, Operation::Step, Operation::RunUntil})
        {
            const Allowed a = allowed(locked, entry, o);
            CHECK(!a.ok);
            check_text(a.reason, "outside-real-time");
            CHECK(a.range.has_value() == (o == Operation::Step)); // a step has its range, taken or not
        }
        CHECK(allowed(locked, entry, Operation::Resume).ok);
        CHECK(allowed(locked, entry, Operation::Stop).ok);
    }
    // The entries: a write, a scenario, a stop once detached.
    CHECK(allowed(paused, Entry::ControlServer, Operation::Write).ok);
    check_text(allowed(paused, Entry::Stdin, Operation::Write).reason, "not-a-time-command");
    const ControlSituation playing = situation(ControlStage::Attached, false, true);
    check_text(allowed(playing, Entry::ControlServer, Operation::Write).reason, "scenario-running");
    check_text(allowed(playing, Entry::ControlServer, Operation::Pause).message, kScenarioMessage);
    CHECK(allowed(playing, Entry::Stdin, Operation::Pause).ok);
    CHECK(allowed(playing, Entry::ControlServer, Operation::Stop).ok);
    CHECK(allowed(situation(ControlStage::Detached), Entry::Stdin, Operation::Stop).ok);
    check_text(allowed(situation(ControlStage::Detached), Entry::ControlServer, Operation::Stop).reason, "not-running");
}

TEST_CASE(control_rules_always_refused_is_a_control_server_while_a_scenario_plays)
{
    const std::optional<Decision> only = always_refused(true, Entry::ControlServer);
    CHECK(only.has_value());
    check_refused(*only, "scenario-running", kScenarioMessage);
    // Without a scenario a control server changes the run; standard input always may.
    CHECK(!always_refused(false, Entry::ControlServer));
    CHECK(!always_refused(true, Entry::Stdin));
    CHECK(!always_refused(false, Entry::Stdin));

    // The same from the controls of a run, whenever it is asked.
    StopRequests stops;
    RunControls playing(stops, false, RunControls::Observers{}, true);
    const std::optional<Decision> before = playing.always_refused(Entry::ControlServer);
    CHECK(before.has_value());
    check_refused(*before, "scenario-running", kScenarioMessage);
    CHECK(!playing.always_refused(Entry::Stdin));
    playing.time().detach();
    const std::optional<Decision> after = playing.always_refused(Entry::ControlServer);
    CHECK(after.has_value());
    check_text(after->reason, "scenario-running");
    RunControls free(stops);
    CHECK(!free.always_refused(Entry::ControlServer));
    free.time().detach(); // everything is refused now, but not for the whole run
    CHECK(!free.always_refused(Entry::ControlServer));
}

TEST_CASE(control_rules_allowed_and_apply_agree_in_every_state_of_the_time_commands)
{
    vehicle_sil::unit::InProcessRegistry registry;
    SimulationControllerOptions options;
    options.registry_uri = registry.uri;
    options.required_participants = {"NeverJoins"};
    options.time_control = TimeControlOptions{.step = std::chrono::milliseconds(10)};
    SimulationController controller(options);

    enum class State
    {
        Before,       ///< before the controller is attached
        BeforePaused, ///< the same, started paused
        Running,      ///< attached, not paused
        Paused,       ///< attached, paused
        Holding,      ///< attached, in the hold before the first step
        After,        ///< detached
    };
    // What each state is to the rules.
    const auto expected = [](State state, bool scenario, bool locked) {
        const ControlStage stage = state == State::Before || state == State::BeforePaused ? ControlStage::SettingUp
                                   : state == State::After                                 ? ControlStage::Detached
                                                                                           : ControlStage::Attached;
        const bool paused = state == State::BeforePaused || state == State::Paused || state == State::Holding;
        return ControlSituation{stage, paused, scenario, locked};
    };
    int refused = 0;
    int taken = 0;
    for (const State state : {State::Before, State::BeforePaused, State::Running, State::Paused, State::Holding, State::After})
    {
        for (const bool scenario : {false, true})
        {
            for (const bool locked : {false, true})
            {
                for (const Entry entry : kEntries)
                {
                    for (const Operation o : {Operation::Pause, Operation::Resume, Operation::Step, Operation::RunUntil,
                                              Operation::Speed, Operation::Stop})
                    {
                        // Each question on time commands of its own: an accepted command changes them.
                        const bool start_paused = state == State::BeforePaused || state == State::Holding;
                        StopRequests stops;
                        RunControls controls(stops, start_paused, RunControls::Observers{}, scenario);
                        TimeCommands& commands = controls.time();
                        if (locked)
                        {
                            controls.set_time_lock(true);
                        }
                        std::thread window;
                        if (state != State::Before && state != State::BeforePaused)
                        {
                            commands.attach(controller, 10, std::nullopt);
                        }
                        if (state == State::Paused)
                        {
                            Command pause = command(Operation::Pause);
                            // A pause is refused under the lock: this state is then one started paused.
                            if (!commands.apply(pause, "stdin").accepted)
                            {
                                continue;
                            }
                        }
                        if (state == State::Holding)
                        {
                            std::atomic<bool> began{false};
                            window = std::thread([&] {
                                static_cast<void>(commands.hold_before_start([] { return false; }, std::chrono::milliseconds(5),
                                                                             [&] { began = true; }));
                            });
                            while (!began.load())
                            {
                                std::this_thread::yield();
                            }
                        }
                        if (state == State::After)
                        {
                            commands.detach();
                        }
                        const Allowed asked = controls.allowed(o, entry);
                        const Allowed by_the_rules = allowed(expected(state, scenario, locked), entry, o);
                        CHECK_EQ(asked.ok, by_the_rules.ok);
                        check_text(asked.reason, by_the_rules.reason, "reason");
                        check_text(asked.message, by_the_rules.message, "message");
                        CHECK_EQ(asked.range.has_value(), by_the_rules.range.has_value());
                        // A speed of 1: the one that is in range under the lock too.
                        Command c = command(o);
                        if (o == Operation::Speed)
                        {
                            c.speed = 1.0;
                        }
                        const Command done = commands.apply(c, entry_name(entry));
                        CHECK_EQ(done.accepted, asked.ok);
                        check_text(done.message, asked.message, "the refusal");
                        ++(done.accepted ? taken : refused);
                        commands.detach();
                        if (window.joinable())
                        {
                            window.join();
                        }
                    }
                }
            }
        }
    }
    CHECK(taken > 0);
    CHECK(refused > 0);
}

TEST_CASE(time_commands_take_a_stop_of_stdin_after_the_run_and_refuse_under_the_time_lock)
{
    // A stop from standard input: taken once detached too (the row of the entries; the launcher's
    // reader does not send its stop through apply(), and would get the same answer).
    TimeCommands over;
    over.detach();
    const Command stop = command(Operation::Stop);
    CHECK(over.apply(stop, "stdin").accepted);
    CHECK(!over.apply(stop, "control_server").accepted);

    // The time lock: false until it is given; then what moves the time away from real time is
    // refused with its event, and nothing of it is kept for the controller.
    std::vector<std::string> events;
    TimeCommands commands(false, [&events](const Command& c, std::string_view source) {
        events.push_back(*c.name + " " + std::string(source) + (c.accepted ? " accepted" : " refused: " + c.message));
    });
    const Command pause = command(Operation::Pause);
    CHECK(commands.allowed(Operation::Pause, Entry::Stdin).ok);
    commands.set_time_lock(true);
    const Allowed asked = commands.allowed(Operation::Pause, Entry::Stdin);
    CHECK(!asked.ok);
    check_text(asked.reason, "outside-real-time");
    const Command done = commands.apply(pause, "stdin");
    CHECK(!done.accepted);
    check_text(done.message, asked.message);
    Command speed = command(Operation::Speed);
    CHECK(!commands.apply(speed, "control_server").accepted);
    speed.speed = 1.0;
    CHECK(commands.apply(speed, "control_server").accepted);
    CHECK(events.size() == 3);
    check_text(events[0], "pause stdin refused: " + asked.message);
    check_text(events[2], "speed control_server accepted");
    CHECK(!commands.first_time_change()); // nothing changed how the time goes
    CHECK(commands.hold_before_start([] { return true; }, std::chrono::milliseconds(1)) == TimeCommands::Hold::None);
}
