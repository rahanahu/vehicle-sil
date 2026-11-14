// SPDX-License-Identifier: MIT
//
// The scenario file of vehicle_sil_launch (YAML or JSON): steps that write the ECUs' variables at
// virtual times and compare their values. Reading it checks the text and, against what the
// participants report about themselves (--describe), every variable it names; the result is a
// Scenario that needs no further check to run (scenario_run.hpp). Internal; the file format is in
// docs/scenario.md.
#pragma once

#include "vehicle_sil/control/variable_map.hpp"
#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/launch/config.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch {

struct CheckReport;

/// The version of the scenario format (its "vehicle_sil_scenario" key).
inline constexpr int kScenarioFormat = 1;

/// A variable a scenario uses: "<participant>.<name>" (the participant ends at the first '.').
struct ScenarioVariable
{
    std::size_t participant = 0; ///< index in Scenario::participants
    std::string name;            ///< the variable's name in its ECU
    control::ValueType type = control::ValueType::Real;
    /// Enum: its items (for the messages).
    std::vector<control::EnumItem> items{};

    /// "<participant>.<name>".
    std::string text;
};

/// A participant whose variables a scenario uses.
struct ScenarioParticipant
{
    std::string name;
    /// Its step (it runs in virtual time: the check refuses one that does not).
    SimDuration step{};
};

enum class Compare
{
    Eq,
    Ne,
    Ge,
    Le,
    Gt,
    Lt,
    Near, ///< |value - x| <= tolerance
};

struct ScenarioComparison
{
    Compare op = Compare::Eq;
    control::Value value{};
    double tolerance = 0.0; ///< Near only
};

/// All comparisons of one variable.
struct ScenarioCondition
{
    std::size_t variable = 0; ///< index in Scenario::variables
    std::vector<ScenarioComparison> all;
    /// As in the messages: "Engine.engine.speed ge 1700, le 1900".
    std::string text;
};

struct ScenarioWrite
{
    std::size_t variable = 0;
    control::Value value{};
};

/// A value over the time of a step, as a function of the virtual time since the step began and of
/// nothing else: not of how often or when it is asked, so a run gives the same values whatever
/// the steps of the launcher and of the ECUs are. What a ramp and every wave is.
class Waveform
{
public:
    virtual ~Waveform() = default;
    /// The value at `t` after the step began (0 <= t < the length). `base` is the value the
    /// variable has when the step begins, or the one the step names instead (ScenarioSignal::base);
    /// 0 if uses_base() is false.
    [[nodiscard]] virtual double value(SimDuration t, double base) const = 0;
    /// The value written at the end of the length, which the variable keeps.
    [[nodiscard]] virtual double end(double base) const = 0;
    /// Whether value() or end() looks at `base`: if so, the step fails when no value is known.
    [[nodiscard]] virtual bool uses_base() const { return true; }
};

/// One variable of a ramp or a wave: the engine writes shape->value() at every step of the
/// variable's ECU for length_ms, then shape->end().
struct ScenarioSignal
{
    std::size_t variable = 0;
    std::shared_ptr<const Waveform> shape;
    std::int64_t length_ms = 0; ///< > 0
    /// The base as the step names it (a ramp's from, a wave's around, the other value of a bool);
    /// empty: the value the variable has when the step begins.
    std::optional<double> base{};
    /// What to do when no base is known, for the message that says so: "give the ramp from: <value>".
    std::string hint;
    /// As in the step's text: "Body.level from 10 to 20 over 100 ms".
    std::string text;
};

enum class StepKind
{
    Set,
    Ramp,
    Wave,
    Expect,
    WaitUntil,
};
/// "set", "ramp", "wave", "expect", "wait_until".
[[nodiscard]] const char* step_kind_name(StepKind kind);

struct ScenarioStep
{
    StepKind kind = StepKind::Set;
    std::size_t line = 0;
    /// Its time: at_ms, or after_ms from the end of the step before it (0 without either).
    std::optional<std::int64_t> at_ms{};
    std::int64_t after_ms = 0;
    std::vector<ScenarioWrite> writes{};       ///< Set
    std::vector<ScenarioSignal> signals{};     ///< Ramp, Wave
    /// Expect, WaitUntil: all of them. Ramp, Wave: the expect beside it (empty without one), which
    /// must be true from the step's time to its end, both included.
    std::vector<ScenarioCondition> conditions{};
    /// Expect with within_ms, WaitUntil (its timeout_ms): true at some sample of this long.
    std::optional<std::int64_t> within_ms{};
    /// Expect with hold_for_ms: true at every sample of this long.
    std::optional<std::int64_t> hold_for_ms{};
    /// What it does, in one line: "set Engine.engine.throttle = 50".
    std::string text;
    /// Why the run skips it, or "" if it runs: it uses a variable that its participant does not
    /// have in this configuration (absent: true in its variable map), or a variable of a
    /// participant that the chosen variant leaves out. A skipped step writes and compares nothing
    /// (`writes`, `signals` and `conditions` are empty) and ends `skipped_ms` after its own time.
    std::string skipped{};
    /// Of a skipped step: the time it takes as the text says it (the longest over_ms of a ramp,
    /// the longest length of a wave, the hold_for_ms of an expect), so that the steps after it have the same times in every
    /// variant. 0 for a set and for a step that waits (when it would end is not in the text).
    std::int64_t skipped_ms = 0;
    /// Of a ramp or a wave that runs: why the run does not check the expect beside it (it uses a
    /// variable that is absent or of a participant the variant leaves out, as `skipped` says it), or
    /// "". `conditions` is then empty; the ramp or wave writes as it would with the expect.
    std::string expect_skipped{};

