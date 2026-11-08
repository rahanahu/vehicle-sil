// SPDX-License-Identifier: MIT
//
// Typed J1939 messages: the signals of a parameter group as values with a state, and their bytes.
// The message types are generated from YAML (vehicle_sil_add_j1939_codec(), tools/j1939_codegen)
// or written by hand in the same form. This header knows no Network and no Node: it only turns
// values into bytes and back (sending them is the job of the node, see codec_node.hpp).
#pragma once

#include "vehicle_sil/j1939/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace vehicle_sil::j1939::codec {

/// How the bits of a signal are laid out in the message.
enum class ByteOrder : std::uint8_t
{
    /// Intel: SignalLayout::start_bit is the least significant bit; more significant bits follow
    /// at the next positions (up the byte, then on into the next byte).
    LittleEndian,
    /// Motorola: SignalLayout::start_bit is the most significant bit; less significant bits go
    /// down the byte, then on from bit 7 of the next byte (the numbering of DBC files).
    BigEndian,
};

/// What a signal's raw value means, checked in this order.
enum class SignalState : std::uint8_t
{
    Valid,        ///< Inside the valid range: value() has the physical value.
    OutOfRange,   ///< Outside the valid range (also reserved and undefined enumerator values).
    Error,        ///< In the signal's error range.
    NotAvailable, ///< In the signal's "not available" range.
};

/// Bit patterns first .. last (inclusive, the raw bits read as unsigned). Empty if first > last.
struct RawRange
{
    std::uint64_t first = 1;
    std::uint64_t last = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return first > last; }
    [[nodiscard]] constexpr bool contains(std::uint64_t bits) const noexcept { return first <= bits && bits <= last; }
};

/// Where a signal is and what its raw value means.
///
/// Positions count bits from the start of the message: bit b of byte n (b = 0 is the least
/// significant bit) is position n * 8 + b.
struct SignalLayout
{
    std::uint32_t start_bit = 0; ///< See ByteOrder.
    std::uint8_t length = 1;     ///< Bits, 1 .. 64.
    ByteOrder byte_order = ByteOrder::LittleEndian;
    bool is_signed = false; ///< Two's complement.
    double scale = 1.0;     ///< physical = raw * scale + offset (scale > 0).
    double offset = 0.0;
    /// The valid raw values raw_min .. raw_max, as bit patterns (two's complement for a signed
    /// signal, sign-extended to 64 bits).
    std::uint64_t raw_min = 0;
    std::uint64_t raw_max = 0;
    RawRange error{};
    RawRange not_available{};
};

/// The `length` low bits set.
[[nodiscard]] constexpr std::uint64_t low_bits(unsigned length) noexcept
{
    return length >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << length) - 1;
}

/// One past the last byte the signal touches.
[[nodiscard]] constexpr std::size_t end_byte(const SignalLayout& layout) noexcept
{
    const std::size_t first = layout.start_bit / 8;
    if (layout.byte_order == ByteOrder::LittleEndian)
    {
        return (layout.start_bit + layout.length - 1) / 8 + 1;
    }
    const unsigned in_first = layout.start_bit % 8 + 1; // bits from the most significant one down to bit 0
    return layout.length <= in_first ? first + 1 : first + 1 + (layout.length - in_first + 7) / 8;
}

/// The signal's bits in `data` (which must hold end_byte(layout) bytes), as unsigned.
[[nodiscard]] constexpr std::uint64_t extract(std::span<const std::uint8_t> data, const SignalLayout& layout) noexcept
{
    std::uint64_t bits = 0;
    if (layout.byte_order == ByteOrder::LittleEndian)
    {
        unsigned got = 0;
        std::uint32_t position = layout.start_bit;
        while (got < layout.length)
        {
            const unsigned shift = position % 8;
            const unsigned take = (std::min)(8u - shift, layout.length - got);
            bits |= ((std::uint64_t{data[position / 8]} >> shift) & low_bits(take)) << got;
            got += take;
            position += take;
        }
        return bits;
    }
    unsigned remaining = layout.length;
    std::size_t index = layout.start_bit / 8;
    unsigned high = layout.start_bit % 8;
    while (remaining > 0)
    {
        const unsigned take = (std::min)(high + 1, remaining);
        const unsigned low = high + 1 - take;
        bits = (bits << take) | ((std::uint64_t{data[index]} >> low) & low_bits(take));
        remaining -= take;
        ++index;
        high = 7;
    }
    return bits;
}

