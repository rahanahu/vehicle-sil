// SPDX-License-Identifier: MIT
//
// What a SocketCAN error frame (CAN_ERR_FLAG in the identifier) says of the controller's error
// state. Pure; the kernel's layout of the frame is in <linux/can/error.h>.
#pragma once

#include "vehicle_sil/can_bridge/can_bridge.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace vehicle_sil::socketcan {

/// The identifier of a SocketCAN frame has these bits above the CAN identifier.
inline constexpr std::uint32_t kErrorFlag = 0x20000000u;

/// The error state an error frame reports: `level` is nullopt where the frame says nothing of it (a
/// bus error, a lost arbitration ...); `tx_errors` and `rx_errors` are the counters where it carries
/// them (kernel 4.x and later), else 0.
struct ErrorFrameState
{
    std::optional<can_bridge::ExternalBusState::Level> level;
    std::uint8_t tx_errors = 0;
    std::uint8_t rx_errors = 0;
};

/// `can_id` and `data` of an error frame. A bus-off wins over a controller state in the same frame;
/// the controller going error active, or restarting after a bus-off, is error active; an error
/// counter that reached the warning limit is error warning; passive in either direction is error
/// passive.
[[nodiscard]] ErrorFrameState interpret_error_frame(std::uint32_t can_id, const std::array<std::uint8_t, 8>& data) noexcept;

} // namespace vehicle_sil::socketcan
