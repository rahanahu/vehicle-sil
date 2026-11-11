// SPDX-License-Identifier: MIT
//
// The toolbox of the J1939 stack (Subscription, Timer, Node::attach) and the engine entries the
// protocol parts use (send() with WhenBusy::Queue, PendingRequest): ownership, removal and
// destruction during deliveries and timers, the reserved timer slots and Network::timers_free(),
// virtual time independent of the step size, and the order in which a network destroys its parts.
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
using vehicle_sil::unit::throws_with;

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

TEST_CASE(a_periodic_timer_shorter_than_the_step_fires_every_deadline_without_falling_behind)
{
    // No frame: every deadline of a step is caught up at its end, however wide the step is.
    std::vector<long long> expected;
    for (long long d = 2; d <= 300; d += 3)
    {
        expected.push_back(d);
    }
    for (const SimDuration step : {SimDuration{1ms}, SimDuration{10ms}, SimDuration{20ms}, SimDuration{50ms}})
    {
        Bench bench({}, step);
        Node& us = bench.add_node(kUs);
        std::vector<long long> fired;
        std::optional<Timer> periodic;
        periodic.emplace(us, TimerKind::Scheduled, [&] {
            fired.push_back(ms_of(*periodic->deadline()));
            periodic->start_after(3ms);
        });
        periodic->start_at(at(2ms));
        bench.steps(static_cast<int>(300ms / step) + 1); // the last step at 300 ms
        if (fired != expected)
        {
            throw vehicle_sil::unit::Failure("step " + std::to_string(step.count()) + " ns: " + std::to_string(fired.size()) +
                                             " firings, the last at " + std::to_string(fired.empty() ? -1 : fired.back()));
        }
        // Nothing overdue is left for the driver to wake up for.
        CHECK(bench.network.next_deadline() == std::optional<SimTime>{at(302ms)});
    }
}

