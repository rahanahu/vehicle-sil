// SPDX-License-Identifier: MIT
//
// The toolbox of the J1939 stack (Subscription, Timer, Node::attach) and the engine
// entries the protocol parts use (send() with WhenBusy::Queue, PendingRequest): ownership,
// removal and destruction during deliveries and timers, the reserved timer slots, virtual time
// independent of the step size, and the order in which a network destroys its parts.
#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"
#include "network_test_access.hpp"
#include "test_harness.hpp"
#include "vehicle_sil/j1939/name.hpp"
#include "vehicle_sil/j1939/toolbox.hpp"

#include <memory>
#include <stdexcept>
#include <optional>
#include <string>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::to_ns;
using vehicle_sil::j1939::AckCode;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Network;
using vehicle_sil::j1939::NetworkOptions;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::NodeOptions;
using vehicle_sil::j1939::PendingRequest;
using vehicle_sil::j1939::RequestContext;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::Subscription;
using vehicle_sil::j1939::Timer;
using vehicle_sil::j1939::TimerKind;
using vehicle_sil::j1939::WhenBusy;
using vehicle_sil::j1939::detail::NetworkTestAccess;
using vehicle_sil::unit::throws;

namespace {

constexpr Address kUs = 0x20;
constexpr Address kOther = 0x21;
constexpr Address kTool = 0x90;

Bytes bytes(std::initializer_list<std::uint8_t> b) { return b; }

J1939Frame prop_b(std::uint8_t value, Address sa = kTool) { return frame(6, kPgnPropB, sa, kGlobal, {value}); }

[[nodiscard]] long long ms_of(SimTime t) { return static_cast<long long>(to_ns(t) / 1000000); }

} // namespace

// --- Subscription

TEST_CASE(subscription_removes_its_handler_when_reset_or_destroyed)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    bench.step(); // frames injected from here on reach the network in the next step
    std::vector<int> seen;
    Subscription kept{us, us.on_message(Pgn{kPgnPropB}, [&](const Message& m) { seen.push_back(m.data[0]); })};
    {
        Subscription scoped{us, us.on_message(Pgn{kPgnPropB}, [&](const Message& m) { seen.push_back(100 + m.data[0]); })};
        CHECK(scoped.active());
        bench.inject(prop_b(1));
        bench.step();
    }
    bench.inject(prop_b(2));
    bench.step();
    Subscription moved = std::move(kept);
    CHECK(!kept.active());
    CHECK(moved.active());
    bench.inject(prop_b(3));
    bench.step();
    moved.reset();
    CHECK(!moved.active());
    bench.inject(prop_b(4));
    bench.step();
    CHECK(seen == (std::vector<int>{1, 101, 2, 3}));
}

TEST_CASE(subscription_reset_during_a_delivery_is_not_called_for_the_rest_of_it)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    bench.step(); // frames injected from here on reach the network in the next step
    std::vector<std::string> log;
    Subscription second;
    std::optional<Subscription> self;
    Subscription first{us, us.on_message(Pgn{kPgnPropB}, [&](const Message& m) {
        log.push_back("first" + std::to_string(m.data[0]));
        second.reset(); // later in the same delivery: not called any more
        self.reset();   // the running handler itself
    })};
    second = Subscription{us, us.on_message(Pgn{kPgnPropB}, [&](const Message&) { log.push_back("second"); })};
    self.emplace(us, us.on_message(Pgn{kPgnPropB}, [&](const Message&) { log.push_back("self"); }));
    bench.inject(prop_b(1));
    bench.inject(prop_b(2));
    bench.step();
    CHECK(log == (std::vector<std::string>{"first1", "first2"}));
}

TEST_CASE(subscription_and_timer_outlive_their_network_safely)
{
    auto bench = std::make_unique<Bench>();
    Node& us = bench->add_node(kUs);
    Subscription sub{us, us.on_any_message([](const Message&) {})};
    int fired = 0;
    Timer timer(us, TimerKind::Scheduled, [&] { ++fired; });
    timer.start_at(at(50ms));
    bench.reset();
    CHECK(!sub.active());
    CHECK(!timer.running());
    CHECK(!timer.cancel());
    CHECK(!timer.deadline().has_value());
    CHECK(throws<std::logic_error>([&] { timer.start_at(at(60ms)); }));
    sub.reset();
    CHECK_EQ(fired, 0);
}

// --- Timer

