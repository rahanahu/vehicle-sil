// SPDX-License-Identifier: MIT
#include "vehicle_sil/ethernet_bridge/bridge_app.hpp"

#include "vehicle_sil/eth_link/frame_filter.hpp"

#include <algorithm>
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
    if (os == HostOs::Linux)
    {
        if (backend == Backend::Npcap)
        {
            return "--backend npcap is for Windows. On Linux use --backend packet or --backend pcap (a network interface), or "
                   "--backend tap (a TAP device).";
        }
        return std::nullopt;
    }
    if (os == HostOs::Windows)
    {
        if (backend && *backend != Backend::Npcap)
        {
            return "--backend " + std::string(to_string(*backend)) +
                   " is for Linux. On Windows use --backend npcap (a network adapter through Npcap).";
        }
        return std::nullopt;
    }
    const std::string subject = backend ? "--backend " + std::string(to_string(*backend)) + " cannot be used here"
                                        : std::string("there is no interface to list here");
    return subject + ": vehicle_sil_ethernet_bridge has no backend for this operating system (tap, packet and pcap are for "
                     "Linux; npcap is for Windows). Run the bridge on a Linux or Windows computer that reaches the same SIL Kit "
                     "registry.";
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
    for (auto& address : received_for_)
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
            if (const auto destination = eth_link::destination_of(event.frame); destination && !destination->is_group())
            {
                if (is_silkit_address(*destination))
                {
                    answered_.store(true);
                }
                else
                {
                    // Not known as an address of the SIL Kit side: the bridge may send from it
                    // later (take_warning() looks). With no room it is not remembered, and that
                    // says nothing about the interface: what this computer itself sends on it
                    // comes in here too, to as many addresses as it talks to.
                    static_cast<void>(remember(received_for_, *destination));
                }
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
        if (const auto source = eth_link::source_of(frame); source && !source->is_group() && !remember(addresses_, *source))
        {
            more_addresses_.store(true);
        }
        if (first_sent_.load() == kNotSent)
        {
            auto expected = kNotSent;
            first_sent_.compare_exchange_strong(expected, clock_().time_since_epoch().count());
        }
    }
    return result;
}

bool PromiscuousHint::remember(Addresses& held, const eth_link::MacAddress& address) noexcept
{
    const std::uint64_t wanted = address.value() | kUsed;
    for (auto& entry : held)
    {
        std::uint64_t now = entry.load();
        if (now == 0 && entry.compare_exchange_strong(now, wanted))
        {
            return true;
        }
        if (now == wanted)
        {
            return true;
        }
    }
    return false;
}

bool PromiscuousHint::holds(const Addresses& held, std::uint64_t entry) noexcept
{
    return std::ranges::any_of(held, [entry](const auto& one) { return one.load() == entry; });
}

bool PromiscuousHint::is_silkit_address(const eth_link::MacAddress& address) const noexcept
{
    return more_addresses_.load() || holds(addresses_, address.value() | kUsed);
}

bool PromiscuousHint::answered_before_sending() const noexcept
{
    return std::ranges::any_of(received_for_, [this](const auto& one) {
        const std::uint64_t entry = one.load();
        return entry != 0 && (more_addresses_.load() || holds(addresses_, entry));
    });
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
    if (answered_before_sending())
    {
        answered_.store(true);
        return std::nullopt;
    }
    warned_ = true;
    answered_.store(true); // said once: nothing more to keep track of
    return "no frame to a MAC address of the SIL Kit side has come from " + link_.describe() + " in the " +
           std::to_string(std::chrono::duration_cast<std::chrono::seconds>(after_).count()) +
           " s since the bridge first sent a frame there, and --promiscuous is off: without it an interface passes on only "
           "frames to its own address and to group addresses, so frames to the SIL Kit devices (they have other addresses) do "
           "not arrive. If devices outside are meant to answer, add --promiscuous.";
}

} // namespace vehicle_sil::ethernet_bridge
