// SPDX-License-Identifier: MIT
//
// vehicle_sil::j1939::Network on the virtual bus: single-frame send and receive, destination
// filtering, local loopback, the order of frames with equal times, timers between frames,
// re-entrancy, failure, step size checks and resource limits. Built without SIL Kit.
#include "j1939_bench.hpp"
#include "network_test_access.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/j1939/network.hpp"

#include <array>
#include <type_traits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::to_ns;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Network;
using vehicle_sil::j1939::NetworkOptions;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::TimerKind;
using vehicle_sil::j1939::TimerTarget;
using vehicle_sil::unit::throws;
namespace can = vehicle_sil::can;

using vehicle_sil::j1939::detail::NetworkTestAccess;

namespace {

constexpr Address kUs = 0x80;
constexpr Address kOther = 0x81;
constexpr Address kTool = 0x90;

std::vector<std::uint8_t> bytes(std::initializer_list<std::uint8_t> b) { return b; }

/// A port the tests drive by hand: records what it accepts, answers send() from a script.
class ScriptedPort final : public can::Port
{
public:
    [[nodiscard]] can::TimeDomain time_domain() const noexcept override { return can::TimeDomain::Virtual; }
    std::size_t release_before(SimTime now, can::FrameSink sink) override
    {
        std::size_t n = 0;
        while (!inbox.empty() && inbox.front().time < now)
        {
            sink(inbox.front());
            inbox.erase(inbox.begin());
            ++n;
        }
        return n;
    }
    can::SendResult send(const can::TimedFrame& frame) override
    {
        can::SendResult result = can::SendResult::Accepted;
        if (!script.empty())
        {
            result = script.front();
            script.erase(script.begin());
        }
        attempts.push_back(from_can(frame.frame));
        if (result == can::SendResult::Accepted)
        {
            accepted.push_back(from_can(frame.frame));
        }
        return result;
    }

    std::vector<can::TimedFrame> inbox;
    std::vector<can::SendResult> script; ///< Results of the next send() calls; Accepted after.
    std::vector<J1939Frame> attempts;
    std::vector<J1939Frame> accepted;
};

/// Runs one step of `network` by hand (the host's five stages).
void step_by_hand(Network& network, can::Port& port, SimTime now, const std::function<void()>& app = {})
{
    network.begin(now);
    port.release_before(now, [&](const can::TimedFrame& f) { network.receive(f); });
    network.process();
    if (app)
    {
        app();
    }
    network.flush(port);
}

} // namespace

// --- single frames

TEST_CASE(single_frame_is_received_with_its_fields)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    std::vector<Seen> seen;
    us.on_any_message([&](const Message& m) { seen.push_back(seen_of(m)); });

    bench.inject(at(3ms), frame(3, kPgnPropB, kTool, kGlobal, bytes({1, 2, 3, 4, 5, 6, 7, 8})));
    bench.inject(at(4ms), frame(7, kPgnPropA, kTool, kUs, {}));
    bench.step(); // t = 0: nothing before 0
    CHECK(seen.empty());
    bench.step(); // t = 10
    CHECK_EQ(seen.size(), 2u);
    CHECK(seen[0] == (Seen{kPgnPropB, kTool, kGlobal, 3, at(3ms), bytes({1, 2, 3, 4, 5, 6, 7, 8})}));
    CHECK(seen[1] == (Seen{kPgnPropA, kTool, kUs, 7, at(4ms), {}}));
    CHECK_EQ(bench.network.statistics().frames_received, 2u);
}

TEST_CASE(single_frame_is_sent_at_the_step_time)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    int calls = 0;
    bench.app = [&](SimTime now) {
        if (now == at(20ms))
        {
            ++calls;
            const std::array<std::uint8_t, 8> eight{1, 2, 3, 4, 5, 6, 7, 8};
            CHECK(us.send(Pgn{kPgnPropA}, eight, 0x25, 3) == SendStatus::Ok);
            CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok);
            const std::array<std::uint8_t, 1> one{0xAA};
            CHECK(us.send(Pgn{kPgnPropA}, one, kGlobal, 0) == SendStatus::Ok);
        }
    };
    bench.steps(3);
    CHECK_EQ(calls, 1);
    CHECK_J1939(bench.take_sent(), {frame(3, kPgnPropA, kUs, 0x25, bytes({1, 2, 3, 4, 5, 6, 7, 8})),
                                    frame(6, kPgnPropB, kUs, kGlobal, {}), frame(0, kPgnPropA, kUs, kGlobal, bytes({0xAA}))});
    CHECK(bench.sent_times == (std::vector<SimTime>{at(20ms), at(20ms), at(20ms)}));
}

TEST_CASE(send_rejects_invalid_arguments_without_queueing)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    bench.app = [&](SimTime) {
        const std::array<std::uint8_t, 1786> tooLong{};
        const std::array<std::uint8_t, 1> one{};
        CHECK(us.send(Pgn{kPgnPropB}, tooLong) == SendStatus::TooLong); // above the transport protocol's 1785
        CHECK(us.send(Pgn{kPgnPropA}, tooLong, 0x25) == SendStatus::TooLong);
        CHECK(us.send(Pgn{kPgnPropB}, one, 0x25) == SendStatus::InvalidArgument); // PDU2 to one node
        CHECK(us.send(Pgn{0xEF01}, one, 0x25) == SendStatus::InvalidArgument);    // PS set in a PDU1 PGN
        CHECK(us.send(Pgn{0x40000}, one) == SendStatus::InvalidArgument);         // above 18 bits
        CHECK(us.send(Pgn{kPgnPropB}, one, kGlobal, 8) == SendStatus::InvalidArgument);
        CHECK(us.send(Pgn{kPgnPropA}, one, kNull) == SendStatus::InvalidArgument);
        CHECK(us.request(Pgn{0xEF01}, 0x25) == SendStatus::InvalidArgument);
    };
    bench.step();
    CHECK_J1939(bench.take_sent(), {});
}

