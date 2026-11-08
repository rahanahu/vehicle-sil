// SPDX-License-Identifier: MIT
//
// vehicle_sil_vt_lin_sensor: a LIN slave sensor in virtual time, on a vehicle_sil::Peer. It
// derives a distance reading from the vehicle state published by the physics peer and sets it
// as its LIN response every step (2 ms by default); vehicle_sil::lin::LinSlave answers the
// master's headers with it.
#include "../common/vt_peer.hpp"

#include "vehicle_sil/lin/lin_slave.hpp"


#include <array>
#include <chrono>
#include <cstdio>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::vt;
using namespace vehicle_sil::lin;

constexpr LinId kDistanceId = 0x20;
constexpr LinId kStatusId = 0x21;

void print_usage()
{
    std::fputs("usage: vehicle_sil_vt_lin_sensor [options]\n", stderr);
    std::fputs(kCommonUsage, stderr);
    std::fputs("  --network <name>              SIL Kit LIN network (VT_LIN)\n", stderr);
}

int run(const Args& args)
{
    const CommonOptions o = parse_common(args, "LinSensor", 2ms);
    const std::string network = args.str("network", "VT_LIN");

    // Used by the steps and SIL Kit handlers: declared before the peer, so they outlive it.
    Trace trace(o.trace_path);
    StepInbox<VehicleState> vehicleStates(1024);
    VehicleState latest{};
    std::uint8_t counter = 0;
    // Overflows that matter: those seen by a handled step. After --end-ms nothing drains the
    // inbox, so it may fill up while other peers keep publishing.
    std::uint64_t overflows = 0;

    Peer peer(peer_options(o));
    subscribe_vehicle_state(peer.runtime(), vehicleStates);
    auto& slave = peer.lin_slave(network);
    // The application runs before the slave's step, so the responses set here hold from this
    // step's time on.
    peer.on_step([&](SimTime now) {
        overflows = vehicleStates.overflows();
        vehicleStates.drain_before(now, [&](SimTime, VehicleState& state) { latest = state; });
        // Distance in cm (16 bit) and a rolling counter, as a sensor would report them.
        const auto distanceCm = static_cast<std::uint16_t>(latest.distance_mm / 10);
        const std::array<std::uint8_t, 3> distance{static_cast<std::uint8_t>(distanceCm & 0xFFu),
                                                   static_cast<std::uint8_t>(distanceCm >> 8), counter};
        slave.set_response(now, kDistanceId, distance);
        const std::array<std::uint8_t, 1> status{static_cast<std::uint8_t>(latest.speed_mm_s > 0 ? 0x01 : 0x00)};
        slave.set_response(now, kStatusId, status);
        trace.line("set t_us=%lld distance_cm=%u counter=%u moving=%u", us(now), distanceCm, counter, status[0]);
        ++counter;
    });

    const int result = run_until_finished(peer, o);
    const auto stats = slave.statistics();
    log(o.participant, "responses=%llu ignored_headers=%llu physics_overflows=%llu",
        static_cast<unsigned long long>(stats.responses_sent), static_cast<unsigned long long>(stats.headers_ignored),
        static_cast<unsigned long long>(overflows));
    if (result == kExitOk && overflows != 0)
    {
        return kExitFailed;
    }
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    return run_main(argc, argv, "lin-sensor", print_usage, run);
}
