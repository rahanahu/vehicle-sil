// SPDX-License-Identifier: MIT
//
// The controls of a run (launch::RunControls) by themselves: what they are made of, the phase they
// keep and tell, the state they put together and when they tell it, and the controls a run makes
// for itself when it is given none (run_file() on a file that cannot be read: nothing starts).
// Whether a run keeps up with its speed (PacingWatch), as written. And when a run gives its
// controls the time lock: the time commands that came before run_file() are decided with it.

#include "test_harness.hpp"
#include "silkit_test_util.hpp"

#include "run_pacing.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"

#include "vehicle_sil/launch/events.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/launch/run_controls.hpp"
#include "vehicle_sil/launch/test.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::launch::Command;
using vehicle_sil::launch::EventStream;
using vehicle_sil::launch::EventStreamOptions;
using vehicle_sil::launch::EventWrite;
using vehicle_sil::launch::Entry;
using vehicle_sil::launch::Operation;
using vehicle_sil::launch::Pacing;
using vehicle_sil::launch::Phase;
using vehicle_sil::launch::RunState;
using vehicle_sil::launch::RunControls;
using vehicle_sil::launch::RunOptions;
using vehicle_sil::launch::StopRequests;
using vehicle_sil::launch::TimeCommands;

Command command(const std::string& name)
{
    Command c;
    c.name = name;
    c.accepted = true;
    return c;
}

/// The events as written, in memory.
struct Written
{
    Written()
    {
        EventStreamOptions options;
        options.heartbeat = 0ms;
        stream = std::make_unique<EventStream>(
            [this](std::string_view bytes) {
                const std::lock_guard lock(mutex);
                text.append(bytes);
                EventWrite out;
                out.bytes = bytes.size();
                return out;
            },
            [](const std::string&) {}, options);
    }

    /// The values of "phase" of the phase events written so far, in their order.
    std::vector<std::string> phases()
    {
        CHECK(stream->wait_written(10s));
        const std::lock_guard lock(mutex);
        std::vector<std::string> out;
        const std::string_view key = "\"event\":\"phase\"";
        const std::string_view value = "\"phase\":\"";
        for (std::size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + 1))
        {
            const std::size_t begin = text.find(value, at) + value.size();
            out.push_back(text.substr(begin, text.find('"', begin) - begin));
        }
        return out;
    }

    bool has(std::string_view part)
    {
        CHECK(stream->wait_written(10s));
        const std::lock_guard lock(mutex);
        return text.find(part) != std::string::npos;
    }

    std::mutex mutex;
    std::string text;
    std::unique_ptr<EventStream> stream; ///< last: its writer thread uses the members above
};

} // namespace

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
    RunControls controls(stops, false, std::move(observers));
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

TEST_CASE(run_controls_without_observers_keep_the_phase)
{
    StopRequests stops;
    RunControls controls(stops);
    controls.set_time_lock({});
    controls.set_phase(Phase::Stopping);
    CHECK(controls.phase() == Phase::Stopping);
    CHECK(controls.time().apply(command("pause"), "stdin").accepted);
}

TEST_CASE(run_controls_hand_the_time_commands_their_observer_and_the_start_paused)
{
    StopRequests stops;
    std::vector<std::string> seen;
    RunControls::Observers observers;
    observers.on_command = [&seen](const Command& c, std::string_view source) {
        seen.push_back(std::string(source) + " " + c.name.value_or("") + (c.accepted ? " accepted" : " refused"));
    };
    RunControls paused(stops, true, std::move(observers));
    paused.set_time_lock({});
    CHECK(&paused.time() == &paused.time());
    CHECK(paused.time().apply(command("pause"), "stdin").accepted);
    CHECK(!paused.time().apply(command("step"), "control_server").accepted); // no controller is attached
    CHECK((seen == std::vector<std::string>{"stdin pause accepted", "control_server step refused"}));
    // Started paused, the run would hold before its first step; not otherwise.
    const auto give_up = [] { return true; };
    CHECK(paused.time().hold_before_start(give_up, 1ms) == TimeCommands::Hold::GaveUp);
    RunControls running(stops, false);
    running.set_time_lock({});
    CHECK(running.time().hold_before_start(give_up, 1ms) == TimeCommands::Hold::None);
    // A stop is no affair of the time commands: nothing here asks for one.
    CHECK_EQ(stops.count(), 0);
}

