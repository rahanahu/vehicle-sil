// SPDX-License-Identifier: MIT
//
// vehicle_sil::spy::Spy::watch<M>(): a typed message (generated or written by hand) published as
// variables, on an ECU of a LocalBench. Also the example of the guide (guide_example.hpp).
#include "codec_test_messages.hpp"
#include "guide_example.hpp"
#include "hand_written_messages.hpp"
#include "spy_rig.hpp"

#include "vehicle_sil/j1939/codec.hpp"
#include "vehicle_sil/spy/spy.hpp"
#include "vehicle_sil/spy/watch.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

using namespace vehicle_sil::unit::spy;
using vehicle_sil::Ecu;
using vehicle_sil::LocalBench;
using vehicle_sil::SimTime;
using vehicle_sil::control::ValueType;
using vehicle_sil::spy::Spy;
using vehicle_sil::spy::WatchOptions;
using vehicle_sil::unit::spy::hand_written::FirstByte;
using vehicle_sil::unit::spy::hand_written::Reading;
using vehicle_sil::unit::spy::hand_written::Wide;
using vehicle_sil::unit::spy::hand_written::WithStaleCount;
namespace codec = vehicle_sil::j1939::codec;

namespace {

constexpr Address kA = 0x21;
constexpr Address kB = 0x22;
constexpr Address kC = 0x23;

template <typename M>
Bytes bytes_of(const M& message)
{
    const auto bytes = codec::message_bytes(message);
    return Bytes(bytes.begin(), bytes.end());
}

Reading reading(double level, double code)
{
    Reading r;
    r.level.set(level);
    r.code.set(code);
    return r;
}

double real_of(Ecu& ecu, std::string_view name) { return std::get<double>(value_of(ecu, name)); }
bool bool_of(Ecu& ecu, std::string_view name) { return std::get<bool>(value_of(ecu, name)); }

Spy& make_spy(Rig& rig) { return rig.ecu.emplace<Spy>(rig.ecu, rig.network); }

} // namespace

TEST_CASE(watch_publishes_each_signal_and_when_the_message_came)
{
    Rig rig;
    rig.ecu.set_agent_options({.sample_period = std::chrono::milliseconds(0)});
    make_spy(rig).watch<codec_test::Status>("st");
    rig.bench.run_until(at(10ms) + 1ns);
    // Nothing yet.
    CHECK_EQ(integer_of(rig.ecu, "st.count"), 0);
    CHECK_EQ(integer_of(rig.ecu, "st.last_ms"), -1);
    CHECK_EQ(integer_of(rig.ecu, "st.period_ms"), -1);
    CHECK_EQ(integer_of(rig.ecu, "st.size"), 0);
    CHECK_EQ(integer_of(rig.ecu, "st.speed.state"), -1);
    CHECK(real_of(rig.ecu, "st.speed") == 0.0);

    codec_test::Status s;
    s.speed.set(1000.0);
    s.temperature.set(20.0);
    s.mode.set(codec_test::Status::Mode::on);
    s.level.set(2.5);
    s.torque.set(-3.25);
    s.counter.set(7.0);
    s.flag.set(1.0);
    rig.inject(at(13ms), frame(3, kPgnPropB, kA, kGlobal, bytes_of(s)));
    s.speed.set(1500.0);
    rig.inject(at(24ms), frame(3, kPgnPropB, kA, kGlobal, bytes_of(s)));
    rig.bench.run_until(at(30ms) + 1ns);

    CHECK(real_of(rig.ecu, "st.speed") == 1500.0);
    CHECK_EQ(integer_of(rig.ecu, "st.temperature"), 20); // scale 1, whole offset: an integer
    CHECK_EQ(integer_of(rig.ecu, "st.mode"), 1);          // an enumeration: its raw value
    CHECK(real_of(rig.ecu, "st.level") == 2.5);
    CHECK(real_of(rig.ecu, "st.torque") == -3.25);
    CHECK_EQ(integer_of(rig.ecu, "st.counter"), 7);
    CHECK_EQ(integer_of(rig.ecu, "st.flag"), 1);
    CHECK_EQ(integer_of(rig.ecu, "st.count"), 2);
    CHECK_EQ(integer_of(rig.ecu, "st.last_ms"), 24);
    CHECK_EQ(integer_of(rig.ecu, "st.period_ms"), 11);
    CHECK_EQ(integer_of(rig.ecu, "st.size"), 8);
    CHECK_EQ(integer_of(rig.ecu, "st.invalid"), 0);
    // A state for the signals that can hold a value that is not valid, only.
    for (const char* name : {"st.speed.state", "st.temperature.state", "st.mode.state", "st.level.state"})
    {
        CHECK_EQ(integer_of(rig.ecu, name), 0);
        const auto info = info_of(rig.ecu, name);
        CHECK(info.type == ValueType::Enum);
        CHECK_EQ(info.items.size(), std::size_t{7});
    }
    for (const char* name : {"st.torque.state", "st.counter.state", "st.flag.state", "st.stale", "st.stale_count"})
    {
        CHECK(!has_variable(rig.ecu, name));
    }
    // Types, units and descriptions from the definition.
    CHECK(info_of(rig.ecu, "st.speed").type == ValueType::Real);
    CHECK(info_of(rig.ecu, "st.speed").unit == "rpm");
    CHECK(info_of(rig.ecu, "st.speed").description.find("SPN 4242") != std::string::npos);
    CHECK(info_of(rig.ecu, "st.temperature").type == ValueType::Integer);
    CHECK(info_of(rig.ecu, "st.temperature").unit == "\302\260C");
    CHECK(info_of(rig.ecu, "st.mode").type == ValueType::Integer);
    CHECK(info_of(rig.ecu, "st.last_ms").unit == "ms");
    CHECK(info_of(rig.ecu, "st.size").unit == "bytes");
    // A message that does not decode (shorter than the type) is not counted.
    rig.inject(at(33ms), frame(3, kPgnPropB, kA, kGlobal, {1, 2, 3}));
    rig.bench.run_until(at(40ms) + 1ns);
    CHECK_EQ(integer_of(rig.ecu, "st.count"), 2);
    CHECK_EQ(rig.network.statistics().frames_sent, 0u);
}

