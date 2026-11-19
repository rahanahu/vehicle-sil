// SPDX-License-Identifier: MIT
//
// The controls of a run (RunControls) without the launcher: the phase they keep and tell, the
// stop requests they lend, the state they put together and the pacing of the run. The controls
// with the launcher's event stream, and the controls that a run makes for itself, are in
// tests/launch/test_run_controls.cpp.

#include "run_controls_support.hpp"
#include "test_harness.hpp"
#include "silkit_test_util.hpp"

#include "run_pacing.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"

#include "vehicle_sil/runner/run_controls.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <thread>
#include <vector>


using namespace std::chrono_literals;
using vehicle_sil::runner::Command;
using vehicle_sil::runner::Entry;
using vehicle_sil::runner::Operation;
using vehicle_sil::runner::Pacing;
using vehicle_sil::runner::Phase;
using vehicle_sil::runner::RunControls;
using vehicle_sil::runner::RunState;
using vehicle_sil::runner::StopRequests;
using vehicle_sil::runner::TimeCommands;
using namespace vehicle_sil::runner::test::run_controls_support;

TEST_CASE(run_controls_lend_the_stop_requests_they_were_made_with)
{
    StopRequests stops;
    RunControls controls(stops);
    CHECK(&controls.stops() == &stops);
    stops.request(2);
    CHECK_EQ(controls.stops().count(), 1);
    controls.stops().request(15);
    CHECK_EQ(stops.count(), 2);
    CHECK_EQ(stops.first(), 2);
}

TEST_CASE(run_controls_keep_the_phase_before_they_tell_it)
{
    StopRequests stops;
    std::vector<Phase> told;
    RunControls* made = nullptr;
    RunControls::Observers observers;
    observers.on_phase = [&](Phase phase) {
        told.push_back(phase);
        CHECK(made->phase() == phase); // phase() has it when the observer hears of it
    };
    RunControls controls(stops, false, std::move(observers), false);
    made = &controls;
    CHECK(controls.phase() == Phase::Checking);
    CHECK(told.empty());
    for (const Phase phase : {Phase::Checking, Phase::Starting, Phase::Waiting, Phase::Running, Phase::Stopping, Phase::Ended})
    {
        controls.set_phase(phase);
        CHECK(controls.phase() == phase);
    }
    CHECK((told == std::vector<Phase>{Phase::Checking, Phase::Starting, Phase::Waiting, Phase::Running, Phase::Stopping,
                                     Phase::Ended}));
}

namespace {

/// What the controls tell, in the order they tell it: "phase <name>", "command <name> <accepted or
/// reason>", "state <phase> <allowed.pause: ok or reason>".
struct Sequence
{
    std::vector<std::string> told;

    RunControls::Observers observers()
    {
        RunControls::Observers out;
        out.on_phase = [this](Phase phase) { told.push_back(std::string("phase ") + vehicle_sil::runner::phase_name(phase)); };
        out.on_command = [this](const Command& c, std::string_view) {
            told.push_back("command " + c.name.value_or("") + " " + (c.accepted ? std::string("accepted") : c.reason));
        };
        out.on_state = [this](const RunState& s) {
            told.push_back(std::string("state ") + vehicle_sil::runner::phase_name(s.phase) + " " +
                           (s.allowed.pause.ok ? std::string("ok") : s.allowed.pause.reason));
        };
        return out;
    }
};

Command named(const std::string& name)
{
    Command c;
    c.name = name;
    c.accepted = true;
    return c;
}

} // namespace

