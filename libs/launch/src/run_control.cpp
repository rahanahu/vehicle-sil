// SPDX-License-Identifier: MIT
#include "run_control.hpp"

#include "vehicle_sil/control/wire.hpp"

#include <algorithm>

namespace vehicle_sil::launch::detail {

control::Hub& RunControl::hub()
{
    if (ctx_.options.hub != nullptr)
    {
        return *ctx_.options.hub;
    }
    if (!own_hub_)
    {
        own_hub_ = std::make_unique<control::Hub>();
    }
    return *own_hub_;
}

ControlLink RunControl::described_link(std::size_t i) const
{
    const auto& ecu = ctx_.check.participants[i].ecu;
    if (!ctx_.config.participants[i].describe || !ecu || ecu->description.variables.empty())
    {
        return ControlLink::None;
    }
    const auto& offer = ecu->description.control;
    if (!offer)
    {
        return ControlLink::NotOffered;
    }
    return offer->transport == "silkit" && offer->wire_version == static_cast<int>(control::wire::kVersion)
               ? ControlLink::Connected
               : ControlLink::VersionMismatch;
}

std::vector<std::string> RunControl::expected_agents() const
{
    std::vector<std::string> names;
    for (std::size_t i = 0; i < ctx_.config.participants.size(); ++i)
    {
        const ParticipantConfig& p = ctx_.config.participants[i];
        // Also an optional one whose variables the scenario uses.
        const bool used = ctx_.scenario != nullptr &&
                          std::any_of(ctx_.scenario->participants.begin(), ctx_.scenario->participants.end(),
                                      [&p](const ScenarioParticipant& s) { return s.name == p.name; });
        if (p.enabled && (p.required || used) && described_link(i) == ControlLink::Connected)
        {
            names.push_back(p.name);
        }
    }
    return names;
}

void RunControl::note_links(RunOutcome& outcome, const std::optional<ControlStart>& start,
                            const std::function<bool(std::size_t)>& in_run) const
{
    const std::optional<control_silkit::HelloReport> report =
        start ? std::optional<control_silkit::HelloReport>(start->report) : std::nullopt;
    const std::vector<control_silkit::LinkInfo> links = start ? start->links : std::vector<control_silkit::LinkInfo>{};
    for (std::size_t i = 0; i < outcome.participants.size(); ++i)
    {
        ParticipantRun& run = outcome.participants[i];
        if (!in_run(i))
        {
            continue;
        }
        const ControlLink described = described_link(i);
        const auto link = std::find_if(links.begin(), links.end(),
                                       [&run](const control_silkit::LinkInfo& l) { return l.participant == run.name; });
        if (link != links.end() && link->state == control_silkit::LinkState::VersionMismatch)
        {
            run.control = ControlLink::VersionMismatch;
            run.control_version = link->version;
            run.control_version_described = described == ControlLink::VersionMismatch;
        }
        else if (link != links.end() && link->state == control_silkit::LinkState::Connected && link->ready)
        {
            run.control = ControlLink::Connected;
            run.control_variables = link->variables;
        }
        else if (described == ControlLink::VersionMismatch)
        {
            run.control = ControlLink::VersionMismatch;
            run.control_version = ctx_.check.participants[i].ecu->description.control->wire_version;
            run.control_version_described = true;
        }
        else if (described == ControlLink::NotOffered)
        {
            run.control = ControlLink::NotOffered;
        }
        // Awaited in vain: no hello, or a hello and no answer to the probe. Not one whose agent
        // said hello and ended in the window (it left), and not when the wait was cut short.
        else if (report && !report->cancelled &&
                 (link == links.end() || (link->state == control_silkit::LinkState::Connected && !link->ready)) &&
                 std::find(report->missing.begin(), report->missing.end(), run.name) != report->missing.end())
        {
            run.control = ControlLink::NoHello;
            run.control_hello_seen = link != links.end();
        }
        out_.event([&run](EventStream& e) {
            e.control_link(run.name, control_link_name(run.control),
                           run.control == ControlLink::Connected ? std::optional<std::size_t>(run.control_variables)
                                                                 : std::nullopt,
                           run.control == ControlLink::VersionMismatch ? std::optional<int>(run.control_version)
                                                                       : std::nullopt);
        });
    }
}

void RunControl::note_late_values(RunOutcome& outcome, control_silkit::HubTransport& transport) const
{
    const control_silkit::HubTransportStats stats = transport.stats();
    outcome.control_refused_hellos = stats.refused_hellos;
    outcome.control_undecodable = stats.undecodable;
    outcome.control_unknown = stats.unknown;
    for (const control_silkit::LinkInfo& link : transport.links())
    {
        for (ParticipantRun& run : outcome.participants)
        {
            if (run.name == link.participant)
            {
                run.control_late_messages = link.late_messages;
            }
        }
    }
}

} // namespace vehicle_sil::launch::detail
