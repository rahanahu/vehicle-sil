// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch run: the order of a run from its setup to the end of the participants
// (Runner::run()), and run_file(). The parts of a run are in the files beside this one:
//
//   run_context.hpp      RunContext       what the run is given; never written
//   run_output.*         RunOutput        the lines, the log files, the events
//   run_control.*        RunControl       the hub, the participants' control links
//   run_participants.*   RunParticipants  the participants' processes: start, look, end
//   run_notes.*          RunNotes         run --events: what has been written already
//   run_scenario.*       RunScenario      run --scenario: the engine between hub, gate and result
//   run_start.*          RunStart, RunStartWindow, StartShared   the wait, the start window, the limits
//   run_start_limit.hpp  StartLimit       when the start is given up
//   run_close.hpp        close_within()   the wait for the end of the SIL Kit objects, with a limit
//   run_watch.*          RunWatch         the watch of the running simulation
//   time_commands.cpp    TimeCommands     pause, resume, step, run_until, speed
//   run_assess.cpp, run_format.cpp, run_words.hpp   the findings, the result, its text and JSON
//
// The threads of a run, and what each may touch:
// - The main thread (run_file()'s caller): everything, and alone the RunOutcome, RunParticipants,
//   RunNotes, RunWatch and RunStart's own members.
// - The start window's thread (one that SimulationController::start() makes for the window, not
//   its SIL Kit thread; RunStartWindow::run()): the StartShared, RunScenario::begin(),
//   RunControl::expected_agents() and hub(), TimeCommands::hold_before_start(), and RunOutput.
// - The scenario's thread (RunScenario::loop()): the scenario's engine, from RunScenario::start()
//   until the main thread joins it, and RunOutput.
// - The controller's SIL Kit thread: the hub's tap of the scenario (RunScenario's samples).
// - Each participant's reader thread (process::Process): RunOutput::on_line().
// - The closing thread (Runner::close_connection(), at the end of run()): destroys the hub's
//   transport, the controller and the registry, and touches nothing else of the run. The main
//   thread waits for it with a limit.
// - Whoever applies a time command (the thread that reads commands, a control server's):
//   TimeCommands.
//
// The locks, in the only orders they are taken together (a lock not named under another is never
// taken while that one is held):
// - TimeCommands' mutex, then the event stream's, the controller's time gate's, or what
//   RunOptions' callbacks take (on_detach, the `accepted` of a stop).
// - RunOutput's mutex, then the event stream's (and RunOptions::write_line).
// - StartLimit's mutex, StartShared::found_mutex, RunScenario's samples' and error mutexes:
//   nothing under them.
//
// A control server (RunOptions::serve_control) has threads of its own. Of the run they touch the
// hub (thread safe; its subscriptions are bounded and never make the simulation wait) and the
// TimeCommands. With a scenario the server only shows the run: its clients' calls that would
// write a variable or move the time are refused in the server, before they reach either.
#include "vehicle_sil/launch/run.hpp"

#include "run_close.hpp"
#include "run_context.hpp"
#include "run_control.hpp"
#include "run_notes.hpp"
#include "run_output.hpp"
#include "run_participants.hpp"
#include "run_scenario.hpp"
#include "run_start.hpp"
#include "run_watch.hpp"

#include "vehicle_sil/launch/events.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <silkit/config/IParticipantConfiguration.hpp>
#include <silkit/vendor/CreateSilKitRegistry.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::launch {

std::string detail::uri_for_participants(std::string uri)
{
    for (const auto& [any, loopback] : {std::pair<std::string, std::string>{"silkit://0.0.0.0:", "silkit://127.0.0.1:"},
                                        std::pair<std::string, std::string>{"silkit://[::]:", "silkit://[::1]:"}})
    {
        if (uri.rfind(any, 0) == 0)
        {
            return loopback + uri.substr(any.size());
        }
    }
    return uri;
}

