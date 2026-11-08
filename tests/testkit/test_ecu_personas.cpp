// SPDX-License-Identifier: MIT
//
// The ECU application API as its users write it, on the local virtual bus: an ECU application
// (examples/j1939/ecu_pair), a protocol of one's own built as a part from the toolbox and the
// Requester, and a test that checks the communication with Monitor, the Observer and the tap.

#include "ecu_pair.hpp"
#include "test_harness.hpp"
#include "vehicle_sil/j1939/address_commander.hpp"
#include "vehicle_sil/j1939/broadcast_suppression.hpp"
#include "vehicle_sil/testkit/local_can.hpp"
#include "vehicle_sil/testkit/recorders.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace ecu_pair;
using vehicle_sil::SimTime;
using vehicle_sil::testkit::FrameLog;
using vehicle_sil::testkit::J1939Events;
using vehicle_sil::testkit::LocalCan;
namespace can = vehicle_sil::can;

namespace {

/// The usual set-up: the engine steps every 10 ms, the cab every 5 ms, as two participants would.
struct Vehicle
{
    EngineModel engine;
    Dashboard dash;
    LocalCan bus;
    j1939::Node& engine_ecu = install_engine(bus.add_j1939("engine", 10ms), engine);
    j1939::Node& cab = install_cab(bus.add_j1939("cab", 5ms), dash);

    Vehicle()
    {
        bus.on_step("engine", [this](SimTime) { engine.tick(); });
    }
};

} // namespace

// P1: the ECU application: periodic messages, a request and its response (over the transport
// protocol), a monitored message, with the address claims under the hood.
TEST_CASE(p1_the_engine_and_the_cab_talk_without_the_application_handling_the_stack)
{
    Vehicle v;
    CHECK(!v.dash.engine_running);
    v.bus.run_for(2s);
    CHECK(v.cab.address_state() == j1939::AddressState::Claimed);
    CHECK(v.dash.engine_running); // the engine's state, an enumerated signal
    CHECK(v.dash.engine_software == std::string("\x02") + "ENGINE*1.0.3*");
    CHECK_EQ(v.engine.requested, 1500);
    CHECK_EQ(v.dash.engine_rpm, 1500);
    CHECK(!v.dash.engine_silent);
    v.dash.wanted_rpm = 900;
    v.bus.run_for(1s);
    CHECK_EQ(v.dash.engine_rpm, 900);
}

// P1/P6: the engine drops off the bus for a while: the cab notices, and recovers by itself.
TEST_CASE(p1_the_cab_notices_when_the_engine_goes_silent)
{
    Vehicle v;
    auto* monitor = v.cab.find<j1939::Monitor>();
    std::vector<bool> silent;
    v.bus.on_step("cab", [&](SimTime) {
        if (silent.empty() || silent.back() != v.dash.engine_silent)
        {
            silent.push_back(v.dash.engine_silent);
        }
    });
    v.bus.run_for(1s);
    v.bus.endpoint("engine").add_outage(v.bus.now(), v.bus.now() + 300ms);
    v.bus.run_for(1s);
    CHECK(silent == (std::vector<bool>{false, true, false}));
    CHECK(monitor != nullptr && monitor->losses() == 1u && monitor->state() == j1939::MonitorState::Receiving);
}

// P1/P3: a service tool moves the cab to another address; the engine's data still reaches it
// (the Monitor follows the engine by NAME, the cab's sender moves along).
TEST_CASE(p1_a_service_tool_commands_the_cab_to_another_address)
{
    Vehicle v;
    auto& tool = v.bus.add_j1939("tool", 10ms).add_node({.address = 0xF9}); // a fixed address, no NAME
    auto& commander = tool.attach<j1939::AddressCommander>();
    std::optional<j1939::CommandOutcome> outcome;
    v.bus.run_for(1s);
    v.bus.host("tool").post([&] {
        commander.command(kCabName, 0x81, [&](const j1939::CommandResult& r) { outcome = r.outcome; });
    });
    v.dash.wanted_rpm = 1000;
    v.bus.run_for(2s);
    CHECK(outcome == j1939::CommandOutcome::Moved);
    CHECK_EQ(v.cab.address(), 0x81);
    CHECK_EQ(v.engine.requested, 1000); // the speed request now comes from 0x81
    CHECK_EQ(v.dash.engine_rpm, 1000);
}

namespace {

// P2: a protocol of one's own, built as parts from the toolbox: "read item n" (proprietary A,
// [0x01, n]) answered by [0x81, n, value low, value high]. No threads, no stack internals.
constexpr j1939::Pgn kPgnItems{0x1EF00}; // proprietary A2 (PDU1, data page 1)

class ItemServer
{
public:
    ItemServer(j1939::Node& node, std::function<std::uint16_t(std::uint8_t)> read)
      : node_(node), read_(std::move(read)),
        commands_(node, node.on_message(kPgnItems, [this](const j1939::Message& m) { on_command(m); }))
    {
    }

private:
    void on_command(const j1939::Message& m)
    {
        if (m.data.size() >= 2 && m.data[0] == 0x01)
        {
            const std::uint16_t value = read_(m.data[1]);
            const std::array<std::uint8_t, 8> reply{0x81, m.data[1], static_cast<std::uint8_t>(value),
                                                    static_cast<std::uint8_t>(value >> 8), 0xFF, 0xFF, 0xFF, 0xFF};
            static_cast<void>(node_.send(kPgnItems, reply, m.source));
        }
    }
    j1939::Node& node_;
    std::function<std::uint16_t(std::uint8_t)> read_;
    j1939::Subscription commands_;
};

class ItemReader
{
public:
    using Done = std::function<void(std::optional<std::uint16_t>)>;