TEST_CASE(timer_fires_once_per_start_and_can_be_restarted_and_cancelled)
{
    Bench bench;
    bench.add_node(kUs);
    std::vector<long long> fired;
    Timer timer(bench.network, TimerKind::Scheduled, [&] { fired.push_back(ms_of(*bench.network.now())); });
    CHECK(!timer.running());
    timer.start_at(at(30ms));
    CHECK(timer.deadline() == at(30ms));
    timer.start_at(at(20ms)); // restart: only the new deadline
    bench.steps(5);           // 0 .. 40
    CHECK(fired == (std::vector<long long>{20}));
    CHECK(!timer.running());
    timer.start_at(at(60ms));
    CHECK(timer.cancel());
    CHECK(!timer.cancel());
    bench.steps(3);
    CHECK(fired == (std::vector<long long>{20}));
    CHECK(throws<std::invalid_argument>([&] { Timer empty(bench.network, TimerKind::Scheduled, {}); }));
}

namespace {

/// A timer that restarts itself every 7 ms from its deadline, and a timeout 5 ms after the frame
/// at 13 ms: the deadlines they fire with, and the frames between them.
std::vector<std::string> periodic_and_frame_timer(SimDuration step)
{
    Bench bench({}, step);
    Node& us = bench.add_node(kUs);
    std::vector<std::string> log;
    std::optional<Timer> periodic;
    periodic.emplace(us, TimerKind::Scheduled, [&] {
        log.push_back("p" + std::to_string(ms_of(*periodic->deadline())));
        periodic->start_after(7ms); // from the deadline, not from the step
    });
    Timer timeout(us, TimerKind::Timeout, [&] { log.push_back("t" + std::to_string(ms_of(*timeout.deadline()))); });
    Subscription sub{us, us.on_message(Pgn{kPgnPropB}, [&](const Message& m) {
        log.push_back("f" + std::to_string(ms_of(m.time)));
        timeout.start_after(5ms); // from the frame's time
    })};
    periodic->start_at(at(3ms));
    bench.inject(at(13ms), prop_b(1));
    bench.steps(static_cast<int>(30ms / step) + 1);
    return log;
}

} // namespace

TEST_CASE(timer_start_after_counts_from_the_event_independent_of_the_step_size)
{
    // The deadline is read in the callback before the restart, so the log shows the deadline
    // each firing had. Firings in the same step keep (deadline, start) order around the frame.
    const std::vector<std::string> expected{"p3", "p10", "f13", "p17", "t18", "p24"};
    for (const SimDuration step : {SimDuration{1ms}, SimDuration{3ms}, SimDuration{5ms}, SimDuration{10ms}})
    {
        const auto log = periodic_and_frame_timer(step);
        std::string text;
        for (const auto& l : log)
        {
            text += l + " ";
        }
        if (log != expected)
        {
            throw vehicle_sil::unit::Failure("step " + std::to_string(step.count()) + " ns: " + text);
        }
    }
}

TEST_CASE(timer_started_outside_a_step_with_a_past_deadline_fires_in_the_next_step)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Timer timer(us, TimerKind::Scheduled, [&] { CHECK(us.send(Pgn{kPgnPropB}, bytes({9})) == SendStatus::Ok); });
    bench.steps(3); // 0, 10, 20
    timer.start_at(at(5ms)); // between steps, already past
    bench.step();            // 30
    CHECK_J1939(bench.take_sent(), {frame(6, kPgnPropB, kUs, kGlobal, {9})});
    CHECK(bench.sent_times == (std::vector<SimTime>{at(30ms)}));
}

TEST_CASE(timer_restarting_itself_at_a_due_deadline_fires_once_per_step)
{
    Bench bench;
    bench.add_node(kUs);
    int fired = 0;
    std::optional<Timer> timer;
    timer.emplace(bench.network, TimerKind::Scheduled, [&] {
        ++fired;
        timer->start_at(at(0ms)); // always due: waits for the next round
    });
    timer->start_at(at(0ms));
    bench.steps(4);
    CHECK_EQ(fired, 4);
}

