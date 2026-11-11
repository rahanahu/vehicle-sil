// SPDX-License-Identifier: MIT
//
// The test kit (vehicle_sil::testkit): J1939 networks on a local virtual bus, the recorders of
// events and raw frames, fault injection and outages, and the checks a test makes with them.
// Built without SIL Kit.
#include "test_harness.hpp"

#include "vehicle_sil/testkit/faults.hpp"
#include "vehicle_sil/testkit/local_can.hpp"
#include "vehicle_sil/testkit/recorders.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::unit::throws;
namespace can = vehicle_sil::can;
namespace j1939 = vehicle_sil::j1939;
namespace testkit = vehicle_sil::testkit;
namespace faults = vehicle_sil::testkit::faults;
using j1939::EventKind;
using testkit::FrameLog;
using testkit::J1939Events;
using testkit::LocalCan;

namespace {

constexpr j1939::Pgn kPgnEec1{0xF004};
constexpr j1939::Pgn kPgnPropA{0xEF00};
constexpr j1939::Pgn kPgnPropB{0xFF10};
using j1939::kPgnAddressClaimed;
using j1939::kPgnTransportControl;
using j1939::kPgnTransportData;
constexpr j1939::Address kEngine = 0x80;
const j1939::Name kEngineName{0x20008200FE001234u};
const j1939::Name kRivalName{0x20008200FE001000u}; ///< lower: wins against kEngineName

SimTime at(std::chrono::nanoseconds t) { return SimTime{} + t; }

std::vector<std::uint8_t> bytes(std::size_t n, std::uint8_t seed = 0)
{
    std::vector<std::uint8_t> out(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        out[i] = static_cast<std::uint8_t>(seed + i * 3);
    }
    return out;
}

j1939::Node& add_named(j1939::Network& network, j1939::Address address, j1939::Name name)
{
    j1939::NodeOptions options;
    options.address = address;
    options.name = name;
    return network.add_node(options);
}

/// An engine that sends EEC1 every 10 ms once it may (send() holds it during the claim wait).
void install_engine(LocalCan& bus, const std::string& name, SimDuration step)
{
    auto& network = bus.add_j1939(name, step);
    auto& ecu = add_named(network, kEngine, kEngineName);
    bus.on_step(name, [&ecu](SimTime now) {
        if ((now - SimTime{}) % 10ms == SimDuration::zero() && ecu.address_state() != j1939::AddressState::CannotClaim)
        {
            const std::uint8_t data[8]{1, 2, 3, 4, 5, 6, 7, 8};
            static_cast<void>(ecu.send(kPgnEec1, data));
        }
    });
}

bool is_pgn(const J1939Events::Record& r, j1939::Pgn pgn)
{
    if (r.event.kind == EventKind::FrameSent)
    {
        const auto id = j1939::Identifier::decode(r.event.frame);
        return id && id->pgn == pgn;
    }
    return r.event.pgn == pgn;
}

/// When the engine lost its address (the time of the change to CannotClaim).
std::optional<SimTime> address_lost(const J1939Events& events)
{
    const auto* lost = events.first(EventKind::AddressChanged, [](const J1939Events::Record& r) {
        return r.event.address.state == j1939::AddressState::CannotClaim;
    });
    return lost != nullptr ? std::optional<SimTime>(lost->event.time) : std::nullopt;
}

} // namespace

TEST_CASE(testkit_engine_stops_sending_when_its_address_is_lost)
{
    LocalCan bus;
    install_engine(bus, "engine", 10ms);
    auto& rival = bus.add_j1939("rival", 5ms);
    bus.on_step("rival", [&rival](SimTime now) {
        if (now == at(300ms))
        {
            add_named(rival, kEngine, kRivalName); // claims in its next step
        }
    });
    J1939Events events(bus.network("engine"));
    FrameLog raw(bus.tap("engine"));
    bus.run_until(at(1s));

    const auto lost = address_lost(events);
    CHECK(lost.has_value());
    CHECK(*lost == at(305ms)); // the rival's claim, sent in its step at 305 ms
    const auto eec1 = [](const J1939Events::Record& r) { return is_pgn(r, kPgnEec1); };
    CHECK(events.count(EventKind::FrameSent, eec1) > 0u);
    CHECK_EQ(events.count(EventKind::FrameSent, [&](const J1939Events::Record& r) { return eec1(r) && r.event.time > *lost; }),
             0u);
    // The raw frames agree: no EEC1 on the bus after the loss, and the claims the engine sent.
    const auto sentEec1After = raw.count(FrameLog::Direction::Sent, [&](const can::TapRecord& r) {
        const auto id = j1939::Identifier::decode(r.frame);
        return id && id->pgn == kPgnEec1 && r.time > *lost;
    });
    CHECK_EQ(sentEec1After, 0u);
    CHECK(raw.count(FrameLog::Direction::Received, [](const can::TapRecord& r) {
              const auto id = j1939::Identifier::decode(r.frame);
              return id && id->pgn == kPgnAddressClaimed && id->source == kEngine;
          }) >= 1u);
    CHECK_EQ(events.dropped(), 0u);
    CHECK_EQ(raw.dropped(), 0u);
}

