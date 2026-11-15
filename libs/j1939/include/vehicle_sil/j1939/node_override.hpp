// SPDX-License-Identifier: MIT
//
// Other values for a node than its code gives: its address and the fields of its NAME that tell
// two units of the same kind apart (vehicle_sil::j1939). For the same program run twice on one
// network. One set of rules for the network that applies them (NetworkOptions::node_overrides)
// and for a tool that says beforehand what they will do (apply_node_overrides()).
#pragma once

#include "vehicle_sil/j1939/name.hpp"
#include "vehicle_sil/j1939/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::j1939 {

/// The values one node gets instead of those in its code (NodeOptions).
///
/// `from` picks the node: the address its code gives it (254: a node whose code asks for any free
/// address). Without `from` the entry is for the only node of the network. The other members are
/// the new values; one that is empty keeps the code's value. Ranges: from and address 0..254
/// (address 254: any free address, for an arbitrary address capable NAME only), identity_number
/// 0..2097151, function_instance 0..31, ecu_instance 0..7. The other fields of the NAME cannot be
/// changed. A value out of its range is refused, never cut to the field's width.
struct NodeOverride
{
    std::optional<Address> from{};
    std::optional<Address> address{};
    std::optional<std::uint32_t> identity_number{};
    std::optional<std::uint8_t> function_instance{};
    std::optional<std::uint8_t> ecu_instance{};

    /// true if a field of the NAME is given.
    [[nodiscard]] constexpr bool changes_name() const noexcept
    {
        return identity_number.has_value() || function_instance.has_value() || ecu_instance.has_value();
    }
    /// true if a new value is given (an entry with `from` only changes nothing).
    [[nodiscard]] constexpr bool changes() const noexcept { return address.has_value() || changes_name(); }

    [[nodiscard]] friend bool operator==(const NodeOverride&, const NodeOverride&) = default;
};

/// The address and the NAME of a node: as its code gives them, or after an override.
struct NodeValues
{
    Address address = kNullAddress;
    std::optional<Name> name{};

    [[nodiscard]] friend bool operator==(const NodeValues&, const NodeValues&) = default;
};

/// Why an override cannot be applied to a node.
enum class NodeOverrideError : std::uint8_t
{
    NameFieldWithoutName,  ///< a field of the NAME for a node without a NAME
    AnyAddressNotCapable,  ///< address 254 for a NAME that is not arbitrary address capable
    AnyAddressWithoutName, ///< address 254 for a node without a NAME
    FieldOutOfRange,       ///< identity_number, function_instance or ecu_instance above its range
    AddressOutOfRange,     ///< address 255
};

/// The values of a node after an override, or why it cannot be applied (`values` is then `coded`).
struct NodeOverrideOutcome
{
    NodeValues values{};
    std::optional<NodeOverrideError> error{};
};

/// Applies `entry` to the values a node's code gives; `from` is not looked at (node_override_matches()).
/// The fields of the NAME that the entry does not give keep their bits.
[[nodiscard]] NodeOverrideOutcome apply_node_override(const NodeValues& coded, const NodeOverride& entry) noexcept;

/// true if `entry` is for a node whose code gives it `coded_address`: its `from` is that address,
/// or it has none.
[[nodiscard]] constexpr bool node_override_matches(const NodeOverride& entry, Address coded_address) noexcept
{
    return !entry.from || *entry.from == coded_address;
}

/// Two entries of one list that cannot both stand.
struct NodeOverrideClash
{
    std::size_t first = 0;
    std::size_t second = 0;
    /// true: one of them has no `from` (it is for the only node, so there can be no other entry);
    /// false: both have the same `from`.
    bool without_from = false;

    [[nodiscard]] friend bool operator==(const NodeOverrideClash&, const NodeOverrideClash&) = default;
};

/// The first two entries of `entries` that cannot both stand, if any: two with the same `from`, or
/// one without `from` and any other. Entries that change nothing count too.
[[nodiscard]] std::optional<NodeOverrideClash> find_node_override_clash(std::span<const NodeOverride> entries) noexcept;

