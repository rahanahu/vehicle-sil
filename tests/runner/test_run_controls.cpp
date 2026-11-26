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
#include "time_commands_internal.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include "vehicle_sil/runner/detail/run_samples.hpp"
#include "vehicle_sil/runner/run_controls.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
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
        vehicle_sil::runner::detail::attach_controller(t.controls.time(), controller, 10, std::optional<double>(1.0));
        vehicle_sil::runner::detail::attach_controller(with.controls.time(), other, 10, std::optional<double>(1.0));
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

TEST_CASE(hold_before_start_and_stop_wake_react_immediately_to_non_signal_stops)
{
    using Clock = std::chrono::steady_clock;
    using vehicle_sil::test::scaled_timeout;
    // Case 1: accepted stop command wakes hold_before_start and stop_wake immediately.
    {
        StopRequests stops;
        RunControls controls(stops, true);
        controls.set_time_lock({});
        std::atomic<int> wakes{0};
        controls.set_stop_wake([&wakes] { wakes.fetch_add(1, std::memory_order_relaxed); });

        std::promise<void> entered;
        auto enteredFuture = entered.get_future();
        TimeCommands::Hold result = TimeCommands::Hold::None;
        std::thread holder([&] {
            result = controls.time().hold_before_start([&] { return stops.count() > 0; }, scaled_timeout(5s),
                                                       [&] { entered.set_value(); });
        });
        CHECK(enteredFuture.wait_for(scaled_timeout(2s)) == std::future_status::ready);
        std::this_thread::sleep_for(10ms);

        const auto t0 = Clock::now();
        const Command applied = controls.time().apply(
            named("stop"), "stdin", [&stops] { stops.request(vehicle_sil::runner::stop_source::kStopCommand); });
        CHECK(applied.accepted);
        holder.join();
        const auto elapsed = Clock::now() - t0;
        CHECK(result == TimeCommands::Hold::GaveUp);
        CHECK_EQ(wakes.load(), 1);
        // With poll = scaled_timeout(5s), a missing wake would wait the full poll.
        CHECK(elapsed < scaled_timeout(500ms));

        // Clearing stop_wake prevents further notifications on subsequent stops.
        controls.set_stop_wake({});
        stops.request(vehicle_sil::runner::stop_source::kStopCommand);
        controls.publish();
        CHECK_EQ(wakes.load(), 1);
    }

    // Case 2: supervisor stop (request + publish) wakes hold_before_start and stop_wake immediately.
    {
        StopRequests stops;
        RunControls controls(stops, true);
        controls.set_time_lock({});
        std::atomic<int> wakes{0};
        controls.set_stop_wake([&wakes] { wakes.fetch_add(1, std::memory_order_relaxed); });

        std::promise<void> entered;
        auto enteredFuture = entered.get_future();
        TimeCommands::Hold result = TimeCommands::Hold::None;
        std::thread holder([&] {
            result = controls.time().hold_before_start([&] { return stops.count() > 0; }, scaled_timeout(5s),
                                                       [&] { entered.set_value(); });
        });
        CHECK(enteredFuture.wait_for(scaled_timeout(2s)) == std::future_status::ready);
        std::this_thread::sleep_for(10ms);

        const auto t0 = Clock::now();
        stops.request(vehicle_sil::runner::stop_source::kEndOfInput);
        controls.publish();
        holder.join();
        const auto elapsed = Clock::now() - t0;
        CHECK(result == TimeCommands::Hold::GaveUp);
        CHECK_EQ(wakes.load(), 1);
        CHECK(elapsed < scaled_timeout(500ms));
    }

    // Case 3: intervening tell() (e.g. set_outside() from a bridge thread) between stops.request()
    // and controls.publish() must not suppress the hold_before_start() wakeup.
    {
        StopRequests stops;
        RunControls controls(stops, true);
        controls.set_time_lock({});
        std::atomic<int> wakes{0};
        controls.set_stop_wake([&wakes] { wakes.fetch_add(1, std::memory_order_relaxed); });

        std::promise<void> entered;
        auto enteredFuture = entered.get_future();
        TimeCommands::Hold result = TimeCommands::Hold::None;
        std::thread holder([&] {
            result = controls.time().hold_before_start([&] { return stops.count() > 0; }, scaled_timeout(5s),
                                                       [&] { entered.set_value(); });
        });
        CHECK(enteredFuture.wait_for(scaled_timeout(2s)) == std::future_status::ready);
        std::this_thread::sleep_for(10ms);

        const auto t0 = Clock::now();
        stops.request(vehicle_sil::runner::stop_source::kEndOfInput);
        RunState::Outside bridge;
        bridge.network = "CAN";
        bridge.target = "can0";
        bridge.participant = "bridge.CAN";
        controls.set_outside({bridge});
        controls.publish();
        holder.join();
        const auto elapsed = Clock::now() - t0;
        CHECK(result == TimeCommands::Hold::GaveUp);
        CHECK_EQ(wakes.load(), 1);
        CHECK(elapsed < scaled_timeout(500ms));
    }
}