TEST_CASE(testkit_lost_claim_delays_the_address_loss_until_the_violation)
{
    // The rival's first claim is dropped on the bus: the engine keeps sending from the address,
    // the rival sees that as a violation and claims again, and only then the engine loses it.
    LocalCan bus;
    install_engine(bus, "engine", 10ms);
    auto& rival = bus.add_j1939("rival", 5ms);
    bus.on_step("rival", [&rival](SimTime now) {
        if (now == at(300ms))
        {
            add_named(rival, kEngine, kRivalName);
        }
    });
    bus.endpoint("rival").set_fault_injector(faults::nth(1, can::FaultAction::drop(), faults::j1939_pgn(kPgnAddressClaimed)));
    J1939Events events(bus.network("engine"));
    J1939Events rivalEvents(bus.network("rival"));
    bus.run_until(at(1s));

    const auto lost = address_lost(events);
    CHECK(lost.has_value());
    CHECK(*lost > at(305ms));
    CHECK(*lost <= at(330ms));
    // The rival sent two claims: the dropped one, and the one after the violation.
    CHECK_EQ(rivalEvents.count(EventKind::FrameSent, [](const J1939Events::Record& r) { return is_pgn(r, kPgnAddressClaimed); }),
             2u);
    CHECK(rivalEvents.count(EventKind::MessageReceived, [](const J1939Events::Record& r) {
              return is_pgn(r, kPgnEec1) && r.event.time > at(305ms);
          }) >= 1u);
}

TEST_CASE(testkit_dropped_broadcast_packet_is_lost_at_the_receiver)
{
    LocalCan bus;
    auto& sender = bus.add_j1939("sender", 10ms).add_node({0x20});
    auto& receiver = bus.add_j1939("receiver", 10ms);
    receiver.add_node({0x30});
    bus.on_step("sender", [&sender](SimTime now) {
        if (now == at(0ms) || now == at(1s))
        {
            static_cast<void>(sender.send(kPgnPropB, bytes(20)));
        }
    });
    // The second packet of the first broadcast is lost; the second broadcast is complete.
    bus.endpoint("sender").set_fault_injector(faults::nth(2, can::FaultAction::drop(), faults::j1939_pgn(kPgnTransportData)));
    J1939Events sent(bus.network("sender"));
    J1939Events received(bus.network("receiver"));
    bus.run_until(at(2s));

    const auto ends = received.of(EventKind::TransferEnded);
    CHECK_EQ(ends.size(), 2u);
    CHECK(ends[0].event.direction == j1939::TransferDirection::Receive);
    CHECK(ends[0].event.result == j1939::TransferResult::Lost);
    CHECK(ends[0].event.reason == j1939::AbortReason::BadSequence);
    CHECK(ends[1].event.result == j1939::TransferResult::Completed);
    const auto messages = received.of(EventKind::MessageReceived);
    CHECK_EQ(messages.size(), 1u);
    CHECK(messages[0].data == bytes(20));
    CHECK(messages[0].event.time >= at(1s));
    // The sender does not know: both broadcasts completed on its side.
    CHECK_EQ(sent.count(EventKind::TransferEnded, [](const J1939Events::Record& r) {
                 return r.event.result == j1939::TransferResult::Completed;
             }),
             2u);
}

