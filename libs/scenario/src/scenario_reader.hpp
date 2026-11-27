// SPDX-License-Identifier: MIT
//
// The reader of a scenario's text (read_scenario()): one class, whose definitions are split by
// subject: scenario.cpp (the file, its parameters and variants, the steps and their conditions),
// scenario_values.cpp (variables and their values, hexadecimal included), scenario_waves.cpp
// (waves, their shapes and points, and what the ECU's step makes of them) and scenario_inputs.cpp
// ("inputs", dithers, and what the steps put on the inputs). Internal.
#pragma once

#include "vehicle_sil/scenario/scenario.hpp"

#include "vehicle_sil/scenario/detail/scenario_csv.hpp"
#include "vehicle_sil/scenario/detail/scenario_line.hpp"
#include "vehicle_sil/scenario/detail/scenario_shapes.hpp"
#include "vehicle_sil/config/detail/yaml_reader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <system_error>
#include <utility>

namespace vehicle_sil::scenario::reader {

// kTopKeys is what the message of an unknown key shows; the reader takes each key in a branch of its own. A key with
// a new branch must be added here too, or the JSON Schema (schema/) and its test (tests/schema), which compare with
// the list in the message, pass without the key. (The keys of a step are taken from kStepKeys itself.)

inline const std::vector<std::string> kTopKeys = {"vehicle_sil_scenario", "name", "samples", "only", "params", "cases", "inputs", "end_ms", "steps"};
inline const std::vector<std::string> kCaseKeys = {"params", "only"};
inline const std::vector<std::string> kStepKeys = {"name",       "at_ms",      "after_ms", "set",        "ramp",       "wave",
                                                   "dither",     "wait_until", "timeout_ms", "expect",  "within_ms",  "hold_for_ms",
                                                   "undersampled", "template", "params"};
inline const std::vector<std::string> kActionKeys = {"set", "ramp", "wave", "dither", "wait_until", "expect", "template"};
inline const std::vector<std::string> kTemplateTopKeys = {"vehicle_sil_scenario_template", "name", "parameters", "steps"};
inline const std::vector<std::string> kParameterDefKeys = {"type", "default", "values", "description"};
inline const std::vector<std::string> kParameterTypes = {"integer", "number", "boolean", "string"};
inline constexpr std::size_t kMaxTemplateDepth = 16;
inline constexpr std::size_t kMaxExpandedSteps = 10'000;
inline const std::vector<std::string> kCompareKeys = {"eq", "ne", "ge", "le", "gt", "lt", "near", "tolerance"};
inline const std::vector<std::string> kConditionKeys = {"eq", "ne", "ge", "le", "gt", "lt", "near", "tolerance", "variable"};
inline const std::vector<std::string> kVariableRefKeys = {"variable"};
inline const std::vector<std::string> kRampKeys = {"to", "over_ms", "from"};
// A wave: the shapes (one per variable) and the keys beside the shape.
inline const std::vector<std::string> kWaveShapes = {"sine", "triangle", "sawtooth", "square", "pulse",
                                                     "noise", "chirp", "lag", "chatter", "points"};
inline const std::vector<std::string> kWaveKeys = {"sine",  "triangle", "sawtooth", "square", "pulse",  "noise",      "chirp",
                                                   "lag",   "chatter",  "points",   "around", "for_ms", "interpolate"};
// A dither: what an entry of "inputs" may have, and the shapes that can be one (around 0, without an end).
inline const std::vector<std::string> kInputKeys = {"dither", "undersampled"};
inline const std::vector<std::string> kDitherShapes = {"sine", "triangle", "sawtooth", "square", "noise"};
inline constexpr const char* kDitherExample = "{ triangle: { amplitude: 1, period_ms: 100 } }";
// The keys of points from a CSV file (points: { file: ..., value: ... }).
inline const std::vector<std::string> kPointsFileKeys = {"file", "value", "time", "time_unit"};
inline constexpr const char* kPointsFileExample = "points: { file: traces/drive.csv, value: throttle }";
// How a wave that an ECU cannot see as written is run anyway (in every such message).
inline constexpr const char* kAsItIs = "; to run it as it is without this message, add undersampled: ok to the step";
// The same for a dither that "inputs" declares.
inline constexpr const char* kAsItIsInput = "; to run it as it is without this message, add undersampled: ok to the input's entry in inputs";

using config::detail::join;

std::string number_text(double value);
const char* type_name(control::ValueType type);
double as_number(const control::Value& value);
std::string items_text(const std::vector<control::EnumItem>& items);

template <class Fn>
class ScopeGuard
{
public:
    explicit ScopeGuard(Fn fn) : fn_(std::move(fn)) {}
    ~ScopeGuard() { fn_(); }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

private:
    Fn fn_;
};

enum class ParamType
{
    Integer,
    Number,
    Boolean,
    String,
};

struct ParameterDef
{
    std::string name;
    ParamType type = ParamType::String;
    std::size_t line = 0;
    std::optional<config::ParamValue> default_value{};
    std::vector<config::ParamValue> allowed_values{};
    std::string description{};
};

struct CachedTemplate
{
    std::unique_ptr<config::detail::YamlDocument> doc;
    std::string shown_file;
    std::string dir_rel;
    std::vector<ParameterDef> parameters;
    ryml::id_type steps_node = ryml::NONE;
    bool valid = false;
    std::string read_error{};
    std::string invalid_summary{};
};

/// A file the scenario names, as read once (ScenarioContext::read_file, read_csv()).
struct CsvFile
{
    std::optional<detail::CsvTable> table;
    std::string problem; ///< if there is no table: after the file's name
};

/// Cache of parsed template files and CSV points files, reusable across cases in inspect_scenario().
struct SharedReadCache
{
    std::map<std::string, CachedTemplate> templates{};
    std::map<std::string, CsvFile> files{};
};

class Reader : config::detail::YamlReader
{
public:
    Reader(const config::detail::YamlDocument& doc, const ScenarioCatalog& catalog, const ScenarioContext& context,
           ScenarioRead& out, bool check_catalog = true, SharedReadCache* shared_cache = nullptr) :
      config::detail::YamlReader(doc, out.findings), catalog_(catalog), context_(context), out_(out),
      check_catalog_(check_catalog), cache_(shared_cache != nullptr ? *shared_cache : owned_cache_)
    {
        const auto slash = context_.scenario_file.find_last_of("/\\");
        if (slash != std::string::npos)
        {
            scenario_dir_prefix_ = context_.scenario_file.substr(0, slash);
            std::replace(scenario_dir_prefix_.begin(), scenario_dir_prefix_.end(), '\\', '/');
            while (!scenario_dir_prefix_.empty() && scenario_dir_prefix_.back() == '/')
            {
                scenario_dir_prefix_.pop_back();
            }
        }
        scopes_.push_back(EvalScope{&doc, context_.scenario_file, ""});
    }

