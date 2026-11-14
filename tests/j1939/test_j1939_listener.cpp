// SPDX-License-Identifier: MIT
//
// Listener: what a participant hears of the other participants on a J1939 bus. First beside
// real engines (the message of a connection between two other ECUs is the message the receiving
// node gets), then what it does not hear (its own network's nodes), the addresses of its own
// nodes, the order of the handler calls, that handlers only read, and its lifetime.
#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/j1939/listener.hpp"
#include "vehicle_sil/j1939/network.hpp"
#include "vehicle_sil/j1939/observer.hpp"
#include "vehicle_sil/j1939/toolbox.hpp"
#include "vehicle_sil/testkit/recorders.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::to_ns;
using vehicle_sil::can::FaultAction;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::Event;
using vehicle_sil::j1939::EventKind;
using vehicle_sil::j1939::Listener;
using vehicle_sil::j1939::ListenerOptions;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Network;
using vehicle_sil::j1939::NetworkOptions;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::NodeOptions;
using vehicle_sil::j1939::Observer;
using vehicle_sil::j1939::ReceivedFrame;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::SniffDelivery;
using vehicle_sil::j1939::SniffedTransfer;
using vehicle_sil::j1939::SniffEnd;
using vehicle_sil::j1939::SnifferOptions;
using vehicle_sil::j1939::SnifferStatistics;
using vehicle_sil::j1939::Timer;
using vehicle_sil::j1939::TimerKind;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;

namespace {

constexpr Address kA = 0x10;
constexpr Address kB = 0x20;
constexpr Address kC = 0x30;
constexpr Address kTool = 0xF9;  // a node of the listening participant
constexpr Address kTool2 = 0xFA; // its second node
constexpr std::uint32_t kPgn = 0xEF00;
constexpr std::uint32_t kPgn2 = 0xED00;

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

NetworkOptions quiet()
{
    NetworkOptions options;
    options.warning = [](std::string_view) {};
    return options;
}

struct Ended
{
    SniffEnd end;
    std::uint32_t pgn;
    std::uint8_t sa;
    std::uint8_t da;
    std::uint8_t reason;
    SimTime time;
    std::uint32_t conflicting;
    bool operator==(const Ended&) const = default;
};

struct HeardFrame
{
    SimTime time;
    std::uint32_t id;
    Bytes data;
    bool local;
    bool operator==(const HeardFrame&) const = default;
};

/// What a Listener handed on, kept by handlers that only record.
struct Recording
{
    void attach(Listener& listener)
    {
        listener.on_any_message([this](const Message& m) { heard.push_back(seen_of(m)); });
        listener.on_transfer([this](const SniffedTransfer& t) {
            ended.push_back(Ended{t.end, t.pgn.value(), t.source, t.destination, t.reason, t.time, t.conflicting_packets});
        });
        listener.on_frame([this](const ReceivedFrame& f) {
            frames.push_back(HeardFrame{f.time, f.frame.id, Bytes(f.data.begin(), f.data.end()), f.local});
            CHECK(f.frame.id == j1939_id(f.priority, f.pgn.value(), f.destination, f.source));
            CHECK(Bytes(f.data.begin(), f.data.end()) == from_can(f.frame).data);
        });
    }
    [[nodiscard]] std::vector<Seen> heard_of(Address sa, Address da) const
    {
        std::vector<Seen> out;
        for (const auto& s : heard)
        {
            if (s.sa == sa && s.da == da)
            {
                out.push_back(s);
            }
        }
        return out;
    }
    [[nodiscard]] std::vector<SniffEnd> ends() const
    {
        std::vector<SniffEnd> out;
        for (const auto& e : ended)
        {
            out.push_back(e.end);
        }
        return out;
    }

    std::vector<Seen> heard;
    std::vector<Ended> ended;
    std::vector<HeardFrame> frames;
};

using EndList = std::vector<SniffEnd>;

/// ECUs A, B and C (one node each, an engine of this repository) and a participant that listens
/// (a Network with a Listener; without a node unless the test adds some), each on its own host
/// and step on one virtual bus.
struct Rig
{
    explicit Rig(const std::vector<SimDuration>& steps, NetworkOptions bOptions = quiet(), ListenerOptions listening = {})
    {
        const Address addresses[] = {kA, kB, kC};
        for (std::size_t i = 0; i < 3; ++i)
        {
            auto e = std::make_unique<Ecu>(bus, i == 1 ? bOptions : quiet());
            e->node = &e->network.add_node({addresses[i]});
            e->node->on_any_message([p = e.get()](const Message& m) { p->seen.push_back(seen_of(m)); });
            e->host.set_application([this, i](SimTime now) {
                if (apps[i])
                {
                    apps[i](now);
                }
            });
            driver.add(e->host, steps[i]);
            ecus.push_back(std::move(e));
        }
        spy = std::make_unique<Ecu>(bus, quiet());
        spy->host.set_application([this](SimTime now) {
            if (spy_app)
            {
                spy_app(now);
            }
        });
        listener = std::make_unique<Listener>(spy->network, listening);
        recording.attach(*listener);
        driver.add(spy->host, steps[3]);
    }

    struct Ecu
    {
        Ecu(vehicle_sil::can::VirtualCanBus& bus, const NetworkOptions& options) : port(bus.attach()), network(options)
        {
            host.add(network, port);
        }
        vehicle_sil::can::VirtualCanBus::Endpoint& port;
        Network network;
        vehicle_sil::can::Host host;
        Node* node = nullptr;
        std::vector<Seen> seen;
    };

    void run_until(SimTime end) { driver.run_until(end); }

    [[nodiscard]] std::vector<Seen> seen_at(std::size_t ecu, Address sa, Address da) const
    {
        std::vector<Seen> out;
        for (const auto& s : ecus[ecu]->seen)
        {
            if (s.sa == sa && s.da == da)
            {
                out.push_back(s);
            }
        }
        return out;
    }
    [[nodiscard]] const SnifferStatistics& stats() const { return listener->sniffer_statistics(); }

