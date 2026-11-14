// SPDX-License-Identifier: MIT
//
// TransferSniffer beside the engine: the same frames go to a receiving node of the engine and
// to the sniffer, and what the sniffer reports is what the node got (messages with their
// times and bytes, and how each transfer ended).
// - Transfers between two engines: retransmissions, timeouts of either side.
// - Seeded fault injection: a talker written here plays a sender that loses, repeats, delays
//   and reorders packets, stops, aborts and announces again, to a receiving node of the engine.
//   A change of the engine's receiver that the sniffer does not follow fails here.
#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/j1939/network.hpp"
#include "vehicle_sil/j1939/transfer_sniffer.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::can::FaultAction;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::j1939::AbortReason;
using vehicle_sil::j1939::IncomingTransfer;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Network;
using vehicle_sil::j1939::NetworkOptions;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::SniffedTransfer;
using vehicle_sil::j1939::SniffEnd;
using vehicle_sil::j1939::SnifferOptions;
using vehicle_sil::j1939::TransferResult;
using vehicle_sil::j1939::TransferSniffer;
using vehicle_sil::j1939::TransferStage;

namespace {

constexpr Address kA = 0x10; // sends connections
constexpr Address kB = 0x20; // the receiving node of the engine
constexpr Address kC = 0x30; // sends broadcasts
constexpr std::uint32_t kPgn = 0xEF00;
constexpr std::uint32_t kOther = 0xD300;

NetworkOptions quiet()
{
    NetworkOptions options;
    options.warning = [](std::string_view) {};
    return options;
}

[[nodiscard]] bool is_dt(const TimedFrame& f, int sequence = -1)
{
    return ((f.frame.id >> 8) & 0x3FF00u) == kPgnTpDt && (sequence < 0 || f.frame.data[0] == sequence);
}

[[nodiscard]] bool is_cm(const TimedFrame& f, std::uint8_t code)
{
    return ((f.frame.id >> 8) & 0x3FF00u) == kPgnTpCm && f.frame.data[0] == code;
}

/// How a transfer ended for the receiving node of the engine.
struct EngineEnd
{
    std::uint32_t pgn;
    Address sa;
    Address da;
    std::size_t size;
    TransferResult result;
    AbortReason reason;
    SimTime time;
};

/// What the receiving node got: its messages and the ends of the transfers to it.
struct Receiver
{
    void attach(Node& node)
    {
        node.on_any_message([this](const Message& m) { messages.push_back(seen_of(m)); });
        node.on_incoming_transfer([this](const IncomingTransfer& t) {
            if (t.stage == TransferStage::Ended)
            {
                ends.push_back(EngineEnd{t.pgn.value(), t.source, t.destination, t.length, t.result, t.reason, t.time});
            }
        });
    }
    std::vector<Seen> messages;
    std::vector<EngineEnd> ends;
};

struct Heard
{
    SniffEnd end;
    std::uint32_t pgn;
    Address sa;
    Address da;
    std::size_t size;
    SimTime time;
    std::uint8_t reason;
};

/// The sniffer with what it reported: completed transfers as messages, and every end.
struct Listener
{
    explicit Listener(SnifferOptions options = with_broadcasts()) : sniffer(options)
    {
        sniffer.on_transfer([this](const SniffedTransfer& t) {
            ends.push_back(Heard{t.end, t.pgn.value(), t.source, t.destination, t.size, t.time, t.reason});
            if (t.end == SniffEnd::Completed)
            {
                messages.push_back(seen_of(t.message()));
            }
        });
    }
    [[nodiscard]] static SnifferOptions with_broadcasts()
    {
        SnifferOptions options;
        options.broadcasts = true;
        return options;
    }

    /// Feeds the frames as a listener with steps of `step` would: advance() at every step end.
    void replay(const std::vector<TimedFrame>& frames, SimTime end, SimDuration step = 1ms)
    {
        SimTime clock{};
        for (const TimedFrame& f : frames)
        {
            while (clock + step <= f.time)
            {
                clock += step;
                sniffer.advance(clock);
            }
            sniffer.feed(f);
        }
        while (clock + step <= end)
        {
            clock += step;
            sniffer.advance(clock);
        }
    }