    void read();
    bool has_cases() const noexcept { return cases_written_; }
    bool cases_incomplete() const noexcept { return cases_incomplete_; }

private:
    const config::detail::YamlDocument& doc() const noexcept override { return *scopes_.back().doc; }
    const ryml::Tree& tree() const noexcept override { return scopes_.back().doc->tree(); }

    std::string current_file() const { return scopes_.size() <= 1 ? std::string{} : scopes_.back().file; }

    // --- findings ---

    void add(config::Severity severity, std::string code, std::size_t line, std::string message,
             std::string participant = {}, std::string network = {}) override
    {
        findings_.push_back(config::Finding{severity, std::move(code), std::move(message), line,
                                            std::move(participant), std::move(network),
                                            format_call_stack(active_templates_), current_file()});
    }
    void add_at(config::Severity severity, std::string code, const std::string& file, std::size_t line,
                std::string message, std::string participant = {}, std::string network = {},
                std::string detail = {})
    {
        findings_.push_back(config::Finding{severity, std::move(code), std::move(message), line,
                                            std::move(participant), std::move(network), std::move(detail), file});
    }
    static std::string format_call_stack(const std::vector<SourceFrame>& stack);
    static std::string loc_text(const std::string& file, std::size_t line)
    {
        return file.empty() ? "line " + std::to_string(line) : file + ":" + std::to_string(line);
    }
    static std::string short_loc(const std::string& file, std::size_t line)
    {
        return file.empty() ? std::to_string(line) : file + ":" + std::to_string(line);
    }

    // --- parameters ---

    /// A parameter that has a value: from the scenario's "params" (`declared`) or from outside.
    struct Known
    {
        config::ParamValue value;
        bool declared = false;
        bool used = false;                     ///< a step has "${name}"
        std::optional<std::string> root_param; ///< root scenario parameter name if bound/forwarded from root scope
    };

    /// Parameter constraint lattice accumulated for a root scenario parameter across called templates
    /// and direct steps. Only `template_type` / `allowed_values` / `description` (from template ParameterDefs)
    /// are emitted as metadata; `effective_type` also tracks direct step usages for static conflict checks.
    struct ParamConstraint
    {
        std::optional<ParamType> template_type{};
        std::vector<config::ParamValue> allowed_values{};
        bool has_values = false;
        bool saw_number = false;
        std::string description{};
        std::optional<ParamType> effective_type{};
        std::string first_origin{};
        bool conflict_reported = false;
    };

