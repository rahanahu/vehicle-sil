// SPDX-License-Identifier: MIT
//
// The configuration's side of a check's result: the counts of a CheckReport and the order of its
// findings, and which of a request's --connect counts for a configuration (connect_for_config()).
// Pure. check_file() (check.cpp) makes the report.
#include "vehicle_sil/launch/check.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

namespace vehicle_sil::launch {

ConfigConnect connect_for_config(const SimulationConfig& config, std::span<const ConnectRequest> connect,
                                 const RequestOrigin& origin)
{
    ConfigConnect out;
    for (const ConnectRequest& request : connect)
    {
        const bool fromFile =
            std::find(origin.connected.begin(), origin.connected.end(), request.network) != origin.connected.end();
        const bool known = std::any_of(config.networks.begin(), config.networks.end(),
                                       [&](const NetworkConfig& n) { return n.name == request.network; });
        if (fromFile && !known)
        {
            out.notes.push_back(Finding{Severity::Note, "connect-network-unused",
                                        "the network " + request.network + " of connect in " + origin.file +
                                            " is not in this configuration: it is left out",
                                        0, "", request.network});
            continue;
        }
        out.connect.push_back(request);
    }
    return out;
}

std::size_t CheckReport::errors() const
{
    return static_cast<std::size_t>(
        std::count_if(findings.begin(), findings.end(), [](const Finding& f) { return f.severity == Severity::Error; }));
}

std::size_t CheckReport::warnings() const
{
    return static_cast<std::size_t>(
        std::count_if(findings.begin(), findings.end(), [](const Finding& f) { return f.severity == Severity::Warning; }));
}

std::size_t CheckReport::notes() const { return findings.size() - errors() - warnings(); }

bool CheckReport::passed() const
{
    return !usage && config.has_value() && errors() == 0 && (!strict || warnings() == 0);
}

void sort_findings(std::vector<Finding>& findings)
{
    std::stable_sort(findings.begin(), findings.end(), [](const Finding& a, const Finding& b) {
        if (a.severity != b.severity)
        {
            return a.severity < b.severity; // errors, warnings, notes
        }
        if (a.file != b.file)
        {
            return a.file.empty(); // the configuration's, then the scenario's
        }
        return a.line < b.line;
    });
}

} // namespace vehicle_sil::launch
