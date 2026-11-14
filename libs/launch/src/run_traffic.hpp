// SPDX-License-Identifier: MIT
//
// What went over the networks of a run (RunTraffic): the count, from the samples of the ECUs'
// control agents as they come. It eats what it is given and touches neither SIL Kit nor the hub
// (RunSamples gives it the samples and what each agent's hello says). Internal.
#pragma once

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/config.hpp"
#include "vehicle_sil/launch/run.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::launch::detail {

/// Where the samples of one control agent have the J1939 counters of one network.
struct TrafficColumns
{
    std::string network; ///< the N of "j1939.<N>.frames_sent": the network's name in the simulation
    std::optional<std::size_t> sent{};     ///< the index of "j1939.<N>.frames_sent"
    std::optional<std::size_t> received{}; ///< the index of "j1939.<N>.frames_received"
    /// A mapping of the entry's `variables` applies to one of the two: the hub has changed its
    /// name or its values before anyone sees a sample.
    bool mapped = false;
    [[nodiscard]] bool complete() const { return sent && received; }
};

/// What a control agent's hello says that the count needs.
struct TrafficSchema
{
    bool virtual_time = true;
    std::vector<TrafficColumns> counters{}; ///< per network, in the order of the agent's variables
};

/// The networks a control agent has J1939 counters for, from the variables of its hello: those
/// its agent announced as "j1939.<N>.frames_sent" and "j1939.<N>.frames_received" (the names of
/// the agent, also where a mapping shows them under another one), integers only.
[[nodiscard]] TrafficSchema traffic_schema(const control::PeerInfo& peer);

/// Who is counted where: the networks of the configuration, and for each participant of the run
/// whether --describe says which networks it is on.
struct TrafficPlan
{
    struct Network
    {
        std::string name;
        std::string kind;
    };
    struct Participant
    {
        std::string name;
        bool in_run = false; ///< enabled: the run has it
        /// --describe says its networks (`on`, under the simulation's names); false: they
        /// are the networks its control agent has counters for.
        bool described = false;
        std::vector<std::string> on{};
    };
    std::vector<Network> buses{};            ///< the networks of the configuration, in its order
    std::vector<Participant> participants{}; ///< in the order of the configuration
};

/// The plan of a checked configuration (`check.participants` in the order of config.participants).
[[nodiscard]] TrafficPlan traffic_plan(const SimulationConfig& config, const CheckReport& check);

/// Counts. Per participant it keeps the last sample that is settled (of a step that began before
/// the controller's time when it was taken) and the samples that are not yet (pending).
///
/// The rule: the numbers of a participant are those of its last step that began before the run's
/// end E. Whoever takes samples from the hub reads the controller's time N first, gives the
/// samples (sample()), and then settles (settle(N)): every sample of a step before N has come by
/// then (docs/internals/control.md), so what moves to "settled" does not depend on when the
/// samples of the steps from N on arrive. At the end, finish(E) settles once more and drops what
/// is still pending.
///
/// Threads and locks: none of its own; its owner (RunSamples) calls it from one thread at a time.
class TrafficCount
{
public:
    /// A participant may have that many samples pending once the samples taken are settled;
    /// beyond, it is not counted (QueueFull). One that runs in virtual time is at most a step of
    /// the controller ahead.
    static constexpr std::size_t kMaxPending = 4096;

    explicit TrafficCount(TrafficPlan plan);

    /// What the hello of participant `i`'s control agent says (of the one whose samples come, or
    /// of the one that is there when none came).
    void hello(std::size_t i, TrafficSchema schema);
    /// More than one process ran as participant `i`.
    void restarted(std::size_t i);
    /// Samples of participant `i` were dropped before they got here.
    void dropped(std::size_t i);
    /// A sample of participant `i`, in the order taken. `incarnation` numbers its control agents
    /// from 1; another number than the last one's is a new process.
    void sample(std::size_t i, std::uint64_t incarnation, std::shared_ptr<const control::Snapshot> sample);
    /// The controller's step at `now` had begun before the samples given so far were taken.
    void settle(SimTime now);
    /// The run ended at `end_ms`: the result. Nothing may be given after it.
    [[nodiscard]] RunTraffic finish(std::int64_t end_ms);

private:
    struct Counted
    {
        bool hello = false;
        TrafficSchema schema{};
        std::uint64_t incarnation = 0; ///< of the samples kept; 0: none came
        bool restarted = false;
        bool dropped = false;
        std::shared_ptr<const control::Snapshot> settled{};
        std::deque<std::shared_ptr<const control::Snapshot>> pending{};
    };

    [[nodiscard]] TrafficParticipant row(std::size_t i, const std::string& network) const;

    TrafficPlan plan_;
    std::vector<Counted> counted_;
};

} // namespace vehicle_sil::launch::detail
