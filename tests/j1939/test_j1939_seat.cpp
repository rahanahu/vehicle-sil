// SPDX-License-Identifier: MIT
//
// The listener's seat of the J1939 engine (detail::FrameSeat, internal): what it hears (frames,
// messages, the end of the step's receive work), where its calls come relative to the observer,
// the handlers and the timers, that it is read only, that it changes nothing, how it is taken
// and left.
#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"
#include "network_test_access.hpp"

#include "frame_seat.hpp"

#include "vehicle_sil/j1939/observer.hpp"
#include "vehicle_sil/j1939/toolbox.hpp"
#include "vehicle_sil/testkit/recorders.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::to_ns;
using vehicle_sil::unit::throws;
using vehicle_sil::j1939::Event;
using vehicle_sil::j1939::EventKind;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Network;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::NodeOptions;
using vehicle_sil::j1939::Observer;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::Subscription;
using vehicle_sil::j1939::Timer;
using vehicle_sil::j1939::TimerKind;
using vehicle_sil::j1939::detail::FrameSeat;
using vehicle_sil::j1939::detail::NetworkTestAccess;
namespace can = vehicle_sil::can;

namespace {

constexpr Address kA = 0x10;
constexpr Address kB = 0x20;
constexpr Address kRemote = 0x30;
constexpr Address kUs = 0x80; ///< in 128..247: waits 250 ms after the claim
constexpr std::uint64_t kOurName = 0x20008200FE001234u;
constexpr std::uint64_t kLowerName = 0x20008200FE001000u; ///< wins against kOurName

const std::string kReadOnly = " called from a Listener handler (handlers only read: update variables there and send from on_step)";

void check_text(const std::string& actual, const std::string& expected, const char* file, int line)
{
    if (actual != expected)
    {
        throw vehicle_sil::unit::Failure(std::string(file) + ":" + std::to_string(line) + ": texts differ\n  actual:   " + actual +
                                         "\n  expected: " + expected);
    }
}

#define CHECK_TEXT(actual, expected) check_text((actual), (expected), __FILE__, __LINE__)

enum class Where
{
    Frame,
    Message,
    StepEnd,
};

/// Keeps what the seat is told, and writes one line per call to `log` (shared with others).
struct TestSeat final : FrameSeat
{
    struct HeardFrame
    {
        SimTime time;
        J1939Frame frame;
        bool local;
    };
    struct HeardMessage
    {
        Seen message;
        bool local;
    };

    void on_frame(SimTime time, const can::Frame& frame, bool local) override
    {
        frames.push_back({time, from_can(frame), local});
        note("seat frame " + to_string(frames.back().frame) + " " + std::to_string(to_ns(time)) + (local ? " local" : ""));
        if (during)
        {
            during(Where::Frame);
        }
    }
    void on_message(const Message& message, bool local) override
    {
        messages.push_back({seen_of(message), local});
        note("seat message " + std::to_string(message.pgn.value()) + " " + std::to_string(message.data.size()) + " " +
             std::to_string(to_ns(message.time)) + (local ? " local" : ""));
        if (during)
        {
            during(Where::Message);
        }
    }
    void on_step_end(SimTime now) override
    {
        step_ends.push_back(now);
        note("seat step end " + std::to_string(to_ns(now)));
        if (during)
        {
            during(Where::StepEnd);
        }
    }
    void note(std::string line) const
    {
        if (log != nullptr)
        {
            log->push_back(std::move(line));
        }
    }
    [[nodiscard]] std::size_t frames_of(std::uint32_t pgn) const
    {
        std::size_t n = 0;
        for (const auto& f : frames)
        {
            n += f.frame.pgn == pgn ? 1u : 0u;
        }
        return n;
    }

