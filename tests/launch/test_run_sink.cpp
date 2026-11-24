// SPDX-License-Identifier: MIT
//
// Custom RunEventSink decoupled from EventStream: verifies that run_file() and RunLent::events
// compile and drive a minimal RunEventSink implementation without including events.hpp.

#include "test_harness.hpp"

#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/launch/run_sink.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using vehicle_sil::launch::Checked;
using vehicle_sil::launch::ControlEndpoint;
using vehicle_sil::launch::RunEventSink;
using vehicle_sil::launch::RunLent;
using vehicle_sil::launch::RunReport;
using vehicle_sil::launch::RunRequest;
using vehicle_sil::launch::run_file;
using vehicle_sil::runner::Command;
using vehicle_sil::runner::ControlServerInfo;
using vehicle_sil::runner::ParticipantState;
using vehicle_sil::runner::Phase;
using vehicle_sil::runner::RunControls;
using vehicle_sil::runner::RunState;
using vehicle_sil::runner::StopRequests;
using vehicle_sil::runner::TimeEvent;

struct RecordingSink final : RunEventSink
{
    struct RecordedCheck
    {
        std::string file;
        bool passed = false;
    };

    std::vector<Phase> phases;
    std::vector<RecordedCheck> checks;
    std::vector<std::string> commands;
    std::vector<RunState> states;

    void phase(Phase p) override { phases.push_back(p); }

    void check(const Checked& report) override { checks.push_back({report.file, report.passed()}); }

    void command(const Command& cmd, std::string_view source) override
    {
        commands.push_back(std::string(source) + ":" + cmd.name.value_or("") + ":" +
                           (cmd.accepted ? "accepted" : cmd.reason));
    }

    void state(const RunState& s) override { states.push_back(s); }

    void registry(const std::string&) override {}
    void control_server(const std::string&, bool, bool) override {}
    void participant(const std::string&, ParticipantState) override {}
    void participant_not_started(const std::string&, const std::string&) override {}
    void participant_failed(const std::string&, std::string_view, const std::string&) override {}
    void participant_started(const std::string&, long, const std::vector<std::string>&, const std::string&) override {}
    void participant_ended(const std::string&, const vehicle_sil::process::ExitStatus&) override {}
    void participant_manual(const std::string&, const std::vector<std::string>&, const std::string&, const std::string&,
                            const std::vector<std::pair<std::string, std::string>>&) override
    {
    }
    void control_link(const std::string&, std::string_view, std::optional<std::size_t>, std::optional<int>) override {}
    void scenario_step(const vehicle_sil::scenario::ScenarioStepRun&) override {}
    void log(const std::string&, const std::string&) override {}
    void outside(const std::string&, std::uint64_t, std::uint64_t) override {}
    void finding(const std::string&, const vehicle_sil::config::Finding&) override {}
    void time(const TimeEvent&) override {}
    [[nodiscard]] std::size_t waiting_events() const override { return 0; }
};

struct StubControlEndpoint final : ControlEndpoint
{
    ControlServerInfo start(std::shared_ptr<vehicle_sil::control::Hub>) override { return {}; }
    void end() override {}
};

RunRequest request_of(const std::string& file)
{
    RunRequest request;
    request.host.config = file;
    return request;
}

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

TEST_CASE(run_file_with_own_controls_drives_a_custom_run_event_sink_without_event_stream)
{
    // Subcase A: plain OwnControls
    {
        RecordingSink sink;
        RunLent lent;
        lent.events = &sink;
        const RunReport report = run_file(request_of("no-such-directory/sim.yaml"), {}, lent);
        CHECK(!report.run.has_value());
        CHECK((sink.phases == std::vector<Phase>{Phase::Checking, Phase::Ended}));
        CHECK_EQ(sink.checks.size(), std::size_t{1});
        CHECK(sink.checks.front().file == "no-such-directory/sim.yaml");
        CHECK(!sink.checks.front().passed);
        CHECK(sink.states.size() >= std::size_t{2});
        CHECK(sink.states.front().phase == Phase::Checking);
        CHECK(sink.states.back().phase == Phase::Ended);
        CHECK(sink.states.back().allowed.write.reason == "no-control-server");
        CHECK(!sink.states.back().scenario);
        CHECK(sink.commands.empty());
    }

    // Subcase B: OwnControls with scenario and lent ControlEndpoint
    {
        RecordingSink sink;
        StubControlEndpoint endpoint;
        RunRequest asked = request_of("no-such-directory/sim.yaml");
        asked.intent.scenario = "drive.scenario.yaml";
        RunLent lent;
        lent.events = &sink;
        lent.control_endpoint = &endpoint;
        const RunReport report = run_file(asked, {}, lent);
        CHECK(!report.run.has_value());
        CHECK((sink.phases == std::vector<Phase>{Phase::Checking, Phase::Ended}));
        CHECK(sink.states.size() >= std::size_t{2});
        CHECK(sink.states.front().scenario);
        CHECK(sink.states.front().allowed.write.reason == "scenario-running");
    }
}

TEST_CASE(run_file_with_lent_controls_forwards_check_and_refuses_missing_observers_for_custom_sink)
{
    StopRequests stops;

    // Missing-observer refusal against a RunEventSink*
    {
        RecordingSink sink;
        RunEventSink* events = &sink;
        const auto try_run = [&](RunControls& controls) {
            RunLent lent;
            lent.controls = &controls;
            lent.events = events;
            return refusal([&] { static_cast<void>(run_file(request_of("no-such-directory/sim.yaml"), {}, lent)); });
        };

        RunControls none(stops);
        CHECK(try_run(none).find("on_phase") != std::string::npos);

        RunControls::Observers only_phase;
        only_phase.on_phase = [](Phase) {};
        RunControls phase_only(stops, false, std::move(only_phase), false);
        CHECK(try_run(phase_only).find("on_command") != std::string::npos);

        RunControls::Observers no_state;
        no_state.on_phase = [](Phase) {};
        no_state.on_command = [](const Command&, std::string_view) {};
        RunControls without_state(stops, false, std::move(no_state), false);
        CHECK(try_run(without_state).find("on_state") != std::string::npos);

        events = nullptr;
        CHECK(try_run(none).empty());
    }

    // Full observer forwarding with pending pause command
    {
        RecordingSink sink;
        RunControls::Observers observers;
        observers.on_phase = [&sink](Phase p) { sink.phase(p); };
        observers.on_command = [&sink](const Command& c, std::string_view src) { sink.command(c, src); };
        observers.on_state = [&sink](const RunState& s) { sink.state(s); };
        RunControls controls(stops, false, std::move(observers), false);

        Command pause;
        pause.name = "pause";
        const Command pending = controls.time().apply(pause, "stdin");
        CHECK(pending.pending);
        CHECK(sink.commands.empty());

        RunLent lent;
        lent.controls = &controls;
        lent.events = &sink;
        const RunReport report = run_file(request_of("no-such-directory/sim.yaml"), {}, lent);
        CHECK(!report.run.has_value());
        CHECK_EQ(sink.checks.size(), std::size_t{1});
        CHECK(!sink.checks.front().passed);
        CHECK((sink.phases == std::vector<Phase>{Phase::Checking, Phase::Ended}));
        CHECK((sink.commands == std::vector<std::string>{"stdin:pause:not-running"}));
        CHECK(!sink.states.empty());
        CHECK(sink.states.back().phase == Phase::Ended);
    }
}