TEST_CASE(send_outside_a_step_throws)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    const std::array<std::uint8_t, 1> one{};
    CHECK(throws<std::logic_error>([&] { us.send(Pgn{kPgnPropB}, one); }));
    bench.step();
    CHECK(throws<std::logic_error>([&] { us.send(Pgn{kPgnPropB}, one); }));
    CHECK(throws<std::logic_error>([&] { us.request(Pgn{kPgnPropB}); }));
    CHECK(!bench.network.failed()); // a usage error outside the step does not fail the network
}

// --- destination filtering (J1939-82 A4-06 equivalent: a node takes the destination-specific
// messages sent to its own address and the global ones, not those sent to other addresses)

TEST_CASE(destination_specific_frames_reach_only_the_addressed_node)
{
    Bench bench;
    Node& a = bench.add_node(kUs);
    Node& b = bench.add_node(kOther);
    std::vector<Seen> seenA;
    std::vector<Seen> seenB;
    a.on_any_message([&](const Message& m) { seenA.push_back(seen_of(m)); });
    b.on_any_message([&](const Message& m) { seenB.push_back(seen_of(m)); });

    bench.inject(at(1ms), frame(6, kPgnPropA, kTool, kUs, bytes({1})));     // to a
    bench.inject(at(2ms), frame(6, kPgnPropA, kTool, kOther, bytes({2})));  // to b
    bench.inject(at(3ms), frame(6, kPgnPropA, kTool, 0x55, bytes({3})));    // to nobody here
    bench.inject(at(4ms), frame(6, kPgnPropA, kTool, kGlobal, bytes({4}))); // PDU1 to all
    bench.inject(at(5ms), frame(6, kPgnPropB, kTool, kGlobal, bytes({5}))); // PDU2
    bench.inject(at(6ms), frame(6, kPgnPropA, kTool, kNull, bytes({6})));   // to the null address
    bench.steps(2);

    std::vector<std::uint8_t> payloadsA;
    for (const auto& s : seenA)
    {
        payloadsA.push_back(s.data[0]);
    }
    std::vector<std::uint8_t> payloadsB;
    for (const auto& s : seenB)
    {
        payloadsB.push_back(s.data[0]);
    }
    CHECK(payloadsA == bytes({1, 4, 5}));
    CHECK(payloadsB == bytes({2, 4, 5}));
}

TEST_CASE(handlers_by_pgn_see_only_their_group)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    std::vector<std::uint32_t> a;
    std::vector<std::uint32_t> b;
    us.on_message(Pgn{kPgnPropA}, [&](const Message& m) { a.push_back(m.pgn.value()); });
    us.on_message(Pgn{kPgnPropB}, [&](const Message& m) { b.push_back(m.pgn.value()); });
    bench.inject(at(1ms), frame(6, kPgnPropA, kTool, kUs, {}));
    bench.inject(at(2ms), frame(6, kPgnPropB, kTool, kGlobal, {}));
    bench.inject(at(3ms), frame(6, 0xFF11, kTool, kGlobal, {}));
    bench.steps(2);
    CHECK(a == (std::vector<std::uint32_t>{kPgnPropA}));
    CHECK(b == (std::vector<std::uint32_t>{kPgnPropB}));
}

TEST_CASE(non_j1939_frames_are_ignored_and_counted)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    int seen = 0;
    us.on_any_message([&](const Message&) { ++seen; });
    can::Frame standard;
    standard.id = 0x123;
    standard.format = can::IdFormat::Standard;
    standard.length = 1;
    bench.tester.send(can::TimedFrame{at(1ms), standard});
    bench.inject(at(2ms), frame(6, kPgnPropB, kGlobal, kGlobal, {})); // source address 255
    bench.inject(at(3ms), frame(6, kPgnPropB, kTool, kGlobal, {}));
    bench.steps(2);
    CHECK_EQ(seen, 1);
    CHECK_EQ(bench.network.statistics().frames_ignored, 2u);
    CHECK_EQ(bench.network.statistics().frames_received, 1u);
}

// --- local loopback

TEST_CASE(local_nodes_receive_each_others_frames_in_the_next_step)
{
    Bench bench;
    Node& a = bench.add_node(kUs);
    Node& b = bench.add_node(kOther);
    std::vector<Seen> seenA;
    std::vector<Seen> seenB;
    a.on_any_message([&](const Message& m) { seenA.push_back(seen_of(m)); });
    b.on_any_message([&](const Message& m) { seenB.push_back(seen_of(m)); });
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            const std::array<std::uint8_t, 1> one{0x11};
            CHECK(a.send(Pgn{kPgnPropB}, one) == SendStatus::Ok);
            CHECK(a.send(Pgn{kPgnPropA}, one, kOther) == SendStatus::Ok);
        }
    };
    bench.step(); // 0: a sends
    CHECK(seenB.empty());
    bench.step(); // 10: b receives with the send time; a never sees its own frames
    CHECK(seenA.empty());
    CHECK_EQ(seenB.size(), 2u);
    CHECK(seenB[0] == (Seen{kPgnPropB, kUs, kGlobal, 6, at(0ms), bytes({0x11})}));
    CHECK(seenB[1] == (Seen{kPgnPropA, kUs, kOther, 6, at(0ms), bytes({0x11})}));
    // The bus got them too.
    CHECK_EQ(bench.take_sent().size(), 2u);
}