    struct Base;
    struct Dither;
    struct StepReadState;

    /// Records a constraint from a template ParameterDef on root parameter `root_name`.
    void record_template_param_constraint(const std::string& root_name, const ParameterDef& def, std::size_t line,
                                          bool emit_errors);
    /// Records a direct typed step usage of a whole-value `${param}` node (conflict check only, not emitted as metadata).
    void record_direct_param_usage(ryml::id_type node, ParamType usage_type);
    static std::optional<ParamType> join_param_types(ParamType a, ParamType b) noexcept;
    static std::optional<std::int64_t> exact_int64_value(const config::ParamValue& pv);
    static std::optional<config::ParamValue> normalize_param_for_type(config::ParamValue pv, ParamType type, bool force_decimal);
    static bool value_fits_type_quiet(const config::ParamValue& pv, ParamType type);
    void collect_cli_string_params(ryml::id_type steps_node);
    void scan_cli_string_bindings(ryml::id_type steps_node, std::set<std::string>& has_string,
                                  std::set<std::string>& has_non_string);
    bool is_cli_string_param(ryml::id_type node) const;
    void add_missing_param(const std::string& name);
    /// Lightweight cache-sharing scan of inputs, steps, and templates for unselected/unknown case metadata
    /// (and fallback metadata collection when parameter resolution fails). Emits no findings.
    void scan_metadata_only(ryml::id_type inputs_node, ryml::id_type steps_node);
    void scan_scalar_param_refs(ryml::id_type node, bool allow_interpolation);
    void scan_interpolated_text_refs(const std::string& text);
    void note_param_ref_metadata(const std::string& name);
    void scan_steps_metadata(ryml::id_type steps_node);

    /// The scenario's defaults (`node`, or ryml::NONE), then the context's and chosen case's by rank.
    void take_params(ryml::id_type node, const std::vector<config::ParamValue>* case_params = nullptr);

    /// Finds every "${name}" under `node` (the steps): as a whole value it stands for the
    /// parameter's value from here on (param_at()); anywhere else, or without a value, it is an
    /// error. false if there was one.
    bool resolve_params(ryml::id_type node);

    /// Resolves parameters in `steps` sequence: step `name:` scalars support `${name}` string
    /// interpolation; all other values remain whole-value `${name}` only.
    bool resolve_step_params(ryml::id_type steps_node);

    /// Interpolates zero or more `${name}` placeholders in a scalar string (no `$${` escape).
    bool resolve_interpolated_string(ryml::id_type node);
    bool resolve_interpolated_string_text(const std::string& text, std::size_t line, std::string& out);
    void report_undefined_param(const std::string& name, std::size_t line);

    /// A --param that the scenario neither declares nor uses changes nothing: an error.
    void check_given_params();

    /// A scenario whose steps that check (expect, wait_until) are all skipped passes whatever the
    /// ECUs do: a warning, so that --strict does not take it for a pass. One that never had such
    /// a step (only set and ramp) is what its author wrote, and is left alone.
    void check_something_is_checked();

    /// The parameter whose value stands at `node`, or null.
    const config::ParamValue* param_at(ryml::id_type node) const;

    // What the steps' values are read with: a "${name}" reads as its parameter's value.
    std::string text_of(ryml::id_type node) const;
    bool quoted(ryml::id_type node) const;
    bool plain_integer(ryml::id_type node, std::int64_t& value) const;
    bool plain_number(ryml::id_type node, double& value) const;
    std::string shown_value(ryml::id_type node) const;

    // --- hexadecimal: the one place a scenario reads it (values of integer and enum variables) ---

    /// A value of a shape as its text has it, for ScenarioShapeText (the shape was read without an
    /// error): a whole number, a number (of an integer variable also hexadecimal), true or false,
    /// or the word.
    ScenarioShapeText::Value shape_value(ryml::id_type node, std::size_t variable) const;
    /// The unquoted value at `node` (or its parameter's) as hexadecimal; None if quoted.
    Hex hex_of(ryml::id_type node) const;
    /// A whole number written for a value of an integer or an enum variable: decimal, or
    /// hexadecimal (0x1F, 0X1F) without a sign and without quotes.
    bool whole_number(ryml::id_type node, std::int64_t& value) const;
    /// A number of the range of the variable, as a ramp or a wave writes it: decimal, or for an
    /// integer variable whole_number().
    bool number_of(ryml::id_type node, std::size_t variable, double& value) const;
    /// What a message adds when the value at `node` is hexadecimal that the variable of `type`
    /// cannot take: "" otherwise (also for a bool, whose message says true or false).
    std::string hex_hint(ryml::id_type node, control::ValueType type) const;
    /// A value at `node` as a message shows it: the number, and for hexadecimal also as written
    /// ("0x65 (101)"), as the range beside it is decimal.
    std::string written(ryml::id_type node, double value) const;