TEST_CASE(watch_keeps_the_latest_valid_value_and_counts_what_was_not_valid)
{
    Rig rig;
    make_spy(rig).watch<codec_test::Status>("st");
    codec_test::Status s;
    s.speed.set(1000.0);
    s.temperature.set(20.0);
    s.level.set(2.5);
    rig.inject(at(1ms), frame(3, kPgnPropB, kA, kGlobal, bytes_of(s)));
    rig.bench.run_until(at(10ms) + 1ns);
    s.speed.set_not_available();
    s.temperature.set_error();
    s.level.set_raw(12); // above the valid range (max 5, raw 10), below the special values
    rig.inject(at(11ms), frame(3, kPgnPropB, kA, kGlobal, bytes_of(s)));
    rig.bench.run_until(at(20ms) + 1ns);
    CHECK(real_of(rig.ecu, "st.speed") == 1000.0);
    CHECK_EQ(integer_of(rig.ecu, "st.speed.state"), 3); // not_available
    CHECK_EQ(integer_of(rig.ecu, "st.temperature"), 20);
    CHECK_EQ(integer_of(rig.ecu, "st.temperature.state"), 2); // error
    CHECK(real_of(rig.ecu, "st.level") == 2.5);
    CHECK_EQ(integer_of(rig.ecu, "st.level.state"), 1); // out_of_range
    CHECK_EQ(integer_of(rig.ecu, "st.mode.state"), 3);  // not_available: a new message's default
    CHECK_EQ(integer_of(rig.ecu, "st.count"), 2);
    CHECK_EQ(integer_of(rig.ecu, "st.invalid"), 2); // the first one too: mode was not available
    const auto items = info_of(rig.ecu, "st.speed.state").items;
    const auto value_of_item = [&](const char* name) {
        for (const auto& item : items)
        {
            if (item.name == name)
            {
                return item.value;
            }
        }
        throw vehicle_sil::unit::Failure(std::string("no item ") + name);
    };
    CHECK_EQ(value_of_item("none"), -1);
    CHECK_EQ(value_of_item("valid"), 0);
    CHECK_EQ(value_of_item("out_of_range"), 1);
    CHECK_EQ(value_of_item("error"), 2);
    CHECK_EQ(value_of_item("not_available"), 3);
    CHECK_EQ(value_of_item("special"), 4);
    CHECK_EQ(value_of_item("undefined"), 5);
    // Valid again: the value follows.
    s.speed.set(1250.0);
    rig.inject(at(21ms), frame(3, kPgnPropB, kA, kGlobal, bytes_of(s)));
    rig.bench.run_until(at(30ms) + 1ns);
    CHECK(real_of(rig.ecu, "st.speed") == 1250.0);
    CHECK_EQ(integer_of(rig.ecu, "st.speed.state"), 0);
}