TEST_CASE(the_phase_ended_ends_the_time_commands_before_it_is_told_whatever_came_before)
{
    // The configuration could not be read: no time lock, and the commands still wait.
    Sequence unread;
    StopRequests stops;
    RunControls waiting(stops, false, unread.observers(), false);
    waiting.set_phase(Phase::Checking);
    CHECK(waiting.time().apply(named("pause"), "stdin").pending);
    CHECK(waiting.time().apply(named("resume"), "control_server").pending);
    waiting.set_phase(Phase::Ended);
    CHECK((unread.told == std::vector<std::string>{"phase checking", "state checking ok", "state checking not-running",
                                                   "command pause not-running", "command resume not-running",
                                                   "phase ended", "state ended not-running"}));
    // The check failed: the lock came and decided the commands; the end is the same.
    Sequence failed;
    RunControls decided(stops, false, failed.observers(), false);
    decided.set_phase(Phase::Checking);
    CHECK(decided.time().apply(named("pause"), "stdin").pending);
    decided.set_time_lock({});
    decided.set_phase(Phase::Ended);
    CHECK((failed.told == std::vector<std::string>{"phase checking", "state checking ok", "command pause accepted",
                                                   "state checking ok", // paused now
                                                   "state checking not-running", "phase ended", "state ended not-running"}));
    // After the end, every time command is refused; a second end tells only its phase.
    for (RunControls* controls : {&waiting, &decided})
    {
        const Command late = controls->time().apply(named("pause"), "stdin");
        CHECK(!late.accepted && late.reason == "not-running");
        CHECK(controls->allowed(Operation::Resume, Entry::ControlServer).reason == "not-running");
    }
    const std::size_t before = failed.told.size();
    decided.set_phase(Phase::Ended);
    CHECK((std::vector<std::string>(failed.told.begin() + static_cast<std::ptrdiff_t>(before), failed.told.end()) ==
           std::vector<std::string>{"phase ended"}));
}

TEST_CASE(a_command_that_races_the_end_comes_before_the_phase_or_after_its_state)
{
    // A command applied on another thread while the run enters the phase ended (here: started by
    // the phase's observer, which then takes its time) waits for the phase, its event and its
    // state: the last state still follows the phase event.
    Sequence seq;
    StopRequests stops;
    std::thread racer;
    RunControls::Observers observers = seq.observers();
    RunControls* made = nullptr;
    observers.on_phase = [&seq, &racer, &made](Phase phase) {
        seq.told.push_back(std::string("phase ") + vehicle_sil::runner::phase_name(phase));
        if (phase == Phase::Ended)
        {
            racer = std::thread([&made] { static_cast<void>(made->time().apply(named("pause"), "stdin")); });
            std::this_thread::sleep_for(50ms); // time enough for the racer, if nothing held it back
        }
    };
    RunControls controls(stops, false, std::move(observers), false);
    made = &controls;
    controls.set_time_lock({});
    controls.set_phase(Phase::Checking);
    controls.set_phase(Phase::Ended);
    racer.join();
    CHECK((seq.told == std::vector<std::string>{"phase checking", "state checking ok", "state checking not-running",
                                                "phase ended", "state ended not-running", "command pause not-running"}));
}

TEST_CASE(run_controls_keep_whether_the_run_has_a_control_server)
{
    StopRequests stops;
    RunControls controls(stops);
    CHECK(!controls.control_server());
    controls.set_control_server(true);
    CHECK(controls.control_server());
}

TEST_CASE(phase_names_are_those_of_the_phase_events)
{
    using vehicle_sil::runner::phase_name;
    CHECK(std::string(phase_name(Phase::Checking)) == "checking");
    CHECK(std::string(phase_name(Phase::Starting)) == "starting");
    CHECK(std::string(phase_name(Phase::Waiting)) == "waiting");
    CHECK(std::string(phase_name(Phase::Running)) == "running");
    CHECK(std::string(phase_name(Phase::Stopping)) == "stopping");
    CHECK(std::string(phase_name(Phase::Ended)) == "ended");
}

TEST_CASE(the_state_names_the_first_stop_request_as_it_is_told_to)
{
    Told t;
    t.controls.set_stop_name([] { return std::string("SIGTERM"); });
    t.controls.publish();
    CHECK(t.states.back().stopped_by.empty()); // nothing has asked
    CHECK_EQ(t.states.back().stop_requests, 0);
    t.stops.request(15);
    t.controls.publish();
    CHECK(t.states.back().stopped_by == "SIGTERM");
}