    std::vector<HeardFrame> frames;
    std::vector<HeardMessage> messages;
    std::vector<SimTime> step_ends;
    std::vector<std::string>* log = nullptr;
    std::function<void(Where)> during;
};

/// Writes one line per event to `log`.
struct LoggingObserver final : Observer
{
    void on_event(const Event& e) override
    {
        log->push_back("observer " + std::to_string(static_cast<int>(e.kind)) + " " + std::to_string(to_ns(e.time)) + " " +
                       std::to_string(e.node) + " " + std::to_string(e.pgn.value()) + " " + std::to_string(e.size) + " " +
                       std::to_string(e.source) + ">" + std::to_string(e.destination) + (e.local ? " local" : ""));
    }
    std::vector<std::string>* log = nullptr;
};

Bytes name_bytes(std::uint64_t name)
{
    Bytes out;
    for (int i = 0; i < 8; ++i)
    {
        out.push_back(static_cast<std::uint8_t>(name >> (8 * i)));
    }
    return out;
}

Node& add_claiming(Bench& bench, Address address, std::uint64_t name)
{
    NodeOptions options;
    options.address = address;
    options.name = Name{name};
    return bench.network.add_node(options);
}

} // namespace

TEST_CASE(seat_hears_the_frames_the_messages_and_the_step_end)
{
    Bench bench;
    TestSeat seat;
    NetworkTestAccess::set_seat(bench.network, &seat);
    CHECK(NetworkTestAccess::seat(bench.network) == &seat);
    Node& a = bench.add_node(kA);
    bench.add_node(kB);
    bench.step(); // 0
    CHECK(seat.frames.empty());
    CHECK(seat.messages.empty());

    const Bytes big = payload(20);
    bench.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
    bench.inject(frame(6, kPgnPropA, kRemote, 0x55, {2})); // to an address that is not here
    bench.inject(bam(kRemote, big.size(), kPgnPropB));
    // What the network does not take does not reach the seat: an 11-bit frame, the source 255.
    can::Frame standard;
    standard.id = 0x123;
    standard.format = can::IdFormat::Standard;
    standard.length = 1;
    bench.tester.send(can::TimedFrame{bench.last_time(), standard});
    bench.inject(frame(6, kPgnPropB, 0xFF, kGlobal, {3}));
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            const Bytes nine{9};
            CHECK(a.send(Pgn{kPgnPropB}, nine) == SendStatus::Ok);
        }
    };
    bench.step(); // 10 ms
    CHECK_EQ(bench.network.statistics().frames_ignored, 2u);
    CHECK_EQ(seat.frames.size(), 3u);
    // In the order the network handles them, with the frame's time, before the message.
    CHECK(seat.frames[0].frame == frame(6, kPgnPropB, kRemote, kGlobal, {1}));
    CHECK(seat.frames[1].frame == frame(6, kPgnPropA, kRemote, 0x55, {2}));
    CHECK_EQ(seat.frames[2].frame.pgn, kPgnTpCm);
    for (const auto& f : seat.frames)
    {
        CHECK(f.time == at(0ms));
        CHECK(!f.local);
    }
    // The transport frame is no message; the frame to another address is one (as for the observer).
    CHECK_EQ(seat.messages.size(), 2u);
    CHECK(seat.messages[0].message == (Seen{kPgnPropB, kRemote, kGlobal, 6, at(0ms), {1}}));
    CHECK(seat.messages[1].message == (Seen{kPgnPropA, kRemote, 0x55, 6, at(0ms), {2}}));
    CHECK(!seat.messages[0].local);

    for (int k = 1; k <= 3; ++k)
    {
        bench.inject(at(10ms) + k * 60ms, dt(kRemote, kGlobal, static_cast<std::uint8_t>(k), big));
    }
    while (bench.last_time() < at(250ms))
    {
        bench.step();
    }
    CHECK_EQ(seat.frames_of(kPgnTpDt), 3u);
    // The node's own message came back as a local frame and a local message (two nodes: loopback).
    CHECK_EQ(seat.frames.size(), 7u);
    CHECK(seat.frames[3].frame == frame(6, kPgnPropB, kA, kGlobal, {9}));
    CHECK(seat.frames[3].local);
    CHECK_EQ(seat.messages.size(), 4u);
    CHECK(seat.messages[2].message == (Seen{kPgnPropB, kA, kGlobal, 6, at(10ms), {9}}));
    CHECK(seat.messages[2].local);
    // The broadcast the engine put together, with the time of its last packet.
    CHECK_EQ(seat.messages[3].message.pgn, kPgnPropB);
    CHECK_EQ(seat.messages[3].message.sa, kRemote);
    CHECK(seat.messages[3].message.time == at(190ms));
    CHECK(seat.messages[3].message.data == big);
    CHECK(!seat.messages[3].local);
}

