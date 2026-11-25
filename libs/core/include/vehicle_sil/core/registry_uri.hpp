// SPDX-License-Identifier: MIT
//
// What vehicle-sil reads from a SIL Kit registry URI, and the variable that opens the listen address
// of a participant. Header only and without SIL Kit (vehicle_sil::sim_base), so that the code that
// checks a configuration can use it without linking vehicle_sil::core.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace vehicle_sil {

/// The environment variable that makes the participants of a process listen on every address
/// even when their registry URI is a loopback address: "1" does, "0", empty or not set does not;
/// any other value is an error (std::invalid_argument when a participant is made). For a registry
/// that other computers reach although this computer's participants connect to it at a loopback
/// address: vehicle_sil_launch run with registry_uri silkit://0.0.0.0:<port> sets it to 1 for the
/// participants it starts (and to 0 when the registry is on a loopback address). Without it, the
/// participants of the other computers are given this computer's loopback acceptors and try them on
/// their own loopback address, where another program may answer: the join then fails.
inline constexpr std::string_view kListenOnNetworkVariable = "VEHICLE_SIL_LISTEN_ON_NETWORK";

/// Internal launcher-to-participant environment variable (POSIX only): absolute path of a
/// per-run directory for local-domain acceptor sockets (`vsil-<16hex>.silkit`). Ignored if
/// unset, empty, not an existing writable absolute directory, or too long for sockaddr_un::sun_path.
inline constexpr std::string_view kSocketDirectoryVariable = "VEHICLE_SIL_SOCKET_DIR";

namespace registry_uri_detail {

/// The host of "silkit://<host>:<port>...", without the brackets of an IPv6 address; empty if
/// `uri` is not of that form.
inline std::string_view registry_host(std::string_view uri) noexcept
{
    constexpr std::string_view scheme = "silkit://";
    if (uri.substr(0, scheme.size()) != scheme)
    {
        return {};
    }
    uri.remove_prefix(scheme.size());
    if (!uri.empty() && uri.front() == '[')
    {
        const auto end = uri.find(']');
        return end == std::string_view::npos ? std::string_view{} : uri.substr(1, end - 1);
    }
    return uri.substr(0, uri.find_first_of(":/"));
}

inline bool equals_ignoring_case(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

/// An IPv4 address in one of the numeric forms that inet_aton() and getaddrinfo() read: one to four
/// parts, each decimal, octal (leading 0) or hexadecimal (0x), the last one filling the rest
/// ("127.1" is 127.0.0.1, "2130706433" too). Empty if `host` is not one.
inline std::optional<std::uint32_t> parse_ipv4(std::string_view host) noexcept
{
    std::uint64_t parts[4] = {};
    std::size_t count = 0;
    while (true)
    {
        if (count == 4)
        {
            return std::nullopt;
        }
        const auto dot = host.find('.');
        std::string_view part = host.substr(0, dot);
        int base = 10;
        if (part.size() > 1 && part[0] == '0' && (part[1] == 'x' || part[1] == 'X'))
        {
            base = 16;
            part.remove_prefix(2);
        }
        else if (part.size() > 1 && part[0] == '0')
        {
            base = 8;
            part.remove_prefix(1);
        }
        std::uint64_t value = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value, base);
        if (part.empty() || error != std::errc{} || end != part.data() + part.size() || value > 0xFFFFFFFFULL)
        {
            return std::nullopt;
        }
        parts[count++] = value;
        if (dot == std::string_view::npos)
        {
            break;
        }
        host.remove_prefix(dot + 1);
    }
    std::uint64_t address = 0;
    for (std::size_t i = 0; i + 1 < count; ++i)
    {
        if (parts[i] > 255)
        {
            return std::nullopt;
        }
        address |= parts[i] << (8 * (3 - i));
    }
    const std::uint64_t last = parts[count - 1];
    if (last > (0xFFFFFFFFULL >> (8 * (count - 1))))
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(address | last);
}

/// The groups of an IPv6 address in text form ("::1", "0:0:0:0:0:0:0:1", "::ffff:127.0.0.1"; a zone
/// "%..." is left out). Empty if `host` is not one.
inline std::optional<std::array<std::uint16_t, 8>> parse_ipv6(std::string_view host)
{
    host = host.substr(0, host.find('%'));
    // The groups of one side of "::"; a dotted IPv4 address may end the address.
    const auto groups_of = [](std::string_view text, bool last, std::vector<std::uint16_t>& out) {
        if (text.empty())
        {
            return true;
        }
        while (true)
        {
            const auto colon = text.find(':');
            const std::string_view part = text.substr(0, colon);
            if (colon == std::string_view::npos && last && part.find('.') != std::string_view::npos)
            {
                if (std::count(part.begin(), part.end(), '.') != 3)
                {
                    return false;
                }
                const auto v4 = parse_ipv4(part);
                if (!v4)
                {
                    return false;
                }
                out.push_back(static_cast<std::uint16_t>(*v4 >> 16));
                out.push_back(static_cast<std::uint16_t>(*v4 & 0xFFFF));
                return true;
            }
            unsigned value = 0;
            const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value, 16);
            if (part.empty() || part.size() > 4 || error != std::errc{} || end != part.data() + part.size())
            {
                return false;
            }
            out.push_back(static_cast<std::uint16_t>(value));
            if (colon == std::string_view::npos)
            {
                return true;
            }
            text.remove_prefix(colon + 1);
        }
    };
    std::vector<std::uint16_t> head;
    std::vector<std::uint16_t> tail;
    const auto gap = host.find("::");
    if (gap == std::string_view::npos)
    {
        if (!groups_of(host, true, head) || head.size() != 8)
        {
            return std::nullopt;
        }
    }
    else if (host.find("::", gap + 1) != std::string_view::npos || !groups_of(host.substr(0, gap), false, head) ||
             !groups_of(host.substr(gap + 2), true, tail) || head.size() + tail.size() > 7)
    {
        return std::nullopt;
    }
    std::array<std::uint16_t, 8> groups{};
    std::copy(head.begin(), head.end(), groups.begin());
    std::copy(tail.begin(), tail.end(), groups.end() - static_cast<std::ptrdiff_t>(tail.size()));
    return groups;
}