    TransferSniffer sniffer;
    std::vector<Seen> messages;
    std::vector<Heard> ends;
};

[[nodiscard]] std::string to_text(const Seen& s)
{
    return "PGN " + std::to_string(s.pgn) + " " + std::to_string(s.sa) + "->" + std::to_string(s.da) + " P" +
           std::to_string(s.priority) + " t=" + std::to_string(vehicle_sil::to_ns(s.time)) + " " +
           std::to_string(s.data.size()) + " bytes";
}

// --- two engines and a tap ----------------------------------------------------------------------

/// Engines A and B (one node each, own host and step) on one bus, and a tap that sees every frame.
struct Engines
{
    struct Ecu
    {
        Ecu(vehicle_sil::can::VirtualCanBus& bus, Address address) : port(bus.attach()), network(quiet())
        {
            host.add(network, port);
            node = &network.add_node({address});
            receiver.attach(*node);
        }
        vehicle_sil::can::VirtualCanBus::Endpoint& port;
        Network network;
        vehicle_sil::can::Host host;
        Node* node = nullptr;
        Receiver receiver;
    };

    Engines(SimDuration stepA, SimDuration stepB, bool withB = true)
    {
        a = std::make_unique<Ecu>(bus, kA);
        driver.add(a->host, stepA);
        if (withB)
        {
            b = std::make_unique<Ecu>(bus, kB);
            driver.add(b->host, stepB);
        }
    }

    /// A sends `message` of `pgn` to `to` at 0 ms; the run ends at `end`.
    void run(const Bytes& message, Address to = kB, SimTime end = at(5s), std::uint32_t pgn = kPgn)
    {
        a->host.set_application([this, message, to, pgn](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(a->node->send(Pgn{pgn}, message, to) == SendStatus::Ok);
            }
        });
        driver.run_until(end);
        tap.release_before(end, [this](const TimedFrame& f) { trace.push_back(f); });
    }

    vehicle_sil::can::VirtualCanBus bus;
    vehicle_sil::can::VirtualCanBus::Endpoint& tap = bus.attach();
    std::unique_ptr<Ecu> a;
    std::unique_ptr<Ecu> b;
    vehicle_sil::can::StepDriver driver;
    std::vector<TimedFrame> trace;
};

const std::vector<std::array<SimDuration, 2>> kSteps{{10ms, 10ms}, {1ms, 10ms}, {10ms, 1ms}, {3ms, 7ms}, {50ms, 20ms}};

/// The listener's step sizes tried: results do not depend on them.
const std::vector<SimDuration> kListenerSteps{1ms, 3ms, 10ms, 50ms};

vehicle_sil::can::FaultInjector drop_packet(int sequence, int times = 1)
{
    auto left = std::make_shared<int>(times);
    return [=](const TimedFrame& f) {
        if (is_dt(f, sequence) && *left > 0)
        {
            --*left;
            return FaultAction::drop();
        }
        return FaultAction::deliver();
    };
}

vehicle_sil::can::FaultInjector delay_packet_once(int sequence, SimDuration delay)
{
    auto left = std::make_shared<bool>(true);
    return [=](const TimedFrame& f) {
        if (is_dt(f, sequence) && *left)
        {
            *left = false;
            return FaultAction::delay_by(delay);
        }
        return FaultAction::deliver();
    };
}

} // namespace

TEST_CASE(sniffer_hears_the_message_the_receiving_node_gets)
{
    for (const auto& steps : kSteps)
    {
        Engines engines(steps[0], steps[1]);
        engines.run(payload(100));
        CHECK_EQ(engines.b->receiver.messages.size(), 1u);
        for (const SimDuration step : kListenerSteps)
        {
            Listener listener;
            listener.replay(engines.trace, at(5s), step);
            CHECK(listener.messages == engines.b->receiver.messages); // group, priority, addresses, time, bytes
            CHECK_EQ(listener.sniffer.statistics().started, 1u);
            CHECK_EQ(listener.sniffer.statistics().orphan_control, 0u);
            CHECK_EQ(listener.sniffer.statistics().orphan_data, 0u);
            CHECK_EQ(listener.sniffer.active(), 0u);
        }
    }
}

