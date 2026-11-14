// SPDX-License-Identifier: MIT
//
// Bidirectional bridge between a SIL Kit Ethernet controller and an external Ethernet link
// (vehicle_sil::eth_link). Platform independent.
#pragma once

#include "vehicle_sil/eth_link/external_link.hpp"
#include "vehicle_sil/eth_link/frame_filter.hpp"
#include "vehicle_sil/eth_link/mac_side_table.hpp"

#include <silkit/services/ethernet/EthernetDatatypes.hpp>
#include <silkit/services/ethernet/fwd_decl.hpp>
#include <silkit/util/HandlerId.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::ethernet_bridge {

struct EthernetBridgeOptions
{
    /// Which frames pass, in both directions (eth_link::FrameFilter).
    eth_link::FrameFilterOptions filter;
    /// Drop frames by the source MAC rule of eth_link::MacSideTable. Off: whoever runs the bridge
    /// keeps frames from going around in a loop.
    bool mac_guard = true;
    /// Addresses the MAC table holds.
    std::size_t mac_table_size = eth_link::MacSideTable::kDefaultCapacity;
};

struct EthernetBridgeStatistics
{
    std::uint64_t silkit_to_ext = 0;      ///< Frames from SIL Kit sent to the external link.
    std::uint64_t ext_to_silkit = 0;      ///< Frames from the external link handed to SIL Kit.
    std::uint64_t own_tx_dropped = 0;     ///< The bridge's own frames that came back from the link.
    std::uint64_t loop_suspected = 0;     ///< Frames whose source MAC is on both sides.
    /// Frames forwarded without the check against loops: their source did not fit in the MAC table.
    std::uint64_t mac_unguarded = 0;
    std::uint64_t filtered_to_ext = 0;    ///< Frames from SIL Kit the filter did not let out.
    std::uint64_t filtered_from_ext = 0;  ///< Frames from the link the filter did not let in.
    std::uint64_t oversize = 0;           ///< Frames longer than the maximum (either direction).
    std::uint64_t malformed = 0;          ///< Frames shorter than an Ethernet header (either direction).
    std::uint64_t ext_overruns = 0;       ///< Received frames the OS lost before the link read them.
    std::uint64_t ext_queue_full = 0;     ///< Frames lost because the link's transmit queue was full.
    std::uint64_t ext_tx_failures = 0;    ///< Other failures to send on the link.
    /// SendFrame threw, or SIL Kit acknowledged a frame as not transmitted (that frame was
    /// counted in ext_to_silkit when it was handed over).
    std::uint64_t silkit_tx_failures = 0;
    /// Frames from the link handed to SIL Kit although their TCP or UDP checksum is not filled in
    /// (eth_link::LinkEvent::checksum_incomplete; they are counted in ext_to_silkit too).
    std::uint64_t ext_checksum_incomplete = 0;
};

/// One line "stats silkit_to_ext=N ext_to_silkit=N ..." with every counter. The key names are
/// part of the bridge's documented output (docs/ethernet-bridge.md); tests parse them.
[[nodiscard]] std::string format_statistics(const EthernetBridgeStatistics& statistics);

/// What the bridge knows about the link at one moment (EthernetBridge::health()).
struct EthernetBridgeHealth
{
    EthernetBridgeStatistics statistics;
    eth_link::LinkState link_state = eth_link::LinkState::Unknown;
};

/// Whether a problem counter grew from `before` to `now`: loop_suspected, mac_unguarded, oversize,
/// malformed, ext_overruns, ext_queue_full, ext_tx_failures, silkit_tx_failures, ext_checksum_incomplete.
/// The forwarded, filtered and own_tx_dropped counts are not problems.
[[nodiscard]] bool problems_grew(const EthernetBridgeStatistics& before, const EthernetBridgeStatistics& now) noexcept;

/// One line about the link between two samples taken by the caller (once a second), or nullopt:
///  - a problem counter grew, or the link state changed (except from unknown to up, the first
///    report of a healthy link):
///      "interface enp3s0: link down; since the last report: ext_tx_failures +12"
///  - nothing grew, but the previous sample had grown (`problems_before`):
///      "interface enp3s0: link up; no new problems since the last report"
[[nodiscard]] std::optional<std::string> format_health_change(const EthernetBridgeHealth& before,
                                                              const EthernetBridgeHealth& now,
                                                              std::string_view interface_name,
                                                              bool problems_before = false);

/// How many addresses on both sides a bridge names in warnings; the rest are only counted.
inline constexpr std::size_t kMaxNamedConflicts = 8;

/// Forwards Ethernet frames, unchanged, in both directions between a SIL Kit Ethernet controller
/// and an external link, through the frame filter and the MAC rule (EthernetBridgeOptions).
///
/// Loop prevention:
///  - the SIL Kit frame handler is registered for the RX direction only, so the bridge never sees
///    the frames it sends into SIL Kit itself;
///  - the MAC rule (eth_link::MacSideTable) drops frames from an address seen on both sides (a
///    second bridge between the same networks, or one address used on both) and warns. A frame
///    from the outside whose source was seen from SIL Kit is the bridge's own coming back if the
///    link echoes its transmissions (eth_link::ExternalLink::echoes_own_transmissions()): dropped
///    quietly. On such a link the bridge cannot find a second bridge from that frame, and a loop
///    through two bridges started by a device outside may go on. A second bridge is a
///    configuration error the rule finds when it can; it does not promise to stop every loop.
///
/// No queue of its own: SIL Kit -> link runs on SIL Kit's receive thread, link -> SIL Kit on the
/// link's receive thread. Nothing pushes back; lost frames are counted.
///
/// Ownership: the bridge does not own the controller or the link. Both must outlive it.
class EthernetBridge
{
public:
    /// Throws std::invalid_argument for options eth_link::FrameFilter or MacSideTable rejects.
    /// The filter's SIL Kit side MAC addresses are learned as on the SIL Kit side at once.
    EthernetBridge(SilKit::Services::Ethernet::IEthernetController& silkit, eth_link::ExternalLink& external,
                   EthernetBridgeOptions options = {});
    ~EthernetBridge();