/// Writes the low layout.length bits of `bits` into `data` (end_byte(layout) bytes at least),
/// leaving the other bits as they are.
constexpr void insert(std::span<std::uint8_t> data, const SignalLayout& layout, std::uint64_t bits) noexcept
{
    const auto put = [&](std::size_t index, unsigned low, unsigned take, std::uint64_t chunk) {
        const auto mask = static_cast<std::uint8_t>(low_bits(take) << low);
        data[index] = static_cast<std::uint8_t>((data[index] & ~mask) | ((chunk << low) & mask));
    };
    if (layout.byte_order == ByteOrder::LittleEndian)
    {
        unsigned done = 0;
        std::uint32_t position = layout.start_bit;
        while (done < layout.length)
        {
            const unsigned shift = position % 8;
            const unsigned take = (std::min)(8u - shift, layout.length - done);
            put(position / 8, shift, take, bits >> done);
            done += take;
            position += take;
        }
        return;
    }
    unsigned remaining = layout.length;
    std::size_t index = layout.start_bit / 8;
    unsigned high = layout.start_bit % 8;
    while (remaining > 0)
    {
        const unsigned take = (std::min)(high + 1, remaining);
        put(index, high + 1 - take, take, bits >> (remaining - take));
        remaining -= take;
        ++index;
        high = 7;
    }
}

/// The signed value of `length` bits (two's complement).
[[nodiscard]] constexpr std::int64_t sign_extend(std::uint64_t bits, unsigned length) noexcept
{
    bits &= low_bits(length);
    if (length < 64 && (bits >> (length - 1)) != 0)
    {
        bits |= ~low_bits(length);
    }
    return static_cast<std::int64_t>(bits);
}

namespace detail {

template <typename Spec>
concept HasEnumerators = requires { Spec::enumerators; };

template <typename Spec>
constexpr std::optional<std::uint32_t> spn_of() noexcept
{
    if constexpr (requires { Spec::spn; })
    {
        return Spec::spn;
    }
    else
    {
        return std::nullopt;
    }
}

} // namespace detail

/// What both kinds of signal share: the raw bits, their state and the special values. A signal
/// type is Signal<Spec> or EnumSignal<Spec, E>; Spec is a type with
///     static constexpr SignalLayout layout;
///     static constexpr std::string_view name, unit;
/// optionally static constexpr std::uint32_t spn (a number for the reader; the codec does not use
/// it), and, for an EnumSignal, static constexpr std::array<std::uint64_t, N> enumerators (the
/// valid raw values).
template <typename Spec>
class SignalBase
{
public:
    static constexpr const SignalLayout& layout = Spec::layout;
    static constexpr std::string_view name = Spec::name;
    static constexpr std::string_view unit = Spec::unit;
    static constexpr std::optional<std::uint32_t> spn = detail::spn_of<Spec>();
    using raw_type = std::conditional_t<layout.is_signed, std::int64_t, std::uint64_t>;

    static_assert(layout.length >= 1 && layout.length <= 64, "a signal has 1 .. 64 bits");
    static_assert(layout.scale > 0, "a signal's scale is above 0");

    /// The lowest and highest valid raw values.
    static constexpr raw_type raw_min = static_cast<raw_type>(layout.raw_min);
    static constexpr raw_type raw_max = static_cast<raw_type>(layout.raw_max);

    /// A new signal is not available (or raw 0 if it has no such range).
    constexpr SignalBase() noexcept : bits_(layout.not_available.empty() ? 0 : layout.not_available.last) {}

    [[nodiscard]] constexpr SignalState state() const noexcept
    {
        if (layout.not_available.contains(bits_))
        {
            return SignalState::NotAvailable;
        }
        if (layout.error.contains(bits_))
        {
            return SignalState::Error;
        }
        const raw_type value = raw();
        bool inside = value <= raw_max;
        if constexpr (layout.is_signed || layout.raw_min != 0)
        {
            inside = inside && value >= raw_min;
        }
        if (!inside)
        {
            return SignalState::OutOfRange;
        }
        if constexpr (detail::HasEnumerators<Spec>)
        {
            if (std::find(Spec::enumerators.begin(), Spec::enumerators.end(), bits_) == Spec::enumerators.end())
            {
                return SignalState::OutOfRange;
            }
        }
        return SignalState::Valid;
    }
    [[nodiscard]] constexpr bool valid() const noexcept { return state() == SignalState::Valid; }

    /// The raw value (sign-extended for a signed signal), whatever its state.
    [[nodiscard]] constexpr raw_type raw() const noexcept
    {
        if constexpr (layout.is_signed)
        {
            return sign_extend(bits_, layout.length);
        }
        else
        {
            return bits_;
        }
    }
    /// The bits as they are in the message (the raw value as unsigned).
    [[nodiscard]] constexpr std::uint64_t bits() const noexcept { return bits_; }

