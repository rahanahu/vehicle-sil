// SPDX-License-Identifier: MIT
//
// Where a run writes: the launcher's own lines, the participants' lines, their log files, and the
// events of run --events. Internal.
#pragma once

#include "check_words.hpp"
#include "run_context.hpp"

#include "vehicle_sil/launch/events.hpp"

#include <chrono>
#include <cstddef>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vehicle_sil::launch::detail {

/// The run's lines and log files.
///
/// Threads: every thread of the run writes through it: the main thread, the start window's
/// thread (the controller's), the scenario's thread, and each participant's reader thread
/// (process::Process calls on_line()).
///
/// Lock: `mutex_` guards the order of the lines (one line at a time, a participant's line with
/// its log file) and the log files. It is taken last but for the event stream's own mutex,
/// RunOptions::write_line and, to tell the state of the bridges, TimeCommands' mutex (through
/// RunControls::set_outside(), then the event stream's, as TimeCommands' mutex always does): the
/// order is `mutex_`, then TimeCommands'. The reverse never happens: what runs under TimeCommands'
/// mutex (the command and state observers, `accepted`, on_detach) writes events and never calls
/// this object. No other lock of the run is taken while `mutex_` is held. hold() gives it to
/// the caller, who then must not call say(), on_line() or anything that takes another lock of the
/// run. event() does not take it (EventStream keeps itself).
///
/// The bridges of --connect: their statistics lines that read_outside_counts() reads are the
/// launcher's to read while the bridge runs: they go to the log file, and their counts to the event
/// "outside" (OutsideNote, under `mutex_`) and, at the end, to the result (take_counts()). Only the
/// last one, the bridge's report for people, is shown, once, at the end (take_counts()). Every count
/// of the line and each link line also go to the bridge's OutsideState (under `mutex_`, as the lines
/// come from the reader threads); the thread that watches the simulation asks it whether the run
/// must stop (outside_lost()), and the result gets its sums (take_link()). The change of a link
/// says itself in one line of the launcher.
///
/// Lifetime: it must outlive the participants' processes (their reader threads end in
/// process::Process's destructor).
class RunOutput
{
public:
    explicit RunOutput(const RunContext& ctx);

    /// The output, held while this lives: no line of the launcher or of a participant gets out in
    /// between. For what must come out together (a participant's "started" event before its lines).
    class Held
    {
    public:
        /// As RunOutput::say().
        void say(const std::string& text) const { out_.say_held(text); }

    private:
        friend class RunOutput;
        explicit Held(RunOutput& out) : out_(out), lock_(out.mutex_) {}
        RunOutput& out_;
        std::unique_lock<std::mutex> lock_;
    };
    [[nodiscard]] Held hold() { return Held(*this); }

    /// A line of the launcher: a "log" event of run --events, else a line with the launcher's tag.
    void say(const std::string& text) { hold().say(text); }

    /// Writes an event (run --events) with what `emit` writes. Does not take the lock.
    template <class Emit>
    void event(Emit emit) const
    {
        if (ctx_.observers.events != nullptr)
        {
            emit(*ctx_.observers.events);
        }
    }

    /// --log-dir: opens a log for every enabled participant and notes its name in `outcome`.
    /// false (with outcome.setup.error) if one cannot be written. Main thread, before any
    /// participant starts: the files are only used under the lock from then on.
    [[nodiscard]] bool open_logs(RunOutcome& outcome);

    /// A line that participant `index` wrote (its reader thread). Of a bridge of --connect, it is
    /// also kept for last_line(), and a statistics line with its counts is not shown (see above).
    void on_line(std::size_t index, std::string_view line);

    /// run --events: writes the event "outside" of each bridge whose counts were held back and are
    /// due at `now` (OutsideNote::due()). The thread that watches the simulation, at each look.
    void outside_due(std::chrono::steady_clock::time_point now);

    /// Why the run must stop for a bridge at `now`: its link or bus has been down for
    /// RunOptions::outside_lost_after. The bridge's index and the reason ("link down for 30 s"), or
    /// empty. The thread that watches the simulation, at each look.
    [[nodiscard]] std::optional<std::pair<std::size_t, std::string>> outside_lost(std::chrono::steady_clock::time_point now);