    /// The listening participant sent nothing (it has no node), did not fail and lost no frame.
    void check_listened_only() const
    {
        CHECK_EQ(spy->network.statistics().frames_sent, 0u);
        CHECK(!spy->network.failed());
        CHECK_EQ(spy->network.statistics().receive_overflows, 0u);
    }

    vehicle_sil::can::VirtualCanBus bus;
    std::vector<std::unique_ptr<Ecu>> ecus;
    std::unique_ptr<Ecu> spy;
    std::unique_ptr<Listener> listener;
    Recording recording;
    std::function<void(SimTime)> apps[3];
    std::function<void(SimTime)> spy_app;
    vehicle_sil::can::StepDriver driver;
};

/// Steps of A, B, C and the listening participant.
const std::vector<std::vector<SimDuration>> kSteps{
    {10ms, 10ms, 10ms, 10ms}, {1ms, 10ms, 5ms, 5ms}, {7ms, 3ms, 10ms, 1ms}, {10ms, 10ms, 10ms, 50ms}, {5ms, 5ms, 5ms, 3ms}};

[[nodiscard]] bool is_dt(const TimedFrame& f, int sequence = -1)
{
    return ((f.frame.id >> 8) & 0x3FF00u) == kPgnTpDt && (sequence < 0 || f.frame.data[0] == sequence);
}

[[nodiscard]] J1939Frame claim(Address sa, Name name)
{
    const auto bytes = name.to_bytes();
    return frame(6, kPgnAddressClaim, sa, kGlobal, Bytes(bytes.begin(), bytes.end()));
}

/// The frames of a connection sa -> da written out by hand, each 1 ms after the one before.
void inject_connection(Bench& bench, SimTime start, Address sa, Address da, const Bytes& message, std::uint32_t pgn = kPgn)
{
    const std::uint8_t packets = packet_count(message.size());
    SimTime t = start;
    bench.inject(t, rts(sa, da, message.size(), pgn));
    bench.inject(t += 1ms, cts(da, sa, packets, 1, pgn));
    for (const auto& f : dts(sa, da, message, 1, packets))
    {
        bench.inject(t += 1ms, f);
    }
    bench.inject(t += 1ms, eoma(da, sa, message.size(), pgn));
}

struct LoggingObserver final : Observer
{
    void on_event(const Event& e) override
    {
        if (e.kind == EventKind::MessageReceived)
        {
            log->push_back("observer " + std::to_string(e.pgn.value()));
        }
    }
    std::vector<std::string>* log = nullptr;
};

} // namespace

// --- beside real engines: what is heard is what the receiver got --------------------------------

TEST_CASE(listener_hears_a_connection_between_two_other_ecus_as_the_receiver_gets_it)
{
    for (const auto& steps : kSteps)
    {
        Rig rig(steps);
        rig.apps[0] = [&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(rig.ecus[0]->node->send(Pgn{kPgn}, payload(100), kB) == SendStatus::Ok);
            }
        };
        rig.run_until(at(2s));
        const auto atB = rig.seen_at(1, kA, kB);
        CHECK_EQ(atB.size(), 1u);
        CHECK(atB.at(0).data == payload(100));
        CHECK(rig.recording.heard == atB);    // group, priority, addresses, time and bytes; nothing else
        CHECK(rig.ecus[2]->seen.empty());     // C, a node, still hears nothing of it
        CHECK(rig.recording.ends() == (EndList{SniffEnd::Completed}));
        CHECK_EQ(rig.recording.ended.at(0).conflicting, 0u);
        CHECK(rig.recording.ended.at(0).time == atB.at(0).time);
        CHECK_EQ(rig.listener->open_transfers(), 0u);
        CHECK(rig.recording.frames.size() >= 18u); // RTS, CTS, 15 packets, the acknowledgement
        CHECK_EQ(rig.stats().orphan_control + rig.stats().orphan_data + rig.stats().no_session, 0u);
        rig.check_listened_only();
    }
}

TEST_CASE(listener_hears_single_frames_and_broadcasts_once)
{
    for (const auto& steps : kSteps)
    {
        // Also when told to follow broadcasts: the network assembles them, the sniffer must not.
        ListenerOptions options;
        options.sniffer.broadcasts = true;
        Rig rig(steps, quiet(), options);
        CHECK(!rig.listener->options().sniffer.broadcasts);
        rig.apps[0] = [&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(rig.ecus[0]->node->send(Pgn{0xFF31}, payload(30), kGlobal) == SendStatus::Ok); // BAM
                CHECK(rig.ecus[0]->node->send(Pgn{kPgn2}, payload(8, 3), kB) == SendStatus::Ok);     // one frame, to B
            }
        };
        rig.run_until(at(2s));
        CHECK_EQ(rig.recording.heard_of(kA, kGlobal).size(), 1u);
        CHECK(rig.recording.heard_of(kA, kGlobal) == rig.seen_at(1, kA, kGlobal));
        CHECK_EQ(rig.recording.heard_of(kA, kB).size(), 1u);
        CHECK(rig.recording.heard_of(kA, kB) == rig.seen_at(1, kA, kB));
        CHECK_EQ(rig.recording.heard.size(), 2u);
        CHECK(rig.recording.ended.empty()); // the broadcast is the network's, not the sniffer's
        CHECK(rig.stats() == SnifferStatistics{});
        rig.check_listened_only();
    }
}

