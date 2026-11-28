// SPDX-License-Identifier: MIT
//
// Passive manual-run timeline recorder (run --record ... --timeline <file> without --scenario):
// captures snapshots from RunSamples into a TimelineRecord on the main thread (RunWatch) with
// zero controller guard overhead. Internal.
#pragma once

#include "run_context.hpp"
#include "run_control.hpp"
#include "vehicle_sil/runner/detail/run_samples.hpp"

#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/core/simulation_watch.hpp"
#include "vehicle_sil/scenario/timeline.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace vehicle_sil::runner::detail {

/// Records requested ECU variables into a TimelineRecord during a manual run (no --scenario).
///
/// Threads and locks:
/// - Constructed on the main thread inside RunnerImpl.
/// - begin() runs once on the start window's thread (inside SimulationController::start(), which
///   synchronizes-with the main thread before watch() / settle() ever run).
/// - drain(), finish(), and end() run on the main thread only (RunWatch::watch(), RunWatch::settle(),
///   and RunnerImpl::run()'s teardown guard). No lock of its own; RunSamples guards its queue.
class RunRecorder
{
public:
    RunRecorder(const RunContext& ctx, RunControl& control, RunSamples& samples) :
      ctx_(ctx), control_(control), samples_(samples)
    {
    }

    /// True when this run records a manual timeline (no scenario, --timeline set, --record non-empty).
    [[nodiscard]] bool active() const noexcept
    {
        return ctx_.scenario == nullptr && !ctx_.plan.timeline.empty() && !ctx_.plan.recorded_variables.empty();
    }

    /// Called on the start window thread once control-link hellos have arrived: creates the
    /// TimelineRecord, binds connected peers' mapped schemas to recorded variables, and enables
    /// sample queueing in RunSamples.
    void begin(control_silkit::HubTransport& transport);

    /// Drains currently queued samples from RunSamples into the TimelineRecord. Main thread.
    void drain();

    /// Stops sample queueing, drains all remaining samples, closes the TimelineRecord at the
    /// appropriate virtual-time horizon, and stores it on `outcome.scenario.timeline_record`.
    /// Main thread (RunWatch::settle(), strictly after controller.stop()/abort()).
    void finish(SimulationController& controller, const WatchResult& watched, RunOutcome& outcome);

    /// Ensures RunSamples stops queueing if the run leaves before finish(). Main thread.
    void end();

private:
    struct Binding
    {
        std::size_t timeline_index = 0; ///< index in TimelineRecord (0..extra_variables().size()-1)
        std::size_t value_index = 0;    ///< index in control::Snapshot::values (mapped schema)
    };

    void consume(std::vector<std::pair<std::size_t, std::shared_ptr<const control::Snapshot>>> taken);

    const RunContext& ctx_;
    RunControl& control_;
    RunSamples& samples_;
    std::shared_ptr<scenario::TimelineRecord> record_{};
    std::vector<std::vector<Binding>> bindings_{}; ///< per configuration participant index
    std::optional<SimTime> last_seen_{};
};

} // namespace vehicle_sil::runner::detail