TEST_CASE(testkit_dropped_connection_packet_is_sent_again)
{
    LocalCan bus;
    auto& sender = bus.add_j1939("sender", 10ms).add_node({0x20});
    bus.add_j1939("receiver", 10ms).add_node({0x30});
    bus.on_step("sender", [&sender](SimTime now) {
        if (now == at(0ms))
        {
            static_cast<void>(sender.send(kPgnPropA, bytes(40), 0x30));
        }
    });
    bus.endpoint("sender").set_fault_injector(faults::nth(3, can::FaultAction::drop(), faults::j1939_pgn(kPgnTransportData)));
    J1939Events sent(bus.network("sender"));
    J1939Events received(bus.network("receiver"));
    FrameLog raw(bus.tap("receiver"));
    bus.run_until(at(3s));

    CHECK(received.count(EventKind::TransferEnded, [](const J1939Events::Record& r) {
              return r.event.result == j1939::TransferResult::Completed;
          }) == 1u);
    CHECK(sent.count(EventKind::TransferEnded, [](const J1939Events::Record& r) {
              return r.event.result == j1939::TransferResult::Completed;
          }) == 1u);
    const auto* message = received.first(EventKind::MessageReceived);
    CHECK(message != nullptr);
    CHECK(message->data == bytes(40));
    // The receiver asked for the packet again: a second clear-to-send among its frames.
    const auto cts = raw.count(FrameLog::Direction::Sent, [](const can::TapRecord& r) {
        const auto id = j1939::Identifier::decode(r.frame);
        return id && id->pgn == kPgnTransportControl && r.frame.data[0] == 17;
    });
    CHECK(cts >= 2u);
}

TEST_CASE(testkit_outage_cuts_one_ecu_off_for_a_span)
{
    LocalCan bus;
    auto& sender = bus.add_j1939("sender", 10ms).add_node({0x20});
    bus.add_j1939("receiver", 10ms).add_node({0x30});
    bus.on_step("sender", [&sender](SimTime) {
        const std::uint8_t data[1]{7};
        static_cast<void>(sender.send(kPgnPropB, data));
    });
    bus.endpoint("receiver").add_outage(at(200ms), at(400ms), can::OutageDirection::Receive);
    J1939Events received(bus.network("receiver"));
    bus.run_until(at(600ms));
    const auto in = [&](SimTime from, SimTime until) {
        return received.count(EventKind::MessageReceived, [&](const J1939Events::Record& r) {
            return r.event.time >= from && r.event.time < until;
        });
    };
    CHECK_EQ(in(at(0ms), at(200ms)), 20u);
    CHECK_EQ(in(at(200ms), at(400ms)), 0u);
    CHECK(in(at(400ms), at(600ms)) >= 19u);
    CHECK_EQ(bus.endpoint("receiver").outage_dropped(), 20u);
}

TEST_CASE(testkit_injects_raw_frames_of_both_formats)
{
    LocalCan bus;
    bus.add_j1939("ecu", 10ms).add_node({0x30});
    J1939Events events(bus.network("ecu"));
    FrameLog raw(bus.tap("ecu"));
    can::Frame standard;
    standard.id = 0x123;
    standard.format = can::IdFormat::Standard;
    standard.length = 2;
    can::Frame j1939;
    j1939.id = j1939::Identifier{6, kPgnPropB, j1939::kGlobalAddress, 0x44}.encode();
    j1939.format = can::IdFormat::Extended;
    j1939.length = 3;
    CHECK(bus.inject(at(0ms), standard) == can::SendResult::Accepted);
    CHECK(bus.inject(at(0ms), j1939) == can::SendResult::Accepted);
    bus.run_until(at(30ms));
    const auto rx = raw.of(FrameLog::Direction::Received);
    CHECK_EQ(rx.size(), 2u);
    CHECK(rx[0].frame == standard);
    CHECK(rx[1].frame == j1939);
    CHECK(rx[0].time == at(0ms));
    // The 11-bit frame is not J1939: counted and ignored.
    CHECK_EQ(events.count(EventKind::MessageReceived), 1u);
    CHECK_EQ(bus.network("ecu").statistics().frames_ignored, 1u);
    // Injecting into the past is refused.
    CHECK(throws<std::logic_error>([&] { static_cast<void>(bus.inject(at(10ms), j1939)); }));
    CHECK(bus.inject(bus.now(), j1939) == can::SendResult::Accepted);
}

