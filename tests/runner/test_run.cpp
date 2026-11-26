// SPDX-License-Identifier: MIT
//
// The run (the runner library, without the launcher): judging what the run saw (assess_run()),
// the close of the connections, the start limit and the lines of the participants that the run
// writes. The start-boundary tests also start a helper process.

#include "run_support.hpp"
#include "launch_test_util.hpp"

#include "vehicle_sil/runner/detail/run_detail.hpp"

#include "vehicle_sil/config/detail/check_words.hpp"
#include "vehicle_sil/runner/detail/run_close.hpp"
#include "run_connection.hpp"
#include "run_control.hpp"
#include "run_notes.hpp"
#include "run_participants.hpp"
#include "run_scenario.hpp"
#include "run_start.hpp"
#include "run_watch.hpp"
#include "time_commands_internal.hpp"
#include "spawn_sleeper.hpp"
#include "vehicle_sil/runner/detail/run_samples.hpp"
#include "vehicle_sil/runner/detail/run_start_limit.hpp"
#include "silkit_test_util.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::runner;
using namespace vehicle_sil::scenario;
using namespace vehicle_sil::config;
using vehicle_sil::config::test::check_text;
using vehicle_sil::config::test::lines;
using vehicle_sil::process::ExitStatus;
using namespace vehicle_sil::runner::test::run_support;

namespace {

// Give each test its own directory without overwriting an earlier run's files.
struct StartBoundaryDirectory
{
    std::filesystem::path path;
    StartBoundaryDirectory()
    {
        do
        {
            path = std::filesystem::temp_directory_path() / ("vehicle-sil-start-boundary-" + std::to_string(std::random_device{}()));
        } while (!std::filesystem::create_directory(path));
    }
    ~StartBoundaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void check_start_boundary(bool deadline)
{
    using namespace vehicle_sil::runner::detail;
    StartBoundaryDirectory dir;
    CheckReport check;
    check.file = "sim.yaml";
    check.config.emplace();
    ParticipantConfig child;
    child.name = deadline ? "Ghost" : "Child";
    child.describe = false;
    child.command = {VEHICLE_SIL_PROCESS_HELPER, "--pid-file", "child.pid", "--exit", "7"};
    check.config->participants.push_back(child);
    ParticipantResult childResult;
    childResult.status = ParticipantStatus::NotChecked;
    childResult.executable = VEHICLE_SIL_PROCESS_HELPER;
    check.participants.push_back(childResult);
    if (!deadline)
    {
        ParticipantConfig last;
        last.name = "Last";
        last.manual = true;
        last.describe = false;
        last.command = {VEHICLE_SIL_PROCESS_HELPER};
        check.config->participants.push_back(last);
        check.participants.push_back(childResult);
    }
    RunPlan plan = plan_run(check, nullptr, dir.path, RunChoices{});
    if (deadline)
    {
        // The internal plan makes the first check observe both the deadline and child exit.
        plan.start_timeout_ms = 0;
    }
    RunObservers observers;
    StopRequests stops;
    RunControls controls(stops);
    RunContext ctx{plan.config(), plan.check, plan, observers, nullptr, nullptr, controls, controls.time()};
    RunOutcome outcome = planned_outcome(plan);
    RunOutput output(ctx);
    RunControl control(ctx, output);
    RunSamples samples({}, {});
    RunParticipants participants(ctx, output, outcome, plan.base);
    RunNotes notes(ctx, participants);
    RunScenario scenario(ctx, output, control, samples);
    RunStart start(ctx, output, control, notes, scenario, participants, outcome);
    vehicle_sil::unit::InProcessRegistry registry;
    SimulationControllerOptions options;
    options.registry_uri = registry.uri;
    options.required_participants = deadline ? std::vector<std::string>{"Ghost"} : std::vector<std::string>{"Child", "Last"};
    SimulationController controller(options);
    control_silkit::HubTransport transport(control.hub(), controller.runtime());

    std::unique_ptr<ParticipantRuntime> childStatus;
    std::unique_ptr<ParticipantRuntime> lastStatus;
    if (!deadline)
    {
        // This in-process participant supplies the Child status while the real child process
        // below exits separately, making both observations available in the first check.
        childStatus = std::make_unique<ParticipantRuntime>(ParticipantRuntimeOptions{"Child", registry.uri});
        childStatus->start(vehicle_sil::test::scaled_timeout(20s));
        CHECK(controller.runtime().wait_for_peer("Child", [](const PeerStatus& s) { return s.state.has_value(); },
                                                 vehicle_sil::test::scaled_timeout(20s)));
    }
    CHECK(participants.start_children(registry.uri, RunParticipants::Which::Others));
    const long pid = vehicle_sil::test::read_pid((dir.path / "child.pid").string(), 20s);
    CHECK(pid > 0);
    CHECK(vehicle_sil::test::wait_gone(pid, 20s));
    if (!deadline)
    {
        lastStatus = std::make_unique<ParticipantRuntime>(ParticipantRuntimeOptions{"Last", registry.uri});
        lastStatus->start(vehicle_sil::test::scaled_timeout(20s));
        CHECK(controller.runtime().wait_for_peer("Last", [](const PeerStatus& s) { return s.state.has_value(); },
                                                 vehicle_sil::test::scaled_timeout(20s)));
        CHECK(controller.missing_participants().empty());
    }
    CHECK(!start.wait_for_start(controller, transport));
    CHECK(!controller.runtime().started());
    CHECK(outcome.participants[0].ending == Ending::ExitedBeforeStart);
    if (deadline)
    {
        CHECK(outcome.start.timed_out);
        CHECK(outcome.participants[0].missing_at_start);
        CHECK(outcome.end.failed_participant.empty());
        std::vector<std::string> childFindings;
        for (const Finding& finding : assess_run(outcome))
        {
            if (finding.participant == "Ghost")
            {
                childFindings.push_back(finding.code);
            }
        }
        CHECK(childFindings == std::vector<std::string>{"ended-before-start"});
    }
    else
    {
        CHECK(!outcome.start.timed_out);
        CHECK(outcome.end.failed_participant == "Child");
    }
}

} // namespace

TEST_CASE(a_child_exit_takes_priority_when_the_last_participant_has_joined)
{
    check_start_boundary(false);
}

TEST_CASE(a_child_exit_and_start_deadline_in_the_same_check_keep_the_existing_findings)
{
    check_start_boundary(true);
}

TEST_CASE(scenario_watch_keeps_controller_completion_ahead_when_already_stopping)
{
    using namespace vehicle_sil::runner::detail;
    using State = ScenarioWatchWait::State;
    for (bool finishes : {false, true})
    {
        auto now = std::chrono::steady_clock::time_point{};
        std::vector<std::chrono::milliseconds> waits;
        ScenarioWatchWait wait;
        wait.now = [&] { return now; };
        wait.scenario = [&](std::chrono::milliseconds timeout) {
            CHECK(timeout == 20ms);
            now += 5ms;
            return true;
        };
        wait.stopping = [] { return true; };
        wait.controller = [&](std::chrono::milliseconds duration) -> std::optional<State> {
            waits.push_back(duration);
            if (duration > 0ms && finishes)
            {
                return State::Shutdown;
            }
            return std::nullopt;
        };
        const auto result = wait_scenario_watch(wait, 20ms);
        CHECK(result.has_value() == finishes);
        if (finishes)
        {
            CHECK(*result == State::Shutdown);
        }
        CHECK((waits == std::vector<std::chrono::milliseconds>{0ms, 15ms}));
    }
    // While still running, completion is handled immediately; no second poll is spent.
    auto now = std::chrono::steady_clock::time_point{};
    std::vector<std::chrono::milliseconds> waits;
    ScenarioWatchWait wait;
    wait.now = [&] { return now; };
    wait.scenario = [&](std::chrono::milliseconds) { now += 5ms; return true; };
    wait.stopping = [] { return false; };
    wait.controller = [&](std::chrono::milliseconds duration) -> std::optional<State> {
        waits.push_back(duration);
        return std::nullopt;
    };
    CHECK(!wait_scenario_watch(wait, 20ms));
    CHECK((waits == std::vector<std::chrono::milliseconds>{0ms, 0ms}));

    // An already finished controller wins without waiting for a scenario.
    bool scenarioSeen = false;
    wait.controller = [](std::chrono::milliseconds) -> std::optional<State> { return State::Shutdown; };
    wait.scenario = [&](std::chrono::milliseconds) { scenarioSeen = true; return true; };
    CHECK(wait_scenario_watch(wait, 20ms) == State::Shutdown);
    CHECK(!scenarioSeen);

    // A timeout does not trigger the stopping path. A fractional remainder rounds up.
    waits.clear();
    wait.controller = [&](std::chrono::milliseconds duration) -> std::optional<State> {
        waits.push_back(duration);
        return std::nullopt;
    };
    wait.stopping = [] { return true; };
    wait.scenario = [&](std::chrono::milliseconds) { now += 20ms; return false; };
    CHECK(!wait_scenario_watch(wait, 20ms));
    CHECK((waits == std::vector<std::chrono::milliseconds>{0ms, 0ms}));
    waits.clear();
    wait.scenario = [&](std::chrono::milliseconds) { now += 5ms + 500us; return true; };
    CHECK(!wait_scenario_watch(wait, 20ms));
    CHECK((waits == std::vector<std::chrono::milliseconds>{0ms, 15ms}));
    waits.clear();
    wait.scenario = [&](std::chrono::milliseconds) { now += 20ms; return true; };
    CHECK(!wait_scenario_watch(wait, 20ms));
    CHECK((waits == std::vector<std::chrono::milliseconds>{0ms, 0ms}));
}

TEST_CASE(scenario_completion_notification_survives_both_wait_orders)
{
    using namespace vehicle_sil::runner::detail;
    CheckReport check;
    check.file = "sim.yaml";
    check.config.emplace();
    ParticipantConfig member;
    member.name = "Member";
    check.config->participants.push_back(member);
    check.participants.emplace_back();
    RunPlan plan = plan_run(check, nullptr, ".", RunChoices{});
    RunObservers observers;
    StopRequests stops;
    RunControls controls(stops);
    Scenario read;
    RunContext ctx{plan.config(), plan.check, plan, observers, nullptr, &read, controls, controls.time()};
    RunOutcome outcome = planned_outcome(plan);
    RunOutput output(ctx);
    RunControl control(ctx, output);
    RunSamples samples({}, {});
    RunScenario scenario(ctx, output, control, samples);
    vehicle_sil::unit::InProcessRegistry registry;
    SimulationControllerOptions options;
    options.registry_uri = registry.uri;
    options.required_participants = {"Member"};
    SimulationController controller(options);

    std::uint64_t generation = 0;
    std::promise<void> entering;
    auto waiting = std::async(std::launch::async, [&] {
        return scenario.wait_for_completion(generation, 5s, [&] { entering.set_value(); });
    });
    CHECK(entering.get_future().wait_for(1s) == std::future_status::ready);
    CHECK(waiting.wait_for(20ms) == std::future_status::timeout);
    // No engine was prepared: this is the completion path when a scenario could not begin.
    scenario.start(controller, outcome);
    CHECK(waiting.wait_for(1s) == std::future_status::ready);
    CHECK(waiting.get());
    CHECK(scenario.over());
    CHECK_EQ(generation, std::uint64_t{1});
    std::uint64_t earlier = 0;
    CHECK(scenario.wait_for_completion(earlier, 5s)); // notification before this wait
    CHECK_EQ(earlier, std::uint64_t{1});
    auto next = std::async(std::launch::async, [&] { return scenario.wait_for_completion(generation, 5s); });
    CHECK(next.wait_for(20ms) == std::future_status::timeout); // an observed notification does not spin
    scenario.start(controller, outcome);
    CHECK(next.wait_for(1s) == std::future_status::ready);
    CHECK(next.get());
    CHECK_EQ(generation, std::uint64_t{2});
}

TEST_CASE(a_timeline_that_dropped_points_or_was_not_written_is_said)
{
    // A record of one sampled variable that keeps 2 points, given 5 samples of 5 values.
    Scenario scenario;
    scenario.variables.resize(1);
    scenario.steps.resize(1);
    scenario.steps[0].conditions.resize(1);
    const auto record = [&scenario](int samples) {
        auto out = std::make_shared<TimelineRecord>(scenario, TimelineLimits{2, 100});
        for (int i = 0; i < samples; ++i)
        {
            out->sampled(0, SimTime{std::chrono::milliseconds(10 * i)}, control::Value{std::int64_t{i}});
        }
        out->close(SimTime{std::chrono::milliseconds(100)});
        return out;
    };
    RunOutcome o = passed_run();
    o.scenario.timeline = "out/t.json";
    o.scenario.timeline_record = record(5);
    check_text(lines(assess_run(o)),
               "N 0 timeline-limit: the timeline out/t.json does not have the whole run: 3 older points were dropped at its "
               "limits (the newest points, up to the end of the scenario, are all there). limits.dropped in the file says of "
               "which series and of which time; for a file with every point, record a shorter scenario\n");
    o.scenario.timeline_record = record(3);
    CHECK(lines(assess_run(o)).find("1 older point was dropped") != std::string::npos);
    // Nothing dropped, and a record without a file (no --timeline): nothing is said.
    o.scenario.timeline_record = record(2);
    check_text(lines(assess_run(o)), "");
    o.scenario.timeline_record = record(5);
    o.scenario.timeline.clear();
    check_text(lines(assess_run(o)), "");
    // The file could not be written: an error, whatever the scenario made.
    o.scenario.timeline = "out/t.json";
    o.scenario.timeline_record = record(2);
    o.scenario.timeline_error = "writing 10 bytes to it failed (is the disk full?)";
    check_text(lines(assess_run(o)),
               "E 0 timeline-write: the timeline out/t.json could not be written: writing 10 bytes to it failed (is the disk "
               "full?); what is in the file is not complete: do not use it. The scenario's result is not changed by this. Make "
               "room on the disk, or name another file with --timeline, and run again\n");
}

TEST_CASE(what_is_settled_before_the_participants_end_is_in_the_findings_of_the_end)
{
    // What the launcher knows when the simulation is over, with Engine and Brake still running...
    std::vector<RunOutcome> middles;
    const auto running = [](RunOutcome o) {
        o.participants = {participant("Engine", 5), participant("Brake", 7), participant("Tool", 9, false)};
        return o;
    };
    RunOutcome o = passed_run();
    middles.push_back(running(o));
    o.end.simulation = SimulationEnd::Aborted;
    o.end.failed_participant = "Engine";
    o.end.failure_reason = "left the simulation (state Error): it failed or lost its connection; see its log for the cause";
    middles.push_back(running(o));
    o.end.failure_reason.clear();
    middles.push_back(running(o));
    o = passed_run();
    o.end.simulation = SimulationEnd::Aborted;
    o.end.timed_out = true;
    o.end.timeout_ms = 500;
    middles.push_back(running(o));
    o = passed_run();
    o.end.simulation = SimulationEnd::Interrupted;
    o.end.interrupted_by = "SIGINT";
    middles.push_back(running(o));
    o = passed_run();
    o.end.simulation = SimulationEnd::Ended;
    o.end.controller_state = "Error";
    middles.push_back(running(o));
    o = passed_run();
    o.end.simulation = SimulationEnd::NotStarted;
    o.start.timed_out = true;
    middles.push_back(running(o));
    middles.back().participants[1].missing_at_start = true;
    o.start.timed_out = false;
    o.end.failed_participant = "Engine";
    middles.push_back(running(o));
    middles.back().participants[0] = exited(participant("Engine", 5), 3);
    middles.back().participants[0].ending = Ending::ExitedBeforeStart;
    o.end.failed_participant.clear();
    o.start.error = "the controller said no";
    middles.push_back(running(o));
    // ... and every way each of them may end after that.
    const auto ends = [](const ParticipantRun& p) {
        std::vector<ParticipantRun> out;
        if (p.exit)
        {
            return std::vector<ParticipantRun>{p}; // it had ended
        }
        for (const int code : {0, 1})
        {
            out.push_back(exited(p, code));
            ParticipantRun stopped = exited(p, code);
            stopped.ending = Ending::Stopped;
            out.push_back(stopped);
            stopped.stopped_after_end = true;
            out.push_back(stopped);
        }
        ParticipantRun killed = p;
        killed.ending = Ending::Killed;
        out.push_back(killed);
        killed.stopped_after_end = true;
        killed.left_processes = true;
        out.push_back(killed);
        killed.stop_request_failed = true;
        out.push_back(killed);
        return out;
    };
    std::size_t compared = 0;
    for (const RunOutcome& middle : middles)
    {
        const std::vector<Finding> settled = vehicle_sil::runner::detail::settled_findings(middle);
        for (const ParticipantRun& engine : ends(middle.participants[0]))
        {
            for (const ParticipantRun& brake : ends(middle.participants[1]))
            {
                for (const ParticipantRun& tool : ends(middle.participants[2]))
                {
                    for (const bool lateInterrupt : {false, true})
                    {
                        RunOutcome end = middle;
                        end.participants = {engine, brake, tool};
                        if (lateInterrupt && end.end.interrupted_by.empty())
                        {
                            end.end.interrupted_by = "SIGINT";
                            end.end.interrupted_while_ending = true;
                        }
                        const std::vector<Finding> final = assess_run(end);
                        for (const Finding& f : settled)
                        {
                            if (std::find(final.begin(), final.end(), f) == final.end())
                            {
                                throw vehicle_sil::unit::Failure("a settled finding is not in the findings of the end:\n" +
                                                                 lines({f}) + "--- the end ---\n" + lines(final));
                            }
                            ++compared;
                        }
                    }
                }
            }
        }
    }
    CHECK(compared > 1000);

    // Left out: only the participant that failed while its process still runs. Its finding names
    // how the process ended, so it is written when that is known.
    check_text(lines(vehicle_sil::runner::detail::settled_findings(middles[1])), "");
    CHECK(lines(assess_run(middles[1])).find("participant-failed") != std::string::npos);
    RunOutcome ended = middles[1];
    ended.participants[0] = exited(participant("Engine", 5), 7);
    check_text(lines(vehicle_sil::runner::detail::settled_findings(ended)), lines(assess_run(ended)));
    CHECK(lines(vehicle_sil::runner::detail::settled_findings(ended)).find("its process exited with code 7") != std::string::npos);
    // Everything else is written at once.
    check_text(lines(vehicle_sil::runner::detail::settled_findings(middles[3])), lines(assess_run(middles[3])));
    check_text(lines(vehicle_sil::runner::detail::settled_findings(middles[4])), lines(assess_run(middles[4])));
    check_text(lines(vehicle_sil::runner::detail::settled_findings(middles[6])), lines(assess_run(middles[6])));
}

TEST_CASE(a_participant_that_does_not_end_after_the_simulation_is_an_error)
{
    RunOutcome o = passed_run();
    o.end.stop_timeout_ms = 300;
    ParticipantRun stuck = participant("Stuck", 5);
    stuck.stopped_after_end = true;
    stuck.ending = Ending::Stopped;
    stuck.exit = ExitStatus{9, std::nullopt};
    ParticipantRun deaf = participant("Deaf", 7);
    deaf.stopped_after_end = true;
    deaf.ending = Ending::Killed;
    ParticipantRun alone = participant("Alone", 9);
    alone.stopped_after_end = true;
    alone.stop_request_failed = true;
    alone.ending = Ending::Killed;
    // An optional participant that was stopped: the normal way it ends.
    ParticipantRun logger = participant("Logger", 11, false);
    logger.ending = Ending::Stopped;
    logger.exit = ExitStatus{0, std::nullopt};
    o.participants = {stuck, deaf, alone, logger};
    check_text(lines(assess_run(o)),
               "E 5 did-not-end: participant 'Stuck': it was still running 300 ms (stop_timeout_ms) after the simulation "
               "ended: the launcher asked it to stop (" + kStop + "), and its process exited with code 9; a participant "
               "should end when the simulation ends (the lines [Stuck] above show what it wrote)\n"
               "E 7 did-not-end: participant 'Deaf': it was still running 300 ms (stop_timeout_ms) after the simulation "
               "ended: the launcher asked it to stop (" + kStop + "), and as it did not end within 300 ms either, ended "
               "it with the processes it started; a participant should end when the simulation ends (the lines [Deaf] "
               "above show what it wrote)\n"
               "E 9 did-not-end: participant 'Alone': it was still running 300 ms (stop_timeout_ms) after the simulation "
               "ended: the launcher could not ask it to stop (no console to send " + kStop + " through) and ended it "
               "with the processes it started; a participant should end when the simulation ends (the lines [Alone] above "
               "show what it wrote)\n");
    // When the simulation never ran, stopping is not the participant's fault.
    o.end.simulation = SimulationEnd::NotStarted;
    o.start.error = "it did not start";
    check_text(lines(assess_run(o)), "E 0 simulation-start: the simulation did not start: it did not start\n");
}

TEST_CASE(processes_left_behind_and_a_participant_that_cannot_start)
{
    RunOutcome o = passed_run();
    o.participants[0].left_processes = true;
    ParticipantRun missing = participant("Tool", 9, false);
    missing.start_error = "cannot start /x/tool: No such file or directory";
    o.participants.push_back(missing);
    ParticipantRun required = participant("Ecu", 11);
    required.start_error = "cannot start /x/ecu: Permission denied";
    o.participants.push_back(required);
    check_text(lines(assess_run(o)),
               "E 11 start: participant 'Ecu': cannot start /x/ecu: Permission denied: check the command in the "
               "configuration file\n"
               "W 5 left-processes: participant 'Engine': processes it started were still running after it had ended: the "
               "launcher ended them; end them in the participant before it exits\n"
               "W 9 start: participant 'Tool': cannot start /x/tool: No such file or directory: check the command in the "
               "configuration file\n");
}

TEST_CASE(a_connection_of_the_launcher_that_did_not_close_is_a_warning_beside_the_result)
{
    RunOutcome o = passed_run();
    o.close.not_closed_ms = 8000;
    check_text(lines(assess_run(o)),
               "W 0 connection-not-closed: the launcher's own connection to the simulation (SIL Kit) had not closed 8000 "
               "ms after the participants had ended: the launcher ended without closing it. The result is complete. This "
               "is not expected of any configuration: it is a defect of the launcher or of SIL Kit. Report it with this "
               "result, the launcher's output and what the participants were doing (was one stopped in a debugger?)\n");
    // One more interrupt ended the wait: the launcher did as it was asked, and that is no finding.
    o.close.interrupted = true;
    check_text(lines(assess_run(o)), "");
}

TEST_CASE(a_close_that_ends_in_time_is_waited_for_and_joined)
{
    std::atomic<bool> ran{false};
    const auto left = vehicle_sil::runner::detail::close_within([&ran] { ran = true; }, 10s, 1ms, [] { return false; });
    CHECK(!left);
    CHECK(ran.load()); // joined: it has run when close_within() returns
}

TEST_CASE(a_close_that_does_not_end_is_left_at_the_limit_and_may_end_later)
{
    struct Latch
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool open = false;
        bool ended = false;
    };
    const auto latch = std::make_shared<Latch>();
    const auto close = [latch] {
        std::unique_lock lock(latch->mutex);
        latch->changed.wait(lock, [&latch] { return latch->open; });
        latch->ended = true;
        latch->changed.notify_all();
    };
    const auto began = std::chrono::steady_clock::now();
    const auto left = vehicle_sil::runner::detail::close_within(close, 100ms, 5ms, [] { return false; });
    const auto took = std::chrono::steady_clock::now() - began;
    CHECK(left.has_value());
    CHECK(left && !left->interrupted);
    CHECK(left && left->waited >= 100ms);
    CHECK(took >= 100ms);
    // The thread that was left ends its close when it can, on its own.
    std::unique_lock lock(latch->mutex);
    latch->open = true;
    latch->changed.notify_all();
    CHECK(latch->changed.wait_for(lock, 30s, [&latch] { return latch->ended; }));
}

