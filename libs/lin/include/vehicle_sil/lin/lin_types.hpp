// SPDX-License-Identifier: MIT
//
// Types shared by the LIN master, slave and schedule table (vehicle_sil::lin).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <silkit/services/lin/LinDatatypes.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace vehicle_sil::lin {

using SilKit::Services::Lin::LinChecksumModel;
using SilKit::Services::Lin::LinId;

inline constexpr LinId kMaxLinId = 63;
inline constexpr std::size_t kMaxLinDataLength = 8;

/// LIN 2.x diagnostic frames: the master request frame (MasterReq, published by the master) and
/// the slave response frame (SlaveResp, published by a slave). Both carry 8 bytes with the
/// classic checksum. The go-to-sleep command is a master request with a first byte (NAD) of 0.
inline constexpr LinId kMasterRequestId = 0x3C;
inline constexpr LinId kSlaveResponseId = 0x3D;
inline constexpr std::size_t kDiagnosticFrameLength = 8;

/// A LIN frame has a 6-bit identifier and 1..8 data bytes.
[[nodiscard]] constexpr bool valid_lin_frame(LinId id, std::size_t data_length) noexcept
{
    return id <= kMaxLinId && data_length >= 1 && data_length <= kMaxLinDataLength;
}

/// The node that provides the response of a frame (the LDF "publisher").
enum class LinPublisher
{
    Slave,  ///< The master sends the header and a slave responds.
    Master, ///< The master sends the header and the response itself (LinMaster::set_frame_data()).
};

/// Sleep state of a LIN node as its application sees it.
enum class LinNodeState
{
    Operational,
    Sleep,
};

[[nodiscard]] constexpr const char* to_string(LinNodeState state) noexcept
{
    return state == LinNodeState::Operational ? "operational" : "sleep";
}

/// A frame that a node received, stamped with the time of its header.
struct LinReceivedFrame
{
    SimTime header_time;
    LinId id = 0;
    std::uint8_t data_length = 0;
    std::array<std::uint8_t, kMaxLinDataLength> data{};
};

} // namespace vehicle_sil::lin
