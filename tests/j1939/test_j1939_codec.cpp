// SPDX-License-Identifier: MIT
//
// Typed J1939 messages (vehicle_sil::j1939_codec) generated from codec/codec_test_messages.yaml:
// bit layout against a bit-by-bit reference, raw and physical values, rounding and limits, the
// error and "not available" values, enumerations, selectors, lengths, and round trips over every
// raw value of the small signals.

#include "codec_test_messages.hpp"
#include "codec_tricky_names.hpp"
#include "test_harness.hpp"
#include "vehicle_sil/j1939/codec.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace codec_test;
using vehicle_sil::j1939::Pgn;
using vehicle_sil::j1939::codec::ByteOrder;
using vehicle_sil::j1939::codec::SignalLayout;
using vehicle_sil::j1939::codec::SignalState;
using vehicle_sil::unit::throws;
namespace codec = vehicle_sil::j1939::codec;

namespace {

using Bytes = std::vector<std::uint8_t>;

// --- A bit-by-bit reference of the layout (written from the definition in codec.hpp) ---------

std::vector<std::uint32_t> reference_positions(const SignalLayout& layout)
{
    // Most significant bit first.
    std::vector<std::uint32_t> out;
    if (layout.byte_order == ByteOrder::LittleEndian)
    {
        for (int i = layout.length - 1; i >= 0; --i)
        {
            out.push_back(layout.start_bit + static_cast<std::uint32_t>(i));
        }
        return out;
    }
    std::uint32_t position = layout.start_bit;
    for (int i = 0; i < layout.length; ++i)
    {
        out.push_back(position);
        position = position % 8 != 0 ? position - 1 : position + 15;
    }
    return out;
}

bool get_bit(const Bytes& data, std::uint32_t position) { return ((data[position / 8] >> (position % 8)) & 1u) != 0; }

void set_bit(Bytes& data, std::uint32_t position, bool value)
{
    const auto mask = static_cast<std::uint8_t>(1u << (position % 8));
    data[position / 8] = static_cast<std::uint8_t>(value ? data[position / 8] | mask : data[position / 8] & ~mask);
}

std::uint64_t reference_extract(const Bytes& data, const SignalLayout& layout)
{
    std::uint64_t value = 0;
    for (const auto position : reference_positions(layout))
    {
        value = (value << 1) | (get_bit(data, position) ? 1u : 0u);
    }
    return value;
}

void reference_insert(Bytes& data, const SignalLayout& layout, std::uint64_t value)
{
    const auto positions = reference_positions(layout);
    for (std::size_t i = 0; i < positions.size(); ++i)
    {
        set_bit(data, positions[i], ((value >> (positions.size() - 1 - i)) & 1u) != 0);
    }
}

/// Status with every signal set to a valid value.
Status sample_status()
{
    Status s;
    CHECK(s.speed.set(1000.0));
    CHECK(s.temperature.set(25.0));
    s.mode.set(Status::Mode::on);
    CHECK(s.level.set(2.5));
    CHECK(s.torque.set(-100.25));
    s.counter.set_raw(0x2AB);
    s.flag.set_raw(1);
    return s;
}

template <typename M>
M round_trip(const M& message)
{
    const auto bytes = message.bytes();
    const auto decoded = M::decode(bytes);
    CHECK(decoded.has_value());
    return *decoded;
}

/// Every raw value of every signal of `message` up to 16 bits (all of them) and the ends and
/// special values of the longer ones: set alone, encoded, decoded. The other signals keep
/// their values and the bits no signal covers keep the fill.
template <typename M>
void check_every_raw_value(const M& base)
{
    const auto base_bytes = base.bytes();
    std::size_t signal_index = 0;
    M probe = base;
    probe.for_each_signal([&](auto& signal) {
        using S = std::remove_cvref_t<decltype(signal)>;
        using Raw = typename S::raw_type;
        std::vector<Raw> raws;
        if constexpr (S::layout.length <= 16)
        {
            for (auto raw = S::lowest_raw();; ++raw)
            {
                raws.push_back(raw);
                if (raw == S::highest_raw())
                {
                    break;
                }
            }
        }
        else
        {
            raws = {S::lowest_raw(), S::highest_raw(), S::raw_min, S::raw_max, Raw{0}, Raw{1}};
            for (const auto& special : S::specials)
            {
                raws.push_back(static_cast<Raw>(special.first));
                raws.push_back(static_cast<Raw>(special.last));
            }
        }
        const S original = signal;
        for (const Raw raw : raws)
        {
            signal.set_raw(raw);
            CHECK(signal.raw() == raw);
            const auto bytes = probe.bytes();
            const auto decoded = M::decode(bytes);
            CHECK(decoded.has_value());
            CHECK(*decoded == probe);
            // Only this signal's bits changed.
            Bytes expected(base_bytes.begin(), base_bytes.end());
            reference_insert(expected, S::layout, static_cast<std::uint64_t>(raw) & codec::low_bits(S::layout.length));
            CHECK(Bytes(bytes.begin(), bytes.end()) == expected);
        }
        signal = original;
        ++signal_index;
    });
    CHECK(probe == base);
    CHECK(signal_index > 0);
}

template <typename S>
concept CanSetNotAvailable = requires(S s) { s.set_not_available(); };
template <typename S>
concept CanSetError = requires(S s) { s.set_error(); };

} // namespace

