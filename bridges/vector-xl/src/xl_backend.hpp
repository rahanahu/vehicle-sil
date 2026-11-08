// SPDX-License-Identifier: MIT
//
// Internal: the operations the XL backend logic (xl_driver.cpp) needs from a CAN driver
// library, in vehicle-sil's own names, types and values. Exactly one implementation is linked:
//  - xl_backend_vector.cpp (Windows): calls Vector's XL Driver Library and only translates
//    types and values; it holds no state and makes no decisions.
//  - tests/vector_xl/fake_xl: an in-process fake for the tests on every platform.
// Not a public header.
#pragma once

#include "vehicle_sil/vector_xl/channel_selection.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::vector_xl::detail::backend {

/// What a call's result means to the logic.
enum class Outcome
{
    Ok,
    NothingToReceive, ///< receive(): the receive queue was empty
    TransmitQueueFull, ///< transmit(): the frame was not queued, try later
    Failed,           ///< anything else; `code` and status_text() tell what
};

struct Status
{
    Outcome outcome = Outcome::Ok;
    /// The library's own status number, passed through unchanged (reported as XlError::status()).
    int code = 0;

    [[nodiscard]] bool ok() const noexcept { return outcome == Outcome::Ok; }
};

/// Bit set of driver channels; one bit per channel, as in XlChannelInfo::channel_mask.
using ChannelMask = std::uint64_t;

/// An open port of the library.
enum class PortHandle : std::int64_t
{
};

/// Something receive-ready can be waited for with wait_for_notification(). Opaque.
using NotificationHandle = void*;

inline constexpr std::size_t kMaxDataBytes = 8;

/// A classic CAN message as the library sends and receives it.
struct CanMessage
{
    std::uint32_t id = 0; ///< without any extended-format marker
    bool extended = false;
    bool remote = false;
    std::uint8_t dlc = 0; ///< received: 0..15 as on the bus; transmitted: 0..8
    std::array<std::uint8_t, kMaxDataBytes> data{};
};

/// One entry of the library's receive queue.
struct ReceivedEvent
{
    /// true for a CAN message event (a frame, an error frame or a transmit receipt); false for
    /// anything else the library reports (state changes, timers, ...), which the logic ignores.
    bool is_can_message = false;
    /// The library lost events before this one (its queue was full).
    bool queue_overrun = false;

    // Meaningful when is_can_message:
    bool error_frame = false;
    bool transmit_receipt = false;  ///< our own frame, reported after it was sent
    bool transmit_request = false;  ///< our own frame, reported when it was handed to the controller
    bool controller_overrun = false; ///< the CAN controller lost messages before this one
    CanMessage message;
};

// ---- Driver -------------------------------------------------------------------------------

[[nodiscard]] Status open_driver();
void close_driver() noexcept;
/// Every channel the driver knows, in driver order.
[[nodiscard]] Status read_channels(std::vector<XlChannelInfo>& out);

// ---- Port ---------------------------------------------------------------------------------

struct OpenedPort
{
    Status status;
    /// Set whenever the library handed out a handle, even if `status` is a failure; the caller
    /// closes it.
    std::optional<PortHandle> port;
    /// The channels of the request that this application may configure (bit rate, receipts).
    ChannelMask init_access = 0;
};

/// Opens a CAN port on `channels` with a receive queue of `rx_queue_size` entries, asking for
/// init access to `request_init_access` (0: none).
[[nodiscard]] OpenedPort open_port(const std::string& application_name, ChannelMask channels,
                                   ChannelMask request_init_access, std::uint32_t rx_queue_size);
Status close_port(PortHandle port) noexcept;

/// Needs init access.
[[nodiscard]] Status set_bitrate(PortHandle port, ChannelMask channels, std::uint32_t bitrate);
/// Whether the library reports our own transmitted frames back (transmit_receipt /
/// transmit_request). Needs init access.
[[nodiscard]] Status set_transmit_receipts(PortHandle port, ChannelMask channels, bool receipts, bool requests);

/// Sets `out` to a handle that is signalled whenever the port's receive queue holds an event.
[[nodiscard]] Status enable_notification(PortHandle port, NotificationHandle& out);
/// Waits until `handle` is signalled or `timeout` passes. true if it was signalled.
bool wait_for_notification(NotificationHandle handle, std::chrono::milliseconds timeout) noexcept;

/// Goes on bus, keeping the hardware clock running (no clock reset).
[[nodiscard]] Status activate(PortHandle port, ChannelMask channels);
Status deactivate(PortHandle port, ChannelMask channels) noexcept;

struct TransmitAttempt
{
    Status status;
    bool queued = false; ///< the library took the message
};
[[nodiscard]] TransmitAttempt transmit(PortHandle port, ChannelMask channels, const CanMessage& message);

struct ReceiveResult
{
    Status status;
    std::optional<ReceivedEvent> event; ///< at most one event per call
};
[[nodiscard]] ReceiveResult receive(PortHandle port);

/// The library's description of a status code, or an empty string if it has none.
[[nodiscard]] std::string status_text(int code);

} // namespace vehicle_sil::vector_xl::detail::backend
