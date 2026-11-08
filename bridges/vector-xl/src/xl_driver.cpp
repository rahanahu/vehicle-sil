// SPDX-License-Identifier: MIT
//
// The XL backend logic, written against the project's own driver interface (xl_backend.hpp).
// Platform independent: on Windows it is linked with xl_backend_vector.cpp, in the tests with
// the fake in tests/vector_xl/fake_xl.
#include "vehicle_sil/vector_xl/xl_driver.hpp"

#include "xl_backend.hpp"
#include "xl_events.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

namespace vehicle_sil::vector_xl {
namespace detail {
namespace {

namespace can = SilKit::Services::Can;

constexpr can::CanFrameFlagMask flag(can::CanFrameFlag f) noexcept
{
    return static_cast<can::CanFrameFlagMask>(f);
}

} // namespace

ClassifiedXlEvent classify_xl_event(const backend::ReceivedEvent& event) noexcept
{
    ClassifiedXlEvent result;
    result.overrun = event.queue_overrun;
    if (!event.is_can_message)
    {
        return result;
    }

    result.overrun = result.overrun || event.controller_overrun;
    if (event.error_frame)
    {
        result.event_class = XlEventClass::ErrorFrame;
        return result;
    }

    const auto& msg = event.message;
    result.frame.canId = msg.id & (msg.extended ? 0x1FFF'FFFFu : 0x7FFu);
    result.frame.flags = static_cast<can::CanFrameFlagMask>((msg.extended ? flag(can::CanFrameFlag::Ide) : 0u) |
                                                            (msg.remote ? flag(can::CanFrameFlag::Rtr) : 0u));
    result.frame.dlc = msg.dlc;
    const std::size_t length = msg.remote ? 0 : std::min<std::size_t>(msg.dlc, msg.data.size());
    result.frame.dataField = SilKit::Util::Span<const std::uint8_t>(msg.data.data(), length);
    result.event_class = (event.transmit_receipt || event.transmit_request) ? XlEventClass::OwnTransmission
                                                                             : XlEventClass::Frame;
    return result;
}

backend::CanMessage to_transmit_message(const can::CanFrame& frame) noexcept
{
    backend::CanMessage msg;
    msg.extended = (frame.flags & flag(can::CanFrameFlag::Ide)) != 0;
    msg.remote = (frame.flags & flag(can::CanFrameFlag::Rtr)) != 0;
    msg.id = frame.canId;
    // The XL driver transmits a DLC of 0..8. Classic CAN DLC 9..15 also means 8 data bytes, so
    // the frame keeps its payload but leaves with DLC 8.
    msg.dlc = static_cast<std::uint8_t>(std::min<std::size_t>(frame.dlc, msg.data.size()));
    std::copy_n(frame.dataField.begin(), std::min(frame.dataField.size(), msg.data.size()), msg.data.begin());
    return msg;
}

} // namespace detail

namespace {

namespace backend = detail::backend;

std::string status_text(int code)
{
    auto text = backend::status_text(code);
    return text.empty() ? "XL status " + std::to_string(code) : text;
}

void check(const char* call, const backend::Status& status)
{
    if (!status.ok())
    {
        throw XlError(call, status.code, status_text(status.code));
    }
}

} // namespace

XlError::XlError(const std::string& call, int status, const std::string& text) :
  std::runtime_error(call + " failed: " + text + " (XL status " + std::to_string(status) + ")"),
  status_(status)
{
}

XlDriver::XlDriver()
{
    check("xlOpenDriver", backend::open_driver());
}

XlDriver::~XlDriver()
{
    backend::close_driver();
}

std::vector<XlChannelInfo> XlDriver::channels() const
{
    std::vector<XlChannelInfo> channels;
    check("xlGetDriverConfig", backend::read_channels(channels));
    return channels;
}

struct XlCanPort::Impl
{
    XlChannelInfo channel;
    XlCanPortOptions options;
    backend::ChannelMask access_mask = 0;
    backend::ChannelMask permission_mask = 0;
    backend::PortHandle port{};
    backend::NotificationHandle notification{};

    std::mutex xl_mutex; // serialises every driver call on `port`
    std::mutex lifecycle_mutex;
    std::thread receive_thread;
    std::atomic<bool> running{false};
    bool active = false;
    EventHandler handler;

    mutable std::mutex error_mutex;
    std::string last_error;

    void set_last_error(std::string message)
    {
        const std::lock_guard lock(error_mutex);
        last_error = std::move(message);
    }