TEST_CASE(run_controls_raise_to_and_stop_wake_ignore_duplicate_levels)
{
    using Clock = std::chrono::steady_clock;
    using vehicle_sil::test::scaled_timeout;
    StopRequests stops;
    RunControls controls(stops, true);
    controls.set_stop_name([&stops] { return vehicle_sil::runner::stop_source_name(stops.first()); });
    controls.set_time_lock({});
    std::atomic<int> wakes{0};
    controls.set_stop_wake([&wakes] { wakes.fetch_add(1, std::memory_order_relaxed); });

    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    TimeCommands::Hold result = TimeCommands::Hold::None;
    std::thread holder([&] {
        result = controls.time().hold_before_start([&] { return stops.count() > 0; }, scaled_timeout(5s),
                                                   [&] { entered.set_value(); });
    });
    CHECK(enteredFuture.wait_for(scaled_timeout(2s)) == std::future_status::ready);
    std::this_thread::sleep_for(10ms);

    namespace stop_level = vehicle_sil::runner::stop_level;
    const auto t0 = Clock::now();
    const Command first = controls.time().apply(named("stop"), "api", [&stops] {
        static_cast<void>(stops.raise_to(vehicle_sil::runner::stop_source::kStopCommand, stop_level::kGraceful));
    });
    CHECK(first.accepted);
    holder.join();
    const auto elapsed = Clock::now() - t0;
    CHECK(result == TimeCommands::Hold::GaveUp);
    CHECK_EQ(wakes.load(), 1);
    CHECK_EQ(controls.state().stop_requests, stop_level::kGraceful);
    CHECK(controls.state().stopped_by == "stop command");
    CHECK(elapsed < scaled_timeout(500ms));

    // Duplicate graceful raise_to does not fire stop_wake again.
    const Command dup1 = controls.time().apply(named("stop"), "api", [&stops] {
        static_cast<void>(stops.raise_to(vehicle_sil::runner::stop_source::kStopCommand, stop_level::kGraceful));
    });
    CHECK(dup1.accepted);
    CHECK_EQ(wakes.load(), 1);
    CHECK_EQ(controls.state().stop_requests, stop_level::kGraceful);

    // Escalation to force fires stop_wake once more.
    const Command force = controls.time().apply(named("stop"), "api", [&stops] {
        static_cast<void>(stops.raise_to(vehicle_sil::runner::stop_source::kStopCommand, stop_level::kForce));
    });
    CHECK(force.accepted);
    CHECK_EQ(wakes.load(), 2);
    CHECK_EQ(controls.state().stop_requests, stop_level::kForce);

    // Duplicate force raise_to does not fire stop_wake again.
    const Command dup2 = controls.time().apply(named("stop"), "api", [&stops] {
        static_cast<void>(stops.raise_to(vehicle_sil::runner::stop_source::kStopCommand, stop_level::kForce));
    });
    CHECK(dup2.accepted);
    CHECK_EQ(wakes.load(), 2);
    CHECK_EQ(controls.state().stop_requests, stop_level::kForce);
}

