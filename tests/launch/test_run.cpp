// SPDX-License-Identifier: MIT
//
// Judging a run of vehicle_sil_launch (assess_run(): what the run saw, made up here, gives which
// errors, warnings and notes), the result and the exit code, and the output for people and as
// JSON. No process is started.
#include "launch_test_util.hpp"

#include "vehicle_sil/launch/error_result.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/launch/run_detail.hpp"

#include "check_words.hpp"
#include "run_close.hpp"
#include "run_connection.hpp"
#include "run_notes.hpp"
#include "run_samples.hpp"
#include "run_start_limit.hpp"
#include "silkit_test_util.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <initializer_list>
#include <vector>

#ifndef _WIN32
#include <csignal>
#endif

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using vehicle_sil::launch::test::check_text;
using vehicle_sil::launch::test::lines;
using vehicle_sil::process::ExitStatus;

namespace {

const std::string kStop(stop_request_name());

ParticipantRun participant(const std::string& name, std::size_t line, bool required = true)
{
    ParticipantRun p;
    p.name = name;
    p.line = line;
    p.required = required;
    return p;
}

ParticipantRun exited(ParticipantRun p, int code)
{
    p.ending = Ending::Exited;
    p.exit = ExitStatus{code, std::nullopt};
    return p;
}

/// A run of two required ECUs that reached stop_at_ms and both exited with 0.
RunOutcome passed_run()
{
    RunOutcome o;
    o.end.simulation = SimulationEnd::ReachedStopAt;
    o.end.stop_at_ms = 1000;
    o.start.timeout_ms = 30000;
    o.end.stop_timeout_ms = 10000;
    o.participants = {exited(participant("Engine", 5), 0), exited(participant("Brake", 7), 0)};
    o.timing.time_step_ms = 10;
    o.timing.late_tolerance_ms = 10;
    o.timing.sim_ms = 1000;
    o.timing.wall_ms = 40;
    return o;
}

/// passed_run() at speed 1 that fell behind: 37 of 100 paced steps late, by 1000 ms in all (1000 ms
/// of simulation in 2000 ms), and paused for a second.
RunOutcome late_run()
{
    RunOutcome o = passed_run();
    o.timing.speed = 1.0;
    o.timing.wall_ms = 3000;
    o.timing.paused_ms = 1000;
    o.timing.paced_steps = 100;
    o.timing.late_steps = 37;
    o.timing.max_late_ms = 412;
    o.timing.slipped_ms = 1000;
    o.timing.ecu_step_ns = 10'000'000;
    o.timing.step_delay_ns = 0;
    o.timing.fine_step_ms = 10;
    return o;
}

/// A report of `outcome` after a check of `config_text` that passed.
RunReport report_of(const RunOutcome& outcome, bool strict = false)
{
    RunReport r;
    r.check.file = "sim.yaml";
    r.check.config = read_config("vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n  - name: Engine\n    command: e\n").config;
    r.check.strict = strict;
    r.strict = strict;
    r.run = outcome;
    r.findings = assess_run(outcome);
    return r;
}

} // namespace

TEST_CASE(a_run_that_ended_normally_has_no_finding_and_passes)
{
    const RunReport r = report_of(passed_run());
    check_text(lines(r.findings), "");
    CHECK(r.result() == RunResult::Passed);
    CHECK_EQ(r.exit_code(), 0);
    // A participant that stopped the simulation: it ended, also fine.
    RunOutcome ended = passed_run();
    ended.end.simulation = SimulationEnd::Ended;
    CHECK(report_of(ended).result() == RunResult::Passed);
}

TEST_CASE(the_participant_that_failed_the_run_is_named_with_how_its_process_ended)
{
    RunOutcome o = passed_run();
    o.end.simulation = SimulationEnd::Aborted;
    o.end.failed_participant = "Engine";
    o.participants[0] = exited(participant("Engine", 5), 7);
    const RunReport r = report_of(o);
    check_text(lines(r.findings),
               "E 5 participant-failed: participant 'Engine': it failed while the simulation was running, and the "
               "launcher aborted the simulation: its process exited with code 7; the lines [Engine] above show what it "
               "wrote\n");
    CHECK(r.result() == RunResult::Failed);
    CHECK_EQ(r.exit_code(), 1);
    // The controller saw it leave, and its process did not end by itself: the controller's reason.
    o.participants[0] = participant("Engine", 5);
    o.participants[0].ending = Ending::Killed;
    o.end.failure_reason = "left the simulation (state Error): it failed or lost its connection; see its log for the cause";
    check_text(lines(assess_run(o)),
               "E 5 participant-failed: participant 'Engine': it failed while the simulation was running, and the "
               "launcher aborted the simulation: it left the simulation (state Error): it failed or lost its connection; "
               "see its log for the cause; the lines [Engine] above show what it wrote\n");
#ifndef _WIN32
    o.participants[0] = participant("Engine", 5);
    o.participants[0].ending = Ending::Exited;
    o.participants[0].exit = ExitStatus{128 + SIGSEGV, SIGSEGV};
    check_text(lines(assess_run(o)),
               "E 5 participant-failed: participant 'Engine': it failed while the simulation was running, and the "
               "launcher aborted the simulation: its process was ended by signal SIGSEGV; the lines [Engine] above show "
               "what it wrote\n");
#endif
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
        const std::vector<Finding> settled = launch::detail::settled_findings(middle);
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
    check_text(lines(launch::detail::settled_findings(middles[1])), "");
    CHECK(lines(assess_run(middles[1])).find("participant-failed") != std::string::npos);
    RunOutcome ended = middles[1];
    ended.participants[0] = exited(participant("Engine", 5), 7);
    check_text(lines(launch::detail::settled_findings(ended)), lines(assess_run(ended)));
    CHECK(lines(launch::detail::settled_findings(ended)).find("its process exited with code 7") != std::string::npos);
    // Everything else is written at once.
    check_text(lines(launch::detail::settled_findings(middles[3])), lines(assess_run(middles[3])));
    check_text(lines(launch::detail::settled_findings(middles[4])), lines(assess_run(middles[4])));
    check_text(lines(launch::detail::settled_findings(middles[6])), lines(assess_run(middles[6])));
}

TEST_CASE(a_participant_that_exits_with_another_code_is_an_error_or_for_an_optional_one_a_warning)
{
    RunOutcome o = passed_run();
    o.participants[1] = exited(participant("Brake", 7), 3);
    o.participants.push_back(exited(participant("Logger", 9, false), 5));
    o.participants.push_back(exited(participant("Quiet", 11, false), 0));
    const RunReport r = report_of(o);
    check_text(lines(r.findings),
               "E 7 exit-code: participant 'Brake': its process exited with code 3; the lines [Brake] above show what it "
               "wrote\n"
               "W 9 exit-code: participant 'Logger': its process exited with code 5; the lines [Logger] above show what it "
               "wrote\n");
    CHECK(r.result() == RunResult::Failed);
    // Only the warning: passes, but not with --strict.
    o.participants[1] = exited(participant("Brake", 7), 0);
    CHECK(report_of(o).result() == RunResult::Passed);
    CHECK(report_of(o, true).result() == RunResult::Failed);
    // An optional participant that failed before the start: its code only (it did not hold the
    // start up).
    o.participants[2].ending = Ending::ExitedBeforeStart;
    check_text(lines(assess_run(o)), "W 9 exit-code: participant 'Logger': its process exited with code 5; the lines "
                                     "[Logger] above show what it wrote\n");
#ifndef _WIN32
    // A signal is a failure too.
    o.participants[1] = participant("Brake", 7);
    o.participants[1].ending = Ending::Exited;
    o.participants[1].exit = ExitStatus{128 + SIGSEGV, SIGSEGV};
    check_text(lines(assess_run(o)),
               "E 7 exit-code: participant 'Brake': its process was ended by signal SIGSEGV; the lines [Brake] above show "
               "what it wrote\n"
               "W 9 exit-code: participant 'Logger': its process exited with code 5; the lines [Logger] above show what it "
               "wrote\n");
    o.participants[1] = exited(participant("Brake", 7), 0);
#endif
    // Windows: an NTSTATUS in hex.
    o.participants[2] = exited(participant("Logger", 9, false), static_cast<int>(0xC0000005u));
    check_text(lines(assess_run(o)), "W 9 exit-code: participant 'Logger': its process exited with code 0xC0000005; the "
                                     "lines [Logger] above show what it wrote\n");
}

