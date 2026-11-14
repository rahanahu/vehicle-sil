// SPDX-License-Identifier: MIT
//
// This computer's addresses that other computers may reach, and the line run writes about them
// (run.hpp, detail::host_addresses() and detail::other_computers_line()).
#include "vehicle_sil/launch/run.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace vehicle_sil::launch {

namespace {

void add_once(std::vector<detail::HostAddress>& to, std::string host, bool default_route)
{
    if (host.empty())
    {
        return;
    }
    for (auto& a : to)
    {
        if (a.host == host)
        {
            a.default_route = a.default_route || default_route;
            return;
        }
    }
    to.push_back({std::move(host), default_route});
}

/// The address of `address` if it is IPv4 or IPv6 and one of `ipv4`, `ipv6`, as reachable_*_text().
std::string reachable_text(const sockaddr* address, bool ipv4, bool ipv6)
{
    if (address == nullptr)
    {
        return {};
    }
    if (address->sa_family == AF_INET && ipv4)
    {
        const auto* in = reinterpret_cast<const sockaddr_in*>(address);
        return detail::reachable_ipv4_text(reinterpret_cast<const std::uint8_t*>(&in->sin_addr));
    }
    if (address->sa_family == AF_INET6 && ipv6)
    {
        const auto* in = reinterpret_cast<const sockaddr_in6*>(address);
        return detail::reachable_ipv6_text(reinterpret_cast<const std::uint8_t*>(&in->sin6_addr));
    }
    return {};
}

#ifndef _WIN32
std::string read_text(const char* path)
{
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
#endif

} // namespace

std::string detail::reachable_ipv4_text(const std::uint8_t* b)
{
    if (b[0] == 0 || b[0] == 127 || (b[0] == 169 && b[1] == 254))
    {
        return {};
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return text;
}

std::string detail::reachable_ipv6_text(const std::uint8_t* b)
{
    const bool zeros10 = std::all_of(b, b + 10, [](std::uint8_t x) { return x == 0; });
    const bool zeros15 = zeros10 && std::all_of(b + 10, b + 15, [](std::uint8_t x) { return x == 0; });
    if ((zeros15 && b[15] <= 1) ||                     // :: and ::1
        (zeros10 && b[10] == 0xff && b[11] == 0xff) || // ::ffff:a.b.c.d
        b[0] == 0xff ||                                // multicast
        (b[0] == 0xfe && (b[1] & 0xc0) == 0x80))       // link-local
    {
        return {};
    }
    in6_addr address{};
    std::copy(b, b + 16, reinterpret_cast<std::uint8_t*>(&address));
    char text[INET6_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET6, &address, text, sizeof(text)) == nullptr)
    {
        return {};
    }
    return "[" + std::string(text) + "]";
}

std::vector<std::string> detail::default_route_interfaces(std::string_view ipv4_routes, std::string_view ipv6_routes)
{
    std::vector<std::string> names;
    const auto add = [&names](const std::string& name) {
        if (std::find(names.begin(), names.end(), name) == names.end())
        {
            names.push_back(name);
        }
    };
    constexpr unsigned long kUp = 0x1;
    constexpr unsigned long kReject = 0x200;
    // /proc/net/route: Iface Destination Gateway Flags RefCnt Use Metric Mask ... (hex, a header line)
    std::istringstream v4{std::string(ipv4_routes)};
    for (std::string line; std::getline(v4, line);)
    {
        std::istringstream words(line);
        std::string name, destination, gateway, flags, refs, use, metric, mask;
        if (words >> name >> destination >> gateway >> flags >> refs >> use >> metric >> mask && destination == "00000000" &&
            mask == "00000000")
        {
            const unsigned long f = std::strtoul(flags.c_str(), nullptr, 16);
            if ((f & kUp) != 0 && (f & kReject) == 0)
            {
                add(name);
            }
        }
    }
    // /proc/net/ipv6_route: destination prefix-length source prefix-length next-hop metric refs use
    // flags device (hex, no header)
    std::istringstream v6{std::string(ipv6_routes)};
    for (std::string line; std::getline(v6, line);)
    {
        std::istringstream words(line);
        std::string destination, length, source, sourceLength, hop, metric, refs, use, flags, name;
        if (words >> destination >> length >> source >> sourceLength >> hop >> metric >> refs >> use >> flags >> name &&
            destination == std::string(32, '0') && length == "00")
        {
            const unsigned long f = std::strtoul(flags.c_str(), nullptr, 16);
            if ((f & kUp) != 0 && (f & kReject) == 0)
            {
                add(name);
            }
        }
    }
    return names;
}

std::vector<detail::HostAddress> detail::host_addresses(bool ipv4, bool ipv6)
{
    std::vector<HostAddress> found;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt)
    {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_UNSPEC,
                                      GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
                                          GAA_FLAG_INCLUDE_GATEWAYS,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR)
    {
        return found;
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr; adapter = adapter->Next)
    {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
        {
            continue;
        }
        // An adapter with a gateway has a default route (WSL's and Hyper-V's virtual switches have none).
        const bool defaultRoute = adapter->FirstGatewayAddress != nullptr;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next)
        {
            add_once(found, reachable_text(unicast->Address.lpSockaddr, ipv4, ipv6), defaultRoute);
        }
    }
