// SPDX-License-Identifier: MIT
//
// What the bridge executable says and decides outside the forwarding itself: which backend can be
// used on which operating system, the line it prints when it starts, and the hint it gives when
// answers do not arrive without promiscuous mode. Platform independent so it can be tested anywhere.
#pragma once

#include "vehicle_sil/eth_link/external_link.hpp"
#include "vehicle_sil/eth_link/mac_address.hpp"
#include "vehicle_sil/ethernet_bridge/bridge_options.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vehicle_sil::ethernet_bridge {

enum class HostOs
{
    Linux,
    Windows,
    Other,
};

/// The operating system this program was built for.
[[nodiscard]] HostOs host_os() noexcept;

/// Why `backend` cannot be used on `os`, and what to do instead; nullopt if it can. Without a
/// backend (--list-interfaces alone): why no backend at all can be used on `os`, or nullopt.
/// tap, packet and pcap are for Linux, npcap is for Windows; nothing elsewhere.
[[nodiscard]] std::optional<std::string> backend_unavailable(std::optional<Backend> backend, HostOs os);

/// The line the bridge prints once it forwards (without the "[eth-bridge] " prefix):
///   "bridging SIL Kit network 'ETH1' (participant 'EthernetBridge', silkit://127.0.0.1:8500) <->
///    packet 'enp3s0' (network interface): EtherType 0x88b5 only, both ways; promiscuous off
///    (only frames to the interface's own address and to group addresses arrive); MAC guard on;
///    Ctrl+C to stop"
/// `link`: eth_link::ExternalLink::describe(). The promiscuous part is left out for tap.
[[nodiscard]] std::string format_start_line(const BridgeOptions& options, std::string_view link);

/// Wraps a link that is not in promiscuous mode and notices the usual sign of that being the
/// mistake: the bridge sent frames out, and no frame to a MAC address of the SIL Kit side came
/// in for a while. (Without promiscuous mode an adapter passes on only frames to its own
/// address and to group addresses; the SIL Kit devices have other addresses.)
///
/// A frame that came in before the bridge sent from its destination counts too: the addresses
/// frames came in for are remembered until the SIL Kit side is known. An interface that passes
/// on frames to more addresses than are remembered does not filter by address, so the warning
/// is not given there either.
///
/// Everything else is passed to the wrapped link unchanged. The contract of
/// eth_link::ExternalLink holds: transmit() takes no lock and does not block.
class PromiscuousHint final : public eth_link::ExternalLink
{
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// How many source addresses of sent frames are remembered; with more, any frame to an
    /// individual address counts as an answer. Also how many destination addresses of received
    /// frames are remembered; with more, the interface is taken not to filter by address.
    static constexpr std::size_t kMaxAddresses = 32;

    /// `link` must outlive this object. `after`: how long after the first frame sent the warning
    /// is due. `clock`: the time (tests give their own).
    explicit PromiscuousHint(eth_link::ExternalLink& link, std::chrono::milliseconds after = std::chrono::seconds(5),
                             Clock clock = [] { return std::chrono::steady_clock::now(); });

    /// The warning, once, when it is due: a frame was sent `after` or longer ago and no frame to
    /// an address of the SIL Kit side has come in, before or after. Call it from one thread.
    [[nodiscard]] std::optional<std::string> take_warning();

    void start(EventHandler handler) override;
    void stop() noexcept override { link_.stop(); }
    eth_link::TransmitResult transmit(std::span<const std::uint8_t> frame) override;
    [[nodiscard]] bool echoes_own_transmissions() const noexcept override { return link_.echoes_own_transmissions(); }
    [[nodiscard]] std::string describe() const override { return link_.describe(); }
    [[nodiscard]] std::string last_error() const override { return link_.last_error(); }

private:
    using Addresses = std::array<std::atomic<std::uint64_t>, kMaxAddresses>;
    /// Puts `address` into `held` if it is not there; false if there is no room for it.
    static bool remember(Addresses& held, const eth_link::MacAddress& address) noexcept;
    [[nodiscard]] static bool holds(const Addresses& held, std::uint64_t entry) noexcept;
    [[nodiscard]] bool is_silkit_address(const eth_link::MacAddress& address) const noexcept;
    /// A frame came in earlier for an address the bridge has sent from since.
    [[nodiscard]] bool answered_before_sending() const noexcept;

    eth_link::ExternalLink& link_;
    const std::chrono::milliseconds after_;
    const Clock clock_;

    /// Addresses frames were sent from: the address's value with kUsed set, 0 while free.
    static constexpr std::uint64_t kUsed = std::uint64_t{1} << 48;
    Addresses addresses_{};
    std::atomic<bool> more_addresses_{false};
    /// Individual addresses frames came in for while they were not known as SIL Kit's; same form.
    Addresses received_for_{};
    /// When the first frame was sent (the clock's count), kNotSent before.
    static constexpr std::int64_t kNotSent = -1;
    std::atomic<std::int64_t> first_sent_{kNotSent};
    std::atomic<bool> answered_{false};
    bool warned_ = false;
};

} // namespace vehicle_sil::ethernet_bridge
