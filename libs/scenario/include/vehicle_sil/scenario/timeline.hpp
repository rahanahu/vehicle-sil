// SPDX-License-Identifier: MIT
//
// The timeline of a scenario run (run --scenario --timeline <file>): what the scenario's engine
// (scenario_run.hpp) wrote and saw, kept while it runs (TimelineRecord). The engine fills it and
// knows nothing of the file made of it afterwards (timeline_format.hpp, which reads the record
// and the engine's result). Internal; what a user sees of it: docs/simulation-run.md,
// schema/timeline.schema.json.
#pragma once

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/scenario/scenario.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::scenario {

/// How many points a timeline keeps. Beyond a limit the older points go and the newer ones stay
/// (the end of the run, where a failure is, keeps every point). Counted are the points that can
/// go: of a series of written values those of a ramp or a wave (a set is always kept), of a
/// series of samples all.
struct TimelineLimits
{
    std::size_t points_per_series = 100'000;
    std::size_t points_total = 1'000'000;
    std::size_t events_per_participant = 2'000;
    std::size_t events_total = 5'000;
};

/// How a step looks at its conditions.
enum class WatchMode
{
    At,     ///< an expect at one time: the values at that time
    Always, ///< an expect with hold_for_ms: at its time and at every sample until its end
    Once,   ///< an expect with within_ms, a wait_until: at its time or at some sample until its end
    During, ///< the expect beside a ramp or a wave: at every sample of a time from its time to its end
};
/// "at", "always", "once", "during".
[[nodiscard]] const char* watch_mode_name(WatchMode mode);

/// A right-hand variable value of a condition that a failed step did not meet.
struct TimelineComparedValue
{
    std::size_t variable = 0;              ///< index in Scenario::variables
    std::optional<control::Value> value{}; ///< what the variable was; empty: no value (no sample)
    std::optional<SimTime> sample{};       ///< the time of that sample
};

/// A condition a failed step did not meet.
struct TimelineFailedCondition
{
    std::size_t condition = 0;                     ///< index in ScenarioStep::conditions
    std::optional<control::Value> value{};         ///< what the variable was; empty: no value (no sample)
    std::optional<SimTime> sample{};               ///< the time of that sample
    std::vector<TimelineComparedValue> compared{}; ///< right-hand variables of this condition (empty when none)
};

/// A write of the request a step failed of.
struct TimelineFailedWrite
{
    std::size_t variable = 0; ///< index in Scenario::variables
    SimTime at{};
    std::optional<control::Value> value{}; ///< empty: not known (it was never worked out)
    /// What TimelineRecord::written() gave for it: which point of the variable's series it is.
    /// Empty: nothing was sent.
    std::optional<std::uint64_t> sent{};
    /// Whether the ECU applied it: true, false (also: not sent), or empty if that is not known
    /// (no result came, or the result does not say which writes of its command took effect).
    std::optional<bool> applied{false};
};

/// What a failed step failed of, as numbers (its message says the same for people).
struct TimelineFailure
{
    std::vector<TimelineFailedCondition> conditions{};
    /// A step that failed of a write: every write of the request that failed (what was sent
    /// together for one time, for all its participants), in the order sent. An ECU takes its
    /// part of them one after the other and stops at the one it rejects, so the writes before
    /// that one are applied. A step that could not send: the one write it could not make.
    std::vector<TimelineFailedWrite> writes{};
    /// Index in `writes` of the write that is the reason, if that is known.
    std::optional<std::size_t> write{};
};

/// What a scenario's engine wrote and saw, per variable of the scenario. The engine fills it on
/// the thread that runs it, and closes it when the scenario is over; then it only is read. Not
/// thread safe.
///
/// Besides the series: the lines of the inputs (Line), the watch of each step and what a failed
/// step failed of.
///
/// Two kinds of series. Written: every value the engine sent for a variable, in the order sent
/// (sent: also the writes that were not applied, which failed() marks).
/// Sampled: the values of a variable that a condition reads, of every sample the engine looked at;
/// kept are the samples at which the value changed, and how many samples there were.
class TimelineRecord
{
public:
    struct Written
    {
        SimTime at{};
        control::Value value{};
        bool motion = false; ///< of a ramp or a wave (may go at a limit); else of a set (is kept)
        /// A write of the request a step failed of that is not known to be applied (failed()):
        /// sent, and not taken, not applied, or without a result.
        bool refused = false;
        std::uint64_t serial = 0; ///< what written() gave for it (points may go at a limit: no index)
        /// What the value was made of before a dither was added and the variable's type applied:
        /// the number of a set's value, a motion's value then. Shown for a series that a dither
        /// was on (Series::dithered).
        double base = 0.0;
    };
    /// Samples of one value, one after the other. A sample with the value of the one before it
    /// that does not come at the same distance begins a new one (`repeats`), so that the samples
    /// up to any time can be counted.
    struct Sampled
    {
        SimTime first{};
        SimTime last{};
        SimDuration interval{}; ///< between its samples (0 with one)
        std::uint64_t count = 0;
        control::Value value{};
        bool repeats = false; ///< the value is that of the samples before it: no point of its own
    };
    struct Series
    {
        std::size_t variable = 0; ///< index in Scenario::variables
        bool sampled = false;
        std::vector<Written> written{};
        std::vector<Sampled> samples{};
        /// Written: the scenario puts a dither on the variable (its "inputs", or a dither step
        /// that runs in this configuration, with a shape): every point of the series is then
        /// shown with its base. Known from the scenario alone, whatever a run got to: the form of
        /// the file does not depend on the controller's step.
        bool dithered = false;
        /// How many of its points can go at a limit: of `written` those of a ramp or a wave, of
        /// `samples` all.
        std::size_t droppable = 0;
        /// Sampled: how many samples there were (also those of points that went at a limit), and
        /// the first and the last of them.
        std::uint64_t sample_count = 0;
        std::optional<SimTime> first_sample{};
        std::optional<SimTime> last_sample{};
        /// How many points went at a limit, the time of the oldest that went and the time of the
        /// oldest that stayed (of `written`: of the points of a ramp or a wave; a set stays).
        std::uint64_t dropped = 0;
        std::optional<SimTime> dropped_from{};
        std::optional<SimTime> dropped_before{};
    };
    struct Watch
    {
        WatchMode mode = WatchMode::At;
        SimTime from{};
        std::optional<SimTime> until{};
    };

    /// What a step put on the line of a variable when its time came: a set its value, a ramp or
    /// a wave its motion (ScenarioEngine, "Lines"). In the order put: by time, then step, then the
    /// order within the step.
    struct Line
    {
        std::size_t step = 0;     ///< index in Scenario::steps; kNoStep: of none (a dither that "inputs" declares)
        /// Index in that step's writes (a set), motions (a ramp, a wave) or overlays (a dither);
        /// of a dither of no step in Scenario::inputs.
        std::size_t part = 0;
        std::size_t variable = 0; ///< index in Scenario::variables
        SimTime from{};
        /// Of a set its value. Of a motion what it starts from or swings around: the number the
        /// step names, or the value the line or the sample had; empty if its shape uses none, or
        /// none was known (the step then failed).
        std::optional<control::Value> base{};
        /// Where a motion's base is from: "written" (the step names it; also a set's value),
        /// "line", "sample"; null without a base.
        const char* base_source = nullptr;
        /// A write of it went out (written() with its step, for its time). The engine puts what
        /// the steps of one time do on the lines before it sends the first write of that time,
        /// and a failure there ends the scenario: a line that sent nothing did not happen, and
        /// close() drops it.
        bool sent = false;
        /// A dither (dither()): no base. `from` is the time of the first write that carried it,
        /// and the next dither line of the variable is where it ends.
        bool dither = false;
    };

    /// A virtual-timestamped structured ECU control event (control::Agent::log / Ecu::log).
    struct Event
    {
        SimTime at{};
        std::string participant{};
        control::EventLevel level = control::EventLevel::Info;
        std::string text{};
        std::uint64_t seq = 0; ///< monotonic insertion sequence for deterministic tie-breaking
    };
    /// Per-participant accounting of dropped ECU control events.
    struct EventDrop
    {
        std::string participant{};
        std::uint64_t dropped = 0;               ///< events evicted by timeline limits for this participant
        std::optional<SimTime> dropped_from{};   ///< virtual time of oldest evicted event (null if dropped == 0)
        std::optional<SimTime> dropped_before{}; ///< virtual time of oldest kept event (null if none kept or dropped == 0)
        std::uint64_t agent_dropped = 0;         ///< events dropped by the ECU's Agent before reaching the Hub
    };

    /// The `step` of a write that is no step's (a dither alone moved the input).
    static constexpr std::size_t kNoStep = static_cast<std::size_t>(-1);

    explicit TimelineRecord(const Scenario& scenario, TimelineLimits limits = {},
                            std::span<const RecordedVariable> extra_recorded = {});
    /// Manual-run record (no scenario): creates sampled series only for `recorded` (deduplicated
    /// in first-seen order), with zero written series, steps, or lines.
    explicit TimelineRecord(std::span<const RecordedVariable> recorded, TimelineLimits limits = {});

    // --- the engine's side ---
    /// A write that is sent, of `step` (index in Scenario::steps; kNoStep: of none): a point of the variable's
    /// series, and the line that step put at `at` on the variable has happened (Line::sent).
    /// `base`: Written::base. The dither that was put last on the variable (dither()) begins
    /// with this write, if it has not yet.
    /// Returns what names that point from then on (TimelineFailedWrite::sent); empty if the
    /// record keeps none.
    std::optional<std::uint64_t> written(std::size_t step, std::size_t variable, SimTime at, const control::Value& value, bool motion,
                                         double base = 0.0);
    /// A sample of `variable` (kept if a condition reads the variable).
    void sampled(std::size_t variable, SimTime at, const control::Value& value);
    void line(Line line);
    /// A dither was put on the line of a variable (or taken off: the step's "none"), in place of
    /// the one before it. It is a line (Line::dither) from the first write of the variable that
    /// goes out while it is there; one that is replaced before that never was.
    void dither(Line line);
    void watch(std::size_t step, Watch watch);
    /// The step failed. Marks the points of `failure.writes` that are not known to be applied.
    void failed(std::size_t step, TimelineFailure failure);
    /// Enables opt-in bounded recording of virtual-timestamped structured ECU events.
    void enable_events() noexcept { events_enabled_ = true; }
    /// Records one structured ECU control event (no-op unless enable_events() was called and record is open).
    void event(std::string participant, SimTime at, control::EventLevel level, std::string text);
    /// Notes events dropped inside an ECU's Agent before reaching the Hub.
    void note_agent_events_dropped(std::string participant, std::uint64_t count);
    /// The index in the written series of `variable` of the point that written() named
    /// `serial`; empty if it went at a limit (or there is none).
    [[nodiscard]] std::optional<std::size_t> point_of(std::size_t variable, std::uint64_t serial) const;
    /// The run is over: nothing after `to` is kept, and the limits are applied for good.
    /// `std::nullopt` closes the record with no recorded horizon (e.g. a manual run with zero samples).
    void close(std::optional<SimTime> to);

    // --- the reader's side ---
    [[nodiscard]] const TimelineLimits& limits() const noexcept { return limits_; }
    /// In the order of Scenario::variables; of one variable the written series first.
    [[nodiscard]] const std::vector<Series>& series() const noexcept { return series_; }
    /// Per step of the scenario; empty for a step without conditions, or one that was not reached.
    [[nodiscard]] const std::vector<std::optional<Watch>>& watches() const noexcept { return watches_; }
    [[nodiscard]] const std::vector<std::optional<TimelineFailure>>& failures() const noexcept { return failures_; }
    /// Once closed: those put at or before the time it was closed at of which a write was sent.
    [[nodiscard]] const std::vector<Line>& lines() const noexcept { return lines_; }
    /// The time close() was given; empty while the scenario runs.
    [[nodiscard]] const std::optional<SimTime>& recorded_to() const noexcept { return recorded_to_; }
    /// How many points went at a limit, of all series.
    [[nodiscard]] std::uint64_t dropped() const noexcept;
    /// Whether opt-in ECU event recording was enabled on this record.
    [[nodiscard]] bool events_enabled() const noexcept { return events_enabled_; }
    /// Once closed: virtual-timestamped ECU events up to recorded_to(), ordered by (at, participant, seq).
    [[nodiscard]] const std::vector<Event>& events() const noexcept { return events_; }
    /// Once closed: per-participant dropped event accounting (only participants with dropped > 0 or agent_dropped > 0).
    [[nodiscard]] const std::vector<EventDrop>& events_dropped() const noexcept { return events_dropped_; }
    /// Total ECU events lost (evicted at timeline limits plus agent-side drops).
    [[nodiscard]] std::uint64_t dropped_events() const noexcept;
    /// True if any series points or ECU events were dropped.
    [[nodiscard]] bool dropped_any() const noexcept { return dropped() > 0 || dropped_events() > 0; }
    /// Whether a step of the scenario writes the variable, whether a condition reads it, and
    /// whether it was explicitly requested via RunIntent::record.
    [[nodiscard]] bool is_written(std::size_t variable) const { return variable < written_.size() && written_[variable].has_value(); }
    [[nodiscard]] bool is_watched(std::size_t variable) const { return variable < watched_.size() && watched_[variable]; }
    [[nodiscard]] bool is_recorded(std::size_t variable) const { return variable < recorded_.size() && recorded_[variable]; }
    /// Extra recorded variables not already present in Scenario::variables, in first-seen order.
    /// Their timeline variable indices are Scenario::variables.size() + k.
    [[nodiscard]] const std::vector<RecordedVariable>& extra_variables() const noexcept { return extra_variables_; }
    /// Total number of variables in the record (Scenario::variables.size() + extra_variables().size()).
    [[nodiscard]] std::size_t variable_count() const noexcept { return sampled_.size(); }

private:
    struct ParticipantEvents
    {
        std::string participant{};
        std::deque<Event> events{};
        std::uint64_t dropped = 0;
        std::optional<SimTime> dropped_from{};
        std::optional<SimTime> dropped_before{};
        std::uint64_t agent_dropped = 0;
    };

    static void drop_oldest(Series& series, std::size_t keep);
    void enforce(bool final);
    ParticipantEvents& participant_events_for(std::string_view participant);
    static void drop_oldest_events(ParticipantEvents& pe, std::size_t keep);
    void enforce_events(bool final);

    TimelineLimits limits_;
    std::vector<RecordedVariable> extra_variables_{};
    std::vector<Series> series_;
    std::vector<std::optional<std::size_t>> written_; ///< per variable: its written series
    std::vector<std::optional<std::size_t>> sampled_; ///< per variable: its sampled series
    std::vector<bool> watched_;                       ///< per variable: read by a scenario condition
    std::vector<bool> recorded_;                      ///< per variable: requested via RunIntent::record
    std::vector<std::optional<Watch>> watches_;
    std::vector<std::optional<TimelineFailure>> failures_;
    std::vector<Line> lines_;
    std::vector<std::optional<Line>> waiting_dither_; ///< per variable: put, and no write since
    std::optional<SimTime> recorded_to_{};
    bool closed_ = false;
    std::size_t total_ = 0; ///< the points that can go, of all series
    std::uint64_t next_serial_ = 0;
    bool events_enabled_ = false;
    std::vector<ParticipantEvents> participant_events_{};
    std::size_t events_total_ = 0;
    std::uint64_t next_event_seq_ = 0;
    std::vector<Event> events_{};
    std::vector<EventDrop> events_dropped_{};
};

} // namespace vehicle_sil::scenario
