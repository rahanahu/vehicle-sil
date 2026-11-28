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
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::scenario {

/// The scenario that was checked with the configuration (--scenario).
struct ScenarioCheck
{
    std::string file; ///< as the user named it
    /// Its "name" and the number of its steps, as far as it could be read.
    std::string name{};
    std::string case_name{};
    std::vector<std::string> cases{};
    std::vector<ScenarioCaseDetail> case_details{};
    std::size_t steps = 0;
    std::optional<std::int64_t> end_ms{};
    /// Set if the configuration could be read and the scenario has no error: it can run.
    std::optional<Scenario> scenario{};
    /// ScenarioRead::params, param_meta, missing_params, only, skipped and unchecked_expects.
    std::vector<config::ParamValue> params{};
    std::vector<ScenarioParamMeta> param_meta{};
    std::vector<std::string> missing_params{};
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

/// Result of validating and resolving extra recorded variables (RunIntent::record) against a
/// ScenarioCatalog.
struct RecordedVariablesCheck
{
    std::vector<RecordedVariable> variables{};
    std::vector<config::Finding> findings{};
};

/// Validates and resolves `requested` ("<participant>.<variable>" entries from RunIntent::record)
/// against `catalog`. Deduplicates entries preserving first-seen order.
[[nodiscard]] RecordedVariablesCheck check_recorded_variables(std::span<const std::string> requested,
                                                              const ScenarioCatalog& catalog);

/// Per participant of the checked configuration: whether the run gives its executable
/// --sample-every-step (ecu_executable::kSampleEveryStepArgument). True where `scenario` asks for
/// a sample in every step (its samples is every_step), reads a value of the participant
/// (participants_read()) or `recorded` names one of its variables, and the participant's --describe
/// says that it takes the argument. An executable that does not say so is started without it (it
/// would not know the argument); for scenario-read participants the check warns of it
/// (scenario_samples_findings()) and the run refuses it if it sends its values less often than its
/// step (extra-recorded participants are best-effort and never trigger those checks).
[[nodiscard]] std::vector<bool> sample_every_step_participants(const config::CheckReport& check, const Scenario& scenario,
                                                               std::span<const RecordedVariable> recorded = {});

/// What the check says about the samples `scenario` judges with (Finding::file is not set):
/// - "scenario-samples-unsupported" (warning) for each participant whose values the scenario reads
///   and whose --describe does not say that it takes --sample-every-step. Only for a participant
///   that was described: without a description nothing says what the executable takes, and the
///   run finds out at its start ("scenario-not-observed").
/// - "scenario-samples-as-sent" (note) for a scenario with samples: as_sent that reads a value.
[[nodiscard]] std::vector<config::Finding> scenario_samples_findings(const config::CheckReport& check, const Scenario& scenario,
                                                             std::string_view launcher_version);

} // namespace vehicle_sil::scenario
