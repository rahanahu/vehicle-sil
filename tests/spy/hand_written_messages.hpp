// SPDX-License-Identifier: MIT
//
// Message types written by hand in the form of vehicle_sil::j1939::codec (MessageType), as
// vehicle_sil_add_j1939_codec() would generate them, for the tests of Spy::watch(). Made up.
#pragma once

#include "vehicle_sil/j1939/codec.hpp"
#include "vehicle_sil/j1939/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace vehicle_sil::unit::spy::hand_written {

namespace codec = vehicle_sil::j1939::codec;

/// Proprietary A, selector 0x10 in byte 0: a level (scale 0.5, valid up to 100, 0xFFFF not
/// available) and a code (every value valid).
struct Reading
{
    static constexpr ::vehicle_sil::j1939::Pgn kPgn{0xEF00};
    static constexpr ::vehicle_sil::j1939::Priority kPriority = 6;
    static constexpr std::size_t kLength = 8;
    static constexpr std::uint8_t kFill = 0xFF;

    struct Selector
    {
        static constexpr codec::SignalLayout layout{.start_bit = 0, .length = 8};
        static constexpr std::uint64_t value = 0x10;
    };
    struct Signals
    {
        struct level
        {
            static constexpr codec::SignalLayout layout{.start_bit = 8, .length = 16, .scale = 0.5, .raw_max = 200};
            static constexpr std::string_view name{"level"};
            static constexpr std::string_view unit{"%"};
            static constexpr std::array<codec::SpecialValue, 1> special_values{{{"not_available", 0xFFFF, 0xFFFF, 0xFFFF}}};
        };
        struct code
        {
            static constexpr codec::SignalLayout layout{.start_bit = 24, .length = 8, .raw_max = 0xFF};
            static constexpr std::string_view name{"code"};
            static constexpr std::string_view unit{""};
        };
    };

    codec::Signal<Signals::level> level;
    codec::Signal<Signals::code> code;

    template <typename V>
    void for_each_signal(V&& v)
    {
        v(level);
        v(code);
    }
    template <typename V>
    void for_each_signal(V&& v) const
    {
        v(level);
        v(code);
    }
    std::size_t encode(std::span<std::uint8_t> out) const { return codec::encode_message(*this, out); }
    static std::optional<Reading> decode(std::span<const std::uint8_t> in) { return codec::decode_message<Reading>(in); }
};
static_assert(codec::MessageType<Reading>);

/// Proprietary A, one byte, no selector: any message of the group decodes as it.
struct FirstByte
{
    static constexpr ::vehicle_sil::j1939::Pgn kPgn{0xEF00};
    static constexpr ::vehicle_sil::j1939::Priority kPriority = 6;
    static constexpr std::size_t kLength = 1;
    static constexpr std::uint8_t kFill = 0x00;

    struct Signals
    {
        struct first
        {
            static constexpr codec::SignalLayout layout{.start_bit = 0, .length = 8, .raw_max = 0xFF};
            static constexpr std::string_view name{"first"};
            static constexpr std::string_view unit{""};
        };
    };

    codec::Signal<Signals::first> first;

    template <typename V>
    void for_each_signal(V&& v)
    {
        v(first);
    }
    template <typename V>
    void for_each_signal(V&& v) const
    {
        v(first);
    }
    std::size_t encode(std::span<std::uint8_t> out) const { return codec::encode_message(*this, out); }
    static std::optional<FirstByte> decode(std::span<const std::uint8_t> in) { return codec::decode_message<FirstByte>(in); }
};
static_assert(codec::MessageType<FirstByte>);

/// Proprietary B, 8 bytes: a 64-bit counter whose every value is valid, so most of them do not
/// fit std::int64_t: Spy::watch() publishes it as a real number.
struct Wide
{
    static constexpr ::vehicle_sil::j1939::Pgn kPgn{0xFF10};
    static constexpr ::vehicle_sil::j1939::Priority kPriority = 6;
    static constexpr std::size_t kLength = 8;
    static constexpr std::uint8_t kFill = 0xFF;

    struct Signals
    {
        struct total
        {
            static constexpr codec::SignalLayout layout{.start_bit = 0, .length = 64, .raw_max = ~std::uint64_t{0}};
            static constexpr std::string_view name{"total"};
            static constexpr std::string_view unit{""};
        };
    };

    codec::Signal<Signals::total> total;

    template <typename V>
    void for_each_signal(V&& v)
    {
        v(total);
    }
    template <typename V>
    void for_each_signal(V&& v) const
    {
        v(total);
    }
    std::size_t encode(std::span<std::uint8_t> out) const { return codec::encode_message(*this, out); }
    static std::optional<Wide> decode(std::span<const std::uint8_t> in) { return codec::decode_message<Wide>(in); }
};
static_assert(codec::MessageType<Wide>);

/// A signal named like an item of the watch: Spy::watch<Clash>() does not build
/// (j1939_spy.watch_of_a_signal_named_count_does_not_build).
struct Clash
{
    static constexpr ::vehicle_sil::j1939::Pgn kPgn{0xEF00};
    static constexpr ::vehicle_sil::j1939::Priority kPriority = 6;
    static constexpr std::size_t kLength = 1;
    static constexpr std::uint8_t kFill = 0x00;

    struct Signals
    {
        struct count
        {
            static constexpr codec::SignalLayout layout{.start_bit = 0, .length = 8, .raw_max = 0xFF};
            static constexpr std::string_view name{"count"};
            static constexpr std::string_view unit{""};
        };
    };

    codec::Signal<Signals::count> count;

    template <typename V>
    void for_each_signal(V&& v)
    {
        v(count);
    }
    template <typename V>
    void for_each_signal(V&& v) const
    {
        v(count);
    }
    std::size_t encode(std::span<std::uint8_t> out) const { return codec::encode_message(*this, out); }
    static std::optional<Clash> decode(std::span<const std::uint8_t> in) { return codec::decode_message<Clash>(in); }
};
static_assert(codec::MessageType<Clash>);

/// A signal named "stale_count": allowed when WatchOptions::stale_after is unset, rejected at
/// setup (std::invalid_argument) when WatchOptions::stale_after is set.
struct WithStaleCount
{
    static constexpr ::vehicle_sil::j1939::Pgn kPgn{0xEF00};
    static constexpr ::vehicle_sil::j1939::Priority kPriority = 6;
    static constexpr std::size_t kLength = 2;
    static constexpr std::uint8_t kFill = 0x00;

    struct Signals
    {
        struct value
        {
            static constexpr codec::SignalLayout layout{.start_bit = 0, .length = 8, .raw_max = 0xFF};
            static constexpr std::string_view name{"value"};
            static constexpr std::string_view unit{""};
        };
        struct stale_count
        {
            static constexpr codec::SignalLayout layout{.start_bit = 8, .length = 8, .raw_max = 0xFF};
            static constexpr std::string_view name{"stale_count"};
            static constexpr std::string_view unit{""};
        };
    };

    codec::Signal<Signals::value> value;
    codec::Signal<Signals::stale_count> stale_count;

    template <typename V>
    void for_each_signal(V&& v)
    {
        v(value);
        v(stale_count);
    }
    template <typename V>
    void for_each_signal(V&& v) const
    {
        v(value);
        v(stale_count);
    }
    std::size_t encode(std::span<std::uint8_t> out) const { return codec::encode_message(*this, out); }
    static std::optional<WithStaleCount> decode(std::span<const std::uint8_t> in)
    {
        return codec::decode_message<WithStaleCount>(in);
    }
};
static_assert(codec::MessageType<WithStaleCount>);

} // namespace vehicle_sil::unit::spy::hand_written