// --- The layout -------------------------------------------------------------------------------

TEST_CASE(insert_and_extract_match_the_bit_by_bit_reference)
{
    std::mt19937_64 random(20261108); // fixed seed; only the engine's output (portable) is used
    for (int round = 0; round < 20000; ++round)
    {
        SignalLayout layout;
        layout.length = static_cast<std::uint8_t>(1 + random() % 64);
        layout.byte_order = random() % 2 == 0 ? ByteOrder::LittleEndian : ByteOrder::BigEndian;
        layout.start_bit = static_cast<std::uint32_t>(random() % (12 * 8));
        Bytes data(24);
        for (auto& b : data)
        {
            b = static_cast<std::uint8_t>(random());
        }
        if (codec::end_byte(layout) > data.size())
        {
            continue;
        }
        // end_byte() is one past the last byte the reference touches.
        std::uint32_t last = 0;
        for (const auto p : reference_positions(layout))
        {
            last = std::max(last, p);
        }
        CHECK_EQ(codec::end_byte(layout), static_cast<std::size_t>(last / 8 + 1));

        CHECK_EQ(codec::extract(data, layout), reference_extract(data, layout));
        const std::uint64_t value = random() & codec::low_bits(layout.length);
        Bytes expected = data;
        reference_insert(expected, layout, value);
        codec::insert(data, layout, value | ~codec::low_bits(layout.length)); // extra high bits are ignored
        CHECK(data == expected);
        CHECK_EQ(codec::extract(data, layout), value);
    }
}

TEST_CASE(sign_extension)
{
    CHECK_EQ(codec::sign_extend(0x7FF, 12), std::int64_t{2047});
    CHECK_EQ(codec::sign_extend(0x800, 12), std::int64_t{-2048});
    CHECK_EQ(codec::sign_extend(0xFFF, 12), std::int64_t{-1});
    CHECK_EQ(codec::sign_extend(0x1, 1), std::int64_t{-1});
    CHECK_EQ(codec::sign_extend(~std::uint64_t{0}, 64), std::int64_t{-1});
    CHECK_EQ(codec::sign_extend(0x8000000000000000ull, 64), std::numeric_limits<std::int64_t>::min());
}

// --- The generated types ----------------------------------------------------------------------

