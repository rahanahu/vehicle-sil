// SPDX-License-Identifier: MIT
//
// The Observer of the J1939 stack: what it reports (address changes, send requests,
// dropped messages, frames handed to the port, received messages, transfers on both sides
// including the receive side's failures, port events), that it is read only, that it changes
// nothing, and its lifetime.
#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"

#include "vehicle_sil/j1939/observer.hpp"
#include "vehicle_sil/j1939/toolbox.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::unit::throws;
using vehicle_sil::j1939::AbortReason;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::DropReason;
using vehicle_sil::j1939::Event;
using vehicle_sil::j1939::EventKind;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::NodeOptions;
using vehicle_sil::j1939::Observer;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::Subscription;
using vehicle_sil::j1939::Timer;
using vehicle_sil::j1939::TimerKind;
using vehicle_sil::j1939::TransferDirection;
using vehicle_sil::j1939::TransferResult;
namespace can = vehicle_sil::can;

namespace {

constexpr Address kA = 0x10;
constexpr Address kB = 0x20;
constexpr Address kRemote = 0x30;
constexpr Address kUs = 0x80; ///< in 128..247: waits 250 ms after the claim
constexpr std::uint64_t kOurName = 0x20008200FE001234u;
constexpr std::uint64_t kLowerName = 0x20008200FE001000u; ///< wins against kOurName

Bytes name_bytes(std::uint64_t name)
{
    Bytes out;
    for (int i = 0; i < 8; ++i)
    {
        out.push_back(static_cast<std::uint8_t>(name >> (8 * i)));
    }
    return out;
}

J1939Frame claim(Address sa, std::uint64_t name) { return frame(6, kPgnAddressClaim, sa, kGlobal, name_bytes(name)); }

/// Keeps a copy of every event (with its data).
struct Recorder final : Observer
{
    void on_event(const Event& e) override
    {
        Event copy = e;
        copy.data = {};
        events.push_back(copy);
        data.emplace_back(e.data.begin(), e.data.end());
        if (during)
        {
            during(e);
        }
    }
    [[nodiscard]] std::vector<Event> of(EventKind kind) const
    {
        std::vector<Event> out;
        for (const auto& e : events)
        {
            if (e.kind == kind)
            {
                out.push_back(e);
            }
        }
        return out;
    }
    [[nodiscard]] std::size_t index_of(EventKind kind, std::size_t from = 0) const
    {
        for (std::size_t i = from; i < events.size(); ++i)
        {
            if (events[i].kind == kind)
            {
                return i;
            }
        }
        return events.size();
    }
    std::vector<Event> events;
    std::vector<Bytes> data;
    std::function<void(const Event&)> during;
};

Node& add_claiming(Bench& bench, Address address, std::uint64_t name)
{
    NodeOptions options;
    options.address = address;
    options.name = Name{name};
    return bench.network.add_node(options);
}

std::uint32_t pgn_of(const can::Frame& f) { return from_can(f).pgn; }

} // namespace

TEST_CASE(observer_reports_the_claim_the_held_send_and_the_frames)
{
    Bench bench;
    Recorder rec;
    bench.network.set_observer(&rec);
    CHECK(bench.network.observer() == &rec);
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            const Bytes data = payload(8);
            CHECK(us.send(Pgn{kPgnPropB}, data) == SendStatus::Ok);
        }
    };
    bench.steps(30);

    const auto changes = rec.of(EventKind::AddressChanged);
    CHECK_EQ(changes.size(), 2u);
    CHECK(changes[0].address.previous_state == AddressState::NotStarted);
    CHECK(changes[0].address.state == AddressState::Claiming);
    CHECK_EQ(changes[0].address.address, kUs);
    CHECK(changes[0].time == at(0ms));
    CHECK_EQ(changes[0].node, 0);
    CHECK(changes[1].address.state == AddressState::Claimed);
    CHECK(changes[1].time == at(250ms));

    const auto requests = rec.of(EventKind::SendRequested);
    CHECK_EQ(requests.size(), 1u);
    CHECK(requests[0].status == SendStatus::Ok);
    CHECK(requests[0].held);
    CHECK_EQ(requests[0].pgn.value(), kPgnPropB);
    CHECK_EQ(requests[0].size, 8u);
    CHECK(requests[0].time == at(0ms));
    CHECK(rec.data[rec.index_of(EventKind::SendRequested)] == payload(8));

    const auto sent = rec.of(EventKind::FrameSent);
    CHECK_EQ(sent.size(), 2u);
    CHECK_EQ(pgn_of(sent[0].frame), kPgnAddressClaim);
    CHECK(sent[0].time == at(0ms));
    CHECK_EQ(pgn_of(sent[1].frame), kPgnPropB);
    CHECK(sent[1].time == at(250ms));
    CHECK(sent[1].send_result == can::SendResult::Accepted);
    CHECK_EQ(sent[1].node, 0);
}