TEST_CASE(seat_step_end_is_told_once_per_step_with_the_step_time)
{
    Bench bench({}, 5ms);
    TestSeat seat;
    std::vector<std::string> log;
    seat.log = &log;
    NetworkTestAccess::set_seat(bench.network, &seat);
    bench.add_node(kA);
    std::vector<SimTime> expected;
    for (int i = 0; i < 12; ++i)
    {
        if (i % 3 == 1) // some steps with frames, most without
        {
            bench.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
            bench.inject(frame(6, kPgnPropB, kRemote, kGlobal, {2}));
        }
        const std::size_t before = log.size();
        bench.step();
        expected.push_back(bench.last_time());
        // After everything the step received.
        CHECK(log.size() > before);
        CHECK_TEXT(log.back(), "seat step end " + std::to_string(to_ns(bench.last_time())));
    }
    CHECK(seat.step_ends == expected);
    CHECK_EQ(seat.frames.size(), 8u);
    // A network without a node has a seat too.
    Bench empty;
    TestSeat other;
    NetworkTestAccess::set_seat(empty.network, &other);
    empty.step();
    empty.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
    empty.step();
    CHECK_EQ(other.frames.size(), 1u);
    CHECK_EQ(other.messages.size(), 1u);
    CHECK_EQ(other.step_ends.size(), 2u);
}

TEST_CASE(seat_is_told_after_the_observer_and_before_the_handlers)
{
    Bench bench;
    std::vector<std::string> log;
    TestSeat seat;
    seat.log = &log;
    LoggingObserver observer;
    observer.log = &log;
    // In either order of attaching.
    bench.network.set_observer(&observer);
    NetworkTestAccess::set_seat(bench.network, &seat);
    Node& a = bench.add_node(kA);
    a.on_any_message([&](const Message& m) { log.push_back("handler " + std::to_string(m.pgn.value())); });
    bench.step();
    log.clear();
    bench.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
    bench.step();
    const std::string received = std::to_string(static_cast<int>(EventKind::MessageReceived));
    CHECK_EQ(log.size(), 5u);
    CHECK_TEXT(log.at(0), "seat frame " + to_string(frame(6, kPgnPropB, kRemote, kGlobal, {1})) + " 0");
    CHECK_TEXT(log.at(1), "observer " + received + " 0 -1 " + std::to_string(kPgnPropB) + " 1 48>255");
    CHECK_TEXT(log.at(2), "seat message " + std::to_string(kPgnPropB) + " 1 0");
    CHECK_TEXT(log.at(3), "handler " + std::to_string(kPgnPropB));
    CHECK_TEXT(log.at(4), "seat step end 10000000");
}

