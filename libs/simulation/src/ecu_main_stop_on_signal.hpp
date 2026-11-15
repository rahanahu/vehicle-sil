// SPDX-License-Identifier: MIT
//
// Stopping ecu_main() (vehicle_sil/simulation/ecu_main.hpp) on a signal or a console event, for
// --registry-uri. Internal to the library; not installed, not part of the API.
#pragma once

#include <atomic>
#include <string>

#ifdef _WIN32
#include <condition_variable>
#include <mutex>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <thread>
#endif

namespace vehicle_sil {
class Peer;
}

namespace vehicle_sil::ecu_main_detail {

/// --registry-uri: while it lives, SIGINT and SIGTERM (Windows: Ctrl+C, Ctrl+Break, closing the
/// console) end the ECU in order once the simulation runs: the participant stops (which stops the
/// coordinated simulation for every participant), run() returns and the destructors run. Before
/// the simulation runs (connecting, waiting for the start) the process ends at once, as it would
/// by default, with a message and kExitInterrupted. A second signal ends it at once.
class StopOnSignal
{
public:
    explicit StopOnSignal(std::string name);
    ~StopOnSignal();
    StopOnSignal(const StopOnSignal&) = delete;
    StopOnSignal& operator=(const StopOnSignal&) = delete;

    /// The participant to stop once it runs.
    void attach(Peer& peer) { peer_.store(&peer); }
#ifdef _WIN32
    /// The run has returned (a handler of a closing console waits for it).
    void run_returned();
#else
    void run_returned() {}
#endif
    /// The signal that stopped the ECU in order ("SIGTERM"), or "".
    [[nodiscard]] std::string stopped_by() const;

private:
    static std::string signal_text(int signal);

    /// Writes out what the application wrote with printf() and the like (not in a signal handler:
    /// this runs on handle()'s thread), but waits for it at most 500 ms: fflush() waits for the
    /// lock of each stream, which the main thread may hold for ever (a write to a full pipe).
    static void flush_briefly();

    /// On its own thread (POSIX: wait_for_signals(); Windows: the console's handler thread).
    void handle(int signal);

#ifdef _WIN32
    static BOOL WINAPI on_console_event(DWORD event);
#else
    static void on_signal(int signal);
    void wait_for_signals();

    int read_ = -1;
    int write_ = -1;
    struct sigaction old_int_{};
    struct sigaction old_term_{};
    std::thread thread_;
#endif

#ifdef _WIN32
    std::mutex mutex_;
    std::condition_variable done_changed_;
    bool done_ = false; ///< guarded by mutex_
#endif
    static inline std::atomic<StopOnSignal*> instance_{nullptr};
    std::string name_;
    std::atomic<Peer*> peer_{nullptr};
    std::atomic<int> stopped_by_{0};
};

} // namespace vehicle_sil::ecu_main_detail
