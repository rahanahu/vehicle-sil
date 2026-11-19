// SPDX-License-Identifier: MIT
//
// Links vehicle_sil::runner alone (not the launcher's library) and uses what its public headers
// declare: if a definition of them is in the launcher's library, this does not link. The program
// is not run.
#include "vehicle_sil/runner/detail/runner.hpp"
#include "vehicle_sil/runner/run_outcome.hpp"

#include <cstdio>
#include <string>
#include <utility>

int main(int argc, char**)
{
    using namespace vehicle_sil::runner;
    if (argc > 100) // never: only the references matter
    {
        RunOutcome outcome;
        std::printf("%zu %s %s %zu\n", assess_run(outcome).size(), control_link_name(ControlLink::None),
                    std::string(stop_request_name()).c_str(), ended_by_force(outcome).size());
        std::printf("%s\n", end_reason_name(end_reason(outcome)));
        RunPlan plan;
        RunControls* controls = nullptr; // never reached
        detail::Runner runner(std::move(plan), RunObservers{}, nullptr, *controls);
        (void)runner.run(detail::ControlServerStart{});
    }
    return 0;
}
