// SPDX-License-Identifier: MIT
//
// The LinkError of each way a TAP device or a packet socket fails to open: what happened, then
// how to fix it. Text only (the error numbers come from the caller), so every platform builds and
// tests it.
#pragma once

#include "vehicle_sil/eth_link/external_link.hpp"

#include <span>
#include <string>
#include <string_view>

namespace vehicle_sil::eth_link::detail {

/// The longest interface name Linux takes (IFNAMSIZ - 1).
inline constexpr std::size_t kMaxInterfaceNameBytes = 15;

/// The OS's text for an errno value ("Operation not permitted").
[[nodiscard]] std::string error_text(int error);

/// "lo, enp3s0, vsil0", or "none" for an empty list.
[[nodiscard]] std::string join_names(std::span<const std::string> names);

/// An empty name, or one longer than kMaxInterfaceNameBytes.
[[nodiscard]] LinkError bad_interface_name(std::string_view name);

/// --backend packet: no interface `name`. `names`: the interfaces of this computer.
[[nodiscard]] LinkError no_packet_interface(std::string_view name, std::span<const std::string> names);
/// --backend tap without --tap-create: no interface `name`.
[[nodiscard]] LinkError no_tap_device(std::string_view name, std::span<const std::string> names);
/// --backend packet: `name` does not carry Ethernet frames.
[[nodiscard]] LinkError not_ethernet(std::string_view name, unsigned link_type);
/// socket(AF_PACKET) or a later step on the socket failed with `error`. `executable`: this
/// program's path for the setcap command, or empty if unknown.
[[nodiscard]] LinkError packet_open_failed(std::string_view name, int error, std::string_view step,
                                           std::string_view executable);
/// open("/dev/net/tun") failed with `error`.
[[nodiscard]] LinkError tun_open_failed(int error);
/// Attaching to the existing interface `name` as a TAP device failed with `error`.
[[nodiscard]] LinkError tap_attach_failed(std::string_view name, int error);
/// --tap-create, and an interface `name` exists already.
[[nodiscard]] LinkError tap_exists(std::string_view name);
/// Creating the TAP device `name` failed with `error`.
[[nodiscard]] LinkError tap_create_failed(std::string_view name, int error);

/// The Warning of a link whose interface is down when it starts.
[[nodiscard]] std::string interface_down_warning(std::string_view name);

} // namespace vehicle_sil::eth_link::detail
