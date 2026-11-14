// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch's run as a library (run_file()) with a hub of the caller and ECUs in their
// own processes (run_test_ecu.cpp, behaviour "variables"): a parameter written in the window
// before the start is accepted by every ECU and seen by its step at 0; when the simulation has
// reached stop_at_ms, the hub has the values of the last steps before it; nothing arrived late.
//
// usage: vehicle_sil_launch_control_test <test ECU executable> <scratch directory> <process helper> <part>
#include "test_timeout_scale.hpp"
#include "topics.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control/wire.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/launch/events.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <silkit/SilKit.hpp>
#include <silkit/services/orchestration/all.hpp>
#include <silkit/services/pubsub/all.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::test::scaled_timeout;
using namespace vehicle_sil;
namespace fs = std::filesystem;

void require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

std::string yaml_quote(const std::string& text)
{
    std::string out = "'";
    for (const char c : text)
    {
        out += c == '\'' ? std::string("''") : std::string(1, c);
    }
    return out + "'";
}

/// The value of `name` in the hub's latest sample of `peer`.
control::Value value_of(const control::Hub& hub, const std::string& peer, const std::string& name)
{
    const auto snapshot = hub.latest(peer);
    require(snapshot != nullptr, "the hub has no sample of " + peer);
    for (const auto& info : hub.peers())
    {
        if (info.participant != peer)
        {
            continue;
        }
        for (std::size_t i = 0; i < info.schema.size() && i < snapshot->values.size(); ++i)
        {
            if (info.schema[i].name == name)
            {
                return snapshot->values[i];
            }
        }
    }
    throw std::runtime_error(peer + " has no variable " + name);
}

std::string status_text(const control::OperationResult& operation)
{
    std::string out;
    for (const auto& t : operation.targets)
    {
        out += " " + t.peer + "=" + std::to_string(static_cast<int>(t.status)) + (t.reason.empty() ? "" : "(" + t.reason + ")");
    }
    return out;
}

void one_run(const fs::path& ecu, const fs::path& dir, int round)
{
    const std::vector<std::string> names{"Engine", "Brake", "Body"};
    std::string config = "vehicle_sil_simulation: 1\nstop_at_ms: 500\nnetworks: []\nparticipants:\n";
    for (const auto& name : names)
    {
        config += "  - name: " + name + "\n    command: [" + yaml_quote(process::shown_path(ecu)) + "]\n";
        std::ofstream(dir / (name + ".behaviour")) << "variables\n";
    }
    std::ofstream(dir / "sim.yaml", std::ios::binary) << config;

    control::Hub hub;
    std::uint64_t seq = 0;
    std::optional<control::OperationResult> queued;
    std::size_t peers_in_window = 0;
    launch::RunOptions options;
    options.hub = &hub;
    options.before_start = [&](control::Hub& h) {
        peers_in_window = h.peers().size();
        control::OperationRequest request;
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            request.writes.push_back({names[i], "gain", control::Value{std::int64_t{1000} * round + static_cast<std::int64_t>(i)}});
        }
        seq = h.write(std::move(request));
        queued = h.wait_queued(seq, scaled_timeout(30s));
    };
    std::string lines;
    options.write_line = [&lines](std::string_view line) { lines += std::string(line) + "\n"; };
    const launch::RunReport report = launch::run_file(dir / "sim.yaml", "sim.yaml", options);
    const std::string shown = "\n--- result ---\n" + launch::format_run_text(report) + "--- lines ---\n" + lines;

    require(report.result() == launch::RunResult::Passed, "the run did not pass" + shown);
    require(report.findings.empty(), "the run has findings" + shown);
    require(report.run.has_value() && report.run->simulation == launch::SimulationEnd::ReachedStopAt, "the run did not reach stop_at_ms" + shown);
    for (const auto& p : report.run->participants)
    {
        require(p.control == launch::ControlLink::Connected, p.name + " is not connected" + shown);
        require(p.control_variables == 8, p.name + " offers " + std::to_string(p.control_variables) + " variables, not 8");
        require(p.control_late_messages == 0, p.name + ": messages arrived late");
    }
    // In the window: every agent had said hello, and each accepted the parameter.
    require(peers_in_window == names.size(), "the window saw " + std::to_string(peers_in_window) + " peers");
    require(queued.has_value(), "the agents did not answer in the window");
    for (const auto& t : queued->targets)
    {
        require(t.status == control::TargetStatus::Queued, "not queued in the window:" + status_text(*queued));
    }
    // Applied in each ECU's step at 0, and seen there by the application.
    const auto result = hub.operation(seq);
    require(result.has_value() && result->done(), "the write is not done");
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        const auto& t = result->targets.at(i);
        require(t.status == control::TargetStatus::Applied && t.applied_at == SimTime{}, "not applied at 0:" + status_text(*result));
        const auto expected = control::Value{std::int64_t{1000} * round + static_cast<std::int64_t>(i)};
        require(value_of(hub, names[i], "gain_at_0") == expected, names[i] + " did not see the parameter in its step at 0");
        // The values of the last step before stop_at_ms (the ECUs step and sample every 10 ms)
        // were there when the simulation ended: nothing is lost at the end.
        const auto snapshot = hub.latest(names[i]);
        require(snapshot->time >= SimTime{} + 490ms, names[i] + ": the last sample is of " +
                                                         std::to_string(snapshot->time.time_since_epoch().count()) + " ns");
        require(std::get<std::int64_t>(value_of(hub, names[i], "ticks")) >= 50, names[i] + ": too few steps in the last sample");
    }
    // The agents have ended with their processes.
    for (const auto& peer : hub.peers())
    {
        require(!peer.connected, peer.participant + " is still connected after the run");
    }
}

