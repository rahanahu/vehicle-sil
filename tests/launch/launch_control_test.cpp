// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch's run as a library (run_file()), whose hub the caller keeps from the start
// window on (RunObservers::before_start), and ECUs in their
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
#include "vehicle_sil/runner/detail/run_detail.hpp"
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
#include <span>
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
/// The source of a stop request where the run does not read it (RunObservers::interrupt_name names it).
constexpr int kAnySource = 1;

void require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

/// The request of a run of the file sim.yaml in `dir`.
launch::RunRequest sim_of(const fs::path& dir)
{
    launch::RunRequest request;
    request.host.config = process::utf8_from_path(dir / "sim.yaml");
    return request;
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

    // The run's hub, kept from the window on: read after the run.
    std::shared_ptr<control::Hub> kept;
    std::uint64_t seq = 0;
    std::optional<control::OperationResult> queued;
    std::size_t peers_in_window = 0;
    launch::RunRequest asked = sim_of(dir);
    vehicle_sil::runner::RunObservers watching;
    launch::RunLent lent;
    vehicle_sil::runner::RunLimits limits;
    watching.before_start = [&](const std::shared_ptr<control::Hub>& given) {
        kept = given;
        control::Hub& h = *given;
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
    watching.write_line = [&lines](std::string_view line) { lines += std::string(line) + "\n"; };
    const launch::RunReport report = launch::run_file(asked, watching, lent, limits);
    const std::string shown = "\n--- result ---\n" + launch::format_run_text(report) + "--- lines ---\n" + lines;

    require(report.result() == launch::RunResult::Passed, "the run did not pass" + shown);
    require(report.findings.empty(), "the run has findings" + shown);
    require(report.run.has_value() && report.run->end.simulation == vehicle_sil::runner::SimulationEnd::ReachedStopAt, "the run did not reach stop_at_ms" + shown);
    for (const auto& p : report.run->participants)
    {
        require(p.control == vehicle_sil::runner::ControlLink::Connected, p.name + " is not connected" + shown);
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
    // The run is over and its parts are gone: the hub is this test's now, with what the agents
    // said last.
    require(kept != nullptr, "the window was not given the run's hub");
    const control::Hub& hub = *kept;
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
    launch::RunRequest asked = sim_of(dir);
    vehicle_sil::runner::RunObservers watching;
    launch::RunLent lent;
    vehicle_sil::runner::RunLimits limits;
    watching.before_start = [](const std::shared_ptr<control::Hub>&) { throw std::runtime_error("the scenario could not be set up"); };
    std::string lines;
    watching.write_line = [&lines](std::string_view line) { lines += std::string(line) + "\n"; };
    const auto begin = std::chrono::steady_clock::now();
    const launch::RunReport report = launch::run_file(asked, watching, lent, limits);
    const std::string shown = "\n--- result ---\n" + launch::format_run_text(report) + "--- lines ---\n" + lines;
    // Without stop_at_ms the simulation would run for ever: it was ended for the failure.
    require(std::chrono::steady_clock::now() - begin < 60s, "the run took too long" + shown);
    require(report.result() == launch::RunResult::Failed, "the run did not fail" + shown);
    require(report.run.has_value() && report.run->end.simulation == vehicle_sil::runner::SimulationEnd::Aborted, "the simulation was not aborted" + shown);
    require(report.findings.size() == 1 && report.findings[0].code == "before-start" &&
                report.findings[0].message.find("the scenario could not be set up") != std::string::npos,
            "the finding says what failed" + shown);
    require(lines.find("the simulation started, but what was to be done before it failed") != std::string::npos,
            "the launcher says that the simulation had started" + shown);
    require(report.run->participants.at(0).control == vehicle_sil::runner::ControlLink::Connected, "Engine was connected" + shown);
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
    vehicle_sil::runner::StopRequests stops;
    launch::RunRequest asked = sim_of(dir);
    vehicle_sil::runner::RunObservers watching;
    launch::RunLent lent;
    vehicle_sil::runner::RunLimits limits;
    lent.stops = &stops;
    watching.interrupt_name = [] { return std::string("control server stop"); };
    watching.before_start = [&stops](const std::shared_ptr<control::Hub>&) {
        stops.request(kAnySource);
        throw std::runtime_error("the scenario could not be set up");
    };
    std::string lines;
    watching.write_line = [&lines](std::string_view line) { lines += std::string(line) + "\n"; };
    const launch::RunReport report = launch::run_file(asked, watching, lent, limits);
    const std::string shown = "\n--- result ---\n" + launch::format_run_text(report) + "--- lines ---\n" + lines;
    require(report.run.has_value() && !report.run->end.interrupted_while_ending &&
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
    launch::RunRequest asked = sim_of(dir);
    vehicle_sil::runner::RunObservers watching;
    launch::RunLent lent;
    vehicle_sil::runner::RunLimits limits;
    limits.control_hello_timeout = 700ms;
    std::string lines;
    watching.write_line = [&](std::string_view line) {
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
    const launch::RunReport report = launch::run_file(asked, watching, lent, limits);
    impostor.reset();
    fs::remove(dir / "test-ecu-no-control");
    const std::string shown = "\n--- result ---\n" + launch::format_run_text(report) + "--- lines ---\n" + lines;
    require(report.result() == launch::RunResult::Passed, "the run did not pass (the findings are warnings)" + shown);
    const auto& engine = report.run->participants.at(0);
    require(engine.control == vehicle_sil::runner::ControlLink::NoHello && engine.control_hello_seen,
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

/// The window before the start waits for the agent of every ECU whose --describe offers one, also
/// of an ECU without variables (the launcher reads the counters of its networks from it). Shown by
/// what a wait in vain leaves: the ECU's --describe has no variables (not even the built-in ones of
/// an ecu_main executable) and offers an agent, and it runs with --no-control behind the launcher's
/// back.
void an_ecu_without_variables_is_waited_for(const fs::path& ecu, const fs::path& dir)
{
    std::ofstream(dir / "sim.yaml", std::ios::binary)
        << "vehicle_sil_simulation: 1\nstop_at_ms: 100\nnetworks: []\nparticipants:\n  - name: Engine\n    command: [" +
               yaml_quote(process::shown_path(ecu)) + "]\n";
    fs::remove(dir / "Engine.behaviour");
    std::ofstream(dir / "test-ecu-no-control") << "\n";
    std::ofstream(dir / "test-ecu-no-variables") << "\n";
    launch::RunRequest asked = sim_of(dir);
    vehicle_sil::runner::RunObservers watching;
    launch::RunLent lent;
    vehicle_sil::runner::RunLimits limits;
    limits.control_hello_timeout = 300ms;
    std::string lines;
    watching.write_line = [&](std::string_view line) { lines += std::string(line) + "\n"; };
    const launch::RunReport report = launch::run_file(asked, watching, lent, limits);
    fs::remove(dir / "test-ecu-no-control");
    fs::remove(dir / "test-ecu-no-variables");
    const std::string shown = "\n--- result ---\n" + launch::format_run_text(report) + "--- lines ---\n" + lines;
    require(report.run.has_value() && report.result() == launch::RunResult::Passed,
            "the run did not pass (the finding is a warning)" + shown);
    const auto& engine = report.run->participants.at(0);
    require(engine.control == vehicle_sil::runner::ControlLink::NoHello && !engine.control_hello_seen,
            "Engine has no variables and was waited for: no_hello" + shown);
    require(std::any_of(report.findings.begin(), report.findings.end(),
                        [](const auto& f) { return f.code == "control-link-no-hello" && f.participant == "Engine"; }),
            "the finding says that its hello did not come" + shown);
}

/// A run of one participant that does not describe itself (Body, describe: false) and has a
/// variable map with an entry that does not fit what its agent announces. The window before the
/// start does not wait for its hello (the launcher does not know that it has an agent), so the
/// hello can come at any time. Here the ECU runs with --no-control behind the launcher's back and
/// the hello under its name comes from this process, at the time the part wants:
/// - `in_window`: in the window, after the launcher has looked at the links there (before_start
///   runs right after that look), with the answer that the agent takes commands;
/// - else: once the launcher has said that the simulation runs.
/// Whenever it comes, the result has what does not fit, and the run fails for it.
void a_late_hello(const fs::path& ecu, const fs::path& dir, bool in_window)
{
    std::ofstream(dir / "sim.yaml", std::ios::binary)
        << "vehicle_sil_simulation: 1\nstop_at_ms: 100\nnetworks: []\nparticipants:\n  - name: Body\n    command: [" +
               yaml_quote(process::shown_path(ecu)) +
               ", --registry-uri, \"{registry_uri}\", --name, \"{name}\"]\n    describe: false\n"
               "    variables:\n      body.fill: { from: levle }\n      body.gain: gain\n";
    fs::remove(dir / "Body.behaviour");
    std::ofstream(dir / "test-ecu-no-control") << "\n";
    namespace topics = control_silkit::detail;
    namespace pubsub = SilKit::Services::PubSub;
    const std::uint64_t session = 4712;
    std::unique_ptr<SilKit::IParticipant> impostor;
    pubsub::IDataPublisher* hello_out = nullptr;
    pubsub::IDataPublisher* submitted_out = nullptr;
    std::atomic<bool> answered{false};
    // The run's hub, from the window on (before_start below). Read by the thread that writes the
    // line "simulation running" too: the window's thread has ended by then.
    std::shared_ptr<control::Hub> hub;
    const auto say_hello = [&](const std::string& as, std::uint64_t with_session) {
        control::wire::Hello hello;
        hello.participant = as;
        hello.session = with_session;
        for (const char* name : {"level", "gain"})
        {
            control::VariableInfo variable;
            variable.name = name;
            hello.schema.push_back(variable);
        }
        control::wire::Bytes bytes;
        std::string why;
        require(control::wire::encode_hello(hello, bytes, why), why);
        hello_out->Publish(topics::span_of(bytes));
    };
    const auto wait_for_peer = [&](const std::string& name) {
        const auto deadline = std::chrono::steady_clock::now() + scaled_timeout(30s);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto peers = hub->peers();
            if (std::any_of(peers.begin(), peers.end(), [&](const control::PeerInfo& p) { return p.participant == name; }))
            {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        return false;
    };
    bool hello_arrived = false;
    bool marker_arrived = false;
    launch::RunRequest asked = sim_of(dir);
    vehicle_sil::runner::RunObservers watching;
    launch::RunLent lent;
    vehicle_sil::runner::RunLimits limits;
    watching.before_start = [&](const std::shared_ptr<control::Hub>& given) {
        hub = given;
        if (in_window)
        {
            say_hello("Body", session);
            hello_arrived = wait_for_peer("Body");
            const auto deadline = std::chrono::steady_clock::now() + scaled_timeout(30s);
            while (!answered.load() && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(2ms);
            }
            // The answer is on its way to the launcher. A message sent after it arrives after it:
            // once this second hello is there, the launcher has the answer.
            say_hello("Marker", session + 1);
            marker_arrived = wait_for_peer("Marker");
        }
    };
    std::string lines;
    watching.write_line = [&](std::string_view line) {
        lines += std::string(line) + "\n";
        const std::string_view head = "[vehicle_sil_launch] registry at ";
        if (line.substr(0, head.size()) == head && !impostor)
        {
            const std::string uri(line.substr(head.size()));
            impostor = SilKit::CreateParticipant(
                SilKit::Config::ParticipantConfigurationFromString(vehicle_sil::default_participant_configuration(uri)), "Impostor",
                uri);
            hello_out = impostor->CreateDataPublisher(topics::kHelloTopic, topics::spec(topics::kHelloTopic), 1);
            submitted_out = impostor->CreateDataPublisher(topics::kSubmittedTopic, topics::spec(topics::kSubmittedTopic), 0);
            impostor->CreateDataSubscriber(
                topics::kCommandTopic, topics::spec(topics::kCommandTopic),
                [&](pubsub::IDataSubscriber*, const pubsub::DataMessageEvent& event) {
                    const auto probe = control::wire::addressed(std::span<const std::uint8_t>(event.data.data(), event.data.size()));
                    if (probe && probe->kind == control::wire::Kind::Probe && probe->session == session)
                    {
                        control::wire::Bytes bytes;
                        control::wire::encode_session(control::wire::Kind::Ready, session, bytes);
                        submitted_out->Publish(topics::span_of(bytes));
                        answered.store(true);
                    }
                });
        }
        if (!in_window && line == "[vehicle_sil_launch] simulation running")
        {
            // After the launcher has looked at the links for the second time (after the start).
            say_hello("Body", session);
            hello_arrived = wait_for_peer("Body");
        }
    };
    const launch::RunReport report = launch::run_file(asked, watching, lent, limits);
    impostor.reset();
    fs::remove(dir / "test-ecu-no-control");
    const std::string shown = "\n--- result ---\n" + launch::format_run_text(report) + "--- lines ---\n" + lines;
    require(hello_arrived && (!in_window || (answered.load() && marker_arrived)), "the hello did not get to the hub" + shown);
    require(report.run.has_value(), "the run did not start" + shown);
    const auto& body = report.run->participants.at(0);
    const std::vector<std::string> problems{"body.fill: the executable has no variable 'levle'"};
    require(body.variable_map_problems == problems, "what does not fit is in the result" + shown);
    require(body.control == (in_window ? vehicle_sil::runner::ControlLink::Connected : vehicle_sil::runner::ControlLink::None),
            in_window ? "an agent that connected in the window is connected" + shown
                      : "the link is as it was at the start" + shown);
    const bool finding = std::any_of(report.findings.begin(), report.findings.end(), [](const auto& f) {
        return f.code == "variable-map-not-applied" && f.participant == "Body";
    });
    require(finding && report.result() == launch::RunResult::Failed, "the run fails for the mapping" + shown);
    const std::string line =
        in_window ? "[vehicle_sil_launch] participant 'Body': variables: 1 of its mappings do not fit the variables its "
                    "control agent announced and are not applied: body.fill: the executable has no variable 'levle'. In "
                    "this run those variables are read under the executable's names and values, and a write to a name of "
                    "these mappings is refused\n"
                  : "[vehicle_sil_launch] participant 'Body': variables: 1 of its mappings do not fit the variables its "
                    "control agent announced, which connected after the simulation had started, and were not applied: "
                    "body.fill: the executable has no variable 'levle'. In this run those variables were read under the "
                    "executable's names and values, and a write to a name of these mappings was refused\n";
    require(lines.find(line) != std::string::npos, "the line of the run about the mapping" + shown);
}

/// The run of a configuration with an ECU (Engine) and, with `stuck`, a participant "Stuck" of this
/// process that joins and never gets ready (its CommunicationReady is never completed), for the
/// limits of the start and the hold before the first step (--start-paused with a control server:
/// here an endpoint that starts nothing). `during_hold` runs on a thread of its own once the
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
                 vehicle_sil::runner::RunControls& controls, std::optional<std::chrono::milliseconds> timeout,
                 const std::function<void()>& during_hold, const std::string& more_keys = {})
{
    vehicle_sil::runner::TimeCommands& commands = controls.time();
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
    launch::RunRequest asked = sim_of(dir);
    vehicle_sil::runner::RunObservers watching;
    launch::RunLent lent;
    vehicle_sil::runner::RunLimits limits;
    asked.intent.start_paused = start_paused;
    lent.controls = &controls;
    asked.host.timeout = timeout;
    watching.interrupt_name = [] { return std::string("SIGINT"); };
    struct NoServer final : launch::ControlEndpoint
    {
        vehicle_sil::runner::ControlServerInfo start(std::shared_ptr<control::Hub>) override { return {"(test)", false, false}; }
        void end() override {}
    } endpoint;
    lent.control_endpoint = &endpoint;
    watching.write_line = [&](std::string_view line) {
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
            vehicle_sil::runner::Command stop;
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
    out.report = launch::run_file(asked, watching, lent, limits);
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

vehicle_sil::runner::Command resume_command()
{
    vehicle_sil::runner::Command c;
    c.name = "resume";
    c.accepted = true;
    return c;
}

/// The start's limit is start_timeout_ms before the hold and again after it, never in it.
void the_start_is_limited_before_and_after_the_hold_but_not_in_it(const fs::path& ecu, const fs::path& helper, const fs::path& dir)
{
    {
        // Without the hold: a participant that never gets ready ends the start at its limit.
        vehicle_sil::runner::StopRequests stops;
        vehicle_sil::runner::RunControls controls(stops, false);
        const HoldRun run = hold_run(ecu, helper, dir, true, false, controls, std::nullopt, [] {});
        require(!run.held, "no hold without --start-paused" + shown(run));
        require(run.report.run && run.report.run->end.simulation == vehicle_sil::runner::SimulationEnd::NotStarted &&
                    run.report.run->start.error.find("did not all get to their first step within") != std::string::npos &&
                    run.report.run->start.error.find("Stuck (") != std::string::npos &&
                    run.report.run->start.error.find("after the run stopped standing still") == std::string::npos,
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
        vehicle_sil::runner::StopRequests stops;
        vehicle_sil::runner::RunControls controls(stops, true);
        vehicle_sil::runner::TimeCommands& commands = controls.time();
        std::chrono::steady_clock::time_point resumed{};
        const HoldRun run = hold_run(ecu, helper, dir, true, true, controls, std::nullopt, [&] {
            std::this_thread::sleep_for(3000ms);
            resumed = std::chrono::steady_clock::now();
            require(commands.apply(resume_command(), "stdin").accepted, "the resume was refused");
        });
        require(run.held, "the run held before the first step" + shown(run));
        require(run.report.run && run.report.run->end.simulation == vehicle_sil::runner::SimulationEnd::NotStarted &&
                    run.report.run->start.error.find("within 2000 ms after the run stopped standing still before it") !=
                        std::string::npos &&
                    run.report.run->start.error.find("Stuck (") != std::string::npos,
                "the start ended start_timeout_ms after the resume" + shown(run));
        require(run.report.run->start.error.find("31536") == std::string::npos, "no year in the message" + shown(run));
    }
}

/// The ECUs wait as long as the run stands still before the first step: the limit the launcher
/// gives them for their own wait for the start (it ends an ECU that never sees the simulation
/// start) does not end the hold. Stands still longer than that limit would be without the hold
/// (start_timeout_ms + stop_timeout_ms + 10 s), then resumes: the run passes.
void the_ecus_wait_as_long_as_the_run_stands_still(const fs::path& ecu, const fs::path& helper, const fs::path& dir)
{
    config::SimulationConfig limits;
    limits.start_timeout_ms = 2000;
    limits.stop_timeout_ms = 1000;
    const std::chrono::milliseconds without_hold{vehicle_sil::runner::detail::ecu_start_timeout_ms(limits, false)};
    require(without_hold == 13000ms, "the limit of an ECU without the hold is the one this test stands still for");
    vehicle_sil::runner::StopRequests stops;
    vehicle_sil::runner::RunControls controls(stops, true);
    vehicle_sil::runner::TimeCommands& commands = controls.time();
    const HoldRun run = hold_run(ecu, helper, dir, false, true, controls, std::nullopt, [&] {
        // The ECU began its wait before the hold: this is longer than its limit would be.
        std::this_thread::sleep_for(without_hold + 1500ms);
        require(commands.apply(resume_command(), "stdin").accepted, "the resume was refused");
    }, "stop_timeout_ms: 1000\n");
    require(run.held, "the run held before the first step" + shown(run));
    require(run.took >= without_hold, "the run stood still longer than the limit" + shown(run));
    require(run.report.result() == launch::RunResult::Passed && run.report.run &&
                run.report.run->end.simulation == vehicle_sil::runner::SimulationEnd::ReachedStopAt,
            "the run went on after the hold and passed" + shown(run));
}

/// --timeout-ms counts the time the run stands still before the first step: a client that never
/// comes does not leave the run waiting.
void timeout_counts_the_hold(const fs::path& ecu, const fs::path& helper, const fs::path& dir)
{
    vehicle_sil::runner::StopRequests stops;
    vehicle_sil::runner::RunControls controls(stops, true);
    const HoldRun run = hold_run(ecu, helper, dir, false, true, controls, 1500ms, [] {});
    require(run.held, "the run held" + shown(run));
    require(run.report.run && run.report.run->end.timed_out && run.report.run->end.timed_out_while_holding &&
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
    vehicle_sil::runner::StopRequests stops;
    vehicle_sil::runner::RunControls controls(stops, true);
    const HoldRun run = hold_run(ecu, helper, dir, false, true, controls, std::nullopt, [&] {
        std::this_thread::sleep_for(200ms);
        stops.request(kAnySource);
    });
    require(run.held, "the run held" + shown(run));
    require(run.report.result() == launch::RunResult::Interrupted && run.report.run &&
                run.report.run->end.simulation == vehicle_sil::runner::SimulationEnd::Interrupted,
            "the run was interrupted after the start" + shown(run));
}

/// A scenario that the run refuses to begin (its ECU sends the values it reads less often than its
/// step) does not hold before the first step: nothing would run after the hold, so the run ends at
/// once with the finding, without a resume. (The launcher's command line refuses a scenario with
/// --start-paused; a caller of run_file() can ask for both.)
void a_refused_scenario_does_not_hold(const fs::path& ecu, const fs::path& dir)
{
    std::ofstream(dir / "sim.yaml", std::ios::binary)
        << "vehicle_sil_simulation: 1\nstart_timeout_ms: 5000\nnetworks: []\nparticipants:\n  - name: Engine\n    command: ["
        << yaml_quote(process::shown_path(ecu)) << "]\n";
    std::ofstream(dir / "Engine.behaviour", std::ios::binary) << "sample-ms 10 scenario-variables\n";
    std::ofstream(dir / "test-ecu-step-ms", std::ios::binary) << "1\n";
    std::ofstream(dir / "test-ecu-own-sample-period", std::ios::binary) << "\n";
    std::ofstream(dir / "s.yaml", std::ios::binary)
        << "vehicle_sil_scenario: 1\nsteps:\n  - at_ms: 20\n    expect: { Engine.ticks: { ge: 0 } }\n";
    runner::StopRequests stops;
    runner::RunControls controls(stops, true, {}, true);
    launch::RunRequest asked = sim_of(dir);
    asked.intent.start_paused = true;
    asked.intent.scenario = process::utf8_from_path(dir / "s.yaml");
    asked.host.timeout = 20s; // a run that held would end by this, as a failure of another kind
    runner::RunObservers watching;
    launch::RunLent lent;
    lent.controls = &controls;
    struct NoServer final : launch::ControlEndpoint
    {
        runner::ControlServerInfo start(std::shared_ptr<control::Hub>) override { return {"(test)", false, false}; }
        void end() override {}
    } endpoint;
    lent.control_endpoint = &endpoint;
    std::string lines;
    watching.write_line = [&](std::string_view line) { lines += std::string(line) + "\n"; };
    const launch::RunReport report = launch::run_file(asked, watching, lent, runner::RunLimits{});
    fs::remove(dir / "test-ecu-step-ms");
    fs::remove(dir / "test-ecu-own-sample-period");
    fs::remove(dir / "Engine.behaviour");
    std::string codes;
    for (const config::Finding& f : report.findings)
    {
        codes += f.code + " ";
    }
    const std::string seen = "\nfindings: " + codes + "\n" + lines;
    require(lines.find("standing still before the first step") == std::string::npos, "the run did not hold" + seen);
    require(report.result() == launch::RunResult::Failed && codes.find("scenario-not-observed") != std::string::npos &&
                codes.find("timeout") == std::string::npos,
            "the run failed for the scenario, not for a timeout" + seen);
    require(report.run && report.run->timing.sim_ms == 0 && report.run->scenario.result &&
                report.run->scenario.result->status == scenario::ScenarioStatus::Failed,
            "it ended at 0 ms with the scenario failed" + seen);
}

/// A resume before the window: the run does not hold (and runs to stop_at_ms). It comes before
/// the run has read its configuration, so it waits for the run's time lock and is accepted then.
void a_resume_before_the_window_means_no_hold(const fs::path& ecu, const fs::path& helper, const fs::path& dir)
{
    vehicle_sil::runner::StopRequests stops;
    vehicle_sil::runner::RunControls controls(stops, true);
    vehicle_sil::runner::TimeCommands& commands = controls.time();
    const vehicle_sil::runner::Command early = commands.apply(resume_command(), "control_server");
    require(early.pending && !early.accepted, "the resume before the run was decided without the run's time lock");
    const HoldRun run = hold_run(ecu, helper, dir, false, true, controls, std::nullopt, [] {});
    require(!run.held && run.report.result() == launch::RunResult::Passed && run.report.run &&
                run.report.run->end.simulation == vehicle_sil::runner::SimulationEnd::ReachedStopAt,
            "no hold, and the run ran to its end" + shown(run));
}

/// A control server of the test: counts its starts, notes what it saw, and fails if told to.
struct CountingEndpoint final : launch::ControlEndpoint
{
    vehicle_sil::runner::ControlServerInfo start(std::shared_ptr<control::Hub> served) override
    {
        ++starts;
        hub = std::move(served);
        if (at_start)
        {
            at_start();
        }
        if (fail_with_no_exception_type)
        {
            throw 42; // not a std::exception: it leaves run_file()
        }
        if (fail)
        {
            throw std::runtime_error("no port\nto listen on");
        }
        return {"(test):1", false, false};
    }
    void end() override {}

    int starts = 0;
    bool fail = false;
    bool fail_with_no_exception_type = false;
    std::function<void()> at_start{};
    std::shared_ptr<control::Hub> hub;
};

/// run_file() starts the control server once, when the run's files are open and before the
/// registry; a start that fails starts nothing; a run that does not get to its files does not
/// start the server.
void the_control_server_is_started_after_the_runs_files_and_before_the_registry(const fs::path& ecu, const fs::path& dir)
{
    std::ofstream(dir / "sim.yaml", std::ios::binary)
        << "vehicle_sil_simulation: 1\nstop_at_ms: 200\nnetworks: []\nparticipants:\n  - name: Engine\n    command: [" +
               yaml_quote(process::shown_path(ecu)) + "]\n";
    fs::remove(dir / "Engine.behaviour");
    const auto run = [](const launch::RunRequest& asked, const launch::RunLent& lent, std::string& lines) {
        vehicle_sil::runner::RunObservers watching;
        watching.write_line = [&lines](std::string_view line) { lines += std::string(line) + "\n"; };
        const launch::RunReport report = launch::run_file(asked, watching, lent);
        lines = "\n--- result ---\n" + launch::format_run_text(report) + "--- lines ---\n" + lines;
        return report;
    };
    const std::string server_line = "[vehicle_sil_launch] control server at (test):1\n";
    const std::string registry_line = "[vehicle_sil_launch] registry at ";
    {
        // It starts: the log directory is there, nothing has been said about a registry yet, and
        // the line about the server comes before the registry's.
        fs::remove_all(dir / "logs");
        CountingEndpoint endpoint;
        std::string lines;
        bool logs_open = false;
        bool registry_said = true;
        endpoint.at_start = [&] {
            logs_open = fs::exists(dir / "logs");
            registry_said = lines.find(registry_line) != std::string::npos;
        };
        launch::RunRequest asked = sim_of(dir);
        launch::RunLent lent;
        asked.host.log_dir = dir / "logs";
        lent.control_endpoint = &endpoint;
        const launch::RunReport report = run(asked, lent, lines);
        require(report.result() == launch::RunResult::Passed, "the run passed" + lines);
        require(endpoint.starts == 1 && endpoint.hub != nullptr, "the server was started once, on a hub" + lines);
        require(logs_open, "the log directory was there when the server started" + lines);
        require(!registry_said, "the server was started before the registry" + lines);
        const std::size_t server = lines.find(server_line);
        const std::size_t registry = lines.find(registry_line);
        require(server != std::string::npos && registry != std::string::npos && server < registry,
                "the line of the server comes before the registry's" + lines);
        require(endpoint.hub->peers().size() == 1, "the hub of the server is the run's: it has the ECU's agent" + lines);
    }
    {
        // It cannot start: nothing is started, and the finding says why, in one line.
        CountingEndpoint endpoint;
        endpoint.fail = true;
        std::string lines;
        launch::RunRequest asked = sim_of(dir);
        launch::RunLent lent;
        lent.control_endpoint = &endpoint;
        const launch::RunReport report = run(asked, lent, lines);
        require(endpoint.starts == 1, "the server was started once" + lines);
        require(report.run && report.run->setup.error == "the control server cannot be started: no port to listen on",
                "the outcome says why" + lines);
        require(report.findings.size() == 1 && report.findings[0].code == "setup" &&
                    report.findings[0].message ==
                        "cannot set up the simulation: the control server cannot be started: no port to listen on",
                "the finding says why" + lines);
        require(lines.find(registry_line) == std::string::npos && lines.find("control server at") == std::string::npos &&
                    report.run->setup.registry_uri.empty(),
                "no registry, and no line about a server" + lines);
    }
    {
        // Asked to stop before it began: the server is not started.
        CountingEndpoint endpoint;
        std::string lines;
        vehicle_sil::runner::StopRequests stops;
        stops.request(kAnySource);
        launch::RunRequest asked = sim_of(dir);
        launch::RunLent lent;
        lent.stops = &stops;
        lent.control_endpoint = &endpoint;
        const launch::RunReport report = run(asked, lent, lines);
        require(endpoint.starts == 0, "the server of a run that was interrupted before it began was not started" + lines);
        require(report.result() == launch::RunResult::Interrupted, "the run was interrupted" + lines);
    }
    {
        // The log directory cannot be made (a file is in its place): the server is not started.
        std::ofstream(dir / "not-a-directory") << "x";
        CountingEndpoint endpoint;
        std::string lines;
        launch::RunRequest asked = sim_of(dir);
        launch::RunLent lent;
        asked.host.log_dir = dir / "not-a-directory" / "logs";
        lent.control_endpoint = &endpoint;
        const launch::RunReport report = run(asked, lent, lines);
        require(endpoint.starts == 0, "the server of a run whose logs cannot be opened was not started" + lines);
        require(report.run && !report.run->setup.error.empty() && report.run->setup.registry_uri.empty(),
                "the run was not set up" + lines);
    }
}

/// What a control server's start throws that run_file() does not make a finding of leaves
/// run_file(), and the time commands have ended by then: they refuse a command, as after a run.
void what_leaves_run_file_between_the_stages_ends_the_time_commands(const fs::path& ecu, const fs::path& dir)
{
    std::ofstream(dir / "sim.yaml", std::ios::binary)
        << "vehicle_sil_simulation: 1\nstop_at_ms: 200\nnetworks: []\nparticipants:\n  - name: Engine\n    command: [" +
               yaml_quote(process::shown_path(ecu)) + "]\n";
    fs::remove(dir / "Engine.behaviour");
    vehicle_sil::runner::StopRequests stops;
    vehicle_sil::runner::RunControls controls(stops, true);
    require(controls.time().apply(resume_command(), "control_server").pending,
            "a command before the run waits for the time lock");
    CountingEndpoint endpoint;
    endpoint.fail_with_no_exception_type = true;
    launch::RunRequest asked = sim_of(dir);
    vehicle_sil::runner::RunObservers watching;
    launch::RunLent lent;
    vehicle_sil::runner::RunLimits limits;
    asked.intent.start_paused = true;
    lent.controls = &controls;
    lent.control_endpoint = &endpoint;
    watching.write_line = [](std::string_view) {};
    bool thrown = false;
    try
    {
        static_cast<void>(launch::run_file(asked, watching, lent, limits));
    }
    catch (int)
    {
        thrown = true;
    }
    require(thrown && endpoint.starts == 1, "what the start threw left run_file()");
    require(!controls.time().apply(resume_command(), "control_server").accepted,
            "the time commands ended with the run: a command after it is refused");
}

/// A request for a control server (RunHost::control) is the run's to read: without the caller's
/// controls it is refused as a mistake of the caller, and one that is wrong is a usage error
/// before anything is checked or told (in a build without the control server: refused, as there
/// is none to make). The file is not read: it does not exist.
void a_request_for_a_control_server_is_read_before_any_check(const fs::path& dir)
{
    vehicle_sil::runner::RunObservers watching;
    watching.write_line = [](std::string_view) {};
    launch::RunRequest asked = sim_of(dir);
    asked.host.control.emplace().listen = "not an address";
    {
        bool refused = false;
        try
        {
            static_cast<void>(launch::run_file(asked, watching));
        }
        catch (const std::invalid_argument& e)
        {
            refused = std::string(e.what()).find("RunLent::controls is null") != std::string::npos;
        }
        require(refused, "a control server without the caller's controls is refused, and the message says what to set");
    }
    vehicle_sil::runner::StopRequests stops;
    int told = 0;
    vehicle_sil::runner::RunControls::Observers observers;
    observers.on_phase = [&told](vehicle_sil::runner::Phase) { ++told; };
    observers.on_state = [&told](const vehicle_sil::runner::RunState&) { ++told; };
    observers.on_command = [&told](const vehicle_sil::runner::Command&, std::string_view) { ++told; };
    vehicle_sil::runner::RunControls controls(stops, false, std::move(observers), false);
    launch::RunLent lent;
    lent.controls = &controls;
    try
    {
        const launch::RunReport report = launch::run_file(asked, watching, lent);
        require(report.check.usage && !report.check.config && !report.run && report.check.findings.size() == 1,
                "a wrong address is a usage error of the run, and nothing ran");
        const config::Finding& finding = report.check.findings[0];
        require(finding.code == "usage" && finding.severity == config::Severity::Error &&
                    finding.message == "--control must be <port> (on 127.0.0.1; 0: any free port), <host>:<port> (a port from "
                                       "0 to 65535) or unix:<path>, not 'not an address'",
                "the finding says what an address is: " + finding.message);
        require(report.check.file == asked.host.config, "the report names the file");
    }
    catch (const std::invalid_argument& e)
    {
        require(std::string(e.what()).find("this build has no control server") != std::string::npos,
                std::string("a build without the control server says so: ") + e.what());
    }
    require(told == 0, "no phase, state or command was told: the check had not begun");
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
        {"an_ecu_without_variables_is_waited_for", [](const fs::path& ecu, const fs::path&, const fs::path& dir) { an_ecu_without_variables_is_waited_for(ecu, dir); }},
        {"a_mapping_that_does_not_fit_is_reported_for_a_hello_after_the_look_in_the_window",
         [](const fs::path& ecu, const fs::path&, const fs::path& dir) { a_late_hello(ecu, dir, true); }},
        {"a_mapping_that_does_not_fit_is_reported_for_a_hello_after_the_start",
         [](const fs::path& ecu, const fs::path&, const fs::path& dir) { a_late_hello(ecu, dir, false); }},
        {"the_start_is_limited_before_and_after_the_hold_but_not_in_it", the_start_is_limited_before_and_after_the_hold_but_not_in_it},
        {"the_ecus_wait_as_long_as_the_run_stands_still", the_ecus_wait_as_long_as_the_run_stands_still},
        {"timeout_counts_the_hold", timeout_counts_the_hold},
        {"an_interrupt_ends_the_hold", an_interrupt_ends_the_hold},
        {"a_resume_before_the_window_means_no_hold", a_resume_before_the_window_means_no_hold},
        {"a_refused_scenario_does_not_hold",
         [](const fs::path& ecu, const fs::path&, const fs::path& dir) { a_refused_scenario_does_not_hold(ecu, dir); }},
        {"the_control_server_is_started_after_the_runs_files_and_before_the_registry",
         [](const fs::path& ecu, const fs::path&, const fs::path& dir) {
             the_control_server_is_started_after_the_runs_files_and_before_the_registry(ecu, dir);
         }},
        {"what_leaves_run_file_between_the_stages_ends_the_time_commands",
         [](const fs::path& ecu, const fs::path&, const fs::path& dir) {
             what_leaves_run_file_between_the_stages_ends_the_time_commands(ecu, dir);
         }},
        {"a_request_for_a_control_server_is_read_before_any_check",
         [](const fs::path&, const fs::path&, const fs::path& dir) { a_request_for_a_control_server_is_read_before_any_check(dir); }},
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