TEST_CASE(generated_constants)
{
    static_assert(Status::kPgn == Pgn{0xFF10});
    static_assert(Status::kPriority == 3);
    static_assert(Status::kLength == 8);
    static_assert(Wide::kPgn == Pgn{0x1FF20});
    static_assert(Wide::kLength == 20 && Wide::kFill == 0x00);
    static_assert(SetPoint::kPgn == Pgn{0xEF00} && Reset::kPgn == Pgn{0xEF00});
    static_assert(SetPoint::kPriority == 6);
    static_assert(Empty::kPgn == Pgn{0x1FF11} && Empty::kLength == 1);
    static_assert(codec::MessageType<Status> && codec::MessageType<Empty>);

    static_assert(decltype(Status::speed)::spn == 4242u);
    static_assert(!decltype(Status::temperature)::spn.has_value());
    static_assert(decltype(Status::speed)::name == "speed");
    static_assert(decltype(Status::speed)::unit == "rpm");
    static_assert(decltype(Status::temperature)::unit == "°C");
    static_assert(std::is_same_v<decltype(Status::torque)::raw_type, std::int64_t>);
    static_assert(std::is_same_v<decltype(Status::speed)::raw_type, std::uint64_t>);

    // No silent conversions: a signal is not a number, and the special values exist only where
    // the definition has them.
    static_assert(!std::is_convertible_v<decltype(Status::speed), double>);
    static_assert(!std::is_assignable_v<decltype(Status::speed)&, double>);
    static_assert(CanSetNotAvailable<decltype(Status::speed)> && CanSetError<decltype(Status::speed)>);
    static_assert(!CanSetNotAvailable<decltype(Status::torque)> && !CanSetError<decltype(Status::torque)>);
    static_assert(!CanSetNotAvailable<decltype(Status::flag)>);
    static_assert(CanSetNotAvailable<decltype(Wide::delta)> && CanSetError<decltype(Wide::delta)>);
}

TEST_CASE(a_new_message_is_not_available_and_filled)
{
    const Status s;
    CHECK(s.speed.state() == SignalState::NotAvailable);
    CHECK(!s.speed.value().has_value());
    CHECK_EQ(s.speed.value_or(-1.0), -1.0);
    CHECK(s.mode.state() == SignalState::NotAvailable);
    CHECK(!s.mode.value().has_value());
    CHECK(s.torque.state() == SignalState::Valid); // no "not available" value: raw 0
    CHECK_EQ(s.torque.raw(), std::int64_t{0});
    // speed FF FF, temperature FF, mode 3 + level F + 2 fill bits, torque 000 + flag 0 + 3 fill
    // bits (byte 5: bits 44 flag, 45..47 fill), counter 0 (byte 6 bits 0..3, byte 7 bits 2..7).
    const std::array<std::uint8_t, 8> expected{0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xE0, 0xF0, 0x03};
    CHECK(s.bytes() == expected);

    const Wide w; // fill 0x00
    const auto wb = w.bytes();
    CHECK_EQ(wb[0], 0xFF); // distance: not available
    CHECK_EQ(wb[3], 0xFF);
    CHECK_EQ(wb[4], 0x00); // not in a signal: fill 0x00
    CHECK_EQ(wb[8], 0x00);  // total: raw 0
    CHECK_EQ(wb[16], 0xFF); // delta: 0x7FFFFF
    CHECK_EQ(wb[18], 0x7F);
    CHECK_EQ(wb[19], 0xFF); // custom: not available 0xF0 .. 0xFF, sends 0xFF
}