    ItemReader(j1939::Node& node, j1939::Address server)
      : node_(node), server_(server),
        replies_(node, node.on_message(kPgnItems, [this](const j1939::Message& m) { on_reply(m); })),
        timeout_(node, j1939::TimerKind::Timeout, [this] { finish(std::nullopt); })
    {
    }

    /// One read at a time; from a step (a handler, a timer or the application stage).
    void read(std::uint8_t item, Done done)
    {
        item_ = item;
        done_ = std::move(done);
        const std::array<std::uint8_t, 8> command{0x01, item, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        static_cast<void>(node_.send(kPgnItems, command, server_));
        timeout_.start_at(*node_.network().now() + 200ms); // from the step that sends it
    }

private:
    void on_reply(const j1939::Message& m)
    {
        if (done_ && m.source == server_ && m.data.size() >= 4 && m.data[0] == 0x81 && m.data[1] == item_)
        {
            timeout_.cancel();
            finish(static_cast<std::uint16_t>(m.data[2] | (m.data[3] << 8)));
        }
    }
    void finish(std::optional<std::uint16_t> value)
    {
        auto done = std::exchange(done_, nullptr); // the callback may read again
        done(value);
    }
    j1939::Node& node_;
    j1939::Address server_;
    j1939::Subscription replies_;
    j1939::Timer timeout_;
    std::uint8_t item_ = 0;
    Done done_;
};

/// P2 reusing the Requester: asks every node for its software identification, keeps the answers.
class SoftwareInventory
{
public:
    explicit SoftwareInventory(j1939::Node& node) : node_(node) {}
    void refresh()
    {
        j1939::Requester::of(node_).request(kPgnSoftwareId, j1939::kGlobalAddress, [this](const j1939::RequestResult& r) {
            if (r.outcome == j1939::RequestOutcome::Response)
            {
                found[r.source].assign(r.data.begin(), r.data.end());
            }
        });
    }
    std::map<j1939::Address, std::string> found;

private:
    j1939::Node& node_;
};

} // namespace

TEST_CASE(p2_a_protocol_of_ones_own_as_parts)
{
    Vehicle v;
    v.engine_ecu.attach<ItemServer>([&](std::uint8_t item) { return item == 3 ? v.engine.rpm : std::uint16_t{0xFFFF}; });
    auto& reader = v.cab.attach<ItemReader>(kEngineAddress);
    auto& lost = v.cab.attach<ItemReader>(0x42); // nobody there
    auto& inventory = v.cab.attach<SoftwareInventory>();
    std::vector<std::optional<std::uint16_t>> values;
    v.bus.run_for(1s);
    v.bus.host("cab").post([&] {
        reader.read(3, [&](std::optional<std::uint16_t> value) { values.push_back(value); });
        lost.read(3, [&](std::optional<std::uint16_t> value) { values.push_back(value); });
        inventory.refresh();
    });
    v.bus.run_for(2s);
    CHECK_EQ(values.size(), 2u);
    CHECK(values.size() == 2 && values[0] == std::optional<std::uint16_t>{1500} && !values[1]);
    CHECK_EQ(inventory.found.size(), 1u);
    CHECK(inventory.found[kEngineAddress] == std::string("\x02") + "ENGINE*1.0.3*");
}

// P6: the test engineer checks what went on the wire and what the stack did, without reaching
// into it: DM13 stops the engine's periodic status (not the cab's requests), the Monitor sees it.
TEST_CASE(p6_dm13_silences_the_engine_status_and_the_test_sees_it)
{
    Vehicle v;
    J1939Events events(v.bus.network("cab"));
    FrameLog wire(v.bus.tap("engine"));
    auto* status = v.engine_ecu.find<j1939::PeriodicSender>();
    v.bus.run_for(1s);
    v.bus.host("engine").post([&] { j1939::BroadcastSuppression::of(v.engine_ecu).suspend(std::chrono::seconds(1)); });
    v.bus.run_for(2s);
    const auto statusFrames = [&](SimTime from, SimTime until) {
        return wire.count(FrameLog::Direction::Sent, [&](const can::TapRecord& r) {
            const auto id = j1939::Identifier::decode(r.frame);
            return id && id->pgn == kPgnEngineStatus && r.time >= from && r.time < until;
        });
    };
    CHECK_EQ(statusFrames(SimTime{} + 1100ms, SimTime{} + 1900ms), 0u);
    CHECK(statusFrames(SimTime{} + 2100ms, SimTime{} + 3000ms) > 40u);
    CHECK(status != nullptr && status->statistics().suppressed >= 49u);
    CHECK_EQ(v.cab.find<j1939::Monitor>()->losses(), 1u);
    // The cab saw the announcement of the suspension, as a message from the engine.
    CHECK(events.has(j1939::EventKind::MessageReceived));
    CHECK_EQ(events.count(j1939::EventKind::MessageReceived,
                          [](const J1939Events::Record& r) { return r.event.pgn == j1939::Pgn{0xDF00}; }),
             1u);
}
