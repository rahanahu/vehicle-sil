// SPDX-License-Identifier: MIT
#include "vehicle_sil/bridge_runtime/registry_failure.hpp"

namespace vehicle_sil::bridge_runtime {

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
    return "cannot connect to the SIL Kit registry at " + uri + " (" + detail +
           "). Start the registry first ('sil-kit-registry --listen-uri " + uri +
           "'), or give the address it listens at with --registry-uri.";
}

} // namespace vehicle_sil::bridge_runtime