TEST_CASE(local_nodes_with_the_same_address_see_each_other)
{
    Bench bench;
    Node& a = bench.add_node(kUs);
    Node& b = bench.add_node(kUs);
    int seenA = 0;
    int seenB = 0;
    a.on_any_message([&](const Message&) { ++seenA; });
    b.on_any_message([&](const Message& m) {
        ++seenB;
        CHECK_EQ(m.source, kUs);
    });
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(a.send(Pgn{kPgnPropA}, {}, kUs) == SendStatus::Ok);
        }
    };
    bench.steps(2);
    CHECK_EQ(seenA, 0);
    CHECK_EQ(seenB, 1);
}

TEST_CASE(loopback_is_not_recorded_with_a_single_node)
{
    Bench bench;
    Node& a = bench.add_node(kUs);
    int seen = 0;
    a.on_any_message([&](const Message&) { ++seen; });
    bench.app = [&](SimTime) { CHECK(a.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok); };
    bench.steps(3);
    CHECK_EQ(seen, 0);
    CHECK_EQ(bench.take_sent().size(), 3u);
}

// --- order of frames with equal times (sender lists merged by CAN arbitration)

namespace {

std::vector<std::string> order_seen(bool bFirst, SimDuration step)
{
    Bench bench({}, step);
    Node& us = bench.add_node(kUs);
    std::vector<std::string> order;
    us.on_any_message([&](const Message& m) {
        order.push_back(std::to_string(m.source) + "/" + std::to_string(m.priority) + "/" + std::to_string(to_ns(m.time)));
    });
    // Sender A (0x20) sends priority 6 then priority 3; sender B (0x30) sends priority 3; all at 5 ms.
    const auto a1 = frame(6, kPgnPropB, 0x20, kGlobal, {});
    const auto a2 = frame(3, kPgnPropB, 0x20, kGlobal, {});
    const auto b1 = frame(3, kPgnPropB, 0x30, kGlobal, {});
    // A later frame with the highest priority, injected first.
    bench.inject(at(7ms), frame(0, kPgnPropB, 0x01, kGlobal, {}));
    if (bFirst)
    {
        bench.inject(at(5ms), b1);
        bench.inject(at(5ms), a1);
        bench.inject(at(5ms), a2);
    }
    else
    {
        bench.inject(at(5ms), a1);
        bench.inject(at(5ms), b1);
        bench.inject(at(5ms), a2);
    }
    bench.inject(at(5ms), frame(7, kPgnPropB, 0x10, kGlobal, {}));
    bench.steps(static_cast<int>(40ms / step) + 1);
    return order;
}

} // namespace

TEST_CASE(equal_times_keep_each_senders_order_and_merge_by_arbitration)
{
    // Heads: A 6, B 3, C(0x10) 7 -> B; then A 6, C 7 -> A(6); then A 3, C 7 -> A(3); then C.
    const std::vector<std::string> expected{"48/3/5000000", "32/6/5000000", "32/3/5000000", "16/7/5000000", "1/0/7000000"};
    CHECK(order_seen(false, 10ms) == expected);
    CHECK(order_seen(true, 10ms) == expected);
    // Independent of the step size.
    CHECK(order_seen(false, 1ms) == expected);
    CHECK(order_seen(true, 3ms) == expected);
    CHECK(order_seen(false, 20ms) == expected);
}

TEST_CASE(equal_times_external_frames_come_before_loopback_of_the_same_address)
{
    Bench bench;
    Node& a = bench.add_node(kUs);
    Node& b = bench.add_node(kOther);
    std::vector<std::string> order;
    b.on_any_message([&](const Message& m) { order.push_back(std::to_string(m.source) + "/" + std::to_string(m.data[0])); });
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            const std::array<std::uint8_t, 1> one{1};
            CHECK(a.send(Pgn{kPgnPropB}, one, kGlobal, 7) == SendStatus::Ok);
        }
    };
    // The tester also sends from our address at time 0, and a high priority frame from another
    // address after a low priority one.
    bench.inject(at(0ms), frame(0, kPgnPropB, kUs, kGlobal, bytes({2})));
    bench.inject(at(0ms), frame(7, kPgnPropB, 0x05, kGlobal, bytes({3})));
    bench.inject(at(0ms), frame(1, kPgnPropB, 0x05, kGlobal, bytes({4})));
    bench.steps(2);
    // Lists: 0x80 = [external P0 (2), loopback P7 (1)], 0x05 = [P7 (3), P1 (4)].
    // Heads P0/0x80 vs P7/0x05 -> 2; P7/0x80 vs P7/0x05 -> lower id (0x05) -> 3; P7/0x80 vs P1 -> 4; then 1.
    CHECK(order == (std::vector<std::string>{"128/2", "5/3", "5/4", "128/1"}));
}

// --- timers between frames

namespace {

struct TimerLog final : TimerTarget
{
    std::vector<std::string>* log = nullptr;
    void on_timer(std::uint32_t tag) override { log->push_back("timer" + std::to_string(tag)); }
};

std::vector<std::string> timers_and_frames(SimDuration step)
{
    Bench bench({}, step);
    Node& us = bench.add_node(kUs);
    std::vector<std::string> log;
    TimerLog target;
    target.log = &log;
    us.on_any_message([&](const Message& m) { log.push_back("frame" + std::to_string(to_ns(m.time) / 1000000)); });
    // Started before the first step (the timers belong to the network, not to a step).
    auto& timers = NetworkTestAccess::timers(bench.network);
    timers.start(TimerKind::Timeout, at(10ms), target, 1);   // between the frames at 5 and 15
    timers.start(TimerKind::Timeout, at(15ms), target, 2);   // equal to a frame time: the frame first
    // A scheduled timer fires in the first step at or after its deadline, so only its order
    // relative to frames with other times is independent of the step size.
    timers.start(TimerKind::Scheduled, at(12ms), target, 3);
    timers.start(TimerKind::Timeout, at(29ms), target, 4);   // after the last frame
    bench.inject(at(5ms), frame(6, kPgnPropB, kTool, kGlobal, {}));
    bench.inject(at(15ms), frame(6, kPgnPropB, kTool, kGlobal, {}));
    bench.inject(at(20ms), frame(6, kPgnPropB, kTool, kGlobal, {}));
    bench.steps(static_cast<int>(60ms / step) + 1);
    return log;
}

} // namespace

