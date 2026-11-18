// SPDX-License-Identifier: MIT
//
// A SocketCAN interface as an ExternalCanPort of the CAN bridge.
#pragma once

#include "vehicle_sil/can_bridge/can_bridge.hpp"
#include "vehicle_sil/socketcan/can_device.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>

namespace vehicle_sil::socketcan {

struct SocketCanPortOptions
{
    /// When no frame this port sent reached the bus (came back as a receipt) for this long, transmit()
    /// refuses new frames (TransmitResult::Dropped) until a receipt comes, so that frames do not
    /// pile up in the kernel's queue and go out late in a burst. The time that counts is that of the oldest
    /// frame no receipt has answered (as in the XL port), not a count of frames: a receipt the kernel never
    /// gives back is forgotten once nothing was sent for this long, and when the interface comes up again.
    /// One frame still goes to the kernel as a probe each time this long has passed. Unset: frames wait
    /// as long as it takes.
    std::optional<std::chrono::milliseconds> transmit_timeout;
    /// How often the thread looks at the interface (up or down, still there, the controller's state).
    std::chrono::milliseconds watch_interval{250};
};

/// Whether the interface is up (IFF_UP): what a program can send to. A vcan interface is up as
/// soon as it exists and is set up; a CAN adapter that lost the bus keeps it up (the bus state says).
enum class LinkState
{
    Unknown,
    Up,
    Down,
};

/// One SocketCAN interface through a CanDevice.
///
/// Receiving (on a thread of the port): a frame this socket sent itself (the receipt the kernel
/// gives back for CAN_RAW_RECV_OWN_MSGS) is ExternalEventKind::OwnTransmission; every other frame is
/// a Frame, including those another program of this computer sent on the interface (candump, cangen
/// ...: they are the devices the bridge is for). An error frame is ErrorFrame and, where it says
/// the controller's state, BusState. The interface going down or coming back is a Warning and
/// link_state(); the interface gone (ENODEV, ENXIO, removed or renamed) is a Fault.
///
/// Sending: Ok; ENOBUFS and EAGAIN (the queue of the interface is full) are QueueFull; ENETDOWN
/// (the interface is down) is Failed and link_state() Down; ENODEV and ENXIO are Failed and a Fault.
///
/// Threading: transmit() from any thread; the handler from the port's thread, and from start() for the
/// first look at the interface (before the thread runs); start() and stop() from one thread at a time.
class SocketCanPort final : public can_bridge::ExternalCanPort
{
public:
    /// `device` must outlive the port.
    explicit SocketCanPort(CanDevice& device, SocketCanPortOptions options = {});
    ~SocketCanPort() override;

    SocketCanPort(const SocketCanPort&) = delete;
    SocketCanPort& operator=(const SocketCanPort&) = delete;

    /// Opens the device (CanDevice::open(); its exception propagates) and starts the thread.
    void start(EventHandler handler) override;
    void stop() noexcept override;
    can_bridge::TransmitResult transmit(const SilKit::Services::Can::CanFrame& frame) override;
    [[nodiscard]] std::string last_error() const override;

    [[nodiscard]] LinkState link_state() const noexcept { return link_.load(); }

private:
    /// The thread. `alive`: the interface was there at the start's look.
    void run(bool alive);
    /// Looks at the interface; false once it is gone (the Fault is reported).
    bool watch();
    void report_fault(const std::string& detail);
    void report_warning(const std::string& text);
    void report_bus_state(can_bridge::ExternalBusState::Level level, std::uint8_t tx_errors, std::uint8_t rx_errors);
    void note_receipt();
    void forget_unconfirmed();
    void set_last_error(std::string text);
    void request_fault(std::string detail);
    [[nodiscard]] std::optional<std::string> requested_fault() const;

    CanDevice& device_;
    const SocketCanPortOptions options_;

    std::mutex lifecycle_mutex_;
    /// Held shared by transmit() while it writes, exclusively while the device is closed.
    mutable std::shared_mutex device_mutex_;
    bool open_ = false; // guarded by device_mutex_
    std::thread thread_;
    EventHandler handler_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> faulted_{false};
    std::atomic<bool> watch_requested_{false}; ///< a send found the interface down
    std::atomic<LinkState> link_{LinkState::Unknown};

    // The thread only.
    can_bridge::ExternalBusState bus_state_;

    mutable std::mutex error_mutex_; // also guards requested_fault_
    std::string last_error_;
    std::optional<std::string> requested_fault_;

    // The transmit timeout: only the time of the oldest frame without a receipt counts, never a count of frames.
    std::mutex tx_mutex_;
    /// When the oldest frame sent since the last receipt was handed to the kernel (before write(), as the XL port does).
    std::optional<std::chrono::steady_clock::time_point> unconfirmed_since_;
    std::chrono::steady_clock::time_point last_attempt_; ///< the last transmit(), whether it sent or not
    std::chrono::steady_clock::time_point last_probe_;
};

} // namespace vehicle_sil::socketcan