TEST_CASE(handover_from_scenario_atomically_pauses_unlocks_writes_and_fails_on_stop_or_detach)
{
    using namespace vehicle_sil;
    using vehicle_sil::runner::detail::attach_controller;
    using vehicle_sil::runner::detail::RunSamples;
    using vehicle_sil::runner::detail::TimeCommandsAccess;
    using vehicle_sil::runner::detail::TrafficPlan;
    using vehicle_sil::test::scaled_timeout;
    using vehicle_sil::unit::coordinated_options;
    using vehicle_sil::unit::InProcessRegistry;
    using vehicle_sil::unit::start_together;

    // 1. Atomic success: pauses guarded controller at P, sets handed_over + scenario_over_at_ms,
    // unlocks write_refusal(), clears guard, and calls samples.scenario_ends().
    {
        InProcessRegistry registry;
        SimulationControllerOptions options;
        options.registry_uri = registry.uri;
        options.required_participants = {"Member"};
        options.time_control = TimeControlOptions{.step = 10ms, .speed = std::nullopt};
        SimulationController controller(options);
        auto peer = std::make_unique<ParticipantRuntime>(
            coordinated_options("Member", registry.uri, VirtualTimeOptions{10ms, std::nullopt}));
        peer->set_step_handler([](SimTime, SimDuration) {});

        StopRequests stops;
        std::vector<RunState> states;
        RunControls::Observers obs;
        obs.on_state = [&](const RunState& s) { states.push_back(s); };
        RunControls controls(stops, /*start_paused=*/false, std::move(obs), /*scenario=*/true);
        controls.set_control_server(true);
        controls.set_time_lock({});
        controls.set_phase(Phase::Running);
        attach_controller(controls.time(), controller, 10, std::nullopt);
        controller.guard_from(SimTime{});
        start_together(controller, {peer.get()}, 30s);

        const auto guarded = controller.wait_guarded(scaled_timeout(10s));
        CHECK(guarded.has_value());
        CHECK(controls.write_refusal().has_value());
        CHECK(controls.write_refusal()->reason == vehicle_sil::runner::reason::kScenarioRunning);
        CHECK(!controls.state().handed_over);
        CHECK(!controls.state().scenario_over_at_ms.has_value());

        RunSamples samples({"Member"}, TrafficPlan{});
        samples.scenario_begins({"inc"});
        const std::atomic<bool> quit{false};
        CHECK(TimeCommandsAccess::handover_from_scenario(controls.time(), controller, stops, *guarded, quit, samples));

        CHECK(!controls.write_refusal().has_value());
        const RunState st = controls.state();
        CHECK(st.scenario);
        CHECK(st.handed_over);
        CHECK(st.paused);
        CHECK(st.allowed.write.ok);
        const std::int64_t at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(guarded->time_since_epoch()).count();
        CHECK(st.scenario_over_at_ms == std::optional<std::int64_t>(at_ms));
        CHECK(st.paused_at_ms == std::optional<std::int64_t>(at_ms));
        CHECK(controller.wait_until_paused(scaled_timeout(10s)) == guarded);
        CHECK(!states.empty() && states.back().handed_over && states.back().paused);

        controls.time().detach();
        controller.stop("done");
        peer->stop("done");
    }

    // 2. Refuses safely when stops > 0, quit == true, or detached == true (state stays untouched).
    {
        InProcessRegistry registry;
        SimulationControllerOptions options;
        options.registry_uri = registry.uri;
        options.required_participants = {"Member"};
        options.time_control = TimeControlOptions{.step = 10ms, .speed = std::nullopt};
        SimulationController controller(options);
        auto peer = std::make_unique<ParticipantRuntime>(
            coordinated_options("Member", registry.uri, VirtualTimeOptions{10ms, std::nullopt}));
        peer->set_step_handler([](SimTime, SimDuration) {});
        controller.guard_from(SimTime{});
        start_together(controller, {peer.get()}, 30s);
        const auto guarded = controller.wait_guarded(scaled_timeout(10s));
        CHECK(guarded.has_value());

        StopRequests stops;
        RunControls controls(stops, false, {}, true);
        controls.set_time_lock({});
        attach_controller(controls.time(), controller, 10, std::nullopt);
        RunSamples samples({"Member"}, TrafficPlan{});

        // quit == true
        const std::atomic<bool> quit_true{true};
        CHECK(!TimeCommandsAccess::handover_from_scenario(controls.time(), controller, stops, *guarded, quit_true, samples));
        CHECK(!controls.state().handed_over);

        // stops > 0
        const std::atomic<bool> quit_false{false};
        stops.request(vehicle_sil::runner::stop_source::kStopCommand);
        CHECK(!TimeCommandsAccess::handover_from_scenario(controls.time(), controller, stops, *guarded, quit_false, samples));
        CHECK(!controls.state().handed_over);

        // detached
        StopRequests clean_stops;
        RunControls detached_controls(clean_stops, false, {}, true);
        detached_controls.set_time_lock({});
        attach_controller(detached_controls.time(), controller, 10, std::nullopt);
        detached_controls.time().detach();
        CHECK(!TimeCommandsAccess::handover_from_scenario(detached_controls.time(), controller, clean_stops, *guarded,
                                                          quit_false, samples));
        CHECK(!detached_controls.state().handed_over);

        // time_locked (defense-in-depth)
        RunControls locked_controls(clean_stops, false, {}, true);
        locked_controls.set_time_lock(vehicle_sil::config::TimeLock{true, {}, {"Gateway"}});
        attach_controller(locked_controls.time(), controller, 10, std::nullopt);
        CHECK(!TimeCommandsAccess::handover_from_scenario(locked_controls.time(), controller, clean_stops, *guarded,
                                                          quit_false, samples));
        CHECK(!locked_controls.state().handed_over);
        CHECK(locked_controls.write_refusal().has_value());
        CHECK(locked_controls.write_refusal()->reason == vehicle_sil::runner::reason::kScenarioRunning);
        locked_controls.time().detach();

        controls.time().detach();
        controller.stop("done");
        peer->stop("done");
    }
}
