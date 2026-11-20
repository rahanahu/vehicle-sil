// SPDX-License-Identifier: MIT
//
// The controls of a run (RunControls) with what only the launcher's library has: the event stream
// (EventStream) that tells the phase and the state, the controls a run makes for itself when it
// is given none (run_file() on a file that cannot be read: nothing starts), and the time lock.
// The controls by themselves are in tests/runner/test_run_controls.cpp.

#include "run_controls_support.hpp"
#include "test_harness.hpp"
#include "silkit_test_util.hpp"

#include "run_pacing.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"

#include "vehicle_sil/launch/events.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/runner/run_controls.hpp"
#include "vehicle_sil/launch/test.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <algorithm>
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
#include <vector>


using namespace vehicle_sil::runner::test::run_controls_support;

namespace {

using namespace std::chrono_literals;
using vehicle_sil::runner::Command;
using vehicle_sil::launch::EventStream;
using vehicle_sil::launch::EventStreamOptions;
using vehicle_sil::launch::EventWrite;
using vehicle_sil::runner::Entry;
using vehicle_sil::runner::Operation;
using vehicle_sil::runner::Pacing;
using vehicle_sil::runner::Phase;
using vehicle_sil::runner::RunState;
using vehicle_sil::runner::RunControls;
using vehicle_sil::launch::RunLent;
using vehicle_sil::runner::RunObservers;
using vehicle_sil::launch::RunRequest;
using vehicle_sil::runner::StopRequests;
using vehicle_sil::runner::TimeCommands;

vehicle_sil::runner::Command command(const std::string& name)
{
    vehicle_sil::runner::Command c;
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

TEST_CASE(run_controls_without_observers_keep_the_phase)
{
    vehicle_sil::runner::StopRequests stops;
    vehicle_sil::runner::RunControls controls(stops);
    controls.set_time_lock({});
    controls.set_phase(vehicle_sil::runner::Phase::Stopping);
    CHECK(controls.phase() == vehicle_sil::runner::Phase::Stopping);
    CHECK(controls.time().apply(command("pause"), "stdin").accepted);
}

TEST_CASE(run_controls_hand_the_time_commands_their_observer_and_the_start_paused)
{
    vehicle_sil::runner::StopRequests stops;
    std::vector<std::string> seen;
    vehicle_sil::runner::RunControls::Observers observers;
    observers.on_command = [&seen](const vehicle_sil::runner::Command& c, std::string_view source) {
        seen.push_back(std::string(source) + " " + c.name.value_or("") + (c.accepted ? " accepted" : " refused"));
    };
    vehicle_sil::runner::RunControls paused(stops, true, std::move(observers), false);
    paused.set_time_lock({});
    CHECK(&paused.time() == &paused.time());
    CHECK(paused.time().apply(command("pause"), "stdin").accepted);
    CHECK(!paused.time().apply(command("step"), "control_server").accepted); // no controller is attached
    CHECK((seen == std::vector<std::string>{"stdin pause accepted", "control_server step refused"}));
    // Started paused, the run would hold before its first step; not otherwise.
    const auto give_up = [] { return true; };
    CHECK(paused.time().hold_before_start(give_up, 1ms) == vehicle_sil::runner::TimeCommands::Hold::GaveUp);
    vehicle_sil::runner::RunControls running(stops, false);
    running.set_time_lock({});
    CHECK(running.time().hold_before_start(give_up, 1ms) == vehicle_sil::runner::TimeCommands::Hold::None);
    // A stop is no affair of the time commands: nothing here asks for one.
    CHECK_EQ(stops.count(), 0);
}

namespace {

/// The request of a run of `file`, as a user names it.
RunRequest request_of(const std::string& file)
{
    RunRequest request;
    request.host.config = file;
    return request;
}

} // namespace

TEST_CASE(a_run_without_controls_makes_its_own_and_writes_its_phases)
{
    Written written;
    const RunRequest asked = request_of("no-such-directory/sim.yaml");
    RunLent lent;
    lent.events = written.stream.get();
    const auto report = vehicle_sil::launch::run_file(asked, {}, lent);
    CHECK(!report.run.has_value());
    CHECK((written.phases() == std::vector<std::string>{"checking", "ended"}));
    // And again: each run has controls of its own, so the second begins as the first did.
    static_cast<void>(vehicle_sil::launch::run_file(asked, {}, lent));
    CHECK((written.phases() == std::vector<std::string>{"checking", "ended", "checking", "ended"}));
}

TEST_CASE(a_run_with_the_callers_controls_sets_their_phase_and_leaves_the_phase_events_to_them)
{
    vehicle_sil::runner::StopRequests stops;
    std::vector<vehicle_sil::runner::Phase> told;
    vehicle_sil::runner::RunControls::Observers observers;
    observers.on_phase = [&told](vehicle_sil::runner::Phase phase) { told.push_back(phase); };
    observers.on_command = [](const vehicle_sil::runner::Command&, std::string_view) {};
    observers.on_state = [](const vehicle_sil::runner::RunState&) {};
    vehicle_sil::runner::RunControls controls(stops, false, std::move(observers), false);
    Written written;
    RunLent lent;
    lent.controls = &controls;
    lent.events = written.stream.get();
    const auto report = vehicle_sil::launch::run_file(request_of("no-such-directory/sim.yaml"), {}, lent);
    CHECK(!report.run.has_value());
    CHECK(controls.phase() == vehicle_sil::runner::Phase::Ended);
    CHECK((told == std::vector<vehicle_sil::runner::Phase>{vehicle_sil::runner::Phase::Checking, vehicle_sil::runner::Phase::Ended}));
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
    vehicle_sil::runner::StopRequests stops;
    vehicle_sil::runner::StopRequests other;
    vehicle_sil::runner::RunControls controls(stops);
    RunLent lent;
    lent.controls = &controls;
    lent.stops = &other;
    const std::string text = refusal([&] { static_cast<void>(vehicle_sil::launch::run_file(request_of("none.yaml"), {}, lent)); });
    CHECK(text.find("`controls` and `stops`") != std::string::npos);
    // The same stop requests as those of the controls say the same thing twice: accepted.
    lent.stops = &stops;
    CHECK(refusal([&] { static_cast<void>(vehicle_sil::launch::run_file(request_of("no-such-directory/sim.yaml"), {}, lent)); })
            .empty());
}

TEST_CASE(a_run_refuses_controls_with_events_when_the_controls_have_no_observer_for_them)
{
    vehicle_sil::runner::StopRequests stops;
    Written written;
    EventStream* events = written.stream.get();
    const auto run = [&](vehicle_sil::runner::RunControls& controls) {
        RunLent lent;
        lent.controls = &controls;
        lent.events = events;
        return refusal(
          [&] { static_cast<void>(vehicle_sil::launch::run_file(request_of("no-such-directory/sim.yaml"), {}, lent)); });
    };
    vehicle_sil::runner::RunControls none(stops);
    CHECK(run(none).find("on_phase") != std::string::npos);
    vehicle_sil::runner::RunControls::Observers only_phase;
    only_phase.on_phase = [](vehicle_sil::runner::Phase) {};
    vehicle_sil::runner::RunControls phase_only(stops, false, std::move(only_phase), false);
    CHECK(run(phase_only).find("on_command") != std::string::npos);
    vehicle_sil::runner::RunControls::Observers only_command;
    only_command.on_command = [](const vehicle_sil::runner::Command&, std::string_view) {};
    vehicle_sil::runner::RunControls command_only(stops, false, std::move(only_command), false);
    CHECK(run(command_only).find("on_phase") != std::string::npos);
    vehicle_sil::runner::RunControls::Observers no_state;
    no_state.on_phase = [](vehicle_sil::runner::Phase) {};
    no_state.on_command = [](const vehicle_sil::runner::Command&, std::string_view) {};
    vehicle_sil::runner::RunControls without_state(stops, false, std::move(no_state), false);
    CHECK(run(without_state).find("on_state") != std::string::npos);
    // Without `events` there is nothing to leave to the observers.
    events = nullptr;
    CHECK(run(none).empty());
}

// Whoever gives the controls observers says whether the run plays a scenario: no default stands in
// for it (one of false let a control server's clients write while a scenario plays).
static_assert(!std::is_constructible_v<vehicle_sil::runner::RunControls, vehicle_sil::runner::StopRequests&, bool, vehicle_sil::runner::RunControls::Observers>);

TEST_CASE(a_run_refuses_controls_made_for_another_request)
{
    vehicle_sil::runner::StopRequests stops;
    RunRequest scenario = request_of("no-such-directory/sim.yaml");
    scenario.intent.scenario = "drive.scenario.yaml";
    RunRequest paused = request_of("no-such-directory/sim.yaml");
    paused.intent.start_paused = true;
    const RunRequest plain_request = request_of("no-such-directory/sim.yaml");
    const auto run = [](const RunRequest& asked, vehicle_sil::runner::RunControls& controls) {
        RunLent lent;
        lent.controls = &controls;
        return refusal([&] { static_cast<void>(vehicle_sil::launch::run_file(asked, {}, lent)); });
    };
    // A run that plays a scenario, on controls made for one without: their rules would let a
    // control server's clients write while it plays.
    vehicle_sil::runner::RunControls plain(stops);
    const std::string text = run(scenario, plain);
    CHECK(text.find("made with start_paused false and scenario false") != std::string::npos);
    CHECK(text.find("the request has start_paused false and scenario true") != std::string::npos);
    CHECK(text.find("RunIntent::scenario.has_value()") != std::string::npos);
    CHECK(text.find("client must not write") != std::string::npos);
    CHECK(text.find("--start-paused") == std::string::npos);
    CHECK(text.ends_with(", or leave `controls` null."));
    const std::string held_text = run(paused, plain);
    CHECK(held_text.find("the request has start_paused true and scenario false") != std::string::npos);
    CHECK(held_text.find("only a run with --start-paused takes a step") != std::string::npos);
    CHECK(held_text.find("client must not write") == std::string::npos);
    // With a control server of the run's own the run needs the caller's controls: leaving them
    // null is no way out then.
    RunRequest served = scenario;
    served.host.control = vehicle_sil::launch::ControlHost{};
    CHECK(run(served, plain).ends_with("RunIntent::scenario.has_value()."));
    CHECK(run(plain_request, plain).empty());
    vehicle_sil::runner::RunControls playing(stops, false, vehicle_sil::runner::RunControls::Observers{}, true);
    CHECK(playing.scenario() && !playing.start_paused());
    CHECK(run(scenario, playing).empty());
    CHECK(run(plain_request, playing).find("made with start_paused false and scenario true") != std::string::npos);
    vehicle_sil::runner::RunControls held(stops, true);
    CHECK(held.start_paused() && !held.scenario());
    CHECK(run(paused, held).empty());
    CHECK(!run(plain_request, held).empty());
}

TEST_CASE(a_run_refuses_controls_that_say_otherwise_of_its_control_server)
{
    vehicle_sil::runner::StopRequests stops;
    const RunRequest plain_request = request_of("no-such-directory/sim.yaml");
    RunRequest served = plain_request;
    served.host.control = vehicle_sil::launch::ControlHost{};
    struct Endpoint final : vehicle_sil::launch::ControlEndpoint
    {
        vehicle_sil::runner::ControlServerInfo start(std::shared_ptr<vehicle_sil::control::Hub>) override { return {}; }
        void end() override {}
    } endpoint;
    const auto run = [](const RunRequest& asked, vehicle_sil::runner::RunControls& controls,
                        vehicle_sil::launch::ControlEndpoint* lent_endpoint) {
        RunLent lent;
        lent.controls = &controls;
        lent.control_endpoint = lent_endpoint;
        return refusal([&] { static_cast<void>(vehicle_sil::launch::run_file(asked, {}, lent)); });
    };
    // The run has a control server (its own, or the caller's), and the controls were not told:
    // the state would refuse every write that the server's clients can send.
    vehicle_sil::runner::RunControls without(stops);
    const std::string text = run(served, without, nullptr);
    CHECK(text.find("say that the run has no control server") != std::string::npos);
    CHECK(text.find("the request has one (RunHost::control or RunLent::control_endpoint)") != std::string::npos);
    CHECK(text.ends_with("call RunControls::set_control_server(true) before the run."));
    CHECK(run(plain_request, without, &endpoint) == text);
    // The controls say there is one, and the run has none: the state would offer writes.
    vehicle_sil::runner::RunControls with(stops);
    with.set_control_server(true);
    const std::string none_text = run(plain_request, with, nullptr);
    CHECK(none_text.find("say that the run has a control server") != std::string::npos);
    CHECK(none_text.find("the request has none") != std::string::npos);
    CHECK(none_text.ends_with("call RunControls::set_control_server(false), or leave `controls` null."));
    // Controls that agree are taken (the file cannot be read: nothing starts).
    vehicle_sil::runner::RunControls agreeing(stops);
    agreeing.set_control_server(true);
    CHECK(run(plain_request, agreeing, &endpoint).empty());
    vehicle_sil::runner::RunControls plain(stops);
    CHECK(run(plain_request, plain, nullptr).empty());
}

TEST_CASE(a_run_that_makes_its_own_controls_tells_them_of_a_lent_control_server)
{
    // Without the caller's controls, a control server that the caller lends is the run's: the
    // state does not refuse a write for want of one.
    struct Endpoint final : vehicle_sil::launch::ControlEndpoint
    {
        vehicle_sil::runner::ControlServerInfo start(std::shared_ptr<vehicle_sil::control::Hub>) override { return {}; }
        void end() override {}
    } endpoint;
    for (const bool lend : {false, true})
    {
        Written written;
        RunLent lent;
        lent.events = written.stream.get();
        lent.control_endpoint = lend ? &endpoint : nullptr;
        static_cast<void>(vehicle_sil::launch::run_file(request_of("no-such-directory/sim.yaml"), {}, lent));
        CHECK(written.has("\"event\":\"state\""));
        CHECK(written.has("\"reason\":\"no-control-server\"") == !lend);
    }
}

TEST_CASE(a_run_that_makes_its_own_controls_says_whether_it_plays_a_scenario)
{
    Written written;
    RunRequest asked = request_of("no-such-directory/sim.yaml");
    asked.intent.scenario = "drive.scenario.yaml";
    RunLent lent;
    lent.events = written.stream.get();
    static_cast<void>(vehicle_sil::launch::run_file(asked, {}, lent));
    CHECK(written.has("\"event\":\"state\""));
    CHECK(written.has("\"scenario\":true"));
    CHECK(!written.has("\"scenario\":false"));
}

TEST_CASE(the_state_is_told_when_it_differs_from_the_last_and_rev_counts_each_one)
{
    Told t;
    CHECK(t.states.empty());
    CHECK_EQ(t.controls.state().rev, std::uint64_t{0});
    // The first is told whatever it says; the same again is not.
    t.controls.publish();
    CHECK_EQ(t.states.size(), std::size_t{1});
    CHECK_EQ(t.states.back().rev, std::uint64_t{1});
    CHECK(t.states.back().phase == vehicle_sil::runner::Phase::Checking);
    t.controls.publish();
    t.controls.set_phase(vehicle_sil::runner::Phase::Checking);
    CHECK_EQ(t.states.size(), std::size_t{1});
    // A phase, a command, a stop request: each is one more, and rev goes up by one.
    t.controls.set_phase(vehicle_sil::runner::Phase::Starting);
    CHECK_EQ(t.states.size(), std::size_t{2});
    CHECK(t.states.back().phase == vehicle_sil::runner::Phase::Starting);
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

TEST_CASE(the_state_says_what_is_taken_before_during_and_after_the_simulation)
{
    Told t;
    t.controls.publish();
    // Setting up: nothing stands still, the speed is not known, a step needs the controller.
    vehicle_sil::runner::RunState s = t.states.back();
    CHECK(!s.paused && !s.holding && !s.speed && !s.scenario);
    CHECK(s.pacing == vehicle_sil::runner::Pacing::NotPaced);
    CHECK(s.allowed.pause.ok && s.allowed.resume.ok && s.allowed.speed.ok && s.allowed.stop.ok && s.allowed.write.ok);
    CHECK(!s.allowed.step.ok && s.allowed.step.reason == "not-set-up");
    CHECK(!s.allowed.run_until.ok && s.allowed.run_until.reason == "not-set-up");
    CHECK(!s.allowed.step.message.empty());
    // The ranges are there whether the operation is taken or not.
    CHECK(s.allowed.step.range.has_value() && s.allowed.speed.range.has_value());
    CHECK(!s.allowed.pause.range && !s.allowed.pause.caution);
    // A speed command before the controller is the speed set.
    vehicle_sil::runner::Command speed = command("speed");
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
    vehicle_sil::runner::Command max = command("speed");
    CHECK(t.controls.time().apply(max, "stdin").accepted);
    s = t.states.back();
    CHECK(s.speed.has_value() && !s.speed->has_value());
    t.controls.set_pacing(vehicle_sil::runner::Pacing::Late);
    t.controls.publish();
    CHECK(t.states.back().pacing == vehicle_sil::runner::Pacing::NotPaced);
    // The pacing counts only while the simulation runs and goes at a speed that is a number.
    speed.speed = 1.0;
    CHECK(t.controls.time().apply(speed, "stdin").accepted);
    CHECK(t.controls.time().apply(command("resume"), "stdin").accepted);
    s = t.states.back();
    CHECK(!s.paused);
    CHECK(s.pacing == vehicle_sil::runner::Pacing::NotPaced); // attached, but the simulation has not started
    t.controls.set_phase(vehicle_sil::runner::Phase::Running);
    CHECK(t.states.back().pacing == vehicle_sil::runner::Pacing::Late);
    // What the watch saw while there was nothing to keep up with says nothing: on time until
    // it sees otherwise.
    t.controls.set_pacing(vehicle_sil::runner::Pacing::NotPaced);
    t.controls.publish();
    CHECK(t.states.back().pacing == vehicle_sil::runner::Pacing::OnTime);
    t.controls.set_pacing(vehicle_sil::runner::Pacing::Late);
    t.controls.publish();
    CHECK(t.states.back().pacing == vehicle_sil::runner::Pacing::Late);
    CHECK(t.controls.time().apply(max, "stdin").accepted);
    CHECK(t.states.back().pacing == vehicle_sil::runner::Pacing::NotPaced);
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

TEST_CASE(a_run_until_of_stdin_is_refused_by_the_time_lock_and_the_state_says_so)
{
    // With and without a control server: the state's run_until is standard input's answer, so
    // the lock's reason is there in both, and the command gets the same answer.
    for (const bool control_server : {false, true})
    {
        vehicle_sil::runner::StopRequests stops;
        std::vector<vehicle_sil::runner::RunState> states;
        std::vector<std::string> answers;
        vehicle_sil::runner::RunControls::Observers observers;
        observers.on_state = [&states](const vehicle_sil::runner::RunState& s) { states.push_back(s); };
        observers.on_command = [&answers](const vehicle_sil::runner::Command& c, std::string_view source) {
            answers.push_back(*c.name + " " + std::string(source) + " " + (c.accepted ? "accepted" : c.reason));
        };
        vehicle_sil::runner::RunControls controls(stops, false, std::move(observers), false);
        controls.set_control_server(control_server);
        vehicle_sil::runner::Command until = command("run_until");
        until.at_ms = 5000;
        // Before the lock is known it waits; the lock then refuses it.
        CHECK(controls.time().apply(until, "stdin").pending);
        vehicle_sil::config::TimeLock lock;
        lock.locked = true;
        lock.connected = {"CAN1"};
        controls.set_time_lock(lock);
        CHECK((answers == std::vector<std::string>{"run_until stdin outside-real-time"}));
        CHECK(!states.empty());
        const vehicle_sil::runner::RunState::Operations& allowed = states.back().allowed;
        CHECK(!allowed.run_until.ok && allowed.run_until.reason == "outside-real-time");
        CHECK(allowed.run_until == controls.allowed(vehicle_sil::runner::Operation::RunUntil, vehicle_sil::runner::Entry::Stdin));
        // After the lock: refused at once, and nothing reaches the simulation.
        const vehicle_sil::runner::Command late = controls.time().apply(until, "stdin");
        CHECK(!late.accepted && !late.pending && late.reason == "outside-real-time");
        CHECK(late.message.find("run_until is refused") != std::string::npos);
        CHECK(late.message.find("keeps to real time") != std::string::npos);
        CHECK_EQ(answers.size(), std::size_t{2});
    }
}

TEST_CASE(a_command_of_a_source_that_is_no_entry_is_refused_and_changes_nothing)
{
    Told t(false, true);
    for (const char* source : {"gui", "Control_Server", ""})
    {
        const vehicle_sil::runner::Command done = t.controls.time().apply(command("pause"), source);
        CHECK(!done.accepted);
        CHECK(done.reason == "unknown-source");
        CHECK(done.message.find("no entry of a run's commands (stdin, control_server)") != std::string::npos);
    }
    CHECK(!t.controls.state().paused);
}

TEST_CASE(the_state_event_follows_the_command_event_of_the_command_that_made_it)
{
    Written written;
    vehicle_sil::runner::StopRequests stops;
    vehicle_sil::runner::RunControls::Observers observers;
    EventStream* events = written.stream.get();
    observers.on_command = [events](const vehicle_sil::runner::Command& c, std::string_view source) { events->command(c, source); };
    observers.on_phase = [events](vehicle_sil::runner::Phase phase) { events->phase(phase); };
    observers.on_state = [events](const vehicle_sil::runner::RunState& s) { events->state(s); };
    vehicle_sil::runner::RunControls controls(stops, false, std::move(observers), false);
    controls.set_time_lock({});
    // Each state is written before the next is made (one that waits would be replaced).
    controls.set_phase(vehicle_sil::runner::Phase::Starting);
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

namespace {

/// What a run that does not get past its check tells of the commands that came before it:
/// "<name> accepted" or "<name> <reason>", in their order; and the `allowed.pause` of each state.
struct Answers
{
    std::vector<std::string> commands;
    std::vector<std::string> pause; ///< "ok", or the reason

    vehicle_sil::runner::RunControls::Observers observers()
    {
        vehicle_sil::runner::RunControls::Observers out;
        out.on_command = [this](const vehicle_sil::runner::Command& c, std::string_view) {
            commands.push_back(c.name.value_or("") + " " + (c.accepted ? std::string("accepted") : c.reason));
        };
        out.on_state = [this](const vehicle_sil::runner::RunState& s) { pause.push_back(s.allowed.pause.ok ? "ok" : s.allowed.pause.reason); };
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
    std::ofstream(file, std::ios::binary) << "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n" << participants;
    return file;
}

} // namespace

TEST_CASE(a_run_decides_the_time_commands_that_came_before_it_with_its_time_lock)
{
    const std::string engine = "  - name: Engine\n    command: [no-such-engine]\n";
    const std::string gateway = "  - name: Gateway\n    role: bridge\n    describe: false\n    required: false\n"
                                "    command: [no-such-bridge]\n";
    vehicle_sil::runner::Command fast = command("speed");
    fast.speed = 5.0;
    /// run_file() of `file` on controls that got a pause, a speed of 5 and a resume before it.
    const auto run = [&fast](const std::filesystem::path& file, RunRequest asked, Answers& answers) {
        vehicle_sil::runner::StopRequests stops;
        vehicle_sil::runner::RunControls controls(stops, false, answers.observers(), false);
        CHECK(controls.time().apply(command("pause"), "stdin").pending);
        CHECK(controls.time().apply(fast, "stdin").pending);
        CHECK(controls.time().apply(command("resume"), "stdin").pending);
        CHECK(answers.commands.empty());
        RunLent lent;
        lent.controls = &controls;
        asked.host.config = vehicle_sil::process::utf8_from_path(file);
        const auto report = vehicle_sil::launch::run_file(asked, {}, lent);
        CHECK(!report.run.has_value());
        return report;
    };
    {
        // A participant that is a bridge: the run keeps real time, and says so to what waited,
        // although its check then fails (the bridge's executable is not there).
        Answers answers;
        const auto report = run(config_file("bridged.yaml", engine + gateway), RunRequest{}, answers);
        CHECK(report.check.config.has_value());
        CHECK(report.check.time_lock.locked);
        CHECK((answers.commands == std::vector<std::string>{"pause outside-real-time", "speed outside-real-time", "resume accepted"}));
        // The states: the first before the file is read; from the lock on, no pause; from the
        // end on (the check failed), no time command.
        CHECK((answers.pause == std::vector<std::string>{"ok", "outside-real-time", "not-running", "not-running"}));
    }
    {
        // The same file with --time-policy free, and one without a bridge: taken, in their order.
        RunRequest free;
        free.intent.time_policy = vehicle_sil::config::TimePolicy::Free;
        Answers answers;
        const auto report = run(config_file("bridged.yaml", engine + gateway), free, answers);
        CHECK(!report.check.time_lock.locked);
        CHECK((answers.commands == std::vector<std::string>{"pause accepted", "speed accepted", "resume accepted"}));
        Answers plain;
        static_cast<void>(run(config_file("plain.yaml", engine), RunRequest{}, plain));
        CHECK((plain.commands == std::vector<std::string>{"pause accepted", "speed accepted", "resume accepted"}));
        // Taken until the run ends (its check failed): then the state when the time commands
        // end, and the one of the phase ended.
        CHECK(plain.pause.size() >= 3);
        for (std::size_t i = 0; i < plain.pause.size(); ++i)
        {
            CHECK(plain.pause[i] == (i + 2 < plain.pause.size() ? "ok" : "not-running"));
        }
    }
    {
        // --disable takes the bridge out before the lock is made.
        RunRequest disabled;
        disabled.intent.disable = {"Gateway"};
        Answers answers;
        const auto report = run(config_file("bridged.yaml", engine + gateway), disabled, answers);
        CHECK(!report.check.time_lock.locked);
        CHECK((answers.commands == std::vector<std::string>{"pause accepted", "speed accepted", "resume accepted"}));
    }
    {
        // A file that cannot be read: no lock, nothing starts, and what waited is refused.
        Answers answers;
        const auto report = run("no-such-directory/sim.yaml", RunRequest{}, answers);
        CHECK(!report.check.config.has_value());
        CHECK((answers.commands == std::vector<std::string>{"pause not-running", "speed not-running", "resume not-running"}));
        CHECK(!answers.pause.empty());
        CHECK(answers.pause.back() == "not-running");
    }
}

TEST_CASE(a_run_that_does_not_start_ends_the_same_way_whatever_kept_it_from_starting)
{
    const std::string engine = "  - name: Engine\n    command: [no-such-engine]\n";
    /// What the controls told in run_file() of `file` (a pause came before it), in order:
    /// "phase <name>", "command <name> <accepted or reason>", "state <phase> <allowed.pause>"; then
    /// what a pause after the run is answered.
    const auto run = [](const std::string& file, RunRequest asked) {
        std::vector<std::string> told;
        vehicle_sil::runner::RunControls::Observers observers;
        observers.on_phase = [&told](Phase phase) { told.push_back(std::string("phase ") + vehicle_sil::runner::phase_name(phase)); };
        observers.on_command = [&told](const Command& c, std::string_view) {
            told.push_back("command " + c.name.value_or("") + " " + (c.accepted ? std::string("accepted") : c.reason));
        };
        observers.on_state = [&told](const RunState& s) {
            told.push_back(std::string("state ") + vehicle_sil::runner::phase_name(s.phase) + " " +
                           (s.allowed.pause.ok ? std::string("ok") : s.allowed.pause.reason));
        };
        StopRequests stops;
        RunControls controls(stops, false, std::move(observers), false);
        CHECK(controls.time().apply(command("pause"), "stdin").pending);
        RunLent lent;
        lent.controls = &controls;
        asked.host.config = file;
        const auto report = vehicle_sil::launch::run_file(asked, {}, lent);
        CHECK(!report.run.has_value());
        const Command late = controls.time().apply(command("resume"), "stdin");
        told.push_back("after " + (late.accepted ? std::string("accepted") : late.reason));
        return told;
    };
    const std::string plain = vehicle_sil::process::utf8_from_path(config_file("plain.yaml", engine));
    RunRequest unknown_name;
    unknown_name.intent.disable = {"NoSuchParticipant"};
    // The file cannot be read; the check failed; a name of --disable that the file does not have
    // (a usage error found by reading the file).
    const std::vector<std::vector<std::string>> runs = {run("no-such-directory/sim.yaml", RunRequest{}),
                                                        run(plain, RunRequest{}), run(plain, unknown_name)};
    // The pause waits for the time lock, which comes only with a configuration: refused at the
    // end when the file cannot be read or the arguments do not fit it; decided by the lock when
    // the check reads the file and then fails.
    const std::vector<bool> waited = {true, false, true};
    for (std::size_t i = 0; i < runs.size(); ++i)
    {
        const std::vector<std::string>& told = runs[i];
        // The end: a state that takes no time command, the refusals of what still waited, the
        // phase, the last state; nothing is taken after it.
        const auto end = std::find(told.begin(), told.end(), "state checking not-running");
        CHECK(end != told.end());
        std::vector<std::string> expected = {"state checking not-running"};
        if (waited[i])
        {
            expected.push_back("command pause not-running");
        }
        expected.insert(expected.end(), {"phase ended", "state ended not-running", "command resume not-running", "after not-running"});
        CHECK((std::vector<std::string>(end, told.end()) == expected));
        // The pause was answered once.
        CHECK(std::count_if(told.begin(), told.end(), [](const std::string& t) { return t.starts_with("command pause"); }) == 1);
    }
    CHECK(std::find(runs[1].begin(), runs[1].end(), "command pause accepted") != runs[1].end());
}

TEST_CASE(a_test_refuses_controls_because_every_run_needs_its_own)
{
    vehicle_sil::runner::StopRequests stops;
    vehicle_sil::runner::RunControls controls(stops);
    vehicle_sil::launch::TestOptions options;
    options.request.host.config = "none.yaml";
    options.lent.controls = &controls;
    const std::string text = refusal([&] { static_cast<void>(vehicle_sil::launch::test_file(options)); });
    CHECK(text.find("TestOptions::lent.controls") != std::string::npos);
}
