// SPDX-License-Identifier: MIT
//
// Vector XL Driver Library access for the CAN bridge. Vector's types stay in the implementation
// (src/xl_backend_vector.cpp), so including this header does not require vxlapi.h.
#pragma once

#include "vehicle_sil/vector_xl/can_bridge.hpp"
#include "vehicle_sil/vector_xl/channel_selection.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace vehicle_sil::vector_xl {

/// An XL API call failed. `status()` is the XLstatus value.
class XlError : public std::runtime_error
{
public:
    XlError(const std::string& call, int status, const std::string& text);
    [[nodiscard]] int status() const noexcept { return status_; }

private:
    int status_;
};

/// RAII for xlOpenDriver() / xlCloseDriver().
class XlDriver
{
public:
    XlDriver();
    ~XlDriver();

    XlDriver(const XlDriver&) = delete;
    XlDriver& operator=(const XlDriver&) = delete;
    XlDriver(XlDriver&&) = delete;
    XlDriver& operator=(XlDriver&&) = delete;

    /// All channels from xlGetDriverConfig(), in driver order.
    [[nodiscard]] std::vector<XlChannelInfo> channels() const;
};

struct XlCanPortOptions
{
    /// Application name passed to xlOpenPort (shown in Vector Hardware Config).
    std::string application_name = "vehicle-sil";
    /// Bit rate to configure, 15000 ... 1000000 bit/s. Requires init access to the channel; if
    /// another application holds it, opening the port fails instead of silently running with a
    /// different bit rate.
    std::optional<std::uint32_t> bitrate;
    /// XL receive queue size (power of two, 16 ... 32768). The driver holds rx_queue_size - 1
    /// events, drops events beyond that and reports an overrun.
    std::uint32_t rx_queue_size = 16384;
    /// Upper bound for one wait on the XL notification event; bounds how long stop() waits.
    std::chrono::milliseconds wait_timeout{50};
    /// How often the running port asks the driver for the controller's error state
    /// (ExternalEventKind::BusState). 0 disables the queries.
    ///  - If the driver refuses the first queries before it ever answered one, the setup does
    ///    not support them: the port reports ExternalEventKind::Warning once and stops asking.
    ///  - After it answered, a query refused bus_state_failures times in a row means that the
    ///    channel is gone (device removed): the port reports ExternalEventKind::Fault once and
    ///    stops receiving.
    ///  - A full driver queue is not a refusal (the answer only comes later).
    std::chrono::milliseconds bus_state_interval{500};
    /// When no receipt came for this long although frames were accepted since the last one (no
    /// ACK, bus off, a cable pulled), transmit() refuses new frames (TransmitResult::Dropped)
    /// until a receipt comes, so that frames do not pile up and go out late in a burst. One
    /// frame per this long still goes to the driver as a probe, in case the driver lost the
    /// waiting frames. The frames already accepted, about this long's worth, still go out once
    /// the bus works: the driver's xlCanFlushTransmitQueue did not remove them on a VN5620.
    /// Unset: frames wait as long as it takes. Like drain_timeout and TransmitUnsent, this
    /// relies on the driver's transmit receipts: they are used once the port switched them on
    /// (init access, `bitrate`) or one arrived; without init access on a bus that never
    /// acknowledged a frame, it does nothing.
    std::optional<std::chrono::milliseconds> transmit_timeout;
    /// stop() waits up to this long for accepted frames to reach the bus before it takes the
    /// channel off the bus; the rest is reported as ExternalEventKind::TransmitUnsent.
    std::chrono::milliseconds drain_timeout{1000};
    /// Refusals in a row, after the driver answered, that count as a lost channel.
    unsigned bus_state_failures = 3;
};

/// One Vector XL CAN channel opened as an ExternalCanPort.
///
/// The port reports any event flagged XL_CAN_MSG_FLAG_TX_COMPLETED or XL_CAN_MSG_FLAG_TX_REQUEST
/// as ExternalEventKind::OwnTransmission, so the bridge never forwards its own frames back. With
/// init access (a bit rate was given) it also switches the receipts off
/// (xlCanSetChannelMode(tx=0, txrq=0), which needs init access); without it the driver keeps
/// sending them and they are only dropped.
///
/// Frames are transmitted with a DLC of at most 8 (xlCanTransmit takes 0..8): a classic DLC of
/// 9..15, which also means 8 data bytes, leaves as DLC 8. The channel is activated without
/// resetting the hardware clock (the bridge does not use timestamps).
///
/// Threading: all XL calls on the port are serialised by a mutex. Events are delivered on a
/// receive thread owned by the port.
class XlCanPort final : public ExternalCanPort
{
public:
    /// Opens the port (xlOpenPort), applies the bit rate if requested, disables TX receipts and
    /// sets up the notification event. Throws XlError.
    XlCanPort(XlDriver& driver, const XlChannelInfo& channel, XlCanPortOptions options = {});
    ~XlCanPort() override;

    XlCanPort(const XlCanPort&) = delete;
    XlCanPort& operator=(const XlCanPort&) = delete;
    XlCanPort(XlCanPort&&) = delete;
    XlCanPort& operator=(XlCanPort&&) = delete;

    /// Activates the channel (goes on bus) and starts the receive thread.
    void start(EventHandler handler) override;
    /// Stops the receive thread and deactivates the channel.
    void stop() noexcept override;
    TransmitResult transmit(const SilKit::Services::Can::CanFrame& frame) override;
    [[nodiscard]] std::string last_error() const override;

    /// true if this application got init access (and could configure the bit rate).
    [[nodiscard]] bool has_init_access() const noexcept;
    [[nodiscard]] const XlChannelInfo& channel() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vehicle_sil::vector_xl