TEST_CASE(a_close_that_does_not_end_is_left_before_the_limit_when_interrupted)
{
    const auto open = std::make_shared<std::promise<void>>();
    const std::shared_future<void> opened = open->get_future().share();
    std::atomic<int> asked{0};
    const auto began = std::chrono::steady_clock::now();
    const auto left = vehicle_sil::runner::detail::close_within([opened] { opened.wait(); }, 60s, 2ms, [&asked] { return ++asked > 3; });
    const auto took = std::chrono::steady_clock::now() - began;
    open->set_value();
    CHECK(left.has_value());
    CHECK(left && left->interrupted);
    CHECK(left && left->waited < 30s);
    CHECK(took < 30s);
}

namespace {

/// What the parts of a connection note as they go (ConnectionOf with parts that do nothing else),
/// and the latch that the transport's end waits for, as SIL Kit's may wait for a connection.
struct Going
{
    std::mutex mutex;
    std::condition_variable changed;
    bool open = true; ///< false: the transport does not go until it is set
    std::vector<std::string> order;
    bool hub_was_there = false; ///< when the transport went

    void note(const std::string& part)
    {
        {
            const std::lock_guard lock(mutex);
            order.push_back(part);
        }
        changed.notify_all();
    }
    [[nodiscard]] std::vector<std::string> noted()
    {
        const std::lock_guard lock(mutex);
        return order;
    }
};

struct NotedHub
{
    explicit NotedHub(std::shared_ptr<Going> g) : going(std::move(g)) {}
    ~NotedHub() { going->note("hub"); }
    std::shared_ptr<Going> going;
};

struct NotedRegistry
{
    explicit NotedRegistry(std::shared_ptr<Going> g) : going(std::move(g)) {}
    ~NotedRegistry() { going->note("registry"); }
    std::shared_ptr<Going> going;
};

struct NotedController
{
    explicit NotedController(std::shared_ptr<Going> g) : going(std::move(g)) {}
    ~NotedController() { going->note("controller"); }
    std::shared_ptr<Going> going;
};

/// As the hub's transport: it does not hold the hub, and tells it at its end that the links are over.
struct NotedTransport
{
    NotedTransport(std::shared_ptr<Going> g, std::weak_ptr<NotedHub> h) : going(std::move(g)), hub(std::move(h)) {}
    ~NotedTransport()
    {
        {
            std::unique_lock lock(going->mutex);
            going->changed.wait(lock, [this] { return going->open; });
            going->hub_was_there = !hub.expired();
        }
        going->note("transport");
    }
    std::shared_ptr<Going> going;
    std::weak_ptr<NotedHub> hub;
};

using NotedConnection = vehicle_sil::runner::detail::ConnectionOf<NotedHub, NotedRegistry, NotedController, NotedTransport>;

/// A connection with all its parts, made in the run's order. `hub`: the maker's own hold of the hub.
std::shared_ptr<NotedConnection> noted_connection(const std::shared_ptr<Going>& going, const std::shared_ptr<NotedHub>& hub)
{
    auto connection = std::make_shared<NotedConnection>(hub);
    connection->registry = std::make_unique<NotedRegistry>(going);
    connection->controller = std::make_unique<NotedController>(going);
    connection->transport = std::make_unique<NotedTransport>(going, hub);
    return connection;
}

} // namespace