TEST_CASE(seat_step_end_comes_before_the_timers_due_at_the_step_time)
{
    Bench bench;
    std::vector<std::string> log;
    TestSeat seat;
    seat.log = &log;
    NetworkTestAccess::set_seat(bench.network, &seat);
    Node& a = bench.add_node(kA);
    a.on_any_message([&](const Message&) { log.push_back("handler"); });
    Timer early(bench.network, TimerKind::Scheduled, [&] { log.push_back("timer 15 ms"); });
    Timer due(bench.network, TimerKind::Scheduled, [&] { log.push_back("timer 20 ms"); });
    Timer between(bench.network, TimerKind::Scheduled, [&] { log.push_back("timer 18 ms"); });
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            early.start_at(at(15ms));
            between.start_at(at(18ms));
            due.start_at(at(20ms));
        }
    };
    bench.steps(2); // 0, 10 ms
    bench.inject(at(17ms), frame(6, kPgnPropB, kRemote, kGlobal, {1}));
    log.clear();
    bench.step(); // 20 ms
    // Timers before a frame's time fire before the frame, as without a seat. The step end is
    // told when the frames are done and before every timer that is left: also one that was due
    // before the step time.
    CHECK_EQ(log.size(), 7u);
    CHECK_TEXT(log.at(0), "timer 15 ms");
    CHECK_TEXT(log.at(1), "seat frame " + to_string(frame(6, kPgnPropB, kRemote, kGlobal, {1})) + " 17000000");
    CHECK_TEXT(log.at(2), "seat message " + std::to_string(kPgnPropB) + " 1 17000000");
    CHECK_TEXT(log.at(3), "handler");
    CHECK_TEXT(log.at(4), "seat step end 20000000");
    CHECK_TEXT(log.at(5), "timer 18 ms");
    CHECK_TEXT(log.at(6), "timer 20 ms");
}

TEST_CASE(seat_is_read_only)
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
        SetSeat,
        SetObserver,
        Attach,
        Throw,
    };
    struct Part
    {
        explicit Part(Node&) {}
    };
    for (const Where where : {Where::Frame, Where::Message, Where::StepEnd})
    {
        for (const Op op : {Op::Send, Op::Request, Op::Register, Op::Unsubscribe, Op::StartTimer, Op::CancelTimer, Op::AddNode,
                            Op::SetSeat, Op::SetObserver, Op::Attach, Op::Throw})
        {
            Bench bench;
            TestSeat seat;
            Node& a = bench.add_node(kA);
            std::size_t handled = 0;
            Subscription subscription(a, a.on_message(Pgn{kPgnPropB}, [&](const Message&) { ++handled; }));
            Timer timer(bench.network, TimerKind::Scheduled, [] {});
            NetworkTestAccess::set_seat(bench.network, &seat);
            bench.step();
            bool once = false;
            bool threw_logic_error = false;
            const auto call = [&] {
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
                case Op::SetSeat:
                    NetworkTestAccess::set_seat(bench.network, nullptr);
                    break;
                case Op::SetObserver:
                    bench.network.set_observer(nullptr);
                    break;
                case Op::Attach:
                    static_cast<void>(a.attach<Part>());
                    break;
                case Op::Throw:
                    throw std::runtime_error("seat failed");
                }
            };
            seat.during = [&](Where now) {
                if (now != where || std::exchange(once, true))
                {
                    return;
                }
                try
                {
                    call();
                }
                catch (const std::logic_error&)
                {
                    threw_logic_error = true;
                    throw;
                }
            };
            bench.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
            CHECK(throws<std::exception>([&] { bench.steps(3); }));
            CHECK(once);
            CHECK(bench.network.failed());
            const std::string& error = bench.network.error();
            if (op == Op::Throw)
            {
                CHECK_TEXT(error, std::string("vehicle_sil::j1939::Network: a Listener handler threw: seat failed"));
            }
            else
            {
                // Unsubscribing and cancelling cannot throw: they fail the network only.
                CHECK_EQ(threw_logic_error, op != Op::Unsubscribe && op != Op::CancelTimer);
                CHECK(error.size() > kReadOnly.size());
                CHECK_TEXT(error.substr(error.size() - kReadOnly.size()), kReadOnly);
                CHECK(error.find("from an observer") == std::string::npos);
            }
            if (op == Op::Send)
            {
                CHECK_TEXT(error, "vehicle_sil::j1939::Network: send()" + kReadOnly);
            }
            // The seat stays where it was, and a failure in the frame's call stops the frame.
            CHECK(NetworkTestAccess::seat(bench.network) == &seat);
            if (where == Where::Frame && op != Op::Unsubscribe && op != Op::CancelTimer)
            {
                CHECK_EQ(handled, 0u);
            }
            CHECK(bench.take_sent().empty());
        }
    }
}

