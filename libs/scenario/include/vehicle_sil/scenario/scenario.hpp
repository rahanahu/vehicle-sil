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
#include "vehicle_sil/config/config.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vehicle_sil::scenario {

/// The version of the scenario format (its "vehicle_sil_scenario" key).
inline constexpr int kScenarioFormat = 1;
/// The version of the scenario template format (its "vehicle_sil_scenario_template" key).
inline constexpr int kScenarioTemplateFormat = 1;

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
    /// The ends of its range as its ECU describes them (empty: none), for the messages of the run
    /// (which end a refused value is beyond). Whether a value is taken is the hub's and the
    /// agent's to say, not these.
    std::optional<double> min{};
    std::optional<double> max{};
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
/// "eq", "ne", "ge", "le", "gt", "lt", "near".
[[nodiscard]] const char* compare_name(Compare op);

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
    /// Of a set, as in the step's text: "Body.level = 20".
    std::string text{};
};

/// A value over the time of a step, as a function of the virtual time since the step began and of
/// nothing else: not of how often or when it is asked, so a run gives the same values whatever
/// the steps of the launcher and of the ECUs are. What a ramp and every wave is.
class Waveform
{
public:
    virtual ~Waveform() = default;
    /// The value at `t` after the step began (0 <= t < the length). `base` is the value the
    /// variable has when the step begins, or the one the step names instead (ScenarioMotion::base);
    /// 0 if uses_base() is false.
    /// The shapes a dither takes (ScenarioOverlay: sine, triangle, sawtooth, a square with an
    /// amplitude, noise) repeat or go on without an end: they give a value for every t >= 0,
    /// however large, and a dither asks them with the virtual time itself and the base 0.
    [[nodiscard]] virtual double value(SimDuration t, double base) const = 0;
    /// The value written at the end of the length, which the variable keeps.
    [[nodiscard]] virtual double end(double base) const = 0;
    /// Whether value() or end() looks at `base`: if so, the step fails when no value is known.
    [[nodiscard]] virtual bool uses_base() const { return true; }
};

/// The shape of a ramp or a wave as the step's text gives it, for what shows a motion without
/// running it (the lines of a timeline file): its name and the values the text has, with the
/// parameters put in. What the text leaves out is not here (the defaults: docs/scenario.md), nor
/// is the base (a ramp's or a lag's from, a wave's around: ScenarioMotion::base).
struct ScenarioShapeText
{
    /// A whole number as it is (a seed keeps every digit), another number, true or false, a word.
    using Value = std::variant<double, std::int64_t, bool, std::string>;
    /// "ramp", or the wave's shape ("sine", "square", ..., "points").
    std::string type;
    /// The keys of the shape with their values, in the order the format lists them; then for_ms.
    /// Of points: "points" (how many), "interpolate" ("linear" or "hold"), and of points from a
    /// file "file", "value" and "time" (the names of the columns) and "time_unit", as given.
    std::vector<std::pair<std::string, Value>> values{};
};

/// One variable of a ramp or a wave: the engine writes shape->value() at every step of the
/// variable's ECU for length_ms, then shape->end().
struct ScenarioMotion
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
    ScenarioShapeText described{};
};

/// One parameter value captured on a template call frame (call_stack).
struct SourceParam
{
    std::string name;
    std::string text;
    bool quoted = false;
    config::ParamSource source = config::ParamSource::TemplateCall;

    friend bool operator==(const SourceParam&, const SourceParam&) = default;
};

/// One template call frame enclosing a ScenarioStep, ScenarioOverlay, or ScenarioStepRun (outermost -> immediate).
struct SourceFrame
{
    std::string file;                  ///< caller file path as shown
    std::size_t line = 0;              ///< 1-based line of the `- template:` call step in `file`
    std::string name{};                ///< optional human `name:` on that call step, or ""
    std::vector<SourceParam> params{}; ///< resolved arguments passed to this template call