namespace {

/// What the receiver's handlers see, as text. Without a connection to the receiver (which
/// answers with clear-to-send frames from its own steps) it does not depend on the receiver's
/// step size.
std::vector<std::string> receiver_trace(SimDuration receiverStep, bool recorders, bool timing, bool connection)
{
    can::VirtualCanBusOptions options;
    if (timing)
    {
        options.timing = can::VirtualCanBusTiming{};
    }
    LocalCan bus(options);
    install_engine(bus, "engine", 10ms);
    auto& sender = bus.add_j1939("sender", 5ms).add_node({0x20});
    bus.on_step("sender", [&sender, connection](SimTime now) {
        if ((now - SimTime{}) % 200ms == SimDuration::zero())
        {
            static_cast<void>(sender.send(kPgnPropB, bytes(30, static_cast<std::uint8_t>((now - SimTime{}) / 1ms))));
            if (connection)
            {
                static_cast<void>(sender.send(kPgnPropA, bytes(50), 0x30));
            }
        }
    });
    auto& receiverNetwork = bus.add_j1939("receiver", receiverStep);
    auto& receiver = receiverNetwork.add_node({0x30});
    std::vector<std::string> trace;
    receiver.on_any_message([&trace](const j1939::Message& m) {
        if (m.time >= at(1400ms))
        {
            return; // a receiver with a coarse step handles these after the end of the run
        }
        trace.push_back(std::to_string(m.pgn.value()) + " " + std::to_string(m.source) + " " +
                        std::to_string((m.time - SimTime{}) / 1us) + " " + std::to_string(m.data.size()) + " " +
                        std::to_string(m.data.empty() ? 0 : m.data[0]));
    });
    bus.endpoint("sender").set_fault_injector(
        faults::first_of({faults::nth(5, can::FaultAction::drop(), faults::j1939_pgn(kPgnTransportData)),
                          faults::between(at(600ms), at(650ms), can::FaultAction::delay_by(3ms))}));
    std::unique_ptr<J1939Events> events;
    std::unique_ptr<FrameLog> raw;
    if (recorders)
    {
        events = std::make_unique<J1939Events>(receiverNetwork);
        raw = std::make_unique<FrameLog>(bus.tap("receiver"));
    }
    bus.run_until(at(1500ms));
    return trace;
}

} // namespace

TEST_CASE(testkit_results_do_not_depend_on_the_receiver_step_or_the_recorders)
{
    for (const bool timing : {false, true})
    {
        const auto reference = receiver_trace(10ms, false, timing, false);
        CHECK(reference.size() > 100u);
        for (const SimDuration step : {SimDuration{1ms}, SimDuration{3ms}, SimDuration{10ms}, SimDuration{20ms}})
        {
            CHECK(receiver_trace(step, true, timing, false) == reference);
            CHECK(receiver_trace(step, false, timing, false) == reference);
            // With a connection to the receiver, its step changes the times, but the recorders
            // still change nothing.
            const auto withConnection = receiver_trace(step, false, timing, true);
            CHECK(withConnection.size() > reference.size());
            CHECK(receiver_trace(step, true, timing, true) == withConnection);
        }
    }
}

TEST_CASE(testkit_setup_errors)
{
    LocalCan bus;
    bus.add_j1939("a", 10ms);
    CHECK(throws<std::invalid_argument>([&] { bus.add_j1939("a", 10ms); }));
    CHECK(throws<std::invalid_argument>([&] { bus.add_j1939("", 10ms); }));
    CHECK(throws<std::invalid_argument>([&] { bus.add_j1939("b", 0ms); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(bus.network("missing")); }));
    CHECK(throws<std::out_of_range>([&] { bus.on_step("missing", [](SimTime) {}); }));
    CHECK(throws<std::invalid_argument>([&] { bus.on_step("a", {}); }));
    bus.run_for(20ms);
    CHECK(bus.now() == at(20ms));
    CHECK(throws<std::logic_error>([&] { bus.add_j1939("c", 10ms); }));
    CHECK(throws<std::logic_error>([&] { bus.on_step("a", [](SimTime) {}); }));
    // A failing application fails its host; the exception reaches the test.
    LocalCan failing;
    failing.add_j1939("x", 10ms);
    failing.on_step("x", [](SimTime now) {
        if (now == at(20ms))
        {
            throw std::runtime_error("model failed");
        }
    });
    CHECK(throws<std::runtime_error>([&] { failing.run_until(at(50ms)); }));
    CHECK(failing.host("x").failed());
}

TEST_CASE(testkit_recorders_keep_up_to_their_capacity)
{
    LocalCan bus;
    auto& sender = bus.add_j1939("sender", 10ms).add_node({0x20});
    bus.add_j1939("receiver", 10ms).add_node({0x30});
    bus.on_step("sender", [&sender](SimTime) {
        const std::uint8_t data[1]{7};
        static_cast<void>(sender.send(kPgnPropB, data));
    });
    J1939Events events(bus.network("receiver"), 5);
    FrameLog raw(bus.tap("receiver"), 3);
    bus.run_until(at(100ms));
    CHECK_EQ(events.all().size(), 5u);
    CHECK_EQ(events.dropped(), 4u);
    CHECK_EQ(raw.all().size(), 3u);
    CHECK_EQ(raw.dropped(), 6u);
    events.clear();
    raw.clear();
    CHECK(events.all().empty());
    CHECK_EQ(events.dropped(), 0u);
    CHECK(events.first(EventKind::MessageReceived) == nullptr);
    bus.run_until(at(120ms));
    CHECK_EQ(events.count(EventKind::MessageReceived), 2u);
    CHECK(events.last(EventKind::MessageReceived)->event.time == at(100ms));
    CHECK_EQ(raw.count(FrameLog::Direction::Received), 2u);
}

