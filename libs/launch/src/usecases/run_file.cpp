// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch run as a use case: run_file() checks the file, makes the run's controls and
// its control server when the caller lends none, drives a Runner (detail/runner.hpp) through its two
// stages, and makes the timeline file of what the run left. What a run is given, the threads
// and the control server's lifetime: the comment at the top of run.cpp.
#include "vehicle_sil/launch/run.hpp"

#include "endpoints/run_control_server.hpp"

#include "vehicle_sil/runner/detail/run_detail.hpp"
#include "vehicle_sil/runner/detail/runner.hpp"
#include "vehicle_sil/launch/events.hpp"
#include "vehicle_sil/launch/timeline_format.hpp"

#include "vehicle_sil/process/paths.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::launch {

namespace {

using runner::detail::ControlServerStart;
using runner::detail::Runner;

/// The controls of a run that was given none (RunLent::controls): made by run_file() for that
/// run, on the caller's stop requests if there are any. Its phases go to RunLent::events.
struct OwnControls
{
    static runner::RunControls::Observers observers(EventStream* events)
    {
        runner::RunControls::Observers out;
        if (events != nullptr)
        {
            out.on_phase = [events](runner::Phase phase) { events->phase(phase); };
            out.on_state = [events](const runner::RunState& state) { events->state(state); };
        }
        return out;
    }

    OwnControls(runner::StopRequests* stops, const RunIntent& intent, const runner::RunObservers& given, EventStream* events,
                bool control_server) :
      controls(stops != nullptr ? *stops : none, intent.start_paused, observers(events), intent.scenario.has_value())
    {
        if (given.interrupt_name)
        {
            controls.set_stop_name(given.interrupt_name);
        }
        controls.set_control_server(control_server);
    }

    runner::StopRequests none; ///< without RunLent::stops: nothing ever asks
    runner::RunControls controls;
};

} // namespace

runner::RunChoices run_choices(const RunRequest& request)
{
    runner::RunChoices choices;
    choices.speed = request.intent.speed;
    choices.start_paused = request.intent.start_paused;
    choices.strict = request.intent.strict;
    choices.log_dir = request.host.log_dir;
    choices.timeline = request.host.timeline;
    choices.timeout = request.host.timeout;
    choices.stop_at_ms = request.intent.stop_at_ms;
    choices.late_tolerance_ms = request.intent.late_tolerance_ms.value_or(config::kDefaultLateToleranceMs);
    choices.start_timeout_ms = request.host.start_timeout_ms;
    choices.stop_timeout_ms = request.host.stop_timeout_ms;
    return choices;
}