TEST_CASE(observer_reports_send_errors_and_requests)
{
    Bench bench;
    Recorder rec;
    bench.network.set_observer(&rec);
    Node& a = bench.add_node(kA);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            const Bytes data = payload(4);
            CHECK(a.send(Pgn{kPgnPropB}, data, kB) == SendStatus::InvalidArgument); // PDU2 to one node
            CHECK(a.send(Pgn{kPgnPropA}, payload(1786), kB) == SendStatus::TooLong);
            CHECK(a.request(Pgn{0xFEDA}, kB) == SendStatus::Ok);
        }
    };
    bench.steps(2);
    const auto requests = rec.of(EventKind::SendRequested);
    CHECK_EQ(requests.size(), 3u);
    CHECK(requests[0].status == SendStatus::InvalidArgument);
    CHECK_EQ(requests[0].destination, kB);
    CHECK(requests[1].status == SendStatus::TooLong);
    CHECK_EQ(requests[1].size, 1786u);
    CHECK(requests[2].status == SendStatus::Ok);
    CHECK(!requests[2].held);
    CHECK_EQ(requests[2].pgn.value(), kPgnRequest);
    CHECK_EQ(requests[2].destination, kB);
    CHECK(rec.data[rec.index_of(EventKind::SendRequested, rec.index_of(EventKind::SendRequested) + 2)] == request(0xFEDA));
    // No transfer started for a refused long message.
    CHECK(rec.of(EventKind::TransferStarted).empty());
}

TEST_CASE(observer_reports_received_messages_external_and_local)
{
    Bench bench;
    Recorder rec;
    bench.network.set_observer(&rec);
    Node& a = bench.add_node(kA);
    bench.add_node(kB);
    bench.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1, 2, 3}));
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            const Bytes data{9, 8};
            CHECK(a.send(Pgn{kPgnPropA}, data, kB) == SendStatus::Ok);
        }
    };
    bench.steps(4);
    const auto received = rec.of(EventKind::MessageReceived);
    CHECK_EQ(received.size(), 2u);
    CHECK(!received[0].local);
    CHECK_EQ(received[0].node, -1);
    CHECK_EQ(received[0].source, kRemote);
    CHECK_EQ(received[0].pgn.value(), kPgnPropB);
    CHECK(received[0].time == at(0ms));
    CHECK(rec.data[rec.index_of(EventKind::MessageReceived)] == (Bytes{1, 2, 3}));
    CHECK(received[1].local);
    CHECK_EQ(received[1].node, 0);
    CHECK_EQ(received[1].source, kA);
    CHECK_EQ(received[1].destination, kB);
    CHECK(received[1].time == at(10ms));
}

TEST_CASE(observer_reports_a_broadcast_sent_and_queue_full_results)
{
    Bench bench;
    Recorder rec;
    bench.network.set_observer(&rec);
    Node& a = bench.add_node(kA);
    int refusals = 1;
    bench.port.set_fault_injector([&](const can::TimedFrame&) {
        return refusals-- > 0 ? can::FaultAction::queue_full() : can::FaultAction::deliver();
    });
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(a.send(Pgn{kPgnPropB}, payload(20)) == SendStatus::Ok);
        }
    };
    bench.steps(30);
    const auto started = rec.of(EventKind::TransferStarted);
    CHECK_EQ(started.size(), 1u);
    CHECK(started[0].direction == TransferDirection::Send);
    CHECK_EQ(started[0].size, 20u);
    CHECK_EQ(started[0].destination, kGlobal);
    CHECK_EQ(started[0].node, 0);
    // SendRequested comes first, then the start of the transfer.
    CHECK(rec.index_of(EventKind::SendRequested) < rec.index_of(EventKind::TransferStarted));
    const auto ended = rec.of(EventKind::TransferEnded);
    CHECK_EQ(ended.size(), 1u);
    CHECK(ended[0].direction == TransferDirection::Send);
    CHECK(ended[0].result == TransferResult::Completed);
    CHECK_EQ(ended[0].size, 20u);
    const auto sent = rec.of(EventKind::FrameSent);
    CHECK_EQ(sent.size(), 5u); // the refused announcement, the announcement again, 3 packets
    CHECK(sent[0].send_result == can::SendResult::QueueFull);
    CHECK(sent[1].send_result == can::SendResult::Accepted);
    CHECK(sent[0].frame == sent[1].frame);
    CHECK_EQ(pgn_of(sent[2].frame), kPgnTpDt);
}