TEST_CASE(listener_hears_transfers_that_run_at_the_same_time)
{
    for (const auto& steps : kSteps)
    {
        Rig rig(steps);
        rig.apps[0] = [&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(rig.ecus[0]->node->send(Pgn{kPgn}, payload(200, 1), kB) == SendStatus::Ok);
            }
        };
        rig.apps[1] = [&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(rig.ecus[1]->node->send(Pgn{kPgn}, payload(150, 2), kA) == SendStatus::Ok); // the other way, same pair
            }
        };
        rig.apps[2] = [&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(rig.ecus[2]->node->send(Pgn{kPgn2}, payload(90, 3), kB) == SendStatus::Ok);
            }
            if (now == at(30ms))
            {
                CHECK(rig.ecus[2]->node->send(Pgn{0xFF40}, payload(40, 4), kGlobal) == SendStatus::Ok); // a BAM beside it
            }
        };
        rig.run_until(at(3s));
        CHECK_EQ(rig.seen_at(1, kA, kB).size(), 1u);
        CHECK_EQ(rig.seen_at(0, kB, kA).size(), 1u);
        CHECK_EQ(rig.seen_at(1, kC, kB).size(), 1u);
        CHECK(rig.recording.heard_of(kA, kB) == rig.seen_at(1, kA, kB));
        CHECK(rig.recording.heard_of(kB, kA) == rig.seen_at(0, kB, kA));
        CHECK(rig.recording.heard_of(kC, kB) == rig.seen_at(1, kC, kB));
        CHECK(rig.recording.heard_of(kC, kGlobal) == rig.seen_at(1, kC, kGlobal));
        CHECK_EQ(rig.recording.heard.size(), 4u);
        CHECK(rig.recording.ends() == (EndList{SniffEnd::Completed, SniffEnd::Completed, SniffEnd::Completed}));
        rig.check_listened_only();
    }
}

TEST_CASE(listener_hears_a_connection_with_retransmissions_and_delays)
{
    for (const auto& steps : kSteps)
    {
        Rig rig(steps);
        int dropped = 0;
        int delayed = 0;
        rig.ecus[0]->port.set_fault_injector([&](const TimedFrame& f) {
            if (is_dt(f, 3) && dropped < 1)
            {
                ++dropped;
                return FaultAction::drop(); // B asks for packet 3 again
            }
            if (is_dt(f, 20) && delayed < 1)
            {
                ++delayed;
                return FaultAction::delay_by(40ms); // late, after later packets
            }
            if (is_dt(f, 12))
            {
                return FaultAction::duplicate();
            }
            return FaultAction::deliver();
        });
        rig.apps[0] = [&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(rig.ecus[0]->node->send(Pgn{kPgn}, payload(150), kB) == SendStatus::Ok);
            }
        };
        rig.run_until(at(5s));
        const auto atB = rig.seen_at(1, kA, kB);
        CHECK_EQ(atB.size(), 1u);
        CHECK(rig.recording.heard == atB);
        CHECK(rig.ecus[1]->network.statistics().transport_retransmit_requests >= 1u);
        CHECK(rig.stats().retransmit_requests >= 1u);
        CHECK(rig.stats().repeated_packets >= 1u);
        CHECK_EQ(rig.stats().conflicting_packets, 0u);
        rig.check_listened_only();
    }
}

TEST_CASE(listener_does_not_hand_on_a_connection_the_receiver_refused_or_gave_up)
{
    for (const auto& steps : kSteps)
    {
        // Refused: B takes one connection at a time; A and C start together.
        NetworkOptions one = quiet();
        one.rts_receive_sessions = 1;
        Rig rig(steps, one);
        rig.apps[0] = [&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(rig.ecus[0]->node->send(Pgn{kPgn}, payload(100, 1), kB) == SendStatus::Ok);
            }
        };
        rig.apps[2] = [&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(rig.ecus[2]->node->send(Pgn{kPgn}, payload(100, 2), kB) == SendStatus::Ok);
            }
        };
        rig.run_until(at(3s));
        CHECK_EQ(rig.ecus[1]->seen.size(), 1u);
        CHECK(rig.recording.heard == rig.ecus[1]->seen);
        CHECK_EQ(rig.recording.ended.size(), 2u);
        CHECK_EQ(rig.stats().aborted_by_receiver, 1u);
        CHECK_EQ(rig.stats().completed, 1u);
        rig.check_listened_only();
    }
    for (const auto& steps : kSteps)
    {
        // Gave up: every packet from 5 on is lost; B aborts after its timeout.
        Rig rig(steps);
        rig.ecus[0]->port.set_fault_injector([&](const TimedFrame& f) {
            return is_dt(f) && f.frame.data[0] >= 5 ? FaultAction::drop() : FaultAction::deliver();
        });
        rig.apps[0] = [&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(rig.ecus[0]->node->send(Pgn{kPgn}, payload(100), kB) == SendStatus::Ok);
            }
        };
        rig.run_until(at(5s));
        CHECK(rig.ecus[1]->seen.empty());
        CHECK(rig.recording.heard.empty());
        CHECK(rig.recording.ends() == (EndList{SniffEnd::AbortedByReceiver}));
        CHECK_EQ(rig.listener->open_transfers(), 0u);
        CHECK_EQ(rig.stats().orphan_control, 0u);
        rig.check_listened_only();
    }
}

// --- the nodes of the listener's own network ----------------------------------------------------

TEST_CASE(listener_with_a_node_gets_the_messages_of_that_nodes_connections_once_and_does_not_sniff_them)
{
    for (const auto& steps : kSteps)
    {
        Rig rig(steps);
        Node& tool = rig.spy->network.add_node({kTool});
        std::vector<Seen> atTool;
        tool.on_any_message([&](const Message& m) { atTool.push_back(seen_of(m)); });
        rig.apps[0] = [&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(rig.ecus[0]->node->send(Pgn{kPgn}, payload(100, 1), kTool) == SendStatus::Ok); // to the node
            }
            if (now == at(490ms))
            {
                CHECK(rig.ecus[0]->node->send(Pgn{kPgn}, payload(60, 2), kB) == SendStatus::Ok); // between others
            }
        };
        rig.spy_app = [&](SimTime now) {
            if (now == at(1050ms))
            {
                CHECK(tool.send(Pgn{kPgn}, payload(80, 3), kB) == SendStatus::Ok); // from the node
            }
        };
        rig.run_until(at(3s));
        CHECK_EQ(atTool.size(), 1u);
        CHECK(rig.recording.heard_of(kA, kTool) == atTool); // assembled by the network, handed on once
        CHECK_EQ(rig.seen_at(1, kTool, kB).size(), 1u);     // B got the node's message,
        CHECK(rig.recording.heard_of(kTool, kB).empty());   // which the listener does not hear
        CHECK(rig.recording.heard_of(kA, kB) == rig.seen_at(1, kA, kB));
        CHECK_EQ(rig.recording.heard.size(), 2u);
        // Only the connection between A and B was sniffed; the node's own left no trace there.
        CHECK(rig.recording.ends() == (EndList{SniffEnd::Completed}));
        CHECK_EQ(rig.stats().started, 1u);
        CHECK_EQ(rig.stats().orphan_control, 0u);
        CHECK_EQ(rig.stats().orphan_data, 0u);
        CHECK_EQ(rig.stats().no_session, 0u);
        CHECK_EQ(rig.stats().lost, 0u);
        CHECK(!rig.spy->network.failed());
    }
}

