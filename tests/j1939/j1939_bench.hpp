// SPDX-License-Identifier: MIT
//
// Test bench for the J1939 stack (vehicle_sil::j1939): one Network on a host and a
// tester endpoint on the same virtual bus that plays the remote nodes frame by frame.
#pragma once

#include "test_harness.hpp"

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"
#include "vehicle_sil/j1939/network.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace vehicle_sil::unit::j1939 {

using namespace std::chrono_literals;
using vehicle_sil::j1939::Address;
using vehicle_sil::j1939::Pgn;

using Bytes = std::vector<std::uint8_t>;

inline constexpr Address kGlobal = 0xFF;
inline constexpr Address kNull = 0xFE;
inline constexpr std::uint32_t kPgnRequest = 0xEA00;
inline constexpr std::uint32_t kPgnAck = 0xE800;
inline constexpr std::uint32_t kPgnAddressClaim = 0xEE00;
inline constexpr std::uint32_t kPgnPropA = 0xEF00; ///< PDU1: destination specific
inline constexpr std::uint32_t kPgnPropB = 0xFF10; ///< PDU2: always global
inline constexpr std::uint8_t kAck = 0x00;
inline constexpr std::uint8_t kNack = 0x01;
inline constexpr std::uint8_t kAccessDenied = 0x02;
inline constexpr std::uint8_t kCannotRespond = 0x03;

[[nodiscard]] inline SimTime at(std::chrono::nanoseconds t) { return SimTime{} + t; }

/// The 29-bit identifier of a J1939 frame, written out independently of the library under test.
[[nodiscard]] constexpr std::uint32_t j1939_id(std::uint8_t priority, std::uint32_t pgn, std::uint8_t da, std::uint8_t sa) noexcept
{
    std::uint32_t field = pgn & 0x3FFFFu;
    if (((field >> 8) & 0xFFu) < 0xF0u)
    {
        field = (field & 0x3FF00u) | da;
    }
    return (static_cast<std::uint32_t>(priority & 0x7u) << 26) | (field << 8) | sa;
}

/// A J1939 frame decoded from its identifier.
struct J1939Frame
{
    std::uint8_t priority = 6;
    std::uint32_t pgn = 0; ///< PDU1: PS (destination) cleared
    std::uint8_t da = kGlobal;
    std::uint8_t sa = 0;
    Bytes data;
    bool operator==(const J1939Frame&) const = default;
};

[[nodiscard]] inline J1939Frame frame(std::uint8_t priority, std::uint32_t pgn, std::uint8_t sa, std::uint8_t da, Bytes data)
{
    return J1939Frame{priority, pgn, da, sa, std::move(data)};
}

[[nodiscard]] inline can::Frame to_can(const J1939Frame& f)
{
    can::Frame out;
    out.id = j1939_id(f.priority, f.pgn, f.da, f.sa);
    out.format = can::IdFormat::Extended;
    out.length = static_cast<std::uint8_t>(f.data.size());
    for (std::size_t i = 0; i < f.data.size() && i < 8; ++i)
    {
        out.data[i] = f.data[i];
    }
    return out;
}

[[nodiscard]] inline J1939Frame from_can(const can::Frame& in)
{
    J1939Frame out;
    out.priority = static_cast<std::uint8_t>((in.id >> 26) & 0x7u);
    out.sa = static_cast<std::uint8_t>(in.id & 0xFFu);
    const std::uint32_t field = (in.id >> 8) & 0x3FFFFu;
    if (((field >> 8) & 0xFFu) < 0xF0u)
    {
        out.pgn = field & 0x3FF00u;
        out.da = static_cast<std::uint8_t>(field & 0xFFu);
    }
    else
    {
        out.pgn = field;
    }
    const auto payload = in.payload();
    out.data.assign(payload.begin(), payload.end());
    return out;
}

[[nodiscard]] inline std::string to_string(const J1939Frame& f)
{
    char text[64];
    std::snprintf(text, sizeof(text), "P%u PGN %05X %02X->%02X [", static_cast<unsigned>(f.priority),
                  static_cast<unsigned>(f.pgn), static_cast<unsigned>(f.sa), static_cast<unsigned>(f.da));
    std::string out = text;
    for (std::size_t i = 0; i < f.data.size(); ++i)
    {
        std::snprintf(text, sizeof(text), i == 0 ? "%02X" : " %02X", static_cast<unsigned>(f.data[i]));
        out += text;
    }
    return out + "]";
}