    friend bool operator==(const SourceFrame&, const SourceFrame&) = default;
};

/// Formats `stack` as human-readable "called from: ..." lines (innermost caller first), joined by
/// '\n' with no trailing newline. Each line starts with `line_prefix`. Empty string when `stack` is empty.
[[nodiscard]] std::string format_call_stack_text(const std::vector<SourceFrame>& stack, std::string_view line_prefix = {});

/// Optional metadata derived from called templates' `parameters:` definitions for a scenario parameter.
struct ScenarioParamMeta
{
    std::string name;
    /// "integer", "number", "boolean", or "string" (empty if unconstrained by any template parameter).
    std::string type{};
    /// Allowed values as strings (empty if unconstrained by `values:`).
    std::vector<std::string> values{};
    /// First non-empty `description:` from a called template's parameter definition (empty if none).
    std::string description{};

    friend bool operator==(const ScenarioParamMeta&, const ScenarioParamMeta&) = default;
};

/// Per-case summary for a scenario that defines `cases:`.
struct ScenarioCaseDetail
{
    std::string name;
    /// Effective `only` variant list for this case (scenario `only` intersected with case `only`),
    /// or empty optional when unrestricted.
    std::optional<std::vector<std::string>> only{};

    friend bool operator==(const ScenarioCaseDetail&, const ScenarioCaseDetail&) = default;
};

/// A dither of one input: a shape around 0 without an end that is on top of whatever the scenario
/// puts on the input. The value written is that of the input's line (a set's value, a ramp's or a
/// wave's value then) plus the shape's value at the virtual time itself, as the variable's type
/// has it. An input has one; a dither step puts another in its place or takes it off.
struct ScenarioOverlay
{
    std::size_t variable = 0;
    /// Asked with the virtual time itself and the base 0 (Waveform::value()); null: none (the
    /// step takes the dither off).
    std::shared_ptr<const Waveform> shape{};
    std::string file{};   ///< origin file path if from a template ("" if root scenario)
    std::size_t line = 0; ///< of a dither step the step's line, of one that "inputs" declares its entry's
    /// As in the step's text: "Body.ratio triangle amplitude 0.1 period 40 ms", "Body.ratio none".
    std::string text;
    /// The shape as the text gives it; `type` is "" for none.
    ScenarioShapeText described{};
    std::string name{};                    ///< optional human-assigned step `name:` ("" for inputs or omitted)
    std::vector<SourceFrame> call_stack{}; ///< outermost caller -> immediate caller (empty for direct steps / inputs)
};

enum class StepKind
{
    Set,
    Ramp,
    Wave,
    Expect,
    WaitUntil,
    Dither,
};
/// "set", "ramp", "wave", "expect", "wait_until", "dither".
[[nodiscard]] const char* step_kind_name(StepKind kind);

struct ScenarioStep
{
    StepKind kind = StepKind::Set;
    std::string file{}; ///< origin file path if from a template ("" if root scenario)
    std::size_t line = 0;
    /// Its time: at_ms, or after_ms from the end of the step before it (0 without either).
    std::optional<std::int64_t> at_ms{};
    std::int64_t after_ms = 0;
    std::vector<ScenarioWrite> writes{};       ///< Set
    std::vector<ScenarioMotion> motions{};     ///< Ramp, Wave
    std::vector<ScenarioOverlay> overlays{};   ///< Dither: per variable the dither from the step's time on
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
    /// (`writes`, `motions`, `overlays` and `conditions` are empty) and ends `skipped_ms` after its own time.
    std::string skipped{};
    /// Of a skipped step: the time it takes as the text says it (the longest over_ms of a ramp,
    /// the longest length of a wave, the hold_for_ms of an expect), so that the steps after it have the same times in every
    /// variant. 0 for a set and for a step that waits (when it would end is not in the text).
    std::int64_t skipped_ms = 0;
    /// Of a ramp or a wave that runs: why the run does not check the expect beside it (it uses a
    /// variable that is absent or of a participant the variant leaves out, as `skipped` says it), or
    /// "". `conditions` is then empty; the ramp or wave writes as it would with the expect.
    std::string expect_skipped{};
    std::string name{};                    ///< optional human-assigned step `name:` ("" if omitted)
    std::vector<SourceFrame> call_stack{}; ///< outermost caller -> immediate caller (empty for direct steps)