namespace {

struct LocalRun
{
    Recording recording;
    SnifferStatistics statistics;
    std::size_t at_b = 0; ///< messages B got from the listening participant's node
};

/// The listening participant has `nodes` nodes (0xF9, 0xFA). Its first node sends a single
/// frame, a broadcast and a connection to B, takes a connection from A and, with `between`,
/// sends a connection to its second node. A also talks to B and to all.
LocalRun run_with_local_nodes(std::size_t nodes, bool hearLocal, bool between)
{
    ListenerOptions options;
    options.hear_local = hearLocal;
    Rig rig({5ms, 5ms, 5ms, 5ms}, quiet(), options);
    Node& tool = rig.spy->network.add_node({kTool});
    if (nodes > 1)
    {
        rig.spy->network.add_node({kTool2});
    }
    rig.apps[0] = [&](SimTime now) {
        if (now == at(50ms))
        {
            CHECK(rig.ecus[0]->node->send(Pgn{kPgnPropB}, Bytes{1, 2, 3}) == SendStatus::Ok);
        }
        if (now == at(300ms))
        {
            CHECK(rig.ecus[0]->node->send(Pgn{kPgn}, payload(60, 1), kB) == SendStatus::Ok);
        }
        if (now == at(600ms))
        {
            CHECK(rig.ecus[0]->node->send(Pgn{kPgn}, payload(60, 2), kTool) == SendStatus::Ok);
        }
    };
    rig.spy_app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(tool.send(Pgn{kPgnPropB}, Bytes{9}) == SendStatus::Ok);
            CHECK(tool.send(Pgn{0xFF31}, payload(30, 4), kGlobal) == SendStatus::Ok);
        }
        if (now == at(100ms))
        {
            CHECK(tool.send(Pgn{kPgn}, payload(100, 5), kB) == SendStatus::Ok);
        }
        if (between && now == at(800ms))
        {
            CHECK(tool.send(Pgn{kPgn2}, payload(40, 6), kTool2) == SendStatus::Ok);
        }
    };
    rig.run_until(at(3s));
    CHECK(!rig.spy->network.failed());
    CHECK(rig.spy->network.statistics().frames_sent > 20u);
    LocalRun out;
    out.recording = rig.recording;
    out.statistics = rig.stats();
    out.at_b = rig.seen_at(1, kTool, kB).size() + rig.seen_at(1, kTool, kGlobal).size();
    return out;
}

[[nodiscard]] bool from_tool(const HeardFrame& f) { return (f.id & 0xFFu) == kTool || (f.id & 0xFFu) == kTool2; }

} // namespace

TEST_CASE(listener_does_not_hear_the_nodes_of_its_own_network_whether_it_has_one_or_two)
{
    const LocalRun one = run_with_local_nodes(1, false, false);
    const LocalRun two = run_with_local_nodes(2, false, false);
    const LocalRun talking = run_with_local_nodes(2, false, true);
    CHECK_EQ(one.at_b, 3u); // the others did get what the node sent
    for (const LocalRun* run : {&one, &two, &talking})
    {
        // A's single frame, its connection to B (sniffed) and its connection to the node.
        CHECK_EQ(run->recording.heard.size(), 3u);
        for (const auto& s : run->recording.heard)
        {
            CHECK(s.sa == kA);
        }
        for (const auto& f : run->recording.frames)
        {
            CHECK(!from_tool(f));
            CHECK(!f.local);
        }
        CHECK(run->recording.ends() == (EndList{SniffEnd::Completed}));
        CHECK_EQ(run->statistics.started, 1u);
        CHECK_EQ(run->statistics.orphan_control + run->statistics.orphan_data + run->statistics.no_session, 0u);
    }
    // The second node changes nothing of what is heard, silent or talked to.
    CHECK(two.recording.heard == one.recording.heard);
    CHECK(two.recording.frames == one.recording.frames);
    CHECK(two.recording.ended == one.recording.ended);
    CHECK(two.statistics == one.statistics);
    CHECK(talking.recording.heard == one.recording.heard);
    CHECK(talking.recording.frames == one.recording.frames);
    CHECK(talking.statistics == one.statistics);
}

TEST_CASE(listener_told_to_hear_local_hears_what_two_nodes_loop_back_and_nothing_of_a_single_node)
{
    const LocalRun base = run_with_local_nodes(1, false, false);
    // One node: no loopback, so nothing more is heard.
    const LocalRun one = run_with_local_nodes(1, true, false);
    CHECK(one.recording.heard == base.recording.heard);
    CHECK(one.recording.frames == base.recording.frames);
    CHECK(one.statistics == base.statistics);
    // Two nodes: the frames of the first node are heard and marked, and the messages the
    // network hands to the other node: the single frame, the broadcast and the connection
    // between the two. The connection to B is not a message of this network.
    const LocalRun two = run_with_local_nodes(2, true, true);
    std::vector<HeardFrame> others;
    std::size_t local = 0;
    for (const auto& f : two.recording.frames)
    {
        CHECK(f.local == from_tool(f));
        if (f.local)
        {
            ++local;
        }
        else
        {
            others.push_back(f);
        }
    }
    CHECK(local > 20u);
    CHECK(others == base.recording.frames);
    std::vector<Seen> fromOthers;
    std::vector<Seen> fromTool;
    for (const auto& s : two.recording.heard)
    {
        (s.sa == kTool ? fromTool : fromOthers).push_back(s);
    }
    CHECK(fromOthers == base.recording.heard);
    CHECK_EQ(fromTool.size(), 3u);
    CHECK(fromTool.at(0).pgn == kPgnPropB && fromTool.at(0).data == Bytes{9});
    CHECK(fromTool.at(1).pgn == 0xFF31u && fromTool.at(1).data == payload(30, 4));
    CHECK(fromTool.at(2).pgn == kPgn2 && fromTool.at(2).da == kTool2 && fromTool.at(2).data == payload(40, 6));
    // The sniffer is not told about the network's own connections either way.
    CHECK(two.recording.ended == base.recording.ended);
    CHECK(two.statistics == base.statistics);
}