TEST_CASE(observer_reports_a_broadcast_received)
{
    Bench bench;
    Recorder rec;
    bench.network.set_observer(&rec);
    bench.add_node(kA);
    const Bytes message = payload(20);
    bench.inject(bam(kRemote, 20, kPgnPropB));
    bench.step();
    for (const auto& f : dts(kRemote, kGlobal, message, 1, 3))
    {
        bench.inject(f);
        bench.step();
    }
    bench.step();
    const std::size_t start = rec.index_of(EventKind::TransferStarted);
    const std::size_t end = rec.index_of(EventKind::TransferEnded);
    const std::size_t received = rec.index_of(EventKind::MessageReceived);
    CHECK(start < end);
    CHECK(end < received);
    CHECK(received < rec.events.size());
    CHECK(rec.events[start].direction == TransferDirection::Receive);
    CHECK_EQ(rec.events[start].node, -1);
    CHECK_EQ(rec.events[start].source, kRemote);
    CHECK_EQ(rec.events[start].size, 20u);
    CHECK(rec.events[start].time == at(0ms));
    CHECK(rec.events[end].result == TransferResult::Completed);
    CHECK(rec.events[end].time == at(20ms));
    CHECK_EQ(rec.events[received].size, 20u);
    CHECK(rec.data[received] == message);
    // The transport frames themselves are not messages.
    CHECK_EQ(rec.of(EventKind::MessageReceived).size(), 1u);
}

TEST_CASE(observer_reports_the_failures_of_received_broadcasts)
{
    {
        // A packet is missing: the broadcast is lost (nothing sent).
        Bench bench;
        Recorder rec;
        bench.network.set_observer(&rec);
        bench.add_node(kA);
        const Bytes message = payload(20);
        bench.inject(bam(kRemote, 20, kPgnPropB));
        bench.step();
        bench.inject(dt(kRemote, kGlobal, 1, message));
        bench.step();
        bench.inject(dt(kRemote, kGlobal, 3, message));
        bench.steps(2);
        const auto ended = rec.of(EventKind::TransferEnded);
        CHECK_EQ(ended.size(), 1u);
        CHECK(ended[0].direction == TransferDirection::Receive);
        CHECK(ended[0].result == TransferResult::Lost);
        CHECK(ended[0].reason == AbortReason::BadSequence);
        CHECK(ended[0].time == at(10ms));
        CHECK(rec.of(EventKind::MessageReceived).empty());
        CHECK(rec.of(EventKind::FrameSent).empty());
    }
    {
        // No packet after the announcement: lost after T1 (750 ms).
        Bench bench;
        Recorder rec;
        bench.network.set_observer(&rec);
        bench.add_node(kA);
        bench.inject(bam(kRemote, 20, kPgnPropB));
        bench.steps(80);
        const auto ended = rec.of(EventKind::TransferEnded);
        CHECK_EQ(ended.size(), 1u);
        CHECK(ended[0].result == TransferResult::Lost);
        CHECK(ended[0].reason == AbortReason::Timeout);
        CHECK(ended[0].time == at(750ms));
    }
}