TEST_CASE(timeouts_fire_between_frames_independent_of_the_step_size)
{
    const std::vector<std::string> expected{"frame5", "timer1", "timer3", "frame15", "timer2", "frame20", "timer4"};
    for (const SimDuration step : {SimDuration{1ms}, SimDuration{3ms}, SimDuration{10ms}, SimDuration{30ms}, SimDuration{50ms}})
    {
        const auto log = timers_and_frames(step);
        if (log != expected)
        {
            std::string text;
            for (const auto& l : log)
            {
                text += l + " ";
            }
            throw vehicle_sil::unit::Failure("step " + std::to_string(step.count()) + " ns: " + text);
        }
    }
}

TEST_CASE(timeout_at_the_step_time_waits_for_the_next_step_but_scheduled_does_not)
{
    Bench bench;
    bench.add_node(kUs);
    std::vector<std::string> log;
    TimerLog target;
    target.log = &log;
    NetworkTestAccess::timers(bench.network).start(TimerKind::Timeout, at(20ms), target, 1);
    NetworkTestAccess::timers(bench.network).start(TimerKind::Scheduled, at(20ms), target, 2);
    CHECK(bench.network.next_deadline() == at(20ms));
    bench.steps(3); // 0, 10, 20
    CHECK(log == (std::vector<std::string>{"timer2"}));
    CHECK(bench.network.next_deadline() == at(20ms));
    bench.step(); // 30
    CHECK(log == (std::vector<std::string>{"timer2", "timer1"}));
    CHECK(!bench.network.next_deadline().has_value());
}

namespace {

struct SendingTimer final : TimerTarget
{
    Node* node = nullptr;
    void on_timer(std::uint32_t tag) override
    {
        const std::array<std::uint8_t, 1> one{static_cast<std::uint8_t>(tag)};
        CHECK(node->send(Pgn{kPgnPropB}, one) == SendStatus::Ok);
    }
};

} // namespace

TEST_CASE(a_timer_may_send_and_the_frame_leaves_in_the_same_step)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    SendingTimer target;
    target.node = &us;
    NetworkTestAccess::timers(bench.network).start(TimerKind::Scheduled, at(10ms), target, 7);
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {frame(6, kPgnPropB, kUs, kGlobal, bytes({7}))});
    CHECK(bench.sent_times == (std::vector<SimTime>{at(10ms)}));
}

// --- handler registration while delivering

// Added: from the next frame. Removed: not called any more, also for the rest of this frame.
TEST_CASE(handlers_added_while_delivering_start_after_the_frame_and_removed_ones_stop_at_once)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    std::vector<std::string> log;
    vehicle_sil::j1939::HandlerId later{};
    bool added = false;
    us.on_any_message([&](const Message& m) {
        log.push_back("first" + std::to_string(m.data[0]));
        if (!added)
        {
            added = true;
            us.on_any_message([&](const Message& m2) { log.push_back("added" + std::to_string(m2.data[0])); });
            CHECK(us.remove_handler(later));
            CHECK(!us.remove_handler(later)); // already removed (pending)
        }
    });
    later = us.on_any_message([&](const Message& m) { log.push_back("later" + std::to_string(m.data[0])); });
    bench.inject(at(1ms), frame(6, kPgnPropB, kTool, kGlobal, bytes({1})));
    bench.inject(at(2ms), frame(6, kPgnPropB, kTool, kGlobal, bytes({2})));
    bench.steps(2);
    CHECK(log == (std::vector<std::string>{"first1", "first2", "added2"}));
    CHECK(!us.remove_handler(vehicle_sil::j1939::HandlerId{999}));
}

// --- re-entrancy and failure

TEST_CASE(step_functions_from_a_handler_throw_and_fail_the_network)
{
    for (int which = 0; which < 4; ++which)
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        bool threw = false;
        us.on_any_message([&](const Message&) {
            try
            {
                switch (which)
                {
                case 0: bench.network.begin(at(100ms)); break;
                case 1: bench.network.receive(can::TimedFrame{at(1ms), to_can(frame(6, kPgnPropB, 1, kGlobal, {}))}); break;
                case 2: bench.network.process(); break;
                default: bench.network.flush(bench.port); break;
                }
            }
            catch (const std::logic_error&)
            {
                threw = true; // swallowed: the network must still be failed
            }
        });
        bench.inject(at(1ms), frame(6, kPgnPropB, kTool, kGlobal, {}));
        bench.step();
        CHECK(throws<std::logic_error>([&] { bench.step(); }));
        CHECK(threw);
        CHECK(bench.network.failed());
        CHECK(bench.host.failed());
        CHECK(throws<std::logic_error>([&] { bench.step(); }));
    }
}

TEST_CASE(add_node_from_the_application_works_from_the_next_step)
{
    Bench bench;
    bench.add_node(kUs);
    Node* added = nullptr;
    std::vector<Seen> seen;
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            added = &bench.network.add_node({0x42});
            added->on_any_message([&](const Message& m) { seen.push_back(seen_of(m)); });
        }
    };
    bench.inject(at(5ms), frame(6, kPgnPropA, kTool, 0x42, bytes({1}))); // handled before the node exists
    bench.inject(at(15ms), frame(6, kPgnPropA, kTool, 0x42, bytes({2})));
    bench.steps(3);
    CHECK(!bench.network.failed());
    CHECK(added != nullptr);
    CHECK_EQ(seen.size(), 1u);
    CHECK(seen[0].data == bytes({2}));
    CHECK_EQ(bench.network.node_count(), 2u);
}

