// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch's run as a library (run_file()) with a hub of the caller and ECUs in their
// own processes (run_test_ecu.cpp, behaviour "variables"): a parameter written in the window
// before the start is accepted by every ECU and seen by its step at 0; when the simulation has
// reached stop_at_ms, the hub has the values of the last steps before it; nothing arrived late.
//
// usage: vehicle_sil_launch_control_test <test ECU executable> <scratch directory>
#include "topics.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control/wire.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <silkit/SilKit.hpp>
#include <silkit/services/pubsub/all.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
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
        queued = h.wait_queued(seq, 30s);
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
        require(p.control_variables == 7, p.name + " offers " + std::to_string(p.control_variables) + " variables, not 7");
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
            impostor = SilKit::CreateParticipant(SilKit::Config::ParticipantConfigurationFromString("{}"), "Impostor",
                                                 std::string(line.substr(head.size())));
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

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: %s <test ECU executable> <scratch directory>\n", argc > 0 ? argv[0] : "launch_control_test");
        return 2;
    }
    try
    {
        const fs::path ecu = process::path_from_utf8(argv[1]);
        const fs::path dir = process::path_from_utf8(argv[2]);
        fs::remove_all(dir);
        fs::create_directories(dir);
        for (int round = 1; round <= 5; ++round)
        {
            one_run(ecu, dir, round);
            std::printf("round %d ok\n", round);
            std::fflush(stdout);
        }
        failing_start(ecu, dir);
        std::printf("failing start ok\n");
        hello_without_an_answer(ecu, dir);
        std::printf("hello without an answer ok\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
