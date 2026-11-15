// SPDX-License-Identifier: MIT
//
// The analysis example of the guide (guide_analysis.hpp): a watcher that judges each request of
// one ECU by the answer of another, on an ECU of a LocalBench that hears both.
#include "guide_analysis.hpp"
#include "spy_rig.hpp"

#include "vehicle_sil/j1939/codec.hpp"

#include <cstdint>

using namespace vehicle_sil::unit::spy;
using vehicle_sil::Ecu;
using vehicle_sil::LocalBench;
using vehicle_sil::SimTime;
namespace codec = vehicle_sil::j1939::codec;

namespace {

constexpr Address kTool = 0xF9;
constexpr Address kEcu = 0x21;

/// The ECU of the test: a Spy with the example's watcher, as the guide adds it.
class CalCheck
{
public:
    explicit CalCheck(Ecu& ecu) : spy_(ecu, ecu.j1939(kBus)) { spy_.add<WriteCheck>("check", 200ms); }

private:
    vehicle_sil::spy::Spy spy_;
};

template <typename M>
Bytes bytes_of(const M& message)
{
    const auto bytes = codec::message_bytes(message);
    return Bytes(bytes.begin(), bytes.end());
}

Bytes write(std::uint16_t address, double value)
{
    cal::Write w;
    w.address.set(address);
    w.value.set(value);
    return bytes_of(w);
}

Bytes answer(std::uint16_t address, double value)
{
    cal::WriteAnswer a;
    a.address.set(address);
    a.result.set(cal::WriteAnswer::Result::ok);
    a.value.set(value);
    return bytes_of(a);
}

} // namespace

TEST_CASE(analysis_example_judges_each_write_by_its_answer)
{
    LocalBench bench;
    Ecu& ecu = bench.add_ecu("CalCheck", 5ms);
    ecu.set_agent_options({.sample_period = std::chrono::milliseconds(0)}); // sample every step
    ecu.emplace<CalCheck>(ecu);
    const auto inject = [&](SimTime t, const J1939Frame& f) {
        CHECK(bench.inject(kBus, t, to_can(f)) == vehicle_sil::can::SendResult::Accepted);
    };
    const auto check = [&](std::string_view item) { return integer_of(ecu, std::string("check.") + std::string(item)); };
    bench.run_until(at(5ms));
    CHECK_EQ(check("answered"), 0);
    CHECK_EQ(check("answer_ms"), -1);

    // An answer that matches, 20 ms after the Write.
    inject(at(1ms), frame(6, kPgnPropA, kTool, kEcu, write(0x0102, 12.5)));
    inject(at(21ms), frame(6, kPgnPropA, kEcu, kTool, answer(0x0102, 12.5)));
    bench.run_until(at(50ms));
    CHECK_EQ(check("answered"), 1);
    CHECK_EQ(check("answer_ms"), 20);
    CHECK_EQ(check("no_answer"), 0);
    CHECK_EQ(check("mismatches"), 0);

    // No answer: counted in the first step more than 200 ms after the Write.
    inject(at(100ms), frame(6, kPgnPropA, kTool, kEcu, write(0x0103, 1.0)));
    bench.run_until(at(300ms) + 1ns); // 200 ms exactly: still waiting
    CHECK_EQ(check("no_answer"), 0);
    bench.run_until(at(305ms) + 1ns);
    CHECK_EQ(check("no_answer"), 1);
    CHECK_EQ(check("answered"), 1);

    // An answer with another value.
    inject(at(400ms), frame(6, kPgnPropA, kTool, kEcu, write(0x0104, 3.0)));
    inject(at(410ms), frame(6, kPgnPropA, kEcu, kTool, answer(0x0104, 2.5)));
    bench.run_until(at(450ms));
    CHECK_EQ(check("answered"), 2);
    CHECK_EQ(check("answer_ms"), 10);
    CHECK_EQ(check("mismatches"), 1);

    // An answer that came longer, over a connection, reaches the same handler: 20 bytes, its
    // time is that of the last packet.
    inject(at(500ms), frame(6, kPgnPropA, kTool, kEcu, write(0x0105, 4.0)));
    Bytes long_answer = answer(0x0105, 4.0);
    long_answer.resize(20, 0xAA);
    SimTime t = at(510ms);
    inject(t, rts(kEcu, kTool, 20, kPgnPropA));
    inject(t += 1ms, cts(kTool, kEcu, 3, 1, kPgnPropA));
    for (const auto& f : dts(kEcu, kTool, long_answer, 1, 3))
    {
        inject(t += 1ms, f); // the last one at 514 ms
    }
    inject(t += 1ms, eoma(kTool, kEcu, 20, kPgnPropA));
    bench.run_until(at(550ms));
    CHECK_EQ(check("answered"), 3);
    CHECK_EQ(check("answer_ms"), 14);
    CHECK_EQ(check("mismatches"), 1);
    CHECK_EQ(check("no_answer"), 1);

    // A new Write before the answer: the first one had none, and the answer is the second's.
    inject(at(600ms), frame(6, kPgnPropA, kTool, kEcu, write(0x0106, 5.0)));
    inject(at(620ms), frame(6, kPgnPropA, kTool, kEcu, write(0x0107, 6.0)));
    inject(at(630ms), frame(6, kPgnPropA, kEcu, kTool, answer(0x0107, 6.0)));
    bench.run_until(at(650ms));
    CHECK_EQ(check("no_answer"), 2);
    CHECK_EQ(check("answered"), 4);
    CHECK_EQ(check("answer_ms"), 10);
    CHECK_EQ(check("mismatches"), 1);
    CHECK_EQ(integer_of(ecu, "spy.missed"), 0);
}
