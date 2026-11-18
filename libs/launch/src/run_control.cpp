// SPDX-License-Identifier: MIT
#include "run_control.hpp"

#include "seen_ecu.hpp"

#include "vehicle_sil/control/wire.hpp"

#include <algorithm>

namespace vehicle_sil::launch::detail {

const std::shared_ptr<control::Hub>& RunControl::shared_hub()
{
    if (!maps_set_)
    {
        // Before any agent connects: the hub shows every client (the scenario, the control server)
        // the variables through the participants' variable maps.
        maps_set_ = true;
        for (const ParticipantConfig& p : ctx_.config.participants)
        {
            if (p.enabled && !p.variables.empty())
            {
                hub_->set_variable_map(p.name, p.variables);
            }
        }
    }
    return hub_;
}

ControlLink RunControl::described_link(std::size_t i) const
{
    const auto& ecu = ctx_.check.participants[i].ecu;
    if (!ctx_.config.participants[i].describe || !ecu)
    {
        return ControlLink::None;
    }
    const SeenEcu seen(ctx_.config.participants[i], *ecu);
    if (!seen.has_variables())
    {
        return ControlLink::None;
    }
    const auto& offer = seen.control();
    if (!offer)
    {
        return ControlLink::NotOffered;
    }
    return offer->transport == "silkit" && offer->wire_version == static_cast<int>(control::wire::kVersion)
               ? ControlLink::Connected
               : ControlLink::VersionMismatch;
}

bool RunControl::offers_control(std::size_t i) const
{
    const auto& ecu = ctx_.check.participants[i].ecu;
    if (!ctx_.config.participants[i].describe || !ecu)
    {
        return false;
    }
    const SeenEcu seen(ctx_.config.participants[i], *ecu);
    if (!seen.control())
    {
        return false;
    }
    const auto& offer = *seen.control();
    return offer.transport == "silkit" && offer.wire_version == static_cast<int>(control::wire::kVersion);
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
        // Also one without variables: its agent has what the launcher reads besides (the counters
        // of its networks), and whether its link is in the result must not depend on when its
        // hello came.
        if (p.enabled && (p.required || used) && offers_control(i))
        {
            names.push_back(p.name);
        }
    }
    return names;
}

void RunControl::say_map_problems(const ParticipantRun& run, bool late) const
{
    std::string problems;
    for (const std::string& problem : run.variable_map_problems)
    {
        problems += (problems.empty() ? "" : "; ") + problem;
    }
    out_.say("participant '" + run.name + "': variables: " + std::to_string(run.variable_map_problems.size()) +
             " of its mappings do not fit the variables its control agent announced" +
             (late ? ", which connected after the simulation had started, and were not applied: "
                   : " and are not applied: ") +
             problems +
             (late ? ". In this run those variables were read under the executable's names and values, and a write to "
                     "a name of these mappings was refused"
                   : ". In this run those variables are read under the executable's names and values, and a write to "
                     "a name of these mappings is refused"));
}

void RunControl::note_links(RunOutcome& outcome, const std::optional<ControlStart>& start,
                            const std::vector<control_silkit::LinkInfo>& now,
                            const std::function<bool(std::size_t)>& in_run) const
{
    const std::optional<control_silkit::HelloReport> report =
        start ? std::optional<control_silkit::HelloReport>(start->report) : std::nullopt;
    const std::vector<control_silkit::LinkInfo> links = start ? start->links : std::vector<control_silkit::LinkInfo>{};
    const std::vector<std::string> awaited = start ? expected_agents() : std::vector<std::string>{};
    const auto find_link = [](const std::vector<control_silkit::LinkInfo>& in, const std::string& name) {
        const auto found = std::find_if(in.begin(), in.end(),
                                        [&name](const control_silkit::LinkInfo& l) { return l.participant == name; });
        return found != in.end() ? &*found : nullptr;
    };
    const std::vector<control::PeerInfo> peers = hub_->peers();
    for (std::size_t i = 0; i < outcome.participants.size(); ++i)
    {
        ParticipantRun& run = outcome.participants[i];
        if (!in_run(i))
        {
            continue;
        }
        const ControlLink described = described_link(i);
        // What the window saw comes first: an agent that has ended since was connected at the
        // start all the same. The window waits only for the agents it knows of (expected_agents());
        // the hello of another one (describe: false) can come after the window has looked, and
        // without the look at the links as they are now, whether its link and what its variable
        // map does not fit are in the result would depend on when its hello came.
        const control_silkit::LinkInfo* link = find_link(links, run.name);
        if ((link == nullptr || (link->state == control_silkit::LinkState::Connected && !link->ready)) &&
            std::find(awaited.begin(), awaited.end(), run.name) == awaited.end())
        {
            if (const control_silkit::LinkInfo* later = find_link(now, run.name))
            {
                link = later;
            }
        }
        if (link != nullptr && link->state == control_silkit::LinkState::VersionMismatch)
        {
            run.control = ControlLink::VersionMismatch;
            run.control_version = link->version;
            run.control_version_described = described == ControlLink::VersionMismatch;
        }
        else if (link != nullptr && link->state == control_silkit::LinkState::Connected && link->ready)
        {
            run.control = ControlLink::Connected;
            run.control_variables = link->variables;
            const auto peer = std::find_if(peers.begin(), peers.end(),
                                           [&run](const control::PeerInfo& p) { return p.participant == run.name; });
            if (peer != peers.end())
            {
                run.variable_map_problems = peer->map_problems;
            }
            if (!run.variable_map_problems.empty())
            {
                // Said now, not only with the result: clients are reading and writing meanwhile.
                say_map_problems(run, false);
            }
        }
        else if (described == ControlLink::VersionMismatch)
        {
            run.control = ControlLink::VersionMismatch;
            run.control_version = SeenEcu(ctx_.config.participants[i], *ctx_.check.participants[i].ecu).control()->wire_version;
            run.control_version_described = true;
        }
        else if (described == ControlLink::NotOffered)
        {
            run.control = ControlLink::NotOffered;
        }
        // Awaited in vain: no hello, or a hello and no answer to the probe. Not one whose agent
        // said hello and ended in the window (it left), and not when the wait was cut short.
        else if (report && !report->cancelled &&
                 (link == nullptr || (link->state == control_silkit::LinkState::Connected && !link->ready)) &&
                 std::find(report->missing.begin(), report->missing.end(), run.name) != report->missing.end())
        {
            run.control = ControlLink::NoHello;
            run.control_hello_seen = link != nullptr;
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
    outcome.control.refused_hellos = stats.refused_hellos;
    outcome.control.undecodable = stats.undecodable;
    outcome.control.unknown = stats.unknown;
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

void RunControl::note_late_maps(RunOutcome& outcome) const
{
    const std::vector<control::PeerInfo> peers = hub_->peers();
    for (ParticipantRun& run : outcome.participants)
    {
        const auto peer = std::find_if(peers.begin(), peers.end(),
                                       [&run](const control::PeerInfo& p) { return p.participant == run.name; });
        if (!run.variable_map_problems.empty() || peer == peers.end() || peer->map_problems.empty())
        {
            continue;
        }
        run.variable_map_problems = peer->map_problems;
        say_map_problems(run, true);
    }
}

} // namespace vehicle_sil::launch::detail
