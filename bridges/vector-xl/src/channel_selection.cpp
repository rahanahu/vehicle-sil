// SPDX-License-Identifier: MIT
#include "vehicle_sil/vector_xl/channel_selection.hpp"

#include "vehicle_sil/json/json_writer.hpp"

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

std::string device_identity(const XlChannelInfo& channel)
{
    if (channel.is_virtual)
    {
        return "xl-virtual";
    }
    return "xl-" + std::to_string(channel.hw_type) + "-" + std::to_string(channel.hw_index) + "-" +
           std::to_string(channel.serial_number) + "-" + std::to_string(channel.hw_channel);
}

namespace {

bridge_runtime::DevicePeers peers_of(const ChannelInUse& in_use, const XlChannelInfo& channel)
{
    return in_use ? in_use(channel) : bridge_runtime::DevicePeers{};
}

} // namespace

std::string format_channel_list(const std::vector<XlChannelInfo>& channels, const ChannelInUse& in_use)
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
            std::snprintf(line, sizeof(line), "  --xl-channel %u  %-32s hwType=%u hwIndex=%u hwChannel=%u serial=%u %s",
                          candidate.number, info.name.c_str(), info.hw_type, info.hw_index, info.hw_channel,
                          info.serial_number, info.transceiver_name.c_str());
            text += line;
            if (const std::string inUse = bridge_runtime::in_use_text(peers_of(in_use, info)); !inUse.empty())
            {
                text += "; " + inUse;
            }
            text += "\n";
        }
    }
    return text;
}

namespace {

/// The members before "targets" (the JSON form of interface_list.hpp of the Ethernet bridge).
void begin_targets(json::JsonWriter& w)
{
    w.begin_object();
    w.integer("vehicle_sil_targets", 1);
    w.string("tool", "vehicle_sil_vector_xl_bridge");
    w.null("backend");
    w.null("source");
    w.begin_array("targets");
}

std::string end_targets(json::JsonWriter& w)
{
    std::string out = w.take();
    out += '\n';
    return out;
}

} // namespace

std::string channels_json(const std::vector<XlChannelInfo>& channels, const ChannelInUse& in_use)
{
    json::JsonWriter w;
    begin_targets(w);
    for (const XlTarget target : {XlTarget::Virtual, XlTarget::Physical})
    {
        const std::string name(to_string(target));
        for (const auto& candidate : selectable_channels(channels, target))
        {
            const auto& info = candidate.info;
            const std::string number = std::to_string(candidate.number);
            const bridge_runtime::DevicePeers peers = peers_of(in_use, info);
            const std::string inUse = bridge_runtime::in_use_text(peers);
            w.begin_object();
            w.string("value", name + ":" + number);
            w.string("label", info.name);
            w.string("detail", name + " " + number + "; hwType=" + std::to_string(info.hw_type) +
                                   " hwIndex=" + std::to_string(info.hw_index) + " hwChannel=" + std::to_string(info.hw_channel) +
                                   " serial=" + std::to_string(info.serial_number) +
                                   (info.transceiver_name.empty() ? "" : " " + info.transceiver_name) +
                                   (inUse.empty() ? "" : "; " + inUse));
            w.boolean("usable", true);
            w.null("reason");
            w.boolean("in_use", peers.count > 0);
            w.begin_array("in_use_by");
            for (const auto pid : peers.pids)
            {
                w.integer("", pid);
            }
            w.end_array();
            w.integer("in_use_count", static_cast<std::int64_t>(peers.count));
            w.begin_object("can");
            w.string("target", name);
            w.integer("number", candidate.number);
            w.string("name", info.name);
            w.integer("hw_type", info.hw_type);
            w.integer("hw_index", info.hw_index);
            w.integer("hw_channel", info.hw_channel);
            w.integer("serial", info.serial_number);
            if (info.transceiver_name.empty())
            {
                w.null("transceiver");
            }
            else
            {
                w.string("transceiver", info.transceiver_name);
            }
            w.end_object();
            w.end_object();
        }
    }
    w.end_array();
    w.begin_array("notes");
    w.end_array();
    w.null("error");
    w.end_object();
    return end_targets(w);
}

std::string channels_error_json(std::string_view code, std::string_view message)
{
    json::JsonWriter w;
    begin_targets(w);
    w.end_array();
    w.begin_array("notes");
    w.end_array();
    w.begin_object("error");
    w.string("code", code);
    w.string("message", message);
    w.end_object();
    w.end_object();
    return end_targets(w);
}

} // namespace vehicle_sil::vector_xl