    /// Any raw value that fits in the signal's bits, also an error, "not available" or out of
    /// range one (for tests). std::out_of_range if it does not fit.
    constexpr void set_raw(raw_type raw)
    {
        bool fits = raw <= highest_raw();
        if constexpr (layout.is_signed)
        {
            fits = fits && raw >= lowest_raw();
        }
        if (!fits)
        {
            throw std::out_of_range("vehicle_sil::j1939::codec: " + std::string(name) + ": raw value " +
                                    std::to_string(raw) + " does not fit in " + std::to_string(layout.length) +
                                    " bits");
        }
        bits_ = static_cast<std::uint64_t>(raw) & low_bits(layout.length);
    }
    /// The last value of the "not available" range (all bits 1 with the j1939 indicators).
    constexpr void set_not_available() noexcept
        requires(!layout.not_available.empty())
    {
        bits_ = layout.not_available.last;
    }
    /// The first value of the error range (0xFE, 0xFE00, ... with the j1939 indicators).
    constexpr void set_error() noexcept
        requires(!layout.error.empty())
    {
        bits_ = layout.error.first;
    }

    /// Reads the signal from `data` (end_byte(layout) bytes at least).
    constexpr void read(std::span<const std::uint8_t> data) noexcept { bits_ = extract(data, layout); }
    /// Writes the signal into `data` (end_byte(layout) bytes at least).
    constexpr void write(std::span<std::uint8_t> data) const noexcept { insert(data, layout, bits_); }

    [[nodiscard]] friend constexpr bool operator==(const SignalBase&, const SignalBase&) noexcept = default;

    /// The lowest and highest raw values the bits can hold.
    [[nodiscard]] static constexpr raw_type lowest_raw() noexcept
    {
        if constexpr (layout.is_signed)
        {
            return sign_extend(std::uint64_t{1} << (layout.length - 1), layout.length);
        }
        else
        {
            return 0;
        }
    }
    [[nodiscard]] static constexpr raw_type highest_raw() noexcept
    {
        return static_cast<raw_type>(low_bits(layout.is_signed ? layout.length - 1u : layout.length));
    }

protected:
    constexpr void set_bits(std::uint64_t bits) noexcept { bits_ = bits & low_bits(layout.length); }

private:
    static_assert(lowest_raw() <= raw_min && raw_min <= raw_max && raw_max <= highest_raw(),
                  "the valid range must fit in the signal's bits");
    static_assert(layout.error.empty() || layout.error.last <= low_bits(layout.length),
                  "the error range must fit in the signal's bits");
    static_assert(layout.not_available.empty() || layout.not_available.last <= low_bits(layout.length),
                  "the \"not available\" range must fit in the signal's bits");
    static constexpr bool enumerators_fit() noexcept
    {
        if constexpr (detail::HasEnumerators<Spec>)
        {
            for (const std::uint64_t value : Spec::enumerators)
            {
                if (value > low_bits(layout.length))
                {
                    return false;
                }
            }
        }
        return true;
    }
    static_assert(enumerators_fit(), "the enumerators must fit in the signal's bits");

    std::uint64_t bits_;
};

/// A signal with a physical value: physical = raw * scale + offset.
template <typename Spec>
class Signal : public SignalBase<Spec>
{
    using Base = SignalBase<Spec>;

public:
    using typename Base::raw_type;
    using Base::layout;
    using Base::raw_max;
    using Base::raw_min;

    /// The physical value of a raw value: the product rounded to a double, then the sum rounded.
    /// The same on every compiler: the product is stored first, so no compiler fuses the two into
    /// one multiply-add (which rounds once and can give another last bit).
    [[nodiscard]] static double to_physical(raw_type raw) noexcept
    {
        const volatile double product = static_cast<double>(raw) * layout.scale;
        return product + layout.offset;
    }
    /// The valid range of the physical value, and the step between two raw values. (In
    /// parentheses: also with the min and max macros of <windows.h> without NOMINMAX.)
    [[nodiscard]] static double(min)() noexcept { return to_physical(raw_min); }
    [[nodiscard]] static double(max)() noexcept { return to_physical(raw_max); }
    [[nodiscard]] static constexpr double resolution() noexcept { return layout.scale; }

    /// The physical value while the signal is Valid; empty otherwise (no "not available" or
    /// error value turns into a number).
    [[nodiscard]] std::optional<double> value() const noexcept
    {
        if (!this->valid())
        {
            return std::nullopt;
        }
        return to_physical(this->raw());
    }
    [[nodiscard]] double value_or(double fallback) const noexcept { return value().value_or(fallback); }