/// What is done before the start fails: the simulation has started all the same (nobody is left
/// waiting), and the launcher aborts it at once and says so, instead of "it did not start".
void failing_start(const fs::path& ecu, const fs::path& dir)
{
    std::ofstream(dir / "sim.yaml", std::ios::binary)
        << "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n  - name: Engine\n    command: [" +
               yaml_quote(process::shown_path(ecu)) + "]\n";
    std::ofstream(dir / "Engine.behaviour") << "variables\n";
    control::Hub hub;
    launch::RunOptions options;
    options.hub = &hub;
    options.before_start = [](control::Hub&) { throw std::runtime_error("the scenario could not be set up"); };
    std::string lines;
    options.write_line = [&lines](std::string_view line) { lines += std::string(line) + "\n"; };
    const auto begin = std::chrono::steady_clock::now();
    const launch::RunReport report = launch::run_file(dir / "sim.yaml", "sim.yaml", options);
    const std::string shown = "\n--- result ---\n" + launch::format_run_text(report) + "--- lines ---\n" + lines;
    // Without stop_at_ms the simulation would run for ever: it was ended for the failure.
    require(std::chrono::steady_clock::now() - begin < 60s, "the run took too long" + shown);
    require(report.result() == launch::RunResult::Failed, "the run did not fail" + shown);
    require(report.run.has_value() && report.run->simulation == launch::SimulationEnd::Aborted, "the simulation was not aborted" + shown);
    require(report.findings.size() == 1 && report.findings[0].code == "before-start" &&
                report.findings[0].message.find("the scenario could not be set up") != std::string::npos,
            "the finding says what failed" + shown);
    require(lines.find("the simulation started, but what was to be done before it failed") != std::string::npos,
            "the launcher says that the simulation had started" + shown);
    require(report.run->participants.at(0).control == launch::ControlLink::Connected, "Engine was connected" + shown);
    require(report.run->participants.at(0).exit.has_value(), "Engine's process was ended and waited for" + shown);
}

/// The same, with a stop request that comes while what is to be done before the start runs: it
/// came during the run, so it does not cut short the wait for the participants to end.
void failing_start_with_a_stop(const fs::path& ecu, const fs::path& dir)
{
    std::ofstream(dir / "sim.yaml", std::ios::binary)
        << "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n  - name: Engine\n    command: [" +
               yaml_quote(process::shown_path(ecu)) + "]\n";
    std::ofstream(dir / "Engine.behaviour") << "variables\n";
    control::Hub hub;
    std::atomic<int> interrupts{0};
    launch::RunOptions options;
    options.hub = &hub;
    options.interrupts = &interrupts;
    options.interrupt_name = [] { return std::string("control server stop"); };
    options.before_start = [&interrupts](control::Hub&) {
        interrupts = 1;
        throw std::runtime_error("the scenario could not be set up");
    };
    std::string lines;
    options.write_line = [&lines](std::string_view line) { lines += std::string(line) + "\n"; };
    const launch::RunReport report = launch::run_file(dir / "sim.yaml", "sim.yaml", options);
    const std::string shown = "\n--- result ---\n" + launch::format_run_text(report) + "--- lines ---\n" + lines;
    require(report.run.has_value() && !report.run->interrupted_while_ending &&
                lines.find("no more waiting for the participants to end") == std::string::npos,
            "the stop before the end of the run did not cut short the wait for the participants" + shown);
}

