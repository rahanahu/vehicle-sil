// SPDX-License-Identifier: MIT
//
// What run_file() tells while a run lasts: runner::RunEvents extended with the launch-level
// events emitted by run_file() and its controls (RunEventSink). EventStream (events.hpp) is one
// implementation; in-process callers may provide another. Internal.
#pragma once

#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/runner/command.hpp"
#include "vehicle_sil/runner/run_controls.hpp"
#include "vehicle_sil/runner/run_events.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace vehicle_sil::launch {

/// Transport-agnostic per-run event sink (RunLent::events).
/// Extends runner::RunEvents with the launch-level events emitted by run_file().
/// Threads & non-blocking contract: same as runner::RunEvents (calls may arrive from several
/// threads, some under RunOutput's mutex; implementations must not block or call back into the run).
class RunEventSink : public runner::RunEvents
{
public:
    RunEventSink() = default;
    RunEventSink(const RunEventSink&) = delete;
    RunEventSink& operator=(const RunEventSink&) = delete;
    ~RunEventSink() override = default;

    virtual void phase(runner::Phase phase) = 0;
    virtual void check(const Checked& report) = 0;
    virtual void command(const runner::Command& command, std::string_view source) = 0;
    virtual void state(const runner::RunState& state) = 0;
    virtual void enqueue_result(std::string /*members*/, bool /*seal*/) {}
    virtual bool wait_result(const std::function<bool()>& /*give_up*/, bool /*seal*/) { return true; }
};

} // namespace vehicle_sil::launch