    /// Its end is not known before it runs (WaitUntil, Expect with within_ms).
    [[nodiscard]] bool waits() const noexcept { return within_ms.has_value(); }
};

/// One thing a scenario puts on an input, as the text says it, for the list of the lines that the
/// check shows. The members that a line of a timeline file has too (TimelineRecord::Line, which
/// is what a run did) mean the same here: variable, kind, line, step, from_ms, to_ms, shape, text.
struct ScenarioLineText
{
    /// Where its times come from, as far as the check knows them before the run.
    enum class TimeSource
    {
        Text,       ///< from_ms (and to_ms, if it has an end) are as the text says
        AfterWait,  ///< not known: its step comes after one that waits (`after_wait`)
        FirstWrite, ///< a dither that "inputs" declares: it begins with the first write of its input
    };
    std::string variable; ///< "<participant>.<name>"
    std::string kind;     ///< "set", "ramp", "wave", "dither"
    std::string file{};   ///< origin file path if from a template ("" if root scenario)
    std::size_t line = 0; ///< the step's; of a dither that "inputs" declares its entry's
    /// The step's index, from 1; empty for a dither that "inputs" declares.
    std::optional<std::size_t> step{};
    /// The step's time; empty if `time_source` is not Text.
    std::optional<std::int64_t> from_ms{};
    /// Where it ends by the text (a set: its time); empty if `time_source` is not Text, and for a
    /// dither, which has no end (as in a timeline file).
    std::optional<std::int64_t> to_ms{};
    TimeSource time_source = TimeSource::Text;
    std::string after_wait_file{}; ///< AfterWait: the file of the step that waits ("" if root)
    std::size_t after_wait = 0;    ///< AfterWait: the line of the step that waits
    /// What it is, as the step's text has it for this variable: "Body.level = 50",
    /// "Body.level to 80 over 1000 ms", "Body.level triangle amplitude 2 period 40 ms", "Body.level none".
    std::string text;
    /// Of a ramp, a wave and a dither: the shape as the text gives it; empty for a set and for none.
    std::optional<ScenarioShapeText> shape{};
};
/// "text", "after_wait", "first_write".
[[nodiscard]] const char* line_time_source_name(ScenarioLineText::TimeSource source);

/// The scenario's "samples": how often the ECUs whose values it reads must send them.
enum class ScenarioSamples
{
    /// Every step of the ECU (the default): the run asks those ECUs for it and does not begin with
    /// one that sends less often ("scenario-not-observed").
    EveryStep,
    /// As each ECU sends them (its AgentOptions::sample_period): nothing is asked or refused, and
    /// a condition does not see what a variable does between two samples.
    AsSent,
};
/// "every_step", "as_sent".
[[nodiscard]] const char* scenario_samples_name(ScenarioSamples samples);