/// An ECU that offers its variables (--describe) and whose agent says hello but never answers
/// the launcher's probe, and something on the control topics that is no message: the start waits
/// in vain and says which of the two it was, and the run says what it dropped.
void hello_without_an_answer(const fs::path& ecu, const fs::path& dir)
{
    std::ofstream(dir / "sim.yaml", std::ios::binary)
        << "vehicle_sil_simulation: 1\nstop_at_ms: 100\nnetworks: []\nparticipants:\n  - name: Engine\n    command: [" +
               yaml_quote(process::shown_path(ecu)) + "]\n";
    std::ofstream(dir / "Engine.behaviour") << "variables\n";
    // The ECU itself runs with --no-control behind the launcher's back; the hello under its name
    // comes from this process.
    std::ofstream(dir / "test-ecu-no-control") << "\n";
    namespace topics = control_silkit::detail;
    std::unique_ptr<SilKit::IParticipant> impostor;
    control::Hub hub;
    launch::RunOptions options;
    options.hub = &hub;
    options.control_hello_timeout = 700ms;
    std::string lines;
    options.write_line = [&](std::string_view line) {
        lines += std::string(line) + "\n";
        const std::string_view head = "[vehicle_sil_launch] registry at ";
        if (line.substr(0, head.size()) == head && !impostor)
        {
            const std::string uri(line.substr(head.size()));
            impostor = SilKit::CreateParticipant(
                SilKit::Config::ParticipantConfigurationFromString(vehicle_sil::default_participant_configuration(uri)), "Impostor",
                uri);
            auto* hello_out = impostor->CreateDataPublisher(topics::kHelloTopic, topics::spec(topics::kHelloTopic), 1);
            auto* step_out = impostor->CreateDataPublisher(topics::kStepTopic, topics::spec(topics::kStepTopic), 1);
            control::wire::Hello hello;
            hello.participant = "Engine";
            hello.session = 4711;
            control::wire::Bytes bytes;
            std::string why;
            require(control::wire::encode_hello(hello, bytes, why), why);
            hello_out->Publish(topics::span_of(bytes));
            const control::wire::Bytes junk{'j', 'u', 'n', 'k'};
            step_out->Publish(topics::span_of(junk));
        }
    };
    const launch::RunReport report = launch::run_file(dir / "sim.yaml", "sim.yaml", options);
    impostor.reset();
    fs::remove(dir / "test-ecu-no-control");
    const std::string shown = "\n--- result ---\n" + launch::format_run_text(report) + "--- lines ---\n" + lines;
    require(report.result() == launch::RunResult::Passed, "the run did not pass (the findings are warnings)" + shown);
    const auto& engine = report.run->participants.at(0);
    require(engine.control == launch::ControlLink::NoHello && engine.control_hello_seen,
            "Engine said hello and did not answer: no_hello" + shown);
    bool no_answer = false;
    bool dropped = false;
    for (const auto& f : report.findings)
    {
        no_answer = no_answer || (f.code == "control-link-no-hello" && f.participant == "Engine" &&
                                  f.message.find("said hello and then did not answer that it receives the launcher's commands "
                                                 "within 700 ms") != std::string::npos);
        dropped = dropped || (f.code == "control-link-dropped" &&
                              f.message.find("0 hellos it did not take, 1 messages it could not read and 0 messages") !=
                                  std::string::npos);
    }
    require(report.findings.size() == 2 && no_answer && dropped, "the two findings say what happened" + shown);
}

