// SPDX-License-Identifier: MIT
//
// Bidirectional classic CAN bridge between a SIL Kit CAN controller and an external CAN port
// (a Vector XL channel in production, a fake in tests). Platform independent.
#pragma once

#include <silkit/services/can/CanDatatypes.hpp>
#include <silkit/services/can/fwd_decl.hpp>
#include <silkit/util/HandlerId.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace vehicle_sil::vector_xl {

/// What an external CAN port reports to the bridge.
enum class ExternalEventKind
{
    Frame,           ///< A frame another node put on the external bus: forwarded to SIL Kit.
    OwnTransmission, ///< Receipt of a frame this port transmitted itself: never forwarded (loop prevention).
    ErrorFrame,      ///< CAN error frame on the external bus: counted, not forwarded.
    Overrun,         ///< The external receive queue overflowed and frames were lost: counted.
};

struct ExternalCanEvent
{
    ExternalEventKind kind = ExternalEventKind::Frame;
    /// Valid for Frame and OwnTransmission. dataField is only valid during the handler call.
    SilKit::Services::Can::CanFrame frame{};
};

enum class TransmitResult
{
    Ok,
    QueueFull, ///< The external transmit queue is full; the frame was not sent.
    Failed,    ///< Any other error (port closed, driver error); see last_error().
};

/// The external side of a CanBridge.
///
/// Threading: transmit() may be called from any thread, also concurrently with the receive
/// handler. The handler is called from one thread at a time.
class ExternalCanPort
{
public:
    using EventHandler = std::function<void(const ExternalCanEvent&)>;

    virtual ~ExternalCanPort() = default;

    /// Goes on bus and starts delivering events to `handler`. Throws std::runtime_error on failure.
    virtual void start(EventHandler handler) = 0;
    /// Stops delivering events. When it returns, the handler is no longer running or called.
    virtual void stop() noexcept = 0;
    /// Sends one classic CAN frame (already validated by the bridge).
    virtual TransmitResult transmit(const SilKit::Services::Can::CanFrame& frame) = 0;
    [[nodiscard]] virtual std::string last_error() const = 0;
};

struct CanBridgeStatistics
{
    std::uint64_t silkit_to_external = 0;        ///< Frames received from SIL Kit and sent to the external bus.
    std::uint64_t external_to_silkit = 0;        ///< Frames received from the external bus and sent to SIL Kit.
    std::uint64_t own_transmissions_dropped = 0; ///< Receipts of the bridge's own external transmissions.
    std::uint64_t external_error_frames = 0;
    std::uint64_t external_overruns = 0;
    std::uint64_t silkit_rejected = 0;           ///< SIL Kit frames that are not classic CAN (FD, XL, bad DLC).
    std::uint64_t external_rejected = 0;         ///< External frames that failed validation.
    std::uint64_t external_queue_full = 0;       ///< Frames lost because the external transmit queue was full.
    std::uint64_t external_transmit_failures = 0;
    std::uint64_t silkit_transmit_failures = 0;  ///< ICanController::SendFrame threw.
};

/// One line "stats silkit_to_xl=N xl_to_silkit=N ..." with every counter. The key names are
/// part of the bridge's documented output (docs/architecture.md); tests parse them.
[[nodiscard]] std::string format_statistics(const CanBridgeStatistics& statistics);

/// Checks that `frame` is a classic CAN 2.0 frame the bridge can carry: no CAN FD/XL flags,
/// an 11 or 29 bit identifier, DLC 0..15 with a data field of min(DLC, 8) bytes (0 bytes for a
/// remote frame). @returns an empty string if valid, otherwise a static reason.
[[nodiscard]] std::string_view validate_classic_frame(const SilKit::Services::Can::CanFrame& frame) noexcept;

/// Forwards classic CAN frames in both directions between a SIL Kit CAN controller and an
/// external port, preserving identifier, IDE, RTR, DLC and data.
///
/// Loop prevention:
///  - the SIL Kit frame handler is registered for the RX direction only, so the bridge never sees
///    the frames it sends into SIL Kit itself;
///  - receipts of the bridge's own external transmissions (ExternalEventKind::OwnTransmission)
///    are dropped.
/// Running two bridges between the same SIL Kit network and the same external bus still forms a
/// loop; that configuration is not supported.
///
/// Ownership: the bridge does not own the controller or the port. Both must outlive it.
class CanBridge
{
public:
    CanBridge(SilKit::Services::Can::ICanController& silkit, ExternalCanPort& external);
    ~CanBridge();

    CanBridge(const CanBridge&) = delete;
    CanBridge& operator=(const CanBridge&) = delete;
    CanBridge(CanBridge&&) = delete;
    CanBridge& operator=(CanBridge&&) = delete;

    /// Starts the external port, registers the SIL Kit RX handler and starts the controller.
    /// Throws on failure (and leaves the bridge stopped).
    void start();

    /// Stops forwarding in both directions. Safe to call more than once.
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept { return running_.load(); }
    [[nodiscard]] CanBridgeStatistics statistics() const noexcept;
    [[nodiscard]] std::string last_error() const;

private:
    void on_silkit_frame(const SilKit::Services::Can::CanFrameEvent& event);
    void on_external_event(const ExternalCanEvent& event);
    /// Undoes start(): stops the external port if it was started, removes the SIL Kit handler
    /// and stops the controller if it was started.
    void teardown(bool external_started, bool silkit_started) noexcept;
    void set_last_error(std::string_view message, std::string_view detail = {});

    SilKit::Services::Can::ICanController& silkit_;
    ExternalCanPort& external_;
    std::mutex lifecycle_mutex_;
    std::optional<SilKit::Util::HandlerId> silkit_handler_;
    std::atomic<bool> running_{false};

    std::atomic<std::uint64_t> silkit_to_external_{0};
    std::atomic<std::uint64_t> external_to_silkit_{0};
    std::atomic<std::uint64_t> own_transmissions_dropped_{0};
    std::atomic<std::uint64_t> external_error_frames_{0};
    std::atomic<std::uint64_t> external_overruns_{0};
    std::atomic<std::uint64_t> silkit_rejected_{0};
    std::atomic<std::uint64_t> external_rejected_{0};
    std::atomic<std::uint64_t> external_queue_full_{0};
    std::atomic<std::uint64_t> external_transmit_failures_{0};
    std::atomic<std::uint64_t> silkit_transmit_failures_{0};

    mutable std::mutex error_mutex_;
    std::string last_error_;
};

} // namespace vehicle_sil::vector_xl
