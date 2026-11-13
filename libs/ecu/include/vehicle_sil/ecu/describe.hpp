// SPDX-License-Identifier: MIT
//
// What an ECU reports about itself (Ecu::describe()) and its JSON form (vehicle_sil::ecu, no
// SIL Kit).
#pragma once

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/j1939/name.hpp"
#include "vehicle_sil/j1939/types.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil {

/// The kind of a network: CAN (Ecu::j1939(), Ecu::can()) or LIN (Ecu::lin_master(),
/// Ecu::lin_slave()). One network name is one kind for every ECU of a simulation.
enum class NetworkKind
{
    Can,
    Lin,
};

/// How an ECU is on a network: the call that put it there.
enum class NetworkUse
{
    Raw,       ///< Ecu::can(): a raw CAN channel
    J1939,     ///< Ecu::j1939(): a J1939 network with its nodes
    LinMaster, ///< Ecu::lin_master()
    LinSlave,  ///< Ecu::lin_slave()
};

/// Every NetworkKind and every NetworkUse, in the order of their values. The names and the kinds
/// below are switches without a default, so a new enumerator does not build (vehicle-sil's own
/// targets always have -Werror=switch with GCC and Clang) until each has a case for it, and the
/// static_asserts below fail until it is in its list. Whatever reads or lists the kinds and uses
/// (the check's messages, the readers of the configuration and of --describe, the refusals of a
/// name of another kind) takes them from here. The steps to add a kind or a use, also where no
/// build or test fails: docs/code-map.md, "ネットワークの種類か使い方を足す".
inline constexpr std::array kNetworkKinds{NetworkKind::Can, NetworkKind::Lin};
inline constexpr std::array kNetworkUses{NetworkUse::Raw, NetworkUse::J1939, NetworkUse::LinMaster, NetworkUse::LinSlave};

/// The kind of network a use is on (Raw and J1939: CAN; LinMaster and LinSlave: LIN).
[[nodiscard]] constexpr NetworkKind kind_of(NetworkUse use) noexcept
{
    switch (use)
    {
    case NetworkUse::Raw:
    case NetworkUse::J1939:
        return NetworkKind::Can;
    case NetworkUse::LinMaster:
    case NetworkUse::LinSlave:
        return NetworkKind::Lin;
    }
    return NetworkKind::Lin; // not reached: a value outside the enumerators
}

namespace detail {
/// The names of to_string(); "" for a value outside the enumerators.
[[nodiscard]] constexpr std::string_view network_kind_name(NetworkKind kind) noexcept
{
    switch (kind)
    {
    case NetworkKind::Can:
        return "can";
    case NetworkKind::Lin:
        return "lin";
    }
    return {};
}
[[nodiscard]] constexpr std::string_view network_use_name(NetworkUse use) noexcept
{
    switch (use)
    {
    case NetworkUse::Raw:
        return "raw";
    case NetworkUse::J1939:
        return "j1939";
    case NetworkUse::LinMaster:
        return "lin_master";
    case NetworkUse::LinSlave:
        return "lin_slave";
    }
    return {};
}
/// How messages name a kind of network: "CAN", "LIN".
[[nodiscard]] constexpr std::string_view network_kind_label(NetworkKind kind) noexcept
{
    switch (kind)
    {
    case NetworkKind::Can:
        return "CAN";
    case NetworkKind::Lin:
        return "LIN";
    }
    return {};
}
/// The calls of Ecu that make a network of `kind`, for messages: "j1939() or can()".
[[nodiscard]] constexpr std::string_view network_kind_calls(NetworkKind kind) noexcept
{
    switch (kind)
    {
    case NetworkKind::Can:
        return "j1939() or can()";
    case NetworkKind::Lin:
        return "lin_master() or lin_slave()";
    }
    return {};
}
/// true if `values` are the values 0, 1, ... of Enum, each with a name, and the next value has none.
template <class Enum, std::size_t N>
constexpr bool lists_every_value(const std::array<Enum, N>& values, std::string_view (*name)(Enum) noexcept) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (values[i] != static_cast<Enum>(i) || name(values[i]).empty())
        {
            return false;
        }
    }
    return name(static_cast<Enum>(N)).empty();
}
} // namespace detail

static_assert(detail::lists_every_value(kNetworkKinds, &detail::network_kind_name),
              "kNetworkKinds must list every NetworkKind, in the order of their values");
static_assert(detail::lists_every_value(kNetworkUses, &detail::network_use_name),
              "kNetworkUses must list every NetworkUse, in the order of their values");

/// A node of a J1939 network as it was added (j1939::NodeOptions), not as it is now: the address
/// is the one it asked for, also after it claimed another one.
struct J1939NodeDescription
{
    /// NodeOptions::address: the address the node claims (with a NAME) or uses (without);
    /// j1939::kNullAddress (254): none, it claims a free address of the arbitrary address range.
    j1939::Address address = j1939::kNullAddress;
    /// NodeOptions::name; empty for a node that does not claim ("j1939_name" in JSON).
    std::optional<j1939::Name> name{};

    /// true if the NAME says the node may take another address (arbitrary address capable).
    [[nodiscard]] bool arbitrary_address_capable() const noexcept { return name && name->arbitrary_address_capable(); }
    friend bool operator==(const J1939NodeDescription&, const J1939NodeDescription&) = default;
};

