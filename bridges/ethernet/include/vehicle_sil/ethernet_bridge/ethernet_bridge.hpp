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

/// Whether a problem counter grew from `before` to `now`: loop_suspected, oversize, malformed,
/// ext_overruns, ext_queue_full, ext_tx_failures, silkit_tx_failures. The forwarded, filtered
/// and own_tx_dropped counts are not problems.
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
///  - the MAC rule (eth_link::MacSideTable) drops the bridge's own frames when the link delivers
///    them back, and frames from an address on both sides (a second bridge, or one address on
///    both sides), whatever the OS does.
///
/// No queue of its own: SIL Kit -> link runs on SIL Kit's receive thread, link -> SIL Kit on the
/// link's receive thread. Nothing pushes back; lost frames are counted.
///
/// Ownership: the bridge does not own the controller or the link. Both must outlive it.
class EthernetBridge
{
public:
    /// Throws std::invalid_argument for options eth_link::FrameFilter or MacSideTable rejects.
    EthernetBridge(SilKit::Services::Ethernet::IEthernetController& silkit, eth_link::ExternalLink& external,
                   EthernetBridgeOptions options = {});
    ~EthernetBridge();

    EthernetBridge(const EthernetBridge&) = delete;
    EthernetBridge& operator=(const EthernetBridge&) = delete;
    EthernetBridge(EthernetBridge&&) = delete;
    EthernetBridge& operator=(EthernetBridge&&) = delete;

    /// Starts the external link, registers the SIL Kit handlers and activates the controller.
    /// Throws on failure (eth_link::LinkError from the link) and leaves the bridge stopped.
    void start();

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
    void on_external_frame(std::span<const std::uint8_t> frame);
    /// Undoes start(): removes the SIL Kit frame handler, stops the link if it was started,
    /// removes the transmit handler and deactivates the controller if it was activated.
    void teardown(bool external_started, bool silkit_activated) noexcept;
    void set_last_error(std::string_view message, std::string_view detail = {});
    void warn(std::string text);
    void warn_conflict(const eth_link::MacAddress& address);
    void warn_table_full();

    SilKit::Services::Ethernet::IEthernetController& silkit_;
    eth_link::ExternalLink& external_;
    const eth_link::FrameFilter filter_;
    const std::unique_ptr<eth_link::MacSideTable> table_; // null without the MAC guard

    std::mutex lifecycle_mutex_;
    std::optional<SilKit::Util::HandlerId> frame_handler_;
    std::optional<SilKit::Util::HandlerId> transmit_handler_;
    std::atomic<bool> running_{false};

    std::atomic<std::uint64_t> silkit_to_ext_{0};
    std::atomic<std::uint64_t> ext_to_silkit_{0};
    std::atomic<std::uint64_t> own_tx_dropped_{0};
    std::atomic<std::uint64_t> loop_suspected_{0};
    std::atomic<std::uint64_t> filtered_to_ext_{0};
    std::atomic<std::uint64_t> filtered_from_ext_{0};
    std::atomic<std::uint64_t> oversize_{0};
    std::atomic<std::uint64_t> malformed_{0};
    std::atomic<std::uint64_t> ext_overruns_{0};
    std::atomic<std::uint64_t> ext_queue_full_{0};
    std::atomic<std::uint64_t> ext_tx_failures_{0};
    std::atomic<std::uint64_t> silkit_tx_failures_{0};

    mutable std::mutex error_mutex_; // also guards link_state_, fault_, warnings_, named_conflicts_
    std::string last_error_;
    eth_link::LinkState link_state_ = eth_link::LinkState::Unknown;
    std::optional<std::string> fault_;
    std::vector<std::string> warnings_;
    std::size_t named_conflicts_ = 0;
};

} // namespace vehicle_sil::ethernet_bridge