TEST_CASE(a_connection_closes_its_transport_then_its_controller_then_its_registry_and_keeps_the_hub_for_its_holders)
{
    const auto going = std::make_shared<Going>();
    auto hub = std::make_shared<NotedHub>(going);
    const auto left =
        vehicle_sil::runner::detail::close_connection_within(noted_connection(going, hub), 60s, 1ms, [] { return false; });
    CHECK(!left);
    // Closed and joined: the three have gone in the order, and the hub is still the holder's.
    CHECK(going->noted() == (std::vector<std::string>{"transport", "controller", "registry"}));
    CHECK(going->hub_was_there);
    hub.reset();
    CHECK(going->noted() == (std::vector<std::string>{"transport", "controller", "registry", "hub"}));
}

TEST_CASE(a_connection_that_was_not_handed_to_a_close_goes_in_the_same_order_with_its_last_holder)
{
    // A run that leaves before its end (a setup that failed): the connection goes where it was made.
    const auto going = std::make_shared<Going>();
    auto connection = noted_connection(going, std::make_shared<NotedHub>(going));
    CHECK(going->noted().empty());
    connection.reset();
    CHECK(going->noted() == (std::vector<std::string>{"transport", "controller", "registry", "hub"}));
    CHECK(going->hub_was_there);
}

TEST_CASE(a_connection_is_closed_once_whoever_closes_it)
{
    const auto going = std::make_shared<Going>();
    auto connection = noted_connection(going, std::make_shared<NotedHub>(going));
    connection->close();
    connection->close();
    CHECK(going->noted() == (std::vector<std::string>{"transport", "controller", "registry"}));
    connection.reset();
    CHECK(going->noted() == (std::vector<std::string>{"transport", "controller", "registry", "hub"}));
}

TEST_CASE(a_connection_whose_close_was_left_keeps_itself_and_the_hub_until_the_close_has_ended)
{
    const auto going = std::make_shared<Going>();
    going->open = false;
    std::weak_ptr<NotedHub> hub;
    std::weak_ptr<NotedConnection> watched;
    std::optional<vehicle_sil::runner::detail::CloseLeft> left;
    {
        // The run's holds: the hub's and the connection's. Both are given up here, as by a caller
        // that destroys the run although its close was left.
        auto held = std::make_shared<NotedHub>(going);
        hub = held;
        auto connection = noted_connection(going, held);
        watched = connection;
        left = vehicle_sil::runner::detail::close_connection_within(std::move(connection), 100ms, 5ms, [] { return false; });
        CHECK(connection == nullptr); // NOLINT(bugprone-use-after-move): the hold was given away
    }
    CHECK(left.has_value());
    CHECK(left && !left->interrupted);
    // The closing thread stands in the transport's end: nothing has gone, and nothing can be
    // taken from under it.
    CHECK(going->noted().empty());
    CHECK(!watched.expired());
    CHECK(!hub.expired());
    // It ends when it can, in the order, the hub last and after the transport has told it.
    std::unique_lock lock(going->mutex);
    going->open = true;
    going->changed.notify_all();
    CHECK(going->changed.wait_for(lock, 30s, [&going] { return going->order.size() == 4; }));
    CHECK(going->order == (std::vector<std::string>{"transport", "controller", "registry", "hub"}));
    CHECK(going->hub_was_there);
    CHECK(hub.expired());
}

namespace {

/// The hub's transport of a connection made of the real parts, whose end waits for a latch
/// first: a close that does not end for as long as the test wants.
struct HeldTransport
{
    struct Latch
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool open = false;
    };

    HeldTransport(std::shared_ptr<Latch> l, control::Hub& hub, ParticipantRuntime& runtime) :
      latch(std::move(l)), transport(hub, runtime)
    {
    }
    ~HeldTransport()
    {
        // The real transport goes after this body, when the latch is open.
        std::unique_lock lock(latch->mutex);
        latch->changed.wait(lock, [this] { return latch->open; });
    }

    std::shared_ptr<Latch> latch;
    control_silkit::HubTransport transport;
};

using HeldConnection =
    vehicle_sil::runner::detail::ConnectionOf<control::Hub, vehicle_sil::unit::InProcessRegistry, SimulationController, HeldTransport>;

/// What a run has beside its connection, and the connection's parts are given pointers to (as
/// the Runner of run.cpp): what the start window's function names, and the hub's tap.
struct RunParts
{
    explicit RunParts(std::shared_ptr<control::Hub> h) : hub(std::move(h)) {}
    std::shared_ptr<control::Hub> hub;
    vehicle_sil::runner::detail::RunSamples samples{{}, {}};
    int windows = 0; ///< the start window's thread; read once start() has returned
};

} // namespace

TEST_CASE(what_a_run_has_beside_its_connection_can_be_destroyed_while_a_close_that_was_left_still_goes_on)
{
    // The real hub, registry, controller and transport, a simulation that started (the start
    // window's function ran) and ended, and a close that stands in the transport's end beyond its
    // limit. The run's own parts are destroyed then, as run_file() destroys its Runner; the
    // closing thread ends later and must not have used them (under AddressSanitizer and
    // ThreadSanitizer this is the test; the count of the window's calls says it everywhere).
    const auto latch = std::make_shared<HeldTransport::Latch>();
    const auto window_calls = std::make_shared<std::atomic<int>>(0);
    // Where the hub was destroyed, told once it has gone.
    // Held by the deleter too, so a left closing thread that ends after this test gave up waiting
    // still writes into a live promise.
    const auto hub_gone = std::make_shared<std::promise<std::thread::id>>();
    std::future<std::thread::id> hub_went = hub_gone->get_future();
    std::weak_ptr<HeldConnection> watched;
    std::optional<vehicle_sil::runner::detail::CloseLeft> left;
    {
        auto parts = std::make_unique<RunParts>(std::shared_ptr<control::Hub>(new control::Hub, [hub_gone](control::Hub* hub) {
            delete hub;
            hub_gone->set_value(std::this_thread::get_id());
        }));
        auto connection = std::make_shared<HeldConnection>(parts->hub);
        watched = connection;
        connection->registry = std::make_unique<vehicle_sil::unit::InProcessRegistry>();
        SimulationControllerOptions options;
        options.registry_uri = connection->registry->uri;
        options.required_participants = {"Ecu"};
        options.stop_at = 10ms;
        connection->controller = std::make_unique<SimulationController>(options);
        connection->transport = std::make_unique<HeldTransport>(latch, *connection->hub, connection->controller->runtime());
        connection->controller->set_start_window([run = parts.get(), window_calls](const StartWindow&) {
            ++run->windows;
            ++*window_calls;
        });
        parts->samples.tap(*parts->hub, 1ms, [c = connection->controller.get()] { return c->runtime().now(); });
        {
            // The run's one participant, which ends before the close as a run's participants do.
            ParticipantRuntime ecu(vehicle_sil::unit::coordinated_options("Ecu", connection->registry->uri, VirtualTimeOptions{1ms}));
            ecu.set_step_handler([](SimTime, SimDuration) {});
            std::thread start_ecu([&ecu] { ecu.start(60s); });
            connection->controller->start(60s);
            start_ecu.join();
            CHECK_EQ(parts->windows, 1);
            CHECK(connection->controller->wait_until_finished(vehicle_sil::test::scaled_timeout(30s)).has_value());
            ecu.stop("done");
        }
        parts->samples.untap();
        left = vehicle_sil::runner::detail::close_connection_within(std::move(connection), 100ms, 5ms, [] { return false; });
        // Here the run's parts go: the tap's owner, what the window's function names, and the
        // run's hold of the hub.
    }
    CHECK(left.has_value());
    CHECK(!watched.expired());
    CHECK(hub_went.wait_for(0s) == std::future_status::timeout); // the connection keeps the hub
    {
        const std::lock_guard lock(latch->mutex);
        latch->open = true;
    }
    latch->changed.notify_all();
    // The closing thread destroys the transport, the controller and the registry, and then the
    // hub, as its last holder.
    const bool ended = hub_went.wait_for(vehicle_sil::test::scaled_timeout(60s)) == std::future_status::ready;
    CHECK(ended);
    if (ended)
    {
        CHECK(hub_went.get() != std::this_thread::get_id());
    }
    CHECK(watched.expired());
    CHECK_EQ(window_calls->load(), 1); // not called again by the controller's end
}

namespace {

/// A check of `config` in which each participant described itself with the step of the same place
/// in `steps_us` (microseconds; 0: an ECU in real time, without a step; -1: not described).
vehicle_sil::config::CheckReport described(const vehicle_sil::config::SimulationConfig& config, const std::vector<std::int64_t>& steps_us)
{
    vehicle_sil::config::CheckReport check;
    check.config = config;
    for (std::size_t i = 0; i < config.participants.size(); ++i)
    {
        vehicle_sil::config::ParticipantResult r;
        if (!config.participants[i].enabled)
        {
            r.status = vehicle_sil::config::ParticipantStatus::Disabled;
        }
        else if (steps_us[i] < 0)
        {
            r.status = vehicle_sil::config::ParticipantStatus::NotChecked;
        }
        else
        {
            r.status = vehicle_sil::config::ParticipantStatus::Described;
            r.ecu.emplace();
            r.ecu->description.name = config.participants[i].name;
            if (steps_us[i] > 0)
            {
                r.ecu->description.step = std::chrono::microseconds(steps_us[i]);
            }
        }
        check.participants.push_back(std::move(r));
    }
    return check;
}

vehicle_sil::config::SimulationConfig config_of(std::size_t participants, std::optional<std::int64_t> time_step_ms = std::nullopt)
{
    vehicle_sil::config::SimulationConfig config;
    config.time_step_ms = time_step_ms;
    for (std::size_t i = 0; i < participants; ++i)
    {
        vehicle_sil::config::ParticipantConfig p;
        p.name = "Ecu" + std::to_string(i);
        config.participants.push_back(p);
    }
    return config;
}

struct Plan
{
    std::int64_t step_ms;
    std::optional<std::int64_t> ecu_step_us;
    std::optional<std::int64_t> delay_us;
    std::int64_t fine_ms;
    friend bool operator==(const Plan&, const Plan&) = default;
};

Plan plan(const vehicle_sil::config::SimulationConfig& config, const std::vector<std::int64_t>& steps_us, std::optional<std::int64_t> stop_at_ms = std::nullopt)
{
    const RunTiming t = vehicle_sil::config::detail::plan_time_step(config, described(config, steps_us), stop_at_ms);
    CHECK(t.time_step_written == config.time_step_ms.has_value());
    const auto us = [](const std::optional<std::int64_t>& ns) { return ns ? std::optional<std::int64_t>(*ns / 1000) : std::nullopt; };
    return Plan{t.time_step_ms, us(t.ecu_step_ns), us(t.step_delay_ns), t.fine_step_ms};
}

} // namespace

