// SPDX-License-Identifier: MIT
//
// Running a scenario (scenario.hpp): the machine that decides, from virtual times alone, when each
// write goes out and what each step's result is. It has no thread, no clock and no SIL Kit: the
// run (run_scenario.cpp) calls it at every step of the launcher's controller, and tests call it
// with a model of the ECUs. Internal; what a user sees of it: docs/scenario.md.
#pragma once

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/launch/scenario.hpp"
#include "vehicle_sil/launch/timeline.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil::launch {

enum class StepStatus
{
    NotRun,     ///< the scenario was over before its time
    Running,    ///< begun and not decided yet (only while the scenario runs)
    Passed,
    Failed,
    Unfinished, ///< begun, and the scenario was over before it was decided
    Skipped,    ///< not for this configuration (ScenarioStep::skipped): known before the run
};
/// "not_run", "running", "passed", "failed", "unfinished", "skipped".
[[nodiscard]] const char* step_status_name(StepStatus status);

/// What became of one step.
struct ScenarioStepRun
{
    std::size_t index = 0; ///< from 1, in the order of the file
    std::size_t line = 0;
    StepKind kind = StepKind::Set;
    std::string text; ///< ScenarioStep::text
    StepStatus status = StepStatus::NotRun;
    /// Its time (at_ms, or after_ms from the end of the step before it), once that is known.
    std::optional<SimTime> start{};
    /// The virtual time that decided it. Passed: the start of the ECU step that applied a set (the
    /// latest of them), the end of a ramp or a wave, the time of an expect,
    /// the sample at which a wait_until or an expect with within_ms became true, the end of a
    /// hold_for_ms. Failed: the sample that broke a hold, the time a wait ran out, the time of the
    /// write that failed; for a failure before anything happened, its start.
    /// Skipped: none (nothing happened at a time of the simulation).
    std::optional<SimTime> at{};
    /// From where the next step's after_ms counts (Passed, and a step whose end is known before).
    std::optional<SimTime> end{};
    /// Failed: the code of its finding ("scenario-failed", "scenario-timeout", "scenario-write",
    /// "scenario-out-of-time") and why, in one line with the times and the values. Skipped:
    /// "scenario-skipped" and why (ScenarioStep::skipped).
    std::string code{};
    std::string message{};
    /// A ramp or a wave whose expect is not checked in this configuration: why
    /// (ScenarioStep::expect_skipped), or "". Known before the run, whatever its status.
    std::string expect_skipped{};
};

enum class ScenarioStatus
{
    NotRun,     ///< the simulation did not start
    Passed,     ///< every step passed or was skipped (ScenarioRun::skipped() says how many)
    Failed,     ///< a step failed (the first one in virtual time; ScenarioRun::steps says which)
    Incomplete, ///< the simulation ended for another reason before the last step was decided
};
/// "not_run", "passed", "failed", "incomplete".
[[nodiscard]] const char* scenario_status_name(ScenarioStatus status);

struct ScenarioRun
{
    std::string file; ///< as the user named it
    std::string name;
    ScenarioStatus status = ScenarioStatus::NotRun;
    std::vector<ScenarioStepRun> steps{};
    /// Passed and Failed: the step of the launcher's controller at which the scenario was over
    /// (the simulation stands there until it is stopped).
    std::optional<SimTime> over_at{};

    /// How many steps are skipped: a scenario that passed with some did not look at everything.
    [[nodiscard]] std::size_t skipped() const;
    /// How many expects beside a ramp or a wave were not checked (ScenarioStepRun::expect_skipped):
    /// their steps ran, and are not counted in skipped().
    [[nodiscard]] std::size_t unchecked_expects() const;
};

/// The run of `scenario` before anything happened: every step NotRun, or Skipped with its reason
/// (`file` is the caller's to set).
[[nodiscard]] ScenarioRun scenario_not_run(const Scenario& scenario);

