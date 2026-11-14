// SPDX-License-Identifier: MIT
//
// Judging a run of vehicle_sil_launch (assess_run(): what the run saw, made up here, gives which
// errors, warnings and notes), the result and the exit code, and the output for people and as
// JSON. No process is started.
#include "launch_test_util.hpp"

#include "vehicle_sil/launch/run.hpp"

#include "run_close.hpp"
#include "run_notes.hpp"
#include "run_start_limit.hpp"

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
    o.simulation = SimulationEnd::ReachedStopAt;
    o.stop_at_ms = 1000;
    o.start_timeout_ms = 30000;
    o.stop_timeout_ms = 10000;
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
    ended.simulation = SimulationEnd::Ended;
    CHECK(report_of(ended).result() == RunResult::Passed);
}

TEST_CASE(the_participant_that_failed_the_run_is_named_with_how_its_process_ended)
{
    RunOutcome o = passed_run();
    o.simulation = SimulationEnd::Aborted;
    o.failed_participant = "Engine";
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
    o.failure_reason = "left the simulation (state Error): it failed or lost its connection; see its log for the cause";
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
    o.simulation = SimulationEnd::Aborted;
    o.failed_participant = "Engine";
    o.failure_reason = "left the simulation (state Error): it failed or lost its connection; see its log for the cause";
    middles.push_back(running(o));
    o.failure_reason.clear();
    middles.push_back(running(o));
    o = passed_run();
    o.simulation = SimulationEnd::Aborted;
    o.timed_out = true;
    o.timeout_ms = 500;
    middles.push_back(running(o));
    o = passed_run();
    o.simulation = SimulationEnd::Interrupted;
    o.interrupted_by = "SIGINT";
    middles.push_back(running(o));
    o = passed_run();
    o.simulation = SimulationEnd::Ended;
    o.controller_state = "Error";
    middles.push_back(running(o));
    o = passed_run();
    o.simulation = SimulationEnd::NotStarted;
    o.start_timed_out = true;
    middles.push_back(running(o));
    middles.back().participants[1].missing_at_start = true;
    o.start_timed_out = false;
    o.failed_participant = "Engine";
    middles.push_back(running(o));
    middles.back().participants[0] = exited(participant("Engine", 5), 3);
    middles.back().participants[0].ending = Ending::ExitedBeforeStart;
    o.failed_participant.clear();
    o.start_error = "the controller said no";
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
                        if (lateInterrupt && end.interrupted_by.empty())
                        {
                            end.interrupted_by = "SIGINT";
                            end.interrupted_while_ending = true;
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
    o.simulation = SimulationEnd::NotStarted;
    o.start_timeout_ms = 2000;
    o.failed_participant = "Quitter";
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
    o.stop_timeout_ms = 300;
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
    o.simulation = SimulationEnd::NotStarted;
    o.start_error = "it did not start";
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
    o.connection_not_closed_ms = 8000;
    check_text(lines(assess_run(o)),
               "W 0 connection-not-closed: the launcher's own connection to the simulation (SIL Kit) had not closed 8000 "
               "ms after the participants had ended: the launcher ended without closing it. The result is complete. This "
               "is not expected of any configuration: it is a defect of the launcher or of SIL Kit. Report it with this "
               "result, the launcher's output and what the participants were doing (was one stopped in a debugger?)\n");
    // One more interrupt ended the wait: the launcher did as it was asked, and that is no finding.
    o.connection_close_interrupted = true;
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

TEST_CASE(a_control_agent_that_is_not_connected_is_told_apart_from_none)
{
    // Nothing to connect, connected, and variables that are not offered (the check notes that):
    // nothing for the run to say.
    RunOutcome o = passed_run();
    o.control_hello_timeout_ms = 5000;
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
        "this launcher version 1, so its variables cannot be read or written in this run: build the ECU and "
        "vehicle_sil_launch from the same version of vehicle-sil\n";
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
    mute.control_hello_timeout_ms = 5000;
    mute.participants[0].control = ControlLink::NoHello;
    mute.participants[0].control_hello_seen = true;
    check_text(lines(assess_run(mute)),
               "W 5 control-link-no-hello: participant 'Engine': it offers its variables (--describe), but its control agent "
               "said hello and then did not answer that it receives the launcher's commands within 5000 ms before the "
               "simulation started, so its variables cannot be read or written in this run. Its transport does not answer "
               "the launcher's probe: build the ECU and vehicle_sil_launch from the same version of vehicle-sil; the lines "
               "[Engine] above show what it wrote\n");

    // What the launcher's end dropped is said once, with the numbers.
    RunOutcome dropped = passed_run();
    dropped.control_refused_hellos = 2;
    dropped.control_unknown = 40;
    const std::string said = lines(assess_run(dropped));
    CHECK(said.rfind("W 0 control-link-dropped: the launcher dropped messages of control agents: 2 hellos it did not take, 0 "
                     "messages it could not read and 40 messages (or samples) that fit no agent it knows. Hellos are not taken "
                     "beyond 256 participants with control agents",
                     0) == 0);
    CHECK(lines(launch::detail::settled_findings(dropped)) == said);
    dropped.control_refused_hellos = 0;
    dropped.control_unknown = 0;
    dropped.control_undecodable = 1;
    CHECK(lines(assess_run(dropped)).find("0 hellos it did not take, 1 messages it could not read and 0 messages") != std::string::npos);

    // What was to be done before the start failed: the simulation had started and was aborted.
    RunOutcome failed = passed_run();
    failed.simulation = SimulationEnd::Aborted;
    failed.before_start_error = "no answer from Engine";
    check_text(lines(assess_run(failed)),
               "E 0 before-start: what was to be done before the simulation started failed, and the launcher aborted the "
               "simulation at once: no answer from Engine\n");
    CHECK(report_of(failed).result() == RunResult::Failed);
}

TEST_CASE(the_simulation_findings_setup_state_abort_and_interrupt)
{
    RunOutcome o = passed_run();
    o.simulation = SimulationEnd::Ended;
    o.controller_state = "Aborting";
    check_text(lines(assess_run(o)), "E 0 simulation: the simulation ended in state Aborting instead of shutting down "
                                     "normally: the lines above show what the participants wrote\n");
    CHECK(report_of(o).result() == RunResult::Failed);
    o.controller_state.clear();
    o.simulation = SimulationEnd::Aborted;
    check_text(lines(assess_run(o)), "E 0 simulation: the simulation was aborted: the lines above show what the "
                                     "participants wrote\n");
    CHECK(report_of(o).result() == RunResult::Failed);
    o.simulation = SimulationEnd::Interrupted;
    o.interrupted_by = "SIGINT";
    const RunReport interrupted = report_of(o);
    check_text(lines(interrupted.findings),
               "N 0 interrupted: interrupted (SIGINT): the launcher stopped the simulation and the participants\n");
    CHECK(interrupted.result() == RunResult::Interrupted);
    CHECK_EQ(interrupted.exit_code(), kExitInterrupted);
    CHECK_EQ(kExitInterrupted, 130);
    o.simulation = SimulationEnd::NotStarted;
    check_text(lines(assess_run(o)),
               "N 0 interrupted: interrupted (SIGINT): the launcher stopped the participants before the simulation "
               "started\n");
    RunOutcome setup;
    setup.setup_error = "the registry cannot listen at silkit://127.0.0.1:8500: in use";
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
    o.simulation = SimulationEnd::Aborted;
    o.failed_participant = "Engine";
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
    RunOutcome s = passed_run();
    check_text(state_line(s), "run: the simulation ran to stop_at_ms (1000 ms) and ended");
    s.simulation = SimulationEnd::Ended;
    check_text(state_line(s), "run: the simulation ran until a participant ended it");
    s.controller_state = "Aborting";
    check_text(state_line(s), "run: the simulation ended in state Aborting");
    s.controller_state.clear();
    s.simulation = SimulationEnd::Interrupted;
    s.interrupted_by = "Ctrl+C";
    check_text(state_line(s), "run: interrupted (Ctrl+C): the simulation was stopped");
    s.simulation = SimulationEnd::NotStarted;
    check_text(state_line(s), "run: interrupted (Ctrl+C) before the simulation started");
    s.interrupted_by.clear();
    check_text(state_line(s), "run: the simulation did not start");
    s.setup_error = "x";
    check_text(state_line(s), "run: the simulation could not be set up");
    // The last line.
    const std::string interrupted = format_run_text(report_of([] {
        RunOutcome i = passed_run();
        i.simulation = SimulationEnd::Interrupted;
        i.interrupted_by = "SIGINT";
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
    o.simulation = SimulationEnd::Interrupted;
    o.interrupted_by = "SIGINT";
    ParticipantRun killed = participant("Brake", 7);
    killed.ending = Ending::Killed;
    killed.left_processes = true;
    o.participants[1] = killed;
    const std::string json = format_run_json(report_of(o));
    const std::string v(version());
    check_text(json.substr(0, json.find("  \"check\": {")),
               "{\n"
               "  \"vehicle_sil_run\": 1,\n"
               "  \"vehicle_sil\": \"" + v + "\",\n"
               "  \"file\": \"sim.yaml\",\n"
               "  \"result\": \"interrupted\",\n"
               "  \"strict\": false,\n"
               "  \"errors\": 0,\n"
               "  \"warnings\": 1,\n"
               "  \"notes\": 1,\n"
               "  \"simulation\": \"interrupted\",\n"
               "  \"interrupted_by\": \"SIGINT\",\n"
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
               "      \"enabled\": true,\n      \"manual\": false,\n"
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
               "      \"enabled\": true,\n      \"manual\": false,\n"
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
    CHECK(empty.find("  \"simulation\": null,\n  \"interrupted_by\": null,\n  \"timing\": null,\n  \"participants\": [],\n"
                     "  \"findings\": [],\n") != std::string::npos);
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
    notStarted.simulation = SimulationEnd::NotStarted;
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
    using launch::detail::noted_paused;
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
    CHECK(!noted_paused(state(zero + 100ms, true, std::nullopt), TimeNote::Start));
    CHECK(noted_paused(state(zero + 100ms, true, std::nullopt), TimeNote::Poll));
    CHECK(!noted_paused(state(zero + 100ms, false, zero + 100ms), TimeNote::Start));
    CHECK(!noted_paused(state(zero + 10ms, false, zero), TimeNote::Start));
    // Standing still at 0 (--start-paused), or about to (the pause at 0 that the gate has not
    // taken yet).
    CHECK(noted_paused(state(zero, true, std::nullopt), TimeNote::Start));
    CHECK(noted_paused(state(zero, false, zero), TimeNote::Start));
    // On its way to a later time, or just running.
    CHECK(!noted_paused(state(zero, false, zero + 100ms), TimeNote::Start));
    CHECK(!noted_paused(state(zero, false, std::nullopt), TimeNote::Start));
    CHECK(!noted_paused(state(zero + 50ms, false, zero + 100ms), TimeNote::Poll));
    // At the target, standing still with the next step; not once the simulation is over.
    CHECK(noted_paused(state(zero + 100ms, false, zero + 100ms), TimeNote::Poll));
    CHECK(!noted_paused(state(zero + 100ms, false, zero + 100ms), TimeNote::Last));
    // An ended simulation stands still nowhere, whatever its gate says (one aborted while it stood
    // still: the gate keeps paused until the abort has gone round, at 0 or later).
    CHECK(!noted_paused(state(zero + 100ms, true, std::nullopt), TimeNote::Last));
    CHECK(!noted_paused(state(zero, true, std::nullopt), TimeNote::Last));
    CHECK(!noted_paused(state(zero, true, zero), TimeNote::Last));
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
    o.simulation = SimulationEnd::NotStarted;
    check_text(lines(assess_run(o)), "E 9 ended-before-start: participant 'Odd': it ended before the simulation started: "
                                     "its process ended, so the simulation could not start; the lines [Odd] above show "
                                     "what it wrote\n");
}

TEST_CASE(a_run_that_ends_before_stop_at_ms_gets_a_note_and_a_timeout_is_an_error)
{
    RunOutcome o = passed_run();
    o.simulation = SimulationEnd::Ended;
    const RunReport early = report_of(o, true);
    check_text(lines(early.findings), "N 0 ended-before-stop-at: the simulation ended before stop_at_ms (1000 ms): a "
                                      "participant stopped it; if that is not expected, the lines above show which one\n");
    CHECK(early.result() == RunResult::Passed); // a note, also with --strict
    o.stop_at_ms.reset();
    check_text(lines(assess_run(o)), ""); // nothing to end before
    RunOutcome t = passed_run();
    t.simulation = SimulationEnd::Aborted;
    t.timed_out = true;
    t.timeout_ms = 5000;
    const RunReport timed = report_of(t);
    check_text(lines(timed.findings), "E 0 timeout: the simulation did not end within 5000 ms of wall-clock time "
                                      "(--timeout-ms), so the launcher aborted it: a participant may hang in a step (the "
                                      "lines above show what the participants wrote); if it is only slow, raise "
                                      "--timeout-ms\n");
    CHECK(timed.result() == RunResult::Failed);
    CHECK(format_run_text(timed).find("\nrun: the simulation was aborted: it did not end within --timeout-ms (5000 ms)\n") !=
          std::string::npos);
    // It stood still before its first step for a while: --timeout-ms counted that too.
    t.held_before_start_ms = 4200;
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
    o.interrupted_by = "SIGINT";
    o.interrupted_while_ending = true;
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
    CHECK_EQ(launch::detail::ecu_start_timeout_ms(c), std::int64_t{20000});
    CHECK(launch::detail::ecu_start_timeout_ms(c) > c.start_timeout_ms + c.stop_timeout_ms);
    // Never more than ecu_main takes as --start-timeout-ms.
    c.start_timeout_ms = kMaxMillis;
    c.stop_timeout_ms = kMaxMillis;
    CHECK_EQ(launch::detail::ecu_start_timeout_ms(c), kMaxMillis);
    c.start_timeout_ms = kMaxMillis - 10000;
    c.stop_timeout_ms = 1;
    CHECK_EQ(launch::detail::ecu_start_timeout_ms(c), kMaxMillis);
}

TEST_CASE(the_command_to_start_a_participant_by_hand_is_quoted_for_the_shell)
{
    using vehicle_sil::launch::detail::shell_command_line;
    const std::vector<std::string> args{"--name", "Engine", "--registry-uri", "silkit://127.0.0.1:8500", "", "a b",
                                        "it's", "say \"hi\"", "C:\\dir\\", "%PATH%", "R&D"};
#ifdef _WIN32
    // As CommandLineToArgvW reads it back: backslashes double only before a quote.
    check_text(shell_command_line("C:\\My ECUs\\engine.exe", args) + "\n",
               "\"C:\\My ECUs\\engine.exe\" --name Engine --registry-uri silkit://127.0.0.1:8500 \"\" \"a b\" it's "
               "\"say \\\"hi\\\"\" C:\\dir\\ %PATH% \"R&D\"\n");
#else
    check_text(shell_command_line("/opt/my ecus/engine", args) + "\n",
               "'/opt/my ecus/engine' --name Engine --registry-uri silkit://127.0.0.1:8500 '' 'a b' 'it'\\''s' "
               "'say \"hi\"' 'C:\\dir\\' %PATH% 'R&D'\n");
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
    CHECK(run.rfind("{\n  \"vehicle_sil_run\": 1,\n", 0) == 0);
    CHECK(run.find("\"code\": \"internal\"") != std::string::npos && run.find("\"message\": \"boom\"") != std::string::npos);
    CHECK(run.find("  \"errors\": 1,\n") != std::string::npos);
    const std::string check = format_internal_error_json(false, "boom");
    CHECK(check.rfind("{\n  \"vehicle_sil_check\": 1,\n", 0) == 0);
    CHECK(check.find("\"code\": \"internal\"") != std::string::npos);
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
    CHECK(json.find("      \"name\": \"Brake\",\n      \"required\": true,\n      \"enabled\": false,\n      \"manual\": false,\n      \"ended\": "
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