    // --- cases & the variants the scenario/case is for ---

    struct CaseDef
    {
        std::string name;
        std::size_t line = 0;
        std::vector<config::ParamValue> params{};
        std::optional<std::vector<std::string>> only{};
        std::size_t only_line = 0;
    };

    void read_cases(ryml::id_type node);
    const CaseDef* resolve_case_selection(ryml::id_type casesNode, bool hasTopLevelErrors, bool& skipReading);
    std::optional<std::vector<std::string>> read_only_list(ryml::id_type node, const std::string& prefix);
    void resolve_only_and_case(ryml::id_type onlyNode, const CaseDef* chosenCase);

    /// A whole number of milliseconds from `least` to kMaxMillis, or empty (and an error).
    std::optional<std::int64_t> millis(ryml::id_type node, const std::string& key, std::int64_t least, const std::string& meaning);

    // --- variables ---

    const control::VariableInfo* info_of(std::size_t variable) const { return infos_[variable]; }

    /// The index of "<participant>.<name>" in the scenario's variables, or empty (and an error).
    /// Appends absent/left-out variable names to `absent` instead of touching member state.
    std::optional<std::size_t> variable_by_name(std::string_view raw_name, std::size_t line, std::vector<std::string>& absent);
    std::optional<std::size_t> variable(ryml::id_type node, std::vector<std::string>& absent);

    std::string example_variable() const;

    /// The value written at `node` as a value of the variable: for a write (`write`: an integer
    /// variable takes whole numbers only, and the range counts) or for a comparison.
    std::optional<control::Value> value_of(ryml::id_type node, std::size_t variable, const std::string& what, bool write,
                                           StepReadState* state = nullptr);

    /// Whether `value` may be written to the variable: as the run's hub decides for a converted
    /// variable (the seen ends of its range are products), else within min and max.
    bool in_range(double value, std::size_t variable) const;

    static std::string range_text(const control::VariableInfo& info);

    /// Whether the step at `time` (empty: not known before the run) may write the variable.
    bool writable(std::size_t variable, std::size_t line, const std::optional<std::int64_t>& time, const char* over_time = nullptr);

    // --- steps & templates ---

    struct InheritedTiming
    {
        std::optional<std::int64_t> at_ms{};
        std::int64_t after_ms = 0;
    };

    void read_steps(ryml::id_type node, std::optional<InheritedTiming> first_timing = std::nullopt,
                    bool* aborted = nullptr);

    void read_step(ryml::id_type node, std::optional<InheritedTiming> inherited = std::nullopt);

    void read_template_step(const std::map<std::string, ryml::id_type>& nodes, const std::string& step_name,
                            std::optional<InheritedTiming> inherited, bool valid_call_time, std::size_t step_line);

    const CachedTemplate* load_template(const std::string& normalized_rel, std::size_t call_line);

    bool parse_template_parameters(ryml::id_type node, std::vector<ParameterDef>& out);

    bool read_template_call_params(ryml::id_type params_node, const CachedTemplate& tpl, std::size_t call_line,
                                   std::vector<Known>& callee_params, std::vector<SourceParam>& frame_params);

    bool validate_param_value(config::ParamValue& val, ParamType type, std::size_t line, const std::string& label);

    static bool is_yaml12_non_string_literal(std::string_view text);

    static bool param_values_equal(ParamType type, const config::ParamValue& a, const config::ParamValue& b);

    static const char* param_type_name(ParamType type);

    std::string shown_path_for(const std::string& normalized_rel) const;

    static std::optional<std::string> resolve_scoped_path(const std::string& dir_rel, const std::string& raw,
                                                          std::string& normalized_out, bool is_template = false);

    // --- what the steps put on the inputs, as the text says it ---

    /// A value the text puts on a written variable (a set's value, an end of a ramp, a level or
    /// an end of a wave), of the step being read: what a dither on the variable may be added to.
    /// `stays`: the step leaves the input there.
    void note_base(StepReadState& state, std::size_t variable, double value, std::size_t line, bool stays);