TEST_CASE(sniffer_hears_a_retransmitted_message_as_the_receiving_node)
{
    // A lost packet, a repeated one, a reordered one: the receiver asks again (or ignores), and
    // its message has the time of the packet it had everything with.
    const std::vector<std::function<vehicle_sil::can::FaultInjector()>> faults{
        [] { return drop_packet(3); },
        [] { return drop_packet(5, 2); },
        [] {
            return vehicle_sil::can::FaultInjector(
                [](const TimedFrame& f) { return is_dt(f) ? FaultAction::duplicate() : FaultAction::deliver(); });
        },
        [] { return delay_packet_once(2, 1ns); },
        [] { return delay_packet_once(15, 40ms); },
    };
    std::uint64_t asked = 0;
    for (const auto& fault : faults)
    {
        for (const auto& steps : kSteps)
        {
            Engines engines(steps[0], steps[1]);
            engines.a->port.set_fault_injector(fault());
            engines.run(payload(100));
            CHECK_EQ(engines.b->receiver.messages.size(), 1u);
            Listener listener;
            listener.replay(engines.trace, at(5s));
            CHECK(listener.messages == engines.b->receiver.messages);
            CHECK_EQ(listener.sniffer.statistics().retransmit_requests,
                     engines.b->network.statistics().transport_retransmit_requests);
            CHECK_EQ(listener.sniffer.statistics().conflicting_packets, 0u);
            asked += listener.sniffer.statistics().retransmit_requests;
        }
    }
    CHECK(asked > 0);
}

TEST_CASE(sniffer_hears_connections_and_broadcasts_from_raw_frames)
{
    Engines engines(10ms, 10ms);
    engines.run(payload(40), kGlobal);
    CHECK_EQ(engines.b->receiver.messages.size(), 1u);
    Listener listener;
    listener.replay(engines.trace, at(5s));
    CHECK(listener.messages == engines.b->receiver.messages);
    CHECK_EQ(listener.messages[0].da, kGlobal);
    // Without the option the broadcast is left to the network.
    Listener connectionsOnly{SnifferOptions{}};
    connectionsOnly.replay(engines.trace, at(5s));
    CHECK(connectionsOnly.messages.empty());
    CHECK_EQ(connectionsOnly.sniffer.statistics().started, 0u);
}

TEST_CASE(sniffer_does_not_report_a_message_the_receiving_node_did_not_take)
{
    // Request is single-frame only: the engine's receiver acknowledges the transfer and drops it.
    Engines engines(10ms, 10ms);
    engines.run(payload(30), kB, at(5s), kPgnRequest);
    CHECK(engines.b->receiver.messages.empty());
    Listener listener;
    listener.replay(engines.trace, at(5s));
    CHECK(listener.messages.empty());
    CHECK_EQ(listener.ends.size(), 1u);
    CHECK(listener.ends[0].end == SniffEnd::Refused);
    CHECK_EQ(engines.b->receiver.ends.size(), 1u);
    CHECK(listener.ends[0].time == engines.b->receiver.ends[0].time);
}

// --- timeouts between engines -------------------------------------------------------------------
// The engine that gives up sends its abort in the step after its timeout: later than the sniffer's
// timeout, within its grace. The transfer ends as aborted (reason 3), never as timed out, and the
// abort is not left over (orphan_control).

namespace {

void check_aborted(const Engines& engines, SniffEnd expected, std::uint64_t leftOver = 0)
{
    for (const SimDuration step : kListenerSteps)
    {
        Listener listener;
        listener.replay(engines.trace, at(5s), step);
        CHECK_EQ(listener.ends.size(), 1u);
        CHECK(listener.ends[0].end == expected);
        CHECK_EQ(listener.ends[0].reason, 3u);
        CHECK(listener.messages.empty());
        CHECK_EQ(listener.sniffer.statistics().timed_out, 0u);
        CHECK_EQ(listener.sniffer.statistics().orphan_control, leftOver);
        CHECK_EQ(listener.sniffer.active(), 0u);
    }
}

} // namespace

TEST_CASE(sniffer_hears_the_sender_give_up_when_nobody_answers_its_rts)
{
    for (const auto& steps : kSteps)
    {
        Engines engines(steps[0], steps[1], false);
        engines.run(payload(100));
        CHECK(std::any_of(engines.trace.begin(), engines.trace.end(), [](const TimedFrame& f) { return is_cm(f, 255); }));
        check_aborted(engines, SniffEnd::AbortedBySender);
    }
}

TEST_CASE(sniffer_hears_the_receiver_give_up_when_the_sender_falls_silent)
{
    int receiverGaveUp = 0;
    int senderGaveUp = 0;
    for (const auto& steps : kSteps)
    {
        Engines engines(steps[0], steps[1]);
        engines.a->port.set_fault_injector([](const TimedFrame& f) { return is_dt(f) ? FaultAction::drop() : FaultAction::deliver(); });
        engines.run(payload(100));
        // Both engines time out. Which abort is first on the bus depends on the steps, and both
        // may send one: the first ends the transfer, the second is left over.
        std::vector<Address> aborts;
        for (const TimedFrame& f : engines.trace)
        {
            if (is_cm(f, 255))
            {
                aborts.push_back(from_can(f.frame).sa);
            }
        }
        CHECK(!aborts.empty());
        const bool receiverFirst = aborts[0] == kB;
        check_aborted(engines, receiverFirst ? SniffEnd::AbortedByReceiver : SniffEnd::AbortedBySender, aborts.size() - 1);
        receiverGaveUp += receiverFirst ? 1 : 0;
        senderGaveUp += receiverFirst ? 0 : 1;
    }
    CHECK(receiverGaveUp > 0);
    static_cast<void>(senderGaveUp);
}

