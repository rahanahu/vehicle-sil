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

/// What a signal's raw value means. A special value of the signal's definition comes first, then
/// the valid range, then (for an EnumSignal) the enumerators.
enum class SignalState : std::uint8_t
{
    Valid,        ///< Inside the valid range: value() has the physical value (or the enumerator).
    OutOfRange,   ///< Outside the valid range, and no special value.
    Error,        ///< The special value named "error".
    NotAvailable, ///< The special value named "not_available".
    Special,      ///< Another special value of the definition (special() has its name).
    Undefined,    ///< EnumSignal only: no special value and no enumerator has this value.
};

/// A named special value of a signal: the raw values first .. last (inclusive) mean `name` and
/// are no physical value. `send` is what set_special(name) writes. Raw values are stored as 64
/// bits (two's complement, sign-extended, for a signed signal), like SignalLayout::raw_min.
struct SpecialValue
{
    std::string_view name;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::uint64_t send = 0;
};

/// The names with a SignalState of their own.
inline constexpr std::string_view kError = "error";
inline constexpr std::string_view kNotAvailable = "not_available";

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
constexpr auto specials_of() noexcept
{
    if constexpr (requires { Spec::special_values; })
    {
        return Spec::special_values;
    }
    else
    {
        return std::array<SpecialValue, 0>{};
    }
}

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
/// it) and static constexpr std::array<SpecialValue, N> special_values (none if absent), and, for
/// an EnumSignal, static constexpr std::array<std::uint64_t, N> enumerators (the valid raw values).
template <typename Spec>
class SignalBase
{
public:
    static constexpr const SignalLayout& layout = Spec::layout;
    static constexpr std::string_view name = Spec::name;
    static constexpr std::string_view unit = Spec::unit;
    static constexpr std::optional<std::uint32_t> spn = detail::spn_of<Spec>();
    /// The special values of the definition (resolved by the generator from the profiles).
    static constexpr auto specials = detail::specials_of<Spec>();
    using raw_type = std::conditional_t<layout.is_signed, std::int64_t, std::uint64_t>;

    static_assert(layout.length >= 1 && layout.length <= 64, "a signal has 1 .. 64 bits");
    static_assert(layout.scale > 0, "a signal's scale is above 0");

    /// The lowest and highest valid raw values.
    static constexpr raw_type raw_min = static_cast<raw_type>(layout.raw_min);
    static constexpr raw_type raw_max = static_cast<raw_type>(layout.raw_max);

    /// true if the definition has a special value of this name.
    [[nodiscard]] static constexpr bool has_special(std::string_view special_name) noexcept
    {
        return find_special(special_name) < specials.size();
    }

    /// A new signal is not available (the value set_special("not_available") writes), or raw 0
    /// if it has no such special value.
    constexpr SignalBase() noexcept
      : bits_(has_special(kNotAvailable) ? specials[find_special(kNotAvailable)].send & low_bits(layout.length) : 0)
    {
    }

    [[nodiscard]] constexpr SignalState state() const noexcept
    {
        if (const std::size_t index = special_of(raw()); index < specials.size())
        {
            const std::string_view found = specials[index].name;
            return found == kNotAvailable ? SignalState::NotAvailable
                   : found == kError      ? SignalState::Error
                                          : SignalState::Special;
        }
        if constexpr (detail::HasEnumerators<Spec>)
        {
            // An enumeration has no range of its own: any other value is undefined.
            const bool defined = std::find(Spec::enumerators.begin(), Spec::enumerators.end(), bits_) !=
                                 Spec::enumerators.end();
            return defined ? SignalState::Valid : SignalState::Undefined;
        }
        else
        {
            const raw_type value = raw();
            bool inside = value <= raw_max;
            if constexpr (layout.is_signed || layout.raw_min != 0)
            {
                inside = inside && value >= raw_min;
            }
            return inside ? SignalState::Valid : SignalState::OutOfRange;
        }
    }
    [[nodiscard]] constexpr bool valid() const noexcept { return state() == SignalState::Valid; }
    /// The name of the special value the signal holds; empty if it holds none.
    [[nodiscard]] constexpr std::optional<std::string_view> special() const noexcept
    {
        if (const std::size_t index = special_of(raw()); index < specials.size())
        {
            return specials[index].name;
        }
        return std::nullopt;
    }

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