namespace {

using detail::one_line;
using detail::RunContext;
using detail::RunControl;
using detail::RunNotes;
using detail::RunOutput;
using detail::RunParticipants;
using detail::RunScenario;
using detail::RunStart;
using detail::RunWatch;
using detail::uri_for_participants;

/// One run: its parts, and the order of its phases (run()). The registry, the controller and the
/// hub's transport live in run() only; the parts are given the controller in the calls that need
/// it.
class Runner
{
public:
    Runner(const SimulationConfig& config, const CheckReport& check, std::filesystem::path base, const RunOptions& options) :
      own_commands_(options.time_commands == nullptr ? std::make_unique<TimeCommands>(options.start_paused) : nullptr),
      ctx_{config, check, options, check.scenario && check.scenario->scenario ? &*check.scenario->scenario : nullptr,
           own_commands_ ? *own_commands_ : *options.time_commands},
      out_(ctx_), control_(ctx_, out_), notes_(ctx_, participants_), scenario_(ctx_, out_, control_),
      start_(ctx_, out_, control_, notes_, scenario_, participants_, outcome_),
      watch_(ctx_, out_, notes_, scenario_, start_, participants_, outcome_), participants_(ctx_, out_, outcome_, std::move(base))
    {
        outcome_.start_timeout_ms = config.start_timeout_ms;
        outcome_.stop_timeout_ms = config.stop_timeout_ms;
        outcome_.control_hello_timeout_ms = options.control_hello_timeout.count();
        outcome_.stop_at_ms = config.stop_at_ms;
        const std::optional<double> speed = options.speed ? *options.speed : config.speed;
        if (ctx_.scenario != nullptr)
        {
            outcome_.scenario = scenario_not_run(*ctx_.scenario);
            outcome_.scenario->file = check.scenario->file;
        }
        // With a scenario the step does not follow the speed: the same steps at every speed.
        outcome_.timing = detail::plan_time_step(config, check, speed, ctx_.scenario != nullptr);
        outcome_.timing.speed = speed;
        outcome_.timing.coarse_step_checked =
            std::any_of(check.findings.begin(), check.findings.end(), [](const Finding& f) { return f.code == "coarse-time-step"; });
        outcome_.timing.late_tolerance_ms = late_tolerance_ms(config);
        if (options.timeout)
        {
            outcome_.timeout_ms = options.timeout->count();
        }
        for (const auto& p : config.participants)
        {
            ParticipantRun run;
            run.name = p.name;
            run.required = p.required;
            run.describe = p.describe;
            run.enabled = p.enabled;
            run.manual = p.manual;
            run.line = p.line;
            outcome_.participants.push_back(std::move(run));
        }
    }