TEST_CASE(sniffer_hears_the_sender_give_up_when_the_acknowledgement_is_lost)
{
    // The receiving node has the message, but nobody on the bus can know: the sniffer reports
    // what the sender saw. (With SniffDelivery::WhenAllPacketsSeen it reports the message.)
    for (const auto& steps : kSteps)
    {
        Engines engines(steps[0], steps[1]);
        engines.b->port.set_fault_injector([](const TimedFrame& f) { return is_cm(f, 19) ? FaultAction::drop() : FaultAction::deliver(); });
        engines.run(payload(100));
        CHECK_EQ(engines.b->receiver.messages.size(), 1u);
        check_aborted(engines, SniffEnd::AbortedBySender);

        SnifferOptions options;
        options.delivery = vehicle_sil::j1939::SniffDelivery::WhenAllPacketsSeen;
        Listener eager(options);
        eager.replay(engines.trace, at(5s));
        CHECK(eager.messages == engines.b->receiver.messages);
    }
}

// --- seeded fault injection ---------------------------------------------------------------------

namespace {

/// A small generator with the same numbers on every platform (the distributions of <random>
/// are not).
class Random
{
public:
    explicit Random(std::uint64_t seed) : state_(seed * 0x9E3779B97F4A7C15ull + 1) {}
    [[nodiscard]] std::uint64_t next() noexcept
    {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    /// 0 .. n-1.
    [[nodiscard]] std::uint32_t below(std::uint32_t n) noexcept { return static_cast<std::uint32_t>(next() % n); }
    [[nodiscard]] bool chance(std::uint32_t percent) noexcept { return below(100) < percent; }

private:
    std::uint64_t state_;
};

/// The bench of the seeded runs: the receiving node B of the engine on its host, the wire the
/// talker writes to, and a tap. One tick is 1 ms.
struct Rig
{
    explicit Rig(int stepMs) : stepMs_(stepMs)
    {
        host.add(network, port);
        host.set_step_size(std::chrono::milliseconds(stepMs));
        receiver.attach(network.add_node({kB}));
    }

    /// The start of tick `ms`: B's step if one is due, then the tap.
    void tick(int ms)
    {
        const SimTime now = at(std::chrono::milliseconds(ms));
        if (ms % stepMs_ == 0)
        {
            host.step(now);
        }
        tap.release_before(now, [this](const TimedFrame& f) { trace.push_back(f); });
    }
    void finish(int ms)
    {
        tap.release_before(at(std::chrono::milliseconds(ms)) + 1ns, [this](const TimedFrame& f) { trace.push_back(f); });
    }

    vehicle_sil::can::VirtualCanBus bus;
    vehicle_sil::can::VirtualCanBus::Endpoint& tap = bus.attach();
    vehicle_sil::can::VirtualCanBus::Endpoint& wire = bus.attach();
    vehicle_sil::can::VirtualCanBus::Endpoint& port = bus.attach();
    Network network{quiet()};
    vehicle_sil::can::Host host;
    Receiver receiver;
    std::vector<TimedFrame> trace;

private:
    int stepMs_;
};

/// What the talker did, to see that the runs are not empty of faults.
struct Tally
{
    int dropped = 0;
    int repeated = 0;
    int delayed = 0;
    int stalled = 0;
    int overlooked = 0;
    int aborted = 0;
    int announced_again = 0;
    int announced_other = 0;
    int malformed = 0;
};

/// Plays the sender A of connections to B: answers B's clear-to-sends with packets, with faults.
/// It keeps two rules of a real sender, without which nobody on the bus can tell the transfers
/// apart (see the table at same_end()): it announces or aborts only while none of its packets
/// is on its way, and after its last packet it waits for the receiver's answer.
class ConnectionTalker
{
public:
    ConnectionTalker(Rig& rig, Random& random, Tally& tally, int transfers)
      : rig_(rig), random_(random), tally_(tally), left_(transfers)
    {
    }

