// SPDX-License-Identifier: MIT
//
// The scenario that check --scenario read and checked against the configuration (ScenarioCheck),
// and the catalog that read_scenario() takes from a checked configuration. The configuration's
// check (check.hpp) does not know either: this header depends on it, not the other way round.
// Internal.
#pragma once

#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/scenario/scenario.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::scenario {

/// The scenario that was checked with the configuration (--scenario).
struct ScenarioCheck
{
    std::string file; ///< as the user named it
    /// Its "name" and the number of its steps, as far as it could be read.
    std::string name{};
    std::size_t steps = 0;
    /// Set if the configuration could be read and the scenario has no error: it can run.
    std::optional<Scenario> scenario{};
    /// ScenarioRead::params, only, skipped and unchecked_expects.
    std::vector<config::ParamValue> params{};
    std::optional<std::vector<std::string>> only{};
    std::size_t skipped = 0;
    std::size_t unchecked_expects = 0;
    /// ScenarioRead::not_for_variant: its "only" does not list the chosen variant (one of the
    /// errors says so). vehicle_sil_launch test skips such a scenario instead of failing.
    bool not_for_variant = false;
};

/// The catalog of a checked configuration, for read_scenario() (CheckReport::config must be set):
/// each participant as a scenario can use it, from its entry and what its executable described.
[[nodiscard]] ScenarioCatalog scenario_catalog(const config::CheckReport& check);

} // namespace vehicle_sil::scenario