TEST_CASE(the_controller_steps_as_finely_as_the_ecus_and_stop_time_need)
{
    // With a speed: the greatest common divisor of 10 ms and the ECUs' steps, so that every ECU
    // step starts at a controller step.
    CHECK(plan(config_of(2), {1000, 10000}) == (Plan{1, 1000, 0, 1}));
    CHECK(plan(config_of(2), {5000, 10000}) == (Plan{5, 5000, 0, 5}));
    CHECK(plan(config_of(2), {10000, 20000}) == (Plan{10, 10000, 0, 10}));
    // Not the smallest step: 4 and 6 ms meet only every 2 ms.
    CHECK(plan(config_of(2), {4000, 6000}) == (Plan{2, 4000, 0, 2}));
    // Larger steps than 10 ms do not make it coarser (pausing and stepping stay as fine).
    CHECK(plan(config_of(2), {20000, 100000}) == (Plan{10, 20000, 0, 10}));
    // And it divides stop_at_ms.
    CHECK(plan(config_of(2), {5000, 10000}, 1003) == (Plan{1, 5000, 0, 1}));
    CHECK(plan(config_of(1), {10000}, 1005) == (Plan{5, 10000, 0, 5}));
    CHECK(plan(config_of(1), {5000}, 1001) == (Plan{1, 5000, 0, 1}));
    // An ECU in real time has no step and does not count; no ECU with a step: 10 ms.
    CHECK(plan(config_of(2), {0, 5000}) == (Plan{5, 5000, 0, 5}));
    CHECK(plan(config_of(1), {0}) == (Plan{10, std::nullopt, 0, 10}));
    // A participant that does not describe itself (describe: false): 1 ms, to be safe, and what
    // the ECUs' steps are is not known.
    CHECK(plan(config_of(2), {-1, 10000}) == (Plan{1, std::nullopt, std::nullopt, 1}));
    // A disabled participant does not count, described or not.
    vehicle_sil::config::SimulationConfig disabled = config_of(2);
    disabled.participants[0].enabled = false;
    CHECK(plan(disabled, {-1, 10000}) == (Plan{10, 10000, 0, 10}));
    // A step that is no whole number of milliseconds: 1 ms, and what is left is told.
    CHECK(plan(config_of(1), {2500}) == (Plan{1, 2500, 500, 1}));
    // time_step_ms as written stays, with a speed too: 10 ms over 1 ms steps runs them up to 9 ms
    // late, 10 ms over 15 ms steps up to 5 ms (15 ms waits for 20 ms), 20 ms over 10 ms steps 10 ms.
    CHECK(plan(config_of(1, 10), {15000}) == (Plan{10, 15000, 5000, 5}));
    CHECK(plan(config_of(1, 20), {10000}) == (Plan{20, 10000, 10000, 10}));
    CHECK(plan(config_of(1, 2), {10000}) == (Plan{2, 10000, 0, 10}));
}