TEST_CASE(listener_ends_a_followed_transfer_as_lost_when_a_node_takes_its_address_and_follows_again_after)
{
    Bench bench(quiet());
    Listener listener(bench.network);
    Recording recording;
    recording.attach(listener);
    const Name ours = Name{}.set_identity_number(7).set_manufacturer_code(0x7F0);
    const Name stronger = Name{}.set_identity_number(1).set_manufacturer_code(0x7F0);
    Node* node = nullptr;
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            node = &bench.network.add_node(NodeOptions{kB, ours}); // takes part from the next step
        }
    };
    const Bytes message = payload(30);
    bench.step(); // 0
    bench.inject(at(1ms), rts(kA, kB, 30, kPgn));
    bench.inject(at(2ms), cts(kB, kA, 5, 1, kPgn));
    bench.step(); // 10 ms: followed; the node is added
    CHECK_EQ(listener.open_transfers(), 1u);
    bench.inject(at(12ms), dt(kA, kB, 1, message));
    bench.step(); // 20 ms: the node has not claimed yet when the frames are handled
    CHECK(node != nullptr);
    CHECK(node->address_state() == AddressState::Claiming || node->address_state() == AddressState::Claimed);
    CHECK_EQ(node->address(), kB);
    CHECK_EQ(listener.open_transfers(), 1u);
    CHECK_EQ(listener.sniffer_statistics().orphan_data, 0u);
    // The node has the address now: the next frame to it ends what was followed, at once.
    bench.inject(at(24ms), dt(kA, kB, 2, message));
    bench.inject(at(26ms), dt(kA, kB, 3, message));
    bench.step(); // 30 ms
    CHECK(recording.ended == (std::vector<Ended>{Ended{SniffEnd::Lost, kPgn, kA, kB, 0, at(24ms), 0}}));
    CHECK_EQ(listener.open_transfers(), 0u);
    CHECK_EQ(listener.sniffer_statistics().lost, 1u);
    CHECK_EQ(listener.sniffer_statistics().orphan_data, 0u); // not fed: the address is the node's
    CHECK(recording.heard.empty());
    // A conversation of that address is not followed while the node has it.
    bench.inject(at(31ms), rts(kA, kB, 14, kPgn)); // the node answers it
    bench.step(); // 40 ms
    CHECK_EQ(listener.sniffer_statistics().started, 1u);
    CHECK(recording.heard.empty());
    // The node loses the address to a stronger NAME. The claim itself is still a frame from the
    // node's address; what follows is between other ECUs again.
    bench.inject(at(41ms), claim(kB, stronger));
    bench.step(); // 50 ms
    CHECK(node->address_state() == AddressState::CannotClaim);
    CHECK_EQ(recording.heard.size(), 1u); // the claim, a single frame
    inject_connection(bench, at(51ms), kA, kB, payload(14, 2));
    bench.step(); // 60 ms
    CHECK(recording.ends() == (EndList{SniffEnd::Lost, SniffEnd::Completed}));
    CHECK_EQ(recording.heard.size(), 2u);
    CHECK(recording.heard.back().data == payload(14, 2));
    CHECK_EQ(listener.sniffer_statistics().orphan_control, 0u);
    CHECK_EQ(listener.sniffer_statistics().orphan_data, 0u);
    CHECK(!bench.network.failed());
}

TEST_CASE(listener_ends_a_followed_transfer_as_lost_when_a_node_takes_the_senders_address)
{
    Bench bench(quiet());
    Listener listener(bench.network);
    Recording recording;
    recording.attach(listener);
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            bench.network.add_node(NodeOptions{kA, std::nullopt}); // without a NAME: uses kA from its first step
        }
    };
    bench.step();
    bench.inject(at(1ms), rts(kA, kB, 30, kPgn));
    bench.inject(at(2ms), rts(kC, kB, 30, kPgn));
    bench.step(); // 10 ms
    CHECK_EQ(listener.open_transfers(), 2u);
    bench.inject(at(13ms), cts(kB, kA, 5, 1, kPgn)); // to the node's address
    bench.step(); // 20 ms
    CHECK(recording.ended == (std::vector<Ended>{Ended{SniffEnd::Lost, kPgn, kA, kB, 0, at(13ms), 0}}));
    CHECK_EQ(listener.open_transfers(), 1u); // C to B goes on
    CHECK_EQ(listener.sniffer_statistics().orphan_control, 0u);
}

// --- the order of the calls ----------------------------------------------------------------------

TEST_CASE(listener_is_told_after_the_observer_and_before_the_nodes_and_calls_its_handlers_as_registered)
{
    Bench bench(quiet());
    std::vector<std::string> log;
    LoggingObserver observer;
    observer.log = &log;
    bench.network.set_observer(&observer);
    Listener listener(bench.network);
    Node& node = bench.add_node(kTool);
    node.on_any_message([&](const Message& m) { log.push_back("node " + std::to_string(m.pgn.value())); });
    // on_message() and on_any_message() are one list; a group may have several handlers.
    listener.on_message(Pgn{kPgnPropB}, [&](const Message&) { log.push_back("first"); });
    listener.on_any_message([&](const Message&) { log.push_back("any"); });
    listener.on_message(Pgn{0xFF20}, [&](const Message&) { log.push_back("other group"); });
    listener.on_message(Pgn{kPgnPropB}, [&](const Message&) { log.push_back("second"); });
    listener.on_any_message([&](const Message&) { log.push_back("any again"); });
    listener.on_frame([&](const ReceivedFrame& f) { log.push_back("frame " + std::to_string(f.pgn.value())); });
    listener.on_frame([&](const ReceivedFrame&) { log.push_back("frame again"); });
    bench.step();
    bench.inject(frame(6, kPgnPropB, kA, kGlobal, {1}));
    bench.step();
    const std::string pgn = std::to_string(kPgnPropB);
    CHECK(log == (std::vector<std::string>{"frame " + pgn, "frame again", "observer " + pgn, "first", "any", "second",
                                            "any again", "node " + pgn}));
}

