// SPDX-License-Identifier: MIT
//
// Choosing a Vector XL channel from the driver configuration. Platform independent: the XL
// driver specific code fills XlChannelInfo from XLdriverConfig.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::vector_xl {

/// Which kind of Vector XL channel the bridge connects to.
enum class XlTarget
{
    Virtual,  ///< Vector Virtual CAN (XL_HWTYPE_VIRTUAL), runs without hardware.
    Physical, ///< A CAN channel of real hardware, e.g. CANcase XL or VN16xx.
};

[[nodiscard]] std::string_view to_string(XlTarget target) noexcept;
/// Parses "virtual" or "physical". @returns nullopt for anything else.
[[nodiscard]] std::optional<XlTarget> parse_xl_target(std::string_view text) noexcept;

/// One entry of the XL driver configuration (XLchannelConfig), reduced to what the bridge needs.
struct XlChannelInfo
{
    std::string name;             ///< e.g. "Virtual Channel 1", "Test CAN Device Channel 1"
    std::uint32_t hw_type = 0;    ///< XL_HWTYPE_*
    std::uint32_t hw_index = 0;   ///< index of the device among devices of the same type
    std::uint32_t hw_channel = 0; ///< channel on the device, 0-based
    std::uint32_t channel_index = 0;
    std::uint64_t channel_mask = 0;
    std::uint32_t serial_number = 0;
    std::string transceiver_name;
    bool is_virtual = false; ///< hw_type == XL_HWTYPE_VIRTUAL
    bool can_capable = false; ///< channelBusCapabilities has XL_BUS_ACTIVE_CAP_CAN
};

/// A channel the CLI can select: `number` is 1-based and counts only the CAN capable channels of
/// one target, in driver configuration order. "--xl-target virtual --xl-channel 1" is the first
/// virtual CAN channel.
struct SelectableChannel
{
    unsigned number = 0;
    XlChannelInfo info;
};

/// The CAN capable channels of `target`, numbered from 1.
[[nodiscard]] std::vector<SelectableChannel> selectable_channels(const std::vector<XlChannelInfo>& channels,
                                                                 XlTarget target);

class ChannelSelectionError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// Picks channel `number` (1-based) of `target`. Throws ChannelSelectionError with the list of
/// available channels if there is no such channel.
[[nodiscard]] XlChannelInfo select_channel(const std::vector<XlChannelInfo>& channels, XlTarget target, unsigned number);

/// Human readable channel table, as printed by --list-channels.
[[nodiscard]] std::string format_channel_list(const std::vector<XlChannelInfo>& channels);

} // namespace vehicle_sil::vector_xl
