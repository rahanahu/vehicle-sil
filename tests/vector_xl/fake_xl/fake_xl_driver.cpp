// SPDX-License-Identifier: MIT
//
// TEST-ONLY in-process fake of the XL backend interface (bridges/vector-xl/src/xl_backend.hpp);
// see fake_xl_control.hpp for the modelled behaviour. Every entry point takes one global mutex.
#include "fake_xl_control.hpp"
#include "xl_backend.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>

namespace backend = vehicle_sil::vector_xl::detail::backend;

namespace fake_xl {
namespace {

constexpr int kEchoNodeId = -1;

struct Notification
{
    std::mutex mutex;
    std::condition_variable cv;
    bool signalled = false;

    void signal()
    {
        {
            const std::lock_guard lock(mutex);
            signalled = true;
        }
        cv.notify_all();
    }

    bool wait(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex);
        const bool woke = cv.wait_for(lock, timeout, [this] { return signalled; });
        signalled = false; // resets itself once a wait returns
        return woke;
    }
};

struct Port
{
    unsigned channel = 0;
    backend::ChannelMask mask = 0;
    bool init_access = false;
    bool tx_receipts = true;
    bool tx_requests = false;
    bool active = false;
    unsigned capacity = 0;
    std::deque<backend::ReceivedEvent> rx;
    bool pending_overrun = false;
    std::shared_ptr<Notification> notification = std::make_shared<Notification>();
    std::optional<unsigned long> bitrate;
};

struct Node
{
    unsigned channel = 0;
    std::deque<Frame> rx;
};

struct State
{
    std::mutex mutex;
    std::condition_variable node_cv;
    std::vector<ChannelDef> channels = default_channels();
    bool fail_open = false;
    bool queue_full = false;
    bool ignore_mode = false;
    std::optional<unsigned> echo_channel;
    int driver_refs = 0;
    std::map<std::int64_t, Port> ports;
    std::int64_t next_port = 1;
    std::map<int, Node> nodes;
    int next_node = 1;
};

State& state()
{
    static State s;
    return s;
}

backend::Status status(int code)
{
    backend::Status result;
    result.code = code;
    switch (code)
    {
    case kStatusOk: result.outcome = backend::Outcome::Ok; break;
    case kStatusNothingToReceive: result.outcome = backend::Outcome::NothingToReceive; break;
    case kStatusQueueFull: result.outcome = backend::Outcome::TransmitQueueFull; break;
    default: result.outcome = backend::Outcome::Failed; break;
    }
    return result;
}

backend::CanMessage to_message(const Frame& frame)
{
    backend::CanMessage message;
    message.id = frame.id;
    message.extended = frame.extended;
    message.remote = frame.remote;
    message.dlc = frame.dlc;
    std::copy_n(frame.data.begin(), std::min(frame.data.size(), message.data.size()), message.data.begin());
    return message;
}

backend::ReceivedEvent rx_event(const Frame& frame)
{
    backend::ReceivedEvent event;
    event.is_can_message = true;
    event.message = to_message(frame);
    return event;
}

// Caller holds state().mutex.
void enqueue(Port& port, backend::ReceivedEvent event)
{
    if (!port.active)
    {
        return;
    }
    if (port.rx.size() >= port.capacity)
    {
        port.pending_overrun = true;
        return;
    }
    if (port.pending_overrun)
    {
        event.queue_overrun = true;
        port.pending_overrun = false;
    }
    port.rx.push_back(event);
    port.notification->signal();
}

Frame echo_reply(const Frame& frame)
{
    Frame reply = frame;
    reply.remote = false;
    const std::size_t length = std::min<std::size_t>(frame.dlc, backend::kMaxDataBytes);
    reply.data.assign(length, 0);
    for (std::size_t i = 0; i < length; ++i)
    {
        reply.data[i] = frame.remote ? static_cast<std::uint8_t>(i) : static_cast<std::uint8_t>(~frame.data[i]);
    }
    return reply;
}

// Puts `frame` on the bus of `channel` as seen by everyone except the sender. Caller holds the mutex.
void deliver_to_bus(unsigned channel, const Frame& frame, std::optional<std::int64_t> senderPort, int senderNode)
{
    auto& s = state();
    for (auto& [handle, port] : s.ports)
    {
        if (port.channel == channel && handle != senderPort)
        {
            enqueue(port, rx_event(frame));
        }
    }
    bool notifyNodes = false;
    for (auto& [id, node] : s.nodes)
    {
        if (node.channel == channel && id != senderNode)
        {
            node.rx.push_back(frame);
            notifyNodes = true;
        }
    }
    if (notifyNodes)
    {
        s.node_cv.notify_all();
    }
    if (s.echo_channel == channel && senderNode != kEchoNodeId)
    {
        deliver_to_bus(channel, echo_reply(frame), std::nullopt, kEchoNodeId);
    }
}

std::int64_t key(backend::PortHandle port)
{
    return static_cast<std::int64_t>(port);
}

Port* find_port(backend::PortHandle handle)
{
    auto& s = state();
    const auto it = s.ports.find(key(handle));
    return it == s.ports.end() ? nullptr : &it->second;
}

Port* find_port(backend::PortHandle handle, backend::ChannelMask mask)
{
    auto* port = find_port(handle);
    return port != nullptr && port->mask == mask ? port : nullptr;
}

std::optional<unsigned> channel_of_mask(backend::ChannelMask mask)
{
    auto& s = state();
    if (mask == 0 || (mask & (mask - 1)) != 0)
    {
        return std::nullopt; // the fake supports exactly one channel per port
    }
    for (unsigned i = 0; i < s.channels.size(); ++i)
    {
        if (mask == (backend::ChannelMask{1} << i))
        {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace

std::vector<ChannelDef> default_channels()
{
    return {
        {"Virtual Channel 1", kVirtualHwType, true, true, 0, 0, 0, "Virtual CAN"},
        {"Virtual Channel 2", kVirtualHwType, true, true, 0, 1, 0, "Virtual CAN"},
        {"Test CAN Device Channel 1", kDeviceHwType, false, true, 0, 0, 12345, "Test Onboard CAN"},
        {"Test CAN Device Channel 2", kDeviceHwType, false, true, 0, 1, 12345, "Test Onboard CAN"},
        {"Test CAN Device Channel 3", kDeviceHwType, false, false, 0, 2, 12345, "Test LIN Device"},
    };
}

void reset()
{
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    s.channels = default_channels();
    s.fail_open = false;
    s.queue_full = false;
    s.ignore_mode = false;
    s.echo_channel.reset();
    s.ports.clear();
    s.nodes.clear();
}

void set_channels(std::vector<ChannelDef> channels)
{
    const std::lock_guard lock(state().mutex);
    state().channels = std::move(channels);
}

void set_fail_open_driver(bool fail)
{
    const std::lock_guard lock(state().mutex);
    state().fail_open = fail;
}

void set_transmit_queue_full(bool full)
{
    const std::lock_guard lock(state().mutex);
    state().queue_full = full;
}

void set_ignore_channel_mode(bool ignore)
{
    const std::lock_guard lock(state().mutex);
    state().ignore_mode = ignore;
}

void inject_overrun(unsigned channel_index)
{
    const std::lock_guard lock(state().mutex);
    for (auto& [handle, port] : state().ports)
    {
        if (port.channel == channel_index)
        {
            port.pending_overrun = true;
        }
    }
}

PortState port_state(unsigned channel_index)
{
    const std::lock_guard lock(state().mutex);
    for (const auto& [handle, port] : state().ports)
    {
        if (port.channel == channel_index)
        {
            return PortState{true, port.active, port.init_access, port.tx_receipts, port.bitrate};
        }
    }
    return {};
}

TestNode::TestNode(unsigned channel_index)
{
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    id_ = s.next_node++;
    s.nodes[id_] = Node{channel_index, {}};
}

TestNode::~TestNode()
{
    const std::lock_guard lock(state().mutex);
    state().nodes.erase(id_);
}

void TestNode::send(const Frame& frame)
{
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    deliver_to_bus(s.nodes.at(id_).channel, frame, std::nullopt, id_);
}

void TestNode::send_error_frame()
{
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    const unsigned channel = s.nodes.at(id_).channel;
    for (auto& [handle, port] : s.ports)
    {
        if (port.channel == channel)
        {
            auto event = rx_event(Frame{});
            event.error_frame = true;
            enqueue(port, event);
        }
    }
}

std::optional<Frame> TestNode::receive(std::chrono::milliseconds timeout)
{
    auto& s = state();
    std::unique_lock lock(s.mutex);
    if (!s.node_cv.wait_for(lock, timeout, [&] { return !s.nodes.at(id_).rx.empty(); }))
    {
        return std::nullopt;
    }
    auto& rx = s.nodes.at(id_).rx;
    Frame frame = rx.front();
    rx.pop_front();
    return frame;
}

} // namespace fake_xl

// ---- The backend interface ----------------------------------------------------------------

namespace vehicle_sil::vector_xl::detail::backend {

using fake_xl::state;
using fake_xl::status;

Status open_driver()
{
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    if (const char* fail = std::getenv("VEHICLE_SIL_FAKE_XL_FAIL_OPEN"); fail != nullptr && std::string_view(fail) == "1")
    {
        s.fail_open = true;
    }
    if (s.fail_open)
    {
        return status(fake_xl::kStatusDriverUnavailable);
    }
    if (const char* ignore = std::getenv("VEHICLE_SIL_FAKE_XL_IGNORE_CHANNEL_MODE");
        ignore != nullptr && std::string_view(ignore) == "1")
    {
        s.ignore_mode = true;
    }
    if (const char* echo = std::getenv("VEHICLE_SIL_FAKE_XL_ECHO"); echo != nullptr && *echo != '\0')
    {
        s.echo_channel = static_cast<unsigned>(std::strtoul(echo, nullptr, 10));
    }
    ++s.driver_refs;
    return status(fake_xl::kStatusOk);
}

void close_driver() noexcept
{
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    if (s.driver_refs > 0)
    {
        --s.driver_refs;
    }
}

Status read_channels(std::vector<XlChannelInfo>& out)
{
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    if (s.driver_refs == 0)
    {
        return status(fake_xl::kStatusBadArgument);
    }
    out.clear();
    for (unsigned i = 0; i < s.channels.size(); ++i)
    {
        const auto& def = s.channels[i];
        XlChannelInfo info;
        info.name = def.name;
        info.hw_type = def.hw_type;
        info.hw_index = def.hw_index;
        info.hw_channel = def.hw_channel;
        info.channel_index = i;
        info.channel_mask = ChannelMask{1} << i;
        info.serial_number = def.serial;
        info.transceiver_name = def.transceiver;
        info.is_virtual = def.is_virtual;
        info.can_capable = def.can_capable;
        out.push_back(std::move(info));
    }
    return status(fake_xl::kStatusOk);
}

OpenedPort open_port(const std::string& application_name, ChannelMask channels, ChannelMask request_init_access,
                     std::uint32_t rx_queue_size)
{
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    OpenedPort result;
    const auto channel = fake_xl::channel_of_mask(channels);
    if (application_name.empty() || s.driver_refs == 0 || !channel || rx_queue_size == 0 ||
        (rx_queue_size & (rx_queue_size - 1)) != 0)
    {
        result.status = status(fake_xl::kStatusBadArgument);
        return result;
    }
    if (!s.channels[*channel].can_capable)
    {
        result.status = status(fake_xl::kStatusNoAccess);
        return result;
    }
    fake_xl::Port port;
    port.channel = *channel;
    port.mask = channels;
    port.capacity = rx_queue_size - 1;
    if ((request_init_access & channels) != 0)
    {
        const bool taken = std::any_of(s.ports.begin(), s.ports.end(), [&](const auto& entry) {
            return entry.second.channel == *channel && entry.second.init_access;
        });
        port.init_access = !taken;
    }
    result.init_access = port.init_access ? channels : 0;
    const std::int64_t handle = s.next_port++;
    s.ports.emplace(handle, std::move(port));
    result.port = static_cast<PortHandle>(handle);
    result.status = status(fake_xl::kStatusOk);
    return result;
}

Status close_port(PortHandle port) noexcept
{
    const std::lock_guard lock(state().mutex);
    return status(state().ports.erase(fake_xl::key(port)) == 1 ? fake_xl::kStatusOk : fake_xl::kStatusBadArgument);
}

Status set_bitrate(PortHandle handle, ChannelMask channels, std::uint32_t bitrate)
{
    const std::lock_guard lock(state().mutex);
    auto* port = fake_xl::find_port(handle, channels);
    if (port == nullptr)
    {
        return status(fake_xl::kStatusBadArgument);
    }
    if (!port->init_access)
    {
        return status(fake_xl::kStatusNoAccess);
    }
    port->bitrate = bitrate;
    return status(fake_xl::kStatusOk);
}

Status set_transmit_receipts(PortHandle handle, ChannelMask channels, bool receipts, bool requests)
{
    const std::lock_guard lock(state().mutex);
    auto* port = fake_xl::find_port(handle, channels);
    if (port == nullptr)
    {
        return status(fake_xl::kStatusBadArgument);
    }
    if (!port->init_access)
    {
        return status(fake_xl::kStatusNoAccess);
    }
    port->tx_receipts = receipts;
    port->tx_requests = requests;
    return status(fake_xl::kStatusOk);
}

Status enable_notification(PortHandle handle, NotificationHandle& out)
{
    const std::lock_guard lock(state().mutex);
    auto* port = fake_xl::find_port(handle);
    if (port == nullptr)
    {
        return status(fake_xl::kStatusBadArgument);
    }
    out = port->notification.get();
    return status(fake_xl::kStatusOk);
}

bool wait_for_notification(NotificationHandle handle, std::chrono::milliseconds timeout) noexcept
{
    auto* notification = static_cast<fake_xl::Notification*>(handle);
    return notification != nullptr && notification->wait(timeout);
}

Status activate(PortHandle handle, ChannelMask channels)
{
    const std::lock_guard lock(state().mutex);
    auto* port = fake_xl::find_port(handle, channels);
    if (port == nullptr)
    {
        return status(fake_xl::kStatusBadArgument);
    }
    port->active = true;
    return status(fake_xl::kStatusOk);
}

Status deactivate(PortHandle handle, ChannelMask channels) noexcept
{
    const std::lock_guard lock(state().mutex);
    auto* port = fake_xl::find_port(handle, channels);
    if (port == nullptr)
    {
        return status(fake_xl::kStatusBadArgument);
    }
    port->active = false;
    port->rx.clear();
    return status(fake_xl::kStatusOk);
}

TransmitAttempt transmit(PortHandle handle, ChannelMask channels, const CanMessage& message)
{
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    TransmitAttempt result;
    auto* port = fake_xl::find_port(handle, channels);
    if (port == nullptr || message.dlc > kMaxDataBytes)
    {
        result.status = status(fake_xl::kStatusBadArgument);
        return result;
    }
    if (!port->active)
    {
        result.status = status(fake_xl::kStatusNotOnBus);
        return result;
    }
    if (s.queue_full)
    {
        result.status = status(fake_xl::kStatusQueueFull);
        return result;
    }
    fake_xl::Frame frame;
    frame.extended = message.extended;
    frame.id = message.id & (frame.extended ? 0x1FFF'FFFFu : 0x7FFu);
    frame.remote = message.remote;
    frame.dlc = message.dlc;
    if (!frame.remote)
    {
        frame.data.assign(message.data.begin(), message.data.begin() + message.dlc);
    }
    if (port->tx_requests)
    {
        auto request = fake_xl::rx_event(frame);
        request.transmit_request = true;
        fake_xl::enqueue(*port, request);
    }
    fake_xl::deliver_to_bus(port->channel, frame, fake_xl::key(handle), 0);
    if (port->tx_receipts || s.ignore_mode)
    {
        auto receipt = fake_xl::rx_event(frame);
        receipt.transmit_receipt = true;
        fake_xl::enqueue(*port, receipt);
    }
    result.status = status(fake_xl::kStatusOk);
    result.queued = true;
    return result;
}

ReceiveResult receive(PortHandle handle)
{
    const std::lock_guard lock(state().mutex);
    ReceiveResult result;
    auto* port = fake_xl::find_port(handle);
    if (port == nullptr)
    {
        result.status = status(fake_xl::kStatusBadArgument);
        return result;
    }
    if (port->rx.empty())
    {
        result.status = status(fake_xl::kStatusNothingToReceive);
        return result;
    }
    result.event = port->rx.front();
    port->rx.pop_front();
    result.status = status(fake_xl::kStatusOk);
    return result;
}

std::string status_text(int code)
{
    switch (code)
    {
    case fake_xl::kStatusOk: return "ok (fake)";
    case fake_xl::kStatusNothingToReceive: return "nothing to receive (fake)";
    case fake_xl::kStatusQueueFull: return "transmit queue full (fake)";
    case fake_xl::kStatusBadArgument: return "bad argument (fake)";
    case fake_xl::kStatusNoAccess: return "no access (fake)";
    case fake_xl::kStatusDriverUnavailable: return "driver unavailable (fake)";
    case fake_xl::kStatusNotOnBus: return "not on bus (fake)";
    default: return {};
    }
}

} // namespace vehicle_sil::vector_xl::detail::backend