/// The run of a configuration with an ECU (Engine) and, with `stuck`, a participant "Stuck" of this
/// process that joins and never gets ready (its CommunicationReady is never completed), for the
/// limits of the start and the hold before the first step (--start-paused with a control server:
/// here a serve_control that starts nothing). `during_hold` runs on a thread of its own once the
/// launcher says that it stands still before the first step.
struct HoldRun
{
    launch::RunReport report;
    std::string lines;
    bool held = false;
    std::chrono::steady_clock::duration took{};
    /// A control server's stop, tried when the launcher began to ask the participants to stop:
    /// whether it was accepted (empty: not tried).
    std::optional<bool> stop_while_ending{};
};

HoldRun hold_run(const fs::path& ecu, const fs::path& helper, const fs::path& dir, bool stuck, bool start_paused,
                 launch::TimeCommands& commands, std::optional<std::chrono::milliseconds> timeout,
                 const std::function<void()>& during_hold, const std::atomic<int>* interrupts = nullptr,
                 const std::string& more_keys = {})
{
    std::string config = "vehicle_sil_simulation: 1\nstop_at_ms: 200\nstart_timeout_ms: 2000\n" + more_keys +
                         "networks: []\nparticipants:\n"
                         "  - name: Engine\n    command: [" + yaml_quote(process::shown_path(ecu)) + "]\n";
    if (stuck)
    {
        config += "  - name: Stuck\n    describe: false\n    command: [" + yaml_quote(process::shown_path(helper)) +
                  ", --sleep-ms, '600000']\n";
    }
    std::ofstream(dir / "sim.yaml", std::ios::binary) << config;
    fs::remove(dir / "Engine.behaviour");
    std::unique_ptr<SilKit::IParticipant> participant;
    std::thread holder;
    HoldRun out;
    launch::RunOptions options;
    options.start_paused = start_paused;
    options.time_commands = &commands;
    options.timeout = timeout;
    options.interrupts = interrupts;
    options.interrupt_name = [] { return std::string("SIGINT"); };
    options.serve_control = [](control::Hub&) { return launch::ControlServerInfo{"(test)", false, false}; };
    options.write_line = [&](std::string_view line) {
        out.lines += std::string(line) + "\n";
        const std::string_view registry = "[vehicle_sil_launch] registry at ";
        if (stuck && line.substr(0, registry.size()) == registry && !participant)
        {
            const std::string uri(line.substr(registry.size()));
            participant = SilKit::CreateParticipant(
                SilKit::Config::ParticipantConfigurationFromString(vehicle_sil::default_participant_configuration(uri)), "Stuck", uri);
            auto* lifecycle = participant->CreateLifecycleService(
                {SilKit::Services::Orchestration::OperationMode::Coordinated});
            lifecycle->SetCommunicationReadyHandlerAsync([] {}); // never completed
            static_cast<void>(lifecycle->StartLifecycle());
        }
        if ((line.find("] asking ") != std::string_view::npos || line.find("] cannot ask ") != std::string_view::npos) &&
            !out.stop_while_ending)
        {
            launch::Command stop;
            stop.name = "stop";
            stop.accepted = true;
            out.stop_while_ending = commands.apply(stop, "control_server").accepted;
        }
        if (line.find("standing still before the first step") != std::string_view::npos && !out.held)
        {
            out.held = true;
            holder = std::thread(during_hold);
        }
    };
    const auto begin = std::chrono::steady_clock::now();
    out.report = launch::run_file(dir / "sim.yaml", "sim.yaml", options);
    out.took = std::chrono::steady_clock::now() - begin;
    if (holder.joinable())
    {
        holder.join();
    }
    participant.reset();
    return out;
}

std::string shown(const HoldRun& run)
{
    return "\n--- result ---\n" + launch::format_run_text(run.report) + "--- lines ---\n" + run.lines;
}

launch::Command resume_command()
{
    launch::Command c;
    c.name = "resume";
    c.accepted = true;
    return c;
}

