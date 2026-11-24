// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch serve: NDJSON adapter over ExecutionRuntime (execution_runtime.hpp) for one
// launcher process that runs one run after the other, each as `run` would (launch::run_file()),
// each with its own run ID, stop requests, controls and window of the events, and nothing of a
// run kept for the next. Internal; the commands, the events and the exit codes:
// docs/simulation-run.md and docs/simulation-events.md.
#pragma once

#include "vehicle_sil/launch/command_input.hpp"
#include "vehicle_sil/launch/events.hpp"
#include "vehicle_sil/launch/execution_runtime.hpp"
#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/runner/stop_requests.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace vehicle_sil::launch {

/// How a run of serve is run: launch::run_file() unless a test gives another (it gets the run's
/// request, observers and controls and window exactly as run_file() would).
using ServeRunFunction = ExecutionRuntimeOptions::RunFunction;

struct ServeOptions
{
    /// The launcher's request, for prepare_execution_run(): RunHost (config, limits, control, log_dir
    /// as the base directory, stdin_commands true) and no intent.
    RunRequest base{};
    /// new_launcher_instance(), as hello says it.
    std::string instance{};
    /// Empty: run_file().
    ServeRunFunction run{};
    /// The name of a stop request's source as the result's interrupted_by says it (a signal's name,
    /// a console event's); empty: runner::stop_source_name(), "interrupt" for any other.
    std::function<std::string(int source)> source_name{};
    /// How long run() waits for the events to be written when the launcher ends while idle.
    std::chrono::milliseconds flush_limit{5000};
    /// Injected by caller (e.g. tools/launch/main.cpp) so libs/launch never links vehicle_sil::trace.
    bool trace_enabled = false;
    /// Exit after 1 Run's result(launcher_ends: true) is written (`serve --once`).
    bool single_run = false;
};

/// How the launcher ends (Serve::run()).
using ServeEnd = ExecutionEnd;

/// Thin NDJSON adapter over ExecutionRuntime for `vehicle_sil_launch serve`.
class Serve
{
public:
    /// `events` and `end` outlive this object.
    Serve(EventStream& events, LauncherEnd& end, ServeOptions options);
    ~Serve();
    Serve(const Serve&) = delete;
    Serve& operator=(const Serve&) = delete;

    /// A line of standard input (the reader's thread): a run command installs a run if the launcher
    /// is ready (else busy, or launcher-ending); any other command needs "run" and goes to that run's
    /// controls if it is the current run (else run-required, run-changed); a line that is no command
    /// is refused. Every refusal is the launcher's own event "command".
    void command(const ServeCommand& line);
    /// Standard input has ended: the launcher ends; the current run, if any, is stopped once
    /// (stop_source::kEndOfInput, with its "command" event of source "eof").
    void end_of_input();
    /// The reader of the events has gone (EventStream::OnClosed): as end_of_input(), source
    /// stop_source::kOutputClosed.
    void output_closed();
    /// The main thread: runs every run handed to it until the launcher ends.
    /// 0 when standard input ended while no run was under way (or after 1 Run in single_run mode);
    /// kExitInterrupted when a signal, the end of input during a run or a closed output ended it;
    /// kExitContaminated (at_once) after a contaminated run that no end request came before.
    ServeEnd run();

private:
    class Sink;
    std::unique_ptr<Sink> sink_;
    std::unique_ptr<ExecutionRuntime> runtime_;
};

} // namespace vehicle_sil::launch