    /// Any raw value that fits in the signal's bits, special and out of range ones too: the raw
    /// value is not limited by the special values. std::out_of_range if it does not fit.
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
    /// Writes the special value `special_name` (its `send` value). std::invalid_argument if the
    /// definition has no special value of this name.
    constexpr void set_special(std::string_view special_name)
    {
        const std::size_t index = find_special(special_name);
        if (index >= specials.size())
        {
            throw std::invalid_argument("vehicle_sil::j1939::codec: " + std::string(name) + " has no special value '" +
                                        std::string(special_name) + "'");
        }
        bits_ = specials[index].send & low_bits(layout.length);
    }
    /// set_special("not_available") and set_special("error"), for a definition that has them
    /// (a compile error otherwise).
    constexpr void set_not_available() noexcept
        requires(has_special(kNotAvailable))
    {
        bits_ = specials[find_special(kNotAvailable)].send & low_bits(layout.length);
    }
    constexpr void set_error() noexcept
        requires(has_special(kError))
    {
        bits_ = specials[find_special(kError)].send & low_bits(layout.length);
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
    // Indices, not pointers: a pointer compared with nullptr is no constant expression under
    // GCC's -fsanitize=null, and has_special() is used in requires-clauses and if constexpr.
    // specials.size() means "none".
    [[nodiscard]] static constexpr std::size_t find_special(std::string_view special_name) noexcept
    {
        for (std::size_t i = 0; i < specials.size(); ++i)
        {
            if (specials[i].name == special_name)
            {
                return i;
            }
        }
        return specials.size();
    }
    [[nodiscard]] static constexpr std::size_t special_of(raw_type value) noexcept
    {
        for (std::size_t i = 0; i < specials.size(); ++i)
        {
            if (static_cast<raw_type>(specials[i].first) <= value && value <= static_cast<raw_type>(specials[i].last))
            {
                return i;
            }
        }
        return specials.size();
    }
    static constexpr bool specials_fit() noexcept
    {
        for (const SpecialValue& special : specials)
        {
            const auto first = static_cast<raw_type>(special.first);
            const auto last = static_cast<raw_type>(special.last);
            const auto send = static_cast<raw_type>(special.send);
            if (!(lowest_raw() <= first && first <= last && last <= highest_raw() && first <= send && send <= last))
            {
                return false;
            }
        }
        return true;
    }
    static_assert(specials_fit(), "a special value must fit in the signal's bits, with first <= send <= last");

    static_assert(lowest_raw() <= raw_min && raw_min <= raw_max && raw_max <= highest_raw(),
                  "the valid range must fit in the signal's bits");
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
            if constexpr (Base::has_special(kNotAvailable))
            {
                this->set_not_available();
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
///     static constexpr std::size_t kLength; and the bits no signal covers: either
///     static constexpr std::uint8_t kFill (every byte) or
///     static constexpr std::array<std::uint8_t, kLength> kFillBytes (one of the two, not both);
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
    requires(requires { { M::kFill } -> std::convertible_to<std::uint8_t>; } !=
             requires { requires std::same_as<std::remove_cv_t<decltype(M::kFillBytes)>, std::array<std::uint8_t, M::kLength>>; });
    message.for_each_signal([](const auto&) {});
    { message.encode(out) } -> std::same_as<std::size_t>;
    { M::decode(in) } -> std::same_as<std::optional<M>>;
};

namespace detail {

template <typename M>
concept HasSelector = requires { M::Selector::layout; M::Selector::value; };

} // namespace detail

/// Writes `message` into the first M::kLength bytes of `out`: the bits no signal covers get
/// M::kFill (or M::kFillBytes). Returns M::kLength. std::length_error if `out` is shorter (for a PeriodicSender:
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
    if constexpr (requires { M::kFillBytes; })
    {
        static_assert(M::kFillBytes.size() == M::kLength, "kFillBytes has kLength bytes");
        std::copy(M::kFillBytes.begin(), M::kFillBytes.end(), data.begin());
    }
    else
    {
        std::fill(data.begin(), data.end(), M::kFill);
    }
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