/// What the engine writes through (the run: the hub; a test: a model).
class ScenarioPort
{
public:
    virtual ~ScenarioPort() = default;
    /// Hands the writes to their agents, to take effect in each agent's first step at or after
    /// `at`, and returns when every agent has queued or refused its part (or the run is ending).
    /// @returns a ticket for result().
    virtual std::uint64_t write(std::vector<control::PeerWrite> writes, SimTime at) = 0;
    /// Where the writes of `ticket` stand now; empty if that is not known any more.
    virtual std::optional<control::OperationResult> result(std::uint64_t ticket) = 0;
};

/// A scenario participant as the run found it when the simulation started.
struct EnginePeer
{
    /// Its agent is connected and takes commands.
    bool connected = false;
    /// The variables its agent offers, in the order of its samples' values.
    std::vector<control::VariableInfo> schema{};
};

/// Runs one scenario. Not thread safe: one caller at a time.
///
/// The contract with the caller (the launcher's controller steps every `tick`, at 0, tick, 2 tick,
/// ...; "a step at P" is one of those):
/// - begin(): once, while every participant waits before its first step (the start window). The
///   engine writes what the scenario sets at 0 there.
/// - sample(): every sample of a scenario participant, in the order its agent took them, at any
///   time before the tick() that may use it: a sample of a step that starts before P before
///   tick(P). Samples that arrive early (of steps at or after P) are kept and not looked at until
///   their turn, so the result does not depend on when a sample arrives.
/// - tick(P): at every step P of the controller in turn, while the simulation stands at P: every
///   participant has run or is running its steps that start at or before P and runs none after
///   P, until tick() has returned. The engine then
///   1. judges the writes whose result must be there (below),
///   2. looks at the samples of steps before P in the order of their times (ties: in the order of
///      Scenario::participants) and decides the expectations and waits whose time has come,
///   3. hands out the writes for the times in (P, P + tick] (ScenarioPort::write()), which are
///      therefore queued in their agents before any step at those times: no write is late.
///   It returns true once the scenario is over (passed or failed): the caller stops the
///   simulation at P.
/// - stopped_at(P): instead of tick(P) when the simulation stopped by itself at the step P
///   (stop_at_ms): what can be decided from the samples before P is decided; then the first step
///   that is not is failed ("scenario-out-of-time").
/// - cut_short(): the simulation ended otherwise (a stop request, a failed participant, a
///   timeout): the scenario is Incomplete.
///
/// Times. A step's time T is at_ms, or after_ms after the end of the step before it. The end of
/// a set and of an expect at one time is T; of a ramp T + over_ms; of a wave T + its longest
/// length (ScenarioSignal::length_ms); of an expect with hold_for_ms T + hold_for_ms: all known when the step before ends, so such steps keep their times whatever
/// happens. A wait_until, or an expect with within_ms, that becomes true at the sample of time t
/// is known to the engine at the first step P of the controller after t, and ends at P + tick:
/// the first time at which a write of the next step can still take effect.
///
/// Values. "The values at time T" are, per variable, those of its ECU's latest sample of a step
/// that starts at or before T. An expect at T compares them. A wait_until or an expect with
/// within_ms from T is true at the first time in [T, T + within] at which they compare true: at T
/// itself, or at a later sample. An expect with hold_for_ms from T passes if they compare true at
/// T and at every sample in (T, T + hold_for]. A variable without a sample yet compares false.
///
/// Writes. A set at T is one command per participant with the time T: it takes effect in that
/// ECU's first step at or after T. A ramp from T writes, per variable, at T, T + step, T + 2 step,
/// ... (the ECU's step) the value on the line from `from` to `to`, and `to` at T + over_ms; a
/// `from` that is not given is the value the scenario wrote to the variable last, or the one of
/// the latest sample the engine has looked at if that is newer. A wave is the same with another
/// function of the time since T (Waveform; the ramp's line is one): at each of those times the
/// function's value then, worked out when the write goes out, and at the end of its length
/// Waveform::end(); the base it swings around or starts from is found as a ramp's `from`. The
/// values depend on those times alone, not on the controller's tick or on what arrives when. The result of a write is judged
/// at a fixed step of the controller: the first after the ECU's step that applies it (by then it
/// has arrived). Anything but "applied" fails the step ("scenario-write").
///
/// An expect beside a ramp or a wave (ScenarioStep::conditions of those) is a watch from the step's
/// time T to U, the latest step of a written variable's ECU that applies an end value (E, the
/// step's end, if E is on that ECU's steps, else its next step). Per variable it judges every
/// sample of a time in [T, U], whichever ECU sends it; a sample before T (the value before the
/// motion) or after U is not its. So whether T and E fall on the ECU's steps does not change the
/// result. A variable without such a sample fails the step at U (it was not looked at). The
/// step's end stays E (the next step counts from there). The step is decided when both are over,
/// every write judged and the watch closed: it passes at E once both passed, and fails with what the engine learns
/// first, a write ("scenario-write") or a sample ("scenario-failed"). That is the order of tick()
/// (results of earlier writes, samples before P, then writes up to P + tick), not the order of the
/// virtual times: a write for P + tick that its agent refuses at once is said at P, before a
/// sample of an earlier time in (P, P + tick) that breaks the watch. A step that passed stays
/// passed.
///
/// The first failure in this order ends the scenario; steps that had begun are Unfinished, later
/// ones NotRun (their writes for times up to P + tick may have gone out).
///
/// Skipped steps (ScenarioStep::skipped) write and compare nothing. One at T ends at T +
/// ScenarioStep::skipped_ms (the time its text says it takes; 0 for a set and for a step that
/// waits), and the step after it counts its after_ms from there: the times of the steps that run
/// are those of a configuration that skips nothing, except after a skipped step that waits. They
/// are reported (OnStep, once) when their time is known, with `start` and `end` and no `at`: the
/// scenario may be over before the simulation reaches those times.
///
/// The record (a TimelineRecord, for run --timeline; none: nothing of what follows happens, and
/// the engine does exactly what it does without one). The engine notes in it every write it
/// sends (submit_one()), the value of every sample it looks at (observe()), each step's watch
/// and what a failed step failed of. When the scenario is over it closes the record at one time,
/// "recorded to": the `at` of the failed step; of a scenario that passed, the latest `at` of its
/// steps; of one that is Incomplete, the time of the last sample looked at (0 without one).
/// Nothing after that time stays in the record, and the samples up to it that the engine had not
/// looked at yet (they do not decide anything any more) are noted first, if they are of steps
/// before the controller's step being handled (those have all arrived). So what the record has
/// of a scenario that passed, or failed at a sample, does not depend on the controller's tick
/// (while no limit of the record drops points). What may depend on it then is the status of a
/// step that was under way when another step failed, and everything of a run that a write ended
/// (see "fails with what the engine learns first" above).
class ScenarioEngine
{
public:
    /// Called when a step begins (status Running) and when it is decided (Passed, Failed, Skipped).
    using OnStep = std::function<void(const ScenarioStepRun& step)>;

