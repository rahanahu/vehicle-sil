// SPDX-License-Identifier: MIT
//
// The requests to stop a run: how many have come, and what asked first.
#pragma once

#include <atomic>
#include <string>

namespace vehicle_sil::runner {

/// What kind of party asked to stop a run: a person or a client that gave the command (Operator),
/// a signal or a console event of the operating system (Signal), or the program that runs the
/// launcher and went away (Supervisor).
enum class StopKind
{
    Operator,
    Signal,
    Supervisor,
};

/// The stop levels that a run distinguishes (StopRequests::count() / raise_to() / request()).
namespace stop_level {
/// No stop has been requested.
inline constexpr int kNone = 0;
/// First stop request: stops the simulation in order.
inline constexpr int kGraceful = 1;
/// Second (or immediate forceful) stop request: skips waits for participants to end.
inline constexpr int kForce = 2;
} // namespace stop_level

/// The sources of a stop request that the library knows (negative; a source of 1 or more is the
/// caller's own: a signal's number, a console event's code).
namespace stop_source {
/// {"command":"stop"} on standard input.
inline constexpr int kStopCommand = -1;
/// Standard input ended (run --stdin-commands).
inline constexpr int kEndOfInput = -2;
/// The reader of the events has gone (run --events).
inline constexpr int kOutputClosed = -3;
/// A client of the control server called Stop (run --control).
inline constexpr int kControlStop = -4;
} // namespace stop_source

/// The name of a source that the library knows, as `interrupted_by` shows it; empty for any other.
[[nodiscard]] inline std::string stop_source_name(int source)
{
    switch (source)
    {
    case stop_source::kStopCommand: return "stop command";
    case stop_source::kEndOfInput: return "end of input";
    case stop_source::kOutputClosed: return "output closed";
    case stop_source::kControlStop: return "control server stop";
    default: return {};
    }
}

/// The kind of a source: the stop command and the control server's Stop are the Operator's; the
/// end of standard input and a closed output are the Supervisor's; any other source (a signal, a
/// console event) is a Signal.
[[nodiscard]] constexpr StopKind stop_kind(int source) noexcept
{
    switch (source)
    {
    case stop_source::kStopCommand:
    case stop_source::kControlStop: return StopKind::Operator;
    case stop_source::kEndOfInput:
    case stop_source::kOutputClosed: return StopKind::Supervisor;
    default: return StopKind::Signal;
    }
}

/// The name of a kind: "operator", "signal" or "supervisor".
[[nodiscard]] constexpr const char* stop_kind_name(StopKind kind) noexcept
{
    switch (kind)
    {
    case StopKind::Operator: return "operator";
    case StopKind::Signal: return "signal";
    case StopKind::Supervisor: return "supervisor";
    }
    return "signal";
}

/// The requests to stop: a count and the source of the first one. The caller of run_file() or
/// test_file() owns it and lends it (to RunControls, or as RunLent::stops); it lives longer than
/// a run, so that a request that comes before a run, or between the runs of a test, is not lost.
///
/// What the count means is the run's: 1 stops the simulation in order, 2 or more skip the waits
/// for the participants to end. Nothing here resets it.
///
/// A source is a number other than 0 whose meaning is the caller's (a signal's number, a code of
/// its own); 0 means that nothing has asked.
///
/// Threads and signals: every function may be called from any thread at any time. request(),
/// raise_to(), count() and first() touch only the two lock-free atomics (no lock, no allocation, no
/// output), so a signal handler or a console control handler may call them. request_first() calls
/// `before`, so it is as safe as `before` is: not for a signal handler unless `before` is.
class StopRequests
{
public:
    /// A request from `source`, counted `count` times: stop_level::kGraceful (1) as one Ctrl+C
    /// (the first stops the run in order, the second skips the waits), stop_level::kForce (2) for
    /// a request that must not wait for a second one. `source` is kept only if nothing asked before.
    void request(int source, int count = stop_level::kGraceful) noexcept
    {
        int none = 0;
        first_.compare_exchange_strong(none, source);
        count_.fetch_add(count);
    }

    /// Atomically raises count_ to at least `level` (no-op if level <= stop_level::kNone or
    /// count_ >= level). Lock-free (safe from any thread or signal handler). Records `source` only
    /// if nothing asked before.
    int raise_to(int source, int level) noexcept
    {
        if (level <= stop_level::kNone)
        {
            return count_.load();
        }
        int none = 0;
        first_.compare_exchange_strong(none, source);
        int cur = count_.load();
        while (cur < level && !count_.compare_exchange_weak(cur, level))
        {
        }
        return cur < level ? level : cur;
    }

    /// A request only if nothing has asked for a stop yet (never a second request). `before` runs
    /// between the decision and the request: what it does (an event that says why) is done before
    /// the run reacts. A request that comes while `before` runs counts as the first request too,
    /// not as a second one. false, without a call of `before`, if a stop had been requested.
    template <class Before>
    bool request_first(int source, Before before)
    {
        int none = 0;
        if (!first_.compare_exchange_strong(none, source))
        {
            return false;
        }
        before();
        int zero = stop_level::kNone;
        count_.compare_exchange_strong(zero, stop_level::kGraceful);
        return true;
    }

    /// How many requests have come (request()'s `count` added up).
    [[nodiscard]] int count() const noexcept { return count_.load(); }
    /// The source of the first request; 0 if nothing has asked.
    [[nodiscard]] int first() const noexcept { return first_.load(); }

    /// A request that reached the participants as well, at the same moment (on Windows, the
    /// console was closed: every process attached to it gets that event, also those in a process
    /// group of their own). The run then counts as interrupted if the request came before its
    /// participants were ended, also after the simulation had ended, and the participants' own ends
    /// that such a request causes are no failures (Runner::run(), assess_run()). Call it before
    /// request(); lock-free, as request().
    void note_reached_participants() noexcept { reached_participants_.store(true); }
    [[nodiscard]] bool reached_participants() const noexcept { return reached_participants_.load(); }

private:
    static_assert(std::atomic<int>::is_always_lock_free, "a signal handler needs lock-free counters");
    static_assert(std::atomic<bool>::is_always_lock_free, "a signal handler needs lock-free flags");

    std::atomic<int> count_{0};
    std::atomic<int> first_{0};
    std::atomic<bool> reached_participants_{false};
};

} // namespace vehicle_sil::runner