TEST_CASE(timer_destroyed_in_its_own_callback_is_safe)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    std::unique_ptr<Timer> timer;
    timer = std::make_unique<Timer>(us, TimerKind::Scheduled, [&] {
        timer.reset(); // destroys the Timer whose callback runs
        CHECK(us.send(Pgn{kPgnPropB}, bytes({1})) == SendStatus::Ok);
    });
    std::unique_ptr<Timer> other =
        std::make_unique<Timer>(us, TimerKind::Scheduled, [&] { CHECK(us.send(Pgn{kPgnPropB}, bytes({2})) == SendStatus::Ok); });
    Timer killer(us, TimerKind::Scheduled, [&] { other.reset(); }); // another one, still waiting
    killer.start_at(at(10ms));
    other->start_at(at(10ms)); // same deadline, started later: would fire after `killer`
    timer->start_at(at(10ms));
    bench.steps(3);
    CHECK(timer == nullptr);
    CHECK(other == nullptr);
    CHECK_J1939(bench.take_sent(), {frame(6, kPgnPropB, kUs, kGlobal, {1})});
}

TEST_CASE(timer_slots_are_reserved_and_address_claiming_keeps_its_own)
{
    NetworkOptions options;
    options.timer_capacity = 4;
    Bench bench(options);
    NodeOptions named;
    named.address = kUs;
    named.name = Name{0x20008200FE001234u};
    bench.network.add_node(named); // reserves 2
    auto a = std::make_unique<Timer>(bench.network, TimerKind::Scheduled, [] {});
    Timer b(bench.network, TimerKind::Timeout, [] {});
    CHECK(throws<std::length_error>([&] { Timer c(bench.network, TimerKind::Scheduled, [] {}); }));
    named.address = kOther;
    named.name = Name{0x20008200FE001235u};
    CHECK(throws<std::invalid_argument>([&] { bench.network.add_node(named); }));
    a.reset(); // returns its slot... still one short of the 2 a claiming node needs
    CHECK(throws<std::invalid_argument>([&] { bench.network.add_node(named); }));
    CHECK_EQ(NetworkTestAccess::timers(bench.network).reserved(), 3u);
    // Every reserved timer can run at the same time as the claim: start never fails.
    b.start_at(at(100ms));
    bench.steps(30); // the claim's 250 ms wait runs meanwhile
    CHECK(bench.network.node(0).address_state() == AddressState::Claimed);
}

// --- attach

namespace {

struct Part
{
    Part(Node& node, std::string name, std::vector<std::string>& log)
      : node_(node), name_(std::move(name)), log_(log),
        timer_(node, TimerKind::Scheduled, [] {}),
        sub_(node, node.on_any_message([this](const Message&) { log_.push_back(name_ + " got"); }))
    {
        timer_.start_at(at(1h));
    }
    ~Part()
    {
        // The node and the network still work while the parts are destroyed.
        log_.push_back(name_ + " destroyed at " + std::to_string(node_.address()) + (timer_.running() ? " running" : ""));
    }
    Node& node_;
    std::string name_;
    std::vector<std::string>& log_;
    Timer timer_;
    Subscription sub_;
};

struct OtherPart
{
    explicit OtherPart(Node&) {}
};

} // namespace

TEST_CASE(attached_parts_are_destroyed_first_last_attached_first)
{
    std::vector<std::string> log;
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        Node& other = bench.add_node(kOther);
        Part& a = us.attach<Part>("a", log);
        other.attach<Part>("b", log);
        Part& c = us.attach<Part>("c", log);
        CHECK(us.find<Part>() == &a);
        CHECK(other.find<Part>() != nullptr);
        CHECK(us.find<OtherPart>() == nullptr);
        CHECK(&c != &a);
        CHECK_EQ(NetworkTestAccess::timers(bench.network).reserved(), 3u);
    }
    CHECK(log == (std::vector<std::string>{"c destroyed at 32 running", "b destroyed at 33 running", "a destroyed at 32 running"}));
}

TEST_CASE(a_part_attached_in_a_handler_receives_from_the_next_message)
{
    std::vector<std::string> log;
    Bench bench;
    Node& us = bench.add_node(kUs);
    bench.step(); // frames injected from here on reach the network in the next step
    Subscription sub{us, us.on_message(Pgn{kPgnPropB}, [&](const Message&) {
        if (us.find<Part>() == nullptr)
        {
            us.attach<Part>("late", log);
        }
    })};
    bench.inject(prop_b(1));
    bench.inject(prop_b(2));
    bench.step();
    CHECK(log == (std::vector<std::string>{"late got"}));
}

// --- send() with WhenBusy::Queue