    RunOutcome run()
    {
        // Before the guard below, so that they go after it.
        std::unique_ptr<SilKit::Vendor::Vector::ISilKitRegistry> registry;
        std::unique_ptr<SimulationController> controller;
        // The hub's end of the ECUs' control agents, on the controller's participant: after the
        // controller, so that it goes before it.
        std::unique_ptr<control_silkit::HubTransport> transport;
        // Whatever happens, the time commands end with the run (they are refused from then on),
        // and before the controller they are given to goes: the thread that reads the commands
        // may apply one at any time.
        struct Detach
        {
            TimeCommands& commands;
            ~Detach() { commands.detach(); }
        } const detach{ctx_.commands};
        // The scenario's thread and its tap on the hub end with the run, before the controller
        // and the transport go (declared after them).
        struct EndScenario
        {
            Runner& runner;
            ~EndScenario() { runner.scenario_.end(); }
        } const endScenario{*this};
        if (ctx_.interrupts() > 0)
        {
            outcome_.interrupted_by = ctx_.interrupt_name();
            return outcome_;
        }
        out_.event([](EventStream& e) { e.phase(Phase::Starting); });
        if (!out_.open_logs(outcome_))
        {
            return outcome_;
        }
        if (ctx_.options.serve_control)
        {
            try
            {
                const ControlServerInfo server = ctx_.options.serve_control(control_.hub());
                out_.event([&server](EventStream& e) { e.control_server(server.address, server.tls, server.credentials); });
                out_.say("control server at " + server.address);
            }
            catch (const std::exception& e)
            {
                outcome_.setup_error = "the control server cannot be started: " + one_line(e.what());
                return outcome_;
            }
        }
        std::string uri;
        std::string listened;
        try
        {
            registry = SilKit::Vendor::Vector::CreateSilKitRegistry(
                SilKit::Config::ParticipantConfigurationFromString(std::string(default_registry_configuration())));
            // A participant started by hand gets a command that stays the same from run to run.
            const bool fixed = !ctx_.config.registry_uri && waits_for_manual(ctx_.config);
            const std::string listen =
                ctx_.config.registry_uri.value_or(fixed ? std::string(kManualRegistryUri) : std::string("silkit://127.0.0.1:0"));
            try
            {
                listened = registry->StartListening(listen);
                uri = uri_for_participants(listened);
            }
            catch (const std::exception& e)
            {
                throw std::runtime_error(
                    "the registry cannot listen at " + listen + ": " + one_line(e.what()) +
                    (ctx_.config.registry_uri ? " (does another registry run there? choose another port in registry_uri, or "
                                            "leave it out for a free port)"
                     : fixed ? " (a run with a participant you start yourself (manual) listens there if the file has no "
                               "registry_uri, so that its command stays the same; does another registry run there? write "
                               "another port in registry_uri)"
                             : ""));
            }
            out_.event([&uri](EventStream& e) { e.registry(uri); });
            out_.say("registry at " + uri);
            if (!is_loopback_registry_uri(listen))
            {
                // On [::]: IPv6, and IPv4 too where such a socket takes it.
                const bool any6 = listened.rfind("silkit://[::]:", 0) == 0;
                const bool ipv4 = !any6 || detail::ipv6_any_takes_ipv4();
                out_.say(detail::other_computers_line(listened, detail::host_addresses(ipv4, any6), any6 && !ipv4));
            }
            SimulationControllerOptions c;
            c.participant_name = std::string(kControllerName);
            c.registry_uri = uri;
            // A registry that other computers may reach: they must reach the controller too.
            c.listen_on_network = !is_loopback_registry_uri(listen);
            for (const auto& p : ctx_.config.participants)
            {
                if (p.required && p.enabled)
                {
                    c.required_participants.push_back(p.name);
                }
            }
            if (ctx_.config.stop_at_ms)
            {
                c.stop_at = std::chrono::milliseconds(*ctx_.config.stop_at_ms);
            }
            // The time gate, always (also as fast as possible): one way for every run. The step
            // divides stop_at_ms (detail::plan_time_step()), so the simulation stops at stop_at_ms
            // itself.
            TimeControlOptions time;
            time.step = std::chrono::milliseconds(outcome_.timing.time_step_ms);
            time.late_tolerance = std::chrono::milliseconds(outcome_.timing.late_tolerance_ms);
            c.time_control = time;
            // The controller's stop may wait that long by itself for a participant that does not
            // answer; beyond it, stop_timeout_ms as for every other wait of the end, but never
            // less than kCloseMargin: a small stop_timeout_ms must not cut a close that is going on.
            close_limit_ = c.shutdown_timeout + std::max(std::chrono::milliseconds(ctx_.config.stop_timeout_ms), kCloseMargin);
            controller = std::make_unique<SimulationController>(std::move(c));
            // Before the participants start and before the controller does: SIL Kit connects the
            // publisher of the commands and the subscribers with every participant that starts.
            transport = std::make_unique<control_silkit::HubTransport>(control_.hub(), controller->runtime());
            controller->set_start_window([this, t = transport.get(), c = controller.get()](const StartWindow& window) {
                start_.window(*t, *c, window);
            });
            ctx_.commands.attach(*controller, outcome_.timing.time_step_ms, outcome_.timing.speed);
            if (ctx_.scenario != nullptr)
            {
                // The scenario decides at every step of the controller, from the one at 0 on.
                controller->guard_from(SimTime{});
                scenario_.tap(std::chrono::milliseconds(outcome_.timing.time_step_ms));
            }
        }
        catch (const std::exception& e)
        {
            outcome_.setup_error = one_line(e.what());
            return outcome_;
        }

        bool ran = false;
        if (participants_.start_children(uri))
        {
            out_.event([](EventStream& e) { e.phase(Phase::Waiting); });
            if (start_.wait_for_start(*controller))
            {
                ran = true;
                scenario_.start(*controller, outcome_);
                interrupts_at_end_ = watch_.watch(*controller);
                control_.note_late_values(outcome_, *transport);
            }
        }
        // No time command from here on, also if the simulation did not start: a stop accepted from
        // now on would only cut short the end of the participants.
        ctx_.commands.detach();
        if (!ran)
        {
            // As RunWatch::watch() does: an interrupt from here on comes while the participants end (a stop
            // accepted before the detach counts as one of the run).
            interrupts_at_end_ = ctx_.interrupts();
        }
        // What is known for good when the simulation is over (why it ended), before the
        // participants end.
        publish_findings(false);
        out_.event([](EventStream& e) { e.phase(Phase::Stopping); });
        participants_.end_children(ran || start_.before_start_failed(), interrupts_at_end_);
        participants_.end_manual(*controller, ran || start_.before_start_failed(), interrupts_at_end_);
        // The agents have said their last word and waited for this side's answer (their processes
        // have ended); what is still open in the hub (of a process that was killed, or whose last
        // word did not arrive within its wait) is settled here.
        close_connection(std::move(transport), std::move(controller), std::move(registry));
        return outcome_;
    }

