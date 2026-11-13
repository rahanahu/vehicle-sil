// SPDX-License-Identifier: MIT
//
// The input ecu.connected that every ECU has: an ECU taken off its buses and put back while it
// runs, on the local virtual buses (in both orders of the ECUs) and on SIL Kit. J1939 and raw
// CAN: the others stop hearing it and it stops hearing them, with the frames stamped at the
// steps of the changes decided alike; its sends fail; it announces its address again when it
// comes back (or not, NetworkOptions::on_reconnect), which finds a node that took its address
// meanwhile. LIN: a master off the bus gets no responses and its slaves no headers, except for
// the header it sent before it went off; a slave off the bus answers nothing and hears nothing.
// The records must be the same on every runtime and order.
#include "test_harness.hpp"

#include "vehicle_sil/simulation/simulation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::Ecu;
using vehicle_sil::SilKitOptions;
using vehicle_sil::SimTime;
using vehicle_sil::Simulation;
namespace can = vehicle_sil::can;
namespace j1939 = vehicle_sil::j1939;
namespace lin = vehicle_sil::lin;
namespace control = vehicle_sil::control;

constexpr j1939::Pgn kSpeed{0xFF10}; // the engine's, every 100 ms
constexpr j1939::Pgn kDash{0xFF20};  // the dashboard's, every 10 ms
constexpr lin::LinId kCommand = 0x10; // the master's frame
constexpr lin::LinId kStatus = 0x11;  // the slave's frame

SimTime at(std::chrono::milliseconds ms) { return SimTime{} + ms; }

long long ms_of(SimTime t) { return static_cast<long long>(t.time_since_epoch() / 1ms); }

j1939::Name name(std::uint32_t identity) { return j1939::Name{}.set_identity_number(identity).set_manufacturer_code(0x7F0); }

j1939::NetworkOptions on_reconnect(j1939::OnReconnect what)
{
    j1939::NetworkOptions options;
    options.on_reconnect = what;
    return options;
}

/// Sends its speed every 100 ms on J1939 (a node with a NAME) and a raw 11-bit frame every 100 ms,
/// hears the dashboard, and runs a LIN schedule of its own frame and the slave's.
class Engine
{
public:
    explicit Engine(Ecu& ecu) :
      network(ecu.j1939("J1939_CAN")),
      node(network.add_node({0x00, name(7)})),
      raw(ecu.can("RAW")),
      master(ecu.lin_master("LIN", {{kCommand, 1, lin::LinChecksumModel::Enhanced, 10ms, lin::LinPublisher::Master},
                                    {kStatus, 1, lin::LinChecksumModel::Enhanced, 10ms, lin::LinPublisher::Slave}}))
    {
        ecu.every(100ms, [this](SimTime) {
            const std::array<std::uint8_t, 1> data{0x42};
            static_cast<void>(node.send(kSpeed, data));
            static_cast<void>(raw.send(can::Frame{0x100, can::IdFormat::Standard, 1, {0x42}}));
        });
        node.on_message(kDash, [this](const j1939::Message& m) { heard.push_back(ms_of(m.time)); });
        master.on_result([this](const lin::LinScheduleResult& r) {
            results.push_back(std::to_string(ms_of(r.header_time)) + " " + std::to_string(r.id) + " " +
                              (r.status == lin::LinResultStatus::Ok ? "ok" : "none"));
        });
        ecu.on_step([this](SimTime) { master.set_frame_data(kCommand, std::array<std::uint8_t, 1>{0x01}); });
    }
    j1939::Network& network;
    j1939::Node& node;
    can::RawChannel& raw;
    lin::LinMaster& master;
    std::vector<long long> heard;      ///< the times of the dashboard's messages received
    std::vector<std::string> results; ///< "<header ms> <id> ok|none"
};