TEST_CASE(testkit_fault_builders)
{
    const auto f = [](std::uint32_t id, SimTime t, can::IdFormat format = can::IdFormat::Extended) {
        can::Frame frame;
        frame.id = id;
        frame.format = format;
        return can::TimedFrame{t, frame};
    };
    const std::uint32_t eec1 = j1939::Identifier{3, kPgnEec1, j1939::kGlobalAddress, 0x00}.encode();
    const std::uint32_t propA = j1939::Identifier{6, kPgnPropA, 0x30, 0x20}.encode();
    using Kind = can::FaultAction::Kind;

    CHECK(faults::j1939_pgn(kPgnEec1)(f(eec1, at(0ms))));
    CHECK(faults::j1939_pgn(kPgnPropA)(f(propA, at(0ms)))); // PDU1: without the destination
    CHECK(!faults::j1939_pgn(kPgnEec1)(f(eec1, at(0ms), can::IdFormat::Standard)));
    CHECK(faults::j1939_source(0x20)(f(propA, at(0ms))));
    CHECK(!faults::j1939_source(0x00)(f(propA, at(0ms))));
    CHECK(faults::can_id(0x123, can::IdFormat::Standard)(f(0x123, at(0ms), can::IdFormat::Standard)));
    CHECK(!faults::can_id(0x123, can::IdFormat::Standard)(f(0x123, at(0ms))));

    const auto window = faults::between(at(10ms), at(20ms), can::FaultAction::drop());
    CHECK(window(f(eec1, at(9ms))).kind == Kind::Deliver);
    CHECK(window(f(eec1, at(10ms))).kind == Kind::Drop);
    CHECK(window(f(eec1, at(20ms))).kind == Kind::Deliver);
    CHECK(throws<std::invalid_argument>([] { static_cast<void>(faults::between(at(1ms), at(1ms), can::FaultAction::drop())); }));

    const auto second = faults::nth(2, can::FaultAction::duplicate(), faults::j1939_pgn(kPgnEec1));
    CHECK(second(f(eec1, at(0ms))).kind == Kind::Deliver);
    CHECK(second(f(propA, at(0ms))).kind == Kind::Deliver); // not counted
    CHECK(second(f(eec1, at(0ms))).kind == Kind::Duplicate);
    CHECK(second(f(eec1, at(0ms))).kind == Kind::Deliver);
    CHECK(throws<std::invalid_argument>([] { static_cast<void>(faults::nth(0, can::FaultAction::drop())); }));

    const auto seq = faults::sequence({can::FaultAction::queue_full(), can::FaultAction::delay_by(2ms)});
    CHECK(seq(f(eec1, at(0ms))).kind == Kind::QueueFull);
    const auto delayed = seq(f(eec1, at(0ms)));
    CHECK(delayed.kind == Kind::Delay);
    CHECK(delayed.delay == SimDuration{2ms});
    CHECK(seq(f(eec1, at(0ms))).kind == Kind::Deliver);

    const auto both = faults::first_of({faults::when(faults::j1939_pgn(kPgnPropA), can::FaultAction::failed()),
                                        faults::when({}, can::FaultAction::transmit_failed()), {}});
    CHECK(both(f(propA, at(0ms))).kind == Kind::Failed);
    CHECK(both(f(eec1, at(0ms))).kind == Kind::TransmitFailed);
    CHECK(faults::first_of({})(f(eec1, at(0ms))).kind == Kind::Deliver);
}

TEST_CASE(testkit_runs_every_step_function_in_order)
{
    LocalCan bus;
    bus.add_j1939("a", 10ms);
    std::vector<std::string> calls;
    bus.on_step("a", [&](SimTime now) { calls.push_back("first " + std::to_string((now - SimTime{}) / 1ms)); });
    bus.on_step("a", [&](SimTime now) { calls.push_back("second " + std::to_string((now - SimTime{}) / 1ms)); });
    bus.run_until(at(20ms));
    CHECK((calls == std::vector<std::string>{"first 0", "second 0", "first 10", "second 10"}));
    const std::size_t endpoints = bus.bus().endpoint_count();
    CHECK(throws<std::logic_error>([&] { bus.add_j1939("b", 10ms); }));
    CHECK_EQ(bus.bus().endpoint_count(), endpoints); // nothing half made
}