namespace {

/// Calls `what` from a handler, a request responder or a timer of a fresh bench, swallowing the
/// exception it throws; returns whether it threw std::logic_error and the network failed.
bool misuse_fails(int from, const std::function<void(Bench&)>& what)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    bool threw = false;
    const auto call = [&] {
        try
        {
            what(bench);
        }
        catch (const std::logic_error&)
        {
            threw = true;
        }
    };
    struct CallingTimer final : TimerTarget
    {
        std::function<void()> f;
        void on_timer(std::uint32_t) override { f(); }
    } timer;
    timer.f = call;
    switch (from)
    {
    case 0: us.on_any_message([&](const Message&) { call(); }); break;
    case 1: us.on_any_request([&](vehicle_sil::j1939::RequestContext&) { call(); }); break;
    default: NetworkTestAccess::timers(bench.network).start(TimerKind::Scheduled, at(10ms), timer, 1); break;
    }
    bench.inject(at(1ms), frame(6, kPgnRequest, kTool, kUs, request(kPgnPropB)));
    bench.step();
    const bool stepThrew = throws<std::logic_error>([&] { bench.step(); });
    return threw && stepThrew && bench.network.failed() && bench.host.failed();
}

} // namespace

TEST_CASE(misuse_from_handlers_responders_and_timers_fails_the_network)
{
    const std::vector<std::function<void(Bench&)>> misuses{
        [](Bench& b) { b.network.begin(at(100ms)); },
        [](Bench& b) { b.network.receive(can::TimedFrame{at(1ms), to_can(frame(6, kPgnPropB, 1, kGlobal, {}))}); },
        [](Bench& b) { b.network.process(); },
        [](Bench& b) { b.network.flush(b.port); },
        [](Bench& b) { b.network.start(10ms); },
        [](Bench& b) { b.network.add_node({0x42}); },
    };
    for (int from = 0; from < 3; ++from)
    {
        for (std::size_t m = 0; m < misuses.size(); ++m)
        {
            if (!misuse_fails(from, misuses[m]))
            {
                throw vehicle_sil::unit::Failure("misuse " + std::to_string(m) + " from " + std::to_string(from) +
                                                 " did not fail the network");
            }
        }
    }
}

TEST_CASE(exceptions_from_responders_timers_and_the_port_fail_the_network)
{
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        us.on_any_request([](vehicle_sil::j1939::RequestContext&) { throw std::runtime_error("responder broke"); });
        bench.inject(at(1ms), frame(6, kPgnRequest, kTool, kUs, request(kPgnPropB)));
        bench.step();
        CHECK(throws<std::runtime_error>([&] { bench.step(); }));
        CHECK(bench.network.error() == "responder broke");
    }
    {
        struct Throwing final : TimerTarget
        {
            void on_timer(std::uint32_t) override { throw std::runtime_error("timer broke"); }
        } timer;
        Bench bench;
        bench.add_node(kUs);
        NetworkTestAccess::timers(bench.network).start(TimerKind::Timeout, at(5ms), timer, 1);
        bench.step();
        CHECK(throws<std::runtime_error>([&] { bench.step(); }));
        CHECK(bench.network.error() == "timer broke");
        CHECK(bench.host.failed());
    }
    {
        struct ThrowingPort final : can::Port
        {
            [[nodiscard]] can::TimeDomain time_domain() const noexcept override { return can::TimeDomain::Virtual; }
            std::size_t release_before(SimTime, can::FrameSink) override { return 0; }
            can::SendResult send(const can::TimedFrame&) override { throw std::runtime_error("port broke"); }
        } port;
        Network network;
        Node& us = network.add_node({kUs});
        CHECK(throws<std::runtime_error>([&] {
            step_by_hand(network, port, at(0ms), [&] { CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok); });
        }));
        CHECK(network.failed());
        CHECK(network.error() == "port broke");
    }
}

TEST_CASE(request_context_cannot_be_copied_out_of_the_responder)
{
    static_assert(!std::is_copy_constructible_v<vehicle_sil::j1939::RequestContext>);
    static_assert(!std::is_move_constructible_v<vehicle_sil::j1939::RequestContext>);
    static_assert(!std::is_copy_assignable_v<vehicle_sil::j1939::RequestContext>);
}

TEST_CASE(frames_from_the_null_address_other_than_claims_and_requests_are_ignored)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    std::vector<std::uint32_t> pgns;
    us.on_any_message([&](const Message& m) { pgns.push_back(m.pgn.value()); });
    bench.inject(at(1ms), frame(6, kPgnPropB, kNull, kGlobal, {}));
    bench.inject(at(2ms), frame(6, kPgnPropA, kNull, kUs, {}));
    bench.inject(at(3ms), frame(6, kPgnAck, kNull, kGlobal, acknowledgement(kNack, kUs, kPgnPropB)));
    bench.inject(at(4ms), frame(6, kPgnAddressClaim, kNull, kGlobal, {1, 2, 3, 4, 5, 6, 7, 8}));
    bench.inject(at(5ms), frame(6, kPgnRequest, kNull, kGlobal, request(kPgnAddressClaim)));
    bench.steps(2);
    CHECK(pgns == (std::vector<std::uint32_t>{kPgnAddressClaim, kPgnRequest}));
    CHECK_EQ(bench.network.statistics().frames_ignored, 3u);
}

