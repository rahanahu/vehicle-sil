// SPDX-License-Identifier: MIT
//
// Special values from profiles (codec/codec_profiles.yaml, a made-up vendor's Proprietary B
// messages): one profile for a hundred signals, the message and signal overrides, every bit
// pattern without special values, special values by name, the raw value kept, and the fill.

#include "codec_profiles.hpp"
#include "test_harness.hpp"
#include "vehicle_sil/j1939/codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace codec_profiles;
using vehicle_sil::j1939::codec::SignalState;
using vehicle_sil::unit::throws_with;

namespace {

template <typename S>
concept CanSetNotAvailable = requires(S s) { s.set_not_available(); };

template <typename S>
std::vector<std::string_view> special_names()
{
    std::vector<std::string_view> names;
    for (const auto& special : S::specials)
    {
        names.push_back(special.name);
    }
    return names;
}

} // namespace

TEST_CASE(one_profile_gives_every_signal_of_the_file_its_special_values)
{
    // 64 + 32 signals, the profile written once at the top of the file.
    Bulk8 bulk;
    std::size_t signals = 0;
    bulk.for_each_signal([&](auto& signal) {
        using S = std::remove_cvref_t<decltype(signal)>;
        CHECK((special_names<S>() == std::vector<std::string_view>{"sensor_error", "not_available"}));
        CHECK(signal.state() == SignalState::NotAvailable); // a new message: not available
        CHECK(S::raw_max == 0xFD);                          // the valid range stops below them
        ++signals;
    });
    CHECK_EQ(signals, std::size_t{64});

    Bulk16 wide;
    std::size_t unsigned_ones = 0;
    std::size_t signed_ones = 0;
    wide.for_each_signal([&](auto& signal) {
        using S = std::remove_cvref_t<decltype(signal)>;
        if (S::name == "w30" || S::name == "w31")
        {
            return; // the exceptions, below
        }
        CHECK((special_names<S>() == std::vector<std::string_view>{"sensor_error", "not_available"}));
        if constexpr (S::layout.is_signed)
        {
            CHECK(S::raw_min == -32767 && S::raw_max == 32766); // -32768 and 32767 are special
            ++signed_ones;
        }
        else
        {
            CHECK(S::raw_max == 0xFDFF);
            ++unsigned_ones;
        }
    });
    CHECK_EQ(unsigned_ones, std::size_t{15});
    CHECK_EQ(signed_ones, std::size_t{15});
}

TEST_CASE(signals_and_messages_override_the_profile_without_merging)
{
    using W30 = decltype(Bulk16::w30);
    using W31 = decltype(Bulk16::w31);
    static_assert(W30::specials.empty());                  // indicators: none
    static_assert(W30::raw_min == 0 && W30::raw_max == 0xFFFF); // every bit pattern
    CHECK((special_names<W31>() == std::vector<std::string_view>{"saturated"})); // its own only
    static_assert(!CanSetNotAvailable<W31>);

    // The message's profile replaces the file's; a signal may go back to the file's.
    CHECK((special_names<decltype(Calibration::gain)>() == std::vector<std::string_view>{"calibrating", "not_available"}));
    CHECK((special_names<decltype(Calibration::bias)>() == std::vector<std::string_view>{"sensor_error", "not_available"}));
    static_assert(decltype(Calibration::flags)::specials.empty()); // no rule for 4 bits: none
    static_assert(decltype(Calibration::mode)::specials.empty());
    static_assert(decltype(Plain::counter)::specials.empty());
}

TEST_CASE(without_special_values_every_bit_pattern_is_a_value)
{
    Plain p;
    for (std::uint64_t raw = 0; raw <= 0xFF; ++raw)
    {
        p.counter.set_raw(raw);
        CHECK(p.counter.valid());
        CHECK(p.counter.value() == std::optional<double>(static_cast<double>(raw)));
    }
    p.offset.set_raw(-1);
    CHECK(p.offset.valid());
    p.wide.set_raw(0xFFFFFFFFFF);
    CHECK(p.wide.valid());
    CHECK(p.counter.set(255.0)); // the top is a value, not "not available"
    const auto decoded = Plain::decode(p.bytes());
    CHECK(decoded.has_value());
    CHECK_EQ(decoded->counter.raw(), std::uint64_t{255});
    CHECK_EQ(decoded->wide.raw(), std::uint64_t{0xFFFFFFFFFF});
    CHECK(!decoded->counter.special().has_value());
}