/// What became of one entry of apply_node_overrides().
enum class NodeOverrideMatch : std::uint8_t
{
    Applied,      ///< `node` gets `values`
    Skipped,      ///< the entry changes nothing (NodeOverride::changes())
    NoNodes,      ///< the network has no node
    NoNodeAtFrom, ///< no node has the address `from` in the code
    Ambiguous,    ///< no `from`, and the nodes have different addresses in the code
    Indistinct,   ///< two or more nodes have the address the entry picks by (with `from`: that
                  ///< address; without: every node has the same one)
    CannotApply,  ///< `error` says why (`node` is the node it is for)
};

struct NodeOverrideResult
{
    NodeOverrideMatch match = NodeOverrideMatch::Skipped;
    /// The node the entry is for (the first one for Ambiguous and Indistinct); 0 if there is none.
    std::size_t node = 0;
    /// Applied: the values after the override.
    NodeValues values{};
    std::optional<NodeOverrideError> error{};

    [[nodiscard]] friend bool operator==(const NodeOverrideResult&, const NodeOverrideResult&) = default;
};

/// What `entries` do to the nodes of one network, given what the code gives each node (`coded`, in
/// the order the nodes are added): one result per entry, in their order. For a tool that knows the
/// nodes beforehand; a Network decides the same way as its nodes are added. Every entry is matched
/// against the addresses in the code, never against what an earlier entry made of them (two entries
/// may swap the addresses of two nodes). The entries are not checked against each other
/// (find_node_override_clash()).
[[nodiscard]] std::vector<NodeOverrideResult> apply_node_overrides(std::span<const NodeValues> coded,
                                                                  std::span<const NodeOverride> entries);

// --- As text

/// The names of the settings in the order they are written, for messages.
inline constexpr std::string_view kNodeOverrideSettings = "from, address, identity_number, function_instance, ecu_instance";

/// An address as format_node_override() writes it: "0x01"; 254 as "254".
[[nodiscard]] std::string node_override_address_text(Address address);
/// An address a node's code gives, for a message: "0x01"; 254 as "any (254)".
[[nodiscard]] std::string coded_address_text(Address address);

/// `entry` as settings: "from:0x00,address:0x01,identity_number:2" (the settings that are given,
/// in the order of kNodeOverrideSettings; an address as node_override_address_text(), the others
/// in decimal). Empty for an entry without any. parse_node_override() reads it back to the same
/// entry.
[[nodiscard]] std::string format_node_override(const NodeOverride& entry);

/// What is wrong with the first value of `entry` that is out of its range, as
/// "identity_number 2097152, which is out of its range 0..2097151"; empty if none is.
[[nodiscard]] std::string node_override_range_problem(const NodeOverride& entry);

/// What is wrong with a text of settings.
enum class NodeOverrideTextError : std::uint8_t
{
    EmptySetting,   ///< nothing between two commas, or an empty text
    NoValue,        ///< `setting` has no ':'
    UnknownSetting, ///< `setting` is not one of kNodeOverrideSettings
    Twice,          ///< `setting` is given twice
    NotANumber,     ///< `value` of `setting` is neither decimal nor 0x.. hexadecimal
    OutOfRange,     ///< `value` of `setting` is above `max`
};

struct NodeOverrideText
{
    /// The entry, if the text is right.
    std::optional<NodeOverride> entry{};
    NodeOverrideTextError error = NodeOverrideTextError::EmptySetting;
    std::string setting{};  ///< the name of the setting the error is about (as written)
    std::string value{};    ///< its value as written (NotANumber, OutOfRange)
    std::uint32_t max = 0;  ///< the largest value of the setting (OutOfRange)
};

/// Reads settings as format_node_override() writes them: `<setting>:<value>` separated by commas,
/// each setting once, in any order; a value in decimal or, with 0x or 0X before it, in hexadecimal
/// (digits only: no sign, no space). An entry that changes nothing (`from` only) is read as it is.
[[nodiscard]] NodeOverrideText parse_node_override(std::string_view text);

} // namespace vehicle_sil::j1939