    [[nodiscard]] bool done() const noexcept { return left_ == 0 && !active_; }

    void tick(int ms, const std::vector<J1939Frame>& fromBus)
    {
        now_ = ms;
        for (const J1939Frame& f : fromBus)
        {
            if (!active_ || f.pgn != kPgnTpCm || f.sa != kB || f.da != kA || f.data.size() < 8)
            {
                continue;
            }
            const std::uint32_t pgn = f.data[5] | (static_cast<std::uint32_t>(f.data[6]) << 8) |
                                      (static_cast<std::uint32_t>(f.data[7]) << 16);
            if (pgn != pgn_)
            {
                continue; // e.g. the refusal of an announcement for another group
            }
            if (f.data[0] == 19 || f.data[0] == 255)
            {
                end();
            }
            else if (f.data[0] == 17 && f.data[1] != 0)
            {
                on_cts(f.data[1], f.data[2]);
            }
        }
        if (active_ && now_ - since_ > 4000)
        {
            end(); // no answer at all (cannot happen with a receiver that aborts)
            ++unanswered;
        }
        if (!active_ && left_ > 0 && now_ >= next_start_ && now_ > quiet_until_)
        {
            --left_;
            announce(false);
        }
    }

    int unanswered = 0;

private:
    void send(int ms, const J1939Frame& f)
    {
        rig_.wire.send(TimedFrame{at(std::chrono::milliseconds(ms)), to_can(f)});
    }

    void end()
    {
        active_ = false;
        next_start_ = now_ + 1 + static_cast<int>(random_.below(40));
    }

    void announce(bool sameGroup)
    {
        static constexpr std::array<std::uint32_t, 4> groups{kPgn, kOther, 0xFECA, kPgnRequest};
        static constexpr std::array<std::size_t, 8> sizes{9, 14, 15, 30, 60, 100, 250, 1785};
        static constexpr std::array<std::uint8_t, 4> limits{0xFF, 0xFF, 3, 1};
        if (!sameGroup)
        {
            pgn_ = groups[random_.below(random_.chance(10) ? 4 : 3)];
        }
        size_ = sizes[random_.below(random_.chance(3) ? 8 : 7)];
        message_ = payload(size_, static_cast<std::uint8_t>(random_.below(256)));
        paced_ = random_.chance(50);
        active_ = true;
        sent_last_ = false;
        since_ = now_;
        if (!sameGroup && random_.chance(3))
        {
            ++tally_.malformed; // the receiver refuses it with an abort, which ends it here too
            send(now_, rts(kA, kB, size_, pgn_, 0xFF, packet_count(size_) + 1));
            return;
        }
        send(now_, rts(kA, kB, size_, pgn_, limits[random_.below(4)]));
    }

    void on_cts(std::uint8_t count, std::uint8_t first)
    {
        if (random_.chance(3))
        {
            ++tally_.overlooked; // the receiver waits for packets and gives up
            return;
        }
        since_ = now_;
        // The two rules: nothing of this transfer is on its way, and its last packet was not
        // sent yet (after it, the receiver's answer may be on its way).
        const bool mayDeviate = now_ > quiet_until_ && !sent_last_;
        const std::uint32_t what = random_.below(100);
        if (what < 4 && mayDeviate)
        {
            ++tally_.announced_again;
            announce(true);
            return;
        }
        if (what < 8 && mayDeviate)
        {
            ++tally_.announced_other;
            send(now_, rts(kA, kB, 30, pgn_ == kPgn ? kOther : kPgn)); // refused; this transfer goes on
        }
        else if (what < 11 && mayDeviate)
        {
            ++tally_.aborted;
            send(now_, conn_abort(kA, kB, 250, pgn_));
            end();
            return;
        }
        int packets = count;
        if (what >= 11 && what < 15)
        {
            ++tally_.stalled;
            packets = static_cast<int>(random_.below(count)); // then silence: the receiver gives up
        }
        const int last = packet_count(size_);
        for (int i = 0; i < packets && first + i <= last; ++i)
        {
            const J1939Frame packet = dt(kA, kB, static_cast<std::uint8_t>(first + i), message_);
            sent_last_ = sent_last_ || first + i == last;
            int when = now_ + (paced_ ? i : 0);
            const std::uint32_t fault = random_.below(100);
            if (fault < 3)
            {
                ++tally_.dropped;
                continue;
            }
            if (fault < 6)
            {
                ++tally_.repeated;
                send(when, packet);
            }
            else if (fault < 10)
            {
                ++tally_.delayed;
                when += 1 + static_cast<int>(random_.below(4));
            }
            send(when, packet);
            quiet_until_ = std::max(quiet_until_, when);
        }
    }

