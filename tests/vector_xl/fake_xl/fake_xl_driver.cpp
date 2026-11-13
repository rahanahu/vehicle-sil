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
    std::deque<Frame> tx_queue; // accepted while the bus is blocked
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
    std::map<unsigned, backend::ChipState> chip_states;
    std::map<unsigned, bool> blocked;
    std::map<unsigned, unsigned> receipts_to_lose;
    bool activate_fails = false;
    std::map<unsigned, bool> bus_off_persists;
    std::map<unsigned, unsigned> activations;
    bool device_lost = false;
    bool chip_state_refused = false;
    std::optional<std::chrono::steady_clock::time_point> lose_device_at;
    unsigned chip_state_requests = 0;
};

// Caller holds state().mutex.
bool device_lost(State& s)
{
    if (s.lose_device_at && std::chrono::steady_clock::now() >= *s.lose_device_at)
    {
        s.device_lost = true;
        s.lose_device_at.reset();
    }
    return s.device_lost;
}

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
    s.chip_states.clear();
    s.blocked.clear();
    s.receipts_to_lose.clear();
    s.activate_fails = false;
    s.bus_off_persists.clear();
    s.activations.clear();
    s.device_lost = false;
    s.chip_state_refused = false;
    s.lose_device_at.reset();
    s.chip_state_requests = 0;
}

void set_activate_fails(bool fails)
{
    const std::lock_guard lock(state().mutex);
    state().activate_fails = fails;
}

void set_bus_off_persists(unsigned channel_index, bool persists)
{
    const std::lock_guard lock(state().mutex);
    state().bus_off_persists[channel_index] = persists;
}

unsigned activations(unsigned channel_index)
{
    const std::lock_guard lock(state().mutex);
    return state().activations[channel_index];
}

void set_chip_state_refused(bool refused)
{
    const std::lock_guard lock(state().mutex);
    state().chip_state_refused = refused;
}

void set_chip_state(unsigned channel_index, backend::ChipState chip)
{
    const std::lock_guard lock(state().mutex);
    state().chip_states[channel_index] = chip;
}

void set_device_lost(bool lost)
{
    const std::lock_guard lock(state().mutex);
    state().device_lost = lost;
    state().lose_device_at.reset();
}

unsigned chip_state_requests()
{
    const std::lock_guard lock(state().mutex);
    return state().chip_state_requests;
}

// Sends `frame` from `port` onto its bus, with the receipt the port asked for. Caller holds the mutex.
void send_from(std::int64_t handle, Port& port, const Frame& frame)
{
    deliver_to_bus(port.channel, frame, handle, 0);
    if (auto& lose = state().receipts_to_lose[port.channel]; lose > 0)
    {
        --lose;
        return;
    }
    if (port.tx_receipts || state().ignore_mode)
    {
        auto receipt = rx_event(frame);
        receipt.transmit_receipt = true;
        enqueue(port, receipt);
    }
}

void set_bus_blocked(unsigned channel_index, bool blocked)
{
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    s.blocked[channel_index] = blocked;
    if (blocked)
    {
        return;
    }
    for (auto& [handle, port] : s.ports)
    {
        if (port.channel != channel_index)
        {
            continue;
        }
        while (!port.tx_queue.empty())
        {
            const Frame frame = port.tx_queue.front();
            port.tx_queue.pop_front();
            send_from(handle, port, frame);
        }
    }
}

void lose_next_receipt(unsigned channel_index)
{
    const std::lock_guard lock(state().mutex);
    ++state().receipts_to_lose[channel_index];
}

void discard_queued_transmissions(unsigned channel_index)
{
    const std::lock_guard lock(state().mutex);
    for (auto& [handle, port] : state().ports)
    {
        if (port.channel == channel_index)
        {
            port.tx_queue.clear();
        }
    }
}

std::size_t queued_transmissions(unsigned channel_index)
{
    const std::lock_guard lock(state().mutex);
    std::size_t count = 0;
    for (const auto& [handle, port] : state().ports)
    {
        if (port.channel == channel_index)
        {
            count += port.tx_queue.size();
        }
    }
    return count;
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
    if (const char* lose = std::getenv("VEHICLE_SIL_FAKE_XL_LOSE_DEVICE_AFTER_MS"); lose != nullptr && *lose != '\0')
    {
        s.lose_device_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::strtoul(lose, nullptr, 10));
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
    if (state().activate_fails)
    {
        return status(fake_xl::kStatusDeviceLost);
    }
    port->active = true;
    ++state().activations[port->channel];
    // Like a real controller, activation resets it: bus off ends, unless set_bus_off_persists.
    if (auto it = state().chip_states.find(port->channel); it != state().chip_states.end() &&
                                                            it->second.level == backend::ChipLevel::BusOff &&
                                                            !state().bus_off_persists[port->channel])
    {
        it->second = backend::ChipState{backend::ChipLevel::ErrorActive, 0, 0};
    }
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
    port->tx_queue.clear();
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
    if (fake_xl::device_lost(s))
    {
        result.status = status(fake_xl::kStatusDeviceLost);
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
    if (s.blocked[port->channel])
    {
        port->tx_queue.push_back(frame);
    }
    else
    {
        fake_xl::send_from(fake_xl::key(handle), *port, frame);
    }
    result.status = status(fake_xl::kStatusOk);
    result.queued = true;
    return result;
}

Status request_chip_state(PortHandle handle, ChannelMask channels)
{
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    ++s.chip_state_requests;
    auto* port = fake_xl::find_port(handle, channels);
    if (port == nullptr)
    {
        return status(fake_xl::kStatusBadArgument);
    }
    if (fake_xl::device_lost(s))
    {
        return status(fake_xl::kStatusDeviceLost);
    }
    if (!port->active)
    {
        return status(fake_xl::kStatusNotOnBus);
    }
    if (s.chip_state_refused)
    {
        return status(fake_xl::kStatusNoAccess);
    }
    if (s.queue_full)
    {
        return status(fake_xl::kStatusQueueFull);
    }
    ReceivedEvent answer;
    const auto it = s.chip_states.find(port->channel);
    answer.chip_state = it != s.chip_states.end() ? it->second : ChipState{ChipLevel::ErrorActive, 0, 0};
    fake_xl::enqueue(*port, answer);
    return status(fake_xl::kStatusOk);
}

ReceiveResult receive(PortHandle handle)
{
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    ReceiveResult result;
    auto* port = fake_xl::find_port(handle);
    if (port == nullptr)
    {
        result.status = status(fake_xl::kStatusBadArgument);
        return result;
    }
    if (port->rx.empty() || fake_xl::device_lost(s))
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
    case fake_xl::kStatusDeviceLost: return "device lost (fake)";
    default: return {};
    }
}

} // namespace vehicle_sil::vector_xl::detail::backend