    /// A step that runs: what it puts on the inputs, for the list of the lines and the findings
    /// about the dithers (a skipped step puts nothing).
    void note_lines(const ScenarioStep& step, const StepReadState& state, const std::optional<std::int64_t>& time);

    /// A number as it is written to the variable: an integer rounded as the run rounds it.
    double as_written(double value, std::size_t variable) const;
    /// Whether `value` with `reach` on both sides, as it is written, stays within the variable's range.
    bool reaches_outside(double value, double reach, std::size_t variable) const;

    /// Once the steps are read: the dithers that may leave the range of their input, those that
    /// an integer rounds away, and those that nothing is ever under.
    void check_dithers();

    /// The step uses variables that their participants do not have (`state.absent`): it does nothing in
    /// the run, which says so; a note says it before.
    void skip(ScenarioStep& step, const StepReadState& state);

    /// Why the variables of `absent` cannot be used: "Body.boost is absent: participant 'Body' does
    /// not have it in this configuration (absent: true in its variables)".
    std::string skip_reason(const std::vector<std::string>& absent) const;

    void read_set(ryml::id_type node, ScenarioStep& step, StepReadState& state, const std::optional<std::int64_t>& time);

    /// The over_ms of the ramp of a variable that is absent (`node`: its entry in "ramp"), or 0 if
    /// it has none that can be read. Only this is read of it: it is a time, which needs no type;
    /// "to" and "from" need the variable's.
    std::int64_t skipped_over_ms(ryml::id_type node);

    /// @returns the longest over_ms, also of the variables that are absent.
    std::int64_t read_ramp(ryml::id_type node, ScenarioStep& step, StepReadState& state, const std::optional<std::int64_t>& time);

    // --- wave ---

    using Parts = std::map<std::string, ryml::id_type>;

    static std::string span_text(SimDuration d);

    /// With one decimal at most: "33.3".
    static std::string rounded_text(double value) { return number_text(std::round(value * 10.0) / 10.0); }

    /// The entries of the mapping `node` by key; a key twice or not in `allowed` is reported and
    /// left out. Empty (and an error) if `node` is not a mapping: `must` says what it must be.
    std::optional<Parts> parts_of(ryml::id_type node, const std::string& what, const std::vector<std::string>& allowed,
                                  const std::string& must);

    /// A number that is not a value of the variable (an amplitude, a frequency), from `least`
    /// (not `least` itself if `above`) to `most`; `meaning` says what it is.
    std::optional<double> measure(ryml::id_type node, const std::string& label, double least, bool above, double most,
                                  const std::string& meaning);

    std::optional<bool> flag(ryml::id_type node, const std::string& label, const std::string& meaning);

    std::optional<std::string> choice(ryml::id_type node, const std::string& label, const std::vector<std::string>& options);

    std::optional<std::uint64_t> seed_of(ryml::id_type node, const std::string& label);

    /// A value of the variable that a wave writes as it is (`key` of `what`): for a bool true or
    /// false, for an enum an item, else a number of the range (also for an integer: the wave
    /// rounds what it writes).
    std::optional<double> level(ryml::id_type node, std::size_t variable, const std::string& key, const std::string& what,
                                StepReadState* state = nullptr);

    /// period_ms or frequency_hz, and phase_deg, of a shape that repeats. `text`: as the step's
    /// text shows them.
    std::optional<detail::Periodic> periodic_of(const Parts& parts, const std::string& what, std::size_t line, std::string& text);

    /// The step of the variable's ECU, to compare a wave with. Empty if the step's waves are not
    /// compared (undersampled: ok) or the step is not known (a note says so).
    std::optional<SimDuration> sampling(std::size_t variable, const std::string& what, std::size_t line, bool quiet);