TEST_CASE(encodes_the_bytes_of_the_definition)
{
    const Status s = sample_status();
    // speed 1000 rpm / 0.125 = 8000 = 0x1F40; temperature 25 + 40 = 65 = 0x41; mode on (1),
    // level 2.5 / 0.5 = 5 at bits 26..29: byte 3 = 0b11 0101 01 = 0xD5; torque -100.25 / 0.25 =
    // -401 = 0xE6F (12 bits): byte 4 = 0x6F, byte 5 low nibble 0xE; flag 1 at bit 44 and fill at
    // 45..47: byte 5 = 0xF0 | 0x1E... = 0b1111 1110 = 0xFE; counter 0x2AB = 10 1010 1011: byte 6
    // bits 3..0 = 1010 (+ fill 1111 above) = 0xFA, byte 7 bits 7..2 = 101011, bits 1..0 fill =
    // 0b1010 1111 = 0xAF.
    const std::array<std::uint8_t, 8> expected{0x40, 0x1F, 0x41, 0xD5, 0x6F, 0xFE, 0xFA, 0xAF};
    CHECK(s.bytes() == expected);
    std::array<std::uint8_t, 12> buffer{};
    buffer.fill(0x55);
    CHECK_EQ(s.encode(buffer), std::size_t{8});
    CHECK(std::equal(expected.begin(), expected.end(), buffer.begin()));
    CHECK_EQ(buffer[8], 0x55); // nothing after the message

    const auto d = Status::decode(expected);
    CHECK(d.has_value());
    CHECK(*d == s);
    CHECK(d->speed.value() == std::optional<double>(1000.0));
    CHECK(d->temperature.value() == std::optional<double>(25.0));
    CHECK(d->mode.value() == std::optional<Status::Mode>(Status::Mode::on));
    CHECK(d->level.value() == std::optional<double>(2.5));
    CHECK(d->torque.value() == std::optional<double>(-100.25));
    CHECK_EQ(d->torque.raw(), std::int64_t{-401});
    CHECK_EQ(d->counter.raw(), std::uint64_t{0x2AB});
    CHECK_EQ(d->flag.raw(), std::uint64_t{1});
}

TEST_CASE(decode_needs_the_whole_message_and_ignores_more)
{
    const auto bytes = sample_status().bytes();
    CHECK(!Status::decode(std::span(bytes).first(7)).has_value());
    CHECK(!Status::decode({}).has_value());
    std::array<std::uint8_t, 9> longer{};
    std::copy(bytes.begin(), bytes.end(), longer.begin());
    longer[8] = 0x12;
    CHECK(Status::decode(longer) == std::optional<Status>(sample_status()));
    CHECK(Empty::decode(std::array<std::uint8_t, 1>{0x00}).has_value());
}

TEST_CASE(encode_needs_a_buffer_of_the_message_length)
{
    const Wide w;
    std::array<std::uint8_t, 8> small{};
    CHECK(throws<std::length_error>([&] { (void)w.encode(small); }));
    std::array<std::uint8_t, 20> enough{};
    CHECK_EQ(w.encode(enough), std::size_t{20});
}

// --- Values -----------------------------------------------------------------------------------

TEST_CASE(physical_values_round_to_the_nearest_raw_value)
{
    Status s;
    using Speed = decltype(Status::speed);
    static_assert(Speed::resolution() == 0.125);
    CHECK_EQ(Speed::min(), 0.0);
    CHECK_EQ(Speed::max(), 0xFAFF * 0.125);
    CHECK(s.speed.set(0.0625)); // 0.5 raw: halves away from zero
    CHECK_EQ(s.speed.raw(), std::uint64_t{1});
    CHECK(s.speed.set(0.06));
    CHECK_EQ(s.speed.raw(), std::uint64_t{0});
    CHECK(s.speed.set(1000.07));
    CHECK_EQ(s.speed.raw(), std::uint64_t{8001});
    CHECK(s.torque.set(-0.125)); // -0.5 raw
    CHECK_EQ(s.torque.raw(), std::int64_t{-1});
    CHECK(s.torque.set(0.125));
    CHECK_EQ(s.torque.raw(), std::int64_t{1});
    CHECK(s.temperature.set(-40.0));
    CHECK_EQ(s.temperature.raw(), std::uint64_t{0});
    CHECK(s.temperature.value() == std::optional<double>(-40.0));
    CHECK(s.level.set(5.2)); // rounds to the end of the range: still exact
    CHECK_EQ(s.level.raw(), std::uint64_t{10});
}

