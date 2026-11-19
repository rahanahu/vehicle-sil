// SPDX-License-Identifier: MIT
//
// What the tests of the controls of a run build: the state of a run as its controls put it
// together. Shared by the tests of the runner (tests/runner) and of the launcher's library (tests/launch).

#pragma once

#include "test_harness.hpp"
#include "silkit_test_util.hpp"

#include "run_pacing.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"

#include "vehicle_sil/runner/run_controls.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace vehicle_sil::runner::test::run_controls_support {



/// Controls whose states are kept as they are told; of a run with a control server unless
/// `control_server` says otherwise.
struct Told
{
    explicit Told(bool start_paused = false, bool scenario = false, bool control_server = true) :
      controls(stops, start_paused, observers(), scenario)
    {
        controls.set_control_server(control_server);
        // A run that is not connected outside: no state is told for it, and the commands are
        // decided as they come.
        controls.set_time_lock({});
    }

    RunControls::Observers observers()
    {
        RunControls::Observers out;
        out.on_state = [this](const RunState& s) { states.push_back(s); };
        return out;
    }

    StopRequests stops;
    std::vector<RunState> states;
    RunControls controls;
};

} // namespace vehicle_sil::runner::test::run_controls_support