TEST_CASE(periodic_timers_with_the_same_deadlines_fire_alike_whatever_the_step_size)
{
    // Periods of 10 ms and 20 ms from 0 (the same deadlines every 20 ms) and 3 ms from 2 ms: what
    // one timer starts in its callback is compared with its own deadline, not with the deadline
    // fired last, so the other timer of the same deadline does not hold it back.
    std::vector<std::string> reference;
    for (const SimDuration step : {SimDuration{1ms}, SimDuration{10ms}, SimDuration{50ms}, SimDuration{200ms}})
    {
        Bench bench({}, step);
        Node& us = bench.add_node(kUs);
        std::vector<std::string> log;
        std::optional<Timer> a;
        std::optional<Timer> b;
        std::optional<Timer> c;
        a.emplace(us, TimerKind::Scheduled, [&] {
            log.push_back("a" + std::to_string(ms_of(*a->deadline())));
            a->start_after(10ms);
        });
        b.emplace(us, TimerKind::Scheduled, [&] {
            log.push_back("b" + std::to_string(ms_of(*b->deadline())));
            b->start_after(20ms);
        });
        c.emplace(us, TimerKind::Scheduled, [&] {
            log.push_back("c" + std::to_string(ms_of(*c->deadline())));
            c->start_after(3ms);
        });
        a->start_at(at(0ms));
        b->start_at(at(0ms));
        c->start_at(at(2ms));
        bench.steps(static_cast<int>(1000ms / step) + 1); // the last step at 1000 ms
        if (step == SimDuration{1ms})
        {
            reference = log;
            CHECK_EQ(log.size(), 101u + 51u + 333u);
        }
        else if (log != reference)
        {
            std::size_t i = 0;
            while (i < log.size() && i < reference.size() && log[i] == reference[i])
            {
                ++i;
            }
            throw vehicle_sil::unit::Failure("step " + std::to_string(step.count()) + " ns: differs at " + std::to_string(i) +
                                             (i < log.size() ? " (" + log[i] + ")" : std::string(" (end)")));
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

TEST_CASE(timers_free_counts_what_the_nodes_and_timers_hold_and_not_the_transport_sessions)
{
    NetworkOptions options;
    options.timer_capacity = 10;
    Bench bench(options);
    CHECK_EQ(bench.network.timers_free(), 10u);
    NodeOptions named;
    named.address = kUs;
    named.name = Name{0x20008200FE001234u};
    Node& us = bench.network.add_node(named);
    CHECK_EQ(bench.network.timers_free(), 8u); // 2 for its address claim
    Node& plain = bench.add_node(kOther);      // without a NAME: none
    CHECK_EQ(bench.network.timers_free(), 8u);
    auto a = std::make_unique<Timer>(bench.network, TimerKind::Scheduled, [] {});
    {
        Timer b(us, TimerKind::Timeout, [] {});
        CHECK_EQ(bench.network.timers_free(), 6u);
    }
    CHECK_EQ(bench.network.timers_free(), 7u);
    // A transfer runs on a timer of its own session.
    bench.steps(30); // claimed
    const std::size_t running = NetworkTestAccess::timers(bench.network).running();
    bench.app = [&](SimTime now) {
        if (now == at(300ms))
        {
            CHECK(plain.send(Pgn{kPgnPropB}, payload(20)) == SendStatus::Ok); // a BAM: 3 packets 50 ms apart
        }
    };
    bench.steps(2); // 300, 310 ms
    CHECK(NetworkTestAccess::timers(bench.network).running() > running);
    CHECK_EQ(bench.network.timers_free(), 7u);
    a.reset();
    CHECK_EQ(bench.network.timers_free(), 8u);
    // None free: a Timer says what is full and what to do.
    std::vector<std::unique_ptr<Timer>> all;
    while (bench.network.timers_free() > 0)
    {
        all.push_back(std::make_unique<Timer>(bench.network, TimerKind::Scheduled, [] {}));
    }
    CHECK_EQ(all.size(), 8u);
    CHECK(throws_with<std::length_error>([&] { Timer c(bench.network, TimerKind::Scheduled, [] {}); },
                                         "all 10 of NetworkOptions::timer_capacity are taken"));
    CHECK(throws_with<std::length_error>([&] { Timer c(bench.network, TimerKind::Scheduled, [] {}); }, "Network::timers_free()"));
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
    bench.inject(frame(6, kPgnRequest, kTool, kGlobal, vehicle_sil::unit::j1939::request(kPgnPropB)));
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

// --- TransferId: matches a send to the end of its transfer

TEST_CASE(transfer_ids_match_each_send_to_the_end_of_its_transfer)
{
    using vehicle_sil::j1939::TransferEnd;
    using vehicle_sil::j1939::TransferId;
    using vehicle_sil::j1939::TransferResult;
    Bench bench;
    Node& us = bench.add_node(kUs);
    bench.add_node(kOther);
    std::vector<TransferEnd> ends;
    Subscription sub{us, us.on_transfer_end([&](const TransferEnd& end) { ends.push_back(end); })};
    TransferId bam{}, rts{}, queued{}, queuedBam{};
    bench.app = [&](SimTime now) {
        if (now != at(0ms))
        {
            return;
        }
        CHECK(us.send(Pgn{kPgnPropB}, payload(8)) == SendStatus::Ok);
        CHECK(!us.last_transfer_id()); // a single frame
        CHECK(us.send(Pgn{kPgnPropB}, payload(10)) == SendStatus::Ok);
        bam = us.last_transfer_id();
        CHECK(us.send(Pgn{kPgnPropB}, payload(11)) == SendStatus::Busy);
        CHECK(!us.last_transfer_id()); // refused
        CHECK(us.send(Pgn{kPgnPropA}, payload(12), kOther) == SendStatus::Ok);
        rts = us.last_transfer_id();
        // Waits behind the connection to the same destination: the id is given now.
        CHECK(us.send(Pgn{kPgnPropA}, payload(12), kOther, 6, WhenBusy::Queue) == SendStatus::Ok);
        queued = us.last_transfer_id();
        CHECK(us.send(Pgn{kPgnPropB}, payload(13), kGlobal, 6, WhenBusy::Queue) == SendStatus::Ok);
        queuedBam = us.last_transfer_id();
    };
    bench.steps(200);
    CHECK(bam && rts && queued && queuedBam);
    CHECK(bam != rts && rts != queued && queued != queuedBam && bam != queuedBam);
    CHECK_EQ(ends.size(), 4u);
    const auto end_of = [&](TransferId id) -> const TransferEnd* {
        for (const auto& e : ends)
        {
            if (e.id == id)
            {
                return &e;
            }
        }
        return nullptr;
    };
    // The two connections have the same PGN, destination and length: only the id tells them apart.
    for (const TransferId id : {bam, rts, queued, queuedBam})
    {
        const TransferEnd* e = end_of(id);
        CHECK(e != nullptr && e->result == TransferResult::Completed);
    }
    CHECK(end_of(bam) != nullptr && end_of(bam)->length == 10u);
    CHECK(end_of(queuedBam) != nullptr && end_of(queuedBam)->length == 13u);
    CHECK(end_of(rts) != nullptr && end_of(queued) != nullptr && end_of(rts)->time < end_of(queued)->time);
}

TEST_CASE(transfer_ids_of_cancelled_and_queued_transfers_reach_their_ends)
{
    using vehicle_sil::j1939::TransferEnd;
    using vehicle_sil::j1939::TransferId;
    using vehicle_sil::j1939::TransferResult;
    // A NAME the tester's claim beats (lower NAME wins): our node loses its address, its
    // transfer and the one queued behind it are cancelled.
    constexpr std::uint64_t kOurName = 0x0000'1000'0000'0002u;
    Bench bench;
    Node& us = bench.network.add_node({.address = kUs, .name = Name{kOurName}});
    std::vector<TransferEnd> ends;
    Subscription sub{us, us.on_transfer_end([&](const TransferEnd& end) { ends.push_back(end); })};
    TransferId first{}, second{};
    bench.app = [&](SimTime now) {
        if (now == at(300ms))
        {
            CHECK(us.send(Pgn{kPgnPropA}, payload(20), kTool) == SendStatus::Ok);
            first = us.last_transfer_id();
            CHECK(us.send(Pgn{kPgnPropA}, payload(20), kTool, 6, WhenBusy::Queue) == SendStatus::Ok);
            second = us.last_transfer_id();
        }
    };
    bench.steps(32); // the RTS has left; no clear-to-send comes
    Bytes lower(8);
    for (int i = 0; i < 8; ++i)
    {
        lower[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((kOurName - 1) >> (8 * i));
    }
    bench.inject(frame(6, kPgnAddressClaim, kUs, kGlobal, lower));
    bench.steps(2);
    CHECK_EQ(ends.size(), 2u);
    CHECK(ends.size() == 2 && ends[0].id == first && ends[1].id == second);
    CHECK(ends.size() == 2 && ends[0].result == TransferResult::Cancelled && ends[1].result == TransferResult::Cancelled);
}

TEST_CASE(transfer_ids_of_held_transfers_and_long_responses_reach_their_ends)
{
    using vehicle_sil::j1939::TransferEnd;
    using vehicle_sil::j1939::TransferId;
    using vehicle_sil::j1939::TransferResult;
    Bench bench;
    // 0x90 (128 .. 247): the node waits 250 ms after its claim; its transfer is held meanwhile.
    Node& us = bench.network.add_node({.address = 0x90, .name = Name{}.set_identity_number(7).set_manufacturer_code(0x7F0)});
    std::vector<TransferEnd> ends;
    Subscription sub{us, us.on_transfer_end([&](const TransferEnd& end) { ends.push_back(end); })};
    TransferId held{}, response{};
    std::optional<PendingRequest> request;
    Subscription responder{us, us.on_request(Pgn{kPgnPropB}, [&](RequestContext& r) {
        CHECK(r.respond(payload(30)) == SendStatus::Ok);
        response = us.last_transfer_id();
        request = r.defer();
    })};
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            CHECK(us.send(Pgn{kPgnPropB}, payload(20)) == SendStatus::Ok);
            held = us.last_transfer_id();
        }
    };
    bench.steps(40);
    bench.inject(frame(6, kPgnRequest, kTool, kGlobal, vehicle_sil::unit::j1939::request(kPgnPropB)));
    bench.steps(60);
    CHECK(held && response && held != response);
    CHECK_EQ(ends.size(), 2u);
    CHECK(ends.size() == 2 && ends[0].id == held && ends[0].length == 20u && ends[0].result == TransferResult::Completed);
    CHECK(ends.size() == 2 && ends[1].id == response && ends[1].length == 30u);
    // A respond() refused (the node changed its address since the request) gives no id.
    CHECK(request.has_value());
    bench.inject(frame(6, kPgnAddressClaim, 0x90, kGlobal, Bytes{1, 0, 0, 0, 0, 0, 0, 0}));
    bench.steps(3);
    CHECK(us.address_state() == AddressState::CannotClaim);
    bench.app = [&](SimTime) {
        if (request)
        {
            CHECK(us.respond(*request, payload(30)) == SendStatus::NoAddress);
            CHECK(!us.last_transfer_id());
            request.reset();
        }
    };
    bench.step();
    CHECK(!request);
}

// --- Network::address_of / name_at: local nodes and the address table

TEST_CASE(address_of_and_name_at_see_local_nodes_and_the_table)
{
    constexpr std::uint64_t kOurName = 0x0000'1000'0000'0002u;
    constexpr std::uint64_t kTheirs = 0x0000'2000'0000'0003u;
    const auto claim = [](Address sa, std::uint64_t name) {
        Bytes data(8);
        for (int i = 0; i < 8; ++i)
        {
            data[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(name >> (8 * i));
        }
        return frame(6, kPgnAddressClaim, sa, kGlobal, data);
    };
    Bench bench;
    Node& us = bench.network.add_node({.address = kUs, .name = Name{kOurName}});
    Node& fixed = bench.add_node(kOther); // no NAME
    static_cast<void>(fixed);
    const Network& net = bench.network;
    CHECK(!net.address_of(Name{kOurName})); // not started
    bench.step();
    CHECK(us.address_state() == AddressState::Claiming);
    CHECK(net.address_of(Name{kOurName}) == std::optional<Address>{kUs});
    CHECK(net.name_at(kUs) == std::optional<Name>{Name{kOurName}});
    CHECK(!net.name_at(kOther)); // a node without a NAME
    CHECK(!net.address_table().address_of(Name{kOurName})); // the table has only the others

    bench.inject(claim(0x30, kTheirs));
    bench.step();
    CHECK(net.address_of(Name{kTheirs}) == std::optional<Address>{0x30});
    CHECK(net.name_at(0x30) == std::optional<Name>{Name{kTheirs}});
    bench.inject(claim(0x31, kTheirs)); // moves
    bench.step();
    CHECK(net.address_of(Name{kTheirs}) == std::optional<Address>{0x31});
    CHECK(!net.name_at(0x30));
    bench.inject(claim(kNull, kTheirs)); // Cannot Claim
    bench.step();
    CHECK(!net.address_of(Name{kTheirs}));
    CHECK(!net.name_at(0x31));
    // Our node loses its address to a lower NAME: the winner is at kUs, our NAME nowhere.
    bench.inject(claim(kUs, kOurName - 1));
    bench.step();
    CHECK(us.address_state() == AddressState::CannotClaim || us.address() != kUs);
    CHECK(net.name_at(kUs) == std::optional<Name>{Name{kOurName - 1}});
    CHECK(us.address_state() != AddressState::CannotClaim || !net.address_of(Name{kOurName}));
}

// --- Node::on_incoming_transfer: the transfers a node receives

namespace {

struct Incoming
{
    vehicle_sil::j1939::TransferStage stage;
    std::uint64_t id;
    std::uint32_t pgn;
    Address source;
    Address destination;
    std::size_t length;
    vehicle_sil::j1939::TransferResult result;
    long long ms;
};

struct IncomingLog
{
    explicit IncomingLog(Node& node)
      : sub(node, node.on_incoming_transfer([this](const vehicle_sil::j1939::IncomingTransfer& t) {
            got.push_back(Incoming{t.stage, t.id.value, t.pgn.value(), t.source, t.destination, t.length, t.result, ms_of(t.time)});
        }))
    {
    }
    std::vector<Incoming> got;
    Subscription sub;
};

using vehicle_sil::j1939::TransferResult;
using vehicle_sil::j1939::TransferStage;

} // namespace

TEST_CASE(incoming_broadcasts_are_reported_started_and_ended_to_every_node)
{
    Bench bench;
    Node& a = bench.add_node(kUs);
    Node& b = bench.add_node(kOther);
    IncomingLog logA(a), logB(b);
    std::vector<long long> messages;
    Subscription m{a, a.on_message(Pgn{kPgnPropB}, [&](const Message& msg) { messages.push_back(ms_of(msg.time)); })};
    const Bytes data = payload(20, 4);
    bench.inject(at(5ms), bam(kTool, 20, kPgnPropB));
    bench.inject(at(15ms), dt(kTool, kGlobal, 1, data));
    bench.inject(at(25ms), dt(kTool, kGlobal, 2, data));
    bench.inject(at(35ms), dt(kTool, kGlobal, 3, data));
    bench.steps(5);
    CHECK_EQ(logA.got.size(), 2u);
    CHECK(logA.got.size() == 2 && logA.got[0].stage == TransferStage::Started && logA.got[0].ms == 5 &&
          logA.got[0].source == kTool && logA.got[0].destination == kGlobal && logA.got[0].length == 20u &&
          logA.got[0].pgn == kPgnPropB && logA.got[0].id != 0);
    CHECK(logA.got.size() == 2 && logA.got[1].stage == TransferStage::Ended && logA.got[1].id == logA.got[0].id &&
          logA.got[1].result == TransferResult::Completed && logA.got[1].ms == 35);
    CHECK(logB.got.size() == 2 && logB.got[0].id == logA.got[0].id);
    CHECK(messages == std::vector<long long>{35});
    // A missing packet (Lost), an announcement that times out (Lost), one replaced (Cancelled).
    bench.inject(at(55ms), bam(kTool, 20, kPgnPropB));
    bench.inject(at(65ms), dt(kTool, kGlobal, 2, data));
    bench.inject(at(75ms), bam(0x91, 20, kPgnPropB));
    bench.inject(at(85ms), bam(0x91, 30, kPgnPropB)); // the same sender again
    bench.steps(100);
    CHECK_EQ(logA.got.size(), 8u);
    CHECK(logA.got.size() == 8 && logA.got[7].result == TransferResult::Lost && logA.got[7].ms == 85 + 750); // T1
}

TEST_CASE(incoming_connections_are_reported_to_their_destination_only)
{
    Bench bench;
    Node& a = bench.add_node(kUs);
    Node& b = bench.add_node(kOther);
    IncomingLog logA(a), logB(b);
    const Bytes data = payload(20, 4);
    // Completed.
    bench.inject(at(5ms), rts(kTool, kUs, 20, kPgnPropA));
    bench.steps(2); // CTS at 10
    bench.inject(at(15ms), dt(kTool, kUs, 1, data));
    bench.inject(at(15ms), dt(kTool, kUs, 2, data));
    bench.inject(at(15ms), dt(kTool, kUs, 3, data));
    bench.steps(2);
    CHECK(logB.got.empty());
    CHECK(logA.got.size() == 2 && logA.got[0].destination == kUs && logA.got[1].result == TransferResult::Completed);
    // Aborted by the originator, and by this node after a timeout (T2: no packet after the CTS).
    bench.inject(at(35ms), rts(kTool, kUs, 20, kPgnPropA));
    bench.steps(2);
    bench.inject(at(55ms), conn_abort(kTool, kUs, 1, kPgnPropA));
    bench.inject(at(55ms), rts(0x91, kUs, 20, kPgnPropA));
    bench.steps(200);
    CHECK(logA.got.size() == 6 && logA.got[3].result == TransferResult::AbortedByPeer && logA.got[3].ms == 55);
    CHECK(logA.got.size() == 6 && logA.got[5].result == TransferResult::Aborted && logA.got[5].source == 0x91);
    CHECK(logB.got.empty());
}

TEST_CASE(a_local_broadcast_is_not_reported_to_its_sender_and_handlers_may_go_during_a_report)
{
    Bench bench;
    Node& a = bench.add_node(kUs);
    Node& b = bench.add_node(kOther);
    IncomingLog logA(a), logB(b);
    int calls = 0;
    std::optional<Subscription> once;
    once.emplace(b, b.on_incoming_transfer([&](const vehicle_sil::j1939::IncomingTransfer&) {
        ++calls;
        once.reset(); // removed in its own call: not called again
    }));
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(a.send(Pgn{kPgnPropB}, payload(20)) == SendStatus::Ok);
        }
    };
    bench.steps(30);
    CHECK(logA.got.empty());
    CHECK(logB.got.size() == 2 && logB.got[0].source == kUs && logB.got[1].result == TransferResult::Completed);
    CHECK_EQ(calls, 1);
}

TEST_CASE(a_node_added_during_a_broadcast_is_not_told_its_end)
{
    Bench bench;
    Node& a = bench.add_node(kUs);
    IncomingLog logA(a);
    const Bytes data = payload(20, 4);
    bench.inject(at(5ms), bam(kTool, 20, kPgnPropB));
    bench.steps(2); // started
    Node& late = bench.add_node(kOther);
    IncomingLog logLate(late);
    bench.inject(at(25ms), dt(kTool, kGlobal, 1, data));
    bench.inject(at(25ms), dt(kTool, kGlobal, 2, data));
    bench.inject(at(25ms), dt(kTool, kGlobal, 3, data));
    bench.steps(3);
    CHECK(logA.got.size() == 2 && logA.got[1].result == TransferResult::Completed);
    CHECK(logLate.got.empty()); // no end without its start
}
