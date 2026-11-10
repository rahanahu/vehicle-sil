// SPDX-License-Identifier: MIT
//
// The time control of a SimulationController: the only part of the control server that depends
// on SIL Kit.
#include "vehicle_sil/control_grpc/server.hpp"

#include "vehicle_sil/core/failure_check.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

namespace vehicle_sil::control_grpc {
namespace {

class SimulationTimeControl final : public TimeControl
{
public:
    explicit SimulationTimeControl(SimulationController& c) : c_(c) { static_cast<void>(c_.time_state()); }

    TimeView state() const override
    {
        const auto s = c_.time_state();
        TimeView v;
        v.available = true;
        v.now = s.now;
        v.paused = s.paused;
        v.pause_target = s.pause_target;
        v.speed = s.speed;
        const bool finished = c_.runtime().started() && has_finished(PeerStatus{c_.runtime().state()});
        v.running = c_.runtime().started() && !finished;
        v.ended = finished;
        return v;
    }
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
        std::vector<std::pair<std::string, std::string>> out;
        // SIL Kit can miss a status that arrives while the handler is being registered.
        c_.runtime().refresh_peer_statuses();
        for (const auto& name : c_.required_participants())
        {
            const auto status = c_.runtime().peer_status(name);
            out.emplace_back(name, status.state ? SilKit::Services::Orchestration::to_string(*status.state) : std::string());
        }
        return out;
    }

    std::optional<LeftParticipant> left_participant() const override { return FailureCheck::left_participant(c_); }

private:
    using PeerStatus = vehicle_sil::PeerStatus;
    SimulationController& c_;
};

} // namespace

std::unique_ptr<TimeControl> time_control_of(SimulationController& controller)
{
    return std::make_unique<SimulationTimeControl>(controller);
}

} // namespace vehicle_sil::control_grpc