    /// Its end is not known before it runs (WaitUntil, Expect with within_ms).
    [[nodiscard]] bool waits() const noexcept { return within_ms.has_value(); }
};

/// A scenario that was read and checked against the participants: it can run as it is.
struct Scenario
{
    std::string name; ///< its "name", or ""
    std::vector<ScenarioParticipant> participants{};
    std::vector<ScenarioVariable> variables{};
    std::vector<ScenarioStep> steps{};
};

/// What a scenario may use of one participant of the configuration.
struct CatalogParticipant
{
    std::string name;
    /// Why its variables cannot be used ("" if they can): it is disabled, was not described, runs
    /// in real time, does not offer its variables. A whole sentence part after "participant 'X' ".
    std::string unusable{};
    SimDuration step{};
    std::vector<control::VariableInfo> variables{};
    /// The names its variable map says it does not have (absent: true): a step that uses one is
    /// skipped.
    std::vector<std::string> absent{};
    /// The chosen variant leaves the participant out (its "enabled: false" in the variant, see
    /// ParticipantConfig::left_out_by_variant): a step that uses any variable of it is skipped,
    /// like one that uses an absent variable. `unusable` is then not looked at.
    bool not_in_variant = false;
    /// Its variable map applied to what it describes (`variables` is its schema()), or null if
    /// its entry has none. The range of a written value is the map's to say
    /// (control::MappedSchema::in_range()): the run's hub decides with the same code.
    std::shared_ptr<const control::MappedSchema> map{};
};
using ScenarioCatalog = std::vector<CatalogParticipant>;

/// The catalog of a checked configuration (CheckReport::config must be set).
[[nodiscard]] ScenarioCatalog scenario_catalog(const CheckReport& check);

/// What a scenario is read for, besides the participants: the variant and the parameters.
struct ScenarioContext
{
    /// The variant of the configuration that was chosen, or "".
    std::string variant{};
    /// The variants the configuration defines (what a scenario's "only" may name).
    std::vector<std::string> variants{};
    /// The parameters from outside the scenario, a later one over an earlier one of its name: the
    /// configuration's (with the variant's), then the command line's. They replace the scenario's
    /// defaults. One of ParamSource::CommandLine that the scenario neither declares nor uses is an
    /// error (a typing error would otherwise change nothing, silently).
    std::vector<ParamValue> params{};
    /// The command line's parameters are for many scenarios (vehicle_sil_launch test): one that
    /// this scenario neither declares nor uses is left alone (no ScenarioRead::usage_errors). The
    /// caller says it if no scenario has it (unused_test_params()).
    bool params_for_many = false;
};

struct ScenarioRead
{
    /// Set if the text has no error.
    std::optional<Scenario> scenario;
    /// Its "name" and the number of its steps, as far as the text could be read.
    std::string name{};
    std::size_t steps = 0;
    /// The parameters the scenario declares ("params") or uses ("${name}"), each with the value in
    /// effect and where it comes from; in the order of "params", then of first use.
    std::vector<ParamValue> params{};
    /// Its "only" (the variants it is for), if written and readable.
    std::optional<std::vector<std::string>> only{};
    /// The scenario is not for the chosen variant (`only` does not list it): an error
    /// ("scenario-variant"), and its steps were not checked. A caller that runs many scenarios
    /// skips it instead.
    bool not_for_variant = false;
    /// How many steps the run will skip (ScenarioStep::skipped), each with a note in `findings`.
    std::size_t skipped = 0;
    /// Every error of the text, a note for each step that is skipped and for each expect beside a ramp
    /// or a wave that is not checked (ScenarioStep::expect_skipped), and a warning
    /// ("scenario-nothing-checked") if every step that checks is skipped, in the order of the file
    /// (Finding::file is not set).
    std::vector<Finding> findings;
    /// What is wrong with the command line, known only with the scenario (code "usage"): a
    /// parameter given from the command line (ParamSource::CommandLine) that the scenario neither
    /// declares nor uses. Looked for only when the text has no error of its parameters.
    std::vector<Finding> usage_errors{};
};

/// Reads a scenario (YAML, or JSON if it starts with '{') and checks all of it: the form, unknown
/// keys, its parameters ("${name}" as a whole value; each needs a value from the scenario's
/// "params" or from `context`), the times and their order, and against `catalog` every variable
/// (it exists, its participant is usable, the value fits its type, range and enum items, a written
/// variable is an input, or a parameter in a step at 0). Reports every problem at once, except
/// that the steps are not read while a parameter has an error or the scenario is not for the
/// variant. No file system access.
[[nodiscard]] ScenarioRead read_scenario(std::string_view text, const ScenarioCatalog& catalog,
                                         const ScenarioContext& context = {});

/// A value as the messages show it: true, 42, 1.5, or an enum's item ("on (1)").
[[nodiscard]] std::string value_text(const control::Value& value, const ScenarioVariable* variable = nullptr);

/// Whether `value` meets `comparison` (numbers are compared as numbers, whatever their types).
[[nodiscard]] bool holds(const ScenarioComparison& comparison, const control::Value& value);

} // namespace vehicle_sil::launch