TEST_CASE(values_outside_the_range_go_to_the_nearest_end)
{
    Status s;
    CHECK(!s.speed.set(1e9));
    CHECK_EQ(s.speed.raw(), std::uint64_t{0xFAFF});
    CHECK(s.speed.valid());
    CHECK(!s.speed.set(-1.0));
    CHECK_EQ(s.speed.raw(), std::uint64_t{0});
    CHECK(!s.level.set(5.3)); // max 5 (raw 10)
    CHECK_EQ(s.level.raw(), std::uint64_t{10});
    CHECK(!s.torque.set(1e300));
    CHECK_EQ(s.torque.raw(), std::int64_t{2047});
    CHECK(!s.torque.set(-std::numeric_limits<double>::infinity()));
    CHECK_EQ(s.torque.raw(), std::int64_t{-2048});
    CHECK(!s.temperature.set(-41.0));
    CHECK_EQ(s.temperature.raw(), std::uint64_t{0});

    Wide w;
    CHECK(w.total.set(18446744073709551615.0)); // 2^64 as a double: the end
    CHECK_EQ(w.total.raw(), ~std::uint64_t{0});
    CHECK(!w.total.set(1e30));
    CHECK_EQ(w.total.raw(), ~std::uint64_t{0});
    CHECK(!w.total.set(-0.6));
    CHECK_EQ(w.total.raw(), std::uint64_t{0});
    CHECK(w.custom.set(10.0));
    CHECK(!w.custom.set(9.0));
    CHECK_EQ(w.custom.raw(), std::uint64_t{10});
}

TEST_CASE(nan_makes_a_signal_not_available)
{
    Status s = sample_status();
    CHECK(!s.speed.set(std::nan("")));
    CHECK(s.speed.state() == SignalState::NotAvailable);
    CHECK_EQ(s.speed.raw(), std::uint64_t{0xFFFF});
    const auto before = s.torque.raw();
    CHECK(!s.torque.set(std::nan(""))); // no such value: unchanged
    CHECK_EQ(s.torque.raw(), before);
}

TEST_CASE(states_of_the_j1939_indicators)
{
    Status s;
    const auto state_of = [&](std::uint64_t raw) {
        s.speed.set_raw(raw);
        return s.speed.state();
    };
    CHECK(state_of(0x0000) == SignalState::Valid);
    CHECK(state_of(0xFAFF) == SignalState::Valid);
    CHECK(state_of(0xFB00) == SignalState::Special);
    CHECK(s.speed.special() == std::optional<std::string_view>("parameter_specific"));
    CHECK(state_of(0xFC00) == SignalState::Special);
    CHECK(s.speed.special() == std::optional<std::string_view>("reserved"));
    CHECK(state_of(0xFDFF) == SignalState::Special);
    CHECK(s.speed.raw() == 0xFDFF); // the raw value stays
    CHECK(state_of(0xFE00) == SignalState::Error);
    CHECK(state_of(0xFEFF) == SignalState::Error);
    CHECK(state_of(0xFF00) == SignalState::NotAvailable);
    CHECK(state_of(0xFFFF) == SignalState::NotAvailable);
    s.speed.set_raw(0xFE80);
    CHECK(!s.speed.value().has_value());
    s.speed.set_error();
    CHECK_EQ(s.speed.raw(), std::uint64_t{0xFE00});
    s.speed.set_not_available();
    CHECK_EQ(s.speed.raw(), std::uint64_t{0xFFFF});

    // 8 bits: valid up to 0xFA; level (4 bits): its own special values, 14 error, 15 not
    // available, and max 5; 2 bits: 0 and 1, 2 error, 3 not available; 32 bits.
    s.temperature.set_raw(0xFA);
    CHECK(s.temperature.state() == SignalState::Valid);
    s.temperature.set_raw(0xFB);
    CHECK(s.temperature.state() == SignalState::Special);
    s.temperature.set_error();
    CHECK_EQ(s.temperature.raw(), std::uint64_t{0xFE});
    s.level.set_raw(11);
    CHECK(s.level.state() == SignalState::OutOfRange);
    s.level.set_raw(14);
    CHECK(s.level.state() == SignalState::Error);
    s.level.set_raw(15);
    CHECK(s.level.state() == SignalState::NotAvailable);
    s.mode.set_raw(2);
    CHECK(s.mode.state() == SignalState::Error);
    CHECK(!s.mode.value().has_value());
    CHECK(s.mode.value_or(Status::Mode::off) == Status::Mode::off);
    Wide w;
    w.distance.set_raw(0xFAFFFFFF);
    CHECK(w.distance.valid());
    w.distance.set_raw(0xFB000000);
    CHECK(w.distance.state() == SignalState::Special);
    w.distance.set_error();
    CHECK_EQ(w.distance.raw(), std::uint64_t{0xFE000000});
    w.distance.set_raw(0xFEFFFFFF);
    CHECK(w.distance.state() == SignalState::Error);
    w.distance.set_raw(0xFF000000);
    CHECK(w.distance.state() == SignalState::NotAvailable);
}

