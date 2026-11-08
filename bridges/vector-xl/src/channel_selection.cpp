// SPDX-License-Identifier: MIT
#include "vehicle_sil/vector_xl/channel_selection.hpp"

#include <cstdio>

namespace vehicle_sil::vector_xl {

std::string_view to_string(XlTarget target) noexcept
{
    return target == XlTarget::Virtual ? "virtual" : "physical";
}

std::optional<XlTarget> parse_xl_target(std::string_view text) noexcept
{
    if (text == "virtual")
    {
        return XlTarget::Virtual;
    }
    if (text == "physical")
    {
        return XlTarget::Physical;
    }
    return std::nullopt;
}

std::vector<SelectableChannel> selectable_channels(const std::vector<XlChannelInfo>& channels, XlTarget target)
{
    std::vector<SelectableChannel> result;
    for (const auto& channel : channels)
    {
        if (!channel.can_capable || channel.is_virtual != (target == XlTarget::Virtual))
        {
            continue;
        }
        result.push_back({static_cast<unsigned>(result.size() + 1), channel});
    }
    return result;
}

XlChannelInfo select_channel(const std::vector<XlChannelInfo>& channels, XlTarget target, unsigned number)
{
    const auto candidates = selectable_channels(channels, target);
    if (number >= 1 && number <= candidates.size())
    {
        return candidates[number - 1].info;
    }
    std::string message = "no " + std::string(to_string(target)) + " CAN channel " + std::to_string(number) + "; ";
    if (candidates.empty())
    {
        message += "the XL driver reports no " + std::string(to_string(target)) + " CAN channels";
    }
    else
    {
        message += "available:";
        for (const auto& candidate : candidates)
        {
            message += " " + std::to_string(candidate.number) + "=\"" + candidate.info.name + "\"";
        }
    }
    throw ChannelSelectionError(message);
}

std::string format_channel_list(const std::vector<XlChannelInfo>& channels)
{
    std::string text;
    char line[256];
    for (const XlTarget target : {XlTarget::Virtual, XlTarget::Physical})
    {
        const auto candidates = selectable_channels(channels, target);
        text += std::string(to_string(target)) + " CAN channels (--xl-target " + std::string(to_string(target)) + "):\n";
        if (candidates.empty())
        {
            text += "  (none)\n";
        }
        for (const auto& candidate : candidates)
        {
            const auto& info = candidate.info;
            std::snprintf(line, sizeof(line), "  --xl-channel %u  %-32s hwType=%u hwIndex=%u hwChannel=%u serial=%u %s\n",
                          candidate.number, info.name.c_str(), info.hw_type, info.hw_index, info.hw_channel,
                          info.serial_number, info.transceiver_name.c_str());
            text += line;
        }
    }
    return text;
}

} // namespace vehicle_sil::vector_xl