    /// run --events: the run's findings that have not been written yet. Before the participants
    /// have ended (`final` false) only those that their end cannot change (settled_findings()):
    /// what is written as an event is in the result as it was written.
    void publish_findings(bool final) { notes_.publish_findings(outcome_, final); }

private:
    /// The end of what the run has of SIL Kit: destroys the hub's transport, the controller and
    /// the registry, in this order, and waits for that close_limit_ at most (one more interrupt
    /// ends the wait). SIL Kit joins the thread of its connections when a participant is
    /// destroyed, and a connection that does not close would keep the run from its result for
    /// ever (seen on Windows before its connections were TCP ones, with a participant whose
    /// threads were suspended: docs/dependencies.md). So the three are destroyed on a thread of
    /// their own (detail::close_within()); if it has not ended in time, it is left with them
    /// (RunOutcome::connection_not_closed_ms), and the run goes on to its result.
    void close_connection(std::unique_ptr<control_silkit::HubTransport> transport, std::unique_ptr<SimulationController> controller,
                          std::unique_ptr<SilKit::Vendor::Vector::ISilKitRegistry> registry)
    {
        struct Closing
        {
            // In the order of their making: destroyed by hand below, in the reverse order.
            std::unique_ptr<SilKit::Vendor::Vector::ISilKitRegistry> registry;
            std::unique_ptr<SimulationController> controller;
            std::unique_ptr<control_silkit::HubTransport> transport;
        };
        const auto closing = std::make_shared<Closing>();
        closing->registry = std::move(registry);
        closing->controller = std::move(controller);
        closing->transport = std::move(transport);
        const int interrupts = ctx_.interrupts();
        // The thread keeps the three (the shared pointer). What they use of this run stays too if
        // it is left: run_file() never destroys the Runner of such a run.
        const auto left = detail::close_within(
            [closing] {
                closing->transport.reset();
                closing->controller.reset();
                closing->registry.reset();
            },
            close_limit_, ctx_.options.poll, [this, interrupts] { return ctx_.interrupts() > interrupts; });
        if (!left)
        {
            return;
        }
        outcome_.connection_not_closed_ms = left->waited.count();
        outcome_.connection_close_interrupted = left->interrupted;
        out_.say("the launcher's own connection to the simulation (SIL Kit) has not closed " + std::to_string(left->waited.count()) +
                 " ms after the participants ended" +
                 (left->interrupted ? " and the launcher was asked to stop once more" : std::string{}) +
                 ": the launcher ends without closing it");
    }

    // The order matters. Each part keeps references to those it uses (all declared here, so they
    // outlive run()); they are destroyed in the reverse order.
    std::unique_ptr<TimeCommands> own_commands_; ///< without RunOptions::time_commands
    const RunContext ctx_;
    RunOutcome outcome_; ///< the main thread's: the other threads of the run never see it
    RunOutput out_;
    RunControl control_; ///< before scenario_: the scenario's engine and port use its hub
    RunNotes notes_;
    RunScenario scenario_;
    RunStart start_;
    RunWatch watch_;
    int interrupts_at_end_ = 0; ///< ctx_.interrupts() when the simulation had ended
    static constexpr std::chrono::milliseconds kCloseMargin{5000};
    std::chrono::milliseconds close_limit_{0}; ///< close_connection(): how long the SIL Kit objects may take to go
    // Last, so that it goes first: a Process ends its reader thread, which uses the output (out_),
    // also when run() leaves with an exception.
    RunParticipants participants_;
};

} // namespace

RunReport run_file(const std::filesystem::path& path, const std::string& shown_as, const RunOptions& options)
{
    const auto event = [&options](auto emit) {
        if (options.events != nullptr)
        {
            emit(*options.events);
        }
    };
    RunReport report;
    report.strict = options.check.strict;
    event([](EventStream& e) { e.phase(Phase::Checking); });
    report.check = check_file(path, shown_as, options.check);
    if (options.speed)
    {
        // The check's coarse-time-step is for the file's speed: the run reports its own.
        std::erase_if(report.check.findings, [](const Finding& f) { return f.code == "coarse-time-step"; });
    }
    event([&report](EventStream& e) { e.check(report.check); });
    if (!report.check.config || !report.check.passed())
    {
        event([](EventStream& e) { e.phase(Phase::Ended); });
        return report;
    }
    // The participants that will not start, before anything starts, in the file's order.
    event([&report](EventStream& e) {
        for (const auto& p : report.check.config->participants)
        {
            if (!p.enabled)
            {
                e.participant(p.name, ParticipantState::Disabled);
            }
        }
    });
    const std::filesystem::path base = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    auto runner = std::make_unique<Runner>(*report.check.config, report.check, base, options);
    report.run = runner->run();
    report.findings = assess_run(*report.run);
    runner->publish_findings(true);
    event([](EventStream& e) { e.phase(Phase::Ended); });
    if (report.run->connection_not_closed_ms)
    {
        // A thread of the run still closes the connection, and may use the parts of the run for
        // as long as the process lives: they are never destroyed. The participants' processes
        // have ended, and their logs are written line by line.
        static_cast<void>(runner.release());
    }
    return report;
}

} // namespace vehicle_sil::launch