    void receive_loop();
    void dispatch(const backend::ReceivedEvent& event);
};

XlCanPort::XlCanPort(XlDriver& /*driver*/, const XlChannelInfo& channel, XlCanPortOptions options) :
  impl_(std::make_unique<Impl>())
{
    auto& impl = *impl_;
    impl.channel = channel;
    impl.options = std::move(options);
    impl.access_mask = channel.channel_mask;
    if (impl.access_mask == 0)
    {
        throw std::invalid_argument("XL channel '" + channel.name + "' has no channel mask");
    }

    // Ask for init access only when we want to configure the channel.
    const backend::ChannelMask requested = impl.options.bitrate ? impl.access_mask : 0;
    const auto opened =
        backend::open_port(impl.options.application_name, impl.access_mask, requested, impl.options.rx_queue_size);
    if (!opened.status.ok() && opened.port)
    {
        (void)backend::close_port(*opened.port); // close the handle even when the open failed
    }
    check("xlOpenPort", opened.status);
    if (!opened.port)
    {
        throw XlError("xlOpenPort", opened.status.code, "returned an invalid port handle");
    }
    impl.port = *opened.port;
    impl.permission_mask = opened.init_access;
    try
    {
        if (impl.options.bitrate)
        {
            if ((impl.permission_mask & impl.access_mask) == 0)
            {
                throw XlError("xlOpenPort", opened.status.code,
                              "no init access to '" + channel.name +
                                  "' (another application configures it), cannot set the bit rate");
            }
            check("xlCanSetChannelBitrate", backend::set_bitrate(impl.port, impl.access_mask, *impl.options.bitrate));
        }
        // No TX receipts: the driver does not echo our own frames back to this port. This needs
        // init access; without it the driver keeps its default (receipts on) and
        // classify_xl_event() drops the receipts instead.
        if ((impl.permission_mask & impl.access_mask) != 0)
        {
            check("xlCanSetChannelMode", backend::set_transmit_receipts(impl.port, impl.access_mask, false, false));
        }
        check("xlSetNotification", backend::enable_notification(impl.port, impl.notification));
    }
    catch (...)
    {
        (void)backend::close_port(impl.port);
        throw;
    }
}

XlCanPort::~XlCanPort()
{
    stop();
    (void)backend::close_port(impl_->port);
}

void XlCanPort::start(EventHandler handler)
{
    auto& impl = *impl_;
    const std::lock_guard lifecycle(impl.lifecycle_mutex);
    if (impl.running.load())
    {
        return;
    }
    impl.handler = std::move(handler);
    {
        const std::lock_guard lock(impl.xl_mutex);
        check("xlActivateChannel", backend::activate(impl.port, impl.access_mask));
        impl.active = true;
    }
    impl.running.store(true);
    impl.receive_thread = std::thread([&impl] { impl.receive_loop(); });
}

void XlCanPort::stop() noexcept
{
    auto& impl = *impl_;
    const std::lock_guard lifecycle(impl.lifecycle_mutex);
    impl.running.store(false);
    if (impl.receive_thread.joinable())
    {
        impl.receive_thread.join();
    }
    const std::lock_guard lock(impl.xl_mutex);
    if (impl.active)
    {
        (void)backend::deactivate(impl.port, impl.access_mask);
        impl.active = false;
    }
}

TransmitResult XlCanPort::transmit(const SilKit::Services::Can::CanFrame& frame)
{
    auto& impl = *impl_;
    const auto message = detail::to_transmit_message(frame);
    backend::TransmitAttempt result;
    {
        const std::lock_guard lock(impl.xl_mutex);
        if (!impl.active)
        {
            impl.set_last_error("XL channel is not active");
            return TransmitResult::Failed;
        }
        result = backend::transmit(impl.port, impl.access_mask, message);
    }
    if (result.status.ok() && result.queued)
    {
        return TransmitResult::Ok;
    }
    if (result.status.outcome == backend::Outcome::TransmitQueueFull)
    {
        return TransmitResult::QueueFull;
    }
    impl.set_last_error("xlCanTransmit failed: " + status_text(result.status.code));
    return TransmitResult::Failed;
}

std::string XlCanPort::last_error() const
{
    const std::lock_guard lock(impl_->error_mutex);
    return impl_->last_error;
}

bool XlCanPort::has_init_access() const noexcept
{
    return (impl_->permission_mask & impl_->access_mask) != 0;
}

const XlChannelInfo& XlCanPort::channel() const noexcept
{
    return impl_->channel;
}

void XlCanPort::Impl::receive_loop()
{
    while (running.load())
    {
        backend::ReceiveResult received;
        {
            const std::lock_guard lock(xl_mutex);
            received = backend::receive(port);
        }
        if (received.status.ok() && received.event)
        {
            dispatch(*received.event);
            continue; // drain the queue before waiting
        }
        if (!received.status.ok() && received.status.outcome != backend::Outcome::NothingToReceive)
        {
            set_last_error("xlReceive failed: " + status_text(received.status.code));
        }
        (void)backend::wait_for_notification(notification, options.wait_timeout);
    }
}

void XlCanPort::Impl::dispatch(const backend::ReceivedEvent& event)
{
    const auto classified = detail::classify_xl_event(event);
    if (classified.overrun)
    {
        handler(ExternalCanEvent{ExternalEventKind::Overrun, {}});
    }
    switch (classified.event_class)
    {
    case detail::XlEventClass::Frame:
        handler(ExternalCanEvent{ExternalEventKind::Frame, classified.frame});
        break;
    case detail::XlEventClass::OwnTransmission:
        handler(ExternalCanEvent{ExternalEventKind::OwnTransmission, classified.frame});
        break;
    case detail::XlEventClass::ErrorFrame:
        handler(ExternalCanEvent{ExternalEventKind::ErrorFrame, {}});
        break;
    case detail::XlEventClass::Ignored:
        break;
    }
}

} // namespace vehicle_sil::vector_xl