RunReport run_file(const RunRequest& request, const runner::RunObservers& observers, const RunLent& lent, const runner::RunLimits& limits,
                   const CheckOptions& check_options)
{
    if (request.host.control && lent.controls == nullptr && lent.control_endpoint == nullptr)
    {
        throw std::invalid_argument(
          "RunHost::control is set, but RunLent::controls is null: the control server's time operations go to the "
          "caller's controls, whose rules also say what a client may do during a scenario; make a RunControls for "
          "this run (with RunControls::set_control_server(true)) and lend it, or leave `control` empty.");
    }
    // The run has a control server: its own, made from the request, or the caller's.
    const bool control_server = request.host.control.has_value() || lent.control_endpoint != nullptr;
    if (lent.controls != nullptr)
    {
        if (lent.stops != nullptr && lent.stops != &lent.controls->stops())
        {
            throw std::invalid_argument(
              "RunLent: both `controls` and `stops` are set, and the run reads only the stop requests of `controls`; "
              "make the RunControls on the same StopRequests and leave `stops` null.");
        }
        if (lent.events != nullptr && (!lent.controls->has_phase_observer() || !lent.controls->has_command_observer() ||
                                       !lent.controls->has_state_observer()))
        {
            throw std::invalid_argument(
              "RunLent::controls and RunLent::events are both set, but the controls have no observer for the "
              "events the run leaves to them; set RunControls::Observers::on_phase to call EventStream::phase(), "
              "Observers::on_command to call EventStream::command() and Observers::on_state to call "
              "EventStream::state(), or leave `events` null.");
        }
        const bool scenario = request.intent.scenario.has_value();
        if (lent.controls->start_paused() != request.intent.start_paused || lent.controls->scenario() != scenario)
        {
            const auto yes_no = [](bool b) { return std::string(b ? "true" : "false"); };
            // What goes wrong, for each that differs.
            std::string why;
            if (lent.controls->scenario() != scenario)
            {
                why = "while a scenario plays, a control server's client must not write variables";
            }
            if (lent.controls->start_paused() != request.intent.start_paused)
            {
                why += std::string(why.empty() ? "" : "; ") +
                       "only a run with --start-paused takes a step before anything has been paused";
            }
            // Without controls the run makes its own, unless a control server of its own needs the caller's.
            const bool may_leave_null = !request.host.control || lent.control_endpoint != nullptr;
            throw std::invalid_argument(
              "RunLent::controls were made with start_paused " + yes_no(lent.controls->start_paused()) + " and scenario " +
              yes_no(lent.controls->scenario()) + ", but the request has start_paused " +
              yes_no(request.intent.start_paused) + " and scenario " + yes_no(scenario) +
              " (RunIntent): the controls' rules would answer for another run (" + why +
              "); make the RunControls with RunIntent::start_paused and RunIntent::scenario.has_value()" +
              (may_leave_null ? ", or leave `controls` null." : "."));
        }
        if (lent.controls->control_server() != control_server)
        {
            throw std::invalid_argument(
              control_server
                ? "RunLent::controls say that the run has no control server (RunControls::set_control_server()), but "
                  "the request has one (RunHost::control or RunLent::control_endpoint): the state would refuse every "
                  "write with no-control-server while the server's clients can write; call "
                  "RunControls::set_control_server(true) before the run."
                : "RunLent::controls say that the run has a control server (RunControls::set_control_server(true)), "
                  "but the request has none (neither RunHost::control nor RunLent::control_endpoint): the state would "
                  "offer writes that nothing can send; call RunControls::set_control_server(false), or leave "
                  "`controls` null.");
        }
    }
    const auto event = [&lent](auto emit) {
        if (lent.events != nullptr)
        {
            emit(*lent.events);
        }
    };
    // The controls of this run: the caller's, or its own. Declared before the Runner, which
    // borrows them: they outlive it.
    const std::unique_ptr<OwnControls> own =
        lent.controls == nullptr ? std::make_unique<OwnControls>(lent.stops, request.intent, observers, lent.events, control_server)
                              : nullptr;
    runner::RunControls& controls = own ? own->controls : *lent.controls;
    RunReport report;
    report.strict = request.intent.strict;
    // The control server of this run: made here from the request, or the caller's. Before the
    // check and its phase: a request for a server that is wrong is a usage error, and nothing is
    // checked. The report holds the run's own from here on, for the caller to let go after the
    // result. Declared after the controls, which the server uses, and before the Runner: on every
    // way out, also with an exception, it is ended after the Runner has gone.
    std::shared_ptr<ControlEndpoint> own_server;
    if (request.host.control && lent.control_endpoint == nullptr)
    {
        detail::ControlServerOfRun made = detail::make_control_server_of_run(*request.host.control, controls);
        if (!made.findings.empty())
        {
            report.check.file = request.host.config;
            report.check.usage = true;
            report.check.findings = std::move(made.findings);
            return report;
        }
        own_server = std::move(made.server);
        report.ended_control_server = own_server;
    }
    const struct EndServer
    {
        ControlEndpoint* server;
        ~EndServer()
        {
            if (server != nullptr)
            {
                server->end(); // every call of a client returns, none comes any more
            }
        }
    } end_own_server{own_server.get()};
    ControlEndpoint* const server_of_run = lent.control_endpoint != nullptr ? lent.control_endpoint : own_server.get();
    controls.set_phase(runner::Phase::Checking);
    // Whatever leaves run_file() by an exception from here on (an internal error of the launcher),
    // the run ends as every run does before the caller writes the result: the time commands are
    // refused (each that waited with its command event), then the phase `ended`. Declared before
    // the Runner, so this comes after the Runner has gone (its participants have ended), and
    // before the control server ends.
    const struct EndOnException
    {
        runner::RunControls& controls;
        ~EndOnException()
        {
            if (controls.phase() == runner::Phase::Ended)
            {
                return; // every way out that returns has ended the run
            }
            try
            {
                controls.set_phase(runner::Phase::Ended);
            }
            catch (...)
            {
                // The exception that leaves run_file() is the one to report; nothing more is told.
            }
        }
    } end_on_exception{controls};
    // The check judges the time by what this run does (its speed and --start-paused, which it
    // reads in the request). A run reads the whole, whatever the caller's options say.
    CheckOptions check = check_options;
    check.file_only = false;
    check.refuse_fixed_registry = lent.refuse_fixed_registry;
    // The controls get the time lock as soon as the configuration has been read, before any
    // --describe: the time commands that came until then wait for it, and are decided there.
    report.check = check_file(request, check, [&controls](const config::TimeLock& lock) { controls.set_time_lock(lock); });
    event([&report](EventStream& e) { e.check(report.check); });
    if (!report.check.passed())
    {
        // Nothing starts: the run ends as every run does. The phase `ended` refuses the time
        // commands from here on, also those that still wait for a lock that never came (the
        // configuration could not be read), each with its command event.
        controls.set_phase(runner::Phase::Ended);
        return report;
    }
    // The participants that will not start, before anything starts, in the file's order.
    event([&report](EventStream& e) {
        for (const auto& p : report.check.config->participants)
        {
            if (!p.enabled)
            {
                e.participant(p.name, runner::ParticipantState::Disabled);
            }
        }
    });
    const std::filesystem::path path = process::path_from_utf8(request.host.config);
    const std::filesystem::path base = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    // Destroyed when run_file() returns, also if the run's closing thread was left at its limit
    // (OutcomeClose::not_closed_ms): that thread uses nothing of the Runner.
    // What the run is given, in three kinds: the plan (a value, settled here), the observers (the
    // caller's functions) and what it borrows (the controls; the stream of the events and the
    // control server stay here).
    // The run tells through the interface it knows (RunEvents), which the stream of the events
    // implements; the stream's own members (check(), phase(), state()) are called here only.
    runner::detail::Runner runner(runner::plan_run(report.check, detail::scenario_of(report.check), base, run_choices(request), limits,
                           server_of_run != nullptr),
                  observers, lent.events, controls);
    // The control server between the Runner's two stages: after the run's files are open, before
    // the registry listens. The Runner does not know the server; it is told what became of it.
    runner::detail::ControlServerStart server;
    // What else leaves here (not a std::exception of the server's start) leaves run_file():
    // end_on_exception ends the run.
    if (const std::shared_ptr<control::Hub> hub = runner.prepare(); hub && server_of_run != nullptr)
    {
        try
        {
            server.listening = server_of_run->start(hub);
        }
        catch (const std::exception& e)
        {
            server.error = e.what();
        }
    }
    report.run = runner.run(server);
    report.findings = runner::assess_run(*report.run);
    if (runner.timeline_open())
    {
        // run --timeline: the file is made here, of the plan and of what the run left (the
        // scenario's result and the record its engine filled); the Runner only writes the text
        // to the file it opened before anything started. The file has the scenario's findings
        // of the check and of the run.
        const scenario::ScenarioCatalog catalog = scenario::scenario_catalog(report.check);
        TimelineInput input;
        input.file = report.check.file;
        input.variant = runner.plan().config().variant;
        input.params = report.check.scenario->params;
        input.time_step_ms = runner.plan().timing.time_step_ms;
        input.scenario = runner.plan().scenario();
        input.run = &*report.run->scenario.result;
        input.catalog = &catalog;
        input.record = report.run->scenario.timeline_record.get();
        input.findings = report.check.findings;
        input.findings.insert(input.findings.end(), report.findings.begin(), report.findings.end());
        if (const std::string error = runner.write_timeline(format_timeline_json(input)); !error.empty())
        {
            report.run->scenario.timeline_error = error;
            report.findings = runner::assess_run(*report.run);
        }
    }
    runner.publish_findings(true);
    controls.set_phase(runner::Phase::Ended);
    return report;
}

} // namespace vehicle_sil::launch