/// A scenario that was read and checked against the participants: it can run as it is.
struct Scenario
{
    std::string name; ///< its "name", or ""
    std::string case_name{};
    std::vector<std::string> cases{};
    std::vector<config::ParamValue> params{};
    ScenarioSamples samples = ScenarioSamples::EveryStep; ///< its "samples"
    std::optional<std::int64_t> end_ms{};                 ///< optional "end_ms": virtual end time in ms
    std::vector<ScenarioParticipant> participants{};
    std::vector<ScenarioVariable> variables{};
    /// The dithers the file declares (its "inputs"), in the order of the file: each is on its
    /// input from the start. Those of variables this configuration does not have are left out
    /// (a note of the check says so).
    std::vector<ScenarioOverlay> inputs{};
    std::vector<ScenarioStep> steps{};
    /// The file has "inputs" or a dither step that runs: its outputs for people then list `inputs`
    /// and `lines` (the JSON always has them: empty without).
    bool has_dither = false;
    /// With has_dither: what "inputs" declares and what each step that runs puts on an input
    /// that has a dither at some time (a declared one, or a dither step's, also none), in the
    /// order of the file (the declared dithers first). Not the writes of the other inputs.
    std::vector<ScenarioLineText> lines{};
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
    std::vector<config::ParamValue> params{};
    /// The command line's parameters are for many scenarios (vehicle_sil_launch test): one that
    /// this scenario neither declares nor uses is left alone (no ScenarioRead::usage_errors). The
    /// caller says it if no scenario has it (unused_test_params()).
    bool params_for_many = false;
    /// The request file (as the user named it) whose own `scenario` this is: its params
    /// (ParamSource::RequestFile) that the scenario neither declares nor uses are then a usage
    /// error too, naming the file. Empty: they are left alone and named in
    /// ScenarioRead::unused_request_params (one file serves several scenarios).
    std::string request_file{};
    /// Reads a file the scenario names (a wave's `points: { file: <path> }`). `path` is relative to
    /// the root scenario's directory, '/' between directories, normalized without escaping above it.
    /// Returns the bytes, or empty and `problem` (what is wrong and how to fix it, after "<path> ").
    /// files_beside() (probe.hpp) gives the one of a scenario file. Not set: a scenario that names a
    /// file is an error ("scenario-points-file").
    std::function<std::optional<std::string>(const std::string& path, std::string& problem)> read_file{};
    /// The root scenario path as shown (e.g. "scenarios/load.yaml"), used to display template file
    /// paths in findings and call stacks. Empty when reading in-memory text without a file path.
    std::string scenario_file{};
    /// Optional callback to read a template file (by path relative to the root scenario's directory).
    /// When empty, `read_file` is used.
    std::function<std::optional<std::string>(const std::string& path, std::string& problem)> read_template{};
    /// Selected case name from `cases:`, or empty when none was given.
    std::optional<std::string> case_name{};
    /// When `case_name` came from a request file or inline serve intent, its shown name (e.g.
    /// "bench.json" or "intent"), used to attribute case usage errors consistently. Empty for CLI `--case`.
    std::string case_request_file{};
};

struct ScenarioRead
{
    /// Set if the text has no error.
    std::optional<Scenario> scenario;
    /// Its "name" and the number of its steps, as far as the text could be read.
    std::string name{};
    std::string case_name{};
    std::vector<std::string> cases{};
    std::vector<ScenarioCaseDetail> case_details{};
    std::size_t steps = 0;
    std::optional<std::int64_t> end_ms{};
    /// The parameters the scenario declares ("params") or uses ("${name}"), each with the value in
    /// effect and where it comes from; in the order of "params", then of first use.
    std::vector<config::ParamValue> params{};
    /// Optional parameter metadata (type, values, description) derived from called templates.
    std::vector<ScenarioParamMeta> param_meta{};
    /// Parameter names required by the scenario (undefined root `${name}` or omitted required
    /// template parameters) that currently have no value.
    std::vector<std::string> missing_params{};
    /// Its "only" (the variants it is for), if written and readable.
    std::optional<std::vector<std::string>> only{};
    /// The scenario is not for the chosen variant (`only` does not list it): an error
    /// ("scenario-variant"), and its steps were not checked. A caller that runs many scenarios
    /// skips it instead.
    bool not_for_variant = false;
    /// How many steps the run will skip (ScenarioStep::skipped), each with a note in `findings`.
    std::size_t skipped = 0;
    /// How many expects beside a ramp or a wave the run will not check (ScenarioStep::expect_skipped;
    /// the ramp or wave runs), each with a note in `findings`. Not counted in `skipped`.
    std::size_t unchecked_expects = 0;
    /// Every error of the text, a note for each step that is skipped and for each expect beside a ramp
    /// or a wave that is not checked (ScenarioStep::expect_skipped), and a warning
    /// ("scenario-nothing-checked") if every step that checks is skipped, in the order of the file
    /// (Finding::file is set for template files and empty for the root scenario).
    std::vector<config::Finding> findings;
    /// What is wrong with the command line, known only with the scenario (code "usage"): a
    /// parameter given from the command line (ParamSource::CommandLine) that the scenario neither
    /// declares nor uses. Looked for only when the text has no error of its parameters.
    std::vector<config::Finding> usage_errors{};
    /// The names of the request file's params (ParamSource::RequestFile) that the scenario neither
    /// declares nor uses, when ScenarioContext::request_file is empty: left alone, for the caller
    /// to note.
    std::vector<std::string> unused_request_params{};
    /// True when parameter discovery could not finish (an early parse, encoding, version, header,
    /// or variant-skip abort, a failed inputs/steps parameter resolution, or a failing template
    /// call or template file), so unused-parameter checks cannot tell whether an external parameter
    /// is used.
    bool params_incomplete = false;
};