/// The start's limit is start_timeout_ms before the hold and again after it, never in it.
void the_start_is_limited_before_and_after_the_hold_but_not_in_it(const fs::path& ecu, const fs::path& helper, const fs::path& dir)
{
    {
        // Without the hold: a participant that never gets ready ends the start at its limit.
        launch::TimeCommands commands(false);
        const HoldRun run = hold_run(ecu, helper, dir, true, false, commands, std::nullopt, [] {});
        require(!run.held, "no hold without --start-paused" + shown(run));
        require(run.report.run && run.report.run->simulation == launch::SimulationEnd::NotStarted &&
                    run.report.run->start_error.find("did not all get to their first step within") != std::string::npos &&
                    run.report.run->start_error.find("Stuck (") != std::string::npos &&
                    run.report.run->start_error.find("after the run stopped standing still") == std::string::npos,
                "the start ended at its limit and named who was not ready" + shown(run));
        require(run.took < 60s, "the run took too long" + shown(run));
        // The run is over once the start has failed: a stop while it ends the participants is
        // refused (it would cut short their end).
        require(run.stop_while_ending == std::optional<bool>(false),
                "a stop while the participants end after a failed start is refused" + shown(run));
    }
    {
        // With the hold: it stands still longer than start_timeout_ms (2000 ms), then the start
        // has start_timeout_ms again from the resume.
        launch::TimeCommands commands(true);
        std::chrono::steady_clock::time_point resumed{};
        const HoldRun run = hold_run(ecu, helper, dir, true, true, commands, std::nullopt, [&] {
            std::this_thread::sleep_for(3000ms);
            resumed = std::chrono::steady_clock::now();
            require(commands.apply(resume_command(), "stdin").accepted, "the resume was refused");
        });
        require(run.held, "the run held before the first step" + shown(run));
        require(run.report.run && run.report.run->simulation == launch::SimulationEnd::NotStarted &&
                    run.report.run->start_error.find("within 2000 ms after the run stopped standing still before it") !=
                        std::string::npos &&
                    run.report.run->start_error.find("Stuck (") != std::string::npos,
                "the start ended start_timeout_ms after the resume" + shown(run));
        require(run.report.run->start_error.find("31536") == std::string::npos, "no year in the message" + shown(run));
    }
}

/// The ECUs wait as long as the run stands still before the first step: the limit the launcher
/// gives them for their own wait for the start (it ends an ECU that never sees the simulation
/// start) does not end the hold. Stands still longer than that limit would be without the hold
/// (start_timeout_ms + stop_timeout_ms + 10 s), then resumes: the run passes.
void the_ecus_wait_as_long_as_the_run_stands_still(const fs::path& ecu, const fs::path& helper, const fs::path& dir)
{
    launch::SimulationConfig limits;
    limits.start_timeout_ms = 2000;
    limits.stop_timeout_ms = 1000;
    const std::chrono::milliseconds without_hold{launch::detail::ecu_start_timeout_ms(limits, false)};
    require(without_hold == 13000ms, "the limit of an ECU without the hold is the one this test stands still for");
    launch::TimeCommands commands(true);
    const HoldRun run = hold_run(ecu, helper, dir, false, true, commands, std::nullopt, [&] {
        // The ECU began its wait before the hold: this is longer than its limit would be.
        std::this_thread::sleep_for(without_hold + 1500ms);
        require(commands.apply(resume_command(), "stdin").accepted, "the resume was refused");
    }, nullptr, "stop_timeout_ms: 1000\n");
    require(run.held, "the run held before the first step" + shown(run));
    require(run.took >= without_hold, "the run stood still longer than the limit" + shown(run));
    require(run.report.result() == launch::RunResult::Passed && run.report.run &&
                run.report.run->simulation == launch::SimulationEnd::ReachedStopAt,
            "the run went on after the hold and passed" + shown(run));
}

/// --timeout-ms counts the time the run stands still before the first step: a client that never
/// comes does not leave the run waiting.
void timeout_counts_the_hold(const fs::path& ecu, const fs::path& helper, const fs::path& dir)
{
    launch::TimeCommands commands(true);
    const HoldRun run = hold_run(ecu, helper, dir, false, true, commands, 1500ms, [] {});
    require(run.held, "the run held" + shown(run));
    require(run.report.run && run.report.run->timed_out && run.report.run->timed_out_while_holding &&
                run.report.result() == launch::RunResult::Failed,
            "the run timed out" + shown(run));
    bool said = false;
    for (const auto& f : run.report.findings)
    {
        said = said || (f.code == "timeout" && f.message.find("it stood still before its first step all that time") !=
                                                   std::string::npos);
    }
    require(said, "the finding says that it stood still" + shown(run));
}

