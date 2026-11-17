// SPDX-License-Identifier: MIT
//
// The requests to stop a run: how many have come, and what asked first.
#pragma once

#include <atomic>

namespace vehicle_sil::launch {

/// The requests to stop: a count and the source of the first one. The caller of run_file() or
/// test_file() owns it and lends it (RunOptions::stops); it lives longer than a run, so that a
/// request that comes before a run, or between the runs of a test, is not lost.
///
/// What the count means is the run's: 1 stops the simulation in order, 2 or more skip the waits
/// for the participants to end. Nothing here resets it.
///
/// A source is a number other than 0 whose meaning is the caller's (a signal's number, a code of
/// its own); 0 means that nothing has asked.
///
/// Threads and signals: every function may be called from any thread at any time. request(),
/// count() and first() touch only the two lock-free atomics (no lock, no allocation, no output),
/// so a signal handler or a console control handler may call them. request_first() calls
/// `before`, so it is as safe as `before` is: not for a signal handler unless `before` is.
class StopRequests
{
public:
    /// A request from `source`, counted `count` times: 1 as one Ctrl+C (the first stops the run in
    /// order, the second skips the waits), 2 for a request that must not wait for a second one.
    /// `source` is kept only if nothing asked before.
    void request(int source, int count = 1) noexcept
    {
        int none = 0;
        first_.compare_exchange_strong(none, source);
        count_.fetch_add(count);
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
        int zero = 0;
        count_.compare_exchange_strong(zero, 1);
        return true;
    }

    /// How many requests have come (request()'s `count` added up).
    [[nodiscard]] int count() const noexcept { return count_.load(); }
    /// The source of the first request; 0 if nothing has asked.
    [[nodiscard]] int first() const noexcept { return first_.load(); }

private:
    static_assert(std::atomic<int>::is_always_lock_free, "a signal handler needs lock-free counters");

    std::atomic<int> count_{0};
    std::atomic<int> first_{0};
};

} // namespace vehicle_sil::launch
