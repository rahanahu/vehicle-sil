// SPDX-License-Identifier: MIT
//
// The CAN interfaces of this computer as the kernel's netlink (RTM_GETLINK, readable without
// privileges) tells them, what the bridge makes of them before it opens one, and the lists that
// --list-channels prints.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vehicle_sil/bridge_runtime/device_mark.hpp"
#include "vehicle_sil/can_bridge/can_bridge.hpp"

namespace vehicle_sil::socketcan {

/// One interface of link type CAN.
struct CanInterfaceInfo
{
    std::string name;
    unsigned index = 0;
    /// "vcan" for a virtual CAN interface, "can" for every other kind (an adapter's driver).
    std::string kind = "can";
    bool up = false;
    /// The bit rate the interface is configured with ("can" only; unset while it has none).
    std::optional<std::uint32_t> bitrate;
    /// The controller's state as the kernel names it ("error-active", "error-warning",
    /// "error-passive", "bus-off", "stopped", "sleeping"), or empty where the interface has none
    /// (vcan, or the driver does not tell).
    std::string state;
    /// How long the kernel waits after a bus-off before it restarts the controller; 0: never
    /// ("can" only).
    std::optional<std::uint32_t> restart_ms;

    [[nodiscard]] bool is_virtual() const noexcept { return kind == "vcan"; }
    [[nodiscard]] bool operator==(const CanInterfaceInfo&) const noexcept = default;
};

/// The controller state of `info` as the bridge's bus state (Unknown where the interface has none).
[[nodiscard]] can_bridge::ExternalBusState::Level bus_level_of(const CanInterfaceInfo& info) noexcept;

/// What one datagram of the kernel's answer to RTM_GETLINK holds.
struct LinkMessages
{
    std::vector<CanInterfaceInfo> interfaces; ///< the CAN interfaces in it, in order
    bool done = false;                        ///< it holds NLMSG_DONE (or an error): the answer ends here
    std::string error;                        ///< the kernel's NLMSG_ERROR as text, or empty
    int error_code = 0;                       ///< its errno (positive), or 0
};

/// Reads one datagram of the answer. Malformed parts are left out; nothing here throws.
[[nodiscard]] LinkMessages parse_link_messages(std::span<const std::uint8_t> datagram);

/// The CAN interfaces of this computer, by name. Throws std::runtime_error (with the reason) where
/// the kernel cannot be asked.
[[nodiscard]] std::vector<CanInterfaceInfo> read_can_interfaces();

/// The interface with index `index`, asked for by itself (RTM_GETLINK with the index, no dump), or nullopt where there
/// is none, or it is not a CAN interface. Throws std::runtime_error where the kernel cannot be asked.
[[nodiscard]] std::optional<CanInterfaceInfo> read_can_interface(unsigned index);

/// The interface called `name`, or nullopt.
[[nodiscard]] std::optional<CanInterfaceInfo> find_can_interface(std::string_view name);

/// What the bridge finds wrong with an interface before it opens it, and what it only notes.
struct InterfaceCheck
{
    /// A reason that stops the start (exit 3), with the way to fix it, or nullopt.
    std::optional<std::string> failure;
    /// Lines for "warning:" (they do not stop the start).
    std::vector<std::string> warnings;
};

/// `bitrate`: --bitrate. The bridge sets nothing: a bit rate that differs, or an interface that
/// is down, stops the start and says the command that fixes it; a vcan with --bitrate and a
/// physical interface that restarts itself never after bus-off are noted.
[[nodiscard]] InterfaceCheck check_interface(const CanInterfaceInfo& info, std::optional<std::uint32_t> bitrate);

/// The text for an interface that does not exist, with the interfaces that do.
[[nodiscard]] std::string missing_interface_message(std::string_view name, const std::vector<CanInterfaceInfo>& interfaces);

/// The marks of vehicle-sil bridges on the interface (bridge_runtime::look_for_marks()). An empty
/// function: none anywhere.
using InterfaceInUse = std::function<bridge_runtime::DevicePeers(const CanInterfaceInfo&)>;

/// Human readable list, as --list-channels prints it.
[[nodiscard]] std::string format_interface_list(const std::vector<CanInterfaceInfo>& interfaces, const InterfaceInUse& in_use = {});

/// --list-channels --json, for programs (vehicle_sil_launch, the GUI): one target per interface,
/// "value" the interface name, "usable" true also for an interface that is down ("detail" says
/// down), "in_use", "in_use_by" and "in_use_count" as the lists of the other bridges have them, and
/// "can": {"kind", "up", "bitrate", "state", "restart_ms"}. Ends with a newline.
[[nodiscard]] std::string interfaces_json(const std::vector<CanInterfaceInfo>& interfaces, const InterfaceInUse& in_use = {});

/// --list-channels --json when the list could not be made: no targets, and "error" with `code`
/// ("failed") and `message`. Ends with a newline.
[[nodiscard]] std::string interfaces_error_json(std::string_view code, std::string_view message);

} // namespace vehicle_sil::socketcan
