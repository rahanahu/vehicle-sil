// SPDX-License-Identifier: MIT
//
// scenario_catalog(): the participants of a checked configuration as a scenario can use them.
#include "vehicle_sil/scenario/scenario_check.hpp"

#include "vehicle_sil/config/detail/seen_ecu.hpp"
#include "vehicle_sil/ecu/executable.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace vehicle_sil::scenario {

ScenarioCatalog scenario_catalog(const config::CheckReport& check)
{
    ScenarioCatalog out;
    if (!check.config)
    {
        return out;
    }
    for (std::size_t i = 0; i < check.config->participants.size(); ++i)
    {
        const config::ParticipantConfig& p = check.config->participants[i];
        CatalogParticipant c;
        c.name = p.name;
        const config::ParticipantResult* result = i < check.participants.size() ? &check.participants[i] : nullptr;
        if (!p.enabled && p.left_out_by_variant && !check.config->variant.empty())
        {
            c.not_in_variant = true;
        }
        else if (!p.enabled)
        {
            c.unusable = "is left out of this run (enabled: false, or --disable): take it in (--enable " + p.name +
                         "), or run a scenario that does not use it";
        }
        else if (!p.describe)
        {
            c.unusable = "has describe: false, so the launcher does not know its variables: a scenario can use the variables "
                         "of ecu_main executables only";
        }
        else if (result == nullptr || result->status != config::ParticipantStatus::Described || !result->ecu)
        {
            c.unusable = "could not be described (the errors of the configuration say why), so its variables are not known";
        }
        else
        {
            const config::SeenEcu d(p, *result->ecu);
            // As the run's hub shows them.
            auto seen = std::make_shared<const control::MappedSchema>(d.variables());
            c.variables = seen->schema();
            for (const control::VariableMapping& mapping : p.variables)
            {
                if (mapping.absent)
                {
                    c.absent.push_back(mapping.name);
                }
            }
            if (!p.variables.empty())
            {
                c.map = std::move(seen);
            }
            if (!d.step())
            {
                c.unusable = "runs in real time (it has no step): a scenario needs the virtual time of its steps";
            }
            else if (d.has_variables() && !d.control())
            {
                c.unusable = "does not offer its variables to the simulation (--no-control, or an executable of an earlier "
                             "vehicle-sil): build it with this vehicle-sil and run it without --no-control";
            }
            else
            {
                c.step = *d.step();
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

namespace {

/// What the samples of a scenario's participants depend on, per participant of the configuration.
struct SampleSource
{
    bool read = false;      ///< the scenario reads a value of it
    bool described = false; ///< its executable described itself: `takes` and `step` are known
    bool takes = false;     ///< its --describe says that it takes --sample-every-step
    std::string built_with; ///< the vehicle-sil its executable was built with, or ""
};

std::vector<SampleSource> sample_sources(const CheckReport& check, const Scenario& scenario)
{
    std::vector<SampleSource> out(check.config ? check.config->participants.size() : 0);
    const std::vector<bool> read = participants_read(scenario);
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        const ParticipantConfig& p = check.config->participants[i];
        const auto used = std::find_if(scenario.participants.begin(), scenario.participants.end(),
                                       [&p](const ScenarioParticipant& s) { return s.name == p.name; });
        out[i].read = used != scenario.participants.end() && read[static_cast<std::size_t>(used - scenario.participants.begin())];
        // By what there is, not by the participant's status: whatever the reason a description is
        // missing, nothing then says what the executable takes.
        if (i < check.participants.size() && check.participants[i].ecu)
        {
            const ReportedEcu& ecu = *check.participants[i].ecu;
            out[i].described = true;
            out[i].takes = SeenEcu(p, ecu).takes(ecu_executable::kFeatureSampleEveryStep);
            out[i].built_with = ecu.vehicle_sil_version;
        }
    }
    return out;
}

} // namespace

std::vector<bool> sample_every_step_participants(const CheckReport& check, const Scenario& scenario)
{
    const std::vector<SampleSource> sources = sample_sources(check, scenario);
    std::vector<bool> out(sources.size(), false);
    for (std::size_t i = 0; i < sources.size(); ++i)
    {
        out[i] = scenario.samples == ScenarioSamples::EveryStep && sources[i].read && sources[i].takes;
    }
    return out;
}

std::vector<Finding> scenario_samples_findings(const CheckReport& check, const Scenario& scenario, std::string_view launcher_version)
{
    std::vector<Finding> out;
    const std::vector<SampleSource> sources = sample_sources(check, scenario);
    if (scenario.samples == ScenarioSamples::AsSent)
    {
        if (std::any_of(sources.begin(), sources.end(), [](const SampleSource& s) { return s.read; }))
        {
            out.push_back(Finding{Severity::Note, "scenario-samples-as-sent",
                                  "samples: as_sent: the scenario judges with the values as often as each ECU sends them "
                                  "(its sample_period), not after every step of the ECU. A condition does not see what a "
                                  "variable does between two samples: an expect reads a value up to one sample_period old, "
                                  "and hold_for_ms passes over a change that is shorter than that. Remove samples (or write "
                                  "samples: every_step) to judge with every step",
                                  0, "", ""});
        }
        return out;
    }
    for (std::size_t i = 0; i < sources.size(); ++i)
    {
        const SampleSource& s = sources[i];
        if (!s.read || !s.described || s.takes)
        {
            continue;
        }
        const std::string& name = check.config->participants[i].name;
        out.push_back(Finding{
            Severity::Warning, "scenario-samples-unsupported",
            "participant '" + name + "': the scenario reads its values, and its executable does not say that it takes " +
                std::string(ecu_executable::kSampleEveryStepArgument) + " (no \"" +
                std::string(ecu_executable::kFeatureSampleEveryStep) + "\" in the features of its --describe" +
                (s.built_with.empty() ? std::string{} : "; it was built with vehicle-sil " + s.built_with) +
                "), so the run cannot ask it to send its values after every step of its own. If it sends them less often "
                "than that, the run fails before the first step (scenario-not-observed). Build the executable again with "
                "vehicle-sil " + std::string(launcher_version) +
                " or later, or write samples: as_sent at the top of the scenario to judge with the values as the ECU sends "
                "them",
            0, name, ""});
    }
    return out;
}

} // namespace vehicle_sil::scenario