TEST_CASE(explicit_ranges_and_signed_signals)
{
    Wide w;
    // custom: valid 10 .. 200, error 0xE0 .. 0xEF, not available 0xF0 .. 0xFF.
    for (std::uint64_t raw = 0; raw <= 0xFF; ++raw)
    {
        w.custom.set_raw(raw);
        const auto expected = raw >= 0xF0   ? SignalState::NotAvailable
                              : raw >= 0xE0 ? SignalState::Error
                              : (raw >= 10 && raw <= 200) ? SignalState::Valid
                                                           : SignalState::OutOfRange;
        CHECK(w.custom.state() == expected);
    }
    // delta: 24-bit signed, not available 0x7FFFFF, error 0x7FFFFE (the bit patterns).
    w.delta.set_raw(-1);
    CHECK(w.delta.valid());
    CHECK_EQ(w.delta.bits(), std::uint64_t{0xFFFFFF});
    w.delta.set_raw(0x7FFFFF);
    CHECK(w.delta.state() == SignalState::NotAvailable);
    w.delta.set_error();
    CHECK_EQ(w.delta.raw(), std::int64_t{0x7FFFFE});
    // The explicit special values sit at the top: the valid range stops below them, so a physical
    // value never turns into one.
    static_assert(decltype(Wide::delta)::raw_max == 0x7FFFFD);
    CHECK(!w.delta.set(1e300));
    CHECK_EQ(w.delta.raw(), std::int64_t{0x7FFFFD});
    CHECK(w.delta.valid());
    CHECK_EQ(decltype(Wide::delta)::max(), double{0x7FFFFD});
    w.delta.set_raw(-0x800000);
    CHECK(w.delta.valid());
    CHECK_EQ(round_trip(w).delta.raw(), std::int64_t{-0x800000});
}

TEST_CASE(set_raw_rejects_values_that_do_not_fit)
{
    Status s;
    CHECK(throws<std::out_of_range>([&] { s.speed.set_raw(0x10000); }));
    CHECK(throws<std::out_of_range>([&] { s.torque.set_raw(2048); }));
    CHECK(throws<std::out_of_range>([&] { s.torque.set_raw(-2049); }));
    CHECK(throws<std::out_of_range>([&] { s.flag.set_raw(2); }));
    s.torque.set_raw(-2048);
    s.torque.set_raw(2047);
    Wide w;
    w.total.set_raw(~std::uint64_t{0});
    CHECK_EQ(round_trip(w).total.raw(), ~std::uint64_t{0});
}

TEST_CASE(enumerations)
{
    Status s;
    s.mode.set(Status::Mode::off);
    CHECK(s.mode.value() == std::optional<Status::Mode>(Status::Mode::off));
    CHECK(round_trip(s).mode.value() == std::optional<Status::Mode>(Status::Mode::off));

    Reset r; // valid 0, 1 and 3; 2 is not an enumerator
    CHECK(r.what.state() == SignalState::NotAvailable);
    r.what.set(Reset::What::counters);
    CHECK_EQ(r.what.raw(), std::uint64_t{3});
    CHECK(r.what.valid());
    r.what.set_raw(2);
    CHECK(r.what.state() == SignalState::Undefined); // in the range, but no enumerator
    CHECK(!r.what.value().has_value());
    r.what.set_raw(4);
    CHECK(r.what.state() == SignalState::Undefined); // above the enumerators too
    r.what.set_raw(0xFE);
    CHECK(r.what.state() == SignalState::Error);
}

