// SPDX-License-Identifier: MIT
//
// The part of examples/pid_stream (pid_stream.hpp), where it refuses work: a Start from a new
// client beyond max_streams, or when the network has too few timers free for a stream (with a
// lease: 2), is ignored and the network goes on; a lease ends a stream from its own timer and
// gives its timers back; bad options are refused when the part is made.
#include "j1939_bench.hpp"
#include "pid_stream.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::j1939::Node;
using vehicle_sil::unit::throws;

namespace {

constexpr Address kEcu = 0x00;

/// Start {50 ms, id 1} from `client` to the ECU, in the default layout.
J1939Frame start_from(Address client) { return frame(6, pid_stream::kPgn.value(), client, kEcu, {0x01, 50, 0, 1}); }

/// The clients the ECU sent replies to (Bench::take_sent() order, each once).
std::vector<Address> replied_to(Bench& bench)
{
    std::vector<Address> out;
    for (const auto& f : bench.take_sent())
    {
        if (f.pgn == pid_stream::kPgn.value() && (out.empty() || out.back() != f.da))
        {
            out.push_back(f.da);
        }
    }
    return out;
}

pid_stream::PidTable table() { return {{1, [] { return std::uint16_t{7}; }}}; }

} // namespace

TEST_CASE(pid_streamer_serves_up_to_max_streams_and_a_lease_gives_the_timers_back)
{
    vehicle_sil::j1939::NetworkOptions options;
    options.timer_capacity = 8;
    Bench bench(options);
    Node& ecu = bench.add_node(kEcu);
    auto& streamer = ecu.attach<pid_stream::PidStreamer>(
        table(), pid_stream::StreamerOptions{.max_streams = 2, .lease = std::chrono::milliseconds(200)});
    for (const Address client : {0x80, 0x81, 0x82})
    {
        bench.inject(start_from(client));
    }
    bench.steps(5); // 0 .. 40 ms
    CHECK(!bench.network.failed());
    CHECK_EQ(streamer.streams(), 2u);
    CHECK_EQ(bench.network.timers_free(), 4u); // 2 streams, a sender and a lease each
    CHECK((replied_to(bench) == std::vector<Address>{0x80, 0x81}));
    bench.steps(20); // .. 240 ms: the leases end (200 ms after the Starts)
    CHECK(!bench.network.failed());
    CHECK_EQ(streamer.streams(), 0u);
    CHECK_EQ(bench.network.timers_free(), 8u);
}

TEST_CASE(pid_streamer_refuses_a_new_client_when_too_few_timers_are_free)
{
    // 3 timers: one stream with a lease takes 2; the second would need 2 more.
    vehicle_sil::j1939::NetworkOptions options;
    options.timer_capacity = 3;
    Bench bench(options);
    Node& ecu = bench.add_node(kEcu);
    auto& streamer =
        ecu.attach<pid_stream::PidStreamer>(table(), pid_stream::StreamerOptions{.lease = std::chrono::milliseconds(200)});
    bench.inject(start_from(0x80));
    bench.inject(start_from(0x81));
    bench.steps(5);
    CHECK(!bench.network.failed());
    CHECK_EQ(streamer.streams(), 1u);
    CHECK_EQ(bench.network.timers_free(), 1u);
    CHECK((replied_to(bench) == std::vector<Address>{0x80}));
}

TEST_CASE(pid_streamer_refuses_bad_options_when_it_is_made)
{
    Bench bench;
    Node& ecu = bench.add_node(kEcu);
    pid_stream::StreamerOptions noPeriod;
    noPeriod.min_period = std::chrono::milliseconds(0);
    CHECK(throws<std::invalid_argument>([&] { pid_stream::PidStreamer s(ecu, table(), noPeriod); }));
    pid_stream::StreamerOptions tooLong;
    tooLong.format.max_reply = vehicle_sil::j1939::kMaxTransportLength + 1;
    CHECK(throws<std::invalid_argument>([&] { pid_stream::PidStreamer s(ecu, table(), tooLong); }));
    pid_stream::StreamerOptions empty;
    empty.format.max_reply = 0;
    CHECK(throws<std::invalid_argument>([&] { pid_stream::PidStreamer s(ecu, table(), empty); }));
}
