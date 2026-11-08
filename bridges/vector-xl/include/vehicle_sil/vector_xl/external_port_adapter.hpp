// SPDX-License-Identifier: MIT
//
// An ExternalCanPort (a Vector XL channel) as a vehicle_sil::can::Port of real time.
#pragma once

#include "vehicle_sil/can/port.hpp"
#include "vehicle_sil/can/real_time_clock.hpp"
#include "vehicle_sil/can/real_time_receiver.hpp"
#include "vehicle_sil/vector_xl/can_bridge.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace vehicle_sil::vector_xl {

struct ExternalPortAdapterOptions
{
    /// Receive inbox in frames (> 0, allocated once): the most frames that can arrive between
    /// two steps of the driver.
    std::size_t inbox_capacity = 1024;
    /// Event inbox (> 0, allocated once).
    std::size_t event_capacity = 1024;
};

/// What an ExternalPortAdapter has seen. Counters only grow.
struct ExternalPortAdapterStatistics
{
    std::uint64_t received = 0;                  ///< Frames queued for the stack.
    std::uint64_t receive_rejected = 0;          ///< Frames that are not classic CAN data frames (remote frames).
    std::uint64_t receive_overflows = 0;         ///< Frames dropped because the inbox was full.
    std::uint64_t own_transmissions_dropped = 0; ///< Receipts of the port's own transmissions.
    std::uint64_t error_frames = 0;              ///< Error frames on the external bus.
    std::uint64_t external_overruns = 0;         ///< Overruns the external port reported.
    std::uint64_t sent = 0;                      ///< Frames the external port took.
    std::uint64_t send_queue_full = 0;           ///< send() calls that returned QueueFull.
    std::uint64_t send_failed = 0;               ///< send() calls that returned Failed.
    std::uint64_t event_overflows = 0;           ///< Events dropped because the event inbox was full.
};

/// Makes an ExternalCanPort (XlCanPort on Windows, a fake in tests) a vehicle_sil::can::Port of
/// TimeDomain::RealTime, for a host that a can::RealTimeDriver steps with the same clock.
///
/// The constructor starts the external port (it goes on bus); close() or the destructor stops
/// it. After close() returns, the external port no longer calls into the adapter.
///
/// Receiving: the external port's events arrive on its receive thread and are stamped with
/// their arrival on the clock (can::RealTimeReceiver); the hardware timestamps are not used
/// (another clock). Frame: queued for the stack (remote frames are dropped and counted).
/// OwnTransmission: dropped (the stack loops its own frames back itself). ErrorFrame:
/// PortEvent::ErrorFrame. Overrun: PortEvent::ReceiveOverflow with an unknown count (0). The
/// external port reports no bus state, so the adapter reports none.
///
/// Sending: ExternalCanPort::transmit() on the driver's thread; Ok is Accepted, QueueFull is
/// QueueFull, Failed is Failed. Whether the frame then left the bus is not reported (the
/// transmit receipts of Vector XL depend on init access).
///
/// Threading: like every port, release_before(), release_events_before() and send() on the
/// driver's thread. close() from any thread except the external port's handler.
class ExternalPortAdapter final : public can::Port
{
public:
    /// Starts `external` (ExternalCanPort::start(); its exception propagates). `external` and
    /// `clock` must outlive the adapter.
    ExternalPortAdapter(ExternalCanPort& external, const can::RealTimeClock& clock, ExternalPortAdapterOptions options = {});
    ~ExternalPortAdapter() override;

    ExternalPortAdapter(const ExternalPortAdapter&) = delete;
    ExternalPortAdapter& operator=(const ExternalPortAdapter&) = delete;

    [[nodiscard]] can::TimeDomain time_domain() const noexcept override { return can::TimeDomain::RealTime; }
    std::size_t release_before(SimTime now, can::FrameSink sink) override;
    can::SendResult send(const can::TimedFrame& frame) override;
    std::size_t release_events_before(SimTime now, can::PortEventSink sink) override;
    void set_wakeup(can::Wakeup wakeup) override;

    /// Stops the external port and discards what is queued. Idempotent. send() afterwards
    /// returns Failed; release_before() releases nothing.
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return open_.load(std::memory_order_acquire); }
    [[nodiscard]] ExternalPortAdapterStatistics statistics() const;

private:
    void on_event(const ExternalCanEvent& event);

    ExternalCanPort& external_;
    can::RealTimeReceiver receiver_;
    std::mutex lifecycle_mutex_;
    std::atomic<bool> open_{false};

    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> receive_rejected_{0};
    std::atomic<std::uint64_t> own_transmissions_dropped_{0};
    std::atomic<std::uint64_t> error_frames_{0};
    std::atomic<std::uint64_t> external_overruns_{0};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> send_queue_full_{0};
    std::atomic<std::uint64_t> send_failed_{0};
};

} // namespace vehicle_sil::vector_xl