TEST_CASE(listener_order_for_one_frame_is_the_frame_then_its_message_then_the_transfer)
{
    for (const SniffDelivery delivery : {SniffDelivery::WhenAcknowledged, SniffDelivery::WhenAllPacketsSeen})
    {
        ListenerOptions options;
        options.sniffer.delivery = delivery;
        Bench bench(quiet());
        Listener listener(bench.network, options);
        std::vector<std::string> log;
        const auto kind = [](const ReceivedFrame& f) {
            return f.pgn.value() == kPgnTpDt ? "packet " + std::to_string(f.data[0])
                                             : f.pgn.value() == kPgnTpCm ? "control " + std::to_string(f.data[0]) : std::string("other");
        };
        listener.on_frame([&](const ReceivedFrame& f) { log.push_back("frame " + kind(f)); });
        listener.on_any_message([&](const Message& m) { log.push_back("message " + std::to_string(m.data.size())); });
        listener.on_message(Pgn{kPgn}, [&](const Message&) { log.push_back("message again"); });
        listener.on_transfer([&](const SniffedTransfer& t) {
            log.push_back("transfer " + std::to_string(static_cast<int>(t.end)) + " " + std::to_string(t.data.size()));
        });
        listener.on_transfer([&](const SniffedTransfer&) { log.push_back("transfer again"); });
        bench.step();
        inject_connection(bench, at(1ms), kA, kB, payload(14));
        bench.step();
        const std::vector<std::string> ended{"message 14", "message again", "transfer 0 14", "transfer again"};
        std::vector<std::string> expected{"frame control 16", "frame control 17", "frame packet 1", "frame packet 2"};
        if (delivery == SniffDelivery::WhenAllPacketsSeen)
        {
            expected.insert(expected.end(), ended.begin(), ended.end());
            expected.push_back("frame control 19");
        }
        else
        {
            expected.push_back("frame control 19");
            expected.insert(expected.end(), ended.begin(), ended.end());
        }
        CHECK(log == expected);
    }
}

TEST_CASE(listener_decides_what_ended_by_time_at_the_end_of_the_steps_frames_before_the_timers)
{
    // Also: a Timer works on a network without a node.
    Bench bench(quiet());
    Listener listener(bench.network);
    std::vector<std::string> log;
    listener.on_transfer([&](const SniffedTransfer& t) {
        log.push_back("transfer " + std::to_string(static_cast<int>(t.end)) + " " + std::to_string(to_ns(t.time)) + " " +
                      std::to_string(to_ns(t.noticed)));
    });
    listener.on_any_message([&](const Message&) { log.push_back("message"); });
    Timer early(bench.network, TimerKind::Scheduled, [&] { log.push_back("timer 1505 ms"); });
    Timer due(bench.network, TimerKind::Scheduled, [&] { log.push_back("timer 1510 ms"); });
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            early.start_at(at(1505ms));
            due.start_at(at(1510ms));
        }
    };
    bench.step();
    bench.inject(at(5ms), rts(kA, kB, 30, kPgn)); // never answered
    while (bench.next_time() <= at(1500ms))
    {
        bench.step();
    }
    // 1500 ms: the timeout (1250 ms) and the grace (250 ms) after the frame are not over yet.
    CHECK(log.empty());
    CHECK_EQ(listener.open_transfers(), 1u);
    bench.step(); // 1510 ms
    CHECK(log == (std::vector<std::string>{"transfer " + std::to_string(static_cast<int>(SniffEnd::TimedOut)) +
                                               " 1255000000 1510000000",
                                           "timer 1505 ms", "timer 1510 ms"}));
    CHECK_EQ(listener.open_transfers(), 0u);
    CHECK_EQ(bench.network.node_count(), 0u);
}

TEST_CASE(listener_hands_on_a_sniffed_message_when_it_is_acknowledged_with_the_time_of_its_last_packet)
{
    Bench bench(quiet());
    Listener listener(bench.network);
    Recording recording;
    recording.attach(listener);
    bench.step();
    const Bytes message = payload(14);
    bench.inject(at(1ms), rts(kA, kB, 14, kPgn));
    bench.inject(at(2ms), cts(kB, kA, 2, 1, kPgn));
    bench.inject(at(3ms), dts(kA, kB, message, 1, 2).at(0));
    bench.inject(at(4ms), dts(kA, kB, message, 1, 2).at(1));
    bench.inject(at(6ms), frame(6, kPgnPropB, kC, kGlobal, {7})); // after the last packet, before the acknowledgement
    bench.inject(at(8ms), eoma(kB, kA, 14, kPgn));
    bench.step();
    // The later message is handed on first: code that keeps "the latest" compares the times.
    CHECK_EQ(recording.heard.size(), 2u);
    CHECK(recording.heard.at(0).pgn == kPgnPropB && recording.heard.at(0).time == at(6ms));
    CHECK(recording.heard.at(1).pgn == kPgn && recording.heard.at(1).time == at(4ms));
    CHECK(recording.heard.at(1).data == message);
    CHECK(recording.heard.at(1).time < recording.heard.at(0).time);
}

// --- handlers only read ---------------------------------------------------------------------------