TEST_CASE(a_required_participant_that_ends_before_the_start_or_does_not_join_is_an_error)
{
    RunOutcome o = passed_run();
    o.end.simulation = SimulationEnd::NotStarted;
    o.start.timeout_ms = 2000;
    o.end.failed_participant = "Quitter";
    ParticipantRun quitter = exited(participant("Quitter", 9), 0);
    quitter.ending = Ending::ExitedBeforeStart;
    ParticipantRun ghost = participant("Ghost", 11);
    ghost.describe = false;
    ghost.missing_at_start = true;
    ghost.ending = Ending::Stopped;
    ghost.exit = ExitStatus{9, std::nullopt};
    ParticipantRun slow = participant("Slow", 13);
    slow.missing_at_start = true;
    slow.ending = Ending::Killed;
    // An optional participant that ended before the start: only its code counts (0: nothing).
    ParticipantRun optional = exited(participant("Logger", 15, false), 0);
    optional.ending = Ending::ExitedBeforeStart;
    o.participants = {participant("Engine", 5), quitter, ghost, slow, optional};
    o.participants[0].ending = Ending::Stopped;
    o.participants[0].exit = ExitStatus{130, std::nullopt};
    const RunReport r = report_of(o);
    check_text(lines(r.findings),
               "E 9 ended-before-start: participant 'Quitter': it ended before the simulation started: its process exited "
               "with code 0, so the simulation could not start; the lines [Quitter] above show what it wrote\n"
               "E 11 not-joined: participant 'Ghost': it did not join the simulation within 2000 ms (start_timeout_ms), "
               "although its process was running: it must connect to the registry given with {registry_uri} as the "
               "participant 'Ghost' with a coordinated lifecycle (or set required: false); if it is only slow, raise "
               "start_timeout_ms\n"
               "E 13 not-joined: participant 'Slow': it did not join the simulation within 2000 ms (start_timeout_ms), "
               "although its process was running: it did not connect to the registry; the lines [Slow] above show what it "
               "wrote; if it is only slow, raise start_timeout_ms\n");
    CHECK(r.result() == RunResult::Failed);
    // Stopped by the launcher before the start (Engine, exit code 130): not its fault.
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
    const auto left = launch::detail::close_within([&ran] { ran = true; }, 10s, 1ms, [] { return false; });
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
    const auto left = launch::detail::close_within(close, 100ms, 5ms, [] { return false; });
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
    const auto left = launch::detail::close_within([opened] { opened.wait(); }, 60s, 2ms, [&asked] { return ++asked > 3; });
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

using NotedConnection = launch::detail::ConnectionOf<NotedHub, NotedRegistry, NotedController, NotedTransport>;

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
        launch::detail::close_connection_within(noted_connection(going, hub), 60s, 1ms, [] { return false; });
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
    std::optional<launch::detail::CloseLeft> left;
    {
        // The run's holds: the hub's and the connection's. Both are given up here, as by a caller
        // that destroys the run although its close was left.
        auto held = std::make_shared<NotedHub>(going);
        hub = held;
        auto connection = noted_connection(going, held);
        watched = connection;
        left = launch::detail::close_connection_within(std::move(connection), 100ms, 5ms, [] { return false; });
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
    launch::detail::ConnectionOf<control::Hub, vehicle_sil::unit::InProcessRegistry, SimulationController, HeldTransport>;

/// What a run has beside its connection, and the connection's parts are given pointers to (as
/// the Runner of run.cpp): what the start window's function names, and the hub's tap.
struct RunParts
{
    explicit RunParts(std::shared_ptr<control::Hub> h) : hub(std::move(h)) {}
    std::shared_ptr<control::Hub> hub;
    launch::detail::RunSamples samples{{}, {}};
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
    std::optional<launch::detail::CloseLeft> left;
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
        left = launch::detail::close_connection_within(std::move(connection), 100ms, 5ms, [] { return false; });
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

TEST_CASE(a_control_agent_that_is_not_connected_is_told_apart_from_none)
{
    // Nothing to connect, connected, and variables that are not offered (the check notes that):
    // nothing for the run to say.
    RunOutcome o = passed_run();
    o.start.control_hello_timeout_ms = 5000;
    check_text(lines(assess_run(o)), "");
    o.participants[0].control = ControlLink::Connected;
    o.participants[0].control_variables = 12;
    o.participants[1].control = ControlLink::NotOffered;
    check_text(lines(assess_run(o)), "");
    std::string json = format_run_json(report_of(o));
    CHECK(json.find("      \"control_link\": {\n        \"state\": \"connected\",\n        \"variables\": 12,\n"
                    "        \"version\": null,\n        \"late_messages\": 0\n      }\n") != std::string::npos);
    CHECK(json.find("        \"state\": \"not_offered\",\n        \"variables\": null,\n") != std::string::npos);

    // Offered, and no hello; a hello of another version than --describe announced.
    o.participants[0].control = ControlLink::NoHello;
    o.participants[1].control = ControlLink::VersionMismatch;
    o.participants[1].control_version = 7;
    const std::string expected =
        "W 5 control-link-no-hello: participant 'Engine': it offers its variables (--describe), but its control agent did "
        "not say hello within 5000 ms before the simulation started, so its variables cannot be read or written in this "
        "run. Either the executable did not make the agent's transport that --describe announces (an application with a "
        "transport of its own gives --no-control), or the launcher did not take its hello (the finding "
        "control-link-dropped says so, if it is there); the lines [Engine] above show what it wrote\n"
        "W 7 control-link-version: participant 'Brake': its control agent speaks version 7 of the control messages and "
        "this launcher version 2, so its variables cannot be read or written in this run: the ECU is of a newer "
        "vehicle-sil; use a vehicle_sil_launch of its vehicle-sil or a newer one\n";
    check_text(lines(assess_run(o)), expected);
    // They are settled when the simulation starts: the participants' end does not change them.
    check_text(lines(launch::detail::settled_findings(o)), expected);
    const RunReport r = report_of(o);
    CHECK(r.result() == RunResult::Passed); // warnings
    CHECK(report_of(o, true).result() == RunResult::Failed); // --strict
    json = format_run_json(r);
    CHECK(json.find("        \"state\": \"no_hello\",\n") != std::string::npos);
    CHECK(json.find("        \"state\": \"version_mismatch\",\n        \"variables\": null,\n        \"version\": 7,\n") !=
          std::string::npos);
    // An older version than the launcher's: the ECU is the one to build again.
    o.participants[0].control = ControlLink::None;
    o.participants[1].control_version = 1;
    check_text(lines(assess_run(o)),
               "W 7 control-link-version: participant 'Brake': its control agent speaks version 1 of the control messages "
               "and this launcher version 2, so its variables cannot be read or written in this run: the ECU is of an older "
               "vehicle-sil; build it again with the vehicle-sil of this vehicle_sil_launch\n");
    // A version that --describe announced is the check's finding: the run does not say it again.
    o.participants[0].control = ControlLink::None;
    o.participants[1].control_version_described = true;
    check_text(lines(assess_run(o)), "");

    RunOutcome late = passed_run();
    late.participants[0].control = ControlLink::Connected;
    late.participants[0].control_late_messages = 3;
    check_text(lines(assess_run(late)),
               "W 5 control-link-late-values: participant 'Engine': 3 messages of its control agent's steps reached the "
               "launcher after the launcher's controller had begun a later step: the values and results that the launcher "
               "had at a pause were not complete. Its agent must publish within its steps (an executable built with "
               "ecu_main() does; a transport of its own must send from control::Agent::set_step_sink(), not from another "
               "thread)\n");
    CHECK(format_run_json(report_of(late)).find("        \"late_messages\": 3\n") != std::string::npos);

    // Mappings that did not fit what the agent announced (an executable that describes one thing
    // and announces another, or one with describe: false): the run is not to be trusted.
    RunOutcome mapped = passed_run();
    mapped.participants[0].control = ControlLink::Connected;
    mapped.participants[0].variable_map_problems = {"engine.speed: the executable has no variable 'rpm'",
                                                    "engine.gear: map names the values of an integer or an enum"};
    check_text(lines(assess_run(mapped)),
               "E 5 variable-map-not-applied: participant 'Engine': variables: 2 of its mappings do not fit the variables "
               "its control agent announced and were not applied, so those variables were seen under the executable's "
               "names and values in this run and writes to the names of these mappings were refused: engine.speed: the "
               "executable has no variable 'rpm'; engine.gear: map names the values of an integer or an enum. Correct variables in the configuration (vehicle_sil_launch check "
               "compares them with what an ecu_main executable describes)\n");
    CHECK(report_of(mapped).result() == RunResult::Failed);
    CHECK(std::string(control_link_name(ControlLink::None)) == "none");
    CHECK(std::string(control_link_name(ControlLink::NotOffered)) == "not_offered");

    // A hello, and no answer to the launcher's probe: no_hello as well, said apart.
    RunOutcome mute = passed_run();
    mute.start.control_hello_timeout_ms = 5000;
    mute.participants[0].control = ControlLink::NoHello;
    mute.participants[0].control_hello_seen = true;
    check_text(lines(assess_run(mute)),
               "W 5 control-link-no-hello: participant 'Engine': it offers its variables (--describe), but its control agent "
               "said hello and then did not answer that it receives the launcher's commands within 5000 ms before the "
               "simulation started, so its variables cannot be read or written in this run. Its transport did not answer "
               "the launcher's probe: look in the ECU's output for an error of its control transport (an application with "
               "a transport of its own answers the probe, or gives --no-control); the lines [Engine] above show what it "
               "wrote\n");

    // What the launcher's end dropped is said once, with the numbers.
    RunOutcome dropped = passed_run();
    dropped.control.refused_hellos = 2;
    dropped.control.unknown = 40;
    const std::string said = lines(assess_run(dropped));
    CHECK(said.rfind("W 0 control-link-dropped: the launcher dropped messages of control agents: 2 hellos it did not take, 0 "
                     "messages it could not read and 40 messages (or samples) that fit no agent it knows. Hellos are not taken "
                     "beyond 256 participants with control agents",
                     0) == 0);
    CHECK(lines(launch::detail::settled_findings(dropped)) == said);
    dropped.control.refused_hellos = 0;
    dropped.control.unknown = 0;
    dropped.control.undecodable = 1;
    CHECK(lines(assess_run(dropped)).find("0 hellos it did not take, 1 messages it could not read and 0 messages") != std::string::npos);

    // What was to be done before the start failed: the simulation had started and was aborted.
    RunOutcome failed = passed_run();
    failed.end.simulation = SimulationEnd::Aborted;
    failed.start.before_start_error = "no answer from Engine";
    check_text(lines(assess_run(failed)),
               "E 0 before-start: what was to be done before the simulation started failed, and the launcher aborted the "
               "simulation at once: no answer from Engine\n");
    CHECK(report_of(failed).result() == RunResult::Failed);
}

TEST_CASE(the_simulation_findings_setup_state_abort_and_interrupt)
{
    RunOutcome o = passed_run();
    o.end.simulation = SimulationEnd::Ended;
    o.end.controller_state = "Aborting";
    check_text(lines(assess_run(o)), "E 0 simulation: the simulation ended in state Aborting instead of shutting down "
                                     "normally: the lines above show what the participants wrote\n");
    CHECK(report_of(o).result() == RunResult::Failed);
    o.end.controller_state.clear();
    o.end.simulation = SimulationEnd::Aborted;
    check_text(lines(assess_run(o)), "E 0 simulation: the simulation was aborted: the lines above show what the "
                                     "participants wrote\n");
    CHECK(report_of(o).result() == RunResult::Failed);
    o.end.simulation = SimulationEnd::Interrupted;
    o.end.interrupted_by = "SIGINT";
    const RunReport interrupted = report_of(o);
    check_text(lines(interrupted.findings),
               "N 0 interrupted: interrupted (SIGINT): the launcher stopped the simulation and the participants\n");
    CHECK(interrupted.result() == RunResult::Interrupted);
    CHECK_EQ(interrupted.exit_code(), kExitInterrupted);
    CHECK_EQ(kExitInterrupted, 130);
    o.end.simulation = SimulationEnd::NotStarted;
    check_text(lines(assess_run(o)),
               "N 0 interrupted: interrupted (SIGINT): the launcher stopped the participants before the simulation "
               "started\n");
    RunOutcome setup;
    setup.setup.error = "the registry cannot listen at silkit://127.0.0.1:8500: in use";
    const RunReport failed = report_of(setup);
    check_text(lines(failed.findings),
               "E 0 setup: cannot set up the simulation: the registry cannot listen at silkit://127.0.0.1:8500: in use\n");
    CHECK(failed.result() == RunResult::SetupFailed);
    CHECK_EQ(failed.exit_code(), 3);
}

TEST_CASE(the_result_when_nothing_was_started)
{
    RunReport r;
    r.check.file = "sim.yaml";
    CHECK(r.result() == RunResult::Unreadable);
    CHECK_EQ(r.exit_code(), 3);
    r.check.config = SimulationConfig{};
    r.check.findings.push_back(Finding{Severity::Error, "unknown-key", "x", 3, "", ""});
    CHECK(r.result() == RunResult::CheckFailed);
    CHECK_EQ(r.exit_code(), 1);
    r.usage = true;
    CHECK(r.result() == RunResult::Usage);
    CHECK_EQ(r.exit_code(), 4);
}

TEST_CASE(the_text_for_people)
{
    RunOutcome o = passed_run();
    o.end.simulation = SimulationEnd::Aborted;
    o.end.failed_participant = "Engine";
    o.participants[0] = exited(participant("Engine", 5), 7);
    ParticipantRun logger = participant("Logger", 9, false);
    logger.ending = Ending::Stopped;
    logger.exit = ExitStatus{0, std::nullopt};
    ParticipantRun deaf = participant("Deaf", 11, false);
    deaf.ending = Ending::Killed;
    ParticipantRun tool = participant("Tool", 13, false);
    tool.start_error = "cannot start t";
    o.participants.push_back(logger);
    o.participants.push_back(deaf);
    o.participants.push_back(tool);
    o.traffic = RunTraffic{
        1000,
        {TrafficNetwork{"J1939_CAN",
                        "can",
                        {TrafficParticipant{"Engine", TrafficNotCounted::None, true, SimTime{} + std::chrono::milliseconds(990), 10, 1},
                         TrafficParticipant{"Logger", TrafficNotCounted::RealTime, false}}}}};
    const std::string text = format_run_text(report_of(o));
    check_text(text,
               "sim.yaml: 1 participant, 0 networks\n"
               "\n"
               "participant  step  networks\n"
               "Engine       -     not read (see the errors)\n"
               "\n"
               "run: the simulation was aborted: participant 'Engine' failed\n"
               "\n"
               "participant  required  ended\n"
               "Engine       yes       exit code 7\n"
               "Brake        yes       exit code 0\n"
               "Logger       no        stopped (" + kStop + "), exit code 0\n"
               "Deaf         no        killed\n"
               "Tool         no        not started\n"
               "\n"
               "traffic: J1939_CAN: 1 participant sent, 1 received (1 counted, 1 not counted) (steps before 1000 ms)\n"
               "\n"
               "network    participant  sent  received\n"
               "J1939_CAN  Engine         10         1\n"
               "J1939_CAN  Logger          -         -  not counted: real time\n"
               "\n"
               "error: sim.yaml:5: participant 'Engine': it failed while the simulation was running, and the launcher "
               "aborted the simulation: its process exited with code 7; the lines [Engine] above show what it wrote [participant-failed]\n"
               "warning: sim.yaml:13: participant 'Tool': cannot start t: check the command in the configuration file [start]\n"
               "\n"
               "failed: 1 error, 1 warning, 0 notes\n");
    // The line of the simulation for each end.
    const auto state_line = [](const RunOutcome& outcome) {
        const std::string t = format_run_text(report_of(outcome));
        const auto at = t.find("run: ");
        return t.substr(at, t.find('\n', at) - at);
    };
    // A run that started and has nobody on a network says so; one that did not start says nothing of it.
    RunOutcome quiet = passed_run();
    CHECK(format_run_text(report_of(quiet)).find("traffic:") == std::string::npos);
    quiet.traffic = RunTraffic{1000, {}};
    CHECK(format_run_text(report_of(quiet)).find("Brake        yes       exit code 0\n\ntraffic: no network in use\n\npassed: ") !=
          std::string::npos);
    RunOutcome s = passed_run();
    check_text(state_line(s), "run: the simulation ran to stop_at_ms (1000 ms) and ended");
    s.end.simulation = SimulationEnd::Ended;
    check_text(state_line(s), "run: the simulation ran until a participant ended it");
    s.end.controller_state = "Aborting";
    check_text(state_line(s), "run: the simulation ended in state Aborting");
    s.end.controller_state.clear();
    s.end.simulation = SimulationEnd::Interrupted;
    s.end.interrupted_by = "Ctrl+C";
    check_text(state_line(s), "run: interrupted (Ctrl+C): the simulation was stopped");
    s.end.simulation = SimulationEnd::NotStarted;
    check_text(state_line(s), "run: interrupted (Ctrl+C) before the simulation started");
    s.end.interrupted_by.clear();
    check_text(state_line(s), "run: the simulation did not start");
    s.setup.error = "x";
    check_text(state_line(s), "run: the simulation could not be set up");
    // The last line.
    const std::string interrupted = format_run_text(report_of([] {
        RunOutcome i = passed_run();
        i.end.simulation = SimulationEnd::Interrupted;
        i.end.interrupted_by = "SIGINT";
        return i;
    }()));
    CHECK(interrupted.ends_with("\ninterrupted: 0 errors, 0 warnings, 1 note\n"));
    RunOutcome warned = passed_run();
    warned.participants[0].left_processes = true;
    CHECK(format_run_text(report_of(warned, true)).ends_with("\nfailed: 0 errors, 1 warning, 0 notes (--strict)\n"));
    CHECK(format_run_text(report_of(warned)).ends_with("\npassed: 0 errors, 1 warning, 0 notes\n"));
}

TEST_CASE(the_text_when_the_check_found_errors)
{
    RunReport r;
    r.check.file = "sim.yaml";
    r.check.config = SimulationConfig{};
    r.check.findings.push_back(Finding{Severity::Error, "unknown-key", "unknown key 'x'", 3, "", ""});
    check_text(format_run_text(r), "sim.yaml: 0 participants, 0 networks\n"
                                   "\n"
                                   "error: sim.yaml:3: unknown key 'x' [unknown-key]\n"
                                   "\n"
                                   "nothing was started: the check found errors (vehicle_sil_launch check sim.yaml shows "
                                   "the same)\n"
                                   "\n"
                                   "failed: 1 error, 0 warnings, 0 notes\n");
    r.check.findings[0].severity = Severity::Warning;
    r.check.strict = true;
    r.strict = true;
    check_text(format_run_text(r), "sim.yaml: 0 participants, 0 networks\n"
                                   "\n"
                                   "warning: sim.yaml:3: unknown key 'x' [unknown-key]\n"
                                   "\n"
                                   "nothing was started: the check found warnings, and --strict fails on them "
                                   "(vehicle_sil_launch check sim.yaml shows the same)\n"
                                   "\n"
                                   "failed: 0 errors, 1 warning, 0 notes\n");
    // A file that cannot be read: the check's text as it is.
    RunReport unreadable;
    unreadable.check.file = "sim.yaml";
    unreadable.check.findings.push_back(Finding{Severity::Error, "file", "the file does not exist: check the path", 0, "", ""});
    check_text(format_run_text(unreadable), format_text(unreadable.check));
}

TEST_CASE(the_json_report)
{
    RunOutcome o = passed_run();
    o.end.simulation = SimulationEnd::Interrupted;
    o.end.interrupted_by = "SIGINT";
    o.setup.registry_uri = "silkit://127.0.0.1:41234";
    ParticipantRun killed = participant("Brake", 7);
    killed.ending = Ending::Killed;
    killed.left_processes = true;
    o.participants[1] = killed;
    o.timing.sim_ms = 1000;
    o.traffic = RunTraffic{
        1000,
        {TrafficNetwork{"J1939_CAN",
                        "can",
                        {TrafficParticipant{"Engine", TrafficNotCounted::None, true, SimTime{} + std::chrono::microseconds(992'500), 10, 0},
                         TrafficParticipant{"Brake", TrafficNotCounted::NoControlLink}}}}};
    const std::string json = format_run_json(report_of(o));
    const std::string v(version());
    check_text(json.substr(0, json.find("  \"check\": {")),
               "{\n"
               "  \"vehicle_sil_run\": 2,\n"
               "  \"vehicle_sil\": \"" + v + "\",\n"
               "  \"file\": \"sim.yaml\",\n"
               "  \"result\": \"interrupted\",\n"
               "  \"end\": \"interrupted\",\n"
               "  \"ok\": false,\n"
               "  \"message\": \"The run was interrupted by a signal\",\n"
               "  \"strict\": false,\n"
               "  \"errors\": 0,\n"
               "  \"warnings\": 1,\n"
               "  \"notes\": 1,\n"
               "  \"simulation\": \"interrupted\",\n"
               "  \"interrupted_by\": \"SIGINT\",\n"
               "  \"registry_uri\": \"silkit://127.0.0.1:41234\",\n"
               "  \"timing\": {\n"
               "    \"speed\": \"max\",\n"
               "    \"time_step_ms\": 10,\n"
               "    \"ecu_step_ms\": null,\n"
               "    \"step_delay_ms\": null,\n"
               "    \"late_tolerance_ms\": 10,\n"
               "    \"sim_ms\": 1000,\n"
               "    \"wall_ms\": 40,\n"
               "    \"actual_speed\": 25,\n"
               "    \"paced_steps\": 0,\n"
               "    \"late_steps\": 0,\n"
               "    \"max_late_ms\": 0,\n"
               "    \"slipped_ms\": 0,\n"
               "    \"late_ms\": 0,\n"
               "    \"paused_ms\": 0\n"
               "  },\n"
               "  \"participants\": [\n"
               "    {\n"
               "      \"name\": \"Engine\",\n"
               "      \"required\": true,\n"
               "      \"enabled\": true,\n      \"manual\": false,\n      \"for_network\": null,\n"
               "      \"ended\": \"exited\",\n"
               "      \"exit_code\": 0,\n"
               "      \"signal\": null,\n"
               "      \"left_processes\": false,\n"
               "      \"log\": null,\n"
               "      \"control_link\": {\n"
               "        \"state\": \"none\",\n"
               "        \"variables\": null,\n"
               "        \"version\": null,\n"
               "        \"late_messages\": 0\n"
               "      }\n"
               "    },\n"
               "    {\n"
               "      \"name\": \"Brake\",\n"
               "      \"required\": true,\n"
               "      \"enabled\": true,\n      \"manual\": false,\n      \"for_network\": null,\n"
               "      \"ended\": \"killed\",\n"
               "      \"exit_code\": null,\n"
               "      \"signal\": null,\n"
               "      \"left_processes\": true,\n"
               "      \"log\": null,\n"
               "      \"control_link\": {\n"
               "        \"state\": \"none\",\n"
               "        \"variables\": null,\n"
               "        \"version\": null,\n"
               "        \"late_messages\": 0\n"
               "      }\n"
               "    }\n"
               "  ],\n"
               "  \"outside\": [],\n"
               "  \"traffic\": {\n"
               "    \"before_ms\": 1000,\n"
               "    \"unplaced\": [],\n"
               "    \"networks\": [\n"
               "      {\n"
               "        \"name\": \"J1939_CAN\",\n"
               "        \"kind\": \"can\",\n"
               "        \"counted\": 1,\n"
               "        \"senders\": 1,\n"
               "        \"receivers\": 0,\n"
               "        \"delivered\": null,\n"
               "        \"unplaced\": [],\n"
               "        \"participants\": [\n"
               "          {\n"
               "            \"name\": \"Engine\",\n"
               "            \"counted\": true,\n"
               "            \"not_counted\": null,\n"
               "            \"virtual_time\": true,\n"
               "            \"at_ms\": 992.5,\n"
               "            \"frames_sent\": 10,\n"
               "            \"frames_received\": 0\n"
               "          },\n"
               "          {\n"
               "            \"name\": \"Brake\",\n"
               "            \"counted\": false,\n"
               "            \"not_counted\": \"no_control_link\",\n"
               "            \"virtual_time\": null,\n"
               "            \"at_ms\": null,\n"
               "            \"frames_sent\": null,\n"
               "            \"frames_received\": null\n"
               "          }\n"
               "        ]\n"
               "      }\n"
               "    ]\n"
               "  },\n"
               "  \"findings\": [\n"
               "    {\n"
               "      \"severity\": \"warning\",\n"
               "      \"code\": \"left-processes\",\n"
               "      \"file\": \"sim.yaml\",\n"
               "      \"line\": 7,\n"
               "      \"participant\": \"Brake\",\n"
               "      \"network\": null,\n"
               "      \"message\": \"participant 'Brake': processes it started were still running after it had ended: "
               "the launcher ended them; end them in the participant before it exits\",\n"
               "      \"detail\": null\n"
               "    },\n"
               "    {\n"
               "      \"severity\": \"note\",\n"
               "      \"code\": \"interrupted\",\n"
               "      \"file\": \"sim.yaml\",\n"
               "      \"line\": null,\n"
               "      \"participant\": null,\n"
               "      \"network\": null,\n"
               "      \"message\": \"interrupted (SIGINT): the launcher stopped the simulation and the participants\",\n"
               "      \"detail\": null\n"
               "    }\n"
               "  ],\n");
    // The check's JSON inside, indented one level, and the end.
    CHECK(json.find("  \"check\": {\n    \"vehicle_sil_check\": 1,\n") != std::string::npos);
    CHECK(json.ends_with("  }\n}\n"));
#ifndef _WIN32
    o.participants[1] = participant("Brake", 7);
    o.participants[1].ending = Ending::Exited;
    o.participants[1].exit = ExitStatus{128 + SIGSEGV, SIGSEGV};
    const std::string signalled = format_run_json(report_of(o));
    CHECK(signalled.find("      \"exit_code\": null,\n      \"signal\": \"SIGSEGV\",\n") != std::string::npos);
#endif
    // Nothing was started: no simulation, no participants.
    RunReport none;
    none.check.file = "sim.yaml";
    none.check.config = SimulationConfig{};
    none.check.findings.push_back(Finding{Severity::Error, "unknown-key", "x", 3, "", ""});
    const std::string empty = format_run_json(none);
    CHECK(empty.find("  \"result\": \"check_failed\",\n") != std::string::npos);
    CHECK(empty.find("  \"simulation\": null,\n  \"interrupted_by\": null,\n  \"registry_uri\": null,\n  \"timing\": null,\n  \"participants\": [],\n"
                     "  \"outside\": [],\n  \"traffic\": null,\n  \"findings\": [],\n") != std::string::npos);
    // A run that did not get to its simulation, and one with nobody on a network; every name of "not_counted".
    CHECK(format_run_json(report_of(passed_run())).find("  ],\n  \"outside\": [],\n  \"traffic\": null,\n  \"findings\": [") != std::string::npos);
    RunOutcome quiet = passed_run();
    quiet.traffic = RunTraffic{990, {}};
    CHECK(format_run_json(report_of(quiet)).find("  \"traffic\": {\n    \"before_ms\": 990,\n    \"unplaced\": [],\n"
                                                "    \"networks\": []\n  },\n") != std::string::npos);
    check_text(std::string(traffic_not_counted_name(TrafficNotCounted::None)) + "," +
                   traffic_not_counted_name(TrafficNotCounted::NoControlLink) + "," +
                   traffic_not_counted_name(TrafficNotCounted::Restarted) + "," +
                   traffic_not_counted_name(TrafficNotCounted::RealTime) + "," +
                   traffic_not_counted_name(TrafficNotCounted::NoCounters) + "," +
                   traffic_not_counted_name(TrafficNotCounted::Mapped) + "," +
                   traffic_not_counted_name(TrafficNotCounted::NoSample),
               ",no_control_link,restarted,real_time,no_counters,mapped,no_sample");
    // What a machine reads of a network: true, false, and "cannot be said".
    RunOutcome heard = passed_run();
    const auto counted = [](const char* name, std::int64_t sent, std::int64_t received) {
        return TrafficParticipant{name, TrafficNotCounted::None, true, SimTime{} + std::chrono::milliseconds(990), sent, received};
    };
    heard.traffic = RunTraffic{1000,
                               {TrafficNetwork{"A", "can", {counted("Engine", 5, 0), counted("Brake", 0, 5)}},
                                TrafficNetwork{"B", "can", {counted("Engine", 5, 0)}}, TrafficNetwork{"C", "lin", {}}}};
    const std::string networks = format_run_json(report_of(heard));
    CHECK(networks.find("        \"counted\": 2,\n        \"senders\": 1,\n        \"receivers\": 1,\n        \"delivered\": true,\n") !=
          std::string::npos);
    CHECK(networks.find("        \"counted\": 1,\n        \"senders\": 1,\n        \"receivers\": 0,\n        \"delivered\": false,\n") !=
          std::string::npos);
    CHECK(networks.find("        \"name\": \"C\",\n        \"kind\": \"lin\",\n        \"counted\": 0,\n        \"senders\": 0,\n"
                        "        \"receivers\": 0,\n        \"delivered\": null,\n        \"unplaced\": [],\n        \"participants\": []\n") !=
          std::string::npos);
    // The participants of the run that the launcher knows on no network, and per network those
    // that may be on it, each with the reason.
    const TrafficUnplaced bridge{"Bridge", TrafficUnplacedReason::NoControlLink};
    const TrafficUnplaced tool{"Tool", TrafficUnplacedReason::NoCounters};
    heard.traffic->unplaced = {bridge, tool};
    heard.traffic->per_network.at(2).unplaced = {bridge, tool, {"Gateway", TrafficUnplacedReason::NoCounters}};
    const std::string unplaced = format_run_json(report_of(heard));
    const auto entry = [](const std::string& indent, const std::string& name, const std::string& reason) {
        return indent + "{\n" + indent + "  \"participant\": \"" + name + "\",\n" + indent + "  \"reason\": \"" + reason +
               "\"\n" + indent + "}";
    };
    CHECK(unplaced.find("    \"before_ms\": 1000,\n    \"unplaced\": [\n" + entry("      ", "Bridge", "no_control_link") + ",\n" +
                        entry("      ", "Tool", "no_counters") + "\n    ],\n    \"networks\": [\n") != std::string::npos);
    CHECK(unplaced.find("        \"delivered\": null,\n        \"unplaced\": [\n" +
                        entry("          ", "Bridge", "no_control_link") + ",\n" + entry("          ", "Tool", "no_counters") +
                        ",\n" + entry("          ", "Gateway", "no_counters") + "\n        ],\n        \"participants\": []\n") !=
          std::string::npos);
    // A speed and how it went.
    const std::string late = format_run_json(report_of(late_run()));
    CHECK(late.find("  \"timing\": {\n    \"speed\": 1,\n    \"time_step_ms\": 10,\n    \"ecu_step_ms\": 10,\n"
                    "    \"step_delay_ms\": 0,\n    \"late_tolerance_ms\": 10,\n"
                    "    \"sim_ms\": 1000,\n    \"wall_ms\": 3000,\n    \"actual_speed\": 0.5,\n    \"paced_steps\": 100,\n"
                    "    \"late_steps\": 37,\n    \"max_late_ms\": 412,\n    \"slipped_ms\": 1000,\n    \"late_ms\": 0,\n"
                    "    \"paused_ms\": 1000\n  },\n") != std::string::npos);
    // ECU steps of half a millisecond under a controller step of 2 ms.
    RunOutcome fine = late_run();
    fine.timing.time_step_ms = 2;
    fine.timing.ecu_step_ns = 500'000;
    fine.timing.step_delay_ns = 1'500'000;
    CHECK(format_run_json(report_of(fine)).find("\"time_step_ms\": 2,\n    \"ecu_step_ms\": 0.5,\n    \"step_delay_ms\": 1.5,\n") !=
          std::string::npos);
}

TEST_CASE(a_run_that_fell_behind_real_time_is_a_warning_that_says_what_to_do)
{
    const RunReport r = report_of(late_run());
    const std::string advice =
        "; a device attached in real time got the ECUs' messages late by up to that. Lower the speed (speed, run --speed), "
        "or find what is slow: an ECU whose step takes long needs a lighter step (a larger step of that ECU does not "
        "help), and many ECUs with small steps cost a round of synchronisation per step (larger ECU steps and a larger "
        "time_step_ms help then). The launcher cannot tell which participant is slow: leave ECUs out one at a time "
        "(--disable <name>) and compare\n";
    check_text(lines(r.findings),
               "W 0 behind-real-time: the simulation fell behind real time: 37 of 100 paced steps of the launcher's "
               "controller (10 ms each) began more than 10 ms (late_tolerance_ms) after the real time they stand for, the "
               "latest by 412 ms. The launcher does not catch up after a late step (the ECUs' periodic messages would leave "
               "in a burst): it goes on at the speed from there, so the simulation came 1000 ms behind in all and ran at 0.5 "
               "times real time (pauses not counted)" + advice);
    // A run that ended in a stall (a timeout, a failed participant, a stop): the step that never
    // began is the late one.
    RunOutcome stalled = late_run();
    stalled.timing.sim_ms = 500;
    stalled.timing.wall_ms = 5000;
    stalled.timing.paused_ms = 0;
    stalled.timing.paced_steps = 51;
    stalled.timing.late_steps = 1;
    stalled.timing.max_late_ms = 4490;
    stalled.timing.slipped_ms = 4490;
    stalled.timing.late_ms = 4490;
    check_text(lines(assess_run(stalled)),
               "W 0 behind-real-time: the simulation fell behind real time: 1 of 51 paced steps of the launcher's "
               "controller (10 ms each) began more than 10 ms (late_tolerance_ms) after the real time they stand for, the "
               "latest by 4490 ms; when the run ended, the next step was 4490 ms overdue (the simulation was not moving). "
               "The launcher does not catch up after a late step (the ECUs' periodic messages would leave in a burst): it "
               "goes on at the speed from there, so the simulation came 4490 ms behind in all and ran at 0.1 times real time "
               "(pauses not counted)" + advice);
    CHECK(r.result() == RunResult::Passed);
    CHECK(report_of(late_run(), true).result() == RunResult::Failed);
    // Settled once the simulation is over: written before the participants are ended.
    CHECK(launch::detail::settled_findings(late_run()) == r.findings);
    // One late step is enough.
    RunOutcome once = late_run();
    once.timing.late_steps = 1;
    CHECK(lines(assess_run(once)).starts_with("W 0 behind-real-time: the simulation fell behind real time: 1 of 100 paced"));
    // Within the tolerance, or nothing paced: no finding.
    RunOutcome onTime = late_run();
    onTime.timing.late_steps = 0;
    onTime.timing.max_late_ms = 9;
    check_text(lines(assess_run(onTime)), "");
    check_text(lines(assess_run(passed_run())), "");
    // How fast it ran in all: without the pauses; unknown if it did not run for a millisecond.
    CHECK(late_run().timing.actual_speed() == 0.5);
    RunTiming t = late_run().timing;
    t.paused_ms = t.wall_ms;
    CHECK(!t.actual_speed());
    t.wall_ms = 3;
    t.paused_ms = 0;
    t.sim_ms = 1;
    CHECK(t.actual_speed() == 0.333);
}

TEST_CASE(the_text_for_people_has_a_line_of_the_time_when_a_speed_was_set)
{
    const auto time_line = [](const RunOutcome& outcome) {
        const std::string t = format_run_text(report_of(outcome));
        const auto at = t.find("\ntime: ");
        return at == std::string::npos ? std::string{} : t.substr(at + 1, t.find('\n', at + 1) - at - 1);
    };
    check_text(time_line(late_run()), "time: speed 1, ran at 0.5 times real time; 37 of 100 paced steps more than 10 ms late "
                                      "(the latest by 412 ms), 1000 ms behind in all; paused for 1000 ms (not counted)");
    RunOutcome onTime = late_run();
    onTime.timing.speed = 2.0;
    onTime.timing.paused_ms = 0;
    onTime.timing.wall_ms = 500;
    onTime.timing.late_steps = 0;
    onTime.timing.max_late_ms = 2;
    onTime.timing.slipped_ms = 0;
    check_text(time_line(onTime), "time: speed 2, ran at 2 times real time; 0 of 100 paced steps more than 10 ms late (the "
                                  "latest by 2 ms), 0 ms behind in all");
    // After the run: the line follows the run's line.
    CHECK(format_run_text(report_of(onTime)).find("and ended\ntime: speed 2,") != std::string::npos);
    // A speed, but nothing paced yet (it ended at once): the line all the same.
    RunOutcome brief = onTime;
    brief.timing.paced_steps = 0;
    brief.timing.max_late_ms = 0;
    CHECK(time_line(brief).starts_with("time: speed 2, ran at 2 times real time; 0 of 0 paced steps"));
    // As fast as possible, nothing paced: no line (the text stays the same from run to run).
    check_text(time_line(passed_run()), "");
    // Paced for a while, as fast as possible at the end: the line.
    RunOutcome changed = late_run();
    changed.timing.speed.reset();
    CHECK(time_line(changed).starts_with("time: speed max, ran at 0.5 times real time; 37 of 100"));
    // The simulation did not start: no line.
    RunOutcome notStarted = late_run();
    notStarted.end.simulation = SimulationEnd::NotStarted;
    check_text(time_line(notStarted), "");
}

namespace {

/// A check of `config` in which each participant described itself with the step of the same place
/// in `steps_us` (microseconds; 0: an ECU in real time, without a step; -1: not described).
launch::CheckReport described(const launch::SimulationConfig& config, const std::vector<std::int64_t>& steps_us)
{
    launch::CheckReport check;
    check.config = config;
    for (std::size_t i = 0; i < config.participants.size(); ++i)
    {
        launch::ParticipantResult r;
        if (!config.participants[i].enabled)
        {
            r.status = launch::ParticipantStatus::Disabled;
        }
        else if (steps_us[i] < 0)
        {
            r.status = launch::ParticipantStatus::NotChecked;
        }
        else
        {
            r.status = launch::ParticipantStatus::Described;
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

launch::SimulationConfig config_of(std::size_t participants, std::optional<std::int64_t> stop_at_ms = std::nullopt,
                                   std::optional<std::int64_t> time_step_ms = std::nullopt)
{
    launch::SimulationConfig config;
    config.stop_at_ms = stop_at_ms;
    config.time_step_ms = time_step_ms;
    for (std::size_t i = 0; i < participants; ++i)
    {
        launch::ParticipantConfig p;
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

Plan plan(const launch::SimulationConfig& config, const std::vector<std::int64_t>& steps_us, std::optional<double> speed)
{
    const RunTiming t = launch::detail::plan_time_step(config, described(config, steps_us), speed);
    CHECK(t.time_step_written == config.time_step_ms.has_value());
    const auto us = [](const std::optional<std::int64_t>& ns) { return ns ? std::optional<std::int64_t>(*ns / 1000) : std::nullopt; };
    return Plan{t.time_step_ms, us(t.ecu_step_ns), us(t.step_delay_ns), t.fine_step_ms};
}

} // namespace

TEST_CASE(with_a_speed_the_controller_steps_as_finely_as_the_ecus_need)
{
    const std::optional<double> max{};
    // As fast as possible: 10 ms (or what of it divides stop_at_ms), whatever the ECUs' steps. How
    // late an ECU step may run under a speed is told all the same: 1 ms steps under 10 ms, 9 ms.
    CHECK(plan(config_of(2), {1000, 10000}, max) == (Plan{10, 1000, 9000, 1}));
    CHECK(plan(config_of(2, 1005), {1000, 10000}, max) == (Plan{5, 1000, 4000, 1}));
    // With a speed: the greatest common divisor of 10 ms and the ECUs' steps, so that every ECU
    // step starts at a controller step.
    CHECK(plan(config_of(2), {1000, 10000}, 1.0) == (Plan{1, 1000, 0, 1}));
    CHECK(plan(config_of(2), {5000, 10000}, 1.0) == (Plan{5, 5000, 0, 5}));
    CHECK(plan(config_of(2), {10000, 20000}, 0.5) == (Plan{10, 10000, 0, 10}));
    // Not the smallest step: 4 and 6 ms meet only every 2 ms.
    CHECK(plan(config_of(2), {4000, 6000}, 1.0) == (Plan{2, 4000, 0, 2}));
    // Larger steps than 10 ms do not make it coarser (pausing and stepping stay as fine).
    CHECK(plan(config_of(2), {20000, 100000}, 1.0) == (Plan{10, 20000, 0, 10}));
    // And it divides stop_at_ms.
    CHECK(plan(config_of(1, 1005), {10000}, 1.0) == (Plan{5, 10000, 0, 5}));
    CHECK(plan(config_of(1, 1001), {5000}, 1.0) == (Plan{1, 5000, 0, 1}));
    // An ECU in real time has no step and does not count; no ECU with a step: 10 ms.
    CHECK(plan(config_of(2), {0, 5000}, 1.0) == (Plan{5, 5000, 0, 5}));
    CHECK(plan(config_of(1), {0}, 1.0) == (Plan{10, std::nullopt, 0, 10}));
    // A participant that does not describe itself (describe: false): 1 ms, to be safe, and what
    // the ECUs' steps are is not known.
    CHECK(plan(config_of(2), {-1, 10000}, 1.0) == (Plan{1, std::nullopt, std::nullopt, 1}));
    CHECK(plan(config_of(2), {-1, 10000}, max) == (Plan{10, std::nullopt, std::nullopt, 1}));
    // A disabled participant does not count, described or not.
    launch::SimulationConfig disabled = config_of(2);
    disabled.participants[0].enabled = false;
    CHECK(plan(disabled, {-1, 10000}, 1.0) == (Plan{10, 10000, 0, 10}));
    // A step that is no whole number of milliseconds: 1 ms, and what is left is told.
    CHECK(plan(config_of(1), {2500}, 1.0) == (Plan{1, 2500, 500, 1}));
    // time_step_ms as written stays, with a speed too: 10 ms over 1 ms steps runs them up to 9 ms
    // late, 10 ms over 15 ms steps up to 5 ms (15 ms waits for 20 ms), 20 ms over 10 ms steps 10 ms.
    CHECK(plan(config_of(1, std::nullopt, 10), {1000}, 1.0) == (Plan{10, 1000, 9000, 1}));
    CHECK(plan(config_of(1, std::nullopt, 10), {15000}, 1.0) == (Plan{10, 15000, 5000, 5}));
    CHECK(plan(config_of(1, std::nullopt, 20), {10000}, 1.0) == (Plan{20, 10000, 10000, 10}));
    CHECK(plan(config_of(1, std::nullopt, 2), {10000}, 1.0) == (Plan{2, 10000, 0, 10}));
}

TEST_CASE(a_controller_step_coarser_than_the_ecus_steps_is_a_warning_when_a_speed_is_set)
{
    // time_step_ms: 10 over an ECU of 1 ms, at speed 1.
    RunOutcome o = passed_run();
    o.timing.speed = 1.0;
    o.timing.time_step_written = true;
    o.timing.ecu_step_ns = 1'000'000;
    o.timing.step_delay_ns = 9'000'000;
    o.timing.fine_step_ms = 1;
    const std::string what =
        ": with a speed, the launcher keeps real time only at the controller's steps. An ECU step that starts between two "
        "of them runs at the later one, up to 9 ms after the real time it stands for (never before it), together with the "
        "other steps in between; a device attached in real time gets the messages of those steps up to 9 ms late and in a "
        "burst. Write time_step_ms: 1";
    check_text(lines(assess_run(o)),
               "W 0 coarse-time-step: the launcher's controller steps every 10 ms (time_step_ms), but the ECUs' steps are as "
               "small as 1 ms" + what + " (or leave time_step_ms out: with a speed, the default is that)\n");
    CHECK(report_of(o).result() == RunResult::Passed);
    CHECK(report_of(o, true).result() == RunResult::Failed);
    CHECK(launch::detail::settled_findings(o) == assess_run(o));
    // The run started as fast as possible (the default step is 10 ms then) and a speed command
    // came later: at the end a speed is set, or steps were paced.
    o.timing.time_step_written = false;
    check_text(lines(assess_run(o)),
               "W 0 coarse-time-step: the launcher's controller steps every 10 ms (the default of a run that starts without "
               "a speed), but the ECUs' steps are as small as 1 ms" + what +
                   " (or give the speed from the start, with speed or run --speed: the default is then that)\n");
    o.timing.speed.reset();
    o.timing.paced_steps = 3;
    CHECK(lines(assess_run(o)).starts_with("W 0 coarse-time-step: "));
    // As fast as possible all the way: no real time is kept, no finding.
    o.timing.paced_steps = 0;
    check_text(lines(assess_run(o)), "");
    // ECU steps that are larger than the controller's but no multiple of it (15 ms under 10 ms).
    o.timing.speed = 1.0;
    o.timing.time_step_written = true;
    o.timing.ecu_step_ns = 15'000'000;
    o.timing.step_delay_ns = 5'000'000;
    o.timing.fine_step_ms = 5;
    CHECK(lines(assess_run(o)).starts_with("W 0 coarse-time-step: the launcher's controller steps every 10 ms (time_step_ms), "
                                           "but not every ECU step starts at one of its steps: with a speed,"));
    CHECK(lines(assess_run(o)).find("up to 5 ms after the real time") != std::string::npos);
    // Nothing runs late, the steps are not known, or no step would be better: no finding.
    o.timing.step_delay_ns = 0;
    check_text(lines(assess_run(o)), "");
    o.timing.step_delay_ns.reset();
    check_text(lines(assess_run(o)), "");
    o.timing.step_delay_ns = 500'000; // 2.5 ms steps under 1 ms
    o.timing.time_step_ms = 1;
    o.timing.fine_step_ms = 1;
    check_text(lines(assess_run(o)), "");
}

TEST_CASE(the_participants_in_real_time_are_the_described_ecus_and_test_participants_without_a_step)
{
    launch::SimulationConfig config = config_of(7);
    config.participants[1].role = launch::ParticipantRole::Test;
    config.participants[3].role = launch::ParticipantRole::Bridge; // a bridge locks the time: not one of them
    config.participants[4].enabled = false;
    config.participants[5].role = launch::ParticipantRole::Bridge; // as is a bridge of --connect (with_bridges())
    // Ecu0 and Ecu1 (a test participant) describe no step; Ecu2 has one; Ecu6 is not described.
    const std::vector<std::string> peers =
        launch::detail::real_time_peers(config, described(config, {0, 0, 10'000, 0, 0, 0, -1}));
    CHECK((peers == std::vector<std::string>{"Ecu0", "Ecu1"}));
    CHECK(launch::detail::real_time_peers(config, described(config, {10'000, 5'000, 10'000, 0, 0, 0, -1})).empty());
    // A check that has fewer results than the file has participants (it stopped early): no reach beyond.
    launch::CheckReport cut = described(config, {0, 0, 0, 0, 0, 0, 0});
    cut.participants.resize(1);
    CHECK((launch::detail::real_time_peers(config, cut) == std::vector<std::string>{"Ecu0"}));
}

TEST_CASE(what_ends_just_as_an_interrupt_comes_has_ended)
{
    using launch::detail::wait_for;
    using launch::detail::Waited;
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
    CHECK(launch::detail::uri_for_participants("silkit://0.0.0.0:8500") == "silkit://127.0.0.1:8500");
    CHECK(launch::detail::uri_for_participants("silkit://[::]:8500") == "silkit://[::1]:8500");
    CHECK(launch::detail::uri_for_participants("silkit://192.168.1.2:8500") == "silkit://192.168.1.2:8500");
    CHECK(launch::detail::uri_for_participants("silkit://127.0.0.1:41234") == "silkit://127.0.0.1:41234");
}

TEST_CASE(the_time_event_at_the_start_says_paused_only_if_it_stands_still_at_0)
{
    using launch::detail::noted_standing;
    using launch::detail::TimeNote;
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
    using launch::detail::StandStill;
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
    using launch::detail::StrangerWait;
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
    using launch::detail::HostAddress;
    using launch::detail::registry_listen_hint;
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
    using launch::detail::HostAddress;
    using launch::detail::other_computers_line;
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
    using launch::detail::reachable_ipv4_text;
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
    using launch::detail::reachable_ipv6_text;
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
    CHECK((launch::detail::default_route_interfaces(v4, v6) == std::vector<std::string>{"eth0", "wlan0"}));
    CHECK(launch::detail::default_route_interfaces("", "").empty());
    // The order: those with the default route first, IPv4 before IPv6, otherwise as they are.
    const auto order = launch::detail::in_order(
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
    const auto here = launch::detail::host_addresses(true, true);
    bool before = true;
    for (const HostAddress& a : here)
    {
        CHECK(!a.host.empty() && a.host.rfind("127.", 0) != 0 && a.host.rfind("169.254.", 0) != 0 && a.host != "0.0.0.0");
        CHECK(a.host.front() == '[' ? a.host.back() == ']' && a.host.rfind("[fe80", 0) != 0 && a.host != "[::1]"
                                    : std::count(a.host.begin(), a.host.end(), '.') == 3);
        CHECK(before || !a.default_route);
        before = a.default_route;
    }
    for (const HostAddress& a : launch::detail::host_addresses(true, false))
    {
        CHECK(a.host.front() != '[');
    }
    for (const HostAddress& a : launch::detail::host_addresses(false, true))
    {
        CHECK(a.host.front() == '[');
    }
}

TEST_CASE(a_participants_line_is_shown_without_control_characters_and_its_log_has_a_plain_name)
{
    CHECK(launch::detail::printable("a\tb\x1b[2Jc\rd\x7f") == "a\tb?[2Jc?d?");
    CHECK(launch::detail::printable("\xC3\xBC ok") == "\xC3\xBC ok"); // UTF-8 stays
    CHECK(launch::detail::log_name("Engine_2.a-b") == "Engine_2.a-b");
    CHECK(launch::detail::log_name("ECU/1 \xC3\xBC:x") == "ECU_1____x"); // one "_" per byte
}

TEST_CASE(a_participant_that_ended_just_before_the_stop_request_ended_by_itself)
{
    // The request found it ended: its exit is recorded, and it is no "did-not-end".
    ParticipantRun run = participant("Engine", 5);
    run.stopped_after_end = true;
    CHECK(launch::detail::record_stop_request(run, false, ExitStatus{3, std::nullopt}) == launch::detail::StopRequest::HadEnded);
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
    CHECK(launch::detail::record_stop_request(asked, true, std::nullopt) == launch::detail::StopRequest::Sent);
    CHECK(asked.ending == Ending::NotStarted && !asked.stop_request_failed);
    CHECK(launch::detail::record_stop_request(asked, false, std::nullopt) == launch::detail::StopRequest::CannotSend);
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

TEST_CASE(a_run_that_ends_before_stop_at_ms_gets_a_note_and_a_timeout_is_an_error)
{
    RunOutcome o = passed_run();
    o.end.simulation = SimulationEnd::Ended;
    const RunReport early = report_of(o, true);
    check_text(lines(early.findings), "N 0 ended-before-stop-at: the simulation ended before stop_at_ms (1000 ms): a "
                                      "participant stopped it; if that is not expected, the lines above show which one\n");
    CHECK(early.result() == RunResult::Passed); // a note, also with --strict
    o.end.stop_at_ms.reset();
    check_text(lines(assess_run(o)), ""); // nothing to end before
    RunOutcome t = passed_run();
    t.end.simulation = SimulationEnd::Aborted;
    t.end.timed_out = true;
    t.end.timeout_ms = 5000;
    const RunReport timed = report_of(t);
    check_text(lines(timed.findings), "E 0 timeout: the simulation did not end within 5000 ms of wall-clock time "
                                      "(--timeout-ms), so the launcher aborted it: a participant may hang in a step (the "
                                      "lines above show what the participants wrote); if it is only slow, raise "
                                      "--timeout-ms\n");
    CHECK(timed.result() == RunResult::Failed);
    CHECK(format_run_text(timed).find("\nrun: the simulation was aborted: it did not end within --timeout-ms (5000 ms)\n") !=
          std::string::npos);
    // It stood still before its first step for a while: --timeout-ms counted that too.
    t.start.held_before_start_ms = 4200;
    check_text(lines(assess_run(t)), "E 0 timeout: the simulation did not end within 5000 ms of wall-clock time "
                                     "(--timeout-ms), so the launcher aborted it: a participant may hang in a step (the "
                                     "lines above show what the participants wrote); if it is only slow, raise "
                                     "--timeout-ms. Those 5000 ms count from when the run began to stand still before its "
                                     "first step (--start-paused with a control server), and 4200 ms of them passed "
                                     "before a client let it go on: allow for that time in --timeout-ms\n");
}

TEST_CASE(an_interrupt_while_the_launcher_waits_for_the_end_is_no_did_not_end)
{
    RunOutcome o = passed_run();
    o.end.interrupted_by = "SIGINT";
    o.end.interrupted_while_ending = true;
    ParticipantRun stuck = participant("Stuck", 5);
    stuck.ending = Ending::Stopped;
    stuck.exit = ExitStatus{9, std::nullopt};
    o.participants = {stuck};
    const RunReport r = report_of(o);
    check_text(lines(r.findings), "N 0 interrupted: interrupted (SIGINT): the launcher stopped the participants instead "
                                  "of waiting for them to end\n");
    CHECK(r.result() == RunResult::Interrupted);
}

TEST_CASE(the_ecu_waits_for_the_start_longer_than_the_launcher)
{
    SimulationConfig c;
    c.start_timeout_ms = 8000;
    c.stop_timeout_ms = 2000;
    CHECK_EQ(launch::detail::ecu_start_timeout_ms(c, false), std::int64_t{20000});
    CHECK(launch::detail::ecu_start_timeout_ms(c, false) > c.start_timeout_ms + c.stop_timeout_ms);
    // A run that stands still before the first step (--start-paused with a control server) may
    // do so for any time: the ECU's limit must not end it.
    CHECK_EQ(launch::detail::ecu_start_timeout_ms(c, true), kMaxMillis);
    // Never more than ecu_main takes as --start-timeout-ms.
    c.start_timeout_ms = kMaxMillis;
    c.stop_timeout_ms = kMaxMillis;
    CHECK_EQ(launch::detail::ecu_start_timeout_ms(c, false), kMaxMillis);
    c.start_timeout_ms = kMaxMillis - 10000;
    c.stop_timeout_ms = 1;
    CHECK_EQ(launch::detail::ecu_start_timeout_ms(c, false), kMaxMillis);
}

TEST_CASE(a_participant_is_started_with_its_network_names_after_everything_else)
{
    using vehicle_sil::launch::detail::participant_arguments;
    const auto words = [](const std::vector<std::string>& list) {
        std::string out;
        for (const std::string& word : list)
        {
            out += "[" + word + "]";
        }
        return out + "\n";
    };
    SimulationConfig config;
    config.start_timeout_ms = 2000;
    ParticipantConfig p;
    p.name = "Engine";
    p.command = {"engine/engine_ecu", "--verbose"};
    const std::string timeout = std::to_string(vehicle_sil::launch::detail::ecu_start_timeout_ms(config, false));
    const std::string own = "[--verbose][--name][Engine][--registry-uri][silkit://127.0.0.1:8500][--start-timeout-ms][" + timeout + "]";
    // Without network_names: what it was.
    check_text(words(participant_arguments(config, p, "silkit://127.0.0.1:8500", false)), own + "\n");
    // In the file's order and direction (<simulation's name>=<executable's name>), after the
    // launcher's own arguments; a line whose two names are the same is not given.
    p.network_names = {NetworkName{"VehicleCAN", "J1939_CAN", 5}, NetworkName{"Body", "Body", 6},
                       NetworkName{"Body CAN", "CAN_B", 7}};
    check_text(words(participant_arguments(config, p, "silkit://127.0.0.1:8500", false)),
               own + "[--map-network][VehicleCAN=J1939_CAN][--map-network][Body CAN=CAN_B]\n");
    // describe: false: after its command with the placeholders replaced, and a name that looks
    // like a placeholder stays as it is.
    p.describe = false;
    p.command = {"tools/remote", "--uri", "{registry_uri}", "--as", "{name}"};
    p.network_names = {NetworkName{"{name}", "{registry_uri}", 5}};
    check_text(words(participant_arguments(config, p, "silkit://127.0.0.1:8500", false)),
               "[--uri][silkit://127.0.0.1:8500][--as][Engine][--map-network][{name}={registry_uri}]\n");
    p.network_names.clear();
    check_text(words(participant_arguments(config, p, "silkit://127.0.0.1:8500", false)), "[--uri][silkit://127.0.0.1:8500][--as][Engine]\n");
}

TEST_CASE(a_participant_is_started_with_its_j1939_nodes_after_its_network_names)
{
    using vehicle_sil::launch::detail::participant_arguments;
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
    const std::string timeout = std::to_string(vehicle_sil::launch::detail::ecu_start_timeout_ms(config, false));
    const std::string own = "[--name][Engine2][--registry-uri][silkit://127.0.0.1:8500][--start-timeout-ms][" + timeout + "]";
    // After --map-network, in the file's order; an entry that changes nothing is not given.
    p.network_names = {NetworkName{"VehicleCAN", "J1939_CAN", 5}};
    p.j1939_nodes = {J1939NodeEntry{{"VehicleCAN", j1939::NodeOverride{0x00, 0x01, 2, {}, {}}}, 7},
                     J1939NodeEntry{{"VehicleCAN", j1939::NodeOverride{0x21, {}, {}, {}, {}}}, 8},
                     J1939NodeEntry{{"BodyCAN", j1939::NodeOverride{{}, 0xFE, {}, {}, {}}}, 9}};
    check_text(words(participant_arguments(config, p, "silkit://127.0.0.1:8500", false)),
               own + "[--map-network][VehicleCAN=J1939_CAN][--j1939-node][VehicleCAN=from:0x00,address:0x01,identity_number:2]"
                     "[--j1939-node][BodyCAN=address:254]\n");
    // describe: false: after its command with the placeholders replaced.
    p.describe = false;
    p.command = {"tools/remote", "--uri", "{registry_uri}"};
    p.network_names.clear();
    check_text(words(participant_arguments(config, p, "silkit://127.0.0.1:8500", false)),
               "[--uri][silkit://127.0.0.1:8500][--j1939-node][VehicleCAN=from:0x00,address:0x01,identity_number:2]"
               "[--j1939-node][BodyCAN=address:254]\n");
}

TEST_CASE(the_command_to_start_a_participant_by_hand_is_quoted_for_the_shell)
{
    using vehicle_sil::launch::detail::shell_command_line;
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

TEST_CASE(log_names_avoid_windows_devices_and_the_json_names_the_log)
{
    CHECK(launch::detail::log_name("NUL") == "NUL_");
    CHECK(launch::detail::log_name("con.x") == "con_.x");
    CHECK(launch::detail::log_name("Com7") == "Com7_");
    CHECK(launch::detail::log_name("LPT9") == "LPT9_");
    CHECK(launch::detail::log_name("COM10") == "COM10");
    CHECK(launch::detail::log_name("Console") == "Console");
    RunOutcome o = passed_run();
    o.participants[0].log = "logs/Engine.log";
    const std::string json = format_run_json(report_of(o));
    CHECK(json.find("      \"left_processes\": false,\n      \"log\": \"logs/Engine.log\",\n") != std::string::npos);
    CHECK(json.find("      \"left_processes\": false,\n      \"log\": null,\n") != std::string::npos);
}

TEST_CASE(a_participants_line_shows_no_c1_control_and_no_broken_utf8)
{
    CHECK(launch::detail::printable("a\xC2\x9B" "1m\xC2\xA0" "b") == "a?1m\xC2\xA0" "b"); // C1 CSI, then NBSP
    CHECK(launch::detail::printable("\xFF\xC3") == "??");                       // not UTF-8, cut short
    CHECK(launch::detail::printable("\xE2\x82\xAC \xF0\x9F\x98\x80") == "\xE2\x82\xAC \xF0\x9F\x98\x80");
    CHECK(launch::detail::printable("\xC0\xAF\xED\xA0\x80") == "?????");        // overlong, a surrogate
}

TEST_CASE(an_unexpected_failure_is_written_in_the_form_of_the_command)
{
    const std::string run = format_internal_error_json(true, "boom");
    CHECK(run.rfind("{\n  \"vehicle_sil_run\": 2,\n", 0) == 0);
    CHECK(run.find("\"code\": \"internal\"") != std::string::npos && run.find("\"message\": \"boom\"") != std::string::npos);
    CHECK(run.find("  \"errors\": 1,\n") != std::string::npos);
    const std::string check = format_internal_error_json(false, "boom");
    CHECK(check.rfind("{\n  \"vehicle_sil_check\": 1,\n", 0) == 0);
    CHECK(check.find("\"code\": \"internal\"") != std::string::npos);
}

TEST_CASE(a_wrong_command_line_and_an_internal_error_are_written_in_the_form_of_each_command)
{
    const std::vector<Finding> errors{Finding{Severity::Error, "usage", "bad", 0, "", ""}};
    const std::vector<std::string> variants{"left_hand"};
    const std::string check = format_usage_json(CommandKind::Check, "a.yaml", errors, variants);
    CHECK(check.rfind("{\n  \"vehicle_sil_check\": 1,\n", 0) == 0);
    CHECK(check.find("\"result\": \"usage\"") != std::string::npos && check.find("left_hand") != std::string::npos);
    CHECK(format_usage_json(CommandKind::Run, "a.yaml", errors, variants).rfind("{\n  \"vehicle_sil_run\": 2,\n", 0) == 0);
    CHECK(format_usage_json(CommandKind::Test, "a.yaml", errors).find("\"usage\"") != std::string::npos);
    CHECK(format_usage_json(CommandKind::Connections, "a.yaml", errors).find("\"usage\"") != std::string::npos);
    const RunReport event = usage_run_report("a.yaml", errors, variants);
    CHECK(event.usage && event.check.usage && event.exit_code() == 4);
    CHECK(internal_error_run_report("boom").findings.at(0).code == "internal");
    for (const CommandKind command : {CommandKind::Check, CommandKind::Run, CommandKind::Test, CommandKind::Connections})
    {
        CHECK(format_internal_error_json(command, "a.yaml", "boom").find("\"code\": \"internal\"") != std::string::npos);
    }
}

TEST_CASE(a_disabled_participant_is_shown_as_not_started_and_is_no_finding)
{
    RunOutcome o = passed_run();
    ParticipantRun off = participant("Brake", 7);
    off.enabled = false;
    o.participants[1] = off;
    const RunReport r = report_of(o);
    check_text(lines(r.findings), "");
    CHECK(r.result() == RunResult::Passed);
    const std::string text = format_run_text(r);
    check_text(text.substr(text.find("run: ")), "run: the simulation ran to stop_at_ms (1000 ms) and ended\n"
                                                "\n"
                                                "participant  required  ended\n"
                                                "Engine       yes       exit code 0\n"
                                                "Brake        -         not started (disabled)\n"
                                                "\n"
                                                "passed: 0 errors, 0 warnings, 0 notes\n");
    const std::string json = format_run_json(r);
    CHECK(json.find("      \"name\": \"Brake\",\n      \"required\": true,\n      \"enabled\": false,\n      \"manual\": false,\n      \"for_network\": null,\n      \"ended\": "
                    "\"not_started\",\n      \"exit_code\": null,\n      \"signal\": null,\n      \"left_processes\": false,\n"
                    "      \"log\": null,\n      \"control_link\": {\n        \"state\": \"none\",\n        \"variables\": null,\n        \"version\": null,\n        \"late_messages\": 0\n      }\n") != std::string::npos);
}

// The limit of the start, in the orders the main thread and the start window's thread can meet
// (no clock: the times are given).
TEST_CASE(the_start_limit_of_a_run_without_manual_participants_is_the_one_set)
{
    using vehicle_sil::launch::detail::StartLimit;
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
    using vehicle_sil::launch::detail::StartLimit;
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

namespace {

/// passed_run() with the bridge of --connect for ETH (line 3) at eth0, which ended as `bridge` says.
RunOutcome connected_run(ParticipantRun bridge)
{
    RunOutcome o = passed_run();
    bridge.name = "bridge.ETH";
    bridge.line = 3;
    bridge.required = false;
    bridge.for_network = "ETH";
    o.participants.push_back(std::move(bridge));
    o.outside.push_back(RunOutside{"ETH", "eth0", "bridge.ETH", std::nullopt, std::nullopt});
    return o;
}

ParticipantRun stopped_bridge(int code)
{
    ParticipantRun p;
    p.ending = Ending::Stopped;
    p.exit = ExitStatus{code, std::nullopt};
    return p;
}

} // namespace

TEST_CASE(with_bridges_adds_a_participant_for_each_connected_network_after_those_of_the_file)
{
    CheckReport check;
    check.file = "sim.yaml";
    check.config = read_config("vehicle_sil_simulation: 1\n"
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
    CHECK_EQ(launch::detail::with_bridges(check).config->participants.size(), std::size_t{1});
    check.outside = {NetworkOutside{"CAN1", "vehicle_sil_vector_xl_bridge", "/t/xl", "virtual:1", "bridge.CAN1", std::nullopt},
                     NetworkOutside{"ETH", "vehicle_sil_ethernet_bridge", "/t/eth", "eth0", "bridge.ETH", std::nullopt}};
    const CheckReport run = launch::detail::with_bridges(check);
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

TEST_CASE(a_bridge_that_ends_before_the_start_is_connect_failed_and_nothing_else)
{
    // Its process ended while the launcher waited for it.
    ParticipantRun ended;
    ended.ending = Ending::ExitedBeforeStart;
    ended.exit = ExitStatus{3, std::nullopt};
    ended.last_line = "[eth-bridge] FAIL: no such interface: eth0";
    RunOutcome o = connected_run(ended);
    o.end.simulation = SimulationEnd::NotStarted;
    o.end.failed_participant = "bridge.ETH";
    o.participants[0] = participant("Engine", 5);
    o.participants[1] = participant("Brake", 7);
    const RunReport r = report_of(o);
    check_text(lines(r.findings),
               "E 3 connect-failed: network 'ETH' could not be connected to the real bus (eth0): bridge.ETH ended before "
               "the simulation started (its process exited with code 3; it wrote: [eth-bridge] FAIL: no such "
               "interface: eth0), so nothing else was started. Check that eth0 is there and free on this computer "
               "(vehicle_sil_launch connections <file> lists where each network can be connected), or give --connect "
               "ETH=none to run the network on SIL Kit only\n");
    CHECK(r.result() == RunResult::Failed);
    CHECK(r.findings[0].participant == "bridge.ETH" && r.findings[0].network == "ETH");
    // Its line is known only once its output has been read: not settled before.
    CHECK(launch::detail::settled_findings(o).empty());
    // The other of two bridges that ended at once: seen when the launcher ended the run.
    o.participants[2].ending = Ending::Exited;
    o.end.failed_participant.clear();
    CHECK(lines(report_of(o).findings).rfind("E 3 connect-failed: network 'ETH' could not be connected to the real bus "
                                             "(eth0): bridge.ETH ended before the simulation started (its process exited "
                                             "with code 3; ", 0) == 0);
    // It could not be started at all.
    ParticipantRun unstarted;
    unstarted.start_error = "no such file";
    o = connected_run(unstarted);
    o.end.simulation = SimulationEnd::NotStarted;
    o.participants[0] = participant("Engine", 5);
    o.participants[1] = participant("Brake", 7);
    check_text(lines(report_of(o).findings),
               "E 3 connect-failed: network 'ETH' could not be connected to the real bus (eth0): bridge.ETH could not be "
               "started (no such file), so nothing else was started. Check that eth0 is there and free on this computer "
               "(vehicle_sil_launch connections <file> lists where each network can be connected), or give --connect "
               "ETH=none to run the network on SIL Kit only\n");
    // It did not get to Running in time.
    ParticipantRun slow = stopped_bridge(0);
    slow.missing_at_start = true;
    o = connected_run(slow);
    o.end.simulation = SimulationEnd::NotStarted;
    o.participants[0] = participant("Engine", 5);
    o.participants[1] = participant("Brake", 7);
    check_text(lines(report_of(o).findings),
               "E 3 connect-failed: network 'ETH' could not be connected to the real bus (eth0): bridge.ETH did not run in "
               "SIL Kit within 30000 ms (start_timeout_ms; it wrote no line that says why), so nothing else was started. Check "
               "that eth0 "
               "is there and free on this computer (vehicle_sil_launch connections <file> lists where each network can be "
               "connected), or give --connect ETH=none to run the network on SIL Kit only\n");
    // It left the simulation before the start while its process ran (the launcher stopped it).
    o = connected_run(stopped_bridge(0));
    o.end.simulation = SimulationEnd::NotStarted;
    o.end.failed_participant = "bridge.ETH";
    o.end.failure_reason = "left the simulation (state Error)";
    o.participants[0] = participant("Engine", 5);
    o.participants[1] = participant("Brake", 7);
    CHECK(lines(report_of(o).findings).find("bridge.ETH ended before the simulation started (it left the simulation (state "
                                            "Error); it wrote no line that says why)") != std::string::npos);
    // After the participants of the file were started (it ended while the run waited for the start).
    o.participants[0] = exited(participant("Engine", 5), 0);
    CHECK(lines(report_of(o).findings).find("), so the simulation was not started. Check that eth0 ") != std::string::npos);
}

TEST_CASE(a_bridge_that_ends_while_the_simulation_runs_is_connect_lost_whatever_its_exit_code)
{
    for (const int code : {0, 1})
    {
        ParticipantRun gone;
        gone.ending = Ending::Exited;
        gone.exit = ExitStatus{code, std::nullopt};
        gone.last_line = "[eth-bridge] FAIL: interface eth0 went down";
        RunOutcome o = connected_run(gone);
        o.end.simulation = SimulationEnd::Aborted;
        o.end.failed_participant = "bridge.ETH";
        const RunReport r = report_of(o);
        check_text(lines(r.findings),
                   "E 3 connect-lost: network 'ETH': the connection to the real bus (eth0) was lost while the simulation "
                   "was running: bridge.ETH ended (its process exited with code " + std::to_string(code) +
                       "; it wrote: [eth-bridge] FAIL: interface eth0 went down), so the launcher aborted the simulation. The devices on the "
                       "bus missed what came after; the lines [bridge.ETH] above show what the bridge wrote\n");
        CHECK(r.result() == RunResult::Failed);
        CHECK(launch::detail::settled_findings(o).empty());
    }
}

TEST_CASE(a_bridge_that_had_ended_when_the_simulation_ended_is_connect_lost)
{
    ParticipantRun gone;
    gone.ending = Ending::Exited;
    gone.exit = ExitStatus{0, std::nullopt};
    gone.ended_with_run = true;
    const RunReport r = report_of(connected_run(gone));
    check_text(lines(r.findings),
               "E 3 connect-lost: network 'ETH': the connection to the real bus (eth0) was lost at about the time the "
               "simulation ended: bridge.ETH ended (its process exited with code 0; it wrote no line that says why) before "
               "the launcher asked it to stop. The devices on the bus may have missed the last frames; the lines "
               "[bridge.ETH] above show what the bridge wrote\n");
    CHECK(r.result() == RunResult::Failed);
}

TEST_CASE(a_bridge_that_follows_an_abort_for_another_participant_is_no_finding)
{
    // Engine failed; SIL Kit told the bridge of the abort, and it ended by itself with 1 before the
    // launcher asked it to stop. Its end is in the result as it was, without a finding.
    ParticipantRun followed;
    followed.ending = Ending::Exited;
    followed.exit = ExitStatus{1, std::nullopt};
    followed.last_line = "[eth-bridge] FAIL: SIL Kit participant left Running (state Shutdown)";
    RunOutcome o = connected_run(followed);
    o.end.simulation = SimulationEnd::Aborted;
    o.participants[0] = exited(participant("Engine", 5), 7);
    o.end.failed_participant = "Engine";
    const RunReport r = report_of(o);
    CHECK(lines(r.findings).find("bridge.ETH") == std::string::npos);
    CHECK(lines(r.findings).find("participant-failed") != std::string::npos);
    CHECK(format_run_json(r).find("\"for_network\": \"ETH\",\n      \"ended\": \"exited\",\n      \"exit_code\": 1,") !=
          std::string::npos);
}

TEST_CASE(the_line_of_a_bridge_that_says_why_it_ended)
{
    const auto failure = [](std::initializer_list<const char*> lines) {
        launch::detail::LastLine last;
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

TEST_CASE(a_bridge_that_the_launcher_stopped_after_the_run_is_no_finding)
{
    RunOutcome o = connected_run(stopped_bridge(0));
    check_text(lines(report_of(o).findings), "");
    CHECK(report_of(o).result() == RunResult::Passed);
    // One that ended by itself after the simulation was over: as any optional participant.
    ParticipantRun late;
    late.ending = Ending::Exited;
    late.exit = ExitStatus{2, std::nullopt};
    o = connected_run(late);
    check_text(lines(report_of(o).findings),
               "W 3 exit-code: participant 'bridge.ETH': its process exited with code 2; the lines [bridge.ETH] above show "
               "what it wrote\n");
}

TEST_CASE(a_time_command_in_a_run_that_is_connected_outside_is_one_note_about_the_first)
{
    // Such a run takes the command only with --time-policy free: the lock names what connects it.
    RunOutcome o = connected_run(stopped_bridge(0));
    o.time_lock = TimeLock{false, {"ETH"}, {}};
    o.time_changed = TimeChange{"pause", "stdin", 1200, std::nullopt, std::nullopt, std::nullopt};
    check_text(lines(report_of(o).findings),
               "N 0 outside-time-changed: --connect connects ETH to a real bus, which does not wait for the virtual time, "
               "and with --time-policy free the run took the time command 'pause' from stdin at 1200 ms: the simulation "
               "stood still while what is connected there went on in real time, so from then on it got the messages at "
               "the wrong times, and the bridge carried frames both ways all the while (frames from outside pile up while "
               "the simulation stands still). It is the first command of the run that paused, stepped or changed the "
               "speed (the command events show each one). To keep the run to real time, leave --time-policy free out; to "
               "pause, step or change the speed with nothing outside, run without --connect\n");
    // A note: the user chose it, so --strict passes too.
    CHECK(report_of(o).result() == RunResult::Passed);
    CHECK(report_of(o, true).result() == RunResult::Passed);
    // Known once the simulation is over: an event before the participants have ended.
    CHECK(launch::detail::settled_findings(o).size() == 1);
    // How each command is named, and what it did.
    const auto said = [&o](TimeChange t) {
        o.time_changed = std::move(t);
        const std::vector<Finding> f = assess_run(o);
        CHECK(f.size() == 1);
        return f.empty() ? std::string() : f[0].message;
    };
    const auto contains = [](const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; };
    CHECK(contains(said({"step", "control_server", 0, 50, std::nullopt, std::nullopt}),
                   "the time command 'step 50 ms' from control_server at 0 ms: the simulation stood still while"));
    CHECK(contains(said({"run_until", "control_server", 70, std::nullopt, 300, std::nullopt}),
                   "the time command 'run_until 300 ms' from control_server at 70 ms: the simulation stood still while"));
    CHECK(contains(said({"pause", "control_server", 70, std::nullopt, 500, std::nullopt}),
                   "the time command 'pause at 500 ms' from control_server at 70 ms: the simulation stood still while"));
    CHECK(contains(said({"speed", "stdin", 400, std::nullopt, std::nullopt, 2.5}),
                   "the time command 'speed 2.5' from stdin at 400 ms: the simulation ran at speed 2.5 while what is"));
    CHECK(contains(said({"speed", "stdin", 400, std::nullopt, std::nullopt, std::nullopt}),
                   "the time command 'speed max' from stdin at 400 ms: the simulation ran as fast as possible while"));
    // Two networks and a participant that is a bridge: all named, and how to run without each.
    o.time_lock = TimeLock{false, {"ETH", "CAN"}, {"Gateway"}};
    const std::string all = said({"pause", "stdin", 0, std::nullopt, std::nullopt, std::nullopt});
    CHECK(contains(all, "--connect connects ETH and CAN to a real bus, and participant 'Gateway' is a bridge (role: bridge) "
                        "to something outside the simulation, which does not wait"));
    CHECK(contains(all, "run without --connect and with --disable Gateway"));
    // A participant that is a bridge alone (no --connect, so no `outside` in the result).
    RunOutcome bridged = passed_run();
    bridged.time_lock = TimeLock{false, {}, {"Gateway"}};
    bridged.time_changed = TimeChange{"pause", "stdin", 1200, std::nullopt, std::nullopt, std::nullopt};
    check_text(lines(assess_run(bridged)).substr(0, 60), "N 0 outside-time-changed: participant 'Gateway' is a bridge ");
    // Not connected outside, or without such a command: nothing.
    RunOutcome plain = passed_run();
    plain.time_changed = TimeChange{"pause", "stdin", 1200, std::nullopt, std::nullopt, std::nullopt};
    CHECK(assess_run(plain).empty());
    RunOutcome quiet = connected_run(stopped_bridge(0));
    quiet.time_lock = TimeLock{false, {"ETH"}, {}};
    CHECK(assess_run(quiet).empty());
    quiet.time_lock = TimeLock{true, {"ETH"}, {}};
    CHECK(assess_run(quiet).empty());
}

TEST_CASE(a_time_command_in_a_run_with_a_participant_in_real_time_is_one_note_about_the_first)
{
    RunOutcome o = passed_run();
    o.real_time_peers = {"Tool"};
    CHECK(assess_run(o).empty()); // no such command: nothing
    o.time_changed = TimeChange{"pause", "stdin", 1200, std::nullopt, std::nullopt, std::nullopt};
    check_text(lines(report_of(o).findings),
               "N 0 unsynchronized-time-changed: participant 'Tool' runs in real time (no step: the virtual time does not "
               "move it), and the run took the time command 'pause' from stdin at 1200 ms: the simulation stood still "
               "while that participant went on, so what it sent and saw from then on does not fit the virtual time, and "
               "another run with the same commands may go differently. It is the first command of the run that paused, "
               "stepped or changed the speed (the command events show each one). For a run that repeats, give the "
               "participant a step (so that it follows the virtual time), or do not pause, step or change the speed\n");
    CHECK(report_of(o, true).result() == RunResult::Passed);
    CHECK(launch::detail::settled_findings(o).size() == 1);
    o.real_time_peers = {"Tool", "Spy"};
    o.time_changed = TimeChange{"speed", "control_server", 0, std::nullopt, std::nullopt, 4.0};
    const std::vector<Finding> two = assess_run(o);
    CHECK(two.size() == 1);
    if (two.size() == 1)
    {
        CHECK(two[0].message.find("participants 'Tool' and 'Spy' run in real time (no step: the virtual time does not move "
                                  "them), and the run took the time command 'speed 4' from control_server at 0 ms: the "
                                  "simulation ran at speed 4 while those participants went on, so what they sent") !=
              std::string::npos);
        CHECK(two[0].message.find("give each of them a step") != std::string::npos);
    }
    // Connected outside as well (with --time-policy free): both notes, the outside one first.
    o.time_lock = TimeLock{false, {"ETH"}, {}};
    const std::vector<Finding> both = assess_run(o);
    CHECK(both.size() == 2);
    if (both.size() == 2)
    {
        check_text(both[0].code, "outside-time-changed");
        check_text(both[1].code, "unsynchronized-time-changed");
        CHECK(both[0].severity == Severity::Note && both[1].severity == Severity::Note);
    }
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

TEST_CASE(the_outputs_of_a_connected_run_have_the_bridge_and_where_it_connects)
{
    const RunOutcome o = connected_run(stopped_bridge(0));
    const RunReport r = report_of(o);
    const std::string text = format_run_text(r);
    CHECK(text.find("outside: ETH -> eth0 (bridge.ETH)\n\nparticipant  required  ended\n") != std::string::npos);
    CHECK(text.find("bridge.ETH   no        stopped (" + kStop + "), exit code 0\n") != std::string::npos);
    const std::string json = format_run_json(r);
    CHECK(json.find("      \"name\": \"bridge.ETH\",\n      \"required\": false,\n      \"enabled\": true,\n      \"manual\": false,\n"
                    "      \"for_network\": \"ETH\",\n      \"ended\": \"stopped\",\n") != std::string::npos);
    CHECK(json.find("  \"outside\": [\n    {\n      \"network\": \"ETH\",\n      \"target\": \"eth0\",\n      \"participant\": "
                    "\"bridge.ETH\",\n      \"to_outside\": null,\n      \"from_outside\": null,\n      \"counts\": {}\n    }\n  ],\n") != std::string::npos);
    // The tool is not in the result.
    CHECK(json.find("vehicle_sil_ethernet_bridge") == std::string::npos);
    // Without --connect: no line, and no change in the table.
    CHECK(format_run_text(report_of(passed_run())).find("outside") == std::string::npos);
}

TEST_CASE(a_bridge_that_sent_and_got_nothing_from_a_device_is_a_note)
{
    const auto counted = [](std::optional<std::uint64_t> to, std::optional<std::uint64_t> from) {
        RunOutcome o = connected_run(stopped_bridge(0));
        o.outside[0].to_outside = to;
        o.outside[0].from_outside = from;
        o.outside[0].tool = "vehicle_sil_ethernet_bridge";
        return o;
    };
    RunOutcome o = counted(152, 0);
    check_text(lines(report_of(o).findings),
               "N 3 connect-nothing-received: network 'ETH': nothing came from a device on the real bus (eth0) before the run "
               "ended: bridge.ETH sent 152 frames there and got none from a device (frames that this computer sent do not "
               "count). If a device is meant to answer, check that it is on, its cable, and that --connect names the adapter "
               "it is on; a short run can end before a device answers\n");
    CHECK(report_of(o).findings[0].participant == "bridge.ETH" && report_of(o).findings[0].network == "ETH");
    // A note: --strict passes.
    CHECK(report_of(o, true).result() == RunResult::Passed);
    // Known once the bridge's output has been read to the end: no event before the participants have ended.
    CHECK(launch::detail::settled_findings(o).empty());
    // One frame; the bridge of a CAN network.
    o = counted(1, 0);
    o.outside[0].tool = "vehicle_sil_vector_xl_bridge";
    check_text(lines(assess_run(o)),
               "N 3 connect-nothing-received: network 'ETH': nothing came from a device on the real bus (eth0) before the run "
               "ended: bridge.ETH sent 1 frame there and got none from a device. If a device is meant to answer, check that "
               "it is on, its cable and termination, the bit rate, and that --connect names the channel it is on; a short run "
               "can end before a device answers\n");
    // Nothing: a frame came, nothing was sent, the counts are not known (none could be read), or only one of them.
    CHECK(assess_run(counted(152, 1)).empty());
    CHECK(assess_run(counted(0, 0)).empty());
    CHECK(assess_run(counted(std::nullopt, std::nullopt)).empty());
    CHECK(assess_run(counted(152, std::nullopt)).empty());
    // The simulation did not start.
    o = counted(152, 0);
    o.end.simulation = SimulationEnd::NotStarted;
    CHECK(lines(assess_run(o)).find("connect-nothing-received") == std::string::npos);
    // The bridge failed the run: connect-lost says it.
    o = counted(152, 0);
    o.participants.back().ending = Ending::Exited;
    o.participants.back().exit = ExitStatus{1, std::nullopt};
    o.end.simulation = SimulationEnd::Aborted;
    o.end.failed_participant = "bridge.ETH";
    const std::string found = lines(assess_run(o));
    CHECK(found.find("connect-lost") != std::string::npos && found.find("connect-nothing-received") == std::string::npos);
}

TEST_CASE(what_a_bridge_saw_of_the_real_bus_is_a_warning_for_each_kind)
{
    const auto seen = [](OutsideLink link, const char* tool = "vehicle_sil_ethernet_bridge") {
        RunOutcome o = connected_run(stopped_bridge(0));
        o.outside[0].tool = tool;
        o.outside[0].link = std::move(link);
        return o;
    };
    // Nothing seen: no finding.
    CHECK(assess_run(seen({})).empty());
    // The link: once or more, with the time in all.
    OutsideLink down;
    down.down_count = 1;
    down.down_total = 2700ms;
    RunOutcome o = seen(down);
    const std::string found = lines(assess_run(o));
    CHECK(found.find("W 3 outside-link-down: network 'ETH' -> eth0: the link was down once, for 2.70 s in all.") == 0);
    CHECK(found.find("--strict") != std::string::npos && found.find("cable") != std::string::npos);
    down.down_count = 3;
    CHECK(lines(assess_run(seen(down))).find("the link was down 3 times, for 2.70 s in all") != std::string::npos);
    // A warning: --strict fails, and the end of the participants decides (no event before).
    CHECK(report_of(o, true).result() != RunResult::Passed);
    CHECK(report_of(o).result() == RunResult::Passed);
    CHECK(launch::detail::settled_findings(o).empty());
    // The bus.
    OutsideLink bus;
    bus.bus_off_count = 2;
    bus.bus_off_total = 1500ms;
    CHECK(lines(assess_run(seen(bus, "vehicle_sil_vector_xl_bridge")))
              .find("outside-bus-off: network 'ETH' -> eth0: the controller was bus off 2 times, for 1.50 s in all.") !=
          std::string::npos);
    OutsideLink passive;
    passive.passive_total = 4000ms;
    passive.counts = {{"error_frames", 17}};
    CHECK(lines(assess_run(seen(passive, "vehicle_sil_vector_xl_bridge")))
              .find("outside-bus-errors: network 'ETH' -> eth0: the controller was error passive for 4.00 s in all (error_frames=17).") !=
          std::string::npos);
    // Dropped frames, from the table only, with the counts: one frame is enough. Not the other keys.
    OutsideLink dropped;
    dropped.counts = {{"ext_queue_full", 1}, {"ext_tx_failures", 3}, {"oversize", 8}, {"own_tx_dropped", 4}};
    const std::string drop = lines(assess_run(seen(dropped)));
    CHECK(drop.find("outside-frames-dropped: network 'ETH' -> eth0: the bridge dropped frames (ext_queue_full=1, ext_tx_failures=3):") !=
          std::string::npos);
    CHECK(drop.find("oversize") == std::string::npos && drop.find("--tx-timeout-ms") == std::string::npos);
    dropped.counts = {{"ext_tx_dropped", 2}};
    CHECK(lines(assess_run(seen(dropped, "vehicle_sil_vector_xl_bridge"))).find("--tx-timeout-ms") != std::string::npos);
    // Counts of 0 and the keys that are no losses: nothing.
    dropped.counts = {{"ext_queue_full", 0}, {"oversize", 8}, {"malformed", 1}, {"ext_checksum_incomplete", 5}, {"filtered_to_ext", 2}};
    CHECK(assess_run(seen(dropped)).empty());
    // A loop.
    OutsideLink loop;
    loop.counts = {{"loop_suspected", 1}};
    CHECK(lines(assess_run(seen(loop))).find("outside-loop-suspected: network 'ETH' -> eth0: the bridge stopped 1 frame whose source MAC address was on both sides (loop_suspected)") !=
          std::string::npos);
    loop.counts = {{"loop_suspected", 0}};
    CHECK(assess_run(seen(loop)).empty());
    // The simulation did not start: none.
    o = seen(down);
    o.end.simulation = SimulationEnd::NotStarted;
    CHECK(lines(assess_run(o)).find("outside-link-down") == std::string::npos);
}

TEST_CASE(a_device_that_another_run_used_too_is_a_warning_with_the_pid_and_the_time)
{
    RunOutcome o = connected_run(stopped_bridge(0));
    o.outside[0].tool = "vehicle_sil_ethernet_bridge";
    o.outside[0].link.device = "ab12";
    // Nobody else: no finding.
    CHECK(assess_run(o).empty());
    o.outside[0].link.shared_most = 1;
    o.outside[0].link.shared_peers = {4321};
    o.outside[0].link.shared_first = 3100ms;
    const std::string found = lines(assess_run(o));
    CHECK(found.find("W 3 outside-device-shared: network 'ETH' -> eth0: the device was also used by another vehicle-sil bridge "
                     "(process 4321) that is not part of this run from 3.10 s on: the frames of both mix there") == 0);
    CHECK(found.find("Connect each run to a device of its own, or stop the other bridge") != std::string::npos &&
          found.find("--strict does not start a run on a device that is shared, and stops one that shares it for 3 s") != std::string::npos);
    // --strict fails on the warning; the end of the participants decides (no event before).
    CHECK(report_of(o, true).result() != RunResult::Passed);
    CHECK(report_of(o).result() == RunResult::Passed);
    CHECK(launch::detail::settled_findings(o).empty());
    // No pid (Windows), and more than one.
    o.outside[0].link.shared_peers = {};
    CHECK(lines(assess_run(o)).find("used by another vehicle-sil bridge that is not part of this run") != std::string::npos);
    o.outside[0].link.shared_most = 2;
    o.outside[0].link.shared_peers = {4321, 4400};
    CHECK(lines(assess_run(o)).find("used by 2 other vehicle-sil bridges (processes 4321, 4400) that is not part of this run") != std::string::npos);
}

TEST_CASE(a_strict_run_that_was_stopped_for_a_shared_device_says_so_as_an_error)
{
    // After 3 s of it: the bridge is blamed (the watch stopped the run), and what it says is the error.
    RunOutcome o = connected_run(stopped_bridge(0));
    o.outside[0].tool = "vehicle_sil_vector_xl_bridge";
    o.outside[0].link.shared_most = 1;
    o.outside[0].link.shared_peers = {4321};
    o.outside[0].link.shared_first = 12500ms;
    o.outside[0].link.shared_stop = OutsideLink::SharedStop::AfterDelay;
    o.end.simulation = SimulationEnd::Aborted;
    o.end.failed_participant = "bridge.ETH";
    const std::string found = lines(assess_run(o));
    CHECK(found.find("E 3 outside-device-shared: network 'ETH' -> eth0: the device was also used by another vehicle-sil bridge "
                     "(process 4321) that is not part of this run from 12.50 s on, and --strict stopped the run after 3 s of it: ") == 0);
    // Said once: neither participant-failed nor the warning again.
    CHECK(found.find("participant-failed") == std::string::npos && found.find("W 3 outside-device-shared") == std::string::npos);
    CHECK(launch::detail::settled_findings(o).empty());
    // Before the simulation started: nothing else was started, the bridge did not fail.
    RunOutcome start = connected_run(stopped_bridge(0));
    start.outside[0].tool = "vehicle_sil_vector_xl_bridge";
    start.outside[0].link.shared_most = 1;
    start.outside[0].link.shared_first = 100ms;
    start.outside[0].link.shared_stop = OutsideLink::SharedStop::AtStart;
    start.end.simulation = SimulationEnd::NotStarted;
    const std::string before = lines(assess_run(start));
    CHECK(before.find("E 3 outside-device-shared: network 'ETH' -> eth0: the device is also used by another vehicle-sil bridge that is not "
                      "part of this run, and --strict does not start a run on it: ") == 0);
    CHECK(before.find("connect-failed") == std::string::npos && before.find("without --strict the run starts and warns") != std::string::npos);
    CHECK(report_of(start).result() == RunResult::Failed);
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
    CHECK(launch::detail::settled_findings(o).empty());
}

TEST_CASE(the_outputs_of_a_connected_run_have_the_counts)
{
    RunOutcome o = connected_run(stopped_bridge(0));
    o.outside[0].to_outside = 152;
    o.outside[0].from_outside = 3;
    o.outside[0].link.counts = {{"ext_from_devices", 3}, {"ext_queue_full", 0}, {"silkit_to_ext", 152}};
    const RunReport r = report_of(o);
    CHECK(format_run_text(r).find("outside: ETH -> eth0 (bridge.ETH): 152 frames out, 3 from devices\n\n") != std::string::npos);
    o.outside[0].to_outside = 1;
    CHECK(format_run_text(report_of(o)).find("outside: ETH -> eth0 (bridge.ETH): 1 frame out, 3 from devices\n") != std::string::npos);
    CHECK(format_run_json(r).find("\"participant\": \"bridge.ETH\",\n      \"to_outside\": 152,\n      \"from_outside\": 3,\n      \"counts\": {\n"
                              "        \"ext_from_devices\": 3,\n        \"ext_queue_full\": 0,\n        \"silkit_to_ext\": 152\n      }\n") !=
          std::string::npos);
    CHECK(lines(r.findings).empty());
}

namespace {

/// A run that `by` (of `kind`) stopped while the simulation ran.
RunOutcome stopped_run(std::string by, vehicle_sil::launch::StopKind kind)
{
    RunOutcome o = passed_run();
    o.end.simulation = SimulationEnd::Interrupted;
    o.end.interrupted_by = std::move(by);
    o.end.interrupted_kind = kind;
    return o;
}

/// `o` with a scenario of one step in `status` (made in place: no struct is built and moved).
RunOutcome with_scenario(RunOutcome o, ScenarioStatus status)
{
    vehicle_sil::launch::ScenarioRun& scenario = o.scenario.result.emplace();
    scenario.file = "s.yaml";
    scenario.status = status;
    vehicle_sil::launch::ScenarioStepRun& step = scenario.steps.emplace_back();
    step.index = 1;
    step.line = 4;
    step.text = "expect Body.level == 1";
    step.status = status == ScenarioStatus::Passed       ? vehicle_sil::launch::StepStatus::Passed
                  : status == ScenarioStatus::Failed     ? vehicle_sil::launch::StepStatus::Failed
                  : status == ScenarioStatus::Incomplete ? vehicle_sil::launch::StepStatus::Unfinished
                                                         : vehicle_sil::launch::StepStatus::NotRun;
    if (status == ScenarioStatus::Failed)
    {
        step.code = "scenario-expect";
        step.message = "it was 0";
    }
    return o;
}

struct Meaning
{
    RunResult result;
    int exit_code;
    bool ok;
    const char* end; ///< the JSON's "end" with its quotes, or "null"
};

/// The result, the exit code and ok of `r` agree with `want`, with each other and with the JSON,
/// and there is a message of one line.
void check_meaning(const RunReport& r, const Meaning& want, const char* where)
{
    const auto text = [](RunResult result, int code, bool ok) {
        return std::to_string(static_cast<int>(result)) + " exit " + std::to_string(code) + (ok ? " ok" : " not ok");
    };
    check_text(text(r.result(), r.exit_code(), r.ok()), text(want.result, want.exit_code, want.ok), where);
    check_text(r.ok() == (r.exit_code() == 0) ? "ok is exit code 0" : "ok differs from exit code 0", "ok is exit code 0", where);
    const std::string message = r.message();
    check_text(message.empty() || message.find('\n') != std::string::npos ? "no message of one line" : "", "", where);
    const std::string json = format_run_json(r);
    const std::string members =
        std::string("  \"end\": ") + want.end + ",\n  \"ok\": " + (want.ok ? "true" : "false") + ",\n  \"message\": \"";
    check_text(json.find(members) != std::string::npos ? members : json.substr(0, 300), members, where);
}

} // namespace

TEST_CASE(why_a_run_ended_is_decided_by_the_setup_then_by_what_asked_first_then_by_the_simulation)
{
    using vehicle_sil::launch::end_reason;
    using vehicle_sil::launch::EndReason;
    using vehicle_sil::launch::StopKind;
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
    // What asked first decides, also when the simulation had ended or failed before it.
    RunOutcome late = stopped_run("stop command", StopKind::Operator);
    late.end.simulation = SimulationEnd::Aborted;
    late.end.interrupted_while_ending = true;
    CHECK(end_reason(late) == EndReason::Stopped);
    // A set-up that failed is stronger than a stop.
    late.setup.error = "no registry";
    CHECK(end_reason(late) == EndReason::SetupFailed);
    check_text(vehicle_sil::launch::end_reason_name(EndReason::Completed), "completed");
    check_text(vehicle_sil::launch::end_reason_name(EndReason::Stopped), "stopped");
    check_text(vehicle_sil::launch::end_reason_name(EndReason::Interrupted), "interrupted");
    check_text(vehicle_sil::launch::end_reason_name(EndReason::Failed), "failed");
    check_text(vehicle_sil::launch::end_reason_name(EndReason::SetupFailed), "setup_failed");
}

TEST_CASE(each_result_has_its_exit_code_its_ok_and_a_message)
{
    using vehicle_sil::launch::StopKind;
    CHECK_EQ(vehicle_sil::launch::kExitIncomplete, 5);
    // Reached its end.
    check_meaning(report_of(passed_run()), {RunResult::Passed, 0, true, "\"completed\""}, "passed");
    check_meaning(report_of(with_scenario(passed_run(), ScenarioStatus::Passed)), {RunResult::Passed, 0, true, "\"completed\""},
                  "scenario passed");
    check_text(report_of(passed_run()).message(), "The run reached its end without errors");
    check_text(report_of(with_scenario(passed_run(), ScenarioStatus::Passed)).message(),
               "The run reached its end without errors, and the scenario passed");
    // An operator stopped it: a success without a scenario (or after it had passed).
    const RunReport stopped = report_of(stopped_run("stop command", StopKind::Operator));
    check_meaning(stopped, {RunResult::Stopped, 0, true, "\"stopped\""}, "stopped");
    check_text(stopped.message(), "The run was stopped by a stop request without errors");
    check_text(lines(stopped.findings), "N 0 stopped: stopped by a stop request: the launcher stopped the simulation and the participants\n");
    CHECK(format_run_text(stopped).ends_with("\nstopped: 0 errors, 0 warnings, 1 note\n"));
    CHECK(format_run_text(stopped).find("run: stopped by a stop request: the simulation was stopped\n") != std::string::npos);
    CHECK(format_run_json(stopped).find("  \"result\": \"stopped\",\n  \"end\": \"stopped\",\n  \"ok\": true,\n") != std::string::npos);
    CHECK(format_run_json(stopped).find("  \"simulation\": \"interrupted\",\n  \"interrupted_by\": \"stop command\",\n") != std::string::npos);
    const RunReport afterPass = report_of(with_scenario(stopped_run("control server stop", StopKind::Operator), ScenarioStatus::Passed));
    check_meaning(afterPass, {RunResult::Stopped, 0, true, "\"stopped\""}, "stopped after the scenario passed");
    check_text(afterPass.message(), "The run was stopped by a stop request without errors, after the scenario had passed");
    // An operator stopped it before the scenario's end: no verdict, 5; the finding is a note.
    const RunReport incomplete = report_of(with_scenario(stopped_run("stop command", StopKind::Operator), ScenarioStatus::Incomplete));
    check_meaning(incomplete, {RunResult::Incomplete, 5, false, "\"stopped\""}, "incomplete");
    check_text(incomplete.message(), "The run was stopped by a stop request before the scenario reached its end, so the scenario has no verdict");
    CHECK_EQ(incomplete.errors(), 0U);
    CHECK(lines(incomplete.findings).find("N 4 scenario-incomplete: the scenario did not get to its end") != std::string::npos);
    CHECK(lines(incomplete.findings).find("; the run was stopped by a stop request before the scenario's end\n") != std::string::npos);
    CHECK(format_run_text(incomplete).ends_with("\nincomplete: 0 errors, 0 warnings, 2 notes\n"));
    // ... or before the scenario began.
    RunOutcome early = with_scenario(stopped_run("stop command", StopKind::Operator), ScenarioStatus::NotRun);
    early.end.simulation = SimulationEnd::NotStarted;
    check_meaning(report_of(early), {RunResult::Incomplete, 5, false, "\"stopped\""}, "stopped before the start");
    // An operator stopped it, and something is wrong: failed.
    const RunReport failedCheck = report_of(with_scenario(stopped_run("stop command", StopKind::Operator), ScenarioStatus::Failed));
    check_meaning(failedCheck, {RunResult::Failed, 1, false, "\"stopped\""}, "stopped after a failed step");
    check_text(failedCheck.message(), "The run was stopped by a stop request and failed: expect Body.level == 1: it was 0");
    RunOutcome warned = stopped_run("stop command", StopKind::Operator);
    warned.participants[0].left_processes = true;
    check_meaning(report_of(warned), {RunResult::Stopped, 0, true, "\"stopped\""}, "stopped with a warning");
    check_meaning(report_of(warned, true), {RunResult::Failed, 1, false, "\"stopped\""}, "stopped with a warning, --strict");
    CHECK(report_of(warned, true).message().starts_with("The run was stopped by a stop request and failed: a warning counts as an error with --strict: "));
    // A stop while the participants were ending did not cut the scenario short: the finding stays an error.
    RunOutcome late = with_scenario(passed_run(), ScenarioStatus::Incomplete);
    late.end.simulation = SimulationEnd::Ended;
    late.end.interrupted_by = "stop command";
    late.end.interrupted_kind = StopKind::Operator;
    late.end.interrupted_while_ending = true;
    check_meaning(report_of(late), {RunResult::Failed, 1, false, "\"stopped\""}, "stopped while ending");
    CHECK(lines(report_of(late).findings).find("E 4 scenario-incomplete: ") != std::string::npos);
    // A signal, or the supervisor went away: interrupted, whatever else is wrong.
    for (const auto& [by, kind] : {std::pair{"SIGINT", StopKind::Signal}, std::pair{"end of input", StopKind::Supervisor},
                                   std::pair{"output closed", StopKind::Supervisor}})
    {
        check_meaning(report_of(stopped_run(by, kind)), {RunResult::Interrupted, 130, false, "\"interrupted\""}, by);
        check_meaning(report_of(with_scenario(stopped_run(by, kind), ScenarioStatus::Incomplete)),
                      {RunResult::Interrupted, 130, false, "\"interrupted\""}, by);
        check_meaning(report_of(with_scenario(stopped_run(by, kind), ScenarioStatus::Failed)),
                      {RunResult::Interrupted, 130, false, "\"interrupted\""}, by);
    }
    check_text(report_of(stopped_run("SIGINT", StopKind::Signal)).message(), "The run was interrupted by a signal");
    const RunReport cut = report_of(with_scenario(stopped_run("SIGINT", StopKind::Signal), ScenarioStatus::Incomplete));
    CHECK(lines(cut.findings).find("N 4 scenario-incomplete: ") != std::string::npos);
    CHECK(lines(cut.findings).find("; the run was interrupted (SIGINT) before the scenario's end\n") != std::string::npos);
    // It could not go on: failed, and the scenario's finding is an error as before.
    RunOutcome timedOut = with_scenario(passed_run(), ScenarioStatus::Incomplete);
    timedOut.end.simulation = SimulationEnd::Aborted;
    timedOut.end.timed_out = true;
    timedOut.end.timeout_ms = 500;
    const RunReport limit = report_of(timedOut);
    check_meaning(limit, {RunResult::Failed, 1, false, "\"failed\""}, "time limit");
    CHECK(limit.message().starts_with("The run failed: the simulation did not end within 500 ms"));
    CHECK(lines(limit.findings).find("E 4 scenario-incomplete: ") != std::string::npos);
    CHECK(lines(limit.findings).find("; the other findings and the lines above say why the simulation ended\n") != std::string::npos);
    // Completed, but an error was found.
    RunOutcome wrong = with_scenario(passed_run(), ScenarioStatus::Failed);
    check_meaning(report_of(wrong), {RunResult::Failed, 1, false, "\"completed\""}, "completed with an error");
    // The set-up failed.
    RunOutcome setup;
    setup.setup.error = "no registry";
    check_meaning(report_of(setup), {RunResult::SetupFailed, 3, false, "\"setup_failed\""}, "set-up");
    check_text(report_of(setup).message(), "The run failed before anything was started: cannot set up the simulation: no registry");
    // Nothing was started: no end.
    RunReport checkFailed = report_of(passed_run());
    checkFailed.run.reset();
    checkFailed.findings.clear();
    checkFailed.check.findings.push_back(Finding{Severity::Error, "command", "participant 'Engine': no such file", 5, "Engine", ""});
    check_meaning(checkFailed, {RunResult::CheckFailed, 1, false, "null"}, "check");
    check_text(checkFailed.message(), "Nothing was started, because the check failed: participant 'Engine': no such file");
    RunReport unreadable;
    unreadable.check.findings.push_back(Finding{Severity::Error, "file", "cannot read sim.yaml", 0, "", ""});
    check_meaning(unreadable, {RunResult::Unreadable, 3, false, "null"}, "unreadable");
    check_text(unreadable.message(), "Nothing was started, because the configuration cannot be read: cannot read sim.yaml");
    RunReport usage;
    usage.usage = true;
    usage.check.usage = true;
    usage.check.findings.push_back(Finding{Severity::Error, "usage", "unknown option --x", 0, "", ""});
    check_meaning(usage, {RunResult::Usage, 4, false, "null"}, "usage");
    check_text(usage.message(), "Nothing was started, because the command line is wrong: unknown option --x");
}

TEST_CASE(a_participant_ended_by_force_in_a_stopped_run_is_no_error_but_is_said)
{
    using vehicle_sil::launch::ended_by_force;
    using vehicle_sil::launch::StopKind;
    // The operator did not wait (a second stop request, or one while the launcher waited): Engine
    // was ended by force. The run is still a success, and the note and the message name Engine.
    RunOutcome o = stopped_run("stop command", StopKind::Operator);
    o.participants[0].ending = Ending::Killed;
    o.participants[0].exit.reset();
    CHECK(ended_by_force(o) == std::vector<std::string>{"Engine"});
    const RunReport forced = report_of(o);
    check_meaning(forced, {RunResult::Stopped, 0, true, "\"stopped\""}, "ended by force");
    check_text(forced.message(), "The run was stopped by a stop request; Engine was ended by force, without waiting");
    check_text(lines(forced.findings),
               "N 0 stopped: stopped by a stop request: the launcher stopped the simulation and the participants\n"
               "N 0 ended-by-force: the launcher did not wait for Engine to end, because it was asked to stop again (or while it "
               "waited for the participants to end): it ended it by force, with the processes started. This is no error of a run "
               "that was stopped that way; whether it ends when asked to is not known from this run (stop once and wait to see)\n");
    // Known once the participants have ended: not among the findings written before that.
    CHECK(lines(launch::detail::settled_findings(o)).find("ended-by-force") == std::string::npos);
    // Two of them, in a scenario that was cut short.
    RunOutcome both = with_scenario(o, ScenarioStatus::Incomplete);
    both.participants[1].ending = Ending::Killed;
    both.participants[1].exit.reset();
    check_text(report_of(both).message(), "The run was stopped by a stop request before the scenario reached its end, so the scenario "
                                          "has no verdict; Engine and Brake were ended by force, without waiting");
    CHECK(lines(report_of(both).findings).find("did not wait for Engine and Brake to end") != std::string::npos);
    check_meaning(report_of(both), {RunResult::Incomplete, 5, false, "\"stopped\""}, "two ended by force");
    // One that the launcher waited for in full is the error did-not-end, not this note.
    RunOutcome waited = o;
    waited.participants[0].stopped_after_end = true;
    CHECK(ended_by_force(waited).empty());
    check_meaning(report_of(waited), {RunResult::Failed, 1, false, "\"stopped\""}, "waited in full");
    CHECK(lines(report_of(waited).findings).find("E 5 did-not-end: ") != std::string::npos);
    // One that could not be asked to stop (no console) was not disobedient.
    RunOutcome unasked = o;
    unasked.participants[0].stop_request_failed = true;
    CHECK(ended_by_force(unasked).empty());
    check_text(report_of(unasked).message(), "The run was stopped by a stop request without errors");
    // Not for an interrupt: its result is no success anyway, and its output is as it was.
    RunOutcome signal = stopped_run("SIGINT", StopKind::Signal);
    signal.participants[0].ending = Ending::Killed;
    signal.participants[0].exit.reset();
    CHECK(ended_by_force(signal).empty());
    check_text(lines(report_of(signal).findings), "N 0 interrupted: interrupted (SIGINT): the launcher stopped the simulation and the participants\n");
}

TEST_CASE(what_asked_to_stop_is_said_in_words_by_its_kind)
{
    using vehicle_sil::launch::StopKind;
    check_text(report_of(stopped_run("stop command", StopKind::Operator)).message(), "The run was stopped by a stop request without errors");
    check_text(report_of(stopped_run("control server stop", StopKind::Operator)).message(), "The run was stopped by a stop request without errors");
    check_text(report_of(stopped_run("SIGTERM", StopKind::Signal)).message(), "The run was interrupted by a signal");
    check_text(report_of(stopped_run("Ctrl+C", StopKind::Signal)).message(), "The run was interrupted by a signal");
    check_text(report_of(stopped_run("end of input", StopKind::Supervisor)).message(), "The run was interrupted because its input closed");
    check_text(report_of(stopped_run("output closed", StopKind::Supervisor)).message(), "The run was interrupted because its output closed");
    // The name for programs stays in interrupted_by.
    CHECK(format_run_json(report_of(stopped_run("control server stop", StopKind::Operator))).find("  \"interrupted_by\": \"control server stop\",\n") !=
          std::string::npos);
    // Stopped, but the simulation did not shut down: an error, also of a run that an operator stopped.
    RunOutcome stuck = stopped_run("stop command", StopKind::Operator);
    stuck.end.controller_state = "Error";
    check_meaning(report_of(stuck), {RunResult::Failed, 1, false, "\"stopped\""}, "stopped, not shut down");
    CHECK(report_of(stuck).message().starts_with("The run was stopped by a stop request and failed: the simulation ended in state Error"));
}