TEST_CASE(send_with_when_busy_queue_waits_for_the_transfer_before_it)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    std::vector<vehicle_sil::j1939::TransferEnd> ends;
    Subscription sub{us, us.on_transfer_end([&](const auto& end) { ends.push_back(end); })};
    const Bytes first = payload(10, 1);
    const Bytes second = payload(12, 2);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgnPropB}, first) == SendStatus::Ok);
            CHECK(us.send(Pgn{kPgnPropB}, second) == SendStatus::Busy);
            CHECK(us.send(Pgn{kPgnPropB}, second, kGlobal, 6, WhenBusy::Queue) == SendStatus::Ok);
        }
    };
    bench.steps(30);
    const auto sent = bench.take_sent();
    std::vector<J1939Frame> announcements;
    for (const auto& f : sent)
    {
        if (f.pgn == kPgnTpCm)
        {
            announcements.push_back(f);
        }
    }
    CHECK_J1939(announcements, {bam(kUs, 10, kPgnPropB), bam(kUs, 12, kPgnPropB)});
    CHECK_EQ(ends.size(), 2u);
    CHECK_EQ(ends[0].length, 10u);
    CHECK_EQ(ends[1].length, 12u);
    CHECK(ends[1].time > ends[0].time);
}

// --- PendingRequest

TEST_CASE(a_deferred_request_is_answered_later)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    bench.step(); // frames injected from here on reach the network in the next step
    std::vector<PendingRequest> kept;
    Subscription a{us, us.on_request(Pgn{kPgnPropA}, [&](RequestContext& r) { kept.push_back(r.defer()); })};
    Subscription b{us, us.on_request(Pgn{kPgnPropB}, [&](RequestContext& r) { kept.push_back(r.defer()); })};
    bench.inject(frame(6, kPgnRequest, kTool, kUs, request(kPgnPropA)));
    bench.inject(frame(6, kPgnRequest, kTool, kGlobal, request(kPgnPropB)));
    bench.step();
    CHECK_J1939(bench.take_sent(), {}); // no NACK, no answer yet
    CHECK_EQ(kept.size(), 2u);
    CHECK(kept[0].pgn() == Pgn{kPgnPropA});
    CHECK_EQ(kept[0].requester(), kTool);
    CHECK(kept[0].is_destination_specific());
    CHECK(!kept[1].is_destination_specific());
    CHECK(throws<std::logic_error>([&] { static_cast<void>(us.respond(kept[0], bytes({1}))); })); // outside a step
    bench.app = [&](SimTime) {
        CHECK(us.respond(kept[0], bytes({1, 2})) == SendStatus::Ok);
        CHECK(us.acknowledge(kept[0], AckCode::AccessDenied));
        CHECK(us.respond(kept[1], bytes({3})) == SendStatus::Ok);
        CHECK(!us.acknowledge(kept[1], AckCode::Positive)); // a global request is not acknowledged
        bench.app = nullptr;
    };
    bench.step();
    CHECK_J1939(bench.take_sent(), {frame(6, kPgnAck, kUs, kGlobal, acknowledgement(kAccessDenied, kTool, kPgnPropA)),
                                    frame(6, kPgnPropA, kUs, kTool, {1, 2}), frame(6, kPgnPropB, kUs, kGlobal, {3})});
}

TEST_CASE(a_deferred_request_is_refused_after_the_node_changed_its_address)
{
    Bench bench;
    NodeOptions options;
    options.address = 0x80;
    options.name = Name{0x80008200FE001234u}; // arbitrary address capable
    Node& us = bench.network.add_node(options);
    Node& other = bench.add_node(kOther);
    std::optional<PendingRequest> kept;
    Subscription a{us, us.on_request(Pgn{kPgnPropA}, [&](RequestContext& r) { kept = r.defer(); })};
    bench.steps(30); // claimed 0x80
    bench.inject(frame(6, kPgnRequest, kTool, 0x80, request(kPgnPropA)));
    bench.step();
    CHECK(kept.has_value());
    // A lower NAME takes 0x80: the node moves to 0x81.
    Bytes lower{0x01, 0, 0, 0, 0, 0, 0, 0};
    bench.inject(frame(6, kPgnAddressClaim, 0x80, kGlobal, lower));
    bench.step();
    CHECK_EQ(us.address(), 0x81);
    (void)bench.take_sent();
    bench.app = [&](SimTime) {
        CHECK(us.respond(*kept, bytes({1})) == SendStatus::NoAddress);
        CHECK(!us.acknowledge(*kept, AckCode::Positive));
        CHECK(throws<std::invalid_argument>([&] { static_cast<void>(other.respond(*kept, bytes({1}))); }));
        bench.app = nullptr;
    };
    bench.step();
    CHECK(!bench.network.failed());
    CHECK_J1939(bench.take_sent(), {});
}