TEST_CASE(seat_exception_in_the_application_stage_fails_the_network)
{
    // A message delivered in the application stage reaches the seat too.
    Bench bench;
    TestSeat seat;
    NetworkTestAccess::set_seat(bench.network, &seat);
    bench.add_node(kA);
    const Bytes one{1};
    seat.during = [](Where where) {
        if (where == Where::Message)
        {
            throw std::runtime_error("seat failed");
        }
    };
    bool threw = false;
    bench.app = [&](SimTime now) {
        threw = throws<std::runtime_error>(
          [&] { NetworkTestAccess::deliver_message(bench.network, Message{Pgn{kPgnPropB}, 6, kRemote, kGlobal, now, one}); });
    };
    try
    {
        bench.step();
    }
    catch (const std::exception&) // the host may notice the failed network
    {
    }
    CHECK(threw);
    CHECK_EQ(seat.messages.size(), 1u);
    CHECK(bench.network.failed());
    CHECK_TEXT(bench.network.error(), std::string("vehicle_sil::j1939::Network: a Listener handler threw: seat failed"));
}

TEST_CASE(seat_is_taken_outside_a_step_only_and_by_one)
{
    Bench bench;
    TestSeat seat;
    bench.add_node(kA);
    std::string in_step;
    bench.app = [&](SimTime) {
        try
        {
            NetworkTestAccess::set_seat(bench.network, &seat);
        }
        catch (const std::logic_error& e)
        {
            in_step = e.what();
        }
    };
    bench.step();
    CHECK_TEXT(in_step, std::string("vehicle_sil::j1939::Network: a Listener is only added or removed outside a step (create it "
                                  "before the simulation starts, next to the nodes)"));
    CHECK(NetworkTestAccess::seat(bench.network) == nullptr);
    CHECK(!bench.network.failed());
    bench.app = {};

    NetworkTestAccess::set_seat(bench.network, &seat); // between steps
    NetworkTestAccess::set_seat(bench.network, &seat); // the same one again: nothing
    TestSeat second;
    std::string taken;
    try
    {
        NetworkTestAccess::set_seat(bench.network, &second);
    }
    catch (const std::logic_error& e)
    {
        taken = e.what();
    }
    CHECK_TEXT(taken, std::string("vehicle_sil::j1939::Network: this Network already has a Listener (a Network has one); add the "
                                "handlers to that Listener"));
    CHECK(NetworkTestAccess::seat(bench.network) == &seat);
    Bench other;
    std::string elsewhere;
    try
    {
        NetworkTestAccess::set_seat(other.network, &seat);
    }
    catch (const std::logic_error& e)
    {
        elsewhere = e.what();
    }
    CHECK_TEXT(elsewhere, std::string("vehicle_sil::j1939::Network: the Listener already listens to another Network; create one "
                                    "Listener per Network"));
    CHECK(NetworkTestAccess::seat(other.network) == nullptr);
    CHECK(!bench.network.failed());
    CHECK(!other.network.failed());

    bench.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
    bench.step();
    CHECK_EQ(seat.frames.size(), 1u);
    bench.app = [&](SimTime) { CHECK(throws<std::logic_error>([&] { NetworkTestAccess::set_seat(bench.network, nullptr); })); };
    bench.step();
    bench.app = {};
    CHECK(NetworkTestAccess::seat(bench.network) == &seat);
    NetworkTestAccess::set_seat(bench.network, nullptr);
    CHECK(NetworkTestAccess::seat(bench.network) == nullptr);
    bench.inject(frame(6, kPgnPropB, kRemote, kGlobal, {2}));
    bench.step();
    CHECK_EQ(seat.frames.size(), 1u);
    CHECK_EQ(seat.step_ends.size(), 2u);
    // Left, it can sit in the other network, and the second one here.
    NetworkTestAccess::set_seat(other.network, &seat);
    NetworkTestAccess::set_seat(bench.network, &second);
    CHECK(NetworkTestAccess::seat(other.network) == &seat);
    CHECK(NetworkTestAccess::seat(bench.network) == &second);
}

