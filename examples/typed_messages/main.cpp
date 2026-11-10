// SPDX-License-Identifier: MIT
//
// Typed messages: signals are defined in typed_messages_messages.yaml, and the build turns them
// into C++ types. Part 1 uses the types alone (encode, decode, special values). Part 2 sends
// them between two ECUs, a 24-byte one included (the transport protocol), with no bit position
// in the code.
#include "typed_messages_messages.hpp"

#include "vehicle_sil/j1939/codec_node.hpp"
#include "vehicle_sil/simulation/simulation.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>

using namespace std::chrono_literals;
using typed_messages::messages::CoolantStatus;
using typed_messages::messages::SamplePack;
namespace codec = vehicle_sil::j1939::codec;
using vehicle_sil::j1939::codec::SignalState;

namespace {

bool g_ok = true;
void check(bool condition, const char* what)
{
    std::printf("  [%s] %s\n", condition ? "ok" : "FAILED", what);
    g_ok = g_ok && condition;
}

void part_one_types()
{
    std::printf("encode and decode\n");
    CoolantStatus m;                     // every signal starts as "not available"
    check(m.temperature.state() == SignalState::NotAvailable, "a new signal is not available");
    m.temperature.set(85.0);             // physical value in, raw value stored
    m.flow.set(12.34);
    m.pump.set(CoolantStatus::Pump::on);
    m.trim.set(-3.5);                    // signed, scaled
    m.level.set_special("calibrating");  // a special value of this signal's own
    check(m.temperature.raw() == 125 && m.trim.raw() == -7, "physical values become raw values");

    const auto bytes = m.bytes();        // the 8 bytes on the wire
    const auto back = CoolantStatus::decode(bytes);
    check(back && *back == m, "decode(encode(m)) is m");
    check(back && back->flow.value_or(0) > 12.3 && back->flow.value_or(0) < 12.4, "flow comes back rounded to its resolution");
    check(back && back->pump.value() == CoolantStatus::Pump::on, "an enumeration comes back as an enumerator");
    check(back && back->level.state() == SignalState::Special && back->level.special() == "calibrating",
          "a special value has a state and a name, and no value");

    CoolantStatus odd = m;
    odd.temperature.set_raw(200);        // 160 degC: valid bits, but beyond the signal's maximum
    odd.flow.set_error();
    check(odd.temperature.state() == SignalState::OutOfRange && !odd.temperature.value(), "beyond max: out of range");
    check(odd.flow.state() == SignalState::Error && odd.flow.raw() == 0xFE00, "error is a state; the raw value is kept");
    check(!CoolantStatus::decode(std::span<const std::uint8_t>(bytes).first(4)), "a short message does not decode");
}

// An ECU is a class that takes the Ecu& it runs on; Simulation runs it on a virtual bus.
class Pump
{
public:
    explicit Pump(vehicle_sil::Ecu& ecu)
    {
        node_ = &ecu.j1939("J1939_CAN").add_node({.address = 0x10});
        codec::attach_sender<CoolantStatus>(*node_, 20ms, [this](CoolantStatus& m) {
            m.temperature.set(60.0 + 0.5 * static_cast<double>(packs_));
            m.flow.set(10.0);
        });
        ecu.every(100ms, [this](vehicle_sil::SimTime) {
            SamplePack pack;
            pack.sequence.set_raw(++packs_);
            pack.first.set(1.5);
            pack.last.set(2.5);
            codec::send(*node_, pack, 0x80); // 24 bytes: the stack splits and reassembles them
        });
    }

private:
    vehicle_sil::j1939::Node* node_ = nullptr;
    std::uint64_t packs_ = 0;
};

class Dash
{
public:
    explicit Dash(vehicle_sil::Ecu& ecu)
    {
        auto& node = ecu.j1939("J1939_CAN").add_node({.address = 0x80});
        codec::on_message<CoolantStatus>(node, [this](const CoolantStatus& m) {
            ++status_;
            temperature_ = m.temperature.value_or(-1.0);
        });
        codec::on_message<SamplePack>(node, [this](const SamplePack& p) {
            ++packs_;
            last_ = p.last.value_or(-1.0);
        });
    }
    int status_ = 0, packs_ = 0;
    double temperature_ = 0, last_ = 0;
};

} // namespace

int main()
{
    try
    {
        part_one_types();
        std::printf("two ECUs on a virtual bus\n");
        vehicle_sil::Simulation sim;
        sim.add<Pump>("Pump", 10ms);
        auto& dash = sim.add<Dash>("Dash", 5ms);
        sim.run_for(1s);
        std::printf("  the dash got %d status messages (%.1f degC) and %d sample packs (last value %.2f)\n", dash.status_,
                    dash.temperature_, dash.packs_, dash.last_);
        // Physical values are raw * scale (a float), so compare with a tolerance, never with ==.
        check(dash.status_ > 40 && dash.packs_ > 5 && std::abs(dash.last_ - 2.5) < 1e-9, "typed messages arrived, the long one whole");
        return g_ok ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
