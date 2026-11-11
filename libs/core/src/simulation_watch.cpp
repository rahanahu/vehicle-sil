// SPDX-License-Identifier: MIT
#include "vehicle_sil/core/simulation_watch.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"

namespace vehicle_sil {

WatchResult watch_simulation(const WatchTarget& target, const WatchOptions& options)
{
    const auto now = [&options] { return options.clock ? options.clock() : std::chrono::steady_clock::now(); };
    FailureCheck check(options.leave_grace);
    WatchResult result;
    const auto abort_with = [&target, &options](const WatchResult& why) {
        if (options.before_abort)
        {
            options.before_abort(why);
        }
        target.abort();
    };
    while (!(result.state = target.wait_until_finished(options.poll)))
    {
        if (options.interrupted && options.interrupted())
        {
            target.stop(options.stop_reason);
            result.end = WatchEnd::Interrupted;
            result.state = target.wait_until_finished(options.stop_wait);
            return result;
        }
        if (options.failed && options.failed())
        {
            result.end = WatchEnd::Failed;
            abort_with(result);
            return result;
        }
        // A failed participant stalls virtual time for everyone; SIL Kit does not end the run.
        if (auto failure = check.check(target.left_participant(), now()))
        {
            result.end = WatchEnd::Failed;
            result.failure = std::move(failure);
            abort_with(result);
            return result;
        }
        if (options.deadline && now() >= *options.deadline)
        {
            result.end = WatchEnd::TimedOut;
            abort_with(result);
            return result;
        }
    }
    result.end = WatchEnd::Ended;
    return result;
}

WatchResult watch_simulation(SimulationController& controller, const WatchOptions& options)
{
    WatchTarget target;
    target.wait_until_finished = [&controller](std::chrono::milliseconds timeout) { return controller.wait_until_finished(timeout); };
    target.left_participant = [&controller] { return FailureCheck::left_participant(controller); };
    target.stop = [&controller](std::string_view reason) { controller.stop(reason); };
    target.abort = [&controller] { controller.abort(); };
    return watch_simulation(target, options);
}

} // namespace vehicle_sil