TEST_CASE(a_timer_callback_that_throws_fails_the_network)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Timer timer(us, TimerKind::Scheduled, [] { throw std::runtime_error("boom"); });
    timer.start_at(at(10ms));
    bench.step();
    CHECK(throws<std::runtime_error>([&] { bench.step(); }));
    CHECK(bench.network.failed());
    CHECK(!timer.running());
    CHECK(bench.network.event_time() == *bench.network.now()); // not left at the deadline
}

namespace {

/// Owns a Subscription; held by the captures of another handler.
struct Holder
{
    Subscription sub;
};

} // namespace

TEST_CASE(destroying_a_handler_may_remove_other_handlers)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    bench.step();
    std::vector<std::string> log;
    // The captures of `outer` own the Subscription of `inner`: removing `outer` destroys them,
    // which removes `inner` from the same list while it is being changed.
    auto holder = std::make_shared<Holder>();
    holder->sub = Subscription{us, us.on_message(Pgn{kPgnPropB}, [&](const Message&) { log.push_back("inner"); })};
    auto outer = std::make_optional<Subscription>(us, us.on_message(Pgn{kPgnPropB}, [&log, h = std::move(holder)](const Message&) {
                                                        log.push_back("outer");
                                                    }));
    outer.reset(); // outside a delivery
    bench.inject(prop_b(1));
    bench.step();
    CHECK(log.empty());

    // The same during a delivery: removed at once, destroyed after it.
    holder = std::make_shared<Holder>();
    holder->sub = Subscription{us, us.on_message(Pgn{kPgnPropB}, [&](const Message&) { log.push_back("inner"); })};
    std::optional<Subscription> self;
    self.emplace(us, us.on_message(Pgn{kPgnPropB}, [&log, &self, h = std::move(holder)](const Message&) {
        log.push_back("self");
        self.reset();
    }));
    bench.inject(prop_b(2));
    bench.inject(prop_b(3));
    bench.step();
    CHECK(log == (std::vector<std::string>{"inner", "self"})); // frame 3: both gone
}

TEST_CASE(handlers_still_registered_when_the_network_is_destroyed_may_own_subscriptions)
{
    // The handler's captures own a Subscription of the same node: destroyed with the node, it
    // must find the network gone and do nothing.
    std::vector<std::string> log;
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        auto holder = std::make_shared<Holder>();
        holder->sub = Subscription{us, us.on_message(Pgn{kPgnPropB}, [&](const Message&) { log.push_back("inner"); })};
        us.on_message(Pgn{kPgnPropB}, [h = std::move(holder)](const Message&) {});
        auto timerHolder = std::make_shared<std::optional<Timer>>();
        timerHolder->emplace(us, TimerKind::Scheduled, [] {});
        (*timerHolder)->start_at(at(1h));
        us.on_message(Pgn{kPgnPropA}, [t = std::move(timerHolder)](const Message&) {});
    }
    CHECK(log.empty());
}

namespace {

/// A connection of 10 bytes from the node to the tester, acknowledged at 13 ms; a Timer started
/// with start_after(5 ms) in the transfer end handler.
std::optional<SimTime> deadline_after_transfer_end(SimDuration step)
{
    Bench bench({}, step);
    Node& us = bench.add_node(kUs);
    std::optional<SimTime> deadline;
    Timer timer(us, TimerKind::Scheduled, [] {});
    Subscription sub{us, us.on_transfer_end([&](const vehicle_sil::j1939::TransferEnd&) {
        timer.start_after(5ms);
        deadline = timer.deadline();
    })};
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgnPropA}, payload(10), kTool) == SendStatus::Ok);
        }
    };
    bench.inject(at(1ms), cts(kTool, kUs, 2, 1, kPgnPropA));
    bench.inject(at(13ms), eoma(kTool, kUs, 10, kPgnPropA));
    bench.steps(static_cast<int>(40ms / step));
    return deadline;
}

} // namespace

TEST_CASE(start_after_in_a_transfer_end_handler_counts_from_the_end)
{
    for (const SimDuration step : {SimDuration{1ms}, SimDuration{5ms}, SimDuration{10ms}})
    {
        CHECK(deadline_after_transfer_end(step) == at(18ms));
    }
}