    /// Sets the physical value, rounded to the nearest raw value (halves away from zero). A value
    /// outside the valid range is set to the nearest end, and NaN makes the signal not available
    /// (unchanged without a "not available" range): both return false.
    bool set(double physical) noexcept
    {
        if (std::isnan(physical))
        {
            if constexpr (!layout.not_available.empty())
            {
                this->set_bits(layout.not_available.last);
            }
            return false;
        }
        const double rounded = std::round((physical - layout.offset) / layout.scale);
        const auto low = static_cast<double>(raw_min);
        const auto high = static_cast<double>(raw_max);
        bool exact = true;
        raw_type raw{};
        if (rounded >= high) // also the double of a 64-bit raw_max, which may be rounded up
        {
            exact = !(rounded > high);
            raw = raw_max;
        }
        else if (rounded <= low)
        {
            exact = !(rounded < low);
            raw = raw_min;
        }
        else
        {
            raw = static_cast<raw_type>(rounded); // strictly inside: fits
        }
        this->set_bits(static_cast<std::uint64_t>(raw));
        return exact;
    }
};

/// A signal whose valid values are the enumerators of E (a scoped enum generated with it).
/// Other raw values in the valid range are OutOfRange.
template <typename Spec, typename E>
    requires std::is_enum_v<E>
class EnumSignal : public SignalBase<Spec>
{
public:
    /// The enumerator while the signal is Valid; empty otherwise.
    [[nodiscard]] constexpr std::optional<E> value() const noexcept
    {
        if (!this->valid())
        {
            return std::nullopt;
        }
        return static_cast<E>(this->bits());
    }
    [[nodiscard]] constexpr E value_or(E fallback) const noexcept { return value().value_or(fallback); }
    constexpr void set(E value) noexcept { this->set_bits(static_cast<std::uint64_t>(value)); }
};

/// What a message type provides (the generated ones and those written by hand):
///     static constexpr Pgn kPgn; static constexpr Priority kPriority;
///     static constexpr std::size_t kLength; static constexpr std::uint8_t kFill;
///     template <typename F> void for_each_signal(F&& f) (const and not const), and the
///     encode(), decode() and bytes() below (usually forwarding to encode_message() and
///     decode_message()).
/// Optionally a constant field that tells messages of the same PGN apart:
///     struct Selector { static constexpr SignalLayout layout; static constexpr std::uint64_t value; };
template <typename M>
concept MessageType = requires(const M& message, std::span<std::uint8_t> out, std::span<const std::uint8_t> in) {
    { M::kPgn } -> std::convertible_to<Pgn>;
    { M::kPriority } -> std::convertible_to<Priority>;
    { M::kLength } -> std::convertible_to<std::size_t>;
    { M::kFill } -> std::convertible_to<std::uint8_t>;
    message.for_each_signal([](const auto&) {});
    { message.encode(out) } -> std::same_as<std::size_t>;
    { M::decode(in) } -> std::same_as<std::optional<M>>;
};

namespace detail {

template <typename M>
concept HasSelector = requires { M::Selector::layout; M::Selector::value; };

} // namespace detail

/// Writes `message` into the first M::kLength bytes of `out`: the bits no signal covers get
/// M::kFill. Returns M::kLength. std::length_error if `out` is shorter (for a PeriodicSender:
/// PeriodicSenderOptions::max_length).
template <typename M>
std::size_t encode_message(const M& message, std::span<std::uint8_t> out)
{
    if (out.size() < M::kLength)
    {
        throw std::length_error("vehicle_sil::j1939::codec: a message of PGN " + std::to_string(M::kPgn.value()) +
                                " needs " + std::to_string(M::kLength) + " bytes, the buffer has " +
                                std::to_string(out.size()));
    }
    const auto data = out.first(M::kLength);
    std::fill(data.begin(), data.end(), M::kFill);
    if constexpr (detail::HasSelector<M>)
    {
        insert(data, M::Selector::layout, M::Selector::value);
    }
    message.for_each_signal([&](const auto& signal) {
        static_assert(end_byte(std::remove_cvref_t<decltype(signal)>::layout) <= M::kLength,
                      "a signal lies beyond the message's length");
        signal.write(data);
    });
    return M::kLength;
}

/// The message in `data`: empty if `data` is shorter than M::kLength or its selector differs.
/// Bytes after M::kLength are ignored.
template <typename M>
std::optional<M> decode_message(std::span<const std::uint8_t> data)
{
    if (data.size() < M::kLength)
    {
        return std::nullopt;
    }
    if constexpr (detail::HasSelector<M>)
    {
        if (extract(data, M::Selector::layout) != M::Selector::value)
        {
            return std::nullopt;
        }
    }
    M message;
    message.for_each_signal([&](auto& signal) { signal.read(data); });
    return message;
}

/// The message's bytes.
template <typename M>
std::array<std::uint8_t, M::kLength> message_bytes(const M& message)
{
    std::array<std::uint8_t, M::kLength> bytes{};
    encode_message(message, bytes);
    return bytes;
}

} // namespace vehicle_sil::j1939::codec
