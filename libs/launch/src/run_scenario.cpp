// SPDX-License-Identifier: MIT
#include "run_scenario.hpp"

#include "check_format.hpp"

#include "vehicle_sil/launch/events.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>

namespace vehicle_sil::launch::detail {

namespace {

namespace orch = SilKit::Services::Orchestration;

} // namespace

/// The engine's way to the ECUs: the hub, and a wait until each agent has answered.
class RunScenario::HubPort final : public ScenarioPort
{
public:
    explicit HubPort(control::Hub& hub) : hub_(hub) {}
    /// Ends the wait of write() (the run is ending). Set while no write runs.
    std::function<bool()> give_up{};

    std::uint64_t write(std::vector<control::PeerWrite> writes, SimTime at) override
    {
        control::OperationRequest request;
        request.writes = std::move(writes);
        request.at = at;
        request.late = control::LatePolicy::Reject;
        request.client = "scenario";
        const std::uint64_t seq = hub_.write(std::move(request));
        // No limit in real time: an agent that is gone is settled by the hub, and a run that
        // ends gives up.
        while (!hub_.wait_queued(seq, std::chrono::milliseconds(20)) && hub_.operation(seq) && !(give_up && give_up()))
        {
        }
        return seq;
    }
    std::optional<control::OperationResult> result(std::uint64_t ticket) override { return hub_.operation(ticket); }

private:
    control::Hub& hub_;
};

RunScenario::RunScenario(const RunContext& ctx, RunOutput& out, RunControl& control) : ctx_(ctx), out_(out), control_(control) {}

RunScenario::~RunScenario() = default;

void RunScenario::note_step(const ScenarioStepRun& step)
{
    if (step.status != StepStatus::Running)
    {
        out_.say("scenario: " + detail::scenario_step_text(step));
    }
    out_.event([&step](EventStream& e) { e.scenario_step(step); });
}

void RunScenario::tap(std::chrono::milliseconds step)
{
    step_ = step;
    samples_.incarnations.assign(ctx_.scenario->participants.size(), std::string{});
    tapped_ = true;
    control_.hub().set_values_tap([this](const std::string& participant, const std::string& incarnation,
                                const std::shared_ptr<const control::Snapshot>& sample) {
        const std::lock_guard lock(samples_.mutex);
        for (std::size_t i = 0; i < ctx_.scenario->participants.size(); ++i)
        {
            if (ctx_.scenario->participants[i].name == participant && samples_.incarnations[i] == incarnation)
            {
                samples_.taken.emplace_back(i, sample);
                return;
            }
        }
    });
}

void RunScenario::drain()
{
    std::vector<std::pair<std::size_t, std::shared_ptr<const control::Snapshot>>> taken;
    {
        const std::lock_guard lock(samples_.mutex);
        taken.swap(samples_.taken);
    }
    for (auto& [participant, sample] : taken)
    {
        engine_->sample(participant, std::move(sample));
    }
}

void RunScenario::begin(control_silkit::HubTransport& transport, const std::function<bool()>& cancelled)
{
    const std::vector<control::PeerInfo> peers = control_.hub().peers();
    const std::vector<control_silkit::LinkInfo> links = transport.links();
    std::vector<EnginePeer> found(ctx_.scenario->participants.size());
    {
        const std::lock_guard lock(samples_.mutex);
        for (std::size_t i = 0; i < found.size(); ++i)
        {
            const std::string& name = ctx_.scenario->participants[i].name;
            const auto peer = std::find_if(peers.begin(), peers.end(), [&name](const control::PeerInfo& info) {
                return info.participant == name && info.connected;
            });
            const auto link = std::find_if(links.begin(), links.end(), [&name](const control_silkit::LinkInfo& info) {
                return info.participant == name && info.state == control_silkit::LinkState::Connected && info.ready;
            });
            if (peer != peers.end() && link != links.end())
            {
                found[i].connected = true;
                found[i].schema = peer->schema;
                samples_.incarnations[i] = peer->incarnation;
            }
        }
    }
    port_ = std::make_unique<HubPort>(control_.hub());
    port_->give_up = cancelled;
    engine_ = std::make_unique<ScenarioEngine>(*ctx_.scenario, step_,
                                                        *port_,
                                                        [this](const ScenarioStepRun& step) { note_step(step); });
    engine_->begin(found);
    port_->give_up = [this] { return quit_.load(); };
}

void RunScenario::start(SimulationController& controller, RunOutcome& outcome)
{
    if (ctx_.scenario == nullptr)
    {
        return;
    }
    if (!engine_)
    {
        // The window ended before the scenario began (the start was being given up): nothing
        // moves the guard, so the simulation is stopped where it stands.
        outcome.scenario_error = "the scenario could not begin before the simulation started";
        over_.store(true);
        return;
    }
    thread_ = std::thread([this, &controller] { loop(controller); });
}

void RunScenario::loop(SimulationController& controller)
{
    try
    {
        while (!quit_.load())
        {
            const std::optional<SimTime> p = controller.wait_guarded(std::chrono::milliseconds(50));
            if (!p)
            {
                // No step is held (yet), or the simulation is ending: the main thread says when to end.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            drain();
            if (engine_->tick(*p))
            {
                // Over: the guard stays, so that the simulation stands at this step until the
                // main thread has stopped it.
                over_.store(true);
                return;
            }
            controller.guard_from(*p + SimDuration(1));
        }
    }
    catch (const std::exception& e)
    {
        const std::lock_guard lock(error_mutex_);
        thread_error_ = *e.what() != '\0' ? e.what() : "exception without message";
        over_.store(true);
    }
}

void RunScenario::finish(SimulationController& controller, const WatchResult& watched, bool for_scenario, RunOutcome& outcome)
{
    if (ctx_.scenario == nullptr)
    {
        return;
    }
    quit_.store(true);
    if (thread_.joinable())
    {
        thread_.join();
    }
    {
        const std::lock_guard lock(error_mutex_);
        if (!thread_error_.empty())
        {
            outcome.scenario_error = thread_error_;
        }
    }
    if (!engine_)
    {
        outcome.scenario->status = ScenarioStatus::Incomplete;
        return;
    }
    if (!engine_->over())
    {
        const bool stoppedAt = !for_scenario && watched.end == WatchEnd::Ended &&
                               watched.state == orch::ParticipantState::Shutdown && ctx_.config.stop_at_ms &&
                               controller.wall_time_to_stop_at().has_value();
        if (stoppedAt)
        {
            // The controller's last step: the samples of the steps before it are all there.
            drain();
            engine_->stopped_at(controller.time_state().now);
        }
        else
        {
            engine_->cut_short();
        }
    }
    if (watched.end == WatchEnd::Failed)
    {
        // A participant failed: whatever the scenario made of it (a write to it that failed),
        // the run's result is the participant's failure and a scenario that did not finish.
        engine_->superseded();
    }
    const std::string file = outcome.scenario->file;
    outcome.scenario = engine_->run();
    outcome.scenario->file = file;
}

void RunScenario::end()
{
    quit_.store(true);
    if (thread_.joinable())
    {
        thread_.join();
    }
    if (tapped_)
    {
        control_.hub().set_values_tap({});
        tapped_ = false;
    }
}

} // namespace vehicle_sil::launch::detail