TEST_CASE(the_state_of_a_run_with_a_scenario_refuses_what_only_a_control_server_has)
{
    Told t(false, true);
    t.controls.publish();
    const RunState s = t.states.back();
    CHECK(s.scenario);
    // Standard input's commands are taken, the run_until among them; the write is a control server's.
    CHECK(s.allowed.pause.ok && s.allowed.resume.ok && s.allowed.speed.ok && s.allowed.stop.ok);
    CHECK(!s.allowed.write.ok && s.allowed.write.reason == "scenario-running");
    CHECK(s.allowed.run_until == t.controls.allowed(Operation::RunUntil, Entry::Stdin));
    // A control server's clients get the same answers to the time commands (a write is refused them).
    CHECK(s.allowed.run_until == t.controls.allowed(Operation::RunUntil, Entry::ControlServer));
    CHECK(s.allowed.pause == t.controls.allowed(Operation::Pause, Entry::ControlServer));
    // The same answers as the rules give those entries.
    CHECK(s.allowed.write == t.controls.allowed(Operation::Write, Entry::ControlServer));
    CHECK(s.allowed.pause == t.controls.allowed(Operation::Pause, Entry::Stdin));
}

TEST_CASE(the_state_of_a_run_without_a_control_server_refuses_what_only_a_control_server_has)
{
    // Whether a scenario plays or not, and wherever the run stands: nothing can send a write, and
    // the state says so with one reason. What standard input takes (the run_until too) is as with
    // a control server.
    for (const bool scenario : {false, true})
    {
        Told t(false, scenario, false);
        Told with(false, scenario, true);
        const auto check = [&t, &with] {
            t.controls.publish();
            with.controls.publish();
            const RunState s = t.states.back();
            CHECK(!s.allowed.write.ok);
            CHECK(s.allowed.write.reason == "no-control-server");
            CHECK(s.allowed.write.message.find("write a variable") != std::string::npos);
            CHECK(!s.allowed.write.range && !s.allowed.write.caution);
            CHECK(s.allowed.write.message.find("write a variable") != std::string::npos);
            const RunState& other = with.states.back();
            CHECK(s.allowed.pause == other.allowed.pause && s.allowed.resume == other.allowed.resume &&
                  s.allowed.step == other.allowed.step && s.allowed.run_until == other.allowed.run_until &&
                  s.allowed.speed == other.allowed.speed &&
                  s.allowed.stop == other.allowed.stop);
            // The rules themselves answer as before: the state alone knows that the entry is not there.
            CHECK(other.allowed.write == t.controls.allowed(Operation::Write, Entry::ControlServer));
        };
        check();
        vehicle_sil::unit::InProcessRegistry registry;
        vehicle_sil::SimulationControllerOptions options;
        options.registry_uri = registry.uri;
        options.required_participants = {"NeverJoins"};
        options.time_control = vehicle_sil::TimeControlOptions{.step = std::chrono::milliseconds(10)};
        vehicle_sil::SimulationController controller(options);
        vehicle_sil::unit::InProcessRegistry other_registry;
        options.registry_uri = other_registry.uri;
        vehicle_sil::SimulationController other(options);
        t.controls.time().attach(controller, 10, std::optional<double>(1.0));
        with.controls.time().attach(other, 10, std::optional<double>(1.0));
        t.controls.set_phase(Phase::Running);
        with.controls.set_phase(Phase::Running);
        check();
        t.controls.time().detach();
        with.controls.time().detach();
        check();
    }
    // Controls that were told nothing have none.
    StopRequests stops;
    RunControls plain(stops);
    CHECK(plain.state().allowed.write.reason == "no-control-server");
}