TEST_CASE(several_groups_of_equal_times_are_each_merged)
{
    Bench bench({}, 20ms);
    Node& us = bench.add_node(kUs);
    std::vector<std::string> order;
    us.on_any_message([&](const Message& m) {
        order.push_back(std::to_string(to_ns(m.time) / 1000000) + ":" + std::to_string(m.source));
    });
    // Two groups in one step, each injected with the higher identifier first.
    bench.inject(at(3ms), frame(6, kPgnPropB, 0x40, kGlobal, {}));
    bench.inject(at(3ms), frame(6, kPgnPropB, 0x30, kGlobal, {}));
    bench.inject(at(3ms), frame(6, kPgnPropB, 0x20, kGlobal, {}));
    bench.inject(at(4ms), frame(6, kPgnPropB, 0x40, kGlobal, {}));
    bench.inject(at(4ms), frame(5, kPgnPropB, 0x41, kGlobal, {}));
    bench.inject(at(9ms), frame(6, kPgnPropB, 0x05, kGlobal, {}));
    bench.inject(at(9ms), frame(6, kPgnPropB, 0x04, kGlobal, {}));
    bench.steps(2);
    CHECK(order == (std::vector<std::string>{"3:32", "3:48", "3:64", "4:65", "4:64", "9:4", "9:5"}));
}

TEST_CASE(exception_from_a_handler_fails_the_network_and_the_host)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    us.on_any_message([](const Message&) { throw std::runtime_error("handler broke"); });
    bench.inject(at(1ms), frame(6, kPgnPropB, kTool, kGlobal, {}));
    bench.step();
    CHECK(throws<std::runtime_error>([&] { bench.step(); }));
    CHECK(bench.network.failed());
    CHECK(bench.network.error() == "handler broke");
    CHECK(throws<std::logic_error>([&] { bench.network.begin(at(1s)); }));
    CHECK(throws<std::logic_error>([&] { bench.network.add_node({1}); }));
}

TEST_CASE(step_misuse_throws_and_fails_the_network)
{
    {
        Network network;
        ScriptedPort port;
        step_by_hand(network, port, at(10ms));
        CHECK(throws<std::logic_error>([&] { network.begin(at(10ms)); })); // not increasing
        CHECK(network.failed());
    }
    {
        Network network;
        network.begin(at(10ms));
        // A frame at or after the step time is not "before now".
        CHECK(throws<std::logic_error>(
            [&] { network.receive(can::TimedFrame{at(10ms), to_can(frame(6, kPgnPropB, 1, kGlobal, {}))}); }));
        CHECK(network.failed());
    }
    {
        Network network;
        ScriptedPort port;
        network.begin(at(0ms));
        CHECK(throws<std::logic_error>([&] { network.flush(port); })); // flush before process
        CHECK(network.failed());
    }
    {
        Network network;
        network.begin(at(0ms));
        network.process();
        CHECK(throws<std::logic_error>([&] { network.process(); })); // twice
    }
    {
        // The first step may be at time 0.
        Network network;
        ScriptedPort port;
        step_by_hand(network, port, at(0ms));
        step_by_hand(network, port, at(1ns));
        CHECK(!network.failed());
    }
}

// --- step size (host -> network)

namespace {

struct StepSizeRun
{
    std::vector<std::string> warnings;
    bool rejected = false;
};

StepSizeRun run_with_step(SimDuration step)
{
    StepSizeRun run;
    NetworkOptions options;
    options.warning = [&run](std::string_view w) { run.warnings.emplace_back(w); };
    can::VirtualCanBus bus;
    Network network(options);
    network.add_node({kUs});
    can::Host host;
    host.add(network, bus.attach());
    can::StepDriver driver;
    driver.add(host, step);
    try
    {
        driver.run_until(at(1s));
    }
    catch (const std::invalid_argument&)
    {
        run.rejected = true;
        CHECK(host.failed());
        CHECK(network.failed());
    }
    return run;
}

} // namespace

TEST_CASE(step_size_above_50_ms_warns_and_above_200_ms_is_rejected)
{
    CHECK(run_with_step(10ms).warnings.empty());
    CHECK(run_with_step(50ms).warnings.empty());
    {
        const auto run = run_with_step(50ms + 1ns);
        CHECK_EQ(run.warnings.size(), 1u); // once, at the start
        CHECK(!run.rejected);
    }
    {
        const auto run = run_with_step(200ms);
        CHECK_EQ(run.warnings.size(), 1u);
        CHECK(!run.rejected);
    }
    {
        const auto run = run_with_step(200ms + 1ns);
        CHECK(run.rejected);
    }
}

TEST_CASE(network_without_a_step_size_runs_without_checks)
{
    NetworkOptions options;
    std::vector<std::string> warnings;
    options.warning = [&](std::string_view w) { warnings.emplace_back(w); };
    can::VirtualCanBus bus;
    Network network(options);
    can::Host host;
    host.add(network, bus.attach());
    host.step(at(0ms));
    host.step(at(500ms));
    CHECK(warnings.empty());
    CHECK(!network.failed());
}

// --- resource limits and the send queues

TEST_CASE(data_queue_full_returns_queue_full_and_frees_after_the_flush)
{
    NetworkOptions options;
    options.data_queue_capacity = 4;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    std::vector<SendStatus> results;
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            for (std::uint8_t i = 0; i < 5; ++i)
            {
                const std::array<std::uint8_t, 1> one{i};
                results.push_back(us.send(Pgn{kPgnPropB}, one));
            }
        }
        if (now == at(10ms))
        {
            const std::array<std::uint8_t, 1> one{9};
            results.push_back(us.send(Pgn{kPgnPropB}, one));
        }
    };
    bench.steps(2);
    CHECK(results == (std::vector<SendStatus>{SendStatus::Ok, SendStatus::Ok, SendStatus::Ok, SendStatus::Ok,
                                              SendStatus::QueueFull, SendStatus::Ok}));
    CHECK_EQ(bench.take_sent().size(), 5u);
}