    /// `scenario`, `port` and `record` (may be null) must outlive the engine. `tick` > 0.
    ScenarioEngine(const Scenario& scenario, SimDuration tick, ScenarioPort& port, OnStep on_step = {},
                   TimelineRecord* record = nullptr);

    /// `peers` in the order of Scenario::participants.
    void begin(const std::vector<EnginePeer>& peers);
    void sample(std::size_t participant, std::shared_ptr<const control::Snapshot> sample);
    bool tick(SimTime p);
    void stopped_at(SimTime p);
    void cut_short();
    /// The simulation ended because a participant failed: a scenario that failed (possibly of
    /// that) is Incomplete instead, its failed step Unfinished. Passed stays.
    void superseded();

    [[nodiscard]] bool over() const noexcept { return over_; }
    /// The steps and the status so far (`file` and `name` are the caller's to set).
    [[nodiscard]] const ScenarioRun& run() const noexcept { return run_; }

private:
    struct Variable
    {
        std::optional<std::size_t> index; ///< in its participant's samples
        std::string problem;              ///< why it cannot be used in this run, or ""
        std::optional<control::Value> value; ///< of the latest sample looked at
        std::optional<SimTime> value_time;
        std::optional<control::Value> written; ///< what the scenario wrote last
    };
    struct Watch
    {
        std::size_t step = 0;
        SimTime from{};
        std::optional<SimTime> until{}; ///< empty: decided at `from`
        bool hold = false;              ///< until: true all the time (else: true at some time)
        bool beside = false;            ///< the expect beside a ramp or a wave: only samples from `from` count
        bool opened = false;
    };
    struct Write
    {
        std::size_t step = 0;
        SimTime at{};
        std::vector<ScenarioWrite> writes;
        std::optional<std::size_t> signal{}; ///< of signals_: its value is worked out when it is sent
        bool signal_last = false;            ///< the signal's end value
        std::uint64_t ticket = 0;
        SimTime judge{};
    };
    /// A ramp or a wave of one variable that is being written.
    struct Signal
    {
        std::size_t step = 0;
        ScenarioSignal signal;
        SimTime from_time{};
        SimDuration period{};
        std::optional<double> base{}; ///< found at its first write, if the step does not name it
        std::int64_t next = 0; ///< the next write is at from_time + next * period (or the end)
        bool done = false;
    };