    std::string takes_inputs(std::size_t variable) const;
    std::string step_of(std::size_t variable) const;
    SimDuration step_ns(std::size_t variable) const;
    /// For a key in whole milliseconds: the first that is not shorter than `d`.
    static std::string whole_ms(SimDuration d) { return std::to_string((d.count() + 999'999) / 1'000'000); }
    const std::string& owner_of(std::size_t variable) const;

    /// A shape that repeats every `period`: too fast for its ECU (a warning), or, if it is smooth
    /// (its values between the corners matter; `factor` is then its value, -1 to 1, at a time
    /// since the step began), all the same at the times the ECU takes it (a warning) or coarse (a
    /// note). @returns whether it warned.
    ///
    /// The wave writes at T, T + step, T + 2 step, ... (T: when the scenario's step begins), and
    /// the ECU applies each in a step of its own, so what the ECU sees does not depend on T.
    bool check_period(std::size_t variable, const std::string& what, std::size_t line, bool quiet, SimDuration period,
                      const std::function<double(SimDuration)>& factor = {});

    /// A square whose edges do not fall on the steps of its ECU: the duty the ECU sees.
    void check_square(std::size_t variable, const std::string& what, std::size_t line, bool quiet, const detail::Periodic& periodic,
                      double duty);

    /// Something of a wave that lasts `span`, shorter than the step of its ECU: `is` says what it
    /// is and how it compares ("is 3 ms wide, shorter than"), `sees` what the ECU sees of it,
    /// `make` what to change.
    void check_span(std::size_t variable, const std::string& what, std::size_t line, bool quiet, SimDuration span, const std::string& is,
                    const std::string& sees, const std::string& make);

    enum class Seen
    {
        Yes,
        No,
        Depends, ///< on when the scenario's step begins, which is not known before the run
    };

    /// Whether the ECU gets a value that a wave has from `from` until `to` (times since the
    /// scenario's step began; `to` is not part of it) and no longer; the wave ends at `length`.
    /// `time`: when the scenario's step begins (ms), if that is known before the run.
    ///
    /// The wave writes at T, T + step, T + 2 step, ... and at T + length (what it leaves), and the
    /// ECU applies each write in its first step at or after the write's time: its steps are at 0,
    /// step, 2 step, .... A value that none of the regular writes carries is never applied. Two
    /// regular writes go to two steps of the ECU; the last regular write and the one of the end
    /// go to the same step, where the end's wins, if no step of the ECU is from the one until the
    /// other. `written`, `ends`: those two times, when that decides (Yes or No with `time`; as
    /// times since the step began for Depends).
    static Seen seen_between(SimDuration step, const std::optional<std::int64_t>& time, SimDuration from, SimDuration to, SimDuration length,
                             SimDuration& written, SimDuration& ends);

    /// A pulse that is shorter than the step of its ECU: whether the ECU sees it, as far as that
    /// is known before the run.
    void check_pulse(std::size_t variable, const std::string& what, std::size_t line, bool quiet, SimDuration width,
                     const std::optional<std::int64_t>& time);

    /// How long the wave of a variable that is absent takes (`node`: its entry in "wave"), as far
    /// as its text says it without the variable's type: for_ms, a pulse's width_ms, the last time
    /// of points, the bounces of a chatter. 0 if that cannot be read.
    std::int64_t skipped_wave_ms(ryml::id_type node);

    /// @returns the longest length, also of the variables that are absent.
    std::int64_t read_wave(ryml::id_type node, ScenarioStep& step, StepReadState& state, const std::optional<std::int64_t>& time, bool quiet);

    // --- dithers ---

    /// The file's "inputs": per input what is on top of it from the start (its dither).
    void read_inputs(ryml::id_type node);

    /// The step "dither": per variable none, or the shape that is on top of it from the step's time.
    void read_dither(ryml::id_type node, ScenarioStep& step, StepReadState& state, bool quiet);

    /// The dither of the variable `v` (`node`: a shape, or in a step none), or empty (and its errors).
    /// `shown`: the line it is known by (ScenarioOverlay::line): the step's, or its entry's in "inputs".
    std::optional<ScenarioOverlay> read_overlay(ryml::id_type node, std::size_t v, bool quiet, bool in_step, std::size_t shown,
                                                std::vector<Dither>* step_dithers = nullptr);

    /// What makes a wave's shape once everything is read: given the wave's length.
    using MakeShape = std::function<std::shared_ptr<const Waveform>(SimDuration)>;

    /// The body of a shape other than points (`body`: the value of its key, `shape`), for the
    /// variable `v`: its keys, with their errors and what the ECU's step makes of it (warnings).
    /// What a wave and a dither have in common; what stands beside the shape (for_ms, around) is
    /// the caller's. `top` is where the shape's key stands (a chatter looks for for_ms there).
    /// Fills in what it found: `text` (appended), `make`, `swing` (how far from the middle it
    /// goes at most, if that is known), `length` (of a shape that says it itself), `wantsFor`,
    /// `noAround` (why around does not go with it), and of `out` the base, the hint and what the
    /// text has (ScenarioMotion::described). `good` turns false with an error.
    /// @returns false if the body is not a mapping of the shape's keys (nothing else was read).
    bool read_shape(const std::string& shape, ryml::id_type body, const Parts& top_parts, std::size_t v, const std::string& what,
                    std::size_t line, const std::optional<std::int64_t>& time, bool quiet, bool& good, ScenarioMotion& out,
                    std::string& text, std::optional<std::int64_t>& length, bool& wantsFor, std::string& noAround,
                    std::optional<double>& swing, MakeShape& make, StepReadState* state = nullptr);

    /// The wave of one variable (`node`: its entry in "wave"), or empty (and its errors).
    std::optional<ScenarioMotion> read_wave_of(ryml::id_type node, std::size_t v, StepReadState& state,
                                               const std::optional<std::int64_t>& time, bool quiet);

    /// A chirp whose highest frequency its ECU cannot see (a warning), or sees coarsely (a note).
    void check_chirp(std::size_t variable, const std::string& what, std::size_t line, bool quiet, double top_hz);

    /// The points of a wave (`body`: the list; `interpolate`: its node or NONE). @returns whether
    /// they have no error; then `length` and `make` are set.
    bool read_points(ryml::id_type body, ryml::id_type interpolate, std::size_t v, const std::string& what,
                     const std::optional<std::int64_t>& time, bool quiet, ScenarioMotion& out, std::string& text, std::optional<std::int64_t>& length,
                     std::function<std::shared_ptr<const Waveform>(SimDuration)>& make, StepReadState& state);

    // --- points from a CSV file ---

    const CsvFile& csv(const std::string& path);

    /// A time cell in milliseconds (`seconds`: the cell is in seconds): empty if it is not a
    /// number (`ms` is then NaN), not a whole number of milliseconds, or not from 0 to kMaxMillis.
    static std::optional<std::int64_t> cell_ms(const std::string& cell, bool seconds, double& ms);

    /// The value of a cell for the variable, as a wave writes it (level()): for a bool true, false,
    /// 1 or 0; for an enum an item by name or by value; else a number of the range (also hexadecimal
    /// for an integer). Empty and an error at `line` if it is not one. `where`: "line 7 of f.csv
    /// (column throttle)".
    std::optional<double> cell_value(const std::string& cell, std::size_t variable, const std::string& where, std::size_t line,
                                     StepReadState& state);

    /// The points of a wave from a CSV file (`body`: { file, value, time, time_unit }), each given
    /// to `add` (read_points()). Stops at the first error. `source`: " from <file> column <name>".
    template <class Add>
    bool file_points(ryml::id_type body, std::size_t v, const std::string& what, const Add& add, std::string& source,
                     ScenarioShapeText& described, StepReadState& state);

    /// Points that are kept until the next (interpolate: hold): a jump is what they are for, so
    /// how close two of them are does not matter; one whose value the ECU never gets does. Says
    /// the first such point, or else the first of which that is not known before the run.
    void check_held_points(std::size_t variable, const std::string& what, std::size_t line, bool quiet,
                           const std::vector<signal::Point>& points, const std::vector<std::string>& names,
                           const std::optional<std::int64_t>& time);

    void read_conditions(ryml::id_type node, ScenarioStep& step, StepReadState& state, const std::string& action);

    bool read_rhs_variable(ryml::id_type var_node, std::size_t lhs_variable, const std::string& what,
                           const std::string& prefix, StepReadState& state, std::optional<std::size_t>& rhs_out,
                           std::string& rhs_text_out);

    bool read_variable_ref_map(ryml::id_type map_node, std::size_t lhs_variable, Compare op, const std::string& lhs_name,
                               const std::string& key, StepReadState& state, std::optional<std::size_t>& rhs_out,
                               std::string& rhs_text_out);

    static std::optional<Compare> compare_of(const std::string& key);

    static std::string condition_text(const ScenarioVariable& var, const ScenarioCondition& condition,
                                      const std::vector<std::string>& rhs_texts);

    std::string step_text(const ScenarioStep& step, const StepReadState& state) const;

    struct EvalScope
    {
        const config::detail::YamlDocument* doc = nullptr;
        std::string file{};
        std::string dir_rel{};
        std::vector<Known> params{};                         ///< every parameter that has a value
        std::map<ryml::id_type, std::size_t> substituted{};  ///< a value node that is "${name}": its index in params
        std::map<ryml::id_type, std::string> interpolated{}; ///< a scalar node with "${name}" interpolation resolved
        std::set<std::string> undefined{};                   ///< the parameters without a value, said once
    };

    EvalScope& scope() noexcept { return scopes_.back(); }
    const EvalScope& scope() const noexcept { return scopes_.back(); }

    const ScenarioCatalog& catalog_;
    const ScenarioContext& context_;
    ScenarioRead& out_;
    bool check_catalog_ = true;
    SharedReadCache owned_cache_{};
    SharedReadCache& cache_;
    std::string scenario_dir_prefix_{};
    std::vector<EvalScope> scopes_;
    std::vector<SourceFrame> active_templates_;
    std::vector<std::string> active_rel_paths_;
    std::map<std::string, ParamConstraint> param_constraints_;
    std::vector<std::string> param_constraint_order_;
    std::set<std::string> cli_string_params_{};
    std::size_t expanded_steps_ = 0;
    bool too_many_expanded_steps_ = false;
    bool caller_template_params_incomplete_ = false;
    std::vector<CaseDef> cases_;
    bool cases_written_ = false;
    bool cases_incomplete_ = false;
    Scenario scenario_;
    std::map<std::string, std::string> absent_variables_; ///< "<participant>.<name>" that is absent: its participant
    /// What the steps that run put on the inputs, as the text says it (note_lines()).
    struct Base
    {
        std::size_t variable;
        double value;
        std::string file{};
        std::size_t line;
        bool stays; ///< the input is left there when the step is over (a set's value, a ramp's to, a wave's middle)
    };
    struct Dither
    {
        std::size_t variable;
        std::string file{};
        std::size_t line;
        double reach;                 ///< how far from the value under it
        bool about;                   ///< `reach` is an estimate (a gaussian noise)
        std::optional<double> swing;  ///< its amplitude, if that bounds it
        bool quiet;                   ///< undersampled: ok
        bool off;                     ///< a dither step's none
        const char* as_it_is;         ///< how to run it as it is (kAsItIs, kAsItIsInput)
        std::string shape;
        std::vector<SourceFrame> call_stack{};
    };
    /// Per-step scratch state passed explicitly while reading a single step (isolated from recursive template calls).
    struct StepReadState
    {
        std::vector<std::string> absent{};
        std::set<std::string> condition_bare_absent{};
        std::set<std::string> condition_formatted_absent{};
        bool beside = false;
        bool unchecked = false;
        std::size_t motion_absent = 0;
        std::vector<Base> bases{};
        std::vector<Dither> dithers{};
    };
    /// In the order of the file, of the steps that run: a value put on an input, or a dither
    /// step's dither (an index in dithers_).
    struct Flow
    {
        std::size_t step;                   ///< which step (a count)
        std::optional<Base> value{};
        std::optional<std::size_t> dither{};
    };
    std::vector<Dither> dithers_;      ///< those of "inputs" (the first declared_ of them), then those of the steps that run
    std::size_t declared_ = 0;
    std::vector<Flow> flow_;
    std::set<std::size_t> placed_; ///< the variables a step that runs puts a value on (set, ramp, wave)
    std::vector<ScenarioLineText> lines_;
    std::vector<std::size_t> line_variables_; ///< per line of lines_: its variable
    bool inputs_written_ = false; ///< the file has "inputs"
    /// How the messages say to run a wave or a dither as it is (kAsItIs; kAsItIsInput while "inputs" is read).
    const char* as_it_is_ = kAsItIs;
    std::size_t unchecked_checks_ = 0; ///< of skipped_checks_, those of an expect beside that is not checked
    std::set<std::string> left_out_;   ///< the participants of absent_variables_ that the variant leaves out
    struct CheckRef
    {
        std::string file;
        std::size_t line = 0;
    };
    std::vector<CheckRef> checks_;         ///< the checks (expect, wait_until, an expect beside)
    std::vector<CheckRef> skipped_checks_; ///< those of them that are skipped
    std::vector<const control::VariableInfo*> infos_; ///< of scenario_.variables, in the catalog
    std::vector<const control::MappedSchema*> maps_;  ///< of scenario_.variables: its participant's map, or nullptr
    std::map<std::string, std::size_t> variable_index_;
    std::map<std::string, std::size_t> bad_variables_;
    /// The end of the step before, in milliseconds, if it is known before the run.
    bool end_known_ = true;
    std::int64_t end_ms_ = 0;
    /// Earliest possible end across all steps regardless of `end_known_` (wait steps contribute
    /// minimum length 0; subsequent `after_ms` and timed steps accumulate over it).
    std::int64_t min_end_ms_ = 0;
    std::string wait_file_{};
    std::size_t wait_line_ = 0; ///< the first step that waits since the end was known
    /// The latest the step before can end, in milliseconds (a wait counted with its whole time).
    std::int64_t latest_ms_ = 0;
    bool too_long_ = false; ///< said once
};

} // namespace vehicle_sil::scenario::reader