TEST_CASE(the_participants_in_real_time_are_the_described_ecus_and_test_participants_without_a_step)
{
    vehicle_sil::config::SimulationConfig config = config_of(7);
    config.participants[1].role = vehicle_sil::config::ParticipantRole::Test;
    config.participants[3].role = vehicle_sil::config::ParticipantRole::Bridge; // a bridge locks the time: not one of them
    config.participants[4].enabled = false;
    config.participants[5].role = vehicle_sil::config::ParticipantRole::Bridge; // as is a bridge of --connect (with_bridges())
    // Ecu0 and Ecu1 (a test participant) describe no step; Ecu2 has one; Ecu6 is not described.
    const std::vector<std::string> peers =
        vehicle_sil::config::detail::real_time_peers(config, described(config, {0, 0, 10'000, 0, 0, 0, -1}));
    CHECK((peers == std::vector<std::string>{"Ecu0", "Ecu1"}));
    CHECK(vehicle_sil::config::detail::real_time_peers(config, described(config, {10'000, 5'000, 10'000, 0, 0, 0, -1})).empty());
    // A check that has fewer results than the file has participants (it stopped early): no reach beyond.
    vehicle_sil::config::CheckReport cut = described(config, {0, 0, 0, 0, 0, 0, 0});
    cut.participants.resize(1);
    CHECK((vehicle_sil::config::detail::real_time_peers(config, cut) == std::vector<std::string>{"Ecu0"}));
}

TEST_CASE(what_ends_just_as_an_interrupt_comes_has_ended)
{
    using vehicle_sil::runner::detail::wait_for;
    using vehicle_sil::runner::detail::Waited;
    using namespace std::chrono_literals;
    // The window: the launcher looks (not done), then the awaited thing happens and an interrupt
    // comes, then the launcher looks at the interrupts. Without a second look the wait would be
    // "interrupted" although what it waited for has happened (a participant that ended by itself
    // would be told as stopped by the launcher instead of waited for).
    int looks = 0;
    bool interrupt = false;
    CHECK(wait_for(10s, 1ms, [&] {
              interrupt = true; // comes right after the first look ...
              return ++looks > 1; // ... as does the end
          },
          [&] { return interrupt; }) == Waited::Done);
    CHECK_EQ(looks, 2);
    // Not done when the interrupt comes: interrupted, at once.
    looks = 0;
    CHECK(wait_for(10s, 1ms, [&] { ++looks; return false; }, [] { return true; }) == Waited::Interrupted);
    CHECK_EQ(looks, 2);
    // Done at the first look: the interrupts are not looked at. Never done: the timeout.
    CHECK(wait_for(10s, 1ms, [] { return true; }, [] { return true; }) == Waited::Done);
    CHECK(wait_for(20ms, 1ms, [] { return false; }, [] { return false; }) == Waited::TimedOut);
    // Done later.
    looks = 0;
    CHECK(wait_for(10s, 1ms, [&] { return ++looks > 3; }, [] { return false; }) == Waited::Done);
}

TEST_CASE(the_participants_reach_a_registry_on_every_address_at_the_loopback_address)
{
    CHECK(vehicle_sil::runner::detail::uri_for_participants("silkit://0.0.0.0:8500") == "silkit://127.0.0.1:8500");
    CHECK(vehicle_sil::runner::detail::uri_for_participants("silkit://[::]:8500") == "silkit://[::1]:8500");
    CHECK(vehicle_sil::runner::detail::uri_for_participants("silkit://192.168.1.2:8500") == "silkit://192.168.1.2:8500");
    CHECK(vehicle_sil::runner::detail::uri_for_participants("silkit://127.0.0.1:41234") == "silkit://127.0.0.1:41234");
}

TEST_CASE(the_time_event_at_the_start_says_paused_only_if_it_stands_still_at_0)
{
    using vehicle_sil::runner::detail::noted_standing;
    using vehicle_sil::runner::detail::TimeNote;
    using namespace std::chrono_literals;
    const SimTime zero{};
    const auto state = [](SimTime now, bool paused, std::optional<SimTime> target) {
        TimeState s;
        s.now = now;
        s.paused = paused;
        s.pause_target = target;
        return s;
    };
    // The event at the start is the one of time 0. A step from the hold before the first step,
    // as fast as possible, may stand still at its end before the launcher looks: that is not
    // standing still at 0, and the next look says where it stands.
    CHECK(!noted_standing(state(zero + 100ms, true, std::nullopt), TimeNote::Start));
    CHECK(noted_standing(state(zero + 100ms, true, std::nullopt), TimeNote::Poll));
    CHECK(!noted_standing(state(zero + 100ms, false, zero + 100ms), TimeNote::Start));
    CHECK(!noted_standing(state(zero + 10ms, false, zero), TimeNote::Start));
    // Standing still at 0 (--start-paused), or about to (the pause at 0 that the gate has not
    // taken yet).
    CHECK(noted_standing(state(zero, true, std::nullopt), TimeNote::Start));
    CHECK(noted_standing(state(zero, false, zero), TimeNote::Start));
    // On its way to a later time, or just running.
    CHECK(!noted_standing(state(zero, false, zero + 100ms), TimeNote::Start));
    CHECK(!noted_standing(state(zero, false, std::nullopt), TimeNote::Start));
    CHECK(!noted_standing(state(zero + 50ms, false, zero + 100ms), TimeNote::Poll));
    // At the target, standing still with the next step; not once the simulation is over.
    CHECK(noted_standing(state(zero + 100ms, false, zero + 100ms), TimeNote::Poll));
    CHECK(!noted_standing(state(zero + 100ms, false, zero + 100ms), TimeNote::Last));
    // An ended simulation stands still nowhere, whatever its gate says (one aborted while it stood
    // still: the gate keeps paused until the abort has gone round, at 0 or later).
    CHECK(!noted_standing(state(zero + 100ms, true, std::nullopt), TimeNote::Last));
    CHECK(!noted_standing(state(zero, true, std::nullopt), TimeNote::Last));
    CHECK(!noted_standing(state(zero, true, zero), TimeNote::Last));
}

TEST_CASE(a_simulation_that_stands_still_is_told_of_at_10_s_60_s_and_10_min)
{
    using vehicle_sil::runner::detail::StandStill;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point t0{};
    const std::string why = " ms, not paused): a participant may have lost its network connection (on another computer), "
                            "stand still in a debugger, or hang in a step; the run waits on (a run with --timeout-ms "
                            "ends when that has passed)";
    StandStill s;
    CHECK(s.look(0, false, std::nullopt, t0).empty());
    CHECK(s.look(10, false, std::nullopt, t0 + 1s).empty()); // it moves
    CHECK(s.look(10, false, std::nullopt, t0 + 10s).empty()); // 9 s
    CHECK(s.look(10, false, std::nullopt, t0 + 11s) == "virtual time has not advanced for 10 s (at t=10" + why);
    CHECK(s.look(10, false, std::nullopt, t0 + 12s).empty()); // once
    CHECK(s.look(10, false, std::nullopt, t0 + 61s) == "virtual time has not advanced for 60 s (at t=10" + why);
    CHECK(s.look(10, false, std::nullopt, t0 + 300s).empty());
    CHECK(s.look(10, false, std::nullopt, t0 + 601s) == "virtual time has not advanced for 600 s (at t=10" + why);
    CHECK(s.look(10, false, std::nullopt, t0 + 6000s).empty()); // not again
    CHECK(s.look(20, false, std::nullopt, t0 + 6001s) == "virtual time advances again: it stood still for 6000 s at t=10 ms");
    CHECK(s.look(30, false, std::nullopt, t0 + 6002s).empty());
    // Paused: it stands still on purpose; after the pause it counts anew.
    CHECK(s.look(30, true, std::nullopt, t0 + 6100s).empty());
    CHECK(s.look(30, false, std::nullopt, t0 + 6105s).empty());
    CHECK(s.look(30, true, std::nullopt, t0 + 6200s).empty());
    CHECK(s.look(30, false, std::nullopt, t0 + 6209s).empty()); // 9 s since the pause
    CHECK(!s.look(30, false, std::nullopt, t0 + 6211s).empty());
    // Paused after a line: no "again" (it was paused, not freed).
    CHECK(s.look(30, true, std::nullopt, t0 + 6212s).empty());
    // It stops: the line that it moved, once, if it did after a line; then nothing.
    StandStill ends;
    CHECK(ends.look(10, false, std::nullopt, t0).empty());
    CHECK(!ends.look(10, false, std::nullopt, t0 + 10s).empty());
    CHECK(ends.stopping(300, t0 + 11s) == "virtual time advances again: it stood still for 11 s at t=10 ms");
    CHECK(ends.stopping(300, t0 + 12s).empty());
    CHECK(ends.look(300, false, std::nullopt, t0 + 100s).empty());
    StandStill stuck;
    CHECK(stuck.look(10, false, std::nullopt, t0).empty());
    CHECK(!stuck.look(10, false, std::nullopt, t0 + 10s).empty());
    CHECK(stuck.stopping(10, t0 + 11s).empty()); // stopped where it stood
    // Stopping before any look, and again: nothing (the first look never came).
    StandStill early;
    CHECK(early.stopping(10, t0 + 100s).empty());
    CHECK(early.stopping(20, t0 + 200s).empty());
    CHECK(early.look(30, false, std::nullopt, t0 + 300s).empty());
    // At a set speed: only the time past the next step's deadline counts.
    StandStill paced;
    CHECK(paced.look(0, false, std::chrono::nanoseconds(0), t0).empty());
    CHECK(paced.look(0, false, std::chrono::nanoseconds(0), t0 + 100s).empty()); // a slow speed: on time
    CHECK(paced.look(0, false, std::chrono::nanoseconds(9s), t0 + 109s).empty());
    CHECK(paced.look(0, false, std::chrono::nanoseconds(70s), t0 + 170s) == "virtual time has not advanced for 70 s (at t=0" + why);
}

TEST_CASE(a_participant_outside_the_file_is_named_when_it_waits_for_the_start)
{
    using vehicle_sil::runner::detail::StrangerWait;
    using State = StrangerWait::State;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point t0{};
    StrangerWait w;
    // In ServicesCreated: named once it has stayed there for 2 s.
    CHECK(!w.look("hand", State::ServicesCreated, t0));
    CHECK(!w.look("hand", State::ServicesCreated, t0 + 1999ms));
    CHECK(w.look("hand", State::ServicesCreated, t0 + 2000ms));
    // An autonomous one passes it on its way to Running: not named, and it counts anew.
    CHECK(!w.look("Sender", State::ServicesCreated, t0));
    CHECK(!w.look("Sender", State::Other, t0 + 100ms));
    CHECK(!w.look("Sender", State::Other, t0 + 5000ms));
    CHECK(!w.look("Sender", State::ServicesCreated, t0 + 6000ms));
    CHECK(!w.look("Sender", State::ServicesCreated, t0 + 7999ms));
    CHECK(w.look("Sender", State::ServicesCreated, t0 + 8000ms));
    // In Error: at once.
    CHECK(w.look("Late", State::Error, t0));
    CHECK(!w.look("Other", State::Other, t0));
}

TEST_CASE(a_registry_that_cannot_listen_is_told_the_likely_cause)
{
    using vehicle_sil::runner::detail::HostAddress;
    using vehicle_sil::runner::detail::registry_listen_hint;
    const std::vector<HostAddress> here = {{"198.51.100.5", true}, {"[2001:db8::10]", false}};
    // An IPv4 address that is not this computer's, with port 0 or not, written or not.
    const std::string elsewhere = " (192.0.2.1 is not an address of this computer, and the registry listens on this computer: "
                                  "write in registry_uri one of this computer's addresses (198.51.100.5, [2001:db8::10]), "
                                  "0.0.0.0 for all of them, or 127.0.0.1 for this computer only)";
    CHECK(registry_listen_hint("silkit://192.0.2.1:0", true, false, here) == elsewhere);
    CHECK(registry_listen_hint("silkit://192.0.2.1:8500", true, false, here) == elsewhere);
    CHECK(registry_listen_hint("silkit://192.0.2.1:0", true, false, {}) ==
          " (192.0.2.1 is not an address of this computer, and the registry listens on this computer: write in registry_uri "
          "0.0.0.0 for all of this computer's addresses, or 127.0.0.1 for this computer only)");
    // This computer's, with a fixed port: another registry may have it.
    const std::string busy = " (does another registry run there? choose another port in registry_uri, or port 0 for a free one)";
    for (const std::string uri : {"silkit://127.0.0.1:8500", "silkit://0.0.0.0:8500", "silkit://198.51.100.5:8500",
                                  "silkit://[::]:8500", "silkit://localhost:8500", "silkit://[::1]:8500"})
    {
        CHECK(registry_listen_hint(uri, true, false, here) == busy);
    }
    // ... and with port 0 nothing has the port: SIL Kit's message is all there is.
    CHECK(registry_listen_hint("silkit://0.0.0.0:0", true, false, here).empty());
    CHECK(registry_listen_hint("silkit://127.0.0.1:0", true, false, here).empty());
    // A name (or an IPv6 address, which may be written in more than one way): it may be another computer's.
    CHECK(registry_listen_hint("silkit://other-pc:0", true, false, here) ==
          " (is other-pc an address of this computer, where the registry listens? If not, write in registry_uri one of this "
          "computer's addresses (198.51.100.5, [2001:db8::10]), 0.0.0.0 for all of them, or 127.0.0.1 for this computer only)");
    CHECK(registry_listen_hint("silkit://[2001:db8::99]:8500", true, false, here) ==
          " (does another registry run there, or is [2001:db8::99] not an address of this computer? Choose another port in "
          "registry_uri (port 0 for a free one), or write there one of this computer's addresses (198.51.100.5, "
          "[2001:db8::10]), 0.0.0.0 for all of them, or 127.0.0.1 for this computer only)");
    // Not written: the port of a run with a participant started by hand, or a free one (nothing to add).
    CHECK(registry_listen_hint("silkit://127.0.0.1:8500", false, true, here) ==
          " (a run with a participant you start yourself (manual) listens there if the file has no registry_uri, so that its "
          "command stays the same; does another registry run there? write another port in registry_uri, or port 0 for a free "
          "one: the command then changes from run to run)");
    CHECK(registry_listen_hint("silkit://127.0.0.1:0", false, false, here).empty());
}

TEST_CASE(other_computers_are_told_where_to_join)
{
    using vehicle_sil::runner::detail::HostAddress;
    using vehicle_sil::runner::detail::other_computers_line;
    const std::string tail = ": give it as --registry-uri to a participant started by hand on another computer (instead of "
                             "the loopback address in its command; it needs no VEHICLE_SIL_LISTEN_ON_NETWORK)";
    CHECK(other_computers_line("silkit://0.0.0.0:8500", {{"192.0.2.10", true}}) ==
          "from other computers, join at silkit://192.0.2.10:8500 (this computer's address)" + tail);
    // The first on the network of the default route (host_addresses() puts it first).
    CHECK(other_computers_line("silkit://0.0.0.0:8500", {{"192.0.2.10", true}, {"198.51.100.5", false}, {"172.17.0.1", false}}) ==
          "from other computers, join at silkit://192.0.2.10:8500, silkit://198.51.100.5:8500 or silkit://172.17.0.1:8500 "
          "(this computer's addresses; the first is on the network of its default route, usually the one the others reach)" +
              tail);
    // None or all with the default route: nothing to tell them apart.
    CHECK(other_computers_line("silkit://0.0.0.0:8500", {{"192.0.2.10", false}, {"198.51.100.5", false}}) ==
          "from other computers, join at silkit://192.0.2.10:8500 or silkit://198.51.100.5:8500 (this computer's addresses)" +
              tail);
    CHECK(other_computers_line("silkit://0.0.0.0:8500", {{"192.0.2.10", true}, {"198.51.100.5", true}}) ==
          "from other computers, join at silkit://192.0.2.10:8500 or silkit://198.51.100.5:8500 (this computer's addresses)" +
              tail);
    // [::]: IPv6 addresses; where it takes no IPv4 (Windows), the line says how to get it.
    CHECK(other_computers_line("silkit://[::]:8501", {{"[2001:db8::10]", true}}, true) ==
          "from other computers, join at silkit://[2001:db8::10]:8501 (this computer's address; it listens on IPv6 only "
          "here: for IPv4, write registry_uri: silkit://0.0.0.0:8501)" + tail);
    CHECK(other_computers_line("silkit://[::]:8501", {{"192.0.2.10", true}, {"[2001:db8::10]", true}}) ==
          "from other computers, join at silkit://192.0.2.10:8501 or silkit://[2001:db8::10]:8501 (this computer's addresses)" +
              tail);
    CHECK(other_computers_line("silkit://0.0.0.0:8500", {}) ==
          "from other computers, join at silkit://<an address of this computer>:8500 (none found here: ipconfig on "
          "Windows, ip addr elsewhere, shows them)" + tail);
    CHECK(other_computers_line("silkit://[::]:8500", {}, true) ==
          "from other computers, join at silkit://<an address of this computer>:8500 (none found here: ipconfig on "
          "Windows, ip addr elsewhere, shows them; it listens on IPv6 only here: for IPv4, write registry_uri: "
          "silkit://0.0.0.0:8500)" + tail);
    // The file has port 0: the way to IPv4 keeps port 0, not the port this run took.
    CHECK(other_computers_line("silkit://[::]:41234", {{"[2001:db8::10]", true}}, true, "0") ==
          "from other computers, join at silkit://[2001:db8::10]:41234 (this computer's address; it listens on IPv6 only "
          "here: for IPv4, write registry_uri: silkit://0.0.0.0:0)" + tail);
    // A registry on one address: that one, whatever this computer has.
    CHECK(other_computers_line("silkit://192.0.2.10:8500", {{"198.51.100.5", true}}) ==
          "from other computers, join at silkit://192.0.2.10:8500" + tail);
    // Which addresses are listed: not 0/8, loopback or link-local.
    using vehicle_sil::runner::detail::reachable_ipv4_text;
    const auto text = [](std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
        const std::uint8_t bytes[] = {a, b, c, d};
        return reachable_ipv4_text(bytes);
    };
    CHECK(text(192, 0, 2, 18) == "192.0.2.18");
    CHECK(text(10, 0, 2, 255) == "10.0.2.255");
    CHECK(text(169, 253, 1, 1) == "169.253.1.1");
    CHECK(text(127, 0, 0, 1).empty());
    CHECK(text(127, 255, 0, 9).empty());
    CHECK(text(169, 254, 3, 4).empty());
    CHECK(text(0, 0, 0, 0).empty());
    using vehicle_sil::runner::detail::reachable_ipv6_text;
    const auto text6 = [](std::initializer_list<std::uint8_t> head, std::uint8_t last) {
        std::uint8_t bytes[16] = {};
        std::copy(head.begin(), head.end(), bytes);
        bytes[15] = last;
        return reachable_ipv6_text(bytes);
    };
    CHECK(text6({0x20, 0x01, 0x0d, 0xb8}, 0x10) == "[2001:db8::10]");
    CHECK(text6({0xfd, 0x12}, 1) == "[fd12::1]"); // unique local: in a LAN
    CHECK(text6({0xfe, 0xc0}, 1) == "[fec0::1]");
    CHECK(text6({}, 0).empty());                 // ::
    CHECK(text6({}, 1).empty());                 // ::1
    CHECK(text6({0xfe, 0x80}, 1).empty());       // link-local
    CHECK(text6({0xfe, 0xbf}, 1).empty());       // link-local too (fe80::/10)
    CHECK(text6({0xff, 0x02}, 1).empty());       // multicast
    CHECK(text6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2}, 10).empty()); // ::ffff:192.0.2.10
    // The interfaces with a default route (Linux: /proc/net/route, /proc/net/ipv6_route).
    const std::string v4 = "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
                           "eth0\t00000000\t0100A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
                           "eth0\t0000A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0\n"
                           "docker0\t000011AC\t00000000\t0001\t0\t0\t0\t0000FFFF\t0\t0\t0\n"
                           "wg0\t00000000\t00000000\t0000\t0\t0\t0\t00000000\t0\t0\t0\n"       // not up
                           "virbr0\t00000000\t00000000\t0201\t0\t0\t0\t00000000\t0\t0\t0\n"    // unreachable
                           "tun0\t00000000\t00000000\t0001\t0\t0\t0\tFFFFFFFF\t0\t0\t0\n";     // 0.0.0.0/32
    const std::string zero = std::string(32, '0');
    const std::string v6 = zero + " 00 " + zero + " 00 fe800000000000000000000000000001 00000400 00000001 00000000 00000003 wlan0\n" +
                           zero + " 00 " + zero + " 00 " + zero + " ffffffff 00000001 00000000 00200200 lo\n" + // unreachable
                           zero + " 00 " + zero + " 00 " + zero + " 00000400 00000001 00000000 00000201 eth9\n" + // rejects
                           "fe800000000000000000000000000000 40 " + zero + " 00 " + zero + " 00000100 00000001 00000000 00000001 eth0\n";
    CHECK((vehicle_sil::runner::detail::default_route_interfaces(v4, v6) == std::vector<std::string>{"eth0", "wlan0"}));
    CHECK(vehicle_sil::runner::detail::default_route_interfaces("", "").empty());
    // The order: those with the default route first, IPv4 before IPv6, otherwise as they are.
    const auto order = vehicle_sil::runner::detail::in_order(
        {{"[2001:db8::5]", false}, {"172.17.0.1", false}, {"[2001:db8::10]", true}, {"198.51.100.5", false}, {"192.0.2.10", true}});
    std::string seen;
    for (const HostAddress& a : order)
    {
        seen += a.host + (a.default_route ? "*" : "") + " ";
    }
    if (seen != "192.0.2.10* [2001:db8::10]* 172.17.0.1 198.51.100.5 [2001:db8::5] ")
    {
        std::fprintf(stderr, "the order: %s\n", seen.c_str());
    }
    CHECK(seen == "192.0.2.10* [2001:db8::10]* 172.17.0.1 198.51.100.5 [2001:db8::5] ");
    // This computer's addresses: none loopback or link-local, those with the default route first.
    const auto here = vehicle_sil::runner::detail::host_addresses(true, true);
    bool before = true;
    for (const HostAddress& a : here)
    {
        CHECK(!a.host.empty() && a.host.rfind("127.", 0) != 0 && a.host.rfind("169.254.", 0) != 0 && a.host != "0.0.0.0");
        CHECK(a.host.front() == '[' ? a.host.back() == ']' && a.host.rfind("[fe80", 0) != 0 && a.host != "[::1]"
                                    : std::count(a.host.begin(), a.host.end(), '.') == 3);
        CHECK(before || !a.default_route);
        before = a.default_route;
    }
    for (const HostAddress& a : vehicle_sil::runner::detail::host_addresses(true, false))
    {
        CHECK(a.host.front() != '[');
    }
    for (const HostAddress& a : vehicle_sil::runner::detail::host_addresses(false, true))
    {
        CHECK(a.host.front() == '[');
    }
}

TEST_CASE(a_participants_line_is_shown_without_control_characters_and_its_log_has_a_plain_name)
{
    CHECK(vehicle_sil::runner::detail::printable("a\tb\x1b[2Jc\rd\x7f") == "a\tb?[2Jc?d?");
    CHECK(vehicle_sil::runner::detail::printable("\xC3\xBC ok") == "\xC3\xBC ok"); // UTF-8 stays
    CHECK(vehicle_sil::runner::detail::log_name("Engine_2.a-b") == "Engine_2.a-b");
    CHECK(vehicle_sil::runner::detail::log_name("ECU/1 \xC3\xBC:x") == "ECU_1____x"); // one "_" per byte
}

TEST_CASE(a_participant_that_ended_just_before_the_stop_request_ended_by_itself)
{
    // The request found it ended: its exit is recorded, and it is no "did-not-end".
    ParticipantRun run = participant("Engine", 5);
    run.stopped_after_end = true;
    CHECK(vehicle_sil::runner::detail::record_stop_request(run, false, ExitStatus{3, std::nullopt}) == vehicle_sil::runner::detail::StopRequest::HadEnded);
    CHECK(run.ending == Ending::Exited);
    CHECK(run.exit == (ExitStatus{3, std::nullopt}));
    CHECK(!run.stopped_after_end);
    RunOutcome o = passed_run();
    o.participants[0] = run;
    check_text(lines(assess_run(o)), "E 5 exit-code: participant 'Engine': its process exited with code 3; the lines "
                                     "[Engine] above show what it wrote\n");
    // Even with the flag left on, an end by itself is no "did-not-end".
    o.participants[0].stopped_after_end = true;
    check_text(lines(assess_run(o)), "E 5 exit-code: participant 'Engine': its process exited with code 3; the lines "
                                     "[Engine] above show what it wrote\n");
    // Sent, or it runs and cannot be asked.
    ParticipantRun asked = participant("Brake", 7);
    CHECK(vehicle_sil::runner::detail::record_stop_request(asked, true, std::nullopt) == vehicle_sil::runner::detail::StopRequest::Sent);
    CHECK(asked.ending == Ending::NotStarted && !asked.stop_request_failed);
    CHECK(vehicle_sil::runner::detail::record_stop_request(asked, false, std::nullopt) == vehicle_sil::runner::detail::StopRequest::CannotSend);
    CHECK(asked.stop_request_failed);
    // A status that is not there is not read (the text without it).
    ParticipantRun unknown = participant("Odd", 9);
    unknown.ending = Ending::ExitedBeforeStart;
    o.participants = {unknown};
    o.end.simulation = SimulationEnd::NotStarted;
    check_text(lines(assess_run(o)), "E 9 ended-before-start: participant 'Odd': it ended before the simulation started: "
                                     "its process ended, so the simulation could not start; the lines [Odd] above show "
                                     "what it wrote\n");
}

TEST_CASE(the_ecu_waits_for_the_start_longer_than_the_launcher)
{
    SimulationConfig c;
    using vehicle_sil::runner::detail::ecu_start_timeout_ms;
    CHECK_EQ(ecu_start_timeout_ms({}, false, 8000, 2000), std::int64_t{20000});
    CHECK(ecu_start_timeout_ms({}, false, 8000, 2000) > 8000 + 2000);
    CHECK_EQ(ecu_start_timeout_ms({}, true, 8000, 2000), kMaxMillis);
    CHECK_EQ(ecu_start_timeout_ms({}, false, kMaxMillis, kMaxMillis), kMaxMillis);
    CHECK_EQ(ecu_start_timeout_ms({}, false, kMaxMillis - 10000, 1), kMaxMillis);
}

TEST_CASE(a_participant_is_started_with_its_network_names_after_everything_else)
{
    using vehicle_sil::runner::detail::participant_arguments;
    const auto words = [](const std::vector<std::string>& list) {
        std::string out;
        for (const std::string& word : list)
        {
            out += "[" + word + "]";
        }
        return out + "\n";
    };
    SimulationConfig config;
    ParticipantConfig p;
    p.name = "Engine";
    p.command = {"engine/engine_ecu", "--verbose"};
    const std::string timeout = std::to_string(vehicle_sil::runner::detail::ecu_start_timeout_ms({}, false, 2000, 10000));
    const std::string own = "[--verbose][--name][Engine][--registry-uri][silkit://127.0.0.1:8500][--start-timeout-ms][" + timeout + "]";
    // Without network_names: what it was.
    check_text(words(participant_arguments(config, p, "silkit://127.0.0.1:8500", false, false, 2000, 10000)), own + "\n");
    // In the file's order and direction (<simulation's name>=<executable's name>), after the
    // launcher's own arguments; a line whose two names are the same is not given.
    p.network_names = {NetworkName{"VehicleCAN", "J1939_CAN", 5}, NetworkName{"Body", "Body", 6},
                       NetworkName{"Body CAN", "CAN_B", 7}};
    check_text(words(participant_arguments(config, p, "silkit://127.0.0.1:8500", false, false, 2000, 10000)),
               own + "[--map-network][VehicleCAN=J1939_CAN][--map-network][Body CAN=CAN_B]\n");
    // describe: false: after its command with the placeholders replaced, and a name that looks
    // like a placeholder stays as it is.
    p.describe = false;
    p.command = {"tools/remote", "--uri", "{registry_uri}", "--as", "{name}"};
    p.network_names = {NetworkName{"{name}", "{registry_uri}", 5}};
    check_text(words(participant_arguments(config, p, "silkit://127.0.0.1:8500", false, false, 2000, 10000)),
               "[--uri][silkit://127.0.0.1:8500][--as][Engine][--map-network][{name}={registry_uri}]\n");
    p.network_names.clear();
    check_text(words(participant_arguments(config, p, "silkit://127.0.0.1:8500", false, false, 2000, 10000)), "[--uri][silkit://127.0.0.1:8500][--as][Engine]\n");
}

TEST_CASE(a_participant_is_started_with_its_j1939_nodes_after_its_network_names)
{
    using vehicle_sil::runner::detail::participant_arguments;
    const auto words = [](const std::vector<std::string>& list) {
        std::string out;
        for (const std::string& word : list)
        {
            out += "[" + word + "]";
        }
        return out + "\n";
    };
    SimulationConfig config;
    ParticipantConfig p;
    p.name = "Engine2";
    p.command = {"engine/engine_ecu"};
    const std::string timeout = std::to_string(vehicle_sil::runner::detail::ecu_start_timeout_ms({}, false, 30000, 10000));
    const std::string own = "[--name][Engine2][--registry-uri][silkit://127.0.0.1:8500][--start-timeout-ms][" + timeout + "]";
    // After --map-network, in the file's order; an entry that changes nothing is not given.
    p.network_names = {NetworkName{"VehicleCAN", "J1939_CAN", 5}};
    p.j1939_nodes = {J1939NodeEntry{{"VehicleCAN", j1939::NodeOverride{0x00, 0x01, 2, {}, {}}}, 7},
                     J1939NodeEntry{{"VehicleCAN", j1939::NodeOverride{0x21, {}, {}, {}, {}}}, 8},
                     J1939NodeEntry{{"BodyCAN", j1939::NodeOverride{{}, 0xFE, {}, {}, {}}}, 9}};
    check_text(words(participant_arguments(config, p, "silkit://127.0.0.1:8500", false, false)),
               own + "[--map-network][VehicleCAN=J1939_CAN][--j1939-node][VehicleCAN=from:0x00,address:0x01,identity_number:2]"
                     "[--j1939-node][BodyCAN=address:254]\n");
    // describe: false: after its command with the placeholders replaced.
    p.describe = false;
    p.command = {"tools/remote", "--uri", "{registry_uri}"};
    p.network_names.clear();
    check_text(words(participant_arguments(config, p, "silkit://127.0.0.1:8500", false, false)),
               "[--uri][silkit://127.0.0.1:8500][--j1939-node][VehicleCAN=from:0x00,address:0x01,identity_number:2]"
               "[--j1939-node][BodyCAN=address:254]\n");
}

TEST_CASE(the_command_to_start_a_participant_by_hand_is_quoted_for_the_shell)
{
    using vehicle_sil::runner::detail::shell_command_line;
    const std::vector<std::string> args{"--name", "Engine", "--registry-uri", "silkit://127.0.0.1:8500", "", "a b",
                                        "it's", "say \"hi\"", "C:\\dir\\", "%PATH%", "R&D",
                                        "VehicleCAN=from:0x00,address:0x01", "a;b"};
#ifdef _WIN32
    // As CommandLineToArgvW reads it back: backslashes double only before a quote. A word with ','
    // or ';' in quotes too: PowerShell reads a,b as an array (two arguments) and ';' as an end.
    check_text(shell_command_line("C:\\My ECUs\\engine.exe", args) + "\n",
               "\"C:\\My ECUs\\engine.exe\" --name Engine --registry-uri silkit://127.0.0.1:8500 \"\" \"a b\" it's "
               "\"say \\\"hi\\\"\" C:\\dir\\ %PATH% \"R&D\" \"VehicleCAN=from:0x00,address:0x01\" \"a;b\"\n");
#else
    check_text(shell_command_line("/opt/my ecus/engine", args) + "\n",
               "'/opt/my ecus/engine' --name Engine --registry-uri silkit://127.0.0.1:8500 '' 'a b' 'it'\\''s' "
               "'say \"hi\"' 'C:\\dir\\' %PATH% 'R&D' VehicleCAN=from:0x00,address:0x01 'a;b'\n");
#endif
}

TEST_CASE(a_participants_line_shows_no_c1_control_and_no_broken_utf8)
{
    CHECK(vehicle_sil::runner::detail::printable("a\xC2\x9B" "1m\xC2\xA0" "b") == "a?1m\xC2\xA0" "b"); // C1 CSI, then NBSP
    CHECK(vehicle_sil::runner::detail::printable("\xFF\xC3") == "??");                       // not UTF-8, cut short
    CHECK(vehicle_sil::runner::detail::printable("\xE2\x82\xAC \xF0\x9F\x98\x80") == "\xE2\x82\xAC \xF0\x9F\x98\x80");
    CHECK(vehicle_sil::runner::detail::printable("\xC0\xAF\xED\xA0\x80") == "?????");        // overlong, a surrogate
}

// The limit of the start, in the orders the main thread and the start window's thread can meet
// (no clock: the times are given).
TEST_CASE(the_start_limit_of_a_run_without_manual_participants_is_the_one_set)
{
    using vehicle_sil::runner::detail::StartLimit;
    const StartLimit::Clock::time_point t0{std::chrono::seconds(100)};
    const std::chrono::milliseconds length(5000);
    StartLimit limit;
    CHECK(limit.at() == StartLimit::Clock::time_point::max());
    limit.set(t0 + length, length);
    // Whether participants started by hand are ready does not matter in a run without them.
    for (const bool ready : {false, true})
    {
        const StartLimit::Now now = limit.take(t0 + std::chrono::seconds(1), ready);
        CHECK(now.at == t0 + length);
        CHECK(now.length == length);
        CHECK(!now.after_hold);
    }
    // In the hold: none. After it: anew from its end, and the message says so.
    limit.begin_hold();
    CHECK(limit.take(t0 + std::chrono::hours(1), true).at == StartLimit::Clock::time_point::max());
    const auto released = t0 + std::chrono::hours(2);
    limit.end_hold(released, length, false);
    const StartLimit::Now after = limit.take(released + std::chrono::seconds(1), false);
    CHECK(after.at == released + length);
    CHECK(after.length == length);
    CHECK(after.after_hold);
}

TEST_CASE(the_start_limit_with_manual_participants_begins_when_they_are_ready_and_never_in_the_hold)
{
    using vehicle_sil::runner::detail::StartLimit;
    const StartLimit::Clock::time_point t0{std::chrono::seconds(100)};
    const auto none = StartLimit::Clock::time_point::max();
    const std::chrono::milliseconds length(5000);
    {
        // Ready before the hold: the limit begins at that look, the hold takes it away.
        StartLimit limit;
        limit.set_once_manual_ready(length);
        CHECK(limit.take(t0, false).at == none);
        CHECK(limit.take(t0 + std::chrono::seconds(1), true).at == t0 + std::chrono::seconds(1) + length);
        CHECK(limit.take(t0 + std::chrono::seconds(2), true).at == t0 + std::chrono::seconds(1) + length); // begun once
        limit.begin_hold();
        CHECK(limit.take(t0 + std::chrono::hours(1), true).at == none);
    }
    {
        // The hold begins before the main thread has seen them ready (the window's thread was
        // first): the first look that finds them ready comes in the hold and sets no limit. A
        // limit set here would be one of 0 ms (the hold has no length), and the start would be
        // given up at once.
        StartLimit limit;
        limit.set_once_manual_ready(length);
        limit.begin_hold();
        const StartLimit::Now held = limit.take(t0, true);
        CHECK(held.at == none);
        // After the hold: from the first look that finds them ready, with the length of the start.
        const auto released = t0 + std::chrono::hours(1);
        limit.end_hold(released, length, true);
        CHECK(limit.take(released, false).at == none);
        const StartLimit::Now after = limit.take(released + std::chrono::seconds(3), true);
        CHECK(after.at == released + std::chrono::seconds(3) + length);
        CHECK(after.length == length);
        CHECK(after.after_hold);
    }
}

TEST_CASE(with_bridges_adds_a_participant_for_each_connected_network_after_those_of_the_file)
{
    CheckReport check;
    check.file = "sim.yaml";
    check.config = read_config("vehicle_sil_simulation: 2\n"
                               "networks:\n"
                               "  - { name: ETH, kind: ethernet }\n"
                               "  - { name: CAN1, kind: can }\n"
                               "participants:\n"
                               "  - name: Engine\n"
                               "    command: e\n")
                       .config;
    check.participants.resize(1);
    check.participants[0].status = ParticipantStatus::Described;
    // Without --connect: the same participants.
    CHECK_EQ(vehicle_sil::runner::detail::with_bridges(check).config->participants.size(), std::size_t{1});
    check.outside = {NetworkOutside{"CAN1", "vehicle_sil_vector_xl_bridge", "/t/xl", "virtual:1", "bridge.CAN1", std::nullopt},
                     NetworkOutside{"ETH", "vehicle_sil_ethernet_bridge", "/t/eth", "eth0", "bridge.ETH", std::nullopt}};
    const CheckReport run = vehicle_sil::runner::detail::with_bridges(check);
    CHECK(run.config->participants.size() == 3 && run.participants.size() == 3);
    if (run.config->participants.size() != 3 || run.participants.size() != 3)
    {
        return;
    }
    CHECK(run.config->participants[0].name == "Engine" && run.config->participants[0].role == ParticipantRole::Ecu);
    const ParticipantConfig& can = run.config->participants[1];
    CHECK(can.name == "bridge.CAN1" && can.command == std::vector<std::string>{"vehicle_sil_vector_xl_bridge"} && can.role == ParticipantRole::Bridge && !can.required &&
          can.enabled && !can.manual);
    CHECK_EQ(can.line, std::size_t{4});
    CHECK(run.config->participants[2].name == "bridge.ETH" && run.config->participants[2].line == 3);
    CHECK(run.participants[1].status == ParticipantStatus::NotChecked && run.participants[1].executable == "/t/xl" &&
          !run.participants[1].ecu);
    CHECK(run.participants[2].executable == "/t/eth");
    // The check itself is left as it was.
    CHECK_EQ(check.config->participants.size(), std::size_t{1});
}

TEST_CASE(the_line_of_a_bridge_that_says_why_it_ended)
{
    const auto failure = [](std::initializer_list<const char*> lines) {
        vehicle_sil::config::detail::LastLine last;
        for (const char* line : lines)
        {
            last.add(line);
        }
        return last.failure();
    };
    // A failure while it ran: what it writes after the FAIL line is not why.
    check_text(failure({"[eth-bridge] bridging SIL Kit network 'ETH' (participant bridge.ETH) <-> pcap 'eth0'; Ctrl+C to stop",
                        "[eth-bridge] FAIL: reading from 'eth0' failed: the interface went down\r",
                        "[eth-bridge] stopping", "[eth-bridge] stats silkit_to_ext=12 ext_to_silkit=11", "[eth-bridge] stopped"}),
               "[eth-bridge] FAIL: reading from 'eth0' failed: the interface went down");
    // Killed: no line says why (not its start line).
    check_text(failure({"[eth-bridge] bridging SIL Kit network 'ETH' (participant bridge.ETH) <-> pcap 'eth0'; Ctrl+C to stop"}),
               "");
    // It could not set up.
    check_text(failure({"[bridge] FAIL setup: no physical CAN channel 9; available: 1=\"Channel 1\""}),
               "[bridge] FAIL setup: no physical CAN channel 9; available: 1=\"Channel 1\"");
    // An argument it refuses (no tag), and a tag that is not one (a space in it) does not hide the word.
    check_text(failure({"error: --interface must not be empty", "Run 'vehicle_sil_ethernet_bridge --help' for the options."}),
               "error: --interface must not be empty");
    check_text(failure({"[not a tag] FAIL"}), "");
}

TEST_CASE(behind_real_time_says_that_what_is_connected_outside_does_not_wait)
{
    RunOutcome o = passed_run();
    o.timing.speed = 1.0;
    o.timing.time_step_ms = 10;
    o.timing.paced_steps = 100;
    o.timing.late_steps = 3;
    o.timing.max_late_ms = 40;
    o.timing.slipped_ms = 90;
    o.timing.late_tolerance_ms = 20;
    const auto message = [](const RunOutcome& outcome) {
        for (const Finding& f : assess_run(outcome))
        {
            if (f.code == "behind-real-time")
            {
                CHECK(f.severity == Severity::Warning);
                return f.message;
            }
        }
        return std::string();
    };
    const std::string plain = message(o);
    CHECK(plain.find("a device attached in real time got the ECUs' messages late by up to that. Lower the speed") !=
          std::string::npos);
    o.time_lock = TimeLock{true, {"ETH"}, {}};
    CHECK(message(o).find("got the ECUs' messages late by up to that (--connect connects ETH to a real bus: what is "
                          "connected there does not wait, so the run was that far off from it). Lower the speed") !=
          std::string::npos);
    // With --time-policy free the run is not kept to real time: the plain message.
    o.time_lock = TimeLock{false, {"ETH"}, {}};
    check_text(message(o), plain);
}

TEST_CASE(two_networks_whose_bridges_report_one_device_under_two_names_are_connect_target_shared)
{
    RunOutcome o = connected_run(stopped_bridge(0));
    o.outside[0].tool = "vehicle_sil_ethernet_bridge";
    o.outside[0].link.device = "ab12";
    ParticipantRun second = stopped_bridge(0);
    second.name = "bridge.ETH2";
    second.line = 4;
    second.for_network = "ETH2";
    o.participants.push_back(second);
    o.outside.push_back(RunOutside{"ETH2", "br0", "bridge.ETH2", std::nullopt, std::nullopt});
    o.outside[1].tool = "vehicle_sil_ethernet_bridge";
    o.outside[1].link.device = "ab12";
    const std::string found = lines(assess_run(o));
    CHECK(found.find("W 0 connect-target-shared: networks 'ETH' and 'ETH2': --connect gives them the same device under two names "
                     "('eth0' and 'br0'; the bridges report one device): the frames of both networks are mixed there: ") == 0);
    // The same name twice: the check has said it.
    o.outside[1].target = "eth0";
    CHECK(assess_run(o).empty());
    // Another device, or one that is not known: nothing.
    o.outside[1].target = "br0";
    o.outside[1].link.device = "cd34";
    CHECK(assess_run(o).empty());
    o.outside[1].link.device.clear();
    o.outside[0].link.device.clear();
    CHECK(assess_run(o).empty());
}

TEST_CASE(a_link_that_stayed_down_is_connect_lost_with_the_time_it_was_waited_for)
{
    RunOutcome o = connected_run(stopped_bridge(0));
    o.outside[0].tool = "vehicle_sil_ethernet_bridge";
    o.outside[0].link.down_count = 1;
    o.outside[0].link.down_total = 31s;
    o.outside[0].link.lost = "link down for 30 s";
    o.end.simulation = SimulationEnd::Aborted;
    o.end.failed_participant = "bridge.ETH";
    const std::string found = lines(assess_run(o));
    CHECK(found.find("E 3 connect-lost: network 'ETH': the connection to the real bus (eth0) was lost while the simulation was "
                     "running: bridge.ETH reported link down for 30 s") == 0);
    // connect-lost says it: not the same again.
    CHECK(found.find("outside-link-down") == std::string::npos);
    CHECK(vehicle_sil::runner::detail::settled_findings(o).empty());
}

TEST_CASE(why_a_run_ended_is_decided_by_the_setup_then_by_what_asked_first_then_by_the_simulation)
{
    using vehicle_sil::runner::end_reason;
    using vehicle_sil::runner::EndReason;
    using vehicle_sil::runner::StopKind;
    CHECK(end_reason(passed_run()) == EndReason::Completed);
    RunOutcome ended = passed_run();
    ended.end.simulation = SimulationEnd::Ended;
    CHECK(end_reason(ended) == EndReason::Completed);
    // The launcher stopped the simulation because the scenario was over: completed, not stopped.
    ended.end.stopped_by_scenario = true;
    CHECK(end_reason(ended) == EndReason::Completed);
    RunOutcome aborted = passed_run();
    aborted.end.simulation = SimulationEnd::Aborted;
    CHECK(end_reason(aborted) == EndReason::Failed);
    aborted.end.simulation = SimulationEnd::NotStarted;
    CHECK(end_reason(aborted) == EndReason::Failed);
    CHECK(end_reason(stopped_run("stop command", StopKind::Operator)) == EndReason::Stopped);
    CHECK(end_reason(stopped_run("control server stop", StopKind::Operator)) == EndReason::Stopped);
    CHECK(end_reason(stopped_run("SIGINT", StopKind::Signal)) == EndReason::Interrupted);
    CHECK(end_reason(stopped_run("end of input", StopKind::Supervisor)) == EndReason::Interrupted);
    CHECK(end_reason(stopped_run("output closed", StopKind::Supervisor)) == EndReason::Interrupted);
    // An operator's stop that came only while the participants were ending did not end the
    // simulation: its own end stays (failed, or completed).
    RunOutcome late = stopped_run("stop command", StopKind::Operator);
    late.end.simulation = SimulationEnd::Aborted;
    late.end.interrupted_while_ending = true;
    CHECK(end_reason(late) == EndReason::Failed);
    late.end.simulation = SimulationEnd::ReachedStopAt;
    CHECK(end_reason(late) == EndReason::Completed);
    late.end.simulation = SimulationEnd::Ended;
    late.end.stopped_by_scenario = true;
    CHECK(end_reason(late) == EndReason::Completed);
    // ... unless it cut short the end of a required participant still running: then it decided
    // how that one ended (asked to stop, or ended by force). An optional one does not count.
    RunOutcome cut = late;
    cut.participants[0].ending = Ending::Stopped;
    cut.participants[0].end_cut_short = true;
    CHECK(end_reason(cut) == EndReason::Stopped);
    cut.participants[0].ending = Ending::Killed;
    CHECK(end_reason(cut) == EndReason::Stopped);
    cut.participants[0].required = false;
    CHECK(end_reason(cut) == EndReason::Completed);
    // An interrupt decides at any time, also while the participants were ending.
    RunOutcome lateSignal = late;
    lateSignal.end.interrupted_by = "SIGINT";
    lateSignal.end.interrupted_kind = StopKind::Signal;
    CHECK(end_reason(lateSignal) == EndReason::Interrupted);
    lateSignal.end.interrupted_by = "output closed";
    lateSignal.end.interrupted_kind = StopKind::Supervisor;
    CHECK(end_reason(lateSignal) == EndReason::Interrupted);
    // A set-up that failed is stronger than a stop.
    late.setup.error = "no registry";
    CHECK(end_reason(late) == EndReason::SetupFailed);
    check_text(vehicle_sil::runner::end_reason_name(EndReason::Completed), "completed");
    check_text(vehicle_sil::runner::end_reason_name(EndReason::Stopped), "stopped");
    check_text(vehicle_sil::runner::end_reason_name(EndReason::Interrupted), "interrupted");
    check_text(vehicle_sil::runner::end_reason_name(EndReason::Failed), "failed");
    check_text(vehicle_sil::runner::end_reason_name(EndReason::SetupFailed), "setup_failed");
}

TEST_CASE(run_watch_wakes_immediately_on_non_signal_stop_for_plain_and_scenario_runs)
{
    using namespace vehicle_sil::runner::detail;
    using Clock = std::chrono::steady_clock;
    namespace orch = SilKit::Services::Orchestration;

    for (const bool with_scenario : {false, true})
    {
        for (const bool via_publish : {false, true})
        {
            CheckReport check;
            check.file = "sim.yaml";
            check.config.emplace();
            ParticipantConfig member;
            member.name = "Member";
            check.config->participants.push_back(member);
            check.participants.emplace_back();
            RunPlan plan = plan_run(check, nullptr, ".", RunChoices{});
            plan.poll = vehicle_sil::test::scaled_timeout(10s); // Without stop wake, watching would sleep up to the poll limit.
            RunObservers observers;
            StopRequests stops;
            RunControls controls(stops, true, {}, with_scenario);
            controls.set_time_lock({});
            Scenario read;
            RunContext ctx{plan.config(), plan.check, plan, observers, nullptr, with_scenario ? &read : nullptr, controls, controls.time()};
            RunOutcome outcome = planned_outcome(plan);
            RunOutput output(ctx);
            RunControl control(ctx, output);
            RunSamples samples({}, {});
            RunParticipants participants(ctx, output, outcome, plan.base);
            RunNotes notes(ctx, participants);
            RunScenario scenario(ctx, output, control, samples);
            RunStart start(ctx, output, control, notes, scenario, participants, outcome);
            RunWatch watch(ctx, output, notes, samples, scenario, start, participants, outcome);

            vehicle_sil::unit::InProcessRegistry registry;
            SimulationControllerOptions options;
            options.registry_uri = registry.uri;
            options.required_participants = {"Member"};
            options.time_control = TimeControlOptions{.step = 10ms, .start_paused = true};
            SimulationController controller(options);
            auto peer = std::make_unique<ParticipantRuntime>(
                vehicle_sil::unit::coordinated_options("Member", registry.uri, VirtualTimeOptions{10ms, std::nullopt}));
            peer->set_step_handler([](SimTime, SimDuration) {});
            vehicle_sil::unit::start_together(controller, {peer.get()}, 30s);
            CHECK(controller.wait_until_paused(vehicle_sil::test::scaled_timeout(10s)).has_value());

            RunWatch::Watched watched;
            std::thread watcher([&] { watched = watch.watch(controller); });
            std::this_thread::sleep_for(30ms);

            const auto t0 = Clock::now();
            if (via_publish)
            {
                stops.request(vehicle_sil::runner::stop_source::kEndOfInput);
                controls.publish();
            }
            else
            {
                Command stop;
                stop.name = "stop";
                stop.accepted = true;
                CHECK(controls.time().apply(stop, "stdin", [&stops] {
                    stops.request(vehicle_sil::runner::stop_source::kStopCommand);
                }).accepted);
            }
            watcher.join();
            const auto elapsed = Clock::now() - t0;
            peer->stop("test done");

            CHECK(watched.result.end == WatchEnd::Interrupted);
            CHECK(!watched.for_scenario);
            CHECK(watched.result.state == std::optional<orch::ParticipantState>(orch::ParticipantState::Shutdown));
            CHECK(elapsed < vehicle_sil::test::scaled_timeout(2s));
        }
    }
}

#ifndef _WIN32
namespace {

void bind_unix_socket(const std::filesystem::path& path)
{
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string native = path.native();
    CHECK(native.size() < sizeof(addr.sun_path));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    std::memcpy(addr.sun_path, native.c_str(), native.size() + 1);
    const int rc = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    ::close(fd);
    CHECK(rc == 0);
}

struct ScopedPathCleanup
{
    std::filesystem::path path;
    ~ScopedPathCleanup()
    {
        if (!path.empty())
        {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
};

struct RunnerEnvGuard
{
    std::string name;
    std::optional<std::string> previous;
    explicit RunnerEnvGuard(const char* key) : name(key)
    {
        if (const char* current = std::getenv(key); current != nullptr)
        {
            previous = current;
        }
    }
    ~RunnerEnvGuard()
    {
        if (previous)
        {
            static_cast<void>(::setenv(name.c_str(), previous->c_str(), 1));
        }
        else
        {
            static_cast<void>(::unsetenv(name.c_str()));
        }
    }
};

} // namespace
#endif

TEST_CASE(run_socket_dir_cleans_only_matching_sockets_and_resists_tampering)
{
    using vehicle_sil::runner::detail::RunSocketDir;
#ifdef _WIN32
    RunSocketDir dir;
    CHECK(dir.path().empty());
    dir.ensure_created();
    CHECK(dir.path().empty());
    dir.cleanup();
    CHECK(dir.path().empty());
#else
    namespace fs = std::filesystem;
    const RunnerEnvGuard tmpdir_guard("TMPDIR");
    CHECK(::setenv("TMPDIR", "/tmp", 1) == 0);

    // 1. Normal lifecycle: 0700 directory, removes matching socket and empty dir, idempotent.
    {
        RunSocketDir dir;
        CHECK(dir.path().empty());
        dir.ensure_created();
        CHECK(!dir.path().empty());
        const fs::path created = dir.path();
        dir.ensure_created();
        CHECK(dir.path() == created.native());

        struct stat st{};
        CHECK(::lstat(created.c_str(), &st) == 0);
        CHECK(S_ISDIR(st.st_mode));
        CHECK((st.st_mode & 0777) == 0700);

        const fs::path sock = created / "vsil-0123456789abcdef.silkit";
        bind_unix_socket(sock);
        CHECK(fs::exists(sock));

        dir.cleanup();
        CHECK(dir.path().empty());
        CHECK(!fs::exists(sock));
        CHECK(!fs::exists(created));
        dir.cleanup(); // idempotent
    }

    // 2. Non-sockets, symlinks (even to sockets), subdirs and non-matching names are untouched;
    //    rmdir leaves the non-empty directory intact.
    {
        StartBoundaryDirectory external;
        const fs::path target_file = external.path / "keep.txt";
        {
            std::ofstream out(target_file);
            out << "keep";
            CHECK(out.good());
        }
        const fs::path target_sock = external.path / "keep.sock";
        bind_unix_socket(target_sock);

        RunSocketDir dir;
        dir.ensure_created();
        const fs::path created = dir.path();
        ScopedPathCleanup guard{created};

        const fs::path matching_sock = created / "vsil-0123456789abcdef.silkit";
        const fs::path regular_file = created / "vsil-1111111111111111.silkit";
        const fs::path symlink_to_file = created / "vsil-2222222222222222.silkit";
        const fs::path symlink_to_sock = created / "vsil-3333333333333333.silkit";
        const fs::path subdir = created / "vsil-4444444444444444.silkit";
        const fs::path other_sock = created / "other.silkit";
        const fs::path upper_sock = created / "vsil-0123456789ABCDEF.silkit";

        bind_unix_socket(matching_sock);
        {
            std::ofstream out(regular_file);
            out << "not-a-socket";
            CHECK(out.good());
        }
        fs::create_symlink(target_file, symlink_to_file);
        fs::create_symlink(target_sock, symlink_to_sock);
        fs::create_directory(subdir);
        bind_unix_socket(other_sock);
        bind_unix_socket(upper_sock);

        dir.cleanup();
        CHECK(dir.path().empty());
        CHECK(!fs::exists(matching_sock));
        CHECK(fs::exists(created));
        CHECK(fs::is_regular_file(regular_file));
        CHECK(fs::is_symlink(symlink_to_file));
        CHECK(fs::is_symlink(symlink_to_sock));
        CHECK(fs::exists(target_file));
        CHECK(fs::exists(target_sock));
        CHECK(fs::is_directory(subdir));
        CHECK(fs::exists(other_sock));
        CHECK(fs::exists(upper_sock));
    }

    // 3. Directory swapped for another directory (different inode) -> untouched.
    {
        RunSocketDir dir;
        dir.ensure_created();
        const fs::path created = dir.path();
        ScopedPathCleanup guard{created};

        // Create decoy while `created` still exists so it gets a distinct inode.
        const fs::path decoy = created.parent_path() / (created.filename().string() + "-decoy");
        ScopedPathCleanup decoy_guard{decoy};
        fs::create_directory(decoy);
        const fs::path decoy_sock = decoy / "vsil-aaaaaaaaaaaaaaaa.silkit";
        bind_unix_socket(decoy_sock);

        CHECK(::rmdir(created.c_str()) == 0);
        fs::rename(decoy, created);
        decoy_guard.path.clear();

        dir.cleanup();
        CHECK(fs::exists(created / "vsil-aaaaaaaaaaaaaaaa.silkit"));
    }

    // 4. Directory swapped for a symlink -> untouched.
    {
        StartBoundaryDirectory target;
        const fs::path target_sock = target.path / "vsil-bbbbbbbbbbbbbbbb.silkit";
        bind_unix_socket(target_sock);

        RunSocketDir dir;
        dir.ensure_created();
        const fs::path created = dir.path();
        ScopedPathCleanup guard{created};

        CHECK(::rmdir(created.c_str()) == 0);
        fs::create_symlink(target.path, created);

        dir.cleanup();
        CHECK(fs::is_symlink(created));
        CHECK(fs::exists(target_sock));
    }

    // 5. Long TMPDIR falls back to /tmp so "<dir>/vsil-<16hex>.silkit" fits sun_path.
    {
        StartBoundaryDirectory root;
        fs::path long_tmp = root.path;
        constexpr std::size_t kMaxSunPath = sizeof(sockaddr_un::sun_path) - 1;
        while (long_tmp.native().size() + 1 + 11 + 1 + 28 <= kMaxSunPath)
        {
            long_tmp /= "abcdefghij";
        }
        fs::create_directories(long_tmp);

        const RunnerEnvGuard tmpdir("TMPDIR");
        CHECK(::setenv("TMPDIR", long_tmp.c_str(), 1) == 0);

        RunSocketDir dir;
        dir.ensure_created();
        CHECK(dir.path().rfind("/tmp/vsil-", 0) == 0);
        const fs::path created = dir.path();
        dir.cleanup();
        CHECK(!fs::exists(created));
    }
#endif
}

TEST_CASE(note_time_emits_time_and_state_from_one_gate_snapshot)
{
    using vehicle_sil::runner::detail::attach_controller;
    using vehicle_sil::runner::detail::RunContext;
    using vehicle_sil::runner::detail::RunNotes;
    using vehicle_sil::runner::detail::RunOutput;
    using vehicle_sil::runner::detail::RunParticipants;
    using vehicle_sil::runner::detail::TimeNote;

    struct RecordingEvents : RunEvents
    {
        std::mutex mutex;
        std::vector<std::string> recorded;
        std::function<void(const TimeEvent&)> on_time;
        void record(std::string entry)
        {
            const std::lock_guard lock(mutex);
            recorded.push_back(std::move(entry));
        }
        void clear()
        {
            const std::lock_guard lock(mutex);
            recorded.clear();
        }
        std::vector<std::string> snapshot()
        {
            const std::lock_guard lock(mutex);
            return recorded;
        }
        void registry(const std::string&) override {}
        void control_server(const std::string&, bool, bool) override {}
        void participant(const std::string&, ParticipantState) override {}
        void participant_not_started(const std::string&, const std::string&) override {}
        void participant_failed(const std::string&, std::string_view, const std::string&) override {}
        void participant_started(const std::string&, long, const std::vector<std::string>&, const std::string&) override {}
        void participant_ended(const std::string&, const ExitStatus&) override {}
        void participant_manual(const std::string&, const std::vector<std::string>&, const std::string&,
                                const std::string&, const std::vector<std::pair<std::string, std::string>>&) override
        {
        }
        void control_link(const std::string&, std::string_view, std::optional<std::size_t>, std::optional<int>) override {}
        void scenario_step(const ScenarioStepRun&) override {}
        void log(const std::string&, const std::string&) override {}
        void outside(const std::string&, std::uint64_t, std::uint64_t) override {}
        void finding(const std::string&, const Finding&) override {}
        void time(const TimeEvent& e) override
        {
            record("time:" + std::to_string(e.sim_ms));
            if (on_time)
            {
                on_time(e);
            }
        }
        std::size_t waiting_events() const override { return 0; }
    };

    RunPlan plan;
    plan.check.file = "sim.yaml";
    plan.check.config.emplace();
    RunObservers observers;
    StopRequests stops;
    RecordingEvents events;
    RunControls::Observers ctrl_obs;
    ctrl_obs.on_command = [&](const Command& command, std::string_view) {
        events.record("command:" + command.name.value_or(""));
    };
    ctrl_obs.on_state = [&](const RunState& state) {
        events.record(std::string("state:") + (state.paused ? "paused:" : "going:") +
                      (state.paused_at_ms ? std::to_string(*state.paused_at_ms) : "null"));
    };
    RunControls controls(stops, /*start_paused=*/true, std::move(ctrl_obs), /*scenario=*/false);
    controls.set_time_lock({});
    RunContext ctx{plan.config(), plan.check, plan, observers, &events, nullptr, controls, controls.time()};
    RunOutcome outcome = planned_outcome(plan);
    RunOutput output(ctx);
    RunParticipants participants(ctx, output, outcome, plan.base);
    RunNotes notes(ctx, participants);

    vehicle_sil::unit::InProcessRegistry registry;
    SimulationControllerOptions options;
    options.registry_uri = registry.uri;
    options.required_participants = {"Member"};
    options.time_control = TimeControlOptions{.step = 10ms, .start_paused = true};
    SimulationController controller(options);
    auto peer = std::make_unique<ParticipantRuntime>(
        vehicle_sil::unit::coordinated_options("Member", registry.uri, VirtualTimeOptions{10ms, std::nullopt}));
    peer->set_step_handler([](SimTime, SimDuration) {});
    attach_controller(controls.time(), controller, 10, std::nullopt);
    vehicle_sil::unit::start_together(controller, {peer.get()}, 30s);
    CHECK(controller.wait_until_paused(vehicle_sil::test::scaled_timeout(10s)) == std::optional<SimTime>(SimTime{}));

    controls.set_phase(Phase::Running);
    events.clear();

    notes.note_time(controller, TimeNote::Start);
    CHECK((events.snapshot() == std::vector<std::string>{"time:0"}));

    // 1. Single TimeState snapshot: advance the controller directly to 30 ms without updating
    // RunControls::told_ (still paused:0), then mutate the gate inside events.time(). Both time
    // and state must still reflect the single snapshot taken at the start of note_time().
    controller.run_until(SimTime{} + 30ms);
    CHECK(controller.wait_until_paused(vehicle_sil::test::scaled_timeout(10s)) == std::optional<SimTime>(SimTime{} + 30ms));

    events.clear();
    events.on_time = [&](const TimeEvent&) { controller.resume(); };
    notes.note_time(controller, TimeNote::Poll);
    events.on_time = nullptr;
    CHECK((events.snapshot() == std::vector<std::string>{"time:30", "state:paused:30"}));

    // 2. Mutex atomicity across time + state: a concurrent command applied while events.time() runs
    // must wait for TimeCommands' mutex until the look's state has been published.
    controller.pause();
    const auto settled = controller.wait_until_paused(vehicle_sil::test::scaled_timeout(10s));
    CHECK(settled.has_value());
    const SimTime next_target = *settled + 30ms;
    const std::int64_t next_ms = std::chrono::duration_cast<std::chrono::milliseconds>(next_target.time_since_epoch()).count();
    controller.run_until(next_target);
    CHECK(controller.wait_until_paused(vehicle_sil::test::scaled_timeout(10s)) == std::optional<SimTime>(next_target));

    events.clear();
    std::promise<void> started;
    std::future<void> ready = started.get_future();
    bool racer_accepted = false;
    std::thread racer;
    events.on_time = [&](const TimeEvent&) {
        racer = std::thread([&] {
            started.set_value();
            Command resume;
            resume.name = "resume";
            racer_accepted = controls.time().apply(resume, "stdin").accepted;
        });
        ready.wait();
        std::this_thread::sleep_for(20ms); // time enough for racer to enter apply() if unlocked
    };
    notes.note_time(controller, TimeNote::Poll);
    events.on_time = nullptr;
    racer.join();
    CHECK(racer_accepted);
    const std::string next_text = std::to_string(next_ms);
    CHECK((events.snapshot() == std::vector<std::string>{"time:" + next_text, "state:paused:" + next_text,
                                                         "command:resume", "state:going:null"}));

    controls.time().detach();
    controller.stop("test done");
    peer->stop("test done");
}
