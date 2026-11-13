// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch run: the wait for the end of what the run has of SIL Kit, with a limit
// (Runner::close_connection()). No dependencies: tested as written.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

namespace vehicle_sil::launch::detail {

/// A close that had not ended when the wait for it did.
struct CloseLeft
{
    std::chrono::milliseconds waited{0};
    /// The wait was ended by `interrupted`, before the limit.
    bool interrupted = false;
};

/// Runs `close` on a thread of its own and waits for it: `limit` at most, and no longer than
/// until `interrupted()` is true (asked every `poll`). Empty if it ended (the thread is joined).
/// Else the thread is left running `close`, which therefore owns or outlives all that it uses,
/// and whatever it does later is nobody's business here. If no thread can be made, `close` runs
/// on the caller's, without the limit.
inline std::optional<CloseLeft> close_within(std::function<void()> close, std::chrono::milliseconds limit,
                                             std::chrono::milliseconds poll, const std::function<bool()>& interrupted)
{
    struct State
    {
        std::function<void()> close;
        std::mutex mutex;
        std::condition_variable changed;
        bool done = false;
    };
    const auto state = std::make_shared<State>();
    state->close = std::move(close);
    std::thread thread;
    try
    {
        thread = std::thread([state] {
            state->close();
            {
                const std::lock_guard lock(state->mutex);
                state->done = true;
            }
            state->changed.notify_all();
        });
    }
    catch (const std::system_error&)
    {
        state->close();
        return std::nullopt;
    }
    const auto began = std::chrono::steady_clock::now();
    bool cut = false;
    std::unique_lock lock(state->mutex);
    while (!state->done && std::chrono::steady_clock::now() - began < limit)
    {
        if (interrupted && interrupted())
        {
            cut = true;
            break;
        }
        state->changed.wait_for(lock, poll);
    }
    const bool done = state->done;
    lock.unlock();
    if (done)
    {
        thread.join();
        return std::nullopt;
    }
    thread.detach();
    return CloseLeft{std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began), cut};
}

} // namespace vehicle_sil::launch::detail
