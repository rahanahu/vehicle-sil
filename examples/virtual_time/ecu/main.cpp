// SPDX-License-Identifier: MIT
//
// vehicle_sil_vt_ecu: hosts an existing ECU model (the legacy_ecu shared library with a C
// interface and a 10 ms tick()) in SIL Kit virtual time, with J1939 on a vehicle_sil::Peer.
//
// The J1939 node claims its address in the first step and owns it 250 ms later. Per step
// (10 ms by default), after the J1939 network has handled what arrived before the step:
//   1. take the vehicle states the physics peer published before this step (StepInbox),
//   2. run the ECU's tick() when it is due and send its CCVS output on J1939 once the address
//      is claimed; the frame leaves at the end of the step, stamped with the step's time.
// The step blocks the time synchronisation, so a slow tick() holds back every other
// participant instead of being skipped.
#include "../common/vt_peer.hpp"

#include "legacy_ecu.h"
#include "vehicle_sil/j1939/network.hpp"

#include <chrono>
#include <cstdio>
#include <span>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::vt;

constexpr j1939::Pgn kPgnCcvs{0xFEF1};

const char* state_name(j1939::AddressState state)
{
    switch (state)
    {
    case j1939::AddressState::Fixed:
        return "fixed";
    case j1939::AddressState::NotStarted:
        return "not_started";
    case j1939::AddressState::Claiming:
        return "claiming";
    case j1939::AddressState::Claimed:
        return "claimed";
    case j1939::AddressState::CannotClaim:
        return "cannot_claim";
    }
    return "?";
}

void print_usage()
{
    std::fputs("usage: vehicle_sil_vt_ecu [options]\n", stderr);
    std::fputs(kCommonUsage, stderr);
    std::fputs("  --network <name>              SIL Kit CAN network (VT_CAN)\n"
               "  --source-address <sa>         J1939 source address to claim (0x80)\n"
               "  --tick-cost-us <us>           wall-clock time each tick() takes (simulates a slow ECU model)\n",
               stderr);
}

int run(const Args& args)
{
    const CommonOptions o = parse_common(args, "Ecu", 10ms);
    const std::string network = args.str("network", "VT_CAN");
    const auto sourceAddress = static_cast<j1939::Address>(args.u64("source-address", 0x80, 0xFD));
    const auto tickCost = std::chrono::microseconds(args.u64("tick-cost-us", 0, 10'000'000));
    // The ECU's tick() runs every LEGACY_ECU_TICK_PERIOD_MS, whatever the participant's step.
    const SimDuration tickPeriod = std::chrono::milliseconds(LEGACY_ECU_TICK_PERIOD_MS);
    if (!on_step_grid(tickPeriod, o.step))
    {
        throw UsageError("--step-us must divide the ECU tick period of " + std::to_string(LEGACY_ECU_TICK_PERIOD_MS) + " ms");
    }

    // Used by the steps and SIL Kit handlers: declared before the peer, so they outlive it.
    Trace trace(o.trace_path);
    StepInbox<VehicleState> vehicleStates(1024);
    VehicleState latest{};
    // Overflows that matter: those seen by a handled step. After --end-ms nothing drains the
    // inbox, so it may fill up while other peers keep publishing.
    std::uint64_t physicsOverflows = 0;
    legacy_ecu_init();

    Peer peer(peer_options(o));
    subscribe_vehicle_state(peer.runtime(), vehicleStates);
    J1939ChannelOptions channel;
    channel.controller_name = "J1939CAN";
    auto& bus = peer.j1939(network, channel);
    auto& ecu = bus.add_node({sourceAddress, j1939::Name{}
                                                 .set_identity_number(0x2345)
                                                 .set_manufacturer_code(0x7F0)
                                                 .set_function(0) // engine
                                                 .set_industry_group(0)});
    ecu.on_address_change([&](const j1939::AddressChange& change) {
        // claim: the step that sent Address Claimed; claimed: the end of the wait after it.
        const char* record = change.state == j1939::AddressState::Claiming  ? "claim"
                             : change.state == j1939::AddressState::Claimed ? "claimed"
                                                                            : "address";
        trace.line("%s t_ms=%lld address=0x%02X state=%s", record, ms(change.time), change.address,
                   state_name(change.state));
    });

    peer.on_step([&](SimTime now) {
        physicsOverflows = vehicleStates.overflows();
        vehicleStates.drain_before(now, [&](SimTime, VehicleState& state) { latest = state; });
    });
    peer.every(tickPeriod, [&](SimTime now) {
        legacy_ecu_set_wheel_speed(latest.speed_mm_s);
        if (tickCost.count() > 0)
        {
            std::this_thread::sleep_for(tickCost); // a slow model: real work would go here
        }
        legacy_ecu_tick();
        trace.line("tick t_ms=%lld n=%u speed_in=%u", ms(now), legacy_ecu_tick_count(), latest.speed_mm_s);
        std::uint8_t ccvs[8];
        if (legacy_ecu_take_ccvs(ccvs))
        {
            const bool sent = ecu.address_state() == j1939::AddressState::Claimed &&
                              ecu.send(kPgnCcvs, std::span<const std::uint8_t>(ccvs)) == j1939::SendStatus::Ok;
            trace.line("ccvs t_ms=%lld raw=%u sent=%d", ms(now), static_cast<unsigned>(ccvs[1] | (ccvs[2] << 8)),
                       sent ? 1 : 0);
        }
    });

    const int result = run_until_finished(peer, o);
    // The network's statistics change only in steps, so they are those of the handled steps.
    const auto& stats = bus.statistics();
    const std::uint64_t canLost = stats.receive_overflows + stats.port_receive_overflows + stats.send_failures +
                                  stats.transmit_failures + stats.send_queue_full;
    log(o.participant,
        "ticks=%u can_tx=%llu can_rx=%llu rx_overflows=%llu tx_failures=%llu physics_overflows=%llu",
        legacy_ecu_tick_count(), static_cast<unsigned long long>(stats.frames_sent),
        static_cast<unsigned long long>(stats.frames_received),
        static_cast<unsigned long long>(stats.receive_overflows + stats.port_receive_overflows),
        static_cast<unsigned long long>(stats.send_failures + stats.transmit_failures),
        static_cast<unsigned long long>(physicsOverflows));
    if (result == kExitOk && (canLost != 0 || physicsOverflows != 0))
    {
        log(o.participant, "FAIL: lost or failed frames");
        return kExitFailed;
    }
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    return run_main(argc, argv, "ecu", print_usage, run);
}