    void issue();
    void issue_step(std::size_t index, SimTime time);
    void submit(SimTime from_exclusive, SimTime to_inclusive, bool at_start);
    bool submit_one(Write write);
    void judge(SimTime p);
    void observe(SimTime p);
    void evaluate(Watch& watch, SimTime t);
    void close(Watch& watch, SimTime t);
    void started(std::size_t step);
    void pass(std::size_t step, SimTime at, SimTime end);
    void fail(std::size_t step, SimTime at, std::string code, std::string message, TimelineFailure failure = {});
    void finish(ScenarioStatus status);
    [[nodiscard]] bool unusable(std::size_t step, SimTime time);
    /// The conditions of `step` that the values do not meet ("" if none); with `since`, only those
    /// whose latest sample is at or after it are judged.
    [[nodiscard]] std::string unmet(const ScenarioStep& step, std::optional<SimTime> since = std::nullopt) const;
    /// The same conditions, for the record: which, with the value and the time of its sample.
    [[nodiscard]] TimelineFailure unmet_values(const ScenarioStep& step, std::optional<SimTime> since = std::nullopt) const;
    [[nodiscard]] bool is_unmet(const ScenarioCondition& condition, std::optional<SimTime> since, bool& judged) const;
    /// The write of `writes` to `peer` that was not made, for the record.
    [[nodiscard]] TimelineFailure failed_write(const std::vector<ScenarioWrite>& writes, SimTime at, const std::string& peer) const;
    /// Once the scenario is over: closes the record (see the class comment).
    void close_record();
    /// The time of the watch beside a ramp or a wave, for the messages: "from 40 ms to 120 ms, ...".
    [[nodiscard]] std::string during(std::size_t step, const Watch& watch) const;
    [[nodiscard]] SimTime judge_time(const std::vector<ScenarioWrite>& writes, SimTime at) const;
    [[nodiscard]] SimTime next_tick_after(SimTime t) const;

    const Scenario& scenario_;
    SimDuration tick_;
    ScenarioPort& port_;
    OnStep on_step_;
    ScenarioRun run_;
    bool over_ = false;
    bool begun_ = false;
    SimTime now_{};                  ///< the tick being handled
    std::vector<Variable> variables_;
    std::vector<std::deque<std::shared_ptr<const control::Snapshot>>> samples_; ///< per participant, not looked at yet
    std::size_t next_ = 0;           ///< the next step to issue
    std::optional<SimTime> base_{SimTime{}}; ///< the end of the step before it; empty while that step waits
    std::deque<Watch> watches_;      ///< in step order
    std::deque<Write> to_send_;      ///< sets, by step
    std::deque<Write> sent_;         ///< in the order sent
    std::deque<Signal> signals_;
    /// Per step: what has yet to happen before it passes: writes not judged yet (signals not done
    /// count their writes to come), and 1 for the watch beside a ramp or a wave until it closes.
    std::vector<std::size_t> open_writes_;
    std::size_t done_ = 0; ///< steps that passed or were skipped
    TimelineRecord* record_ = nullptr;
    bool record_closed_ = false;
    std::optional<SimTime> last_seen_{}; ///< the time of the last sample looked at
    /// With a record, once the scenario is over: the samples that were not looked at.
    std::vector<std::pair<std::size_t, std::shared_ptr<const control::Snapshot>>> left_;
};

} // namespace vehicle_sil::launch
