// SPDX-License-Identifier: MIT
//
// A Spy does not allocate memory in steady-state steps: its watchers' handlers, its clock and
// the getters of its variables. This executable counts every allocation of the process
// (allocation_counter.hpp), so it is built on its own.
#include "allocation_counter.hpp"
#include "spy_rig.hpp"

#include "vehicle_sil/spy/spy.hpp"

#include <cstdint>

using namespace vehicle_sil::unit::spy;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::ReceivedFrame;
using vehicle_sil::j1939::SniffedTransfer;
using vehicle_sil::spy::Scope;
using vehicle_sil::spy::Spy;

namespace {

/// Every kind of handler, a step function, a pointer variable and a getter that reads
/// Scope::now().
class Busy
{
public:
    explicit Busy(Scope& s) : scope_(s)
    {
        s.on_message(vehicle_sil::j1939::Pgn{kPgnPropA}, [this](const Message& m) {
            if (m.time >= latest_)
            {
                latest_ = m.time;
                ++count_;
            }
        });
        s.on_any_message([this](const Message& m) { bytes_ = static_cast<int>(m.data.size()); });
        s.on_transfer([this](const SniffedTransfer&) { ++transfers_; });
        s.on_frame([this](const ReceivedFrame&) { ++frames_; });
        s.on_step([this](SimTime now) { quiet_steps_ += now - latest_ > std::chrono::milliseconds(5) ? 1 : 0; });
        s.output("count", &count_);
        s.output("bytes", &bytes_);
        s.output("transfers", &transfers_);
        s.output("frames", &frames_);
        s.output("silent_ms", [this] { return std::chrono::duration_cast<std::chrono::milliseconds>(scope_.now() - latest_).count(); });
    }

private:
    Scope& scope_;
    SimTime latest_{};
    std::int64_t count_ = 0;
    int bytes_ = 0;
    std::int64_t transfers_ = 0;
    std::int64_t frames_ = 0;
    std::int64_t quiet_steps_ = 0;
};

} // namespace

TEST_CASE(steady_state_steps_of_a_spy_do_not_allocate)
{
    Rig rig;
    rig.ecu.set_agent_options({.sample_period = std::chrono::milliseconds(0)}); // sample every step
    Spy& spy = rig.ecu.emplace<Spy>(rig.ecu, rig.network);
    spy.add<Busy>("busy");
    const Bytes message = payload(20);
    std::uint8_t k = 0;
    // Each step: two single frames and a connection between two other ECUs (RTS/CTS).
    const auto inject_traffic = [&](SimTime t) {
        rig.inject(t + std::chrono::milliseconds(1), frame(6, kPgnPropA, 0x21, 0x22, {k++}));
        rig.inject(t + std::chrono::milliseconds(2), frame(6, kPgnPropB, 0x23, kGlobal, {1, 2}));
        SimTime c = t + std::chrono::milliseconds(3);
        rig.inject(c, rts(0x21, 0x22, 20, kPgnPropA));
        rig.inject(c += std::chrono::milliseconds(1), cts(0x22, 0x21, 3, 1, kPgnPropA));
        for (const auto& f : dts(0x21, 0x22, message, 1, 3))
        {
            rig.inject(c += std::chrono::milliseconds(1), f);
        }
        rig.inject(c += std::chrono::milliseconds(1), eoma(0x22, 0x21, 20, kPgnPropA));
    };
    SimTime t = at(std::chrono::milliseconds(0));
    for (int i = 0; i < 5; ++i) // warm up: the agent's and the bus's buffers
    {
        inject_traffic(t);
        t += std::chrono::milliseconds(10);
        rig.bench.run_until(t);
    }
    std::size_t counted = 0;
    constexpr int kSteps = 100;
    for (int i = 0; i < kSteps; ++i)
    {
        inject_traffic(t);
        t += std::chrono::milliseconds(10);
        g_allocations.store(0);
        g_counting.store(true);
        rig.bench.run_until(t);
        g_counting.store(false);
        counted += g_allocations.load();
    }
    CHECK_EQ(counted, 0u);
    CHECK(integer_of(rig.ecu, "busy.count") > kSteps);
    CHECK(integer_of(rig.ecu, "busy.transfers") > kSteps);
    CHECK_EQ(integer_of(rig.ecu, "spy.missed"), 0);
}
