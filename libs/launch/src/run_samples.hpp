// SPDX-License-Identifier: MIT
//
// Every sample of the ECUs' control agents that the run's hub takes: the hub's one tap, the queue
// behind it, and who gets what is taken from the queue (the scenario's engine, the count of the
// traffic). Internal.
#pragma once

#include "run_traffic.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/core/sim_time.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vehicle_sil::launch::detail {

/// The samples of a run. The hub has one tap, and it is this one, in every run: it puts each
/// sample of a participant of the configuration into a queue. take() empties the queue and gives
/// what was in it, in the order taken, to the scenario (if one has begun) and to the count of the
/// traffic.
///
/// Who takes: with a scenario, the scenario's thread at every step of the controller it holds
/// (RunScenario::loop()), and the main thread once that thread has ended (RunScenario::finish());
/// without one, the main thread at every look of the watch (RunWatch::watch()). At the end the
/// main thread takes once more (finish()). Whoever takes reads the controller's time first and
/// passes it: the samples of every step before that time are in the queue by then.
///
/// The state, who touches it, and under which lock:
/// - queue_ (queue_mutex_): the hub's tap appends, on the controller's SIL Kit thread and inside
///   the hub's call (under the hub's lock); take() swaps it out; scenario_begins() reads how many
///   have been put. Nothing is taken and nothing is called while it is held: the tap only puts
///   (it does not wait, does not call the hub, and allocates only where the queue or a
///   participant's list of incarnations grows).
/// - sink_, from_, known_, count_, finished_, hub_ (take_mutex_): whoever takes, one at a time. Under it
///   take() takes queue_mutex_ (for the swap only), calls the hub (peers(), when a participant's
///   new process is seen first; the hub's lock), the scenario's sink and the count. Neither the
///   hub nor the scenario calls back into this class from there, and take_mutex_ is never taken
///   inside the hub's call of the tap.
/// - tapped_: the main thread only.
/// The order of the locks is so: take_mutex_, then the hub's or queue_mutex_; the hub's, then
/// queue_mutex_ (the tap).
class RunSamples
{
public:
    /// The queue holds that many samples, and that many values in them, at most (a taker that
    /// does not come: the main thread waiting for a reader of its output while the simulation
    /// runs on). What comes beyond is dropped, and the participant is not counted
    /// (TrafficNotCounted::QueueFull); a scenario does not see it either. Far more than a take
    /// finds in a run as fast as possible.
    static constexpr std::size_t kMaxQueued = 1u << 16;
    static constexpr std::size_t kMaxQueuedValues = 1u << 22;

    /// A sample for the scenario: the participant's place in the configuration, the incarnation
    /// of its control agent, the sample.
    using Sink =
        std::function<void(std::size_t participant, const std::string& incarnation, const std::shared_ptr<const control::Snapshot>& sample)>;

    /// `names`: the participants in the order of the configuration (the places the samples are
    /// given under). `plan`: who is counted where.
    RunSamples(std::vector<std::string> names, TrafficPlan plan);
    ~RunSamples();
    RunSamples(const RunSamples&) = delete;
    RunSamples& operator=(const RunSamples&) = delete;

    /// Sets the tap on `hub`. Main thread, in the setup (before the hub's transport brings
    /// anything). The hub must live until untap().
    void tap(control::Hub& hub);
    /// Takes the tap off (once it returns, the tap is not running and is not called again). Main
    /// thread; before the hub's transport goes. Does nothing without a tap.
    void untap();

    /// From here on the samples go to `sink` too: those the tap puts after this call (what is in
    /// the queue already is for the count only). The sink is called inside take(), on the thread
    /// that takes.
    void scenario_begins(Sink sink);
    /// No sample goes to the scenario's sink any more (it is not running once this returns).
    void scenario_ends();

    /// Takes what has arrived and gives it on. `now`: the controller's time (the start of its
    /// step that runs or ran last), read before this call.
    void take(SimTime now);
    /// The last take, with the run's end (RunTiming::sim_ms), then the tap goes: the traffic.
    /// Main thread. A take after it does nothing.
    [[nodiscard]] RunTraffic finish(std::int64_t end_ms);

private:
    struct Queued
    {
        std::size_t participant;
        std::uint64_t incarnation; ///< counted from 1 per participant
        std::shared_ptr<const control::Snapshot> sample;
    };
    struct Queue
    {
        std::vector<Queued> samples;
        std::size_t values = 0; ///< the values in `samples`
        std::uint64_t put = 0;  ///< how many samples have been put so far
        /// Per participant: the incarnations seen, in the order seen (a sample's number is its
        /// place here, from 1), and whether a sample of it was dropped since the last take.
        std::vector<std::vector<std::string>> incarnations;
        std::vector<bool> dropped;
    };

    void take_locked(SimTime now);

    const std::vector<std::string> names_;
    std::mutex queue_mutex_;
    Queue queue_; ///< guarded by queue_mutex_
    std::mutex take_mutex_;
    // Guarded by take_mutex_:
    Sink sink_;
    std::uint64_t from_ = 0;  ///< the scenario gets the samples put from this one on
    std::vector<std::vector<std::string>> known_; ///< the incarnations take() has seen
    TrafficCount count_;
    bool finished_ = false;
    control::Hub* hub_ = nullptr; ///< set by tap(), before anyone takes; never changed after
    // The main thread's:
    bool tapped_ = false;
};

} // namespace vehicle_sil::launch::detail