/// Sends every 10 ms; hears the engine's speed, its raw frames and its address claims.
class Dashboard
{
public:
    explicit Dashboard(Ecu& ecu)
    {
        auto& node = ecu.j1939("J1939_CAN").add_node({0x28, name(8)});
        node.on_message(kSpeed, [this](const j1939::Message& m) { speeds.push_back(ms_of(m.time)); });
        node.on_message(j1939::kPgnAddressClaimed, [this](const j1939::Message& m) {
            if (m.source == 0x00)
            {
                claims.push_back(ms_of(m.time));
            }
        });
        ecu.every(10ms, [&node](SimTime) {
            const std::array<std::uint8_t, 1> data{0x01};
            static_cast<void>(node.send(kDash, data));
        });
        ecu.can("RAW").on_frame([this](const can::TimedFrame& f) { raw.push_back(ms_of(f.time)); });
    }
    std::vector<long long> speeds; ///< the times of the speeds received
    std::vector<long long> claims; ///< the times of the engine's Address Claimed
    std::vector<long long> raw;    ///< the times of the engine's raw frames received
};

/// The LIN slave: answers kStatus, receives kCommand.
class Door
{
public:
    explicit Door(Ecu& ecu) : slave(ecu.lin_slave("LIN", lin::LinSlaveOptions{{kCommand}, std::nullopt}))
    {
        slave.on_frame([this](const lin::LinReceivedFrame& f) { frames.push_back(ms_of(f.header_time)); });
        ecu.on_step([this](SimTime now) { slave.set_response(now, kStatus, std::array<std::uint8_t, 1>{0x5A}); });
    }
    lin::LinSlave& slave;
    std::vector<long long> frames; ///< the header times of the commands received
};

struct Records
{
    std::vector<std::string> results;
    std::vector<long long> speeds;
    std::vector<long long> claims;
    std::vector<long long> raw;
    std::vector<long long> heard;
    std::vector<long long> frames;
    std::uint64_t send_failures = 0;
    std::uint64_t raw_dropped = 0;
    std::vector<control::CommandResult> writes;
};

void write(Ecu& ecu, std::uint64_t seq, bool connected, std::chrono::milliseconds when)
{
    CHECK(ecu.agent().submit({seq, {{"ecu.connected", connected}}, at(when)}).status == control::SubmitStatus::Queued);
}

/// Engine off its buses from 300 to 600 ms, off and on again within its step at 800 ms; Door off
/// from 1000 to 1200 ms. `door_first`: on the local buses the slave steps before the master, so
/// its change at 1000 ms has taken effect when the header stamped 1000 ms reaches it.
Records run(std::optional<SilKitOptions> silkit, bool door_first)
{
    Simulation sim(silkit);
    Door* door = door_first ? &sim.add<Door>("Door", 5ms) : nullptr;
    auto& engine = sim.add<Engine>("Engine", 10ms);
    auto& dashboard = sim.add<Dashboard>("Dashboard", 5ms);
    if (door == nullptr)
    {
        door = &sim.add<Door>("Door", 5ms);
    }
    write(sim.ecu("Engine"), 1, false, 300ms);
    write(sim.ecu("Engine"), 2, true, 600ms);
    write(sim.ecu("Engine"), 3, false, 800ms);
    write(sim.ecu("Engine"), 4, true, 800ms);
    write(sim.ecu("Door"), 1, false, 1000ms);
    write(sim.ecu("Door"), 2, true, 1200ms);
    sim.run_for(1400ms);
    Records out{engine.results,
                dashboard.speeds,
                dashboard.claims,
                dashboard.raw,
                engine.heard,
                door->frames,
                engine.network.statistics().send_failures,
                engine.raw.statistics().frames_dropped,
                {}};
    sim.ecu("Engine").agent().drain_results(out.writes);
    sim.ecu("Door").agent().drain_results(out.writes);
    return out;
}

bool in(long long t, long long from, long long to) { return t >= from && t < to; }