TEST_CASE(observer_reports_the_failures_of_received_connections)
{
    {
        // The originator sends no packet after our clear-to-send: we abort after T2.
        Bench bench;
        Recorder rec;
        bench.network.set_observer(&rec);
        bench.add_node(kA);
        bench.inject(rts(kRemote, kA, 20, kPgnPropA));
        bench.steps(200);
        const auto started = rec.of(EventKind::TransferStarted);
        CHECK_EQ(started.size(), 1u);
        CHECK(started[0].direction == TransferDirection::Receive);
        CHECK_EQ(started[0].node, 0);
        CHECK_EQ(started[0].destination, kA);
        const auto ended = rec.of(EventKind::TransferEnded);
        CHECK_EQ(ended.size(), 1u);
        CHECK(ended[0].result == TransferResult::Aborted);
        CHECK(ended[0].reason == AbortReason::Timeout);
        CHECK_EQ(ended[0].node, 0);
        // The clear-to-send and the abort left as frames.
        const auto sent = rec.of(EventKind::FrameSent);
        CHECK_EQ(sent.size(), 2u);
        CHECK_EQ(from_can(sent[1].frame).data[0], 255);
    }
    {
        // The originator aborts.
        Bench bench;
        Recorder rec;
        bench.network.set_observer(&rec);
        bench.add_node(kA);
        bench.inject(rts(kRemote, kA, 20, kPgnPropA));
        bench.steps(2);
        bench.inject(conn_abort(kRemote, kA, 3, kPgnPropA));
        bench.steps(2);
        const auto ended = rec.of(EventKind::TransferEnded);
        CHECK_EQ(ended.size(), 1u);
        CHECK(ended[0].result == TransferResult::AbortedByPeer);
        CHECK(ended[0].reason == AbortReason::Timeout);
        CHECK(ended[0].time == at(10ms));
    }
    {
        // Another NAME claims the originator's address: the connection is cancelled.
        Bench bench;
        Recorder rec;
        bench.network.set_observer(&rec);
        bench.add_node(kA);
        bench.inject(claim(kRemote, kOurName));
        bench.step();
        bench.inject(rts(kRemote, kA, 20, kPgnPropA));
        bench.steps(2);
        bench.inject(claim(kRemote, kLowerName));
        bench.steps(2);
        const auto ended = rec.of(EventKind::TransferEnded);
        CHECK_EQ(ended.size(), 1u);
        CHECK(ended[0].result == TransferResult::Cancelled);
        CHECK(ended[0].direction == TransferDirection::Receive);
    }
}

TEST_CASE(observer_reports_held_messages_dropped_when_the_address_is_lost)
{
    Bench bench;
    Recorder rec;
    bench.network.set_observer(&rec);
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgnPropB}, Bytes{1, 2}) == SendStatus::Ok);
        }
    };
    bench.step();
    bench.inject(claim(kUs, kLowerName));
    bench.steps(30);
    const auto dropped = rec.of(EventKind::MessageDropped);
    CHECK_EQ(dropped.size(), 1u);
    CHECK(dropped[0].drop_reason == DropReason::AddressChanged);
    CHECK_EQ(dropped[0].pgn.value(), kPgnPropB);
    CHECK_EQ(dropped[0].size, 2u);
    CHECK_EQ(dropped[0].node, 0);
    CHECK(dropped[0].time == at(0ms));
    // Dropped before the address handlers are told about the loss.
    const std::size_t lost = rec.index_of(EventKind::AddressChanged, rec.index_of(EventKind::AddressChanged) + 1);
    CHECK(rec.index_of(EventKind::MessageDropped) < lost);
    CHECK(rec.events[lost].address.state == AddressState::CannotClaim);
    for (const auto& e : rec.of(EventKind::FrameSent))
    {
        CHECK(pgn_of(e.frame) != kPgnPropB);
    }
}

TEST_CASE(observer_reports_port_events)
{
    Bench bench;
    Recorder rec;
    bench.network.set_observer(&rec);
    bench.add_node(kA);
    CHECK(bench.port.inject_event(can::PortEvent{can::PortEvent::Kind::BusStateChanged, at(5ms), 0, std::nullopt, 0,
                                                 can::BusState::BusOff}));
    bench.steps(2);
    const auto events = rec.of(EventKind::PortEvent);
    CHECK_EQ(events.size(), 1u);
    CHECK(events[0].port_event.bus_state == can::BusState::BusOff);
    CHECK(events[0].time == at(5ms));
}