TEST_CASE(seat_and_network_can_be_destroyed_in_either_order)
{
    {
        auto seat = std::make_unique<TestSeat>();
        Bench bench;
        NetworkTestAccess::set_seat(bench.network, seat.get());
        seat.reset(); // leaves the seat
        CHECK(NetworkTestAccess::seat(bench.network) == nullptr);
        bench.add_node(kA);
        bench.step();
        bench.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
        bench.steps(2);
        CHECK(!bench.network.failed());
    }
    {
        TestSeat seat;
        {
            auto bench = std::make_unique<Bench>();
            NetworkTestAccess::set_seat(bench->network, &seat);
            bench->add_node(kA);
            bench->steps(1);
        } // the network goes first
        CHECK_EQ(seat.step_ends.size(), 1u);
        Bench other;
        NetworkTestAccess::set_seat(other.network, &seat);
        other.add_node(kA);
        other.step();
        other.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
        other.step();
        CHECK_EQ(seat.messages.size(), 1u);
    }
    {
        // A failed network lets the seat go.
        Bench bench;
        TestSeat seat;
        NetworkTestAccess::set_seat(bench.network, &seat);
        bench.add_node(kA);
        seat.during = [&](Where) { throw std::runtime_error("seat failed"); };
        CHECK(throws<std::runtime_error>([&] { bench.step(); }));
        CHECK(bench.network.failed());
        NetworkTestAccess::set_seat(bench.network, nullptr);
        CHECK(NetworkTestAccess::seat(bench.network) == nullptr);
    }
}

namespace {

struct Run
{
    std::vector<std::string> trace; ///< The frames sent, what the handlers saw, the observer's events.
    std::size_t steps = 0;
};

/// A run with claims, a held message, a request and its answer, a broadcast sent and one
/// received, a connection received, local loopback and an address lost, always with an observer:
/// the frames sent with their times, what the handlers saw and the observer's events, as text.
Run scenario(FrameSeat* seat, SimDuration step)
{
    Bench bench({}, step);
    Run run;
    LoggingObserver observer;
    observer.log = &run.trace;
    bench.network.set_observer(&observer);
    if (seat != nullptr)
    {
        NetworkTestAccess::set_seat(bench.network, seat);
    }
    Node& us = add_claiming(bench, kUs, kOurName);
    Node& a = bench.add_node(kA);
    a.on_request(Pgn{0xFEDA}, [](vehicle_sil::j1939::RequestContext& r) { static_cast<void>(r.respond(payload(30))); });
    for (Node* n : {&us, &a})
    {
        n->on_any_message([&run, n](const Message& m) {
            run.trace.push_back("rx " + std::to_string(n->address()) + " " + std::to_string(m.pgn.value()) + " " +
                                std::to_string(to_ns(m.time)) + " " + std::to_string(m.data.size()));
        });
        n->on_transfer_end([&run](const vehicle_sil::j1939::TransferEnd& e) {
            run.trace.push_back("end " + std::to_string(static_cast<int>(e.result)) + " " + std::to_string(to_ns(e.time)));
        });
    }
    Timer timer(bench.network, TimerKind::Scheduled, [&] {
        run.trace.push_back("timer " + std::to_string(to_ns(bench.network.event_time())));
        timer.start_after(70ms);
    });
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            static_cast<void>(us.send(Pgn{kPgnPropB}, payload(12)));
            static_cast<void>(a.send(Pgn{kPgnPropB}, payload(3)));
            timer.start_at(at(35ms));
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
    script.emplace_back(at(900ms), frame(6, kPgnAddressClaim, kUs, kGlobal, name_bytes(kLowerName)));
    std::size_t next = 0;
    while (bench.next_time() < at(3s))
    {
        while (next < script.size() && script[next].first <= bench.last_time())
        {
            bench.inject(script[next].first < bench.last_time() ? bench.last_time() : script[next].first, script[next].second);
            ++next;
        }
        bench.step();
        ++run.steps;
        for (const auto& f : bench.take_sent())
        {
            run.trace.push_back("tx " + to_string(f) + " " + std::to_string(to_ns(bench.sent_times.back())));
        }
    }
    CHECK(!bench.network.failed());
    return run;
}

} // namespace

