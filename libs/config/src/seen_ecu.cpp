// SPDX-License-Identifier: MIT
#include "vehicle_sil/config/detail/seen_ecu.hpp"

#include <algorithm>

namespace vehicle_sil::config {

control::MappedSchema seen_variables(const ParticipantConfig& participant, const ReportedEcu& ecu)
{
    return control::MappedSchema::make(ecu.description.variables, participant.variables);
}

const std::string& SeenEcu::name() const noexcept
{
    return ecu_.description.name;
}

const std::optional<SimDuration>& SeenEcu::step() const noexcept
{
    return ecu_.description.step;
}

std::vector<SeenNetwork> SeenEcu::seen_networks() const
{
    return config::seen_networks(participant_, ecu_);
}

control::MappedSchema SeenEcu::variables() const
{
    return seen_variables(participant_, ecu_);
}

const std::vector<control::VariableInfo>& SeenEcu::own_variables() const noexcept
{
    return ecu_.description.variables;
}

bool SeenEcu::has_variables() const noexcept
{
    return !ecu_.description.variables.empty();
}

const std::optional<ControlOffer>& SeenEcu::control() const noexcept
{
    return ecu_.description.control;
}

bool SeenEcu::takes(std::string_view feature) const
{
    const auto& features = ecu_.description.features;
    return std::find(features.begin(), features.end(), feature) != features.end();
}

const EcuDescription& SeenEcu::as_described() const noexcept
{
    return ecu_.description;
}

} // namespace vehicle_sil::config