bool has(const std::vector<long long>& v, long long t) { return std::find(v.begin(), v.end(), t) != v.end(); }

void check(const Records& r)
{
    CHECK_EQ(r.writes.size(), std::size_t{6});
    for (const auto& w : r.writes)
    {
        CHECK(w.status == control::CommandStatus::Applied);
    }
    // J1939 and raw CAN, sent by the engine: those of 300, 400 and 500 ms did not leave (failed
    // sends), the others arrived (also at 800 ms: off and on again in that step).
    const std::vector<long long> sent{0, 100, 200, 600, 700, 800, 900, 1000, 1100, 1200, 1300};
    CHECK(r.speeds == sent);
    CHECK(r.raw == sent);
    CHECK_EQ(r.send_failures, std::uint64_t{3});
    CHECK_EQ(r.raw_dropped, std::uint64_t{3});
    // Received by the engine: what was stamped up to its change at 300 ms, nothing up to and with
    // the change back at 600 ms, everything after.
    std::vector<long long> heard;
    for (long long t = 0; t < 1390; t += 10) // the one of 1390 ms would arrive at 1400 ms
    {
        if (t <= 300 || t > 600)
        {
            heard.push_back(t);
        }
    }
    CHECK(r.heard == heard);
    // The first claim, and the claim the engine sends again when it comes back at 600 ms (not at
    // 800 ms: it was not off before that step).
    CHECK(r.claims == (std::vector<long long>{0, 600}));
    // LIN, Engine (the master) off from 300 to 600 ms: no response to any header of it, and the
    // slave gets none of its frames. Door off from 1000 to 1200 ms: its frame is not answered and
    // it hears no command; what was stamped at 1000 ms (before its change took effect) it still
    // hears, what was stamped at 1200 ms not yet.
    CHECK(!r.results.empty());
    for (const auto& line : r.results)
    {
        const long long t = std::stoll(line);
        const bool status_frame = line.find(" " + std::to_string(kStatus) + " ") != std::string::npos;
        const bool lost = in(t, 300, 600) || (status_frame && t > 1000 && t <= 1200);
        CHECK_EQ(line.substr(line.size() - 4) == "none", lost);
    }
    for (const long long t : r.frames)
    {
        CHECK(!in(t, 300, 600) && !(t > 1000 && t <= 1200));
    }
    CHECK(has(r.frames, 280) && has(r.frames, 600) && has(r.frames, 1000) && has(r.frames, 1220));
    CHECK(r.frames.size() > 40);
}

bool same(const Records& a, const Records& b)
{
    return a.results == b.results && a.speeds == b.speeds && a.claims == b.claims && a.raw == b.raw &&
           a.heard == b.heard && a.frames == b.frames;
}

/// A LIN master with a step much shorter than its slot: only the slave's frame, every 10 ms.
class FastMaster
{
public:
    explicit FastMaster(Ecu& ecu) :
      master(ecu.lin_master("LIN", {{kStatus, 1, lin::LinChecksumModel::Enhanced, 10ms, lin::LinPublisher::Slave}}))
    {
        master.on_result([this](const lin::LinScheduleResult& r) {
            results.push_back(std::to_string(ms_of(r.header_time)) + (r.status == lin::LinResultStatus::Ok ? " ok" : " none"));
        });
    }
    lin::LinMaster& master;
    std::vector<std::string> results; ///< "<header ms> ok|none"
};

/// The master (step 1 ms) off its bus from 101 ms (in the slot of the header of 100 ms) to 205 ms.
std::vector<std::string> run_fast_master(std::optional<SilKitOptions> silkit, bool slave_first)
{
    Simulation sim(silkit);
    if (slave_first)
    {
        sim.add<Door>("Door", 5ms);
    }
    auto& body = sim.add<FastMaster>("Body", 1ms);
    if (!slave_first)
    {
        sim.add<Door>("Door", 5ms);
    }
    write(sim.ecu("Body"), 1, false, 101ms);
    write(sim.ecu("Body"), 2, true, 205ms);
    sim.run_for(260ms);
    return body.results;
}