    EthernetBridge(const EthernetBridge&) = delete;
    EthernetBridge& operator=(const EthernetBridge&) = delete;
    EthernetBridge(EthernetBridge&&) = delete;
    EthernetBridge& operator=(EthernetBridge&&) = delete;

    /// Starts the external link and registers the SIL Kit handlers: frames from SIL Kit go to the
    /// link from here on. Call it before the participant's lifecycle starts. The SIL Kit
    /// controller is activated by start_controller(); a start() after start_controller() was
    /// called (a restart) activates the controller as well. Throws on failure (eth_link::LinkError
    /// from the link) and leaves the bridge stopped.
    void start();

    /// Activates the SIL Kit controller: frames from the link go to SIL Kit from here on (those
    /// that came before are not forwarded and not counted). Call it from the participant's
    /// CommunicationReady handler (ParticipantRuntime::add_communication_ready_handler()), where
    /// SIL Kit wants IEthernetController::Activate() called: on its thread, before Running. A
    /// network simulator that the controller has not found yet does not learn of an earlier
    /// Activate(), and a second Activate() does nothing. Before start(), it only notes that the
    /// time has come. Nothing a second time. If the controller cannot be activated, fault() tells
    /// why.
    void start_controller() noexcept;

    /// Stops forwarding in both directions. Safe to call more than once.
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept { return running_.load(); }
    [[nodiscard]] EthernetBridgeStatistics statistics() const noexcept;
    /// The statistics and the link state the link last reported.
    [[nodiscard]] EthernetBridgeHealth health() const;
    /// Set once the link reported eth_link::LinkEventKind::Fault: why it stopped working. The
    /// bridge keeps running; the caller decides to stop. start() clears it.
    [[nodiscard]] std::optional<std::string> fault() const;
    /// The warnings since the last call, oldest first: the link's, and the bridge's own (an
    /// address on both sides, the MAC table full).
    [[nodiscard]] std::vector<std::string> take_warnings();
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] const eth_link::FrameFilter& filter() const noexcept { return filter_; }

private:
    void on_silkit_frame(const SilKit::Services::Ethernet::EthernetFrameEvent& event);
    void on_silkit_transmit(const SilKit::Services::Ethernet::EthernetFrameTransmitEvent& event);
    void on_external_event(const eth_link::LinkEvent& event);
    void on_external_frame(std::span<const std::uint8_t> frame, bool checksum_incomplete);
    /// Whether a frame from the outside whose source is on the SIL Kit side is the bridge's own
    /// coming back. Today: whether the link echoes its transmissions at all. (A link that echoes
    /// could keep a record of the frames sent and match them here, to find a second bridge.)
    [[nodiscard]] bool is_own_frame_coming_back(std::span<const std::uint8_t> frame) const noexcept;
    /// Undoes start(): removes the SIL Kit frame handler, stops the link if it was started,
    /// removes the transmit handler and deactivates the controller if it was activated.
    void teardown(bool external_started, bool silkit_activated) noexcept;
    void set_last_error(std::string_view message, std::string_view detail = {});
    void warn(std::string text);
    /// `seen_first`, `seen_now`: "the external link" or "SIL Kit".
    void warn_conflict(const eth_link::MacAddress& address, std::string_view seen_first, std::string_view seen_now);
    void warn_table_full();

    SilKit::Services::Ethernet::IEthernetController& silkit_;
    eth_link::ExternalLink& external_;
    const eth_link::FrameFilter filter_;
    const std::unique_ptr<eth_link::MacSideTable> table_; // null without the MAC guard

    std::mutex lifecycle_mutex_;
    std::optional<SilKit::Util::HandlerId> frame_handler_;
    std::optional<SilKit::Util::HandlerId> transmit_handler_;
    std::atomic<bool> running_{false};
    std::atomic<bool> controller_started_{false}; ///< the controller is activated (until stop())
    bool communication_ready_ = false;            ///< start_controller() was called; guarded by lifecycle_mutex_
    bool link_echoes_ = false; // set by start() before the link starts

    std::atomic<std::uint64_t> silkit_to_ext_{0};
    std::atomic<std::uint64_t> ext_to_silkit_{0};
    std::atomic<std::uint64_t> own_tx_dropped_{0};
    std::atomic<std::uint64_t> loop_suspected_{0};
    std::atomic<std::uint64_t> mac_unguarded_{0};
    std::atomic<std::uint64_t> filtered_to_ext_{0};
    std::atomic<std::uint64_t> filtered_from_ext_{0};
    std::atomic<std::uint64_t> oversize_{0};
    std::atomic<std::uint64_t> malformed_{0};
    std::atomic<std::uint64_t> ext_overruns_{0};
    std::atomic<std::uint64_t> ext_queue_full_{0};
    std::atomic<std::uint64_t> ext_tx_failures_{0};
    std::atomic<std::uint64_t> silkit_tx_failures_{0};
    std::atomic<std::uint64_t> ext_checksum_incomplete_{0};

    mutable std::mutex error_mutex_; // also guards link_state_, fault_, warnings_, named_conflicts_
    std::string last_error_;
    eth_link::LinkState link_state_ = eth_link::LinkState::Unknown;
    std::optional<std::string> fault_;
    std::vector<std::string> warnings_;
    std::size_t named_conflicts_ = 0;
};

} // namespace vehicle_sil::ethernet_bridge