TEST_CASE(watch_takes_only_the_messages_from_and_to_the_given_addresses)
{
    Rig rig;
    Spy& spy = make_spy(rig);
    auto& filtered = spy.watch<Reading>("r", {.from = kA, .to = kB});
    spy.watch<Reading>("any");
    spy.watch<FirstByte>("first"); // no selector: every message of the group
    rig.inject(at(1ms), frame(6, kPgnPropA, kA, kB, bytes_of(reading(10.0, 1))));
    rig.inject(at(2ms), frame(6, kPgnPropA, kC, kB, bytes_of(reading(20.0, 2)))); // another source
    rig.inject(at(3ms), frame(6, kPgnPropA, kA, kC, bytes_of(reading(30.0, 3)))); // another destination
    Bytes other = bytes_of(reading(40.0, 4));
    other[0] = 0x11; // the selector of another message of the group
    rig.inject(at(4ms), frame(6, kPgnPropA, kA, kB, other));
    rig.inject(at(5ms), frame(6, kPgnPropA, kA, kB, {0x10, 1, 2})); // too short for Reading
    rig.bench.run_until(at(10ms) + 1ns);
    CHECK_EQ(integer_of(rig.ecu, "r.count"), 1);
    CHECK(real_of(rig.ecu, "r.level") == 10.0);
    CHECK_EQ(integer_of(rig.ecu, "r.code"), 1);
    CHECK_EQ(filtered.count(), 1);
    CHECK(filtered.latest().has_value() && filtered.latest()->code.raw() == 1u);
    CHECK_EQ(integer_of(rig.ecu, "any.count"), 3);
    CHECK(real_of(rig.ecu, "any.level") == 30.0);
    CHECK_EQ(integer_of(rig.ecu, "first.count"), 5);
    CHECK_EQ(integer_of(rig.ecu, "first.first"), 0x10);
    CHECK_EQ(integer_of(rig.ecu, "first.size"), 3);
}

TEST_CASE(watch_counts_an_older_message_and_keeps_the_latest)
{
    // The message of a connection has the time of its last packet and is handed on after the
    // receiver's acknowledgement: here after a single frame that came later.
    Rig rig;
    make_spy(rig).watch<Reading>("r");
    Bytes long_one = bytes_of(reading(20.0, 2));
    long_one.resize(20, 0xAA);
    rig.inject(at(1ms), frame(6, kPgnPropA, kA, kB, bytes_of(reading(5.0, 5))));
    rig.inject(at(21ms), rts(kA, kB, 20, kPgnPropA));
    rig.inject(at(22ms), cts(kB, kA, 3, 1, kPgnPropA));
    SimTime t = at(22ms);
    for (const auto& f : dts(kA, kB, long_one, 1, 3))
    {
        rig.inject(t += 1ms, f); // the last one at 25 ms
    }
    rig.inject(at(25ms) + 500us, frame(6, kPgnPropA, kA, kB, bytes_of(reading(10.0, 1))));
    rig.inject(at(26ms), eoma(kB, kA, 20, kPgnPropA));
    rig.bench.run_until(at(40ms) + 1ns);
    CHECK_EQ(integer_of(rig.ecu, "r.count"), 3);
    CHECK(real_of(rig.ecu, "r.level") == 10.0);
    CHECK_EQ(integer_of(rig.ecu, "r.code"), 1);
    CHECK_EQ(integer_of(rig.ecu, "r.size"), 8);
    CHECK_EQ(integer_of(rig.ecu, "r.last_ms"), 25);
    CHECK_EQ(integer_of(rig.ecu, "r.period_ms"), 24);
    // The next one in time order counts from the latest.
    rig.inject(at(45ms), frame(6, kPgnPropA, kA, kB, bytes_of(reading(20.0, 2))));
    rig.bench.run_until(at(50ms) + 1ns);
    CHECK(real_of(rig.ecu, "r.level") == 20.0);
    CHECK_EQ(integer_of(rig.ecu, "r.period_ms"), 19);
    CHECK_EQ(integer_of(rig.ecu, "r.size"), 8); // a single frame has 8 bytes
}

