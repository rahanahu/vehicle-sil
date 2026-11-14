// SPDX-License-Identifier: MIT
//
// Ecu::ethernet(): channels of raw Ethernet frames on the local virtual network and on SIL Kit.
// A frame sent in a step reaches every other channel of the network in that ECU's next step, in
// the same order on both runtimes; describe() reports the address; the refusals say what to do.
#include "test_harness.hpp"

#include "vehicle_sil/local/local_bench.hpp"
#include "vehicle_sil/simulation/simulation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::Ecu;
using vehicle_sil::SilKitOptions;
using vehicle_sil::SimTime;
using vehicle_sil::Simulation;
namespace eth = vehicle_sil::eth;

constexpr eth::MacAddress mac(std::uint8_t last) { return eth::MacAddress{{0x02, 0x00, 0x00, 0x00, 0x00, last}}; }

long long ms_of(SimTime t) { return static_cast<long long>(t.time_since_epoch() / 1ms); }

/// A short frame (the networks pad it): `to`, `from`, an EtherType of the local experimental
/// range, and `seq`.
eth::Frame frame(const eth::MacAddress& to, const eth::MacAddress& from, std::uint8_t seq)
{
    std::array<std::uint8_t, eth::kHeaderBytes + 1> bytes{};
    std::copy(to.bytes.begin(), to.bytes.end(), bytes.begin());
    std::copy(from.bytes.begin(), from.bytes.end(), bytes.begin() + 6);
    bytes[12] = 0x88;
    bytes[13] = 0xB5;
    bytes[14] = seq;
    return *eth::Frame::from(bytes);
}

/// "<step ms> from <xx> to <xx> seq <n> sent <ms>"
std::string line(SimTime now, const eth::TimedFrame& f)
{
    char text[96];
    std::snprintf(text, sizeof text, "%lld from %02x to %02x seq %d sent %lld", ms_of(now),
                  static_cast<unsigned>(f.frame.source().bytes[5]), static_cast<unsigned>(f.frame.destination().bytes[5]),
                  static_cast<int>(f.frame.bytes()[14]), ms_of(f.time));
    return text;
}

/// Sends a broadcast frame with a running number in every step; its handler keeps what arrives,
/// and the step writes it down with the step's time.
class Talker
{
public:
    Talker(Ecu& ecu, std::uint8_t address) : channel(ecu.ethernet("ETH", {.mac = mac(address)}))
    {
        channel.on_frame([this](const eth::TimedFrame& f) { arrived_.push_back(f); });
        ecu.on_step([this, address](SimTime now) {
            for (const auto& f : arrived_)
            {
                log.push_back(line(now, f));
            }
            arrived_.clear();
            static_cast<void>(channel.send(frame(eth::kBroadcast, mac(address), seq_++)));
        });
    }
    eth::Channel& channel;
    std::vector<std::string> log;

private:
    std::vector<eth::TimedFrame> arrived_;
    std::uint8_t seq_ = 0;
};

/// Takes the frames of a step with receive() and answers each to its sender.
class Echo
{
public:
    explicit Echo(Ecu& ecu) : channel(ecu.ethernet("ETH", {.mac = mac(0x02)}))
    {
        ecu.on_step([this](SimTime now) {
            while (const auto f = channel.receive())
            {
                log.push_back(line(now, *f));
                static_cast<void>(channel.send(frame(f->frame.source(), mac(0x02), f->frame.bytes()[14])));
            }
        });
    }
    eth::Channel& channel;
    std::vector<std::string> log;
};

/// Hears only what is addressed to it and the broadcasts.
class Listener
{
public:
    explicit Listener(Ecu& ecu) :
      channel(ecu.ethernet("ETH", {.mac = mac(0x04), .accept = eth::Accept::Addressed}))
    {
        channel.on_frame([this](const eth::TimedFrame& f) { from.push_back(f.frame.source().bytes[5]); });
    }
    eth::Channel& channel;
    std::vector<int> from;
};

struct Records
{
    std::vector<std::string> first;
    std::vector<std::string> echo;
    std::vector<int> listener;
    std::uint64_t filtered = 0;
    std::uint64_t echo_sent = 0;
};

Records run(std::optional<SilKitOptions> silkit)
{
    Simulation sim(silkit);
    // The talker with the higher address first: the order of arrival is not the order added.
    sim.add<Talker>("Second", 10ms, std::uint8_t{0x03});
    auto& echo = sim.add<Echo>("Echo", 5ms);
    auto& first = sim.add<Talker>("First", 10ms, std::uint8_t{0x01});
    auto& listener = sim.add<Listener>("Listener", 10ms);
    sim.run_for(30ms);
    return {first.log, echo.log, listener.from, listener.channel.statistics().frames_filtered,
            echo.channel.statistics().frames_sent};
}

std::string joined(const std::vector<std::string>& lines)
{
    std::string out;
    for (const auto& l : lines)
    {
        out += l + "\n";
    }
    return out;
}

void check_text(const std::string& actual, const std::string& expected)
{
    if (actual != expected)
    {
        throw vehicle_sil::unit::Failure("expected\n" + expected + "but got\n" + actual);
    }
}