/// Ctrl+C in the hold ends it: the simulation starts and is stopped (its step at 0 runs).
void an_interrupt_ends_the_hold(const fs::path& ecu, const fs::path& helper, const fs::path& dir)
{
    launch::TimeCommands commands(true);
    std::atomic<int> interrupts{0};
    const HoldRun run = hold_run(ecu, helper, dir, false, true, commands, std::nullopt, [&] {
        std::this_thread::sleep_for(200ms);
        interrupts = 1;
    }, &interrupts);
    require(run.held, "the run held" + shown(run));
    require(run.report.result() == launch::RunResult::Interrupted && run.report.run &&
                run.report.run->simulation == launch::SimulationEnd::Interrupted,
            "the run was interrupted after the start" + shown(run));
}

/// A resume before the window: the run does not hold (and runs to stop_at_ms).
void a_resume_before_the_window_means_no_hold(const fs::path& ecu, const fs::path& helper, const fs::path& dir)
{
    launch::TimeCommands commands(true);
    require(commands.apply(resume_command(), "control_server").accepted, "the resume was refused");
    const HoldRun run = hold_run(ecu, helper, dir, false, true, commands, std::nullopt, [] {});
    require(!run.held && run.report.result() == launch::RunResult::Passed && run.report.run &&
                run.report.run->simulation == launch::SimulationEnd::ReachedStopAt,
            "no hold, and the run ran to its end" + shown(run));
}

} // namespace

int main(int argc, char** argv)
{
    // Each part is a test of its own (they run side by side), in a directory of its own.
    using Part = std::function<void(const fs::path& ecu, const fs::path& helper, const fs::path& dir)>;
    const std::vector<std::pair<std::string, Part>> parts = {
        {"a_write_in_the_start_window_reaches_each_process_before_its_first_step",
         [](const fs::path& ecu, const fs::path&, const fs::path& dir) {
             for (int round = 1; round <= 5; ++round)
             {
                 one_run(ecu, dir, round);
                 std::printf("round %d ok\n", round);
                 std::fflush(stdout);
             }
         }},
        {"what_fails_before_the_start_aborts_the_simulation", [](const fs::path& ecu, const fs::path&, const fs::path& dir) { failing_start(ecu, dir); }},
        {"a_stop_while_what_is_done_before_the_start_fails_does_not_cut_the_end_short",
         [](const fs::path& ecu, const fs::path&, const fs::path& dir) { failing_start_with_a_stop(ecu, dir); }},
        {"a_hello_without_an_answer_and_what_is_no_message_are_reported", [](const fs::path& ecu, const fs::path&, const fs::path& dir) { hello_without_an_answer(ecu, dir); }},
        {"the_start_is_limited_before_and_after_the_hold_but_not_in_it", the_start_is_limited_before_and_after_the_hold_but_not_in_it},
        {"the_ecus_wait_as_long_as_the_run_stands_still", the_ecus_wait_as_long_as_the_run_stands_still},
        {"timeout_counts_the_hold", timeout_counts_the_hold},
        {"an_interrupt_ends_the_hold", an_interrupt_ends_the_hold},
        {"a_resume_before_the_window_means_no_hold", a_resume_before_the_window_means_no_hold},
    };
    std::string names;
    for (const auto& [name, part] : parts)
    {
        names += (names.empty() ? "" : "|") + name;
    }
    if (argc != 5)
    {
        std::fprintf(stderr, "usage: %s <test ECU executable> <scratch directory> <process helper> <%s>\n",
                     argc > 0 ? argv[0] : "launch_control_test", names.c_str());
        return 2;
    }
    const auto it = std::find_if(parts.begin(), parts.end(), [&](const auto& p) { return p.first == argv[4]; });
    if (it == parts.end())
    {
        std::fprintf(stderr, "launch_control_test: no part %s (the parts: %s)\n", argv[4], names.c_str());
        return 2;
    }
    try
    {
        const fs::path ecu = process::path_from_utf8(argv[1]);
        const fs::path dir = process::path_from_utf8(argv[2]) / it->first;
        const fs::path helper = process::path_from_utf8(argv[3]);
        fs::remove_all(dir);
        fs::create_directories(dir);
        it->second(ecu, helper, dir);
        std::printf("%s ok\n", it->first.c_str());
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
