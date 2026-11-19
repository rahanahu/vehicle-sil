// SPDX-License-Identifier: MIT
#include "run_scenario.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>

namespace vehicle_sil::launch::detail {

namespace {

namespace orch = SilKit::Services::Orchestration;

/// One step in words, as a line of the run's output says it when the step is decided:
/// "line 4: set ...: ok at 100 ms", with the step's message for one that failed or was skipped.
std::string scenario_step_text(const scenario::ScenarioStepRun& step)
{
    // The time as the result's table of the steps says it (check_words.hpp: millis_text(), as
    // scenario_text() writes it).
    return "line " + std::to_string(step.line) + ": " + step.text + ": " + scenario::step_result_text(step.status) +
           (step.at ? " at " + config::detail::millis_text(to_ns(*step.at)) + " ms" : std::string{}) +
           (step.status == scenario::StepStatus::Failed || step.status == scenario::StepStatus::Skipped ? " (" + step.message + ")" : std::string{});
}

} // namespace

/// The engine's way to the ECUs: the hub, and a wait until each agent has answered.
class RunScenario::HubPort final : public scenario::ScenarioPort
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
        // A fixed name: a Watch leaves it out by operation_clients_excluded (docs/control-api.md). Changing it
        // makes the GUI server's exclusion stop working without an error; control_server.launcher guards it.
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

RunScenario::RunScenario(const RunContext& ctx, RunOutput& out, RunControl& control, RunSamples& samples) :
  ctx_(ctx), out_(out), control_(control), samples_(samples)
{
    if (ctx_.scenario != nullptr)
    {
        for (const config::ParticipantConfig& p : ctx_.config.participants)
        {
            const auto used = std::find_if(ctx_.scenario->participants.begin(), ctx_.scenario->participants.end(),
                                           [&p](const scenario::ScenarioParticipant& s) { return s.name == p.name; });
            places_.push_back(used != ctx_.scenario->participants.end()
                                  ? std::optional<std::size_t>(static_cast<std::size_t>(used - ctx_.scenario->participants.begin()))
                                  : std::nullopt);
        }
    }
}

RunScenario::~RunScenario() = default;

void RunScenario::note_step(const scenario::ScenarioStepRun& step)
{
    if (step.status != scenario::StepStatus::Running)
    {
        out_.say("scenario: " + scenario_step_text(step));
    }
    out_.event([&step](RunEvents& e) { e.scenario_step(step); });
}

void RunScenario::drain()
{
    for (auto& [participant, sample] : samples_.take())
    {
        // RunSamples gives only the samples of the control agents begin() named.
        engine_->sample(*places_[participant], std::move(sample));
    }
}

void RunScenario::begin(control_silkit::HubTransport& transport, const std::function<bool()>& cancelled)
{
    const std::vector<control::PeerInfo> peers = control_.hub().peers();
    const std::vector<control_silkit::LinkInfo> links = transport.links();
    std::vector<scenario::EnginePeer> found(ctx_.scenario->participants.size());
    // Per participant of the configuration: the control agent whose samples the scenario gets.
    std::vector<std::string> incarnations(places_.size());
    {
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
                for (std::size_t place = 0; place < places_.size(); ++place)
                {
                    if (places_[place] == std::optional<std::size_t>(i))
                    {
                        incarnations[place] = peer->incarnation;
                    }
                }
            }
        }
    }
    port_ = std::make_unique<HubPort>(control_.hub());
    port_->give_up = cancelled;
    if (!ctx_.plan.timeline.empty())
    {
        record_ = std::make_shared<scenario::TimelineRecord>(*ctx_.scenario);
    }
    engine_ = std::make_unique<scenario::ScenarioEngine>(*ctx_.scenario, step_,
                                                        *port_,
                                                        [this](const scenario::ScenarioStepRun& step) { note_step(step); }, record_.get());
    // From here on the scenario gets the samples of these agents, and none that came before.
    samples_.scenario_begins(std::move(incarnations));
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
        outcome.scenario.error = "the scenario could not begin before the simulation started";
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
            outcome.scenario.error = thread_error_;
        }
    }
    if (!engine_)
    {
        outcome.scenario.result->status = scenario::ScenarioStatus::Incomplete;
        return;
    }
    // Whatever is taken from here on is not the scenario's (below: once more, for a stop_at_ms).
    const struct Ends
    {
        RunSamples& samples;
        ~Ends() { samples.scenario_ends(); }
    } ends{samples_};
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
    const std::string file = outcome.scenario.result->file;
    outcome.scenario.result = engine_->run();
    outcome.scenario.result->file = file;
    outcome.scenario.timeline_record = record_;
}

void RunScenario::end()
{
    quit_.store(true);
    if (thread_.joinable())
    {
        thread_.join();
    }
    samples_.scenario_ends();
}

} // namespace vehicle_sil::launch::detail