/// ::1.
inline bool is_ipv6_loopback(const std::array<std::uint16_t, 8>& groups) noexcept
{
    return std::all_of(groups.begin(), groups.end() - 1, [](std::uint16_t g) { return g == 0; }) && groups[7] == 1;
}

} // namespace registry_uri_detail

/// Whether `registry_uri` (silkit://<host>:<port>...) names its host by a loopback address: an IPv4
/// address in 127.0.0.0/8 in any numeric form that getaddrinfo() reads ("127.0.0.1", "127.1"),
/// ::1 in any IPv6 form ([::1], [0:0:0:0:0:0:0:1]), 127.x.x.x mapped to IPv6
/// ([::ffff:127.0.0.1]), or localhost (any case, also "localhost."). Other names are not resolved:
/// they count as not loopback.
[[nodiscard]] inline bool is_loopback_registry_uri(std::string_view registry_uri)
{
    std::string_view host = registry_uri_detail::registry_host(registry_uri);
    if (host.find(':') != std::string_view::npos)
    {
        // ::1, or 127.x.x.x mapped to IPv6 (::ffff:127.0.0.1).
        const auto v6 = registry_uri_detail::parse_ipv6(host);
        return v6 && (registry_uri_detail::is_ipv6_loopback(*v6) || (std::all_of(v6->begin(), v6->begin() + 5, [](std::uint16_t g) { return g == 0; }) &&
                                                (*v6)[5] == 0xFFFF && ((*v6)[6] >> 8) == 127));
    }
    if (const auto v4 = registry_uri_detail::parse_ipv4(host))
    {
        return (*v4 >> 24) == 127;
    }
    if (!host.empty() && host.back() == '.')
    {
        host.remove_suffix(1); // the root of the names: "localhost." is "localhost"
    }
    return registry_uri_detail::equals_ignoring_case(host, "localhost");
}

} // namespace vehicle_sil