TEST_CASE(special_values_by_name_keep_their_raw_value)
{
    Bulk16 m;
    m.w00.set_special("sensor_error");
    CHECK(m.w00.state() == SignalState::Special);
    CHECK(m.w00.special() == std::optional<std::string_view>("sensor_error"));
    CHECK_EQ(m.w00.raw(), std::uint64_t{0xFEFF}); // the profile's `send`, not the range's first value
    m.w00.set_special("not_available");
    CHECK_EQ(m.w00.raw(), std::uint64_t{0xFF00}); // without `send`, the range's first value
    CHECK(m.w00.special() == std::optional<std::string_view>("not_available"));
    m.w00.set_raw(0xFE01);
    CHECK(m.w00.special() == std::optional<std::string_view>("sensor_error"));
    CHECK(!m.w00.value().has_value());
    m.w00.set_raw(0xFE42); // any raw value of the range is the same special value
    CHECK(m.w00.special() == std::optional<std::string_view>("sensor_error"));
    m.w01.set_special("sensor_error"); // signed: -32768
    CHECK_EQ(m.w01.raw(), std::int64_t{-32768});
    m.w01.set_not_available();
    CHECK(m.w01.state() == SignalState::NotAvailable);
    CHECK_EQ(m.w01.raw(), std::int64_t{32767});
    m.w31.set_special("saturated");
    CHECK(m.w31.state() == SignalState::Special);

    const auto decoded = Bulk16::decode(m.bytes());
    CHECK(decoded.has_value());
    CHECK_EQ(decoded->w00.raw(), std::uint64_t{0xFE42}); // the received raw value, not just the name
    CHECK(decoded->w00.special() == std::optional<std::string_view>("sensor_error"));
    CHECK_EQ(decoded->w01.raw(), std::int64_t{32767});
    CHECK(decoded->w31.special() == std::optional<std::string_view>("saturated"));

    static_assert(decltype(Bulk16::w00)::has_special("sensor_error"));
    static_assert(!decltype(Bulk16::w00)::has_special("saturated"));
    CHECK(throws_with<std::invalid_argument>([&] { m.w00.set_special("saturated"); },
                                             "w00 has no special value 'saturated'"));
}

TEST_CASE(special_out_of_range_and_undefined_values_stay_apart)
{
    Calibration c;
    c.gain.set_raw(0);
    CHECK(c.gain.state() == SignalState::Special); // calibrating
    c.gain.set_raw(0xFF);
    CHECK(c.gain.state() == SignalState::NotAvailable);
    c.gain.set_raw(1);
    CHECK(c.gain.state() == SignalState::Valid);
    c.mode.set_raw(1);
    CHECK(c.mode.state() == SignalState::Undefined); // between the enumerators 0 and 2
    c.mode.set_raw(3);
    CHECK(c.mode.state() == SignalState::Undefined); // above them too: an enumeration has no range
    c.gain.set_raw(0x80);
    CHECK(!c.gain.special().has_value());
    c.mode.set(Calibration::Mode::high);
    CHECK(c.mode.value() == std::optional<Calibration::Mode>(Calibration::Mode::high));
}

TEST_CASE(physical_values_never_become_special_values)
{
    Bulk8 m;
    CHECK(!m.s00.set(1000.0)); // above the range: the top valid value, not 0xFE or 0xFF
    CHECK_EQ(m.s00.raw(), std::uint64_t{0xFD});
    CHECK(m.s00.valid());
    Bulk16 w;
    CHECK(!w.w01.set(-1e9)); // signed: -32767, not the special -32768
    CHECK_EQ(w.w01.raw(), std::int64_t{-32767});
    CHECK(!w.w30.set(1e9)); // none: the whole range
    CHECK_EQ(w.w30.raw(), std::uint64_t{0xFFFF});
}

TEST_CASE(the_fill_of_the_file_and_of_a_message)
{
    // File: 0x00. Plain has 64 bits of 64 in signals: nothing to fill. Calibration: its own bytes;
    // bits 22..23 of byte 2 (above mode) are filled with 1, byte 3 with 0.
    static_assert(Bulk8::kFill == 0x00);
    static_assert(Calibration::kFillBytes == std::array<std::uint8_t, 4>{0xFF, 0xFF, 0xFF, 0x00});
    Calibration c;
    c.gain.set_raw(1);
    c.bias.set_raw(2);
    c.flags.set_raw(0x3);
    c.mode.set(Calibration::Mode::low);
    CHECK((c.bytes() == std::array<std::uint8_t, 4>{0x01, 0x02, 0xC3, 0x00}));
    CHECK(Calibration::decode(c.bytes()) == std::optional<Calibration>(c));
}

TEST_CASE(a_64_bit_signed_special_value_keeps_its_sign)
{
    using Total = decltype(Totals::total);
    static_assert(Total::raw_min == std::numeric_limits<std::int64_t>::min() + 1); // the special value is out of the valid range
    Totals t;
    CHECK(t.total.valid()); // raw 0: no "not available"
    t.total.set_special("overflow");
    CHECK_EQ(t.total.raw(), std::numeric_limits<std::int64_t>::min());
    CHECK_EQ(t.total.bits(), std::uint64_t{0x8000000000000000});
    const auto decoded = Totals::decode(t.bytes());
    CHECK(decoded.has_value());
    CHECK(decoded->total.state() == SignalState::Special);
    CHECK_EQ(decoded->total.raw(), std::numeric_limits<std::int64_t>::min());
    CHECK(!t.total.set(-1e30)); // the lowest physical value is the one above the special value
    CHECK_EQ(t.total.raw(), std::numeric_limits<std::int64_t>::min() + 1);
}
