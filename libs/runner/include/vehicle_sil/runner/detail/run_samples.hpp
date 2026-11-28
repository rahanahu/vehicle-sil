// SPDX-License-Identifier: MIT
//
// Every sample of the ECUs' control agents that the run's hub takes: the hub's one tap, what it
// keeps for the scenario (a queue) and for the traffic of the result (the latest samples of each
// participant). Internal.
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
#include <utility>
#include <vector>

namespace vehicle_sil::runner::detail {

/// The samples of a run. The hub has one tap, and it is this one, in every run. For each sample
/// of a participant of the configuration it does two things:
/// - For the traffic: it puts the sample among the participant's latest ones (TrafficLatest),
///   which are a few however long the run lasts and however long nobody looks. No queue, so
///   nothing can fill up, and nobody has to take anything while the run lasts. finish() reads
///   them once, at the end.
/// - For the scenario: from scenario_begins() to scenario_ends(), it appends the samples of the
///   control agents named then to a queue, which take() empties. As before the traffic was
///   counted: only the scenario's participants, every sample of them, without a limit (the
///   scenario holds the controller's step while it works, so the queue holds a step's samples).
///
/// The state, who touches it, and under which lock (mutex_, the only one):
/// - kept_, scenario_, queue_: the hub's tap, on the controller's SIL Kit thread and inside the
///   hub's call (under the hub's lock); scenario_begins() (the start window's thread), take()
///   (the scenario's thread, then the main thread), scenario_ends() and finish() (the main
///   thread). Nothing is taken and nothing is called while it is held, but `now_` in the tap,
///   which must only read (the controller's time is an atomic). The tap does not wait, does not
///   call the hub, and allocates only where the scenario's queue or a participant's incarnation
///   grows.
/// - hub_, step_, now_: set by tap() before the hub's transport brings anything; never changed after.
/// - tapped_, finished_: the main thread only.
/// finish() asks the hub (peers()) without holding mutex_. The order of the locks: the hub's,
/// then mutex_ (the tap); never the other way round.
class RunSamples
{
public:
    /// The controller's time: the start of its step that runs or ran last. Called inside the
    /// tap, on the controller's SIL Kit thread: it must not wait.
    using Now = std::function<SimTime()>;

    /// One batch of virtual-timestamped structured ECU events received via Hub::set_events_tap().
    struct EventBatch
    {
        std::string participant{};
        std::vector<control::Event> events{};
        std::uint64_t agent_dropped = 0;
    };

    /// `names`: the participants in the order of the configuration (the places the samples are
    /// given under). `plan`: who is counted where.
    RunSamples(std::vector<std::string> names, TrafficPlan plan);
    ~RunSamples();
    RunSamples(const RunSamples&) = delete;
    RunSamples& operator=(const RunSamples&) = delete;

    /// Sets the tap on `hub`. `step`: the controller's step (> 0). `tap_events`: also attach
    /// Hub::set_events_tap() for opt-in virtual-time ECU event recording. Main thread, in the setup
    /// (before the hub's transport brings anything). The hub and what `now` reads must live
    /// until untap().
    void tap(control::Hub& hub, SimDuration step, Now now, bool tap_events = false);
    /// Takes the tap(s) off (once it returns, no tap is running or called again). Main
    /// thread; before the hub's transport and the controller go. Does nothing without a tap.
    void untap();
    /// A tap is set (between tap() and untap()). Main thread.
    [[nodiscard]] bool tapped() const { return tapped_ || events_tapped_; }

    /// From here on the scenario or manual timeline recorder gets samples: those the tap takes
    /// after this call, of the control agents `incarnations` names (per participant of the
    /// configuration; "": none of it).
    void scenario_begins(std::vector<std::string> incarnations);
    /// The scenario or manual timeline recorder gets no sample any more; what it has not taken is dropped.
    void scenario_ends();
    /// Alias of scenario_begins() for manual-run timeline recording.
    void recording_begins(std::vector<std::string> incarnations) { scenario_begins(std::move(incarnations)); }
    /// Alias of scenario_ends() for manual-run timeline recording.
    void recording_ends() { scenario_ends(); }
    /// From here on structured ECU events from all connected agents are queued for timeline recording.
    void events_begins();
    /// Stops queueing structured ECU events and clears any unread batches.
    void events_ends();
    /// The scenario's or manual recorder's samples since the last call, in the order taken: the
    /// participant's place in the configuration, the sample.
    [[nodiscard]] std::vector<std::pair<std::size_t, std::shared_ptr<const control::Snapshot>>> take();
    /// Atomically returns all remaining queued samples and stops queueing new ones (as take() +
    /// recording_ends() under one lock).
    [[nodiscard]] std::vector<std::pair<std::size_t, std::shared_ptr<const control::Snapshot>>> take_and_end();
    /// Structured ECU event batches received since the last call, in arrival order.
    [[nodiscard]] std::vector<EventBatch> take_events();
    /// Atomically returns all remaining queued event batches and stops queueing new ones.
    [[nodiscard]] std::vector<EventBatch> take_events_and_end();

    /// The run ended at `end_ms` (RunTiming::sim_ms): the traffic, from each participant's last
    /// step that began before it; then the tap goes. Main thread, once.
    [[nodiscard]] RunTraffic finish(std::int64_t end_ms);

private:
    struct Kept
    {
        TrafficLatest latest;
        std::string incarnation;        ///< of the samples in `latest`; "": none came
        std::uint64_t incarnations = 0; ///< how many control agents' samples came
    };

    const std::vector<std::string> names_;
    const TrafficPlan plan_;
    control::Hub* hub_ = nullptr;
    SimDuration step_{1};
    Now now_;
    std::mutex mutex_;
    // Guarded by mutex_:
    std::vector<Kept> kept_;
    std::vector<std::string> scenario_; ///< per participant: the agent whose samples the scenario gets; empty: no scenario runs
    std::vector<std::pair<std::size_t, std::shared_ptr<const control::Snapshot>>> queue_;
    bool events_recording_ = false;
    std::vector<EventBatch> event_queue_;
    // The main thread's:
    bool tapped_ = false;
    bool events_tapped_ = false;
};

} // namespace vehicle_sil::runner::detail