void check(const Records& r)
{
    // The echo (step 5 ms) sees what the talkers sent at t in its first step after t, the lower
    // source address first, and answers in that step.
    check_text(joined(r.echo), "5 from 01 to ff seq 0 sent 0\n"
                               "5 from 03 to ff seq 0 sent 0\n"
                               "15 from 01 to ff seq 1 sent 10\n"
                               "15 from 03 to ff seq 1 sent 10\n"
                               "25 from 01 to ff seq 2 sent 20\n"
                               "25 from 03 to ff seq 2 sent 20\n");
    // A talker sees everything on the network (a hub), also the answer to the other talker, in the
    // order of the time sent and, for one sender and time, the order sent.
    check_text(joined(r.first), "10 from 03 to ff seq 0 sent 0\n"
                                "10 from 02 to 01 seq 0 sent 5\n"
                                "10 from 02 to 03 seq 0 sent 5\n"
                                "20 from 03 to ff seq 1 sent 10\n"
                                "20 from 02 to 01 seq 1 sent 15\n"
                                "20 from 02 to 03 seq 1 sent 15\n");
    CHECK((r.listener == std::vector<int>{0x01, 0x03, 0x01, 0x03}));
    CHECK_EQ(r.filtered, std::uint64_t{4}); // the answers of 5 and 15 ms
    CHECK_EQ(r.echo_sent, std::uint64_t{6});
}

std::string refusal(const std::function<void()>& fn)
{
    try
    {
        fn();
    }
    catch (const std::invalid_argument& e)
    {
        return e.what();
    }
    catch (const std::logic_error& e)
    {
        return std::string("logic_error: ") + e.what();
    }
    return "(nothing thrown)";
}

std::optional<SilKitOptions> on(bool silkit)
{
    return silkit ? std::optional<SilKitOptions>(SilKitOptions{}) : std::nullopt;
}

} // namespace

TEST_CASE(ethernet_frames_arrive_in_the_next_step_in_the_same_order_locally_and_on_silkit)
{
    check(run(std::nullopt));
    for (int i = 0; i < 3; ++i)
    {
        check(run(SilKitOptions{}));
    }
}

TEST_CASE(ethernet_is_described_with_its_address)
{
    for (const bool silkit : {false, true})
    {
        Simulation sim(on(silkit));
        Ecu& ecu = sim.add_ecu("A", 5ms);
        static_cast<void>(ecu.ethernet("ETH", {.mac = mac(0x0A)}));
        static_cast<void>(ecu.can("CAN"));
        static_cast<void>(ecu.ethernet("ETH2"));
        const vehicle_sil::EcuDescription d = ecu.describe();
        CHECK_EQ(d.networks.size(), std::size_t{3});
        CHECK(d.networks[0].name == "ETH" && d.networks[0].use == vehicle_sil::NetworkUse::Ethernet);
        CHECK(d.networks[0].kind() == vehicle_sil::NetworkKind::Ethernet);
        CHECK(d.networks[0].mac == mac(0x0A));
        CHECK(d.networks[1].name == "CAN" && !d.networks[1].mac);
        CHECK(d.networks[2].name == "ETH2" && !d.networks[2].mac);
        const std::string json = to_json(d);
        CHECK(json.find("\"use\": \"ethernet\",\n      \"nodes\": [],\n      \"mac\": \"02:00:00:00:00:0a\"") != std::string::npos);
    }
}

