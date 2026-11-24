// SPDX-License-Identifier: MIT
//
// The time control of a SimulationController: the only part of the control server that depends
// on SIL Kit.
#include "vehicle_sil/control_grpc/server.hpp"

#include "vehicle_sil/core/failure_check.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>
#include <mutex>

namespace vehicle_sil::control_grpc {
namespace {

class SimulationTimeControl final : public TimeControl
{
public:
    explicit SimulationTimeControl(SimulationController& c) : c_(c) { static_cast<void>(c_.time_state()); }

    TimeView state() const override { return detail::time_view_of(c_); }
    void pause(std::optional<SimTime> at) override { at ? c_.pause_at(*at) : c_.pause(); }
    void resume() override { c_.resume(); }
    void run_until(SimTime t) override { c_.run_until(t); }
    void step(SimDuration d) override { c_.step(d); }
    void set_speed(std::optional<double> speed) override { c_.set_speed(speed); }
    std::optional<SimTime> wait_paused(std::chrono::milliseconds timeout) override { return c_.wait_until_paused(timeout); }
    // Asks to stop without waiting; thread safe (the controller's own stop() is neither).
    void stop(const std::string& reason) override { c_.runtime().request_stop(reason); }

    std::vector<std::pair<std::string, std::string>> participants() const override
    {
        // SIL Kit can miss a status that arrives while the runtime's status handler is being
        // registered (in its constructor, long before this); one re-read afterwards finds it.
        // Later statuses reach that handler. Not on every read: each re-read registers a
        // handler and replays every status under SIL Kit's lock, and this runs every period
        // and on every time call.
        std::call_once(refreshed_, [this] { c_.runtime().refresh_peer_statuses(); });
        return detail::simulation_participants_of(c_);
    }

    std::optional<LeftParticipant> left_participant() const override { return FailureCheck::left_participant(c_); }

private:
    SimulationController& c_;
    mutable std::once_flag refreshed_;
};

} // namespace

namespace detail {

TimeView time_view_of(SimulationController& controller, bool holding)
{
    TimeView v;
    v.available = true;
    const TimeState s = controller.time_state();
    v.speed = s.speed;
    const auto state = controller.runtime().state();
    const bool finished = controller.runtime().started() && has_finished(PeerStatus{state});
    if (holding && !finished)
    {
        // Before the first step: it stands still at 0, and has not started.
        v.paused = true;
        return v;
    }
    v.now = holding ? SimTime{} : s.now;
    v.pause_target = s.pause_target;
    // One answer from two looks: a simulation that ended between them stands still nowhere (as
    // time_state() says of one that had ended before).
    v.paused = s.paused && !finished;
    v.running = !finished && state == SilKit::Services::Orchestration::ParticipantState::Running;
    v.ended = finished;
    if (finished)
    {
        // Where it ended, as the result's sim_ms counts it, and no pause target remains.
        v.now = std::max(v.now, controller.runtime().now());
        v.pause_target.reset();
    }
    return v;
}

std::vector<std::pair<std::string, std::string>> simulation_participants_of(SimulationController& controller)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& name : controller.required_participants())
    {
        const auto status = controller.runtime().peer_status(name);
        out.emplace_back(name, status.state ? SilKit::Services::Orchestration::to_string(*status.state) : std::string());
    }
    return out;
}

} // namespace detail

std::unique_ptr<TimeControl> time_control_of(SimulationController& controller)
{
    return std::make_unique<SimulationTimeControl>(controller);
}

} // namespace vehicle_sil::control_grpc