    /// Looks at which bridges share their device with a bridge that is not this run's
    /// (foreign_sharing() with `own`: one entry per participant), keeps what it finds, and says
    /// once of each new one "outside: CAN -> can0: also used by ... at 3.10 s". With `stop_after`
    /// (--strict): the first bridge whose device has been shared for that long or more, and the
    /// bridge is marked in its OutsideLink (OutsideLink::shared_stop; `stop_after` 0 is "at the
    /// start"). The thread that watches the simulation, at each look; the start of a --strict run.
    [[nodiscard]] std::optional<std::size_t> outside_shared(std::chrono::steady_clock::time_point now,
                                                            const std::vector<OutsideOwn>& own,
                                                            std::optional<std::chrono::milliseconds> stop_after);

    /// Every bridge of `bridges` has written a link line (it says whether the device is shared).
    [[nodiscard]] bool link_lines_read(const std::vector<std::size_t>& bridges);

    /// What was read of the bridge `index`'s link line and counts, added up at `now` (OutsideState::summary()).
    /// After its output has been read to the end.
    [[nodiscard]] OutsideLink take_link(std::size_t index, std::chrono::steady_clock::time_point now);

    /// The last counts of the bridge `index`, or empty if none were read; first writes the event
    /// "outside" with any that were held back (OutsideNote::flush()), then shows the last statistics
    /// line read as a line of the bridge. After its output has been read to the end, and before
    /// the result: the last event "outside" has the result's counts.
    [[nodiscard]] std::optional<OutsideCounts> take_counts(std::size_t index);

    /// The last line of the bridge `index` (RunMember::outside) that says why it failed
    /// (detail::LastLine::failure()), or "". Takes the lock.
    [[nodiscard]] std::string last_line(std::size_t index);

private:
    void write(const std::string& line) const;
    void say_held(const std::string& text) const;
    /// The line of participant `index` as people see it: a "log" event, or a line with its tag. Under the lock.
    void show(std::size_t index, std::string_view line) const;
    /// What the plan says the participant `index` connects (RunPlan::members: fixed before the run,
    /// read without the lock); null if it is not a bridge of --connect. The bridge's state below
    /// (`outside_`, `states_`, at the same index) is what changes while it runs.
    [[nodiscard]] const NetworkOutside* outside_of(std::size_t index) const;
    /// The event "outside" of the bridge `index` (run --events). Under the lock. Only for a bridge.
    void write_outside(std::size_t index, const OutsideCounts& counts) const;
    /// Tells RunControls the state of the bridges at `now` (RunState::outside), if it differs from
    /// what was told last. Under the lock, which also keeps two threads from telling an older state
    /// after a newer one. Only for a run with a bridge of --connect.
    void tell_outside(std::chrono::steady_clock::time_point now);
    /// "outside: ETH_EXT -> enp3s0: link down at 12.40 s" for a change of the link line of the bridge `index`. Under the lock. Only for a bridge.
    void say_link_change(std::size_t index, const std::string& what, std::chrono::steady_clock::time_point now);
    /// "outside: CAN -> can0: also used by another vehicle-sil bridge (process 4321) at 3.10 s". Under the lock.
    void say_outside_shared(std::size_t index, const OutsideForeign& foreign, std::chrono::steady_clock::time_point now);

    const RunContext& ctx_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<std::ofstream>> logs_; ///< per participant; guarded by mutex_ once the participants run
    std::vector<LastLine> last_lines_;                 ///< per participant (only a bridge's is used); guarded by mutex_
    std::vector<OutsideNote> outside_; ///< per participant (only a bridge's is used); guarded by mutex_
    /// Per participant: the bridge's link line and counts added up (only a bridge's is used); guarded by mutex_
    std::vector<OutsideState> states_;
    /// What tell_outside() told last; guarded by mutex_
    std::vector<RunState::Outside> told_outside_;
    /// When this was made: the run's start in the lines that say when the link changed.
    std::chrono::steady_clock::time_point began_ = std::chrono::steady_clock::now();
    /// Per participant: the last statistics line read (a bridge's), until take_counts() shows it; guarded by mutex_
    std::vector<std::string> last_statistics_;
};

} // namespace vehicle_sil::launch::detail