namespace {

enum class In
{
    Frame,
    Message,        ///< a single frame, handed on by the network
    SniffedMessage, ///< a connection between others, handed on by the sniffer
    Transfer,
    TimedOutTransfer, ///< decided at the end of the step's frames
};

/// Runs `misuse` once in the handler `in` of a Listener on a network with a node; returns the
/// bench after the step that should have failed.
struct Misuse
{
    Misuse(In in, const std::function<void(Node&, Listener&)>& misuse) : listener(bench.network)
    {
        Node& node = bench.add_node(kTool);
        node.on_any_message([this](const Message&) { ++handled; });
        const auto run = [this, &node, misuse] {
            if (!std::exchange(ran, true))
            {
                misuse(node, listener);
            }
        };
        switch (in)
        {
        case In::Frame:
            listener.on_frame([run](const ReceivedFrame&) { run(); });
            break;
        case In::Message:
        case In::SniffedMessage:
            listener.on_any_message([run](const Message&) { run(); });
            break;
        case In::Transfer:
        case In::TimedOutTransfer:
            listener.on_transfer([run](const SniffedTransfer&) { run(); });
            break;
        }
        bench.app = [this](SimTime) { ++application_steps; };
        bench.step();
        switch (in)
        {
        case In::Frame:
        case In::Message:
            bench.inject(at(1ms), frame(6, kPgnPropB, kA, kGlobal, {1}));
            break;
        case In::SniffedMessage:
        case In::Transfer:
            inject_connection(bench, at(1ms), kA, kB, payload(14));
            break;
        case In::TimedOutTransfer:
            bench.inject(at(1ms), rts(kA, kB, 30, kPgn));
            break;
        }
        threw = throws<std::exception>([this] { bench.steps(160); });
    }

    Bench bench{quiet()};
    Listener listener;
    bool ran = false;
    bool threw = false;
    std::size_t handled = 0;
    std::size_t application_steps = 0;
};

const In kEverywhere[] = {In::Frame, In::Message, In::SniffedMessage, In::Transfer, In::TimedOutTransfer};

} // namespace

TEST_CASE(listener_handler_that_sends_fails_the_network_also_if_it_catches_the_exception)
{
    for (const In in : kEverywhere)
    {
        for (const bool swallow : {false, true})
        {
            bool refused = false;
            Misuse m(in, [&](Node& node, Listener&) {
                try
                {
                    static_cast<void>(node.send(Pgn{kPgnPropB}, Bytes{1}));
                }
                catch (const std::logic_error&)
                {
                    refused = true;
                    if (!swallow)
                    {
                        throw;
                    }
                }
            });
            CHECK(m.ran);
            CHECK(refused);
            CHECK(m.threw);
            CHECK(m.bench.network.failed());
            // The network failed when send() refused; what the handler does with the exception
            // changes nothing.
            CHECK_TEXT(m.bench.network.error(), "vehicle_sil::j1939::Network: send()" + kReadOnly);
            CHECK_EQ(m.handled, 0u); // the step stopped before the node's handlers
            if (in != In::TimedOutTransfer)
            {
                CHECK_EQ(m.application_steps, 1u); // and before the application
            }
            CHECK(m.bench.take_sent().empty());
        }
    }
}

TEST_CASE(listener_handler_that_throws_fails_the_network_and_leaves_no_transfer_half_done)
{
    for (const In in : kEverywhere)
    {
        Misuse m(in, [](Node&, Listener&) { throw std::runtime_error("handler failed"); });
        CHECK(m.ran);
        CHECK(m.threw);
        CHECK(m.bench.network.failed());
        CHECK_TEXT(m.bench.network.error(), std::string("vehicle_sil::j1939::Network: a Listener handler threw: handler failed"));
        // Whatever the handler did, the sniffer finished its frame: the transfer is over and counted.
        const SnifferStatistics& s = m.listener.sniffer_statistics();
        if (in == In::SniffedMessage || in == In::Transfer)
        {
            CHECK_EQ(s.completed, 1u);
            CHECK_EQ(m.listener.open_transfers(), 0u);
        }
        if (in == In::TimedOutTransfer)
        {
            CHECK_EQ(s.timed_out, 1u);
            CHECK_EQ(m.listener.open_transfers(), 0u);
        }
        CHECK_EQ(m.handled, 0u);
    }
}

TEST_CASE(listener_handlers_are_registered_outside_its_handlers)
{
    const std::string text = " was called from a handler of this Listener. Register the handlers before the simulation "
                             "starts (outside a step); a handler only reads what it is given";
    for (const In in : kEverywhere)
    {
        for (const bool swallow : {false, true})
        {
            std::string said;
            std::size_t late = 0;
            Misuse m(in, [&](Node&, Listener& listener) {
                try
                {
                    listener.on_any_message([&](const Message&) { ++late; });
                }
                catch (const std::logic_error& e)
                {
                    said = e.what();
                    if (!swallow)
                    {
                        throw;
                    }
                }
            });
            CHECK_TEXT(said, "vehicle_sil::j1939::Listener: on_any_message()" + text);
            CHECK(m.threw);
            CHECK(m.bench.network.failed());
            CHECK_TEXT(m.bench.network.error(),
                       "vehicle_sil::j1939::Network: a Listener handler threw: vehicle_sil::j1939::Listener: on_any_message()" + text);
            CHECK_EQ(late, 0u);
            CHECK_EQ(m.handled, 0u);
        }
    }
    // Each of the four says its own name.
    Bench bench(quiet());
    Listener listener(bench.network);
    std::vector<std::string> said;
    listener.on_frame([&](const ReceivedFrame&) {
        const std::function<void()> calls[] = {
            [&] { listener.on_message(Pgn{kPgn}, [](const Message&) {}); },
            [&] { listener.on_transfer([](const SniffedTransfer&) {}); },
            [&] { listener.on_frame([](const ReceivedFrame&) {}); },
        };
        for (const auto& call : calls)
        {
            try
            {
                call();
            }
            catch (const std::logic_error& e)
            {
                said.emplace_back(e.what());
            }
        }
    });
    bench.step();
    bench.inject(frame(6, kPgnPropB, kA, kGlobal, {1}));
    CHECK(throws<std::logic_error>([&] { bench.step(); }));
    CHECK_EQ(said.size(), 3u);
    CHECK_TEXT(said.at(0), "vehicle_sil::j1939::Listener: on_message()" + text);
    CHECK_TEXT(said.at(1), "vehicle_sil::j1939::Listener: on_transfer()" + text);
    CHECK_TEXT(said.at(2), "vehicle_sil::j1939::Listener: on_frame()" + text);
}