TEST_CASE(phase_names_are_those_of_the_phase_events)
{
    using vehicle_sil::launch::phase_name;
    CHECK(std::string(phase_name(Phase::Checking)) == "checking");
    CHECK(std::string(phase_name(Phase::Starting)) == "starting");
    CHECK(std::string(phase_name(Phase::Waiting)) == "waiting");
    CHECK(std::string(phase_name(Phase::Running)) == "running");
    CHECK(std::string(phase_name(Phase::Stopping)) == "stopping");
    CHECK(std::string(phase_name(Phase::Ended)) == "ended");
}

TEST_CASE(a_run_without_controls_makes_its_own_and_writes_its_phases)
{
    Written written;
    RunOptions options;
    options.events = written.stream.get();
    const auto report = vehicle_sil::launch::run_file("no-such-directory/sim.yaml", "sim.yaml", options);
    CHECK(!report.run.has_value());
    CHECK((written.phases() == std::vector<std::string>{"checking", "ended"}));
    // And again: each run has controls of its own, so the second begins as the first did.
    static_cast<void>(vehicle_sil::launch::run_file("no-such-directory/sim.yaml", "sim.yaml", options));
    CHECK((written.phases() == std::vector<std::string>{"checking", "ended", "checking", "ended"}));
}

TEST_CASE(a_run_with_the_callers_controls_sets_their_phase_and_leaves_the_phase_events_to_them)
{
    StopRequests stops;
    std::vector<Phase> told;
    RunControls::Observers observers;
    observers.on_phase = [&told](Phase phase) { told.push_back(phase); };
    observers.on_command = [](const Command&, std::string_view) {};
    observers.on_state = [](const RunState&) {};
    RunControls controls(stops, false, std::move(observers));
    Written written;
    RunOptions options;
    options.controls = &controls;
    options.events = written.stream.get();
    const auto report = vehicle_sil::launch::run_file("no-such-directory/sim.yaml", "sim.yaml", options);
    CHECK(!report.run.has_value());
    CHECK(controls.phase() == Phase::Ended);
    CHECK((told == std::vector<Phase>{Phase::Checking, Phase::Ended}));
    // The observer wrote none, so there is none: the run does not write them a second time.
    CHECK(written.phases().empty());
    CHECK(written.has("\"event\":\"check\""));
}

namespace {

/// The text of the std::invalid_argument that `call` throws; empty if it throws none.
template <typename Call> std::string refusal(Call&& call)
{
    try
    {
        call();
    }
    catch (const std::invalid_argument& e)
    {
        return e.what();
    }
    return {};
}

} // namespace

TEST_CASE(a_run_refuses_controls_together_with_other_stop_requests)
{
    StopRequests stops;
    StopRequests other;
    RunControls controls(stops);
    RunOptions options;
    options.controls = &controls;
    options.stops = &other;
    const std::string text = refusal([&] { static_cast<void>(vehicle_sil::launch::run_file("none.yaml", "none.yaml", options)); });
    CHECK(text.find("`controls` and `stops`") != std::string::npos);
    // The same stop requests as those of the controls say the same thing twice: accepted.
    options.stops = &stops;
    CHECK(refusal([&] { static_cast<void>(vehicle_sil::launch::run_file("no-such-directory/sim.yaml", "sim.yaml", options)); })
            .empty());
}

TEST_CASE(a_run_refuses_controls_with_events_when_the_controls_have_no_observer_for_them)
{
    StopRequests stops;
    Written written;
    RunOptions options;
    options.events = written.stream.get();
    const auto run = [&](RunControls& controls) {
        options.controls = &controls;
        return refusal([&] { static_cast<void>(vehicle_sil::launch::run_file("no-such-directory/sim.yaml", "sim.yaml", options)); });
    };
    RunControls none(stops);
    CHECK(run(none).find("on_phase") != std::string::npos);
    RunControls::Observers only_phase;
    only_phase.on_phase = [](Phase) {};
    RunControls phase_only(stops, false, std::move(only_phase));
    CHECK(run(phase_only).find("on_command") != std::string::npos);
    RunControls::Observers only_command;
    only_command.on_command = [](const Command&, std::string_view) {};
    RunControls command_only(stops, false, std::move(only_command));
    CHECK(run(command_only).find("on_phase") != std::string::npos);
    RunControls::Observers no_state;
    no_state.on_phase = [](Phase) {};
    no_state.on_command = [](const Command&, std::string_view) {};
    RunControls without_state(stops, false, std::move(no_state));
    CHECK(run(without_state).find("on_state") != std::string::npos);
    // Without `events` there is nothing to leave to the observers.
    options.events = nullptr;
    CHECK(run(none).empty());
}

