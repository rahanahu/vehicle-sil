// SPDX-License-Identifier: MIT
//
// One place that watches the simulation for the control server: time, the participants' SIL Kit
// states, a failed participant and the end (vehicle_sil::control_grpc).
#pragma once

#include "vehicle_sil/control_grpc/server.hpp"
#include "vehicle_sil/core/failure_check.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vehicle_sil::control_grpc {

/// The simulation as SimulationMonitor last saw it.
struct SimulationSnapshot
{
    /// Where the time stands (TimeControl::state()).
    TimeView time{};
    /// Not empty: TimeControl::state() failed with this message (`time` is then the previous one).
    std::string time_error{};
    /// The participants and their SIL Kit lifecycle states (TimeControl::participants()).
    std::vector<std::pair<std::string, std::string>> participants{};
    /// Not empty: the participants could not be read, with this message (`participants` is then
    /// the previous list).
    std::string participants_error{};
    /// The participant that failed the simulation (FailureCheck), once one did. It stays set.
    std::optional<ParticipantFailure> failure{};
    /// The simulation ran and is over (time.ended).
    bool ended = false;
    /// Grows by one with every change; 0 before the first read.
    std::uint64_t version = 0;

    /// The SIL Kit state of `participant`; "" if it is not a participant or not seen yet.
    [[nodiscard]] std::string state_of(const std::string& participant) const;
    /// Equal to `other` apart from the time and the version.
    [[nodiscard]] bool same_state(const SimulationSnapshot& other) const;
};

struct SimulationMonitorOptions
{
    /// How often the monitor's thread reads the TimeControl. 0: no thread (only poll() reads).
    std::chrono::milliseconds period{50};
    /// FailureCheck's grace for a participant that left in a state other than Error.
    std::chrono::steady_clock::duration leave_grace = FailureCheck::kDefaultGrace;
    /// The clock of the failure check; empty: std::chrono::steady_clock::now.
    std::function<std::chrono::steady_clock::time_point()> clock{};
};

/// Reads a TimeControl in one thread, keeps the last snapshot, and tells subscribers when it
/// changes. Any number of readers (ListPeers, Watch streams, the tool's main loop) then share one
/// reader of the simulation. Depends only on TimeControl.
///
/// Thread safety: every function is thread safe. Subscribers are called on the monitor's thread
/// (or the thread that calls poll()), one at a time, with no lock of the caller held; they must be
/// short and must not subscribe, unsubscribe or poll. Unsubscribing (destroying the Subscription)
/// waits for a call to that subscriber that is running, so a subscriber may refer to the
/// subscription's owner. The TimeControl must outlive the monitor.
class SimulationMonitor
{
public:
    using Callback = std::function<void(const SimulationSnapshot&)>;

    /// Unsubscribes when destroyed (or reset()). Move only.
    class Subscription
    {
    public:
        Subscription() = default;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        ~Subscription();
        void reset();

    private:
        friend class SimulationMonitor;
        Subscription(SimulationMonitor* monitor, std::uint64_t id) : monitor_(monitor), id_(id) {}
        SimulationMonitor* monitor_ = nullptr;
        std::uint64_t id_ = 0;
    };

    /// Reads `time` once, then (with a period) starts the thread.
    explicit SimulationMonitor(TimeControl& time, SimulationMonitorOptions options = {});
    /// Stops the thread. Subscriptions must have been reset before.
    ~SimulationMonitor();
    SimulationMonitor(const SimulationMonitor&) = delete;
    SimulationMonitor& operator=(const SimulationMonitor&) = delete;

    [[nodiscard]] SimulationSnapshot snapshot() const;
    /// `callback` is called with the new snapshot after each change. It is not called for the
    /// current one: read snapshot() after subscribing and skip what is not newer (version).
    [[nodiscard]] Subscription subscribe(Callback callback);
    /// Reads the TimeControl now (as the thread does every period) and tells the subscribers if
    /// something changed.
    void poll();

private:
    void unsubscribe(std::uint64_t id);
    void run();

    TimeControl& time_;
    SimulationMonitorOptions options_;

    std::mutex poll_mutex_; ///< one poll at a time; also guards check_ and the subscriber calls
    FailureCheck check_;

    mutable std::mutex mutex_; ///< snapshot_, subscribers_, next_id_, stop_
    std::condition_variable cv_;
    SimulationSnapshot snapshot_;
    std::map<std::uint64_t, Callback> subscribers_;
    std::uint64_t next_id_ = 1;
    bool stop_ = false;

    std::thread thread_;
};

} // namespace vehicle_sil::control_grpc
