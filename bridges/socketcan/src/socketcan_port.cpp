// SPDX-License-Identifier: MIT
#include "vehicle_sil/socketcan/socketcan_port.hpp"

#include "vehicle_sil/socketcan/error_frame.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>

namespace vehicle_sil::socketcan {
namespace {

namespace can = SilKit::Services::Can;
using can_bridge::ExternalBusState;
using can_bridge::ExternalCanEvent;
using can_bridge::ExternalEventKind;
using can_bridge::TransmitResult;

constexpr std::uint32_t kExtendedFlag = 0x80000000u;
constexpr std::uint32_t kRemoteFlag = 0x40000000u;
constexpr std::uint32_t kExtendedMask = 0x1FFFFFFFu;
constexpr std::uint32_t kStandardMask = 0x7FFu;
/// Frames read in a row before the thread looks at the stop request again.
constexpr int kFramesPerWake = 64;
/// The pause after a wait that said "readable" and a read that found nothing.
constexpr auto kSpuriousWakePause = std::chrono::milliseconds(5);

constexpr can::CanFrameFlagMask flag(can::CanFrameFlag f) noexcept
{
    return static_cast<can::CanFrameFlagMask>(f);
}

} // namespace

SocketCanPort::SocketCanPort(CanDevice& device, SocketCanPortOptions options) : device_(device), options_(options) {}

SocketCanPort::~SocketCanPort()
{
    stop();
}

void SocketCanPort::start(EventHandler handler)
{
    const std::lock_guard lifecycle(lifecycle_mutex_);
    if (thread_.joinable())
    {
        return;
    }
    device_.open();
    {
        const std::unique_lock lock(device_mutex_);
        open_ = true;
    }
    handler_ = std::move(handler);
    stopping_.store(false);
    faulted_.store(false);
    watch_requested_.store(false);
    link_.store(LinkState::Unknown);
    bus_state_ = {};
    {
        const std::lock_guard errors(error_mutex_);
        last_error_.clear();
        requested_fault_.reset();
    }
    {
        const std::lock_guard tx(tx_mutex_);
        unconfirmed_since_.reset();
        last_attempt_ = {};
        last_probe_ = {};
    }
    // The first look before the thread runs: the first link line a program reads is not "unknown".
    const bool alive = watch();
    thread_ = std::thread([this, alive] { run(alive); });
}

void SocketCanPort::stop() noexcept
{
    const std::lock_guard lifecycle(lifecycle_mutex_);
    if (!thread_.joinable())
    {
        return;
    }
    stopping_.store(true);
    {
        const std::shared_lock lock(device_mutex_);
        device_.wake();
    }
    thread_.join();
    {
        const std::unique_lock lock(device_mutex_);
        open_ = false;
        device_.close();
    }
    handler_ = nullptr;
}

TransmitResult SocketCanPort::transmit(const can::CanFrame& frame)
{
    const std::shared_lock lock(device_mutex_);
    if (!open_)
    {
        set_last_error("the SocketCAN port is not started");
        return TransmitResult::Failed;
    }
    if (faulted_.load())
    {
        return TransmitResult::Failed; // last_error() tells the fault
    }
    const auto now = options_.clock ? options_.clock() : std::chrono::steady_clock::now();
    // The time of the oldest frame no receipt has answered, as it was before this frame (to put back if the write fails).
    std::optional<std::chrono::steady_clock::time_point> unconfirmed_before;
    if (options_.transmit_timeout)
    {
        const std::lock_guard tx(tx_mutex_);
        // Nothing was sent for a whole timeout: a receipt the kernel never gave back (a queue emptied by a bus-off or an
        // interface that went down, an overflow) must not make the next frames look stuck.
        if (unconfirmed_since_ && now - last_attempt_ >= *options_.transmit_timeout)
        {
            unconfirmed_since_.reset();
        }
        last_attempt_ = now;
        if (unconfirmed_since_ && now - *unconfirmed_since_ > *options_.transmit_timeout)
        {
            if (now - last_probe_ < *options_.transmit_timeout)
            {
                return TransmitResult::Dropped;
            }
            last_probe_ = now; // a probe: one frame goes to the kernel to find out whether the bus works again
        }
        unconfirmed_before = unconfirmed_since_;
        // Before the write: with a receipt that the kernel gives back inside write(), the thread may handle it first.
        if (!unconfirmed_since_)
        {
            unconfirmed_since_ = now;
        }
    }

    RawFrame raw;
    const bool extended = (frame.flags & flag(can::CanFrameFlag::Ide)) != 0;
    const bool remote = (frame.flags & flag(can::CanFrameFlag::Rtr)) != 0;
    raw.can_id = (frame.canId & (extended ? kExtendedMask : kStandardMask)) | (extended ? kExtendedFlag : 0u) |
                 (remote ? kRemoteFlag : 0u);
    raw.length = static_cast<std::uint8_t>(std::min<std::size_t>(frame.dlc, raw.data.size()));
    if (!remote)
    {
        std::copy_n(frame.dataField.begin(), std::min(frame.dataField.size(), raw.data.size()), raw.data.begin());
    }

    const int error = device_.write(raw);
    if (error == 0)
    {
        return TransmitResult::Ok;
    }
    if (options_.transmit_timeout)
    {
        const std::lock_guard tx(tx_mutex_);
        if (unconfirmed_since_ && unconfirmed_since_ == now)
        {
            unconfirmed_since_ = unconfirmed_before; // the frame did not go: it waits for no receipt
        }
    }
    const std::string name = std::strerror(error);
    const std::string where = "sending on '" + device_.name() + "' failed: " + name;
    if (error == ENOBUFS || error == EAGAIN || error == EWOULDBLOCK)
    {
        return TransmitResult::QueueFull;
    }
    if (error == ENETDOWN)
    {
        set_last_error(where + " (the interface is down: sudo ip link set " + device_.name() + " up)");
        if (link_.load() != LinkState::Down)
        {
            watch_requested_.store(true);
            device_.wake(); // the thread looks at the interface now
        }
    }
    else if (error == ENODEV || error == ENXIO)
    {
        set_last_error(where);
        request_fault("the interface is gone (removed while the bridge ran): " + name);
    }
    else
    {
        set_last_error(where);
    }
    return TransmitResult::Failed;
}

std::string SocketCanPort::last_error() const
{
    const std::lock_guard lock(error_mutex_);
    return last_error_;
}

void SocketCanPort::set_last_error(std::string text)
{
    const std::lock_guard lock(error_mutex_);
    last_error_ = std::move(text);
}

void SocketCanPort::request_fault(std::string detail)
{
    {
        const std::lock_guard lock(error_mutex_);
        if (!requested_fault_)
        {
            requested_fault_ = std::move(detail);
        }
    }
    device_.wake(); // the caller holds the device lock (shared)
}

std::optional<std::string> SocketCanPort::requested_fault() const
{
    const std::lock_guard lock(error_mutex_);
    return requested_fault_;
}

void SocketCanPort::report_fault(const std::string& detail)
{
    set_last_error(detail);
    faulted_.store(true);
    ExternalCanEvent event;
    event.kind = ExternalEventKind::Fault;
    event.detail = detail;
    handler_(event);
}

void SocketCanPort::report_warning(const std::string& text)
{
    ExternalCanEvent event;
    event.kind = ExternalEventKind::Warning;
    event.detail = text;
    handler_(event);
}

void SocketCanPort::report_bus_state(ExternalBusState::Level level, std::uint8_t tx_errors, std::uint8_t rx_errors)
{
    ExternalBusState state;
    state.level = level;
    state.tx_errors = tx_errors;
    state.rx_errors = rx_errors;
    if (state == bus_state_)
    {
        return;
    }
    bus_state_ = state;
    ExternalCanEvent event;
    event.kind = ExternalEventKind::BusState;
    event.bus_state = state;
    handler_(event);
}

void SocketCanPort::note_receipt()
{
    if (!options_.transmit_timeout)
    {
        return;
    }
    // A receipt: the bus took a frame. Whatever else is waiting is not known to be stuck; the next frame starts the clock again.
    forget_unconfirmed();
}

void SocketCanPort::forget_unconfirmed()
{
    const std::lock_guard tx(tx_mutex_);
    unconfirmed_since_.reset();
}

bool SocketCanPort::watch()
{
    std::optional<CanInterfaceInfo> info;
    try
    {
        info = device_.interface();
    }
    catch (const std::exception&)
    {
        return true; // the kernel could not be asked this time: the next look tries again
    }
    if (!info)
    {
        report_fault("the interface is gone (removed or renamed while the bridge ran)");
        return false;
    }
    const LinkState now = info->up ? LinkState::Up : LinkState::Down;
    if (const LinkState before = link_.exchange(now); before != now && before != LinkState::Unknown)
    {
        if (now == LinkState::Up && options_.transmit_timeout)
        {
            forget_unconfirmed(); // the kernel emptied the queue of the interface that went down
        }
        report_warning(now == LinkState::Down
                           ? "interface " + info->name + " is down: frames are not sent or received until it is up again (sudo ip link set " +
                                 info->name + " up)"
                           : "interface " + info->name + " is up again");
    }
    if (const auto level = bus_level_of(*info); level != ExternalBusState::Level::Unknown)
    {
        // The counters of the kernel's answer where it has them; else the last ones (from an error frame).
        report_bus_state(level, info->tx_errors.value_or(bus_state_.tx_errors), info->rx_errors.value_or(bus_state_.rx_errors));
    }
    return true;
}

void SocketCanPort::run(bool alive)
{
    auto watched = std::chrono::steady_clock::now();
    while (alive && !stopping_.load())
    {
        const WaitResult waited = device_.wait(options_.watch_interval);
        if (stopping_.load())
        {
            return;
        }
        if (const auto fault = requested_fault())
        {
            report_fault(*fault);
            return;
        }
        if (watch_requested_.exchange(false))
        {
            alive = watch(); // a send found the interface down: say so now, not at the next look
            watched = std::chrono::steady_clock::now();
            if (!alive)
            {
                return;
            }
        }
        if (waited == WaitResult::Readable)
        {
            for (int i = 0; i < kFramesPerWake && alive; ++i)
            {
                const ReadResult result = device_.read();
                if (result.status == ReadStatus::Empty)
                {
                    if (i == 0)
                    {
                        // Woken to read, and there was nothing (a socket flag that stays set): do not spin on it.
                        std::this_thread::sleep_for(kSpuriousWakePause);
                    }
                    break;
                }
                if (result.status == ReadStatus::Error)
                {
                    if (result.error == ENODEV || result.error == ENXIO)
                    {
                        report_fault("the interface is gone (removed while the bridge ran): " +
                                     std::string(std::strerror(result.error)));
                        return;
                    }
                    if (result.error == ENETDOWN)
                    {
                        alive = watch(); // says that it is down, and finds out whether it still is
                        watched = std::chrono::steady_clock::now();
                    }
                    else if (result.error != EINTR && result.error != ENOBUFS && result.error != ENOMEM)
                    {
                        report_fault("reading from the interface failed: " + std::string(std::strerror(result.error)));
                        return;
                    }
                    break;
                }
                const RawFrame& raw = result.frame;
                ExternalCanEvent event;
                if ((raw.can_id & kErrorFlag) != 0)
                {
                    event.kind = ExternalEventKind::ErrorFrame;
                    handler_(event);
                    const ErrorFrameState state = interpret_error_frame(raw.can_id, raw.data);
                    if (state.level)
                    {
                        report_bus_state(*state.level, state.tx_errors, state.rx_errors);
                    }
                    continue;
                }
                const bool extended = (raw.can_id & kExtendedFlag) != 0;
                const bool remote = (raw.can_id & kRemoteFlag) != 0;
                event.kind = raw.confirm ? ExternalEventKind::OwnTransmission : ExternalEventKind::Frame;
                event.frame.canId = raw.can_id & (extended ? kExtendedMask : kStandardMask);
                event.frame.flags = static_cast<can::CanFrameFlagMask>((extended ? flag(can::CanFrameFlag::Ide) : 0u) |
                                                                      (remote ? flag(can::CanFrameFlag::Rtr) : 0u));
                event.frame.dlc = raw.length;
                event.frame.dataField = SilKit::Util::Span<const std::uint8_t>(raw.data.data(), remote ? 0 : raw.length);
                if (raw.confirm)
                {
                    note_receipt();
                }
                handler_(event);
            }
        }
        if (alive && std::chrono::steady_clock::now() - watched >= options_.watch_interval)
        {
            alive = watch();
            watched = std::chrono::steady_clock::now();
        }
    }
}

} // namespace vehicle_sil::socketcan
