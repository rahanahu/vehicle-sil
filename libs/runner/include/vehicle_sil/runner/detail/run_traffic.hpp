// SPDX-License-Identifier: MIT
//
// What went over the networks of a run (RunTraffic): which sample of each ECU's control agent
// counts, what a hello says of the counters, and the result. It eats what it is given and touches
// neither SIL Kit nor the hub (RunSamples gives it the samples and the hellos). Internal.
#pragma once

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/config/config.hpp"
#include "vehicle_sil/runner/run_outcome.hpp"
#include "vehicle_sil/runner/run_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

/// The plan of a checked configuration, as a run has it (RunPlan::config(), check and members):
/// `check.participants` in the order of config.participants, and one of `members` for each
/// participant of `config` (it says which are bridges of --connect).
[[nodiscard]] TrafficPlan traffic_plan(const SimulationConfig& config, const CheckReport& check,
                                       const std::vector<RunMember>& members);

/// The latest samples of one participant: enough of them to say, once the run's end E is known,
/// which was the sample of its last step that began before E. The hub's tap puts every sample
/// here and keeps no queue for the count.
///
/// The rule: E is the start of a step of the controller, and so is the controller's time N at any
/// moment (N <= E). When the controller's step at N has begun, every sample of a step before N
/// has come (docs/internals/control.md), so of those only the last one can be the answer: it is
/// `settled`. Of the samples from N on, the answer for an end at the controller's step B is the
/// last one before B, so one sample is kept per step of the controller (`ahead`, the last one
/// that began in it). A participant that runs in virtual time is at most a step of the controller
/// ahead, so `ahead` holds one or two samples; what was kept never depends on when the samples of
/// the steps from N on arrive, nor on how long nobody looks.
///
/// Threads and locks: none of its own (RunSamples keeps it under its mutex). put() does not wait
/// and does not allocate.
class TrafficLatest
{
public:
    /// A participant whose samples are that many steps of the controller ahead of it is not on
    /// the run's time (one that runs in real time while the simulation stands still): nothing of
    /// it is kept from then on, and it is not counted (off_time()).
    static constexpr std::size_t kMaxAhead = 64;

    TrafficLatest() { ahead_.reserve(kMaxAhead + 1); }

    /// A sample, as the hub takes it. `now`: the start of the controller's step that runs or ran
    /// last; `step`: the controller's step (> 0).
    void put(std::shared_ptr<const control::Snapshot> sample, SimTime now, SimDuration step);
    /// Another process runs under the participant's name: what was kept is not its.
    void clear();
    /// The sample of the last step that began before `end` (the start of a step of the
    /// controller, not before any `now` given); null if none came.
    [[nodiscard]] std::shared_ptr<const control::Snapshot> last_before(SimTime end, SimDuration step) const;
    [[nodiscard]] bool off_time() const { return off_time_; }

private:
    std::shared_ptr<const control::Snapshot> settled_{};
    /// (the number of the controller's step, the last sample that began in it), in the order of the steps.
    std::vector<std::pair<std::int64_t, std::shared_ptr<const control::Snapshot>>> ahead_{};
    bool off_time_ = false;
};

/// What the run knows of one participant at its end.
struct TrafficSeen
{
    bool hello = false; ///< a control agent of it said hello: `schema`
    TrafficSchema schema{};
    bool restarted = false; ///< more than one process ran under its name
    bool off_time = false;  ///< TrafficLatest::off_time()
    /// The sample of its last step that began before the end; null if none came.
    std::shared_ptr<const control::Snapshot> last{};
};

/// The result: every network of the plan with the participants known to be on it (`seen` in the
/// order of plan.participants), each counted or with the reason why not, the participants of
/// the run that may be on it without the launcher knowing (TrafficNetwork::unplaced), and those
/// that are known to be on no network (RunTraffic::unplaced).
[[nodiscard]] RunTraffic count_traffic(const TrafficPlan& plan, const std::vector<TrafficSeen>& seen, std::int64_t end_ms);

} // namespace vehicle_sil::launch::detail