inline void check_frames(const std::vector<J1939Frame>& actual, const std::vector<J1939Frame>& expected, const char* file, int line)
{
    if (actual != expected)
    {
        std::string a;
        std::string e;
        for (const auto& f : actual)
        {
            a += "\n    " + to_string(f);
        }
        for (const auto& f : expected)
        {
            e += "\n    " + to_string(f);
        }
        throw Failure(std::string(file) + ":" + std::to_string(line) + ": frames differ\n  actual:" +
                      (a.empty() ? "\n    (none)" : a) + "\n  expected:" + (e.empty() ? "\n    (none)" : e));
    }
}

#define CHECK_J1939(actual, ...)                                                                                 \
    ::vehicle_sil::unit::j1939::check_frames((actual), std::vector<::vehicle_sil::unit::j1939::J1939Frame> __VA_ARGS__, \
                                              __FILE__, __LINE__)

[[nodiscard]] inline Bytes le24(std::uint32_t v)
{
    return {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v >> 16)};
}
[[nodiscard]] inline Bytes request(std::uint32_t pgn) { return le24(pgn); }
/// Acknowledgement payload: control byte, 0xFF, two reserved 0xFF, requester address, PGN.
[[nodiscard]] inline Bytes acknowledgement(std::uint8_t control, std::uint8_t requester, std::uint32_t pgn)
{
    Bytes out{control, 0xFF, 0xFF, 0xFF, requester};
    const Bytes p = le24(pgn);
    out.insert(out.end(), p.begin(), p.end());
    return out;
}

/// One Network under test on a host with a fixed step, and a tester endpoint on the same
/// virtual bus. Steps run at 0, step, 2*step, ...; frames the tester injects at time t reach
/// the network in the first step after t.
class Bench
{
public:
    explicit Bench(vehicle_sil::j1939::NetworkOptions options = {}, SimDuration step = 10ms)
      : network(std::move(options)), step_(step)
    {
        host.add(network, port);
        host.set_step_size(step);
        host.set_application([this](SimTime now) {
            if (app)
            {
                app(now);
            }
        });
    }

    vehicle_sil::j1939::Node& add_node(Address address) { return network.add_node({address}); }

    /// Injects a frame from the tester at `time` (not before the last step).
    void inject(SimTime time, const J1939Frame& f) { tester.send(can::TimedFrame{time, to_can(f)}); }
    /// Injects a frame at the time of the last step (time 0 before the first step), so the next
    /// step after time 0 handles it.
    void inject(const J1939Frame& f) { inject(last_, f); }

    /// Runs one step at the next step time.
    void step()
    {
        last_ = next_;
        host.step(next_);
        next_ += step_;
    }
    void steps(int n)
    {
        for (int i = 0; i < n; ++i)
        {
            step();
        }
    }

    /// The frames the network sent so far (and not taken yet), as the tester saw them.
    std::vector<J1939Frame> take_sent()
    {
        std::vector<J1939Frame> out;
        tester.release_before(last_ + 1ns, [&](const can::TimedFrame& f) {
            out.push_back(from_can(f.frame));
            sent_times.push_back(f.time);
        });
        return out;
    }

    [[nodiscard]] SimTime next_time() const { return next_; }
    [[nodiscard]] SimTime last_time() const { return last_; }

    can::VirtualCanBus bus;
    can::VirtualCanBus::Endpoint& tester = bus.attach();
    can::VirtualCanBus::Endpoint& port = bus.attach();
    vehicle_sil::j1939::Network network;
    can::Host host;
    std::function<void(SimTime)> app;
    std::vector<SimTime> sent_times;

private:
    SimDuration step_;
    SimTime next_{};
    SimTime last_{};
};

/// Records every message a node's handler sees.
struct Seen
{
    std::uint32_t pgn;
    std::uint8_t sa;
    std::uint8_t da;
    std::uint8_t priority;
    SimTime time;
    Bytes data;
    bool operator==(const Seen&) const = default;
};

[[nodiscard]] inline Seen seen_of(const vehicle_sil::j1939::Message& m)
{
    return Seen{m.pgn.value(), m.source, m.destination, m.priority, m.time, Bytes(m.data.begin(), m.data.end())};
}

} // namespace vehicle_sil::unit::j1939
