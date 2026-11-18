// SPDX-License-Identifier: MIT
//
// A participant's ECU as the simulation sees it: what its executable described (--describe), with
// the participant's entry of the configuration applied. The check (its rules and its outputs),
// the run and the scenario read an ECU's description through SeenEcu only; describe_read.cpp
// makes the description from the JSON, seen_networks() and write_reported_networks()
// (check_networks.cpp) read its networks. Internal.
#pragma once

#include "vehicle_sil/config/check.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch {

/// One participant's ECU as the simulation sees it.
///
/// A view: `participant` and `ecu` must outlive it. What it gives is worked out on every call,
/// as the functions it calls do (keep the result where it is needed more than once).
class SeenEcu
{
public:
    SeenEcu(const ParticipantConfig& participant, const ReportedEcu& ecu) noexcept : participant_(participant), ecu_(ecu) {}

    /// The name the executable reports (the --name it was given, if its main() passes it on).
    [[nodiscard]] const std::string& name() const noexcept;
    /// Its step; empty: it runs in real time.
    [[nodiscard]] const std::optional<SimDuration>& step() const noexcept;
    /// Its networks, each under its name in the simulation, with its J1939 nodes as the
    /// simulation has them (launch::seen_networks()). Not named networks(): no member of that
    /// name is read outside check_networks.cpp (launch.described_networks_are_read_in_one_place).
    [[nodiscard]] std::vector<SeenNetwork> seen_networks() const;
    /// Its variables as the simulation sees them, through the participant's variable map
    /// (seen_variables()).
    [[nodiscard]] control::MappedSchema variables() const;
    /// Its variables under the names the executable gives them (before the map): for a message
    /// that says what it has.
    [[nodiscard]] const std::vector<control::VariableInfo>& own_variables() const noexcept;
    /// It describes variables.
    [[nodiscard]] bool has_variables() const noexcept;
    /// How it offers its variables to the simulation; empty: it does not offer them.
    [[nodiscard]] const std::optional<ControlOffer>& control() const noexcept;
    /// It says that it takes `feature` (one of EcuDescription::features).
    [[nodiscard]] bool takes(std::string_view feature) const;
    /// What it described, as it said it: for an output that writes the description back.
    [[nodiscard]] const EcuDescription& as_described() const noexcept;

private:
    const ParticipantConfig& participant_;
    const ReportedEcu& ecu_;
};

} // namespace vehicle_sil::launch