TEST_CASE(listener_rejects_empty_handlers_and_options_that_cannot_work)
{
    Bench bench(quiet());
    Listener listener(bench.network);
    CHECK(throws_with<std::invalid_argument>([&] { listener.on_message(Pgn{kPgn}, {}); },
                                             "vehicle_sil::j1939::Listener: on_message() got an empty handler. Pass a callable"));
    CHECK(throws_with<std::invalid_argument>([&] { listener.on_any_message({}); }, "on_any_message() got an empty handler"));
    CHECK(throws_with<std::invalid_argument>([&] { listener.on_transfer({}); }, "on_transfer() got an empty handler"));
    CHECK(throws_with<std::invalid_argument>([&] { listener.on_frame({}); }, "on_frame() got an empty handler"));
    // The sniffer's own check; the seat is not taken by a Listener that was not created.
    Bench other(quiet());
    ListenerOptions options;
    options.sniffer.sessions = 0;
    CHECK(throws_with<std::invalid_argument>([&] { Listener bad(other.network, options); }, "SnifferOptions::sessions is 0"));
    Listener good(other.network);
    CHECK(!good.options().hear_local);
    CHECK_EQ(good.options().sniffer.sessions, 32u);
}

// --- lifetime -------------------------------------------------------------------------------------

TEST_CASE(listener_is_one_per_network_and_says_what_to_do_about_a_second)
{
    Bench bench(quiet());
    Listener listener(bench.network);
    CHECK(throws_with<std::logic_error>([&] { Listener second(bench.network); },
                                        "vehicle_sil::j1939::Network: this Network already has a Listener (a Network has one); "
                                        "add the handlers to that Listener"));
    // The first one still listens.
    std::size_t heard = 0;
    listener.on_any_message([&](const Message&) { ++heard; });
    bench.step();
    bench.inject(frame(6, kPgnPropB, kA, kGlobal, {1}));
    bench.step();
    CHECK_EQ(heard, 1u);
    // Not created inside a step.
    std::string said;
    Bench stepping(quiet());
    stepping.app = [&](SimTime) {
        try
        {
            Listener inside(stepping.network);
        }
        catch (const std::logic_error& e)
        {
            said = e.what();
        }
    };
    stepping.step();
    CHECK_TEXT(said, std::string("vehicle_sil::j1939::Network: a Listener is only added or removed outside a step "
                                 "(create it before the simulation starts, next to the nodes)"));
    CHECK(!stepping.network.failed());
}

TEST_CASE(listener_and_network_can_be_destroyed_in_either_order_and_a_new_listener_hears_again)
{
    {
        Bench bench(quiet());
        std::size_t heard = 0;
        std::size_t handled = 0;
        Node& node = bench.add_node(kTool);
        node.on_any_message([&](const Message&) { ++handled; });
        auto listener = std::make_unique<Listener>(bench.network);
        listener->on_any_message([&](const Message&) { ++heard; });
        bench.step();
        bench.inject(frame(6, kPgnPropB, kA, kGlobal, {1}));
        bench.step();
        CHECK_EQ(heard, 1u);
        listener.reset(); // the listener first
        bench.inject(frame(6, kPgnPropB, kA, kGlobal, {2}));
        bench.step();
        CHECK_EQ(heard, 1u);
        CHECK_EQ(handled, 2u);
        // A new one: with its own handlers, and it hears again.
        std::size_t heardAgain = 0;
        Listener again(bench.network);
        again.on_any_message([&](const Message&) { ++heardAgain; });
        inject_connection(bench, bench.last_time() + 1ms, kA, kB, payload(14));
        bench.step();
        CHECK_EQ(heard, 1u);
        CHECK_EQ(heardAgain, 1u);
        CHECK(!bench.network.failed());
    }
    {
        // The network first.
        auto bench = std::make_unique<Bench>(quiet());
        auto listener = std::make_unique<Listener>(bench->network);
        bench->step();
        bench.reset();
        CHECK_EQ(listener->open_transfers(), 0u);
        listener.reset();
    }
}

TEST_CASE(listener_can_be_destroyed_from_a_nodes_handler)
{
    Bench bench(quiet());
    std::size_t heard = 0;
    std::size_t transfers = 0;
    auto listener = std::make_unique<Listener>(bench.network);
    listener->on_any_message([&](const Message&) { ++heard; });
    listener->on_transfer([&](const SniffedTransfer&) { ++transfers; });
    Node& node = bench.add_node(kTool);
    node.on_message(Pgn{kPgnPropB}, [&](const Message&) { listener.reset(); });
    bench.step();
    bench.inject(at(1ms), rts(kA, kB, 30, kPgn)); // followed when the listener goes
    bench.inject(at(2ms), frame(6, kPgnPropB, kA, kGlobal, {1}));
    bench.inject(at(3ms), frame(6, kPgnPropB, kA, kGlobal, {2}));
    bench.step();
    CHECK(listener == nullptr);
    CHECK_EQ(heard, 1u); // the message whose handler destroyed it; nothing after
    bench.steps(200);    // no step end reaches it either
    CHECK_EQ(transfers, 0u);
    CHECK(!bench.network.failed());
}

TEST_CASE(listener_and_the_test_kits_recorder_listen_to_the_same_network)
{
    Bench bench(quiet());
    vehicle_sil::testkit::J1939Events events(bench.network);
    Listener listener(bench.network);
    Recording recording;
    recording.attach(listener);
    bench.add_node(kTool);
    bench.step();
    bench.inject(at(1ms), frame(6, kPgnPropB, kA, kGlobal, {1}));
    bench.inject(at(2ms), bam(kA, 14, 0xFF31));
    bench.inject(at(3ms), dts(kA, kGlobal, payload(14), 1, 2).at(0));
    bench.inject(at(4ms), dts(kA, kGlobal, payload(14), 1, 2).at(1));
    bench.inject(at(5ms), rts(kB, kTool, 14, kPgn)); // to the node: the node answers
    bench.step();
    bench.inject(at(11ms), dts(kB, kTool, payload(14, 1), 1, 2).at(0));
    bench.inject(at(12ms), dts(kB, kTool, payload(14, 1), 1, 2).at(1));
    bench.step();
    // Whatever the network hands to its nodes, the recorder and the listener both get.
    CHECK_EQ(events.count(EventKind::MessageReceived), 3u);
    CHECK_EQ(recording.heard.size(), 3u);
    CHECK(recording.ended.empty());
    CHECK(!bench.network.failed());
}
