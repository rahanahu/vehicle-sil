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

/// Whether `host` is this computer by its name alone (the loopback). An empty host counts: the
/// advice for this computer is the one that was always given.
bool is_this_computer(std::string_view host)
{
    return host.empty() || host == "localhost" || host == "::1" || host.rfind("127.", 0) == 0;
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
    if (const Address address = address_of(registry_uri); !is_this_computer(address.host))
    {
        // A registry started with this address on this computer would not help: say what to look at on the other one.
        const std::string listen = "silkit://0.0.0.0" + (address.port.empty() ? std::string() : ":" + address.port);
        return "cannot connect to the SIL Kit registry at " + uri + " (" + detail + "). " + address.host +
               " is another computer: check that the registry runs there, that it listens on an address this computer reaches "
               "('sil-kit-registry --listen-uri " + listen + "' listens on all of that computer's addresses, not only on "
               "its loopback), and that the firewall there lets the port through. If the registry is elsewhere, give its "
               "address with --registry-uri.";
    }
    return "cannot connect to the SIL Kit registry at " + uri + " (" + detail +
           "). Start the registry first ('sil-kit-registry --listen-uri " + uri +
           "'), or give the address it listens at with --registry-uri.";
}

} // namespace vehicle_sil::bridge_runtime