TEST_CASE(selectors_tell_messages_of_one_pgn_apart)
{
    SetPoint p;
    CHECK(p.target.set(-12.5));
    const auto pb = p.bytes();
    CHECK_EQ(pb[0], 0x01);
    CHECK_EQ(pb[1] | (pb[2] << 8), 31875); // (-12.5 + 3200) / 0.1
    CHECK(SetPoint::decode(pb) == std::optional<SetPoint>(p));
    CHECK(!Reset::decode(pb).has_value());

    Reset r;
    r.what.set(Reset::What::faults);
    const auto rb = r.bytes();
    CHECK(rb == (std::array<std::uint8_t, 3>{0x02, 0x01, 0xCC})); // the selector, `what`, the fill bytes
    CHECK(Reset::decode(rb).has_value());
    CHECK(!SetPoint::decode(std::array<std::uint8_t, 8>{0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}).has_value());
    // SetPoint's range from min/max: -100 .. 100.
    CHECK(!p.target.set(150.0));
    // 33000 * 0.1 - 3200: the product is rounded before the sum on every compiler (a fused
    // multiply-add would give 100.00000000000018).
    CHECK(p.target.value() == std::optional<double>(100.0));
    CHECK_EQ(decltype(SetPoint::target)::max(), 100.0);
    CHECK_EQ(decltype(SetPoint::target)::min(), -100.0);
}

TEST_CASE(every_raw_value_round_trips)
{
    check_every_raw_value(Status{});
    check_every_raw_value(sample_status());
    check_every_raw_value(Wide{});
    check_every_raw_value(SetPoint{});
    check_every_raw_value(Reset{});
}

TEST_CASE(every_valid_raw_value_round_trips_through_its_physical_value)
{
    Status s;
    s.for_each_signal([&](auto& signal) {
        using S = std::remove_cvref_t<decltype(signal)>;
        if constexpr (requires { S::to_physical(S::raw_min); })
        {
            for (auto raw = S::raw_min;; ++raw)
            {
                signal.set_raw(raw);
                const auto physical = signal.value();
                CHECK(physical.has_value());
                CHECK_EQ(*physical, S::to_physical(raw));
                S other;
                CHECK(other.set(*physical));
                CHECK(other.raw() == raw);
                if (raw == S::raw_max)
                {
                    break;
                }
            }
        }
    });
    // SetPoint: offset and a scale that is not a power of two.
    SetPoint p;
    using Target = decltype(SetPoint::target);
    for (auto raw = Target::raw_min; raw <= Target::raw_max; ++raw)
    {
        CHECK(p.target.set(Target::to_physical(raw)));
        CHECK_EQ(p.target.raw(), raw);
    }
}

TEST_CASE(names_that_clash_with_the_generated_code_still_work)
{
    namespace names = codec_names::vehicle_sil::std;
    names::std m;
    CHECK(m.f.set(1.0));
    CHECK(m.out.set(2.0));
    CHECK(m.data.set(3.0));
    m.uint8_t.set(names::std::Uint8T::optional);
    const auto decoded = names::std::decode(m.bytes());
    CHECK(decoded.has_value());
    CHECK(*decoded == m);
    CHECK(decoded->data.value() == std::optional<double>(3.0));
    names::Pgn p;
    p.array.set(names::Pgn::Array::big);
    CHECK_EQ(p.array.raw(), std::uint64_t{0xF000000000000000});
    CHECK(names::Pgn::decode(p.bytes())->array.value() == std::optional<names::Pgn::Array>(names::Pgn::Array::big));
}
