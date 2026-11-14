// SPDX-License-Identifier: MIT
#include "vehicle_sil/bridge_runtime/registry_failure.hpp"

namespace vehicle_sil::bridge_runtime {
namespace {

struct Address
{
    std::string host; ///< without the brackets of an IPv6 address
    std::string port; ///< empty if the address names none
};

/// The host and the port of silkit://<host>:<port>; a host that is empty if the address has another form.
Address address_of(std::string_view uri)
{
    if (const auto scheme = uri.find("://"); scheme != std::string_view::npos)
    {
        uri.remove_prefix(scheme + 3);
    }
    uri = uri.substr(0, uri.find('/'));
    Address address;
    if (!uri.empty() && uri.front() == '[')
    {
        const auto end = uri.find(']');
        if (end == std::string_view::npos)
        {
            return {};
        }
        address.host = std::string(uri.substr(1, end - 1));
        uri.remove_prefix(end + 1);
        if (!uri.empty() && uri.front() == ':')
        {
            address.port = std::string(uri.substr(1));
        }
        return address;
    }
    const auto colon = uri.rfind(':');
    if (colon != std::string_view::npos && uri.find(':') != colon)
    {
        // More than one colon and no brackets: an IPv6 address alone.
        address.host = std::string(uri);
        return address;
    }
    address.host = std::string(uri.substr(0, colon));
    if (colon != std::string_view::npos)
    {
        address.port = std::string(uri.substr(colon + 1));
    }
    return address;
}

/// `text` with its ASCII letters in lower case (host names and the hex digits of an IPv6 address
/// are compared without their case).
std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

/// Whether `text` is four numbers with dots between them.
bool is_ipv4(std::string_view text)
{
    int dots = 0;
    bool digit_before = false;
    for (const char c : text)
    {
        if (c == '.')
        {
            if (!digit_before)
            {
                return false;
            }
            ++dots;
            digit_before = false;
        }
        else if (c >= '0' && c <= '9')
        {
            digit_before = true;
        }
        else
        {
            return false;
        }
    }
    return dots == 3 && digit_before;
}

/// Whether `host` (in lower case) is the loopback of this computer by how it is written. An
/// empty host counts: the advice for this computer is the one that was always given.
bool is_loopback(std::string_view host)
{
    constexpr std::string_view mapped = "::ffff:";
    if (host.rfind(mapped, 0) == 0)
    {
        host.remove_prefix(mapped.size());
    }
    return host.empty() || host == "localhost" || host == "::1" || host == "0:0:0:0:0:0:0:1" ||
           (host.rfind("127.", 0) == 0 && is_ipv4(host));
}

/// Whether `host` is the address that stands for every address of a computer: one to listen on.
bool is_any_address(std::string_view host)
{
    return host == "0.0.0.0" || host == "::" || host == "0:0:0:0:0:0:0:0";
}

} // namespace

std::optional<std::string> registry_failure_message(std::string_view what, std::string_view registry_uri)
{
    // The text SIL Kit throws when no registry answers at the address.
    if (what.find("connect to SIL Kit Registry") == std::string_view::npos)
    {
        return std::nullopt;
    }
    // SIL Kit's text has line breaks; the bridge's message is one line.
    std::string detail(what);
    for (char& c : detail)
    {
        if (c == '\n' || c == '\r')
        {
            c = ' ';
        }
    }
    const std::string uri(registry_uri);
    const std::string start = "cannot connect to the SIL Kit registry at " + uri + " (" + detail + "). ";
    Address address = address_of(registry_uri);
    address.host = lower(address.host);
    // The port a registry listens on when its address names none.
    const std::string port = address.port.empty() ? "8500" : address.port;
    if (is_any_address(address.host))
    {
        return start + address.host +
               " stands for every address of a computer: a registry listens on it, a participant cannot connect to it. "
               "Give --registry-uri the address of the computer the registry runs on (silkit://127.0.0.1:" + port +
               " if it runs on this one).";
    }
    if (!is_loopback(address.host))
    {
        // Starting a registry with this address may not be what is missing, and whose address it is
        // is not known here (another computer's, or one of this computer's own): say what to look at
        // where the registry runs.
        return start + address.host +
               " is not the loopback address of this computer: check that the registry runs on the computer with that "
               "address (which may be this one), that it listens on an address reached from here ('sil-kit-registry "
               "--listen-uri silkit://0.0.0.0:" + port + "' there listens on all of its addresses, not only on its "
               "loopback), and that its firewall lets port " + port + " through.";
    }
    return start + "Start the registry first ('sil-kit-registry --listen-uri " + uri +
           "'), or give the address it listens at with --registry-uri.";
}

} // namespace vehicle_sil::bridge_runtime
