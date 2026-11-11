// SPDX-License-Identifier: MIT
//
// Judging a run of vehicle_sil_launch (assess_run(): what the run saw, made up here, gives which
// errors, warnings and notes), the result and the exit code, and the output for people and as
// JSON. No process is started.
#include "launch_test_util.hpp"

#include "vehicle_sil/launch/run.hpp"

#include <cstdint>
#include <string>
#include <vector>

#ifndef _WIN32
#include <csignal>
#endif

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
               "aborted the simulation: its process exited with code 7; the lines [Engine] above show what it wrote\n"
               "warning: sim.yaml:13: participant 'Tool': cannot start t: check the command in the configuration file\n"
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
                                   "error: sim.yaml:3: unknown key 'x'\n"
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
                                   "warning: sim.yaml:3: unknown key 'x'\n"
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
               "  \"participants\": [\n"
               "    {\n"
               "      \"name\": \"Engine\",\n"
               "      \"required\": true,\n"
               "      \"enabled\": true,\n"
               "      \"ended\": \"exited\",\n"
               "      \"exit_code\": 0,\n"
               "      \"signal\": null,\n"
               "      \"left_processes\": false,\n"
               "      \"log\": null\n"
               "    },\n"
               "    {\n"
               "      \"name\": \"Brake\",\n"
               "      \"required\": true,\n"
               "      \"enabled\": true,\n"
               "      \"ended\": \"killed\",\n"
               "      \"exit_code\": null,\n"
               "      \"signal\": null,\n"
               "      \"left_processes\": true,\n"
               "      \"log\": null\n"
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
    CHECK(empty.find("  \"simulation\": null,\n  \"interrupted_by\": null,\n  \"participants\": [],\n  \"findings\": [],\n") !=
          std::string::npos);
}

TEST_CASE(the_participants_reach_a_registry_on_every_address_at_the_loopback_address)
{
    CHECK(launch::detail::uri_for_participants("silkit://0.0.0.0:8500") == "silkit://127.0.0.1:8500");
    CHECK(launch::detail::uri_for_participants("silkit://[::]:8500") == "silkit://[::1]:8500");
    CHECK(launch::detail::uri_for_participants("silkit://192.168.1.2:8500") == "silkit://192.168.1.2:8500");
    CHECK(launch::detail::uri_for_participants("silkit://127.0.0.1:41234") == "silkit://127.0.0.1:41234");
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
    CHECK(json.find("      \"left_processes\": false,\n      \"log\": \"logs/Engine.log\"\n") != std::string::npos);
    CHECK(json.find("      \"left_processes\": false,\n      \"log\": null\n") != std::string::npos);
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
    CHECK(json.find("      \"name\": \"Brake\",\n      \"required\": true,\n      \"enabled\": false,\n      \"ended\": "
                    "\"not_started\",\n      \"exit_code\": null,\n      \"signal\": null,\n      \"left_processes\": false,\n"
                    "      \"log\": null\n") != std::string::npos);
}