#else
    const std::vector<std::string> withDefault =
        default_route_interfaces(read_text("/proc/net/route"), read_text("/proc/net/ipv6_route"));
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0)
    {
        return found;
    }
    for (const ifaddrs* i = list; i != nullptr; i = i->ifa_next)
    {
        // Up and running (an interface without a carrier is up but not running), not loopback.
        const bool usable = (i->ifa_flags & IFF_UP) != 0 && (i->ifa_flags & IFF_RUNNING) != 0 &&
                            (i->ifa_flags & IFF_LOOPBACK) == 0;
        if (usable)
        {
            const bool defaultRoute =
                i->ifa_name != nullptr && std::find(withDefault.begin(), withDefault.end(), i->ifa_name) != withDefault.end();
            add_once(found, reachable_text(i->ifa_addr, ipv4, ipv6), defaultRoute);
        }
    }
    ::freeifaddrs(list);
#endif
    return in_order(std::move(found));
}

std::vector<detail::HostAddress> detail::in_order(std::vector<HostAddress> addresses)
{
    std::stable_sort(addresses.begin(), addresses.end(), [](const HostAddress& a, const HostAddress& b) {
        const bool a6 = a.host.front() == '[';
        const bool b6 = b.host.front() == '[';
        return a.default_route != b.default_route ? a.default_route : (a6 != b6 && !a6);
    });
    return addresses;
}

bool detail::ipv6_any_takes_ipv4()
{
#ifdef _WIN32
    return false;
#else
    return read_text("/proc/sys/net/ipv6/bindv6only").rfind('1', 0) != 0;
#endif
}

std::string detail::other_computers_line(const std::string& uri, const std::vector<HostAddress>& addresses, bool ipv6_only)
{
    const auto colon = uri.rfind(':');
    const std::string port = colon == std::string::npos ? std::string{} : uri.substr(colon + 1);
    const std::string host = colon == std::string::npos ? uri : uri.substr(0, colon);
    const std::string tail =
        ": give it as --registry-uri to a participant started by hand on another computer (instead of the loopback "
        "address in its command; it needs no VEHICLE_SIL_LISTEN_ON_NETWORK)";
    if (host != "silkit://0.0.0.0" && host != "silkit://[::]")
    {
        return "from other computers, join at " + uri + tail;
    }
    const std::string only6 = ipv6_only ? "; it listens on IPv6 only here: for IPv4, write registry_uri: silkit://0.0.0.0:" + port
                                        : std::string{};
    if (addresses.empty())
    {
        return "from other computers, join at silkit://<an address of this computer>:" + port +
               " (none found here: ipconfig on Windows, ip addr elsewhere, shows them" + only6 + ")" + tail;
    }
    std::string list;
    for (std::size_t i = 0; i < addresses.size(); ++i)
    {
        list += (i == 0 ? "" : i + 1 == addresses.size() ? " or " : ", ") + std::string("silkit://") + addresses[i].host + ":" + port;
    }
    std::string which = addresses.size() > 1 ? "this computer's addresses" : "this computer's address";
    // Sorted: the first has the default route and the last has not.
    if (addresses.size() > 1 && addresses.front().default_route && !addresses.back().default_route)
    {
        which += "; the first is on the network of its default route, usually the one the others reach";
    }
    return "from other computers, join at " + list + " (" + which + only6 + ")" + tail;
}

} // namespace vehicle_sil::launch
