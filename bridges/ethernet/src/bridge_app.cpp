// SPDX-License-Identifier: MIT
#include "vehicle_sil/ethernet_bridge/bridge_app.hpp"

#include "vehicle_sil/eth_link/frame_filter.hpp"

#include <utility>

namespace vehicle_sil::ethernet_bridge {

HostOs host_os() noexcept
{
#if defined(__linux__)
    return HostOs::Linux;
#elif defined(_WIN32)
    return HostOs::Windows;
#else
    return HostOs::Other;
#endif
}

std::optional<std::string> backend_unavailable(std::optional<Backend> backend, HostOs os)
{
    if (os != HostOs::Linux)
    {
        const std::string subject = backend ? "--backend " + std::string(to_string(*backend)) + " cannot be used here"
                                            : std::string("there is no interface to list here");
        return subject + ": this version of vehicle_sil_ethernet_bridge has no backend for " +
               (os == HostOs::Windows ? "Windows" : "this operating system") +
               " (tap and packet are for Linux; npcap and pcap are not part of this version). Run the bridge on a Linux "
               "computer that reaches the same SIL Kit registry.";
    }
    if (backend == Backend::Npcap)
    {
        return "--backend npcap is for Windows. On Linux use --backend packet (a network interface) or --backend tap (a TAP "
               "device).";
    }
    if (backend == Backend::Pcap)
    {
        return "--backend pcap is not part of this version. Use --backend packet (a network interface) or --backend tap (a "
               "TAP device).";
    }
    return std::nullopt;
}

std::string format_start_line(const BridgeOptions& options, std::string_view link)
{
    std::string line = "bridging SIL Kit network '" + options.network + "' (participant '" + options.participant + "', " +
                       options.registry_uri + ") <-> " + std::string(link) + ": " +
                       eth_link::FrameFilter(options.bridge.filter).describe();
    if (options.backend && reaches_real_adapters(*options.backend))
    {
        line += options.promiscuous ? "; promiscuous on"
                                    : "; promiscuous off (only frames to the interface's own address and to group addresses "
                                      "arrive)";
    }
    line += options.bridge.mac_guard ? "; MAC guard on" : "; MAC guard off";
    return line + "; Ctrl+C to stop";
}

PromiscuousHint::PromiscuousHint(eth_link::ExternalLink& link, std::chrono::milliseconds after, Clock clock) :
  link_(link),
  after_(after),
  clock_(std::move(clock))
{
}

void PromiscuousHint::start(EventHandler handler)
{
    for (auto& address : addresses_)
    {
        address.store(0);
    }
    more_addresses_.store(false);
    first_sent_.store(kNotSent);
    answered_.store(false);
    warned_ = false;
    link_.start([this, handler = std::move(handler)](const eth_link::LinkEvent& event) {
        if (event.kind == eth_link::LinkEventKind::Frame && !answered_.load(std::memory_order_relaxed))
        {
            if (const auto destination = eth_link::destination_of(event.frame);
                destination && !destination->is_group() && is_silkit_address(*destination))
            {
                answered_.store(true);
            }
        }
        handler(event);
    });
}

eth_link::TransmitResult PromiscuousHint::transmit(std::span<const std::uint8_t> frame)
{
    const auto result = link_.transmit(frame);
    if (result == eth_link::TransmitResult::Ok && !answered_.load(std::memory_order_relaxed))
    {
        if (const auto source = eth_link::source_of(frame); source && !source->is_group())
        {
            sent_from(*source);
        }
        if (first_sent_.load() == kNotSent)
        {
            auto expected = kNotSent;
            first_sent_.compare_exchange_strong(expected, clock_().time_since_epoch().count());
        }
    }
    return result;
}

void PromiscuousHint::sent_from(const eth_link::MacAddress& source) noexcept
{
    const std::uint64_t wanted = source.value() | kUsed;
    for (auto& address : addresses_)
    {
        std::uint64_t held = address.load();
        if (held == 0 && address.compare_exchange_strong(held, wanted))
        {
            return;
        }
        if (held == wanted)
        {
            return;
        }
    }
    more_addresses_.store(true);
}

bool PromiscuousHint::is_silkit_address(const eth_link::MacAddress& address) const noexcept
{
    if (more_addresses_.load())
    {
        return true;
    }
    const std::uint64_t wanted = address.value() | kUsed;
    for (const auto& held : addresses_)
    {
        if (held.load() == wanted)
        {
            return true;
        }
    }
    return false;
}

std::optional<std::string> PromiscuousHint::take_warning()
{
    if (warned_ || answered_.load())
    {
        return std::nullopt;
    }
    const auto sent = first_sent_.load();
    if (sent == kNotSent)
    {
        return std::nullopt;
    }
    const auto since = clock_().time_since_epoch() - std::chrono::steady_clock::duration(sent);
    if (since < after_)
    {
        return std::nullopt;
    }
    warned_ = true;
    return "no frame to a MAC address of the SIL Kit side has come from " + link_.describe() + " in the " +
           std::to_string(std::chrono::duration_cast<std::chrono::seconds>(after_).count()) +
           " s since the bridge first sent a frame there, and --promiscuous is off: without it an interface passes on only "
           "frames to its own address and to group addresses, so frames to the SIL Kit devices (they have other addresses) do "
           "not arrive. If devices outside are meant to answer, add --promiscuous.";
}

} // namespace vehicle_sil::ethernet_bridge