namespace {

/// Controls whose states are kept as they are told; of a run with a control server unless
/// `control_server` says otherwise.
struct Told
{
    explicit Told(bool start_paused = false, bool scenario = false, bool control_server = true) :
      controls(stops, start_paused, observers(), scenario)
    {
        controls.set_control_server(control_server);
        // A run that is not connected outside: no state is told for it, and the commands are
        // decided as they come.
        controls.set_time_lock({});
    }

    RunControls::Observers observers()
    {
        RunControls::Observers out;
        out.on_state = [this](const RunState& s) { states.push_back(s); };
        return out;
    }

    StopRequests stops;
    std::vector<RunState> states;
    RunControls controls;
};

} // namespace

TEST_CASE(the_state_is_told_when_it_differs_from_the_last_and_rev_counts_each_one)
{
    Told t;
    CHECK(t.states.empty());
    CHECK_EQ(t.controls.state().rev, std::uint64_t{0});
    // The first is told whatever it says; the same again is not.
    t.controls.publish();
    CHECK_EQ(t.states.size(), std::size_t{1});
    CHECK_EQ(t.states.back().rev, std::uint64_t{1});
    CHECK(t.states.back().phase == Phase::Checking);
    t.controls.publish();
    t.controls.set_phase(Phase::Checking);
    CHECK_EQ(t.states.size(), std::size_t{1});
    // A phase, a command, a stop request: each is one more, and rev goes up by one.
    t.controls.set_phase(Phase::Starting);
    CHECK_EQ(t.states.size(), std::size_t{2});
    CHECK(t.states.back().phase == Phase::Starting);
    CHECK(t.controls.time().apply(command("pause"), "stdin").accepted);
    CHECK_EQ(t.states.size(), std::size_t{3});
    CHECK(t.states.back().paused);
    // A command that changes nothing (a second pause; one that is refused) tells nothing.
    CHECK(t.controls.time().apply(command("pause"), "stdin").accepted);
    CHECK(!t.controls.time().apply(command("step"), "stdin").accepted);
    CHECK_EQ(t.states.size(), std::size_t{3});
    // A stop request is seen by the next publish (a signal handler calls nothing here).
    t.stops.request(15);
    CHECK_EQ(t.states.size(), std::size_t{3});
    t.controls.publish();
    CHECK_EQ(t.states.size(), std::size_t{4});
    CHECK_EQ(t.states.back().stop_requests, 1);
    CHECK(t.states.back().stopped_by == "interrupt");
    // The second request too; more than two say nothing more.
    t.stops.request(2);
    t.controls.publish();
    CHECK_EQ(t.states.back().stop_requests, 2);
    t.stops.request(2);
    t.controls.publish();
    CHECK_EQ(t.states.size(), std::size_t{5});
    for (std::size_t i = 0; i < t.states.size(); ++i)
    {
        CHECK_EQ(t.states[i].rev, static_cast<std::uint64_t>(i + 1));
    }
    // state() is the same state, with the rev told last.
    CHECK(t.controls.state() == t.states.back());
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

TEST_CASE(the_state_says_what_is_taken_before_during_and_after_the_simulation)
{
    Told t;
    t.controls.publish();
    // Setting up: nothing stands still, the speed is not known, a step needs the controller.
    RunState s = t.states.back();
    CHECK(!s.paused && !s.holding && !s.speed && !s.scenario);
    CHECK(s.pacing == Pacing::NotPaced);
    CHECK(s.allowed.pause.ok && s.allowed.resume.ok && s.allowed.speed.ok && s.allowed.stop.ok && s.allowed.write.ok);
    CHECK(!s.allowed.step.ok && s.allowed.step.reason == "not-set-up");
    CHECK(!s.allowed.run_until.ok && s.allowed.run_until.reason == "not-set-up");
    CHECK(!s.allowed.step.message.empty());
    // The ranges are there whether the operation is taken or not.
    CHECK(s.allowed.step.range.has_value() && s.allowed.speed.range.has_value());
    CHECK(!s.allowed.pause.range && !s.allowed.pause.caution);
    // A speed command before the controller is the speed set.
    Command speed = command("speed");
    speed.speed = 0.5;
    CHECK(t.controls.time().apply(speed, "stdin").accepted);
    CHECK(t.states.back().speed == std::optional<std::optional<double>>(std::optional<double>(0.5)));

    vehicle_sil::unit::InProcessRegistry registry;
    vehicle_sil::SimulationControllerOptions options;
    options.registry_uri = registry.uri;
    options.required_participants = {"NeverJoins"};
    options.time_control = vehicle_sil::TimeControlOptions{.step = std::chrono::milliseconds(10)};
    vehicle_sil::SimulationController controller(options);
    const std::size_t before = t.states.size();
    t.controls.time().attach(controller, 10, std::optional<double>(2.0));
    // Attached: one more state; a run_until is taken, a step only while paused.
    CHECK_EQ(t.states.size(), before + 1);
    s = t.states.back();
    CHECK(s.allowed.run_until.ok);
    CHECK(!s.allowed.step.ok && s.allowed.step.reason == "not-paused");
    CHECK(!s.paused);
    CHECK(t.controls.time().apply(command("pause"), "stdin").accepted);
    s = t.states.back();
    CHECK(s.allowed.step.ok && s.allowed.step.reason.empty() && s.allowed.step.message.empty());
    // As fast as possible: the speed is max (the inner empty), and there is nothing to keep up with.
    Command max = command("speed");
    CHECK(t.controls.time().apply(max, "stdin").accepted);
    s = t.states.back();
    CHECK(s.speed.has_value() && !s.speed->has_value());
    t.controls.set_pacing(Pacing::Late);
    t.controls.publish();
    CHECK(t.states.back().pacing == Pacing::NotPaced);
    // The pacing counts only while the simulation runs and goes at a speed that is a number.
    speed.speed = 1.0;
    CHECK(t.controls.time().apply(speed, "stdin").accepted);
    CHECK(t.controls.time().apply(command("resume"), "stdin").accepted);
    s = t.states.back();
    CHECK(!s.paused);
    CHECK(s.pacing == Pacing::NotPaced); // attached, but the simulation has not started
    t.controls.set_phase(Phase::Running);
    CHECK(t.states.back().pacing == Pacing::Late);
    // What the watch saw while there was nothing to keep up with says nothing: on time until
    // it sees otherwise.
    t.controls.set_pacing(Pacing::NotPaced);
    t.controls.publish();
    CHECK(t.states.back().pacing == Pacing::OnTime);
    t.controls.set_pacing(Pacing::Late);
    t.controls.publish();
    CHECK(t.states.back().pacing == Pacing::Late);
    CHECK(t.controls.time().apply(max, "stdin").accepted);
    CHECK(t.states.back().pacing == Pacing::NotPaced);
    CHECK(t.controls.time().apply(speed, "stdin").accepted);
    // Over: one more state; the time commands are refused, a stop and a write are not, the speed
    // stays as it was, nothing stands still.
    const std::size_t attached = t.states.size();
    t.controls.time().detach();
    CHECK_EQ(t.states.size(), attached + 1);
    s = t.states.back();
    CHECK(!s.paused && !s.holding);
    CHECK(s.speed == std::optional<std::optional<double>>(std::optional<double>(1.0)));
    for (const auto* a : {&s.allowed.pause, &s.allowed.resume, &s.allowed.step, &s.allowed.speed, &s.allowed.run_until})
    {
        CHECK(!a->ok && a->reason == "not-running");
    }
    CHECK(s.allowed.stop.ok && s.allowed.write.ok);
    t.controls.time().detach();
    CHECK_EQ(t.states.size(), attached + 1);
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
    CHECK(t.controls.allowed(Operation::RunUntil, Entry::ControlServer).reason == "scenario-running");
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

TEST_CASE(a_run_until_of_stdin_is_refused_by_the_time_lock_and_the_state_says_so)
{
    // With and without a control server: the state's run_until is standard input's answer, so
    // the lock's reason is there in both, and the command gets the same answer.
    for (const bool control_server : {false, true})
    {
        StopRequests stops;
        std::vector<RunState> states;
        std::vector<std::string> answers;
        RunControls::Observers observers;
        observers.on_state = [&states](const RunState& s) { states.push_back(s); };
        observers.on_command = [&answers](const Command& c, std::string_view source) {
            answers.push_back(*c.name + " " + std::string(source) + " " + (c.accepted ? "accepted" : c.reason));
        };
        RunControls controls(stops, false, std::move(observers));
        controls.set_control_server(control_server);
        Command until = command("run_until");
        until.at_ms = 5000;
        // Before the lock is known it waits; the lock then refuses it.
        CHECK(controls.time().apply(until, "stdin").pending);
        vehicle_sil::launch::TimeLock lock;
        lock.locked = true;
        lock.connected = {"CAN1"};
        controls.set_time_lock(lock);
        CHECK((answers == std::vector<std::string>{"run_until stdin outside-real-time"}));
        CHECK(!states.empty());
        const RunState::Operations& allowed = states.back().allowed;
        CHECK(!allowed.run_until.ok && allowed.run_until.reason == "outside-real-time");
        CHECK(allowed.run_until == controls.allowed(Operation::RunUntil, Entry::Stdin));
        // After the lock: refused at once, and nothing reaches the simulation.
        const Command late = controls.time().apply(until, "stdin");
        CHECK(!late.accepted && !late.pending && late.reason == "outside-real-time");
        CHECK(late.message.find("run_until is refused") != std::string::npos);
        CHECK(late.message.find("keeps to real time") != std::string::npos);
        CHECK_EQ(answers.size(), std::size_t{2});
    }
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

TEST_CASE(a_command_of_a_source_that_is_no_entry_is_refused_and_changes_nothing)
{
    Told t(false, true);
    for (const char* source : {"gui", "Control_Server", ""})
    {
        const Command done = t.controls.time().apply(command("pause"), source);
        CHECK(!done.accepted);
        CHECK(done.reason == "unknown-source");
        CHECK(done.message.find("no entry of a run's commands (stdin, control_server)") != std::string::npos);
    }
    CHECK(!t.controls.state().paused);
}

TEST_CASE(the_state_event_follows_the_command_event_of_the_command_that_made_it)
{
    Written written;
    StopRequests stops;
    RunControls::Observers observers;
    EventStream* events = written.stream.get();
    observers.on_command = [events](const Command& c, std::string_view source) { events->command(c, source); };
    observers.on_phase = [events](Phase phase) { events->phase(phase); };
    observers.on_state = [events](const RunState& s) { events->state(s); };
    RunControls controls(stops, false, std::move(observers));
    controls.set_time_lock({});
    // Each state is written before the next is made (one that waits would be replaced).
    controls.set_phase(Phase::Starting);
    CHECK(written.stream->wait_written(10s));
    CHECK(controls.time().apply(command("pause"), "stdin").accepted);
    CHECK(!controls.time().apply(command("step"), "stdin").accepted);
    CHECK(written.has("\"event\":\"state\""));
    std::vector<std::string> order;
    {
        const std::lock_guard lock(written.mutex);
        const std::string_view key = "{\"event\":\"";
        for (std::size_t at = written.text.find(key); at != std::string::npos; at = written.text.find(key, at + 1))
        {
            const std::size_t begin = at + key.size();
            std::string name = written.text.substr(begin, written.text.find('"', begin) - begin);
            if (name == "state")
            {
                const std::size_t rev = written.text.find("\"rev\":", begin) + 6;
                name += " " + written.text.substr(rev, written.text.find(',', rev) - rev);
            }
            order.push_back(std::move(name));
        }
    }
    // The phase, then its state; the command, then the state it made; a refused command makes none.
    CHECK((order == std::vector<std::string>{"phase", "state 1", "command", "state 2", "command"}));
}

TEST_CASE(the_pacing_is_late_until_five_seconds_have_passed_without_a_late_step)
{
    using vehicle_sil::launch::detail::PacingWatch;
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
    CHECK(std::string(vehicle_sil::launch::pacing_name(Pacing::NotPaced)) == "not-paced");
    CHECK(std::string(vehicle_sil::launch::pacing_name(Pacing::OnTime)) == "on-time");
    CHECK(std::string(vehicle_sil::launch::pacing_name(Pacing::Late)) == "late");
}

namespace {

/// What a run that does not get past its check tells of the commands that came before it:
/// "<name> accepted" or "<name> <reason>", in their order; and the `allowed.pause` of each state.
struct Answers
{
    std::vector<std::string> commands;
    std::vector<std::string> pause; ///< "ok", or the reason

    RunControls::Observers observers()
    {
        RunControls::Observers out;
        out.on_command = [this](const Command& c, std::string_view) {
            commands.push_back(c.name.value_or("") + " " + (c.accepted ? std::string("accepted") : c.reason));
        };
        out.on_state = [this](const RunState& s) { pause.push_back(s.allowed.pause.ok ? "ok" : s.allowed.pause.reason); };
        return out;
    }
};

/// A configuration file with `participants` (YAML lines), whose executables are not there: the
/// check reads the file, then fails, and nothing starts.
std::filesystem::path config_file(const std::string& name, const std::string& participants)
{
    const std::filesystem::path dir = std::filesystem::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "run_controls";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name;
    std::ofstream(file, std::ios::binary) << "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n" << participants;
    return file;
}

} // namespace

TEST_CASE(a_run_decides_the_time_commands_that_came_before_it_with_its_time_lock)
{
    const std::string engine = "  - name: Engine\n    command: [no-such-engine]\n";
    const std::string gateway = "  - name: Gateway\n    role: bridge\n    describe: false\n    required: false\n"
                                "    command: [no-such-bridge]\n";
    Command fast = command("speed");
    fast.speed = 5.0;
    /// run_file() of `file` on controls that got a pause, a speed of 5 and a resume before it.
    const auto run = [&fast](const std::filesystem::path& file, RunOptions options, Answers& answers) {
        StopRequests stops;
        RunControls controls(stops, false, answers.observers());
        CHECK(controls.time().apply(command("pause"), "stdin").pending);
        CHECK(controls.time().apply(fast, "stdin").pending);
        CHECK(controls.time().apply(command("resume"), "stdin").pending);
        CHECK(answers.commands.empty());
        options.controls = &controls;
        const auto report = vehicle_sil::launch::run_file(file, "sim.yaml", options);
        CHECK(!report.run.has_value());
        return report;
    };
    {
        // A participant that is a bridge: the run keeps real time, and says so to what waited,
        // although its check then fails (the bridge's executable is not there).
        Answers answers;
        const auto report = run(config_file("bridged.yaml", engine + gateway), RunOptions{}, answers);
        CHECK(report.check.config.has_value());
        CHECK(report.check.time_lock.locked);
        CHECK((answers.commands == std::vector<std::string>{"pause outside-real-time", "speed outside-real-time", "resume accepted"}));
        // The states: the first before the file is read; from the lock on, no pause.
        CHECK(answers.pause.size() >= 2);
        CHECK(answers.pause.front() == "ok");
        CHECK(answers.pause.back() == "outside-real-time");
    }
    {
        // The same file with --time-policy free, and one without a bridge: taken, in their order.
        RunOptions free;
        free.check.time_policy = vehicle_sil::launch::TimePolicy::Free;
        Answers answers;
        const auto report = run(config_file("bridged.yaml", engine + gateway), free, answers);
        CHECK(!report.check.time_lock.locked);
        CHECK((answers.commands == std::vector<std::string>{"pause accepted", "speed accepted", "resume accepted"}));
        Answers plain;
        static_cast<void>(run(config_file("plain.yaml", engine), RunOptions{}, plain));
        CHECK((plain.commands == std::vector<std::string>{"pause accepted", "speed accepted", "resume accepted"}));
        for (const std::string& pause : plain.pause)
        {
            CHECK(pause == "ok");
        }
    }
    {
        // --disable takes the bridge out before the lock is made.
        RunOptions disabled;
        disabled.check.overrides.disable = {"Gateway"};
        Answers answers;
        const auto report = run(config_file("bridged.yaml", engine + gateway), disabled, answers);
        CHECK(!report.check.time_lock.locked);
        CHECK((answers.commands == std::vector<std::string>{"pause accepted", "speed accepted", "resume accepted"}));
    }
    {
        // A file that cannot be read: no lock, nothing starts, and what waited is refused.
        Answers answers;
        const auto report = run("no-such-directory/sim.yaml", RunOptions{}, answers);
        CHECK(!report.check.config.has_value());
        CHECK((answers.commands == std::vector<std::string>{"pause not-running", "speed not-running", "resume not-running"}));
        CHECK(!answers.pause.empty());
        CHECK(answers.pause.back() == "not-running");
    }
}

TEST_CASE(a_test_refuses_controls_because_every_run_needs_its_own)
{
    StopRequests stops;
    RunControls controls(stops);
    vehicle_sil::launch::TestOptions options;
    options.run.controls = &controls;
    const std::string text =
      refusal([&] { static_cast<void>(vehicle_sil::launch::test_file("none.yaml", "none.yaml", options)); });
    CHECK(text.find("TestOptions::run.controls") != std::string::npos);
}

TEST_CASE(run_controls_tell_the_state_of_the_bridges_once_for_each_change)
{
    StopRequests stops;
    std::vector<RunState> told;
    RunControls::Observers observers;
    observers.on_state = [&](const RunState& state) { told.push_back(state); };
    RunControls controls(stops, false, std::move(observers));
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
