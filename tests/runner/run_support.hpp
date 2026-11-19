// SPDX-License-Identifier: MIT
//
// The participants and runs that the tests of the run's judgement (assess_run()) build: what the
// run saw, made up. Shared by the run's own tests (tests/runner) and by the tests of the report
// of the launcher's library (tests/launch), which read the same runs through the outputs.

#pragma once

#include "launch_test_util.hpp"

#include "vehicle_sil/runner/detail/run_detail.hpp"

#include "vehicle_sil/config/detail/check_words.hpp"
#include "vehicle_sil/runner/detail/run_close.hpp"
#include "run_connection.hpp"
#include "run_notes.hpp"
#include "vehicle_sil/runner/detail/run_samples.hpp"
#include "vehicle_sil/runner/detail/run_start_limit.hpp"
#include "silkit_test_util.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <initializer_list>
#include <vector>

#ifndef _WIN32
#include <csignal>
#endif

namespace vehicle_sil::runner::test::run_support {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::runner;
using vehicle_sil::config::test::check_text;
using vehicle_sil::config::test::lines;
using vehicle_sil::process::ExitStatus;

const std::string kStop(stop_request_name());

inline ParticipantRun participant(const std::string& name, std::size_t line, bool required = true)
{
    ParticipantRun p;
    p.name = name;
    p.line = line;
    p.required = required;
    return p;
}

inline ParticipantRun exited(ParticipantRun p, int code)
{
    p.ending = Ending::Exited;
    p.exit = ExitStatus{code, std::nullopt};
    return p;
}

/// A run of two required ECUs that reached stop_at_ms and both exited with 0.
inline RunOutcome passed_run()
{
    RunOutcome o;
    o.end.simulation = SimulationEnd::ReachedStopAt;
    o.end.stop_at_ms = 1000;
    o.start.timeout_ms = 30000;
    o.end.stop_timeout_ms = 10000;
    o.participants = {exited(participant("Engine", 5), 0), exited(participant("Brake", 7), 0)};
    o.timing.time_step_ms = 10;
    o.timing.late_tolerance_ms = 10;
    o.timing.sim_ms = 1000;
    o.timing.wall_ms = 40;
    return o;
}

/// passed_run() with the bridge of --connect for ETH (line 3) at eth0, which ended as `bridge` says.
inline RunOutcome connected_run(ParticipantRun bridge)
{
    RunOutcome o = passed_run();
    bridge.name = "bridge.ETH";
    bridge.line = 3;
    bridge.required = false;
    bridge.for_network = "ETH";
    o.participants.push_back(std::move(bridge));
    o.outside.push_back(RunOutside{"ETH", "eth0", "bridge.ETH", std::nullopt, std::nullopt});
    return o;
}

inline ParticipantRun stopped_bridge(int code)
{
    ParticipantRun p;
    p.ending = Ending::Stopped;
    p.exit = ExitStatus{code, std::nullopt};
    return p;
}

/// A run that `by` (of `kind`) stopped while the simulation ran.
inline RunOutcome stopped_run(std::string by, vehicle_sil::runner::StopKind kind)
{
    RunOutcome o = passed_run();
    o.end.simulation = SimulationEnd::Interrupted;
    o.end.interrupted_by = std::move(by);
    o.end.interrupted_kind = kind;
    return o;
}

} // namespace vehicle_sil::runner::test::run_support