TEST_CASE(ethernet_refusals_say_what_is_wrong_and_what_to_do)
{
    // SIL Kit: the peer's and the simulation's checks.
    {
        Simulation sim(SilKitOptions{});
        Ecu& a = sim.add_ecu("A", 5ms);
        Ecu& b = sim.add_ecu("B", 5ms);
        a.j1939("CAN1");
        static_cast<void>(a.ethernet("ETH"));
        check_text(refusal([&] { static_cast<void>(a.ethernet("ETH")); }),
                   "vehicle_sil::Peer: ethernet(\"ETH\"): the peer has an Ethernet channel on \"ETH\" already; an ECU "
                   "has one channel per Ethernet network (keep the reference the first call returned)");
        check_text(refusal([&] { static_cast<void>(a.ethernet("CAN1")); }),
                   "vehicle_sil::Peer: ethernet(\"CAN1\"): \"CAN1\" is a CAN network of this peer; an Ethernet network "
                   "needs a name of its own");
        check_text(refusal([&] { static_cast<void>(a.can("ETH")); }),
                   "vehicle_sil::Peer: can(\"ETH\"): \"ETH\" is an Ethernet network of this peer; a CAN network needs "
                   "a name of its own");
        check_text(refusal([&] { static_cast<void>(b.ethernet("CAN1")); }),
                   "vehicle_sil::Simulation: ethernet(\"CAN1\") of the ECU \"B\": \"CAN1\" is a CAN network of the ECU "
                   "\"A\" (made by j1939() or can()); an Ethernet network needs a name of its own");
        check_text(refusal([&] { b.j1939("ETH"); }),
                   "vehicle_sil::Simulation: j1939(\"ETH\") of the ECU \"B\": \"ETH\" is an Ethernet network of the ECU "
                   "\"A\" (made by ethernet()); a CAN network needs a name of its own");
        // Options that the channel refuses leave nothing behind: the name can be used again.
        check_text(refusal([&] { static_cast<void>(b.ethernet("ETH", {.accept = eth::Accept::Addressed})); }),
                   "vehicle_sil::eth::Channel: Accept::Addressed needs the channel's own address (mac)");
        auto& channel = b.ethernet("ETH", {.mac = mac(0x0B), .accept = eth::Accept::Addressed});
        check_text(refusal([&] { static_cast<void>(channel.send(frame(eth::kBroadcast, mac(0x0B), 0))); }),
                   "logic_error: vehicle_sil::eth::Channel: send() is only allowed inside a step (a handler or the "
                   "application stage)");
        sim.run_for(20ms);
        CHECK(refusal([&] { static_cast<void>(b.ethernet("ETH3")); }).rfind("logic_error: ", 0) == 0);
    }
    // Locally: the bench's.
    {
        Simulation sim;
        Ecu& a = sim.add_ecu("A", 5ms);
        Ecu& b = sim.add_ecu("B", 5ms);
        a.j1939("CAN1");
        static_cast<void>(a.ethernet("ETH"));
        check_text(refusal([&] { static_cast<void>(a.ethernet("ETH")); }),
                   "vehicle_sil::LocalBench: ethernet(\"ETH\"): the ECU \"A\" has an Ethernet channel on \"ETH\" "
                   "already; an ECU has one channel per Ethernet network (keep the reference the first call returned)");
        check_text(refusal([&] { static_cast<void>(b.ethernet("CAN1")); }),
                   "vehicle_sil::LocalBench: ethernet(\"CAN1\") of the ECU \"B\": \"CAN1\" is a CAN bus (made by j1939() "
                   "or can()); an Ethernet bus needs a name of its own");
        check_text(refusal([&] { b.j1939("ETH"); }),
                   "vehicle_sil::LocalBench: j1939(\"ETH\") of the ECU \"B\": \"ETH\" is an Ethernet bus (made by "
                   "ethernet()); a CAN bus needs a name of its own");
        check_text(refusal([&] { static_cast<void>(b.ethernet("NEW", {.max_frame_bytes = 4000})); }),
                   "vehicle_sil::eth::Channel: max_frame_bytes must be from 14 to 2048, not 4000");
        static_cast<void>(b.lin_slave("NEW")); // no Ethernet network "NEW" was made
        static_cast<void>(b.ethernet("ETH", {.mac = mac(0x0B)}));
        sim.run_for(20ms);
        CHECK(refusal([&] { static_cast<void>(b.ethernet("ETH3")); }).rfind("logic_error: ", 0) == 0);
    }
}

TEST_CASE(ethernet_of_a_local_bench_is_found_by_name_and_takes_frames_of_a_test)
{
    vehicle_sil::LocalBench bench;
    Ecu& ecu = bench.add_ecu("A", 5ms);
    auto& channel = ecu.ethernet("ETH", {.mac = mac(0x0A)});
    std::vector<long long> heard;
    channel.on_frame([&](const eth::TimedFrame& f) { heard.push_back(ms_of(f.time)); });
    ecu.at(SimTime{} + 10ms, [&](SimTime) { static_cast<void>(channel.send(frame(eth::kBroadcast, mac(0x0A), 7))); });
    bench.add_ecu("B", 5ms).j1939("CAN1");
    CHECK(&bench.ethernet("A", "ETH") == &channel);
    // An endpoint of the test on the ECU's network: it sends to the ECU and sees what the ECU sends.
    auto& probe = bench.ethernet_bus("ETH").attach();
    CHECK(probe.send({SimTime{}, frame(mac(0x0A), mac(0x0F), 1)}) == eth::SendResult::Accepted);
    bench.run_for(20ms);
    CHECK((heard == std::vector<long long>{0}));
    std::vector<std::string> seen;
    probe.release_before(SimTime{} + 20ms, [&](const eth::TimedFrame& f) { seen.push_back(line(SimTime{} + 20ms, f)); });
    check_text(joined(seen), "20 from 0a to ff seq 7 sent 10\n");

    const auto out_of_range = [](const std::function<void()>& fn) -> std::string {
        try
        {
            fn();
        }
        catch (const std::out_of_range& e)
        {
            return e.what();
        }
        return "(nothing thrown)";
    };
    check_text(out_of_range([&] { static_cast<void>(bench.ethernet("B", "ETH")); }),
               "vehicle_sil::LocalBench: the ECU \"B\" has no Ethernet channel on \"ETH\". Its Ethernet channels: none "
               "(Ecu::ethernet(network) makes one)");
    check_text(out_of_range([&] { static_cast<void>(bench.ethernet_bus("CAN1")); }),
               "vehicle_sil::LocalBench: ethernet_bus(\"CAN1\"): no Ethernet network named \"CAN1\". Ethernet networks: "
               "\"ETH\" (an ECU's ethernet(network) makes one); \"CAN1\" is a CAN bus");
}
