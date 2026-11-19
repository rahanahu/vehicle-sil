// SPDX-License-Identifier: MIT
//
// Judging a run of vehicle_sil_launch (assess_run(): what the run saw, made up here, gives which
// errors, warnings and notes) as the launcher reports it: the result and the exit code, and the
// output for people and as JSON. No process is started. The judgement itself is in
// tests/runner/test_run.cpp.

#include "run_support.hpp"
#include "launch_test_util.hpp"

#include "vehicle_sil/launch/error_result.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/runner/detail/run_detail.hpp"

#include "vehicle_sil/config/detail/check_words.hpp"
#include "vehicle_sil/runner/detail/run_close.hpp"
#include "run_connection.hpp"
#include "run_notes.hpp"
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
using namespace vehicle_sil::runner;
using namespace vehicle_sil::scenario;
using namespace vehicle_sil::config;
using vehicle_sil::config::test::check_text;
using vehicle_sil::config::test::lines;
using vehicle_sil::process::ExitStatus;
using namespace vehicle_sil::runner::test::run_support;

namespace {

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
    check_text(lines(vehicle_sil::runner::detail::settled_findings(o)), expected);
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
    CHECK(lines(vehicle_sil::runner::detail::settled_findings(dropped)) == said);
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
    // check --compare-commands is the check's alone: a run's check compares nothing.
    CHECK(json.find("    \"commands\": null,\n") != std::string::npos);
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
    CHECK(vehicle_sil::runner::detail::settled_findings(late_run()) == r.findings);
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
    CHECK(vehicle_sil::runner::detail::settled_findings(o) == assess_run(o));
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

TEST_CASE(log_names_avoid_windows_devices_and_the_json_names_the_log)
{
    CHECK(vehicle_sil::runner::detail::log_name("NUL") == "NUL_");
    CHECK(vehicle_sil::runner::detail::log_name("con.x") == "con_.x");
    CHECK(vehicle_sil::runner::detail::log_name("Com7") == "Com7_");
    CHECK(vehicle_sil::runner::detail::log_name("LPT9") == "LPT9_");
    CHECK(vehicle_sil::runner::detail::log_name("COM10") == "COM10");
    CHECK(vehicle_sil::runner::detail::log_name("Console") == "Console");
    RunOutcome o = passed_run();
    o.participants[0].log = "logs/Engine.log";
    const std::string json = format_run_json(report_of(o));
    CHECK(json.find("      \"left_processes\": false,\n      \"log\": \"logs/Engine.log\",\n") != std::string::npos);
    CHECK(json.find("      \"left_processes\": false,\n      \"log\": null,\n") != std::string::npos);
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
    CHECK(vehicle_sil::runner::detail::settled_findings(o).empty());
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
        CHECK(vehicle_sil::runner::detail::settled_findings(o).empty());
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
    CHECK(vehicle_sil::runner::detail::settled_findings(o).size() == 1);
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
    CHECK(vehicle_sil::runner::detail::settled_findings(o).size() == 1);
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
    CHECK(vehicle_sil::runner::detail::settled_findings(o).empty());
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
    CHECK(vehicle_sil::runner::detail::settled_findings(o).empty());
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
    CHECK(vehicle_sil::runner::detail::settled_findings(o).empty());
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
    CHECK(vehicle_sil::runner::detail::settled_findings(o).empty());
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

/// `o` with a scenario of one step in `status` (made in place: no struct is built and moved).
RunOutcome with_scenario(RunOutcome o, ScenarioStatus status)
{
    vehicle_sil::scenario::ScenarioRun& scenario = o.scenario.result.emplace();
    scenario.file = "s.yaml";
    scenario.status = status;
    vehicle_sil::scenario::ScenarioStepRun& step = scenario.steps.emplace_back();
    step.index = 1;
    step.line = 4;
    step.text = "expect Body.level == 1";
    step.status = status == ScenarioStatus::Passed       ? vehicle_sil::scenario::StepStatus::Passed
                  : status == ScenarioStatus::Failed     ? vehicle_sil::scenario::StepStatus::Failed
                  : status == ScenarioStatus::Incomplete ? vehicle_sil::scenario::StepStatus::Unfinished
                                                         : vehicle_sil::scenario::StepStatus::NotRun;
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

TEST_CASE(each_result_has_its_exit_code_its_ok_and_a_message)
{
    using vehicle_sil::runner::StopKind;
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
    check_text(failedCheck.message(), "The run failed (it was stopped by a stop request): expect Body.level == 1: it was 0");
    RunOutcome warned = stopped_run("stop command", StopKind::Operator);
    warned.participants[0].left_processes = true;
    check_meaning(report_of(warned), {RunResult::Stopped, 0, true, "\"stopped\""}, "stopped with a warning");
    check_meaning(report_of(warned, true), {RunResult::Failed, 1, false, "\"stopped\""}, "stopped with a warning, --strict");
    CHECK(report_of(warned, true).message().starts_with("The run failed (it was stopped by a stop request): a warning counts as an error with --strict: "));
    // A stop while the participants were ending did not end the simulation: the run ended as the
    // simulation did, and the stop is a note. It did not cut the scenario short: the finding stays an error.
    RunOutcome late = with_scenario(passed_run(), ScenarioStatus::Incomplete);
    late.end.simulation = SimulationEnd::Ended;
    late.end.interrupted_by = "stop command";
    late.end.interrupted_kind = StopKind::Operator;
    late.end.interrupted_while_ending = true;
    check_meaning(report_of(late), {RunResult::Failed, 1, false, "\"completed\""}, "stopped while ending");
    CHECK(lines(report_of(late).findings).find("E 4 scenario-incomplete: ") != std::string::npos);
    CHECK(report_of(late).message().starts_with("The run failed: the scenario did not get to its end"));
    // ... after the simulation had reached its end without errors: passed, not stopped.
    RunOutcome lateAfterPass = with_scenario(passed_run(), ScenarioStatus::Passed);
    lateAfterPass.end.interrupted_by = "stop command";
    lateAfterPass.end.interrupted_kind = StopKind::Operator;
    lateAfterPass.end.interrupted_while_ending = true;
    check_meaning(report_of(lateAfterPass), {RunResult::Passed, 0, true, "\"completed\""}, "stopped while ending, after a pass");
    check_text(report_of(lateAfterPass).message(), "The run reached its end without errors, and the scenario passed");
    check_text(lines(report_of(lateAfterPass).findings),
               "N 0 stopped: stopped by a stop request: the launcher stopped the participants instead of waiting for them to end\n");
    CHECK(format_run_json(report_of(lateAfterPass)).find("  \"simulation\": \"reached_stop_at\",\n  \"interrupted_by\": \"stop command\",\n") !=
          std::string::npos);
    // ... after a participant had failed it: failed, and the message does not say that the stop failed it.
    RunOutcome lateAfterAbort = lateAfterPass;
    lateAfterAbort.scenario = passed_run().scenario;
    lateAfterAbort.end.simulation = SimulationEnd::Aborted;
    lateAfterAbort.end.failed_participant = "Engine";
    check_meaning(report_of(lateAfterAbort), {RunResult::Failed, 1, false, "\"failed\""}, "stopped while ending, after an abort");
    CHECK(report_of(lateAfterAbort).message().starts_with("The run failed: participant 'Engine': it failed while the simulation was running"));
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

TEST_CASE(a_participant_ended_by_force_after_a_stop_request_is_no_error_but_is_said)
{
    using vehicle_sil::runner::ended_by_force;
    using vehicle_sil::runner::StopKind;
    // The operator did not wait (a second stop request, or one while the launcher waited): Engine
    // was ended by force. The run is still a success, and the note and the message name Engine.
    RunOutcome o = stopped_run("stop command", StopKind::Operator);
    o.participants[0].ending = Ending::Killed;
    o.participants[0].exit.reset();
    o.participants[0].end_cut_short = true;
    CHECK(ended_by_force(o) == std::vector<std::string>{"Engine"});
    const RunReport forced = report_of(o);
    check_meaning(forced, {RunResult::Stopped, 0, true, "\"stopped\""}, "ended by force");
    check_text(forced.message(), "The run was stopped by a stop request; Engine was ended by force, without waiting");
    check_text(lines(forced.findings),
               "N 0 stopped: stopped by a stop request: the launcher stopped the simulation and the participants\n"
               "N 0 ended-by-force: the launcher did not wait for Engine to end, because it was asked to stop again (or while it "
               "waited for the participants to end): it ended it by force, with the processes started. This is no error: whoever "
               "asked chose not to wait; whether it ends when asked to is not known from this run (stop once and wait to see)\n");
    // Known once the participants have ended: not among the findings written before that.
    CHECK(lines(vehicle_sil::runner::detail::settled_findings(o)).find("ended-by-force") == std::string::npos);
    // Two of them, in a scenario that was cut short.
    RunOutcome both = with_scenario(o, ScenarioStatus::Incomplete);
    both.participants[1].ending = Ending::Killed;
    both.participants[1].exit.reset();
    both.participants[1].end_cut_short = true;
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
    // Whatever asked: an interrupt's result is no success, but who was ended by force is said all the same.
    RunOutcome signal = stopped_run("SIGINT", StopKind::Signal);
    signal.participants[0].ending = Ending::Killed;
    signal.participants[0].exit.reset();
    signal.participants[0].end_cut_short = true;
    CHECK(ended_by_force(signal) == std::vector<std::string>{"Engine"});
    check_meaning(report_of(signal), {RunResult::Interrupted, 130, false, "\"interrupted\""}, "interrupted, ended by force");
    check_text(report_of(signal).message(), "The run was interrupted by a signal; Engine was ended by force, without waiting");
    CHECK(lines(report_of(signal).findings).starts_with("N 0 interrupted: interrupted (SIGINT): the launcher stopped the simulation and the participants\n"
                                                        "N 0 ended-by-force: the launcher did not wait for Engine to end"));
    // A stop that came only while the participants were ending, after the simulation had reached
    // its end, and cut short the end of a required participant: it decided how that one ended, so
    // the run was stopped (not passed: whether Engine ends by itself is not known).
    RunOutcome late = o;
    late.end.simulation = SimulationEnd::ReachedStopAt;
    late.end.interrupted_while_ending = true;
    check_meaning(report_of(late), {RunResult::Stopped, 0, true, "\"stopped\""}, "a required one ended by force after the end");
    check_text(report_of(late).message(), "The run was stopped by a stop request; Engine was ended by force, without waiting");
    RunOutcome lateAsked = late;
    lateAsked.participants[0].ending = Ending::Stopped;
    lateAsked.participants[0].exit = ExitStatus{9, std::nullopt};
    check_meaning(report_of(lateAsked), {RunResult::Stopped, 0, true, "\"stopped\""}, "a required one asked to stop after the end");
    // ... that cut short only the end of an optional one (every required one had ended by itself):
    // the run passed, and the message says who was ended by force.
    RunOutcome lateOptional = late;
    lateOptional.participants[0].required = false;
    check_meaning(report_of(lateOptional), {RunResult::Passed, 0, true, "\"completed\""}, "an optional one ended by force after the end");
    check_text(report_of(lateOptional).message(), "The run reached its end; Engine was ended by force, without waiting");
    check_text(report_of(with_scenario(lateOptional, ScenarioStatus::Passed)).message(),
               "The run reached its end, and the scenario passed; Engine was ended by force, without waiting");
    // One killed after the full waits (an interrupt before the start, nothing asked again): no wait
    // was cut short, so it is not named "without waiting".
    RunOutcome early = stopped_run("SIGINT", StopKind::Signal);
    early.end.simulation = SimulationEnd::NotStarted;
    early.participants[0].ending = Ending::Killed;
    early.participants[0].exit.reset();
    CHECK(ended_by_force(early).empty());
    check_text(report_of(early).message(), "The run was interrupted by a signal");
    // Nothing asked to stop: nobody is named (a kill of the launcher's own waits is did-not-end).
    RunOutcome unstopped = passed_run();
    unstopped.participants[0].ending = Ending::Killed;
    unstopped.participants[0].exit.reset();
    CHECK(ended_by_force(unstopped).empty());
}

TEST_CASE(what_asked_to_stop_is_said_in_words_by_its_kind)
{
    using vehicle_sil::runner::StopKind;
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
    CHECK(report_of(stuck).message().starts_with("The run failed (it was stopped by a stop request): the simulation ended in state Error"));
}
