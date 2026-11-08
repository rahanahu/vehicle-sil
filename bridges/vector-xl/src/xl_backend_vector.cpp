// SPDX-License-Identifier: MIT
//
// xl_backend.hpp implemented with Vector's XL Driver Library (Windows only; the library is not
// part of vehicle-sil, see docs/dependencies.md). This file only calls the library and
// translates types and values: no state, no decisions. All behaviour lives in xl_driver.cpp,
// which the tests run against a fake backend.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vxlapi.h>

#include "xl_backend.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

namespace vehicle_sil::vector_xl::detail::backend {
namespace {

constexpr std::uint32_t kExtendedIdFlag = XL_CAN_EXT_MSG_ID;

Status to_status(XLstatus status) noexcept
{
    Status result;
    result.code = static_cast<int>(status);
    if (status == XL_SUCCESS)
    {
        result.outcome = Outcome::Ok;
    }
    else if (status == XL_ERR_QUEUE_IS_EMPTY)
    {
        result.outcome = Outcome::NothingToReceive;
    }
    else if (status == XL_ERR_QUEUE_IS_FULL)
    {
        result.outcome = Outcome::TransmitQueueFull;
    }
    else
    {
        result.outcome = Outcome::Failed;
    }
    return result;
}

XLportHandle to_xl(PortHandle port) noexcept
{
    return static_cast<XLportHandle>(port);
}

XLaccess to_xl(ChannelMask channels) noexcept
{
    return static_cast<XLaccess>(channels);
}

// The library's fixed-size, NUL padded text fields.
template <std::size_t N>
std::string fixed_text(const char (&text)[N])
{
    return std::string(text, std::find(text, text + N, '\0'));
}

} // namespace

Status open_driver()
{
    return to_status(xlOpenDriver());
}

void close_driver() noexcept
{
    (void)xlCloseDriver();
}

Status read_channels(std::vector<XlChannelInfo>& out)
{
    // XLdriverConfig holds an entry for every possible channel; keep it off the stack.
    auto config = std::make_unique<XLdriverConfig>();
    std::memset(config.get(), 0, sizeof(XLdriverConfig));
    const Status status = to_status(xlGetDriverConfig(config.get()));
    if (!status.ok())
    {
        return status;
    }
    const auto count = std::min<unsigned>(config->channelCount, XL_CONFIG_MAX_CHANNELS);
    out.clear();
    out.reserve(count);
    for (unsigned i = 0; i < count; ++i)
    {
        const XLchannelConfig& source = config->channel[i];
        XlChannelInfo info;
        info.name = fixed_text(source.name);
        info.hw_type = source.hwType;
        info.hw_index = source.hwIndex;
        info.hw_channel = source.hwChannel;
        info.channel_index = source.channelIndex;
        info.channel_mask = static_cast<std::uint64_t>(source.channelMask);
        info.serial_number = source.serialNumber;
        info.transceiver_name = fixed_text(source.transceiverName);
        info.is_virtual = source.hwType == XL_HWTYPE_VIRTUAL;
        info.can_capable = (source.channelBusCapabilities & XL_BUS_ACTIVE_CAP_CAN) != 0;
        out.push_back(std::move(info));
    }
    return status;
}

OpenedPort open_port(const std::string& application_name, ChannelMask channels, ChannelMask request_init_access,
                     std::uint32_t rx_queue_size)
{
    std::vector<char> name(application_name.begin(), application_name.end()); // xlOpenPort takes char*
    name.push_back('\0');
    XLportHandle port = XL_INVALID_PORTHANDLE;
    XLaccess permission = to_xl(request_init_access);
    const XLstatus status = xlOpenPort(&port, name.data(), to_xl(channels), &permission, rx_queue_size,
                                       XL_INTERFACE_VERSION, XL_BUS_TYPE_CAN);
    OpenedPort result;
    result.status = to_status(status);
    if (port != XL_INVALID_PORTHANDLE)
    {
        result.port = static_cast<PortHandle>(port);
    }
    result.init_access = static_cast<ChannelMask>(permission);
    return result;
}

Status close_port(PortHandle port) noexcept
{
    return to_status(xlClosePort(to_xl(port)));
}

Status set_bitrate(PortHandle port, ChannelMask channels, std::uint32_t bitrate)
{
    return to_status(xlCanSetChannelBitrate(to_xl(port), to_xl(channels), bitrate));
}

Status set_transmit_receipts(PortHandle port, ChannelMask channels, bool receipts, bool requests)
{
    return to_status(xlCanSetChannelMode(to_xl(port), to_xl(channels), receipts ? 1 : 0, requests ? 1 : 0));
}

Status enable_notification(PortHandle port, NotificationHandle& out)
{
    XLhandle handle{};
    const Status status = to_status(xlSetNotification(to_xl(port), &handle, 1)); // 1: signal at one queued event
    out = handle;
    return status;
}

bool wait_for_notification(NotificationHandle handle, std::chrono::milliseconds timeout) noexcept
{
    const auto ms = timeout.count() < 0 ? 0 : static_cast<DWORD>(timeout.count());
    return WaitForSingleObject(static_cast<HANDLE>(handle), ms) == WAIT_OBJECT_0;
}

Status activate(PortHandle port, ChannelMask channels)
{
    // Never ask the driver to reset its clock: the bridge does not use timestamps, and a reset
    // clashes with Vector's time synchronisation.
    return to_status(xlActivateChannel(to_xl(port), to_xl(channels), XL_BUS_TYPE_CAN, XL_ACTIVATE_NONE));
}

Status deactivate(PortHandle port, ChannelMask channels) noexcept
{
    return to_status(xlDeactivateChannel(to_xl(port), to_xl(channels)));
}

TransmitAttempt transmit(PortHandle port, ChannelMask channels, const CanMessage& message)
{
    XLevent event;
    std::memset(&event, 0, sizeof(event));
    event.tag = XL_TRANSMIT_MSG;
    auto& msg = event.tagData.msg;
    msg.id = message.id | (message.extended ? kExtendedIdFlag : 0u);
    msg.flags = message.remote ? XL_CAN_MSG_FLAG_REMOTE_FRAME : 0;
    msg.dlc = message.dlc;
    std::copy_n(message.data.begin(), std::min<std::size_t>(message.data.size(), MAX_MSG_LEN), msg.data);

    unsigned int count = 1;
    TransmitAttempt result;
    result.status = to_status(xlCanTransmit(to_xl(port), to_xl(channels), &count, &event));
    result.queued = count == 1;
    return result;
}

ReceiveResult receive(PortHandle port)
{
    XLevent event;
    std::memset(&event, 0, sizeof(event));
    unsigned int count = 1; // one event per call
    ReceiveResult result;
    result.status = to_status(xlReceive(to_xl(port), &count, &event));
    if (!result.status.ok() || count == 0)
    {
        return result;
    }

    ReceivedEvent out;
    out.queue_overrun = (event.flags & XL_EVENT_FLAG_OVERRUN) != 0;
    out.is_can_message = event.tag == XL_RECEIVE_MSG;
    if (out.is_can_message)
    {
        const auto& msg = event.tagData.msg;
        out.error_frame = (msg.flags & XL_CAN_MSG_FLAG_ERROR_FRAME) != 0;
        out.transmit_receipt = (msg.flags & XL_CAN_MSG_FLAG_TX_COMPLETED) != 0;
        out.transmit_request = (msg.flags & XL_CAN_MSG_FLAG_TX_REQUEST) != 0;
        out.controller_overrun = (msg.flags & XL_CAN_MSG_FLAG_OVERRUN) != 0;
        const auto rawId = static_cast<std::uint32_t>(msg.id);
        out.message.extended = (rawId & kExtendedIdFlag) != 0;
        out.message.id = rawId & ~kExtendedIdFlag;
        out.message.remote = (msg.flags & XL_CAN_MSG_FLAG_REMOTE_FRAME) != 0;
        out.message.dlc = static_cast<std::uint8_t>(msg.dlc);
        std::copy_n(msg.data, std::min<std::size_t>(MAX_MSG_LEN, out.message.data.size()), out.message.data.begin());
    }
    result.event = out;
    return result;
}

std::string status_text(int code)
{
    const char* text = xlGetErrorString(static_cast<XLstatus>(code));
    return text != nullptr ? std::string(text) : std::string();
}

} // namespace vehicle_sil::vector_xl::detail::backend
