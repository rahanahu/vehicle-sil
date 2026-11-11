// SPDX-License-Identifier: MIT
//
// What an ECU reports about itself (Ecu::describe()) and its JSON form (vehicle_sil::ecu, no
// SIL Kit).
#pragma once

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/j1939/name.hpp"
#include "vehicle_sil/j1939/types.hpp"

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

/// The kind of network a use is on (Raw and J1939: CAN; LinMaster and LinSlave: LIN).
[[nodiscard]] constexpr NetworkKind kind_of(NetworkUse use) noexcept
{
    return use == NetworkUse::Raw || use == NetworkUse::J1939 ? NetworkKind::Can : NetworkKind::Lin;
}

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
struct EcuDescription
{
    /// The ECU's name (on SIL Kit, its participant name).
    std::string name;
    /// The ECU's step. Empty for a Peer in real time (stepped as frames and timers need).
    std::optional<SimDuration> step{};
    /// In the order of the calls that added them (j1939(), can(), lin_master(), lin_slave()),
    /// whatever their kind.
    std::vector<NetworkDescription> networks{};
    /// The variables the ECU declared (input(), output(), ... and those registered on agent()),
    /// in the order declared; not a Peer's monitoring outputs (PeerOptions::monitor). Empty for an
    /// ECU without a control agent.
    std::vector<control::VariableInfo> variables{};

    friend bool operator==(const EcuDescription&, const EcuDescription&) = default;
};

/// The version of the JSON form of to_json() (its "vehicle_sil_ecu" key). It goes up when a key
/// changes meaning or goes away, and when a key that has a fixed set of values ("kind", "use",
/// "type", "causality") gets a new one. New keys may come without a new version. A reader
/// refuses a version it does not know and ignores keys it does not know.
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

/// The names to_json() uses: "can", "lin"; "raw", "j1939", "lin_master", "lin_slave".
/// std::logic_error for a value outside the enumerators.
[[nodiscard]] const char* to_string(NetworkKind kind);
[[nodiscard]] const char* to_string(NetworkUse use);

namespace detail {
/// The length of the valid UTF-8 sequence at the start of `text` (1 to 4), or 0 if it starts
/// with none (an empty text, a stray or missing continuation byte, an overlong form, a surrogate,
/// a value above U+10FFFF).
[[nodiscard]] std::size_t utf8_sequence_length(std::string_view text) noexcept;
/// true if all of `text` is valid UTF-8 (ASCII included).
[[nodiscard]] bool is_utf8(std::string_view text) noexcept;
} // namespace detail

} // namespace vehicle_sil
