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
#include <vector>

namespace vehicle_sil::vector_xl {

/// What an external CAN port reports to the bridge.
enum class ExternalEventKind
{
    Frame,           ///< A frame another node put on the external bus: forwarded to SIL Kit.
    OwnTransmission, ///< Receipt of a frame this port transmitted itself: never forwarded (loop prevention).
    ErrorFrame,      ///< CAN error frame on the external bus: counted, not forwarded.
    Overrun,         ///< The external receive queue overflowed and frames were lost: counted.
    BusState,        ///< The CAN controller's error state (the port asks for it periodically): bus_state.
    /// The port stopped working (device removed, driver failure); `detail` tells why. No
    /// further events follow; frames are neither sent nor received any more.
    Fault,
    /// `count` accepted frames had not reached the bus when the port went off the bus, and are
    /// lost: counted. Reported by stop(), and when the port restarts a channel to leave bus off.
    TransmitUnsent,
    /// Something the user should know that does not stop the port (e.g. the driver does not
    /// answer the bus state queries, so the bus is not monitored); `detail` tells what.
    Warning,
};

/// Error state of the external CAN controller.
struct ExternalBusState
{
    enum class Level
    {
        Unknown, ///< not reported (yet)
        ErrorActive,
        ErrorWarning, ///< an error counter reached the warning limit; still error active
        ErrorPassive,
        BusOff,
    };
    Level level = Level::Unknown;
    std::uint8_t tx_errors = 0; ///< transmit error counter
    std::uint8_t rx_errors = 0; ///< receive error counter

    [[nodiscard]] friend bool operator==(const ExternalBusState&, const ExternalBusState&) noexcept = default;
};

[[nodiscard]] std::string_view to_string(ExternalBusState::Level level) noexcept;

struct ExternalCanEvent
{
    ExternalEventKind kind = ExternalEventKind::Frame;
    /// Valid for Frame and OwnTransmission. dataField is only valid during the handler call.
    SilKit::Services::Can::CanFrame frame{};
    /// Valid for BusState.
    ExternalBusState bus_state{};
    /// Valid for TransmitUnsent.
    std::uint64_t count = 0;
    /// Valid for Fault and Warning, during the handler call only.
    std::string_view detail{};
};

enum class TransmitResult
{
    Ok,
    QueueFull, ///< The external transmit queue is full; the frame was not sent.
    Failed,    ///< Any other error (port closed, driver error); see last_error().
    /// Not sent on purpose: the frames sent before have not reached the bus for longer than the
    /// port's transmit timeout (XlCanPortOptions::transmit_timeout).
    Dropped,
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
    std::uint64_t external_transmit_dropped = 0; ///< Frames not sent because the bus was stuck (TransmitResult::Dropped).
    std::uint64_t external_transmit_unsent = 0;  ///< Accepted frames lost unsent (at stop, at a bus-off restart).
};

/// One line "stats silkit_to_xl=N xl_to_silkit=N ..." with every counter. The key names are
/// part of the bridge's documented output (docs/architecture.md); tests parse them.
[[nodiscard]] std::string format_statistics(const CanBridgeStatistics& statistics);

/// What the bridge knows about the external bus at one moment (CanBridge::health()).
struct CanBridgeHealth
{
    CanBridgeStatistics statistics;
    ExternalBusState bus_state;
};

/// Whether a problem counter (everything in CanBridgeStatistics except the forwarded and
/// own-transmission counts) grew from `before` to `now`.
[[nodiscard]] bool problems_grew(const CanBridgeStatistics& before, const CanBridgeStatistics& now) noexcept;

/// "error passive (tx errors 128, rx errors 0)", or "state unknown".
[[nodiscard]] std::string format_bus_state(const ExternalBusState& state);

/// One line about the external bus between two samples taken by the caller (the bridge
/// executable samples once a second), or nullopt:
///  - a problem counter grew, or the controller's error state changed (except from Unknown to
///    error active, the first answer of a healthy bus):
///      "XL bus: error passive (tx errors 128, rx errors 0); since the last report: error_frames +3650"
///  - nothing grew, but the previous sample had grown (`problems_before`): the problems stopped.
///    The error state may stay error passive (a controller that only transmits keeps its
///    receive error counter until it receives again), so this line is the sign of recovery:
///      "XL bus: error passive (tx errors 0, rx errors 128); no new problems since the last report"
[[nodiscard]] std::optional<std::string> format_health_change(const CanBridgeHealth& before, const CanBridgeHealth& now,
                                                              bool problems_before = false);

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
    /// The statistics and the external controller's last reported error state.
    [[nodiscard]] CanBridgeHealth health() const;
    /// Set once the external port reported ExternalEventKind::Fault: why it stopped working.
    /// The bridge keeps running; the caller decides to stop (the executable exits with 1).
    /// start() clears it.
    [[nodiscard]] std::optional<std::string> fault() const;
    /// The ExternalEventKind::Warning texts reported since the last call, oldest first.
    [[nodiscard]] std::vector<std::string> take_warnings();
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
    std::atomic<std::uint64_t> external_transmit_dropped_{0};
    std::atomic<std::uint64_t> external_transmit_unsent_{0};

    mutable std::mutex error_mutex_; // also guards bus_state_ and fault_
    std::string last_error_;
    ExternalBusState bus_state_;
    std::optional<std::string> fault_;
    std::vector<std::string> warnings_;
};

} // namespace vehicle_sil::vector_xl