TEST_CASE(seat_does_not_change_the_trace_or_the_observers_events)
{
    for (const SimDuration step : {SimDuration{1ms}, SimDuration{10ms}})
    {
        TestSeat seat;
        const Run with = scenario(&seat, step);
        const Run without = scenario(nullptr, step);
        CHECK(with.trace == without.trace);
        CHECK(with.trace.size() > 40u);
        // The seat did hear the run: frames from outside and looped back, transport frames,
        // single frame messages and transfers, every step.
        CHECK(seat.frames_of(kPgnTpCm) >= 3u);
        CHECK(seat.frames_of(kPgnTpDt) >= 3u);
        std::size_t local_frames = 0;
        for (const auto& f : seat.frames)
        {
            local_frames += f.local ? 1u : 0u;
        }
        CHECK(local_frames > 0u);
        CHECK(local_frames < seat.frames.size());
        std::size_t long_messages = 0;
        for (const auto& m : seat.messages)
        {
            long_messages += m.message.data.size() > 8 ? 1u : 0u;
        }
        CHECK(long_messages >= 2u);
        CHECK_EQ(seat.step_ends.size(), with.steps);
        CHECK_EQ(with.steps, without.steps);
    }
}

TEST_CASE(seat_and_the_test_kits_recorder_hear_the_same_messages)
{
    // The seat is not the observer's place: a recorder (an Observer) and the seat are on one
    // network at the same time, and get the same messages.
    Bench bench;
    vehicle_sil::testkit::J1939Events events(bench.network);
    TestSeat seat;
    NetworkTestAccess::set_seat(bench.network, &seat);
    CHECK(bench.network.observer() == &events);
    Node& a = bench.add_node(kA);
    bench.add_node(kB);
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            CHECK(a.send(Pgn{kPgnPropB}, payload(12)) == SendStatus::Ok); // a broadcast, looped back to the other node
        }
    };
    bench.step();
    bench.inject(frame(6, kPgnPropB, kRemote, kGlobal, {1}));
    bench.inject(frame(3, kPgnPropA, kRemote, kA, {2, 3}));
    while (bench.last_time() < at(400ms))
    {
        bench.step();
    }
    const auto received = events.of(EventKind::MessageReceived);
    CHECK_EQ(received.size(), 3u);
    CHECK_EQ(seat.messages.size(), received.size());
    for (std::size_t i = 0; i < received.size() && i < seat.messages.size(); ++i)
    {
        const auto& e = received[i].event;
        const auto& m = seat.messages[i];
        CHECK(m.message == (Seen{e.pgn.value(), e.source, e.destination, e.priority, e.time, received[i].data}));
        CHECK_EQ(m.local, e.local);
    }
    CHECK(seat.messages[2].local);
    CHECK_EQ(seat.messages[2].message.data.size(), 12u);
    CHECK_EQ(events.dropped(), 0u);
}