/// Reads a scenario (YAML, or JSON if it starts with '{') and checks all of it: the form, unknown
/// keys, its parameters ("${name}" as a whole value; each needs a value from the scenario's
/// "params" or from `context`), the times and their order, and against `catalog` every variable
/// (it exists, its participant is usable, the value fits its type, range and enum items, a written
/// variable is an input, or a parameter in a step at 0). Reports every problem at once, except
/// that the steps are not read while a parameter has an error or the scenario is not for the
/// variant. No file system access but through ScenarioContext::read_file and
/// ScenarioContext::read_template (CSV points and template files, read once per normalized path).
[[nodiscard]] ScenarioRead read_scenario(std::string_view text, const ScenarioCatalog& catalog,
                                         const ScenarioContext& context = {});

/// Per-case inspection result recorded by inspect_scenario() so callers do not re-parse per case.
struct CaseInspection
{
    std::string name;
    std::vector<std::string> param_names{};
    bool params_incomplete = false;
    bool has_error = false;
};

/// Lightweight scenario inspection for `test` multi-case expansion and `unused_params()`.
struct ScenarioInspection
{
    bool has_cases = false;
    std::vector<std::string> cases{};
    std::vector<CaseInspection> case_inspections{};
    std::vector<std::string> param_names{};
    bool params_incomplete = false;
    std::vector<config::Finding> findings{};
};

[[nodiscard]] ScenarioInspection inspect_scenario(std::string_view text, const ScenarioContext& context = {});

/// Per participant of `scenario` (Scenario::participants): whether a step that runs reads a value
/// its ECU sends: a condition (expect, wait_until, the expect beside a ramp or a wave) names one of
/// its variables, or a ramp or a wave of one of its variables takes its base from the variable's
/// value (no from, no around) and no earlier step of the scenario has put a value on that
/// variable (a skipped step has not). The one place that says which ECUs a scenario needs samples of in
/// every step: the check's warning, the argument the run gives them, and what the run refuses.
[[nodiscard]] std::vector<bool> participants_read(const Scenario& scenario);

/// The dithers that the scenario's "inputs" declare, as lines of the list (kind "dither", no
/// step, TimeSource::FirstWrite, no end).
[[nodiscard]] std::vector<ScenarioLineText> scenario_inputs_text(const Scenario& scenario);

/// A value as the messages show it: true, 42, 1.5, or an enum's item ("on (1)").
[[nodiscard]] std::string value_text(const control::Value& value, const ScenarioVariable* variable = nullptr);

/// Whether `value` meets `comparison` (numbers are compared as numbers, whatever their types).
[[nodiscard]] bool holds(const ScenarioComparison& comparison, const control::Value& value);

} // namespace vehicle_sil::scenario