TEST_CASE(watch_takes_the_later_of_two_messages_with_the_same_time)
{
    // One sender's step: both frames have its step time.
    Rig rig;
    make_spy(rig).watch<Reading>("r");
    rig.inject(at(3ms), frame(6, kPgnPropA, kA, kB, bytes_of(reading(5.0, 5))));
    rig.inject(at(3ms), frame(6, kPgnPropA, kA, kB, bytes_of(reading(10.0, 1))));
    rig.bench.run_until(at(10ms) + 1ns);
    CHECK_EQ(integer_of(rig.ecu, "r.count"), 2);
    CHECK(real_of(rig.ecu, "r.level") == 10.0);
    CHECK_EQ(integer_of(rig.ecu, "r.code"), 1);
    CHECK_EQ(integer_of(rig.ecu, "r.period_ms"), 0);
}

// Scale 1 and a whole offset: an integer while every valid value fits std::int64_t.
namespace wide_signals {
template <std::uint8_t kLength, bool kSigned, std::uint64_t kRawMin, std::uint64_t kRawMax, double kOffset>
struct Spec
{
    static constexpr codec::SignalLayout layout{
        .length = kLength, .is_signed = kSigned, .offset = kOffset, .raw_min = kRawMin, .raw_max = kRawMax};
    static constexpr std::string_view name{"s"};
    static constexpr std::string_view unit{""};
};
template <std::uint8_t kLength, bool kSigned, std::uint64_t kRawMin, std::uint64_t kRawMax, double kOffset>
constexpr bool integer = vehicle_sil::spy::detail::is_integer_signal<
    codec::Signal<Spec<kLength, kSigned, kRawMin, kRawMax, kOffset>>>();
constexpr std::uint64_t kInt64Max = 0x7FFF'FFFF'FFFF'FFFF;
constexpr std::uint64_t kInt64Min = 0x8000'0000'0000'0000; // as a bit pattern
static_assert(integer<32, false, 0, 0xFFFF'FFFF, -1000.0>);
static_assert(integer<64, false, 0, kInt64Max, 0.0>);
static_assert(!integer<64, false, 0, kInt64Max, 1.0>);
static_assert(!integer<64, false, 0, kInt64Max + 1, 0.0>);
static_assert(integer<64, false, 0, kInt64Max - 5, 5.0>);
static_assert(integer<64, true, kInt64Min, kInt64Max, 0.0>);
static_assert(!integer<64, true, kInt64Min, kInt64Max, -1.0>);
static_assert(!integer<64, true, kInt64Min, kInt64Max, 1.0>);
static_assert(integer<64, true, kInt64Min + 1, kInt64Max, -1.0>);
} // namespace wide_signals

TEST_CASE(watch_publishes_a_64_bit_counter_beyond_int64_as_a_real_number)
{
    Rig rig;
    make_spy(rig).watch<Wide>("w");
    Wide w;
    w.total.set(1.0e19); // above 2^63 - 1: as a std::int64_t it would turn negative
    rig.inject(at(3ms), frame(6, kPgnPropB, kA, kGlobal, bytes_of(w)));
    rig.bench.run_until(at(10ms) + 1ns);
    CHECK(info_of(rig.ecu, "w.total").type == ValueType::Real);
    CHECK(real_of(rig.ecu, "w.total") == 1.0e19);
}

TEST_CASE(watch_stale_turns_on_when_nothing_came_for_longer_than_stale_after)
{
    Rig rig; // a step of 10 ms
    rig.ecu.set_agent_options({.sample_period = std::chrono::milliseconds(0)});
    make_spy(rig).watch<Reading>("r", {.stale_after = 50ms});
    const auto stale_at = [&](SimTime step) {
        rig.bench.run_until(step + 1ns);
        return bool_of(rig.ecu, "r.stale");
    };
    CHECK(!stale_at(at(50ms))); // before the first message, counted from the time 0
    CHECK_EQ(integer_of(rig.ecu, "r.stale_count"), 0);
    CHECK(stale_at(at(60ms)));
    CHECK_EQ(integer_of(rig.ecu, "r.stale_count"), 1);
    rig.inject(at(70ms), frame(6, kPgnPropA, kA, kB, bytes_of(reading(1.0, 1)))); // taken in the step at 80 ms
    CHECK(stale_at(at(70ms)));
    CHECK_EQ(integer_of(rig.ecu, "r.stale_count"), 1); // stays 1 while continuously stale
    CHECK(!stale_at(at(80ms)));
    CHECK_EQ(integer_of(rig.ecu, "r.stale_count"), 1); // recovers without decrementing
    CHECK(!stale_at(at(120ms))); // exactly 50 ms
    CHECK_EQ(integer_of(rig.ecu, "r.stale_count"), 1);
    CHECK(stale_at(at(130ms)));
    CHECK_EQ(integer_of(rig.ecu, "r.stale_count"), 2); // second false -> true transition
    CHECK(stale_at(at(140ms)));
    CHECK_EQ(integer_of(rig.ecu, "r.stale_count"), 2);
    CHECK(info_of(rig.ecu, "r.stale").type == ValueType::Bool);
    CHECK(info_of(rig.ecu, "r.stale_count").type == ValueType::Integer);
}

TEST_CASE(watch_errors_say_what_is_wrong_and_how_to_fix_it)
{
    {
        Rig rig;
        Spy& spy = make_spy(rig);
        CHECK_MESSAGE(message_of<std::invalid_argument>([&] { spy.watch<Reading>("r", {.stale_after = 0ms}); }),
                      "vehicle_sil::spy::Spy::watch() \"r\": WatchOptions::stale_after is 0 ns: give a time above 0, "
                      "or leave it out for no \"r.stale\"");
        CHECK(!has_variable(rig.ecu, "r.count"));
        spy.watch<Reading>("r");
        CHECK_MESSAGE(message_of<std::invalid_argument>([&] { spy.watch<FirstByte>("r"); }),
                      "vehicle_sil::spy::Spy::watch() \"r\": the name is used already, by an earlier watch(): give "
                      "each watcher a name of its own");
    }
    {
        // A variable of the watch that the ECU has already: the watch publishes none, and the
        // ECU runs.
        Rig rig;
        std::int64_t other = 0;
        rig.ecu.output("r.count", &other);
        Spy& spy = make_spy(rig);
        CHECK_MESSAGE(message_of<std::invalid_argument>([&] { spy.watch<Reading>("r"); }),
                      "vehicle_sil::spy::Spy::watch() \"r\": the ECU has the variable \"r.count\" already, and the "
                      "watch publishes it too: give the watch another name, or rename the other variable (the watch "
                      "published nothing)");
        for (const char* name : {"r.level", "r.level.state", "r.code"})
        {
            CHECK(!has_variable(rig.ecu, name));
        }
        rig.bench.run_until(at(10ms) + 1ns);
        CHECK_EQ(integer_of(rig.ecu, "r.count"), 0);
    }
    {
        // Pre-existing stale_count blocks a watch with stale_after atomically.
        Rig rig;
        std::int64_t other = 0;
        rig.ecu.output("r.stale_count", &other);
        Spy& spy = make_spy(rig);
        CHECK_MESSAGE(message_of<std::invalid_argument>([&] { spy.watch<Reading>("r", {.stale_after = 50ms}); }),
                      "vehicle_sil::spy::Spy::watch() \"r\": the ECU has the variable \"r.stale_count\" already, and "
                      "the watch publishes it too: give the watch another name, or rename the other variable (the "
                      "watch published nothing)");
        CHECK(!has_variable(rig.ecu, "r.stale"));
        CHECK(!has_variable(rig.ecu, "r.count"));
    }
    {
        Rig rig;
        Spy& spy = make_spy(rig);
        rig.bench.run_until(at(10ms) + 1ns);
        CHECK_MESSAGE(message_of<std::logic_error>([&] { spy.watch<Reading>("r"); }),
                      "vehicle_sil::spy::Spy::watch() after the run started: add the watchers in the ECU class's "
                      "constructor");
    }
    // The names a signal may not have (a build error: j1939_spy.watch_of_a_signal_named_count_does_not_build).
    for (const char* item : {"count", "last_ms", "period_ms", "size", "invalid", "stale", "state"})
    {
        CHECK(vehicle_sil::spy::detail::is_watch_item(item));
    }
    CHECK(!vehicle_sil::spy::detail::is_watch_item("stale_count"));
    CHECK(!vehicle_sil::spy::detail::is_watch_item("speed"));
}

TEST_CASE(watch_allows_signal_named_stale_count_only_without_stale_after)
{
    {
        // Without stale_after: compiles, publishes the message's own signal as "<as>.stale_count".
        Rig rig;
        rig.ecu.set_agent_options({.sample_period = std::chrono::milliseconds(0)});
        make_spy(rig).watch<WithStaleCount>("s");
        WithStaleCount m;
        m.value.set(42.0);
        m.stale_count.set(7.0);
        rig.inject(at(3ms), frame(6, kPgnPropA, kA, kB, bytes_of(m)));
        rig.bench.run_until(at(10ms) + 1ns);
        CHECK_EQ(integer_of(rig.ecu, "s.value"), 42);
        CHECK_EQ(integer_of(rig.ecu, "s.stale_count"), 7);
        CHECK_EQ(integer_of(rig.ecu, "s.count"), 1);
        CHECK(!has_variable(rig.ecu, "s.stale"));
    }
    {
        // With stale_after: throws std::invalid_argument atomically before publishing any variable.
        Rig rig;
        rig.ecu.set_agent_options({.sample_period = std::chrono::milliseconds(0)});
        Spy& spy = make_spy(rig);
        CHECK_MESSAGE(
            message_of<std::invalid_argument>([&] { spy.watch<WithStaleCount>("s", {.stale_after = 50ms}); }),
            "vehicle_sil::spy::Spy::watch() \"s\": a signal of M is named \"stale_count\", which "
            "WatchOptions::stale_after publishes too: rename the signal in M's definition, or leave out "
            "WatchOptions::stale_after (the watch published nothing)");
        for (const char* name : {"s.value", "s.stale_count", "s.stale", "s.count"})
        {
            CHECK(!has_variable(rig.ecu, name));
        }
        // The failed watch published nothing and did not reserve the name, so watching without stale_after succeeds.
        spy.watch<WithStaleCount>("s");
        WithStaleCount m;
        m.value.set(11.0);
        m.stale_count.set(3.0);
        rig.inject(at(3ms), frame(6, kPgnPropA, kA, kB, bytes_of(m)));
        rig.bench.run_until(at(10ms) + 1ns);
        CHECK_EQ(integer_of(rig.ecu, "s.value"), 11);
        CHECK_EQ(integer_of(rig.ecu, "s.stale_count"), 3);
    }
}

TEST_CASE(guide_example_publishes_what_it_hears)
{
    LocalBench bench;
    Ecu& ecu = bench.add_ecu("CalSpy", 5ms);
    ecu.set_agent_options({.sample_period = std::chrono::milliseconds(0)}); // sample every step
    ecu.emplace<CalSpy>(ecu);
    const auto inject = [&](SimTime t, const J1939Frame& f) {
        CHECK(bench.inject(kBus, t, to_can(f)) == vehicle_sil::can::SendResult::Accepted);
    };
    cal::Write write;
    write.address.set(0x0102);
    write.value.set(12.5);
    inject(at(1ms), frame(6, kPgnPropA, 0xF9, 0x21, bytes_of(write)));
    inject(at(2ms), frame(6, kPgnPropA, 0xF9, 0x22, bytes_of(write))); // to another ECU: not counted
    cal::WriteAnswer answer;
    answer.address.set(0x0102);
    answer.result.set(cal::WriteAnswer::Result::ok);
    inject(at(4ms), frame(6, kPgnPropA, 0x21, 0xF9, bytes_of(answer)));
    // A connection from 0x21, 20 bytes: a block.
    const Bytes message = payload(20);
    SimTime t = at(10ms);
    inject(t, rts(0x21, 0x22, 20, kPgnPropA));
    inject(t += 1ms, cts(0x22, 0x21, 3, 1, kPgnPropA));
    for (const auto& f : dts(0x21, 0x22, message, 1, 3))
    {
        inject(t += 1ms, f);
    }
    inject(t += 1ms, eoma(0x22, 0x21, 20, kPgnPropA));
    bench.run_until(at(50ms));
    CHECK_EQ(integer_of(ecu, "write.count"), 1);
    CHECK_EQ(integer_of(ecu, "write.address"), 0x0102);
    CHECK(std::get<double>(value_of(ecu, "write.value")) == 12.5);
    CHECK(info_of(ecu, "write.value").unit == "V");
    CHECK_EQ(integer_of(ecu, "answer.count"), 1);
    CHECK_EQ(integer_of(ecu, "answer.result"), 0);
    CHECK_EQ(integer_of(ecu, "answer.result.state"), 0);
    CHECK(!std::get<bool>(value_of(ecu, "answer.stale")));
    CHECK_EQ(integer_of(ecu, "answer.stale_count"), 0);
    CHECK_EQ(integer_of(ecu, "block.count"), 2); // the answer and the connection
    CHECK_EQ(integer_of(ecu, "block.bytes"), 20);
    CHECK(info_of(ecu, "block.bytes").unit == "bytes");
    CHECK_EQ(integer_of(ecu, "spy.missed"), 0);
    bench.run_until(at(510ms));
    CHECK(std::get<bool>(value_of(ecu, "answer.stale")));
    CHECK_EQ(integer_of(ecu, "answer.stale_count"), 1);
}