    Rig& rig_;
    Random& random_;
    Tally& tally_;
    int left_;
    int now_ = 0;
    bool active_ = false;
    bool paced_ = false;
    bool sent_last_ = false;
    std::uint32_t pgn_ = kPgn;
    std::size_t size_ = 0;
    Bytes message_;
    int since_ = 0;
    int next_start_ = 0;
    int quiet_until_ = -1;
};

/// Plays the sender C of broadcasts: packets 2 ms apart, with faults; sometimes it stops, and
/// sometimes it announces the next one before the last one ended.
class BroadcastTalker
{
public:
    BroadcastTalker(Rig& rig, Random& random, Tally& tally, int transfers)
      : rig_(rig), random_(random), tally_(tally), left_(transfers)
    {
    }

    [[nodiscard]] bool done() const noexcept { return left_ == 0; }

    void tick(int ms)
    {
        if (left_ == 0 || ms < next_start_)
        {
            return;
        }
        --left_;
        static constexpr std::array<std::size_t, 5> sizes{9, 20, 30, 70, 200};
        static constexpr std::array<std::uint32_t, 3> groups{0xFECA, 0xFEE3, kPgnAddressClaim};
        const std::size_t size = sizes[random_.below(5)];
        const std::uint32_t pgn = groups[random_.below(random_.chance(10) ? 3 : 2)];
        const Bytes message = payload(size, static_cast<std::uint8_t>(random_.below(256)));
        const int packets = packet_count(size);
        const auto send = [&](int when, const J1939Frame& f) {
            rig_.wire.send(TimedFrame{at(std::chrono::milliseconds(when)), to_can(f)});
        };
        if (random_.chance(3))
        {
            ++tally_.malformed;
            send(ms, bam(kC, size, pgn, packets + 1)); // not followed: the packets belong to nothing
        }
        else
        {
            send(ms, bam(kC, size, pgn));
        }
        int sent = packets;
        if (random_.chance(8))
        {
            ++tally_.stalled;
            sent = static_cast<int>(random_.below(static_cast<std::uint32_t>(packets)));
        }
        for (int i = 0; i < sent; ++i)
        {
            const J1939Frame packet = dt(kC, kGlobal, static_cast<std::uint8_t>(1 + i), message);
            int when = ms + 2 * (1 + i);
            const std::uint32_t fault = random_.below(200);
            if (fault < 3)
            {
                ++tally_.dropped;
                continue;
            }
            if (fault < 6)
            {
                ++tally_.repeated;
                send(when, packet);
            }
            else if (fault < 9)
            {
                ++tally_.delayed;
                when += 3;
            }
            send(when, packet);
        }
        // Mostly after the end of this one (and sometimes after its timeout), sometimes during it.
        const int length = 2 * (1 + packets);
        next_start_ = ms + (random_.chance(15) ? 1 + static_cast<int>(random_.below(static_cast<std::uint32_t>(length)))
                                               : length + 5 + static_cast<int>(random_.below(random_.chance(30) ? 1500 : 50)));
    }

private:
    Rig& rig_;
    Random& random_;
    Tally& tally_;
    int left_;
    int next_start_ = 3;
};

/// The end the sniffer reports for an end of the engine's receiver. The time is compared too,
/// except where the table says otherwise.
///
/// | the engine's receiving node            | the sniffer                    | time                |
/// |----------------------------------------|--------------------------------|---------------------|
/// | Completed (and a message)              | Completed (the same message)   | equal               |
/// | Cancelled, a group it does not take    | Refused                        | equal               |
/// | Cancelled, the sender announced again  | Replaced                       | equal               |
/// | AbortedByPeer, reason                  | AbortedBySender, reason        | equal               |
/// | Aborted, reason (timeout, asked twice) | AbortedByReceiver, reason      | engine: when it gave|
/// |                                        |                                | up; sniffer: abort  |
/// | Lost, BadSequence (broadcast)          | Lost                           | equal               |
/// | Lost, Timeout (broadcast)              | TimedOut                       | equal               |
///
/// Only the sniffer has Incomplete and, for connections, TimedOut: neither happens when it sees
/// every frame the receiver sees. What nobody on the bus can tell apart is kept out of the runs
/// by the talker: packets of an earlier transfer that arrive during the next one of the same
/// sender, and a sender that announces or aborts between its last packet and the receiver's
/// answer to it (the receiver may have the message already; the bus shows it only afterwards).
[[nodiscard]] bool same_end(const EngineEnd& engine, const Heard& sniffer)
{
    if (engine.pgn != sniffer.pgn || engine.sa != sniffer.sa || engine.da != sniffer.da || engine.size != sniffer.size)
    {
        return false;
    }
    const bool sameTime = engine.time == sniffer.time;
    const auto reason = static_cast<std::uint8_t>(engine.reason);
    switch (engine.result)
    {
    case TransferResult::Completed:
        return sniffer.end == SniffEnd::Completed && sameTime;
    case TransferResult::Cancelled:
        if (sniffer.end == SniffEnd::Refused)
        {
            return sameTime && (engine.pgn == kPgnRequest || engine.pgn == kPgnAddressClaim);
        }
        return sniffer.end == SniffEnd::Replaced && sameTime;
    case TransferResult::AbortedByPeer:
        return sniffer.end == SniffEnd::AbortedBySender && sniffer.reason == reason && sameTime;
    case TransferResult::Aborted:
        return sniffer.end == SniffEnd::AbortedByReceiver && sniffer.reason == reason;
    case TransferResult::Lost:
        return sameTime && sniffer.end == (engine.reason == AbortReason::Timeout ? SniffEnd::TimedOut : SniffEnd::Lost);
    default:
        return false;
    }
}

[[nodiscard]] std::string to_text(const EngineEnd& e)
{
    return "engine: result " + std::to_string(static_cast<int>(e.result)) + " reason " +
           std::to_string(static_cast<int>(e.reason)) + " PGN " + std::to_string(e.pgn) + " " + std::to_string(e.sa) +
           "->" + std::to_string(e.da) + " size " + std::to_string(e.size) + " t=" + std::to_string(vehicle_sil::to_ns(e.time));
}

[[nodiscard]] std::string to_text(const Heard& h)
{
    return "sniffer: end " + std::to_string(static_cast<int>(h.end)) + " reason " + std::to_string(h.reason) + " PGN " +
           std::to_string(h.pgn) + " " + std::to_string(h.sa) + "->" + std::to_string(h.da) + " size " +
           std::to_string(h.size) + " t=" + std::to_string(vehicle_sil::to_ns(h.time));
}

template <typename T>
[[nodiscard]] std::vector<T> from(const std::vector<T>& all, Address source)
{
    std::vector<T> out;
    std::copy_if(all.begin(), all.end(), std::back_inserter(out), [source](const T& x) { return x.sa == source; });
    return out;
}

struct Totals
{
    std::map<SniffEnd, int> ends;
    Tally tally;
    std::uint64_t retransmit_requests = 0;
    std::uint64_t announcements_ignored = 0;
    std::size_t messages = 0;
};

/// One seeded run: the talkers and the engine's node B on one bus, then the recorded frames
/// through the sniffer. Throws (with the seed) at the first difference.
void run_seed(std::uint64_t seed, Totals& totals)
{
    Random random(seed);
    static constexpr std::array<int, 3> steps{1, 3, 10};
    Rig rig(steps[random.below(3)]);
    Tally tally;
    // The receiver's clear-to-sends are sometimes repeated on the bus. None of its frames is
    // lost: a frame the receiver believes it sent and nobody saw does not happen on a CAN bus
    // (the talker overlooks some instead).
    auto busRandom = std::make_shared<Random>(seed ^ 0x5555);
    rig.port.set_fault_injector([busRandom](const TimedFrame& f) {
        if (!is_cm(f, 17))
        {
            return FaultAction::deliver();
        }
        return busRandom->chance(4) ? FaultAction::duplicate() : FaultAction::deliver();
    });
    ConnectionTalker connections(rig, random, tally, 25);
    BroadcastTalker broadcasts(rig, random, tally, 25);
    std::vector<J1939Frame> fromBus;
    int ms = 0;
    int quietSince = -1;
    for (;; ++ms)
    {
        rig.tick(ms);
        fromBus.clear();
        rig.wire.release_before(at(std::chrono::milliseconds(ms)) + 1ns,
                                [&](const TimedFrame& f) { fromBus.push_back(from_can(f.frame)); });
        connections.tick(ms, fromBus);
        broadcasts.tick(ms);
        if (connections.done() && broadcasts.done())
        {
            quietSince = quietSince < 0 ? ms : quietSince;
            if (ms - quietSince > 3000)
            {
                break; // every transfer has ended or timed out
            }
        }
        if (ms > 600000)
        {
            throw vehicle_sil::unit::Failure("seed " + std::to_string(seed) + ": the run does not end");
        }
    }
    rig.finish(ms);

    Listener listener;
    listener.replay(rig.trace, at(std::chrono::milliseconds(ms)), std::chrono::milliseconds(1 + random.below(20)));

    const auto fail = [&](const std::string& what) {
        throw vehicle_sil::unit::Failure("seed " + std::to_string(seed) + ": " + what);
    };
    if (connections.unanswered != 0)
    {
        fail("the receiver left an announcement without an answer");
    }
    for (const Address source : {kA, kC})
    {
        const auto engineMessages = from(rig.receiver.messages, source);
        const auto sniffed = from(listener.messages, source);
        for (std::size_t i = 0; i < std::max(engineMessages.size(), sniffed.size()); ++i)
        {
            if (i >= engineMessages.size() || i >= sniffed.size() || !(engineMessages[i] == sniffed[i]))
            {
                fail("message " + std::to_string(i) + " from " + std::to_string(source) + " differs\n  engine:  " +
                     (i < engineMessages.size() ? to_text(engineMessages[i]) : "(none)") + "\n  sniffer: " +
                     (i < sniffed.size() ? to_text(sniffed[i]) : "(none)"));
            }
        }
        const auto engineEnds = from(rig.receiver.ends, source);
        const auto sniffedEnds = from(listener.ends, source);
        for (std::size_t i = 0; i < std::max(engineEnds.size(), sniffedEnds.size()); ++i)
        {
            if (i >= engineEnds.size() || i >= sniffedEnds.size() || !same_end(engineEnds[i], sniffedEnds[i]))
            {
                fail("end " + std::to_string(i) + " from " + std::to_string(source) + " differs\n  " +
                     (i < engineEnds.size() ? to_text(engineEnds[i]) : "engine: (none)") + "\n  " +
                     (i < sniffedEnds.size() ? to_text(sniffedEnds[i]) : "sniffer: (none)"));
            }
        }
    }
    const auto& statistics = listener.sniffer.statistics();
    if (listener.sniffer.active() != 0 || statistics.incomplete != 0 || statistics.conflicting_packets != 0 ||
        statistics.no_session != 0)
    {
        fail("the sniffer is left with " + std::to_string(listener.sniffer.active()) + " transfers, " +
             std::to_string(statistics.incomplete) + " incomplete, " + std::to_string(statistics.conflicting_packets) +
             " conflicting packets, " + std::to_string(statistics.no_session) + " announcements without a place");
    }
    for (const Heard& h : listener.ends)
    {
        ++totals.ends[h.end];
    }
    totals.messages += listener.messages.size();
    totals.retransmit_requests += statistics.retransmit_requests;
    totals.announcements_ignored += statistics.announcements_ignored;
    totals.tally.dropped += tally.dropped;
    totals.tally.repeated += tally.repeated;
    totals.tally.delayed += tally.delayed;
    totals.tally.stalled += tally.stalled;
    totals.tally.overlooked += tally.overlooked;
    totals.tally.aborted += tally.aborted;
    totals.tally.announced_again += tally.announced_again;
    totals.tally.announced_other += tally.announced_other;
    totals.tally.malformed += tally.malformed;
}

} // namespace

TEST_CASE(sniffer_agrees_with_the_receiving_node_under_seeded_faults)
{
    Totals totals;
    for (std::uint64_t seed = 1; seed <= 40; ++seed)
    {
        run_seed(seed, totals);
    }
    // The runs had every kind of fault and every end the table names.
    CHECK(totals.tally.dropped > 0);
    CHECK(totals.tally.repeated > 0);
    CHECK(totals.tally.delayed > 0);
    CHECK(totals.tally.stalled > 0);
    CHECK(totals.tally.overlooked > 0);
    CHECK(totals.tally.aborted > 0);
    CHECK(totals.tally.announced_again > 0);
    CHECK(totals.tally.announced_other > 0);
    CHECK(totals.tally.malformed > 0);
    CHECK(totals.messages > 400);
    CHECK(totals.retransmit_requests > 0);
    CHECK(totals.announcements_ignored > 0);
    for (const SniffEnd end : {SniffEnd::Completed, SniffEnd::AbortedBySender, SniffEnd::AbortedByReceiver,
                               SniffEnd::Replaced, SniffEnd::Lost, SniffEnd::TimedOut, SniffEnd::Refused})
    {
        CHECK(totals.ends[end] > 0);
    }
    CHECK_EQ(totals.ends[SniffEnd::Incomplete], 0);
}