/// One network an ECU is on, for one use. An ECU with j1939(N) and can(N) is on the CAN network
/// N twice (two controllers).
struct NetworkDescription
{
    /// The network's name (empty only for LocalBench::add_j1939()'s bus while it has no name).
    std::string name;
    NetworkUse use = NetworkUse::J1939;
    /// NetworkUse::J1939: the nodes in the order added (also those added from a step, once added).
    /// Empty for the other uses.
    std::vector<J1939NodeDescription> nodes{};

    [[nodiscard]] NetworkKind kind() const noexcept { return kind_of(use); }
    friend bool operator==(const NetworkDescription&, const NetworkDescription&) = default;
};

/// What an ECU is: its name, its step, the networks it is on and the variables it declared.
/// Ecu::describe() makes it; to_json() writes it.
/// What an ECU's executable says about offering its variables to the simulation.
struct ControlOffer
{
    /// How the control agent is reached: "silkit" (vehicle_sil::control_silkit, over SIL Kit's
    /// publish/subscribe).
    std::string transport;
    /// The version of the messages it speaks (control::wire::kVersion of its build).
    int wire_version = 0;

    friend bool operator==(const ControlOffer&, const ControlOffer&) = default;
};

struct EcuDescription
{
    /// The ECU's name (on SIL Kit, its participant name).
    std::string name;
    /// The ECU's step. Empty for a Peer in real time (stepped as frames and timers need).
    std::optional<SimDuration> step{};
    /// In the order of the calls that added them (j1939(), can(), lin_master(), lin_slave()),
    /// whatever their kind.
    std::vector<NetworkDescription> networks{};
    /// The variables of the ECU: first the one every ECU has ("ecu.connected",
    /// is_builtin_variable()), then those it declared (input(), output(), ... and those
    /// registered on agent()) in the order declared; not a Peer's monitoring outputs
    /// (PeerOptions::monitor).
    std::vector<control::VariableInfo> variables{};
    /// Set by whoever runs the ECU with a transport for its control agent (ecu_main(), unless
    /// --no-control); Ecu::describe() leaves it empty. In the JSON: "control", null if empty.
    std::optional<ControlOffer> control{};

    friend bool operator==(const EcuDescription&, const EcuDescription&) = default;
};

/// The version of the JSON form of to_json() (its "vehicle_sil_ecu" key). It goes up when a key
/// changes meaning or goes away. New keys, and new values of a key that has a fixed set of values
/// ("kind", "use", "type", "causality"), may come without a new version. A reader refuses a
/// version it does not know, ignores keys it does not know, and refuses only the description (of
/// one ECU) with a value it does not know.
inline constexpr int kEcuDescriptionFormat = 1;

/// The version of vehicle-sil this library was built from (the CMake project version, e.g.
/// "0.1.0"); to_json() writes it as "vehicle_sil".
[[nodiscard]] const char* version() noexcept;

/// `description` as a JSON object, indented by two spaces, ending with a newline (LF). The same
/// description gives the same bytes: the keys come in a fixed order (all of them, also when empty
/// or null), the integers are decimal and exact (also an enum item's 64-bit value: read it as an
/// integer), a floating-point value is written in the shortest form that reads back to the same
/// double (std::to_chars) and as null if it is not finite. The step is in integer nanoseconds
/// ("step_ns"; null in real time), a J1939 NAME is a string of 16 hexadecimal digits ("0x" and
/// upper case, key "j1939_name"). Strings: valid UTF-8 as it is, with '"', '\\' and the control
/// characters below 0x20 escaped; a byte that starts no valid UTF-8 sequence becomes U+FFFD, so the
/// output is always valid JSON. std::logic_error for an enumeration value outside its
/// enumerators. The keys and an example: docs/ecu-executable.md.
[[nodiscard]] std::string to_json(const EcuDescription& description);

/// true for a variable vehicle-sil gives every ECU (its name starts with "ecu.", which an
/// application cannot declare): "builtin" in the JSON of a variable. "ecu.connected" is one.
[[nodiscard]] constexpr bool is_builtin_variable(std::string_view name) noexcept
{
    return name.substr(0, 4) == "ecu.";
}

/// The names to_json() uses: "can", "lin"; "raw", "j1939", "lin_master", "lin_slave".
/// std::logic_error for a value outside the enumerators.
[[nodiscard]] const char* to_string(NetworkKind kind);
[[nodiscard]] const char* to_string(NetworkUse use);
/// The kind or use that to_string() names `name` (exactly, lower case), or empty for another text.
[[nodiscard]] std::optional<NetworkKind> network_kind_named(std::string_view name) noexcept;
[[nodiscard]] std::optional<NetworkUse> network_use_named(std::string_view name) noexcept;

namespace detail {
/// The length of the valid UTF-8 sequence at the start of `text` (1 to 4), or 0 if it starts
/// with none (an empty text, a stray or missing continuation byte, an overlong form, a surrogate,
/// a value above U+10FFFF).
[[nodiscard]] std::size_t utf8_sequence_length(std::string_view text) noexcept;
/// true if all of `text` is valid UTF-8 (ASCII included).
[[nodiscard]] bool is_utf8(std::string_view text) noexcept;
} // namespace detail

} // namespace vehicle_sil