TEST_CASE(observer_is_read_only)
{
    enum class Op
    {
        Send,
        Request,
        Register,
        Unsubscribe,
        StartTimer,
        CancelTimer,
        AddNode,
        SetObserver,
        Attach,
        Throw,
    };
    struct Part
    {
        explicit Part(Node&) {}
    };
    for (const Op op : {Op::Send, Op::Request, Op::Register, Op::Unsubscribe, Op::StartTimer, Op::CancelTimer, Op::AddNode,
                        Op::SetObserver, Op::Attach, Op::Throw})
    {
        Bench bench;
        Recorder rec;
        Node& a = bench.add_node(kA);
        Subscription subscription(a, a.on_message(Pgn{kPgnPropB}, [](const Message&) {}));
        Timer timer(bench.network, TimerKind::Scheduled, [] {});
        bench.network.set_observer(&rec);
        bool once = false;
        rec.during = [&](const Event&) {
            if (std::exchange(once, true))
            {
                return;
            }
            switch (op)
            {
            case Op::Send:
                static_cast<void>(a.send(Pgn{kPgnPropB}, Bytes{1}));
                break;
            case Op::Request:
                static_cast<void>(a.request(Pgn{0xFEDA}));
                break;
            case Op::Register:
                static_cast<void>(a.on_message(Pgn{kPgnPropB}, [](const Message&) {}));
                break;
            case Op::Unsubscribe:
                subscription.reset();
                break;
            case Op::StartTimer:
                timer.start_after(10ms);
                break;
            case Op::CancelTimer:
                CHECK(!timer.cancel());
                break;
            case Op::AddNode:
                static_cast<void>(bench.network.add_node({kB}));
                break;
            case Op::SetObserver:
                bench.network.set_observer(nullptr);
                break;
            case Op::Attach:
                static_cast<void>(a.attach<Part>());
                break;
            case Op::Throw:
                throw std::runtime_error("observer failed");
            }
        };
        bench.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
        CHECK(throws<std::exception>([&] { bench.steps(3); }));
        CHECK(bench.network.failed());
        CHECK(bench.network.error().find("observer") != std::string::npos);
    }
}

TEST_CASE(observer_is_set_outside_a_step_only)
{
    Bench bench;
    Recorder rec;
    bench.add_node(kA);
    bool threw = false;
    bench.app = [&](SimTime) { threw = threw || throws<std::logic_error>([&] { bench.network.set_observer(&rec); }); };
    bench.step();
    CHECK(threw);
    CHECK(bench.network.observer() == nullptr);
    bench.app = {};
    bench.network.set_observer(&rec); // between steps
    bench.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
    bench.step();
    CHECK_EQ(rec.of(EventKind::MessageReceived).size(), 1u);
    bench.network.set_observer(nullptr);
    CHECK(bench.network.observer() == nullptr);
}

namespace {

/// A run with claims, a held message, a request and its answer, a broadcast sent and one
/// received, a connection received, and an address lost: the frames sent and what the handlers
/// saw, as text.
std::vector<std::string> scenario_trace(Observer* observer, SimDuration step)
{
    Bench bench({}, step);
    if (observer != nullptr)
    {
        bench.network.set_observer(observer);
    }
    std::vector<std::string> trace;
    Node& us = add_claiming(bench, kUs, kOurName);
    Node& a = bench.add_node(kA);
    a.on_request(Pgn{0xFEDA}, [](vehicle_sil::j1939::RequestContext& r) { static_cast<void>(r.respond(payload(30))); });
    for (Node* n : {&us, &a})
    {
        n->on_any_message([&trace, n](const Message& m) {
            trace.push_back("rx " + std::to_string(n->address()) + " " + std::to_string(m.pgn.value()) + " " +
                            std::to_string(m.time.time_since_epoch().count()) + " " + std::to_string(m.data.size()));
        });
        n->on_transfer_end([&trace](const vehicle_sil::j1939::TransferEnd& e) {
            trace.push_back("end " + std::to_string(static_cast<int>(e.result)) + " " + std::to_string(e.time.time_since_epoch().count()));
        });
    }
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            static_cast<void>(us.send(Pgn{kPgnPropB}, payload(12)));
            static_cast<void>(a.send(Pgn{kPgnPropB}, payload(3)));
        }
        if (now >= at(300ms) && now < at(300ms) + step)
        {
            static_cast<void>(us.request(Pgn{0xFEDA}, kA));
        }
    };
    const Bytes message = payload(20);
    std::vector<std::pair<SimTime, J1939Frame>> script{{at(100ms), bam(kRemote, 20, kPgnPropB)}};
    for (int k = 1; k <= 3; ++k)
    {
        script.emplace_back(at(100ms) + k * 60ms, dt(kRemote, kGlobal, static_cast<std::uint8_t>(k), message));
    }
    script.emplace_back(at(500ms), rts(kRemote, kA, 20, kPgnPropA));
    script.emplace_back(at(900ms), claim(kUs, kLowerName));
    std::size_t next = 0;
    while (bench.next_time() < at(3s))
    {
        while (next < script.size() && script[next].first <= bench.last_time())
        {
            bench.inject(script[next].first < bench.last_time() ? bench.last_time() : script[next].first, script[next].second);
            ++next;
        }
        bench.step();
        for (const auto& f : bench.take_sent())
        {
            trace.push_back("tx " + to_string(f) + " " + std::to_string(bench.sent_times.back().time_since_epoch().count()));
        }
    }
    return trace;
}

} // namespace

