// SPDX-License-Identifier: MIT
//
// What ParticipantRuntime::start() promises when it returns, with real SIL Kit participants
// (in-process registry): state() says Running, not a state before it.
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <chrono>
#include <string>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
namespace orch = SilKit::Services::Orchestration;
using vehicle_sil::unit::InProcessRegistry;

} // namespace

TEST_CASE(participant_start_without_virtual_time_returns_once_the_state_is_running)
{
    // Without time synchronisation, start() learns of the start from SIL Kit's starting handler,
    // which runs before SIL Kit sets Running. The gap is short: look several times.
    InProcessRegistry registry;
    for (int i = 0; i < 10; ++i)
    {
        ParticipantRuntime runtime(ParticipantRuntimeOptions{"Starter" + std::to_string(i), registry.uri});
        runtime.start(10s);
        const auto state = runtime.state();
        if (state != orch::ParticipantState::Running)
        {
            throw vehicle_sil::unit::Failure("start() returned in state " + orch::to_string(state) + " (round "
                                             + std::to_string(i) + "), expected Running");
        }
        runtime.stop("test");
    }
}