TEST_CASE(frames_per_step_limit_carries_the_rest_over_in_order)
{
    NetworkOptions options;
    options.max_frames_per_step = 2;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            for (std::uint8_t i = 0; i < 3; ++i)
            {
                const std::array<std::uint8_t, 1> one{i};
                CHECK(us.send(Pgn{kPgnPropB}, one) == SendStatus::Ok);
            }
        }
    };
    // Three requests to us: their NACKs are control frames, sent first and not counted against
    // the limit.
    for (std::uint32_t pgn : {0xFEF1u, 0xFEF2u, 0xFEF3u})
    {
        bench.inject(at(5ms), frame(6, kPgnRequest, kTool, kUs, request(pgn)));
    }
    bench.steps(2); // 0, 10
    CHECK_J1939(bench.take_sent(), {frame(6, kPgnAck, kUs, kGlobal, acknowledgement(kNack, kTool, 0xFEF1)),
                                    frame(6, kPgnAck, kUs, kGlobal, acknowledgement(kNack, kTool, 0xFEF2)),
                                    frame(6, kPgnAck, kUs, kGlobal, acknowledgement(kNack, kTool, 0xFEF3)),
                                    frame(6, kPgnPropB, kUs, kGlobal, bytes({0})), frame(6, kPgnPropB, kUs, kGlobal, bytes({1}))});
    bench.step(); // 20: the rest
    CHECK_J1939(bench.take_sent(), {frame(6, kPgnPropB, kUs, kGlobal, bytes({2}))});
}

TEST_CASE(port_queue_full_on_a_control_frame_holds_the_data_queue_too)
{
    Network network;
    Node& us = network.add_node({kUs});
    ScriptedPort port;
    for (std::uint32_t pgn : {0xFEF1u, 0xFEF2u})
    {
        port.inbox.push_back(can::TimedFrame{at(1ms), to_can(frame(6, kPgnRequest, kTool, kUs, request(pgn)))});
    }
    port.script = {can::SendResult::Accepted, can::SendResult::QueueFull};
    step_by_hand(network, port, at(10ms), [&] { CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok); });
    // The second NACK did not fit; the data frame was not tried.
    CHECK_J1939(port.attempts, {frame(6, kPgnAck, kUs, kGlobal, acknowledgement(kNack, kTool, 0xFEF1)),
                                frame(6, kPgnAck, kUs, kGlobal, acknowledgement(kNack, kTool, 0xFEF2))});
    step_by_hand(network, port, at(20ms));
    CHECK_J1939(port.accepted, {frame(6, kPgnAck, kUs, kGlobal, acknowledgement(kNack, kTool, 0xFEF1)),
                                frame(6, kPgnAck, kUs, kGlobal, acknowledgement(kNack, kTool, 0xFEF2)),
                                frame(6, kPgnPropB, kUs, kGlobal, {})});
}

TEST_CASE(port_queue_full_keeps_the_frame_and_the_rest_for_the_next_step)
{
    Network network;
    Node& us = network.add_node({kUs});
    ScriptedPort port;
    port.script = {can::SendResult::Accepted, can::SendResult::QueueFull};
    step_by_hand(network, port, at(0ms), [&] {
        for (std::uint8_t i = 0; i < 3; ++i)
        {
            const std::array<std::uint8_t, 1> one{i};
            CHECK(us.send(Pgn{kPgnPropB}, one) == SendStatus::Ok);
        }
    });
    CHECK_EQ(port.accepted.size(), 1u);
    CHECK_EQ(network.statistics().send_queue_full, 1u);
    step_by_hand(network, port, at(10ms));
    CHECK_J1939(port.accepted, {frame(6, kPgnPropB, kUs, kGlobal, bytes({0})), frame(6, kPgnPropB, kUs, kGlobal, bytes({1})),
                                frame(6, kPgnPropB, kUs, kGlobal, bytes({2}))});
    CHECK_EQ(network.statistics().frames_sent, 3u);
}

TEST_CASE(port_failure_drops_only_that_frame)
{
    Network network;
    Node& a = network.add_node({kUs});
    Node& b = network.add_node({kOther});
    std::vector<std::uint8_t> seenB;
    b.on_any_message([&](const Message& m) { seenB.push_back(m.data[0]); });
    ScriptedPort port;
    port.script = {can::SendResult::Accepted, can::SendResult::Failed, can::SendResult::Accepted};
    step_by_hand(network, port, at(0ms), [&] {
        for (std::uint8_t i = 0; i < 3; ++i)
        {
            const std::array<std::uint8_t, 1> one{i};
            CHECK(a.send(Pgn{kPgnPropB}, one) == SendStatus::Ok);
        }
    });
    CHECK_EQ(port.accepted.size(), 2u);
    CHECK_EQ(network.statistics().send_failures, 1u);
    // Only frames the port accepted are looped back.
    step_by_hand(network, port, at(10ms));
    CHECK(seenB == bytes({0, 2}));
}

TEST_CASE(control_queue_overflow_drops_and_counts)
{
    NetworkOptions options;
    options.receive_capacity = 1;
    options.max_nodes = 1;
    options.hold_capacity = 0; // control queue: (1 + 1 * (0 + 2) + 0 sessions + 0 responses) * 2 = 6
    options.bam_receive_sessions = 0;
    options.rts_receive_sessions = 0;
    options.bam_transmit_sessions = 0;
    options.rts_transmit_sessions = 0;
    options.response_queue_capacity = 0;
    Network network(options);
    network.add_node({kUs});
    ScriptedPort port;
    for (int i = 0; i < 8; ++i)
    {
        port.inbox.push_back(can::TimedFrame{at(std::chrono::milliseconds(10 * i)),
                                             to_can(frame(6, kPgnRequest, kTool, kUs, request(0xFEF1)))});
    }
    // The port stays full: every NACK waits in the control queue.
    port.script.assign(8, can::SendResult::QueueFull);
    for (int i = 1; i <= 8; ++i)
    {
        step_by_hand(network, port, at(std::chrono::milliseconds(10 * i)));
    }
    CHECK_EQ(network.statistics().control_frames_dropped, 2u);
    port.script.clear();
    step_by_hand(network, port, at(90ms));
    CHECK_EQ(port.accepted.size(), 6u);
}