TEST_CASE(observer_does_not_change_the_trace)
{
    for (const SimDuration step : {SimDuration{1ms}, SimDuration{10ms}})
    {
        Recorder rec;
        const auto with = scenario_trace(&rec, step);
        const auto without = scenario_trace(nullptr, step);
        CHECK(with == without);
        CHECK(with.size() > 20u);
        for (const EventKind kind : {EventKind::AddressChanged, EventKind::SendRequested, EventKind::FrameSent, EventKind::MessageReceived, EventKind::TransferStarted,
                                     EventKind::TransferEnded})
        {
            CHECK(!rec.of(kind).empty());
        }
    }
}

TEST_CASE(observer_and_network_can_be_destroyed_in_either_order)
{
    {
        auto rec = std::make_unique<Recorder>();
        Bench bench;
        bench.network.set_observer(rec.get());
        rec.reset(); // detaches itself
        CHECK(bench.network.observer() == nullptr);
        bench.add_node(kA);
        bench.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
        bench.steps(2);
    }
    {
        Recorder rec;
        {
            auto bench = std::make_unique<Bench>();
            bench->network.set_observer(&rec);
            bench->add_node(kA);
            bench->steps(1);
        } // the network goes first
        Bench other;
        other.network.set_observer(&rec);
        other.add_node(kA);
        other.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
        other.steps(2);
        CHECK_EQ(rec.of(EventKind::MessageReceived).size(), 1u);
    }
    {
        // An observer set on a second network leaves the first one.
        Recorder rec;
        Bench first;
        Bench second;
        first.network.set_observer(&rec);
        second.network.set_observer(&rec);
        CHECK(first.network.observer() == nullptr);
        CHECK(second.network.observer() == &rec);
    }
}

namespace {

/// Every transfer that started ended once: per direction and node, the starts and ends match
/// in order, with the same PGN, size and addresses.
void check_transfers_pair_up(const Recorder& rec)
{
    for (const TransferDirection direction : {TransferDirection::Send, TransferDirection::Receive})
    {
        std::vector<Event> open;
        for (const auto& e : rec.events)
        {
            if (e.direction != direction)
            {
                continue;
            }
            if (e.kind == EventKind::TransferStarted)
            {
                open.push_back(e);
            }
            else if (e.kind == EventKind::TransferEnded)
            {
                const auto match = std::find_if(open.begin(), open.end(), [&](const Event& s) {
                    return s.node == e.node && s.pgn == e.pgn && s.size == e.size && s.source == e.source &&
                           s.destination == e.destination && s.priority == e.priority;
                });
                CHECK(match != open.end());
                if (match != open.end())
                {
                    open.erase(match);
                }
            }
        }
        CHECK(open.empty());
    }
}

} // namespace

