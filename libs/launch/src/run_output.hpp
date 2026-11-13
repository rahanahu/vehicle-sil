// SPDX-License-Identifier: MIT
//
// Where a run writes: the launcher's own lines, the participants' lines, their log files, and the
// events of run --events. Internal.
#pragma once

#include "run_context.hpp"

#include "vehicle_sil/launch/events.hpp"

#include <cstddef>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch::detail {

/// The run's lines and log files.
///
/// Threads: every thread of the run writes through it: the main thread, the start window's
/// thread (the controller's), the scenario's thread, and each participant's reader thread
/// (process::Process calls on_line()).
///
/// Lock: `mutex_` guards the order of the lines (one line at a time, a participant's line with
/// its log file) and the log files. It is taken last but for the event stream's own mutex and
/// RunOptions::write_line: no other lock of the run is taken while it is held. hold() gives it to
/// the caller, who then must not call say(), on_line() or anything that takes another lock of the
/// run. event() does not take it (EventStream keeps itself).
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
        if (ctx_.options.events != nullptr)
        {
            emit(*ctx_.options.events);
        }
    }

    /// --log-dir: opens a log for every enabled participant and notes its name in `outcome`.
    /// false (with outcome.setup_error) if one cannot be written. Main thread, before any
    /// participant starts: the files are only used under the lock from then on.
    [[nodiscard]] bool open_logs(RunOutcome& outcome);

    /// A line that participant `index` wrote (its reader thread).
    void on_line(std::size_t index, std::string_view line);

private:
    void write(const std::string& line) const;
    void say_held(const std::string& text) const;

    const RunContext& ctx_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<std::ofstream>> logs_; ///< per participant; guarded by mutex_ once the participants run
};

} // namespace vehicle_sil::launch::detail