TEST_CASE(receive_capacity_overflow_drops_and_counts)
{
    NetworkOptions options;
    options.receive_capacity = 3;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    int seen = 0;
    us.on_any_message([&](const Message&) { ++seen; });
    for (int i = 0; i < 5; ++i)
    {
        bench.inject(at(1ms), frame(6, kPgnPropB, kTool, kGlobal, {}));
    }
    bench.steps(2);
    CHECK_EQ(seen, 3);
    CHECK_EQ(bench.network.statistics().receive_overflows, 2u);
    // The next step takes frames again.
    bench.inject(at(15ms), frame(6, kPgnPropB, kTool, kGlobal, {}));
    bench.step();
    CHECK_EQ(seen, 4);
}

TEST_CASE(node_limit_and_address_checks)
{
    NetworkOptions options;
    options.max_nodes = 2;
    Network network(options);
    network.add_node({0});
    network.add_node({253});
    CHECK(throws<std::length_error>([&] { network.add_node({1}); }));
    Network other;
    CHECK(throws<std::invalid_argument>([&] { other.add_node({kNull}); }));
    CHECK(throws<std::invalid_argument>([&] { other.add_node({kGlobal}); }));
    NetworkOptions zero;
    zero.receive_capacity = 0;
    CHECK(throws<std::invalid_argument>([&] { Network bad(zero); }));
}

TEST_CASE(port_queue_full_frames_are_looped_back_only_once_accepted)
{
    Network network;
    Node& a = network.add_node({kUs});
    Node& b = network.add_node({kOther});
    std::vector<std::pair<std::uint8_t, std::int64_t>> seenB;
    b.on_any_message([&](const Message& m) { seenB.emplace_back(m.data[0], to_ns(m.time)); });
    ScriptedPort port;
    port.script = {can::SendResult::QueueFull};
    step_by_hand(network, port, at(0ms), [&] {
        const std::array<std::uint8_t, 1> one{7};
        CHECK(a.send(Pgn{kPgnPropB}, one) == SendStatus::Ok);
    });
    step_by_hand(network, port, at(10ms)); // b sees nothing yet; the frame leaves now
    CHECK(seenB.empty());
    step_by_hand(network, port, at(20ms));
    // Once, with the time of the step that sent it.
    CHECK((seenB == std::vector<std::pair<std::uint8_t, std::int64_t>>{{7, 10000000}}));
}

TEST_CASE(handler_added_for_another_node_while_delivering_misses_the_current_frame)
{
    Bench bench;
    Node& a = bench.add_node(kUs);
    Node& b = bench.add_node(kOther);
    std::vector<std::string> log;
    bool added = false;
    a.on_any_message([&](const Message& m) {
        if (!added)
        {
            added = true;
            // Enough registrations to make the handler list grow while it is being delivered.
            for (int i = 0; i < 64; ++i)
            {
                a.on_any_message([](const Message&) {});
                b.on_any_message([&log, i](const Message& m2) {
                    if (i == 0)
                    {
                        log.push_back("b" + std::to_string(m2.data[0]));
                    }
                });
            }
            log.push_back("a" + std::to_string(m.data[0]));
        }
    });
    bench.inject(at(1ms), frame(6, kPgnPropB, kTool, kGlobal, bytes({1})));
    bench.inject(at(2ms), frame(6, kPgnPropB, kTool, kGlobal, bytes({2})));
    bench.steps(2);
    CHECK(log == (std::vector<std::string>{"a1", "b2"}));
}

namespace {

/// Restarts itself at the network's current step time (a delay of 0), three times.
struct ZeroDelayTimer final : TimerTarget
{
    Network* network = nullptr;
    std::vector<std::string>* log = nullptr;
    void on_timer(std::uint32_t tag) override
    {
        log->push_back("timer" + std::to_string(tag) + "@" + std::to_string(to_ns(*network->now()) / 1000000));
        if (tag < 3)
        {
            NetworkTestAccess::timers(*network).start(TimerKind::Scheduled, *network->now(), *this, tag + 1);
        }
    }
};

} // namespace

TEST_CASE(timer_restarted_at_a_due_deadline_fires_in_the_next_round_without_failing)
{
    // A timer restarted at the deadline fired last in the round waits for the next round: at the
    // end of a step the next step, before a frame the next frame boundary or the end of the step.
    // A later deadline (timer2, fired late at 20 ms, restarts at 20 ms) fires in the same round.
    for (const SimDuration first : {SimDuration{10ms}, SimDuration{5ms}})
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        std::vector<std::string> log;
        us.on_any_message([&](const Message& m) { log.push_back("frame" + std::to_string(to_ns(m.time) / 1000000)); });
        ZeroDelayTimer timer;
        timer.network = &bench.network;
        timer.log = &log;
        NetworkTestAccess::timers(bench.network).start(TimerKind::Scheduled, at(first), timer, 1);
        bench.inject(at(7ms), frame(6, kPgnPropB, kTool, kGlobal, {}));
        bench.steps(5);
        CHECK(!bench.network.failed());
        const std::vector<std::string> expected =
            first == SimDuration{10ms} ? std::vector<std::string>{"frame7", "timer1@10", "timer2@20", "timer3@20"}
                                       : std::vector<std::string>{"timer1@10", "frame7", "timer2@10", "timer3@20"};
        if (log != expected)
        {
            std::string text;
            for (const auto& l : log)
            {
                text += l + " ";
            }
            throw vehicle_sil::unit::Failure(text);
        }
    }
}