/// A node with a NAME at 0x00 that sends nothing on its own.
class Claimer
{
public:
    Claimer(Ecu& ecu, j1939::OnReconnect what) : node(ecu.j1939("J1939_CAN", on_reconnect(what)).add_node({0x00, name(7)})) {}
    j1939::Node& node;
};

/// Takes 0x00 at 400 ms with a NAME of higher priority than the claimer's.
class Rival
{
public:
    explicit Rival(Ecu& ecu)
    {
        auto& network = ecu.j1939("J1939_CAN");
        ecu.at(at(400ms), [this, &network](SimTime) { node = &network.add_node({0x00, name(1)}); });
    }
    j1939::Node* node = nullptr;
};

/// "<claimer state> <address> / <rival state> <address>" after the claimer was off from 300 to
/// 600 ms, while the rival took its address.
std::string run_contention(std::optional<SilKitOptions> silkit, j1939::OnReconnect what)
{
    Simulation sim(silkit);
    auto& claimer = sim.add<Claimer>("Claimer", 10ms, what);
    auto& rival = sim.add<Rival>("Rival", 10ms);
    write(sim.ecu("Claimer"), 1, false, 300ms);
    write(sim.ecu("Claimer"), 2, true, 600ms);
    sim.run_for(1000ms);
    const auto shown = [](const j1939::Node& n) {
        return std::to_string(static_cast<int>(n.address_state())) + " " + std::to_string(n.address());
    };
    return shown(claimer.node) + " / " + (rival.node != nullptr ? shown(*rival.node) : "none");
}

} // namespace

TEST_CASE(disconnect_takes_an_ecu_off_its_buses_and_back_locally_and_on_silkit)
{
    const Records local = run(std::nullopt, true);
    const Records reversed = run(std::nullopt, false);
    const Records silkit = run(SilKitOptions{}, true);
    check(local);
    check(reversed);
    check(silkit);
    CHECK(same(local, reversed));
    CHECK(same(local, silkit));
}

TEST_CASE(disconnect_a_lin_master_in_the_middle_of_a_slot_keeps_the_response_to_the_header_it_sent)
{
    // The header of 100 ms went out before the master went off: its response counts, whenever it
    // arrives (on SIL Kit possibly after the master's step at 101 ms). The headers of 110 to
    // 200 ms did not go out; the one of 210 ms did. (At 0 ms the slave has no value yet.)
    std::vector<std::string> expected;
    for (int t = 0; t < 250; t += 10)
    {
        expected.push_back(std::to_string(t) + (t == 0 || (t > 100 && t < 210) ? " none" : " ok"));
    }
    CHECK(run_fast_master(std::nullopt, false) == expected);
    CHECK(run_fast_master(std::nullopt, true) == expected);
    for (int i = 0; i < 4; ++i)
    {
        CHECK(run_fast_master(SilKitOptions{}, i % 2 == 0) == expected);
    }
}

TEST_CASE(disconnect_j1939_on_reconnect_finds_a_node_that_took_the_address_or_not)
{
    // Announce (the default): the claimer's claim meets the rival's higher priority NAME, so it
    // cannot claim (state 4, address 254) and the rival keeps 0x00 (claimed, 3). Nothing: both
    // still think they own 0x00 (nobody sent anything).
    const std::string announce = "4 254 / 3 0";
    const std::string nothing = "3 0 / 3 0";
    CHECK(run_contention(std::nullopt, j1939::OnReconnect::Announce) == announce);
    CHECK(run_contention(SilKitOptions{}, j1939::OnReconnect::Announce) == announce);
    CHECK(run_contention(std::nullopt, j1939::OnReconnect::Nothing) == nothing);
    CHECK(run_contention(SilKitOptions{}, j1939::OnReconnect::Nothing) == nothing);
}