TEST_CASE(the_state_says_the_hold_while_it_lasts)
{
    Told t(true);
    t.controls.publish();
    CHECK(t.states.back().paused); // it will stand still at 0
    CHECK(!t.states.back().paused_at_ms); // and does not yet
    CHECK(!t.states.back().holding);
    std::vector<bool> during;
    int asked = 0;
    const auto give_up = [&] {
        during.push_back(t.controls.state().holding);
        return ++asked >= 2;
    };
    CHECK(t.controls.time().hold_before_start(give_up, 1ms) == TimeCommands::Hold::GaveUp);
    CHECK((during == std::vector<bool>{true, true}));
    // Told when it began and when it ended.
    std::vector<bool> holding;
    for (const RunState& s : t.states)
    {
        holding.push_back(s.holding);
    }
    CHECK((holding == std::vector<bool>{false, true, false}));
    // While it holds it stands still at 0; before and after, it is to stand still and does not yet.
    CHECK(t.states.at(1).paused && t.states.at(1).paused_at_ms == std::optional<std::int64_t>{0});
    CHECK(t.states.at(2).paused && !t.states.at(2).paused_at_ms);
}

TEST_CASE(the_pacing_is_late_until_five_seconds_have_passed_without_a_late_step)
{
    using vehicle_sil::runner::detail::PacingWatch;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point t0{};
    PacingWatch w;
    // Going at a speed, nothing late: on time.
    CHECK(w.look(t0, true, false, 0) == Pacing::OnTime);
    // The step that has not begun is too late: late, and for 5 s after the last such look.
    CHECK(w.look(t0 + 1s, true, true, 0) == Pacing::Late);
    CHECK(w.look(t0 + 2s, true, false, 0) == Pacing::Late);
    CHECK(w.look(t0 + 5999ms, true, false, 0) == Pacing::Late);
    CHECK(w.look(t0 + 6s, true, false, 0) == Pacing::OnTime);
    // A step that was late since the last look: late for 5 s from that look.
    CHECK(w.look(t0 + 10s, true, false, 1) == Pacing::Late);
    CHECK(w.look(t0 + 14s, true, false, 1) == Pacing::Late);
    CHECK(w.look(t0 + 14500ms, true, false, 2) == Pacing::Late); // another: 5 s from here
    CHECK(w.look(t0 + 19s, true, false, 2) == Pacing::Late);
    CHECK(w.look(t0 + 19500ms, true, false, 2) == Pacing::OnTime);
    // Nothing to keep up with (standing still, as fast as possible) forgets what was late; a step
    // counted meanwhile is not held against the time after.
    CHECK(w.look(t0 + 20s, true, true, 2) == Pacing::Late);
    CHECK(w.look(t0 + 21s, false, false, 3) == Pacing::NotPaced);
    CHECK(w.look(t0 + 22s, true, false, 3) == Pacing::OnTime);
    CHECK(std::string(vehicle_sil::runner::pacing_name(Pacing::NotPaced)) == "not-paced");
    CHECK(std::string(vehicle_sil::runner::pacing_name(Pacing::OnTime)) == "on-time");
    CHECK(std::string(vehicle_sil::runner::pacing_name(Pacing::Late)) == "late");
}

TEST_CASE(run_controls_tell_the_state_of_the_bridges_once_for_each_change)
{
    StopRequests stops;
    std::vector<RunState> told;
    RunControls::Observers observers;
    observers.on_state = [&](const RunState& state) { told.push_back(state); };
    RunControls controls(stops, false, std::move(observers), false);
    CHECK(controls.state().outside.empty());
    RunState::Outside bridge;
    bridge.network = "CAN";
    bridge.target = "can0";
    bridge.participant = "bridge.CAN";
    controls.set_outside({bridge});
    CHECK_EQ(told.size(), std::size_t{1});
    CHECK(told.back().outside.size() == 1 && told.back().outside.front() == bridge && told.back().outside.front().condition == "ok");
    // The same again tells nothing; a change does, and state() has it.
    controls.set_outside({bridge});
    controls.publish();
    CHECK_EQ(told.size(), std::size_t{1});
    bridge.condition = "interrupted";
    bridge.reasons = {"bus-off"};
    controls.set_outside({bridge});
    CHECK_EQ(told.size(), std::size_t{2});
    CHECK(told.back().rev == 2 && controls.state().outside.front().reasons == std::vector<std::string>({"bus-off"}));
}
