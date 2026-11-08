// SPDX-License-Identifier: MIT
//
// vehicle_sil_vt_physics: an external physics simulator that takes part in the SIL Kit time
// synchronisation. A longitudinal vehicle model is integrated with a fixed 1 ms step (by
// default) and the vehicle state is published on the "VehicleState" topic every step. It runs on
// a vehicle_sil::Peer without networks: the application stage of each step does the work.
//
// The model uses integer arithmetic so that its output is bit-identical everywhere.
#include "../common/vt_peer.hpp"

#include <silkit/services/pubsub/all.hpp>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::vt;

/// Acceleration profile in mm/s^2: accelerate, cruise, brake, hold.
std::int64_t acceleration_mm_s2(SimTime t)
{
    if (t < SimTime{} + 400ms)
    {
        return 2500;
    }
    if (t < SimTime{} + 600ms)
    {
        return 0;
    }
    if (t < SimTime{} + 900ms)
    {
        return -1500;
    }
    return 0;
}

void print_usage()
{
    std::fputs("usage: vehicle_sil_vt_physics [options]\n", stderr);
    std::fputs(kCommonUsage, stderr);
    std::fputs("  --busy-us <us>                wall-clock work per step (simulates an expensive model)\n"
               "  --fail-at-ms <ms>             throw from the step at this time (simulates a failing model)\n",
               stderr);
}

int run(const Args& args)
{
    const CommonOptions o = parse_common(args, "Physics", 1ms);
    const auto busy = std::chrono::microseconds(args.u64("busy-us", 0, 10'000'000));
    const auto failAt = args.optional_millis("fail-at-ms");
    Trace trace(o.trace_path);

    // State at the start of the step; the step integrates over [now, now + step).
    std::int64_t speed_um_s = 0;  // micrometres per second
    std::int64_t distance_nm = 0; // nanometres
    const std::int64_t stepNs = o.step.count();

    Peer peer(peer_options(o));
    auto* publisher = peer.runtime().participant().CreateDataPublisher("VehicleStateOut", vehicle_state_spec(), 0);

    peer.on_step([&](SimTime now) {
        if (failAt && now >= SimTime{} + *failAt)
        {
            throw std::runtime_error("model failure requested by --fail-at-ms");
        }
        if (busy.count() > 0)
        {
            std::this_thread::sleep_for(busy);
        }
        const VehicleState state{static_cast<std::uint32_t>(speed_um_s / 1000), static_cast<std::uint64_t>(distance_nm / 1'000'000)};
        const auto payload = encode(state);
        publisher->Publish(SilKit::Util::Span<const std::uint8_t>(payload.data(), payload.size()));
        trace.line("state t_us=%lld speed=%u distance=%llu", us(now), state.speed_mm_s,
                   static_cast<unsigned long long>(state.distance_mm));

        // Explicit Euler with exact integer arithmetic (um/s * ns = 1e-15 m -> nm needs /1e6).
        const std::int64_t a = acceleration_mm_s2(now) * 1000; // um/s^2
        distance_nm += speed_um_s * stepNs / 1'000'000;
        speed_um_s += a * stepNs / 1'000'000'000;
        if (speed_um_s < 0)
        {
            speed_um_s = 0;
        }
    });
    return run_until_finished(peer, o);
}

} // namespace

int main(int argc, char** argv)
{
    return run_main(argc, argv, "physics", print_usage, run);
}