TEST_CASE(observer_transfers_that_start_end_once)
{
    {
        // A broadcast waiting after the claim, and a NACK held during the wait, when the
        // address is lost: the transfer is cancelled, the NACK dropped.
        Bench bench;
        Recorder rec;
        bench.network.set_observer(&rec);
        Node& us = add_claiming(bench, kUs, kOurName);
        bench.app = [&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(us.send(Pgn{kPgnPropB}, payload(20)) == SendStatus::Ok);
            }
        };
        bench.step();
        bench.inject(frame(6, kPgnRequest, 0x90, kUs, request(0xFEDA))); // nobody answers: NACK, held
        bench.step();
        bench.inject(claim(kUs, kLowerName));
        bench.steps(30);
        const auto ended = rec.of(EventKind::TransferEnded);
        CHECK_EQ(ended.size(), 1u);
        CHECK(ended[0].result == TransferResult::Cancelled);
        CHECK_EQ(ended[0].source, kUs);
        const auto dropped = rec.of(EventKind::MessageDropped);
        CHECK_EQ(dropped.size(), 1u);
        CHECK_EQ(dropped[0].pgn.value(), kPgnAck);
        CHECK(dropped[0].time == at(10ms));
        check_transfers_pair_up(rec);
    }
    {
        // A broadcast answer under way and a second one waiting in the response queue when the
        // node loses its address: both cancelled, from the address they were sent from.
        Bench bench;
        Recorder rec;
        bench.network.set_observer(&rec);
        Node& node = add_claiming(bench, 0x20, kOurName); // 0..127: no wait after the claim
        node.on_request(Pgn{0xFEDA}, [](vehicle_sil::j1939::RequestContext& r) { static_cast<void>(r.respond(payload(30))); });
        bench.step();
        bench.inject(frame(6, kPgnRequest, 0x90, kGlobal, request(0xFEDA)));
        bench.inject(frame(6, kPgnRequest, 0x91, kGlobal, request(0xFEDA)));
        bench.steps(2);
        CHECK_EQ(rec.of(EventKind::TransferStarted).size(), 2u);
        bench.inject(claim(0x20, kLowerName));
        bench.steps(3);
        const auto ended = rec.of(EventKind::TransferEnded);
        CHECK_EQ(ended.size(), 2u);
        for (const auto& e : ended)
        {
            CHECK(e.result == TransferResult::Cancelled);
            CHECK_EQ(e.source, 0x20);
        }
        check_transfers_pair_up(rec);
    }
    {
        // A connection replaced by a new RTS of the same group, then timed out.
        Bench bench;
        Recorder rec;
        bench.network.set_observer(&rec);
        bench.add_node(kA);
        bench.inject(rts(kRemote, kA, 20, kPgnPropA));
        bench.steps(3);
        bench.inject(rts(kRemote, kA, 30, kPgnPropA));
        bench.steps(200);
        const auto ended = rec.of(EventKind::TransferEnded);
        CHECK_EQ(ended.size(), 2u);
        CHECK(ended[0].result == TransferResult::Cancelled);
        CHECK_EQ(ended[0].size, 20u);
        CHECK(ended[1].result == TransferResult::Aborted);
        CHECK_EQ(ended[1].size, 30u);
        check_transfers_pair_up(rec);
    }
    {
        // The scenario of the trace test.
        Recorder rec;
        static_cast<void>(scenario_trace(&rec, 10ms));
        CHECK(rec.of(EventKind::TransferStarted).size() >= 4u);
        check_transfers_pair_up(rec);
    }
}

TEST_CASE(observer_reports_a_late_answer_to_an_old_address)
{
    Bench bench;
    Recorder rec;
    bench.network.set_observer(&rec);
    Node& node = add_claiming(bench, 0x20, kOurName);
    std::optional<vehicle_sil::j1939::PendingRequest> kept;
    node.on_request(Pgn{0xFEDA}, [&](vehicle_sil::j1939::RequestContext& r) { kept = r.defer(); });
    bench.step();
    bench.inject(frame(6, kPgnRequest, 0x90, 0x20, request(0xFEDA)));
    bench.step();
    CHECK(kept.has_value());
    bench.inject(claim(0x20, kLowerName));
    bench.step();
    bench.step();
    bool answered = false;
    bench.app = [&](SimTime) {
        if (!answered)
        {
            answered = true;
            CHECK(node.respond(*kept, Bytes{1, 2, 3}) == SendStatus::NoAddress);
        }
    };
    bench.step();
    const auto requests = rec.of(EventKind::SendRequested);
    CHECK_EQ(requests.size(), 1u);
    CHECK(requests[0].status == SendStatus::NoAddress);
    CHECK_EQ(requests[0].destination, kGlobal); // PDU2: a single frame goes to all
    CHECK(!requests[0].held);
}

TEST_CASE(observer_must_not_step_the_network_or_take_an_observer_in_use)
{
    {
        Bench bench;
        Recorder rec;
        bench.network.set_observer(&rec);
        bench.add_node(kA);
        rec.during = [&](const Event& e) {
            if (e.kind == EventKind::MessageReceived)
            {
                bench.network.on_port_event(can::PortEvent{});
            }
        };
        bench.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
        CHECK(throws<std::logic_error>([&] { bench.steps(2); }));
        CHECK(bench.network.failed());
        CHECK_EQ(bench.network.statistics().frames_transmitted, 0u);
    }
    {
        // Network B's handler takes the observer of network A while A is in its step.
        Recorder rec;
        Bench a;
        Bench b;
        a.network.set_observer(&rec);
        a.add_node(kA);
        bool threw = false;
        a.add_node(kB).on_message(Pgn{kPgnPropB}, [&](const Message&) {
            threw = throws<std::logic_error>([&] { b.network.set_observer(&rec); });
        });
        a.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
        a.steps(2);
        CHECK(threw);
        CHECK(a.network.observer() == &rec);
        CHECK(b.network.observer() == nullptr);
        b.network.set_observer(&rec); // between A's steps
        CHECK(b.network.observer() == &rec);
        CHECK(a.network.observer() == nullptr);
    }
}

TEST_CASE(observer_exception_from_the_application_stage_fails_the_network)
{
    Bench bench;
    Recorder rec;
    bench.network.set_observer(&rec);
    Node& a = bench.add_node(kA);
    rec.during = [](const Event& e) {
        if (e.kind == EventKind::SendRequested)
        {
            throw std::runtime_error("observer failed");
        }
    };
    bench.app = [&](SimTime) { static_cast<void>(a.send(Pgn{kPgnPropB}, Bytes{1})); };
    CHECK(throws<std::runtime_error>([&] { bench.step(); }));
    CHECK(bench.network.failed());
    CHECK(bench.network.error().find("observer") != std::string::npos);
}

TEST_CASE(observer_reports_a_queued_frame_dropped_when_the_address_is_lost)
{
    // A handler answers a message; a claim later in the same step takes the address before the
    // answer leaves.
    Bench bench;
    Recorder rec;
    bench.network.set_observer(&rec);
    Node& node = add_claiming(bench, 0x20, kOurName); // 0..127: no wait after the claim
    node.on_message(Pgn{kPgnPropB}, [&](const Message&) { static_cast<void>(node.send(Pgn{kPgnPropB}, Bytes{9})); });
    bench.step();
    bench.inject(bench.last_time(), frame(6, kPgnPropB, kRemote, kGlobal, {1}));
    bench.inject(bench.last_time() + 1ms, claim(0x20, kLowerName));
    bench.steps(2);
    const auto dropped = rec.of(EventKind::MessageDropped);
    CHECK_EQ(dropped.size(), 1u);
    CHECK(dropped[0].drop_reason == DropReason::AddressChanged);
    CHECK_EQ(pgn_of(dropped[0].frame), kPgnPropB);
    CHECK_EQ(dropped[0].source, 0x20);
    CHECK(dropped[0].time == at(10ms));
}

TEST_CASE(observer_reports_held_messages_the_full_data_queue_drops)
{
    vehicle_sil::j1939::NetworkOptions options;
    options.data_queue_capacity = 2;
    Bench bench(options);
    Recorder rec;
    bench.network.set_observer(&rec);
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            for (std::uint8_t i = 0; i < 5; ++i)
            {
                CHECK(us.send(Pgn{kPgnPropB}, Bytes{i}) == SendStatus::Ok); // held during the wait
            }
        }
    };
    bench.steps(30);
    const auto dropped = rec.of(EventKind::MessageDropped);
    CHECK_EQ(dropped.size(), 3u);
    for (const auto& e : dropped)
    {
        CHECK(e.drop_reason == DropReason::QueueFull);
        CHECK(e.time == at(250ms));
        CHECK_EQ(e.pgn.value(), kPgnPropB);
    }
    CHECK_EQ(bench.network.statistics().held_dropped, 3u);
}
