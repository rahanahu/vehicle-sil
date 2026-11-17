// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch run: the order of a run from its setup to the end of the participants
// (Runner::run()), and run_file(). The parts of a run are in the files beside this one:
//
//   run_context.hpp      RunContext       what the run is given; never written
//   run_output.*         RunOutput        the lines, the log files, the events
//   run_connection.hpp   ConnectionOf     the hub with the registry, the controller and the hub's
//                        transport: what the closing thread takes over at the end
//   run_control.*        RunControl       the participants' control links, their variable maps on the hub
//   run_participants.*   RunParticipants  the participants' processes: start, look, end
//   run_notes.*          RunNotes         run --events: what has been written already
//   run_samples.*        RunSamples       the hub's tap: every sample, for the scenario and the traffic
//   run_traffic.*        TrafficLatest, count_traffic()   the result's traffic, from the samples
//   run_scenario.*       RunScenario      run --scenario: the engine between hub, gate and result
//   run_start.*          RunStart, RunStartWindow, StartShared   the wait, the start window, the limits
//   run_start_limit.hpp  StartLimit       when the start is given up
//   run_close.hpp        close_within()   the wait for the end of the SIL Kit objects, with a limit
//   run_watch.*          RunWatch         the watch of the running simulation, then what is settled at its end
//   time_commands.cpp    TimeCommands     pause, resume, step, run_until, speed
//   run_assess.cpp, run_format.cpp, run_words.hpp   the findings, the result, its text and JSON
//   run_connect.cpp      with_bridges()   run --connect: the bridges as participants of the run only
//   run_outside.cpp      read_outside_counts(), OutsideNote   run --connect: the counts in a bridge's
//                        statistics lines, and when the event "outside" goes out
//   timeline.cpp         TimelineRecord   run --timeline: what the scenario's engine notes
//   timeline_format.cpp  format_timeline_json()   the file made of it (Runner::open_timeline(), write_timeline())
//
// The threads of a run, and what each may touch:
// - The main thread (run_file()'s caller): everything, and alone the RunOutcome, RunParticipants,
//   RunNotes, RunWatch and RunStart's own members.
// - The start window's thread (one that SimulationController::start() makes for the window, not
//   its SIL Kit thread; RunStartWindow::run()): the StartShared, RunScenario::begin(),
//   RunControl::expected_agents() and hub(), TimeCommands::hold_before_start(), and RunOutput;
//   it reads the peers' states (gone_before_start(): the required participants and the bridges of
//   --connect, whose mark ParticipantConfig::for_network is fixed before the run).
// - The scenario's thread (RunScenario::loop()): the scenario's engine and, with --timeline, the
//   TimelineRecord it fills, from RunScenario::start() until the main thread joins it,
//   RunSamples::take(), and RunOutput. The record is filled on this thread (and on the window's,
//   in begin()) only: never in the hub's tap, and nothing of it is touched under a lock. The
//   timeline file is the main thread's (opened before anything starts, written after the
//   participants have ended).
// - The controller's SIL Kit thread: the hub's tap (what RunSamples keeps).
// - Each participant's reader thread (process::Process): RunOutput::on_line() (which keeps a bridge's
//   last line and its counts under RunOutput's mutex, and writes the event "outside" there; the
//   main thread writes the counts held back while it watches the simulation (outside_due()), and
//   takes the last ones, with their event, into the result after the end (take_counts()), before
//   the result's event).
// - The closing thread (Runner::close_connection(), at the end of run()): destroys the hub's
//   transport, the controller and the registry (the RunConnection, which it holds from then on),
//   and touches nothing else of the run. The main thread waits for it with a limit. What a closing
//   thread that was left at the limit may still reach is listed below.
// - Whoever applies a time command (the thread that reads commands, a control server's):
//   TimeCommands.
//
// The locks, in the only orders they are taken together (a lock not named under another is never
// taken while that one is held):
// - TimeCommands' mutex, then the event stream's, the controller's time gate's, or what
//   RunOptions' callbacks take (on_detach, the `accepted` of a stop).
// - RunOutput's mutex, then the event stream's (and RunOptions::write_line).
// - The hub's mutex, then RunSamples' mutex (the tap).
// - StartLimit's mutex, StartShared::found_mutex, RunSamples' mutex, RunScenario's error mutex:
//   nothing under them.
// The controls (RunControls) add no lock: the main thread sets the phase with none of the locks
// above held, and the phase's observer takes the event stream's only. The run's state is put
// together and told under TimeCommands' mutex (RunControls::publish(), TimeCommands::OnChange),
// which reads the time gate and then takes the event stream's: the order of the first line. So
// publish() is called with none of the locks above held (set_phase() and RunNotes::note_time() on
// the main thread; the hold's begin and end on the window's thread, where TimeCommands' mutex is
// taken already).
//
// A control server (RunOptions::control_endpoint) is not the Runner's: run_file() starts it on the
// main thread between the Runner's two stages (prepare(), then run()), on the hub that prepare()
// gives it, and tells run() how that went (ControlServerStart); the Runner never calls the server.
// The server has threads of its own. Of the run they touch the
// hub (thread safe; its subscriptions are bounded and never make the simulation wait; the server
// holds the hub itself, so it may use it after the run) and the TimeCommands. With a scenario the
// server only shows the run: its clients' calls that would write a variable or move the time are
// refused in the server, before they reach either.
//
// What the closing thread reaches, also when it is left at the limit and ends later (or never).
// Everything that the three SIL Kit objects call was read for this list; a change that gives the
// controller, its participant or the hub a new function must keep it true.
// - The RunConnection itself (the thread's own hold): the transport, the controller, the registry,
//   and through the transport's destructor the hub (control::Hub::agent_ended() for every link
//   that is still open). The hub then tells its subscriptions, which keep themselves (the hub
//   holds them weakly), and no tap: Runner::run() takes the tap off before the hand-over. The
//   thread's share of the hub may be the last one (the Runner and a control server have gone
//   meanwhile): then the hub is destroyed on the closing thread, which frees the hub's own only.
// - The functions given to the controller's participant by the transport (the observer of the
//   peers' states, the three subscribers) hold the transport's own state, which keeps itself and
//   does nothing once the transport has gone. Those of the controller (the step, the hold, the
//   stop observer, the opening of the start window) use the controller's members only.
// - The start window's function (Runner::run() gives it to the controller; it names the Runner,
//   the transport and the controller): never called by the closing thread. It runs only inside
//   SimulationController::start(), which waits for it before it returns or throws, and the
//   controller's destructor completes a window that opens later without calling it. The
//   controller keeps the function until it goes, so the Runner it names may be gone by then:
//   the function is destroyed there, not called (it holds pointers only).
// - Nothing of TimeCommands: it is detached on the main thread before the hand-over, and has no
//   function in the controller (it calls the controller, never the other way round). The same
//   holds for the watch of the simulation (FailureCheck, SimulationWatch: they ask the controller
//   on the main thread and leave nothing in it) and for the scenario's thread (joined before).
// So a closing thread that was left touches the RunConnection only, and run_file() destroys the
// Runner of such a run as that of any other: nothing of the Runner has to outlive run(). What is
// left for the caller is to end the process without the destructors of static objects (the thread
// is inside SIL Kit): RunOutcome::connection_not_closed_ms.
#include "vehicle_sil/launch/run.hpp"

#include "run_close.hpp"
#include "run_connection.hpp"
#include "run_context.hpp"
#include "run_control.hpp"
#include "run_notes.hpp"
#include "run_output.hpp"
#include "run_participants.hpp"
#include "run_samples.hpp"
#include "run_scenario.hpp"
#include "run_start.hpp"
#include "run_watch.hpp"

#include "vehicle_sil/launch/events.hpp"
#include "vehicle_sil/launch/timeline_format.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <silkit/config/IParticipantConfiguration.hpp>
#include <silkit/vendor/CreateSilKitRegistry.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
/// What a run has of SIL Kit, with its hub (run_connection.hpp).
using RunConnection = detail::ConnectionOf<control::Hub, SilKit::Vendor::Vector::ISilKitRegistry, SimulationController,
                                           control_silkit::HubTransport>;

using detail::RunNotes;
/// The controls of a run that was given none (RunOptions::controls): made by run_file() for that
/// run, on the caller's stop requests if there are any. Its phases go to RunOptions::events.
struct OwnControls
{
    static RunControls::Observers observers(EventStream* events)
    {
        RunControls::Observers out;
        if (events != nullptr)
        {
            out.on_phase = [events](Phase phase) { events->phase(phase); };
            out.on_state = [events](const RunState& state) { events->state(state); };
        }
        return out;
    }

    explicit OwnControls(const RunOptions& options) :
      controls(options.stops != nullptr ? *options.stops : none, options.start_paused, observers(options.events))
    {
        if (options.interrupt_name)
        {
            controls.set_stop_name(options.interrupt_name);
        }
    }

    StopRequests none; ///< without RunOptions::stops: nothing ever asks
    RunControls controls;
};

using detail::RunOutput;
using detail::RunParticipants;
using detail::RunSamples;
using detail::RunScenario;
using detail::RunStart;
using detail::RunWatch;
using detail::uri_for_participants;

/// What run_file() did between the two stages of a Runner (prepare(), then run()): whether it
/// started a control server (RunOptions::control_endpoint), and how that went. As made: none.
struct ControlServerStart
{
    std::optional<ControlServerInfo> listening; ///< the server listens there
    std::optional<std::string> error;           ///< it could not be started: why (what its start threw)
};

/// One run: its parts, and the order of its phases in two stages. prepare() opens the run's files;
/// run() does everything from the registry to the end. Between them the caller starts what serves
/// the run's hub from outside (a control server), so that it listens before the registry does.
/// The hub is made with the Runner (by its RunControl) and lives as long as it (and as whoever else holds it). The registry, the
/// controller and the hub's transport live in run() only, in a RunConnection; the parts are given
/// the controller in the calls that need it. A Runner can be destroyed whenever run() has
/// returned, also while the closing thread of its run is still closing (it was left at its limit).
class Runner
{
    static std::vector<std::string> participant_names(const SimulationConfig& config)
    {
        std::vector<std::string> names;
        for (const auto& p : config.participants)
        {
            names.push_back(p.name);
        }
        return names;
    }

public:
    /// `check`: the check that passed, with the bridges of --connect (detail::with_bridges()).
    /// `controls`: RunOptions::controls, or those of `own` (null with the caller's).
    Runner(CheckReport checked, std::filesystem::path base, const RunOptions& options, RunControls& controls,
           std::unique_ptr<OwnControls> own) :
      own_controls_(std::move(own)), check_(std::move(checked)),
      ctx_{*check_.config, check_, options, check_.scenario && check_.scenario->scenario ? &*check_.scenario->scenario : nullptr,
           controls, controls.time()},
      out_(ctx_), control_(ctx_, out_),
      samples_(participant_names(*check_.config), detail::traffic_plan(*check_.config, check_)),
      notes_(ctx_, participants_), scenario_(ctx_, out_, control_, samples_),
      start_(ctx_, out_, control_, notes_, scenario_, participants_, outcome_),
      watch_(ctx_, out_, notes_, samples_, scenario_, start_, participants_, outcome_),
      participants_(ctx_, out_, outcome_, std::move(base))
    {
        const SimulationConfig& config = ctx_.config;
        const CheckReport& check = ctx_.check;
        outcome_.start_timeout_ms = config.start_timeout_ms;
        outcome_.stop_timeout_ms = config.stop_timeout_ms;
        outcome_.control_hello_timeout_ms = options.control_hello_timeout.count();
        outcome_.stop_at_ms = config.stop_at_ms;
        // 1 with the time lock, whatever was asked for (the check refuses another --speed).
        const std::optional<double> speed = planned_speed(check.time_lock, options.speed, config.speed);
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
            run.for_network = p.for_network;
            run.line = p.line;
            outcome_.participants.push_back(std::move(run));
        }
        for (const NetworkOutside& o : check.outside)
        {
            outcome_.outside.push_back(RunOutside{o.network, o.target, o.participant, std::nullopt, std::nullopt, o.tool});
        }
    }

    /// The first stage: the phase "starting", then the run's files (the participants' logs, the
    /// timeline). Returns the run's hub (never null, for whoever serves it; the holder keeps the
    /// hub only, see RunConnection) if the run goes on; null if it does not (it was interrupted
    /// before it began, or a file cannot be opened: the outcome says so). Nothing of SIL Kit
    /// exists yet, and no other thread of the run. run() must follow, whatever this returned.
    [[nodiscard]] std::shared_ptr<control::Hub> prepare()
    {
        if (ctx_.interrupts() > 0)
        {
            outcome_.interrupted_by = ctx_.interrupt_name();
            return nullptr;
        }
        ctx_.controls.set_phase(Phase::Starting);
        if (!out_.open_logs(outcome_) || !open_timeline())
        {
            return nullptr;
        }
        prepared_ = true;
        return control_.shared_hub();
    }

    /// The second stage, once, after prepare(): the registry, the participants, the simulation and
    /// its end; all of the outcome. After a prepare() that returned null it starts nothing and
    /// only ends the time commands. `server`: what the caller did in between; the run says where
    /// the server listens, or starts nothing if it could not be started.
    RunOutcome run(const ControlServerStart& server)
    {
        // The registry, the controller and the hub's transport of this run (the hub's end of the
        // ECUs' control agents, on the controller's participant), with the hub. Before the guards
        // below, so that it goes after them: if run() leaves before close_connection(), the three
        // go here, on this thread, in the connection's order (transport, controller, registry).
        // run() is its only holder until it gives it to close_connection(). Its hub is the run's
        // one hub (RunControl's, which got the variable maps in prepare() or gets them here): the
        // transport below is made on the connection's hub, so what the closing thread reaches is
        // what the connection keeps.
        auto connection = std::make_shared<RunConnection>(control_.shared_hub());
        // Whatever happens, the time commands end with the run (they are refused from then on),
        // and before the controller they are given to goes: the thread that reads the commands
        // may apply one at any time.
        struct Detach
        {
            TimeCommands& commands;
            ~Detach() { commands.detach(); }
        } const detach{ctx_.commands};
        // The hub's tap and the scenario's thread end with the run, before the controller (whose
        // time the tap reads) and the transport go (declared after them); the thread first.
        struct EndSamples
        {
            Runner& runner;
            ~EndSamples() { runner.samples_.untap(); }
        } const endSamples{*this};
        struct EndScenario
        {
            Runner& runner;
            ~EndScenario() { runner.scenario_.end(); }
        } const endScenario{*this};
        if (!prepared_)
        {
            return outcome_;
        }
        const auto no_server = [this](const std::string& why) {
            outcome_.setup_error = "the control server cannot be started: " + one_line(why);
        };
        if (server.error)
        {
            no_server(*server.error);
            return outcome_;
        }
        if (server.listening)
        {
            try
            {
                const ControlServerInfo& at = *server.listening;
                out_.event([&at](EventStream& e) { e.control_server(at.address, at.tls, at.credentials); });
                out_.say("control server at " + at.address);
            }
            catch (const std::exception& e)
            {
                no_server(e.what());
                return outcome_;
            }
        }
        std::string uri;
        std::string listened;
        try
        {
            connection->registry = SilKit::Vendor::Vector::CreateSilKitRegistry(
                SilKit::Config::ParticipantConfigurationFromString(std::string(default_registry_configuration())));
            // A participant started by hand gets a command that stays the same from run to run.
            const bool fixed = !ctx_.config.registry_uri && waits_for_manual(ctx_.config);
            const std::string listen =
                ctx_.config.registry_uri.value_or(fixed ? std::string(kManualRegistryUri) : std::string("silkit://127.0.0.1:0"));
            try
            {
                listened = connection->registry->StartListening(listen);
                uri = uri_for_participants(listened);
            }
            catch (const std::exception& e)
            {
                throw std::runtime_error("the registry cannot listen at " + listen + ": " + one_line(e.what()) +
                                         detail::registry_listen_hint(listen, ctx_.config.registry_uri.has_value(), fixed,
                                                                      detail::host_addresses(true, true)));
            }
            outcome_.registry_uri = uri;
            out_.event([&uri](EventStream& e) { e.registry(uri); });
            out_.say("registry at " + uri);
            if (!is_loopback_registry_uri(listen))
            {
                // On [::]: IPv6, and IPv4 too where such a socket takes it.
                const bool any6 = listened.rfind("silkit://[::]:", 0) == 0;
                const bool ipv4 = !any6 || detail::ipv6_any_takes_ipv4();
                out_.say(detail::other_computers_line(listened, detail::host_addresses(ipv4, any6), any6 && !ipv4,
                                                      listen.substr(listen.rfind(':') + 1)));
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
            connection->controller = std::make_unique<SimulationController>(std::move(c));
            // Before the participants start and before the controller does: SIL Kit connects the
            // publisher of the commands and the subscribers with every participant that starts.
            connection->transport =
                std::make_unique<control_silkit::HubTransport>(*connection->hub, connection->controller->runtime());
            // Called inside SimulationController::start() only (wait_for_start() below), never
            // once that has returned: not by the closing thread, which may outlive this Runner.
            connection->controller->set_start_window(
                [this, t = connection->transport.get(), c = connection->controller.get()](const StartWindow& window) {
                    start_.window(*t, *c, window);
                });
            ctx_.commands.attach(*connection->controller, outcome_.timing.time_step_ms, outcome_.timing.speed);
            // Every sample the hub takes, in every run: for the traffic of the result, and for
            // the scenario.
            samples_.tap(control_.hub(), std::chrono::milliseconds(outcome_.timing.time_step_ms),
                         [c = connection->controller.get()] { return c->runtime().now(); });
            if (ctx_.scenario != nullptr)
            {
                // The scenario decides at every step of the controller, from the one at 0 on.
                connection->controller->guard_from(SimTime{});
                scenario_.prepare(std::chrono::milliseconds(outcome_.timing.time_step_ms));
            }
        }
        catch (const std::exception& e)
        {
            outcome_.setup_error = one_line(e.what());
            return outcome_;
        }

        bool ran = false;
        // The bridges of --connect first, each Running in SIL Kit before an ECU starts: a frame an
        // ECU sends before its bridge has joined would not reach the bus outside. A bridge does not
        // take part in the coordinated start (it runs in real time), so it does not hold it up.
        if (participants_.start_children(uri, RunParticipants::Which::Bridges) &&
            start_.wait_for_bridges(*connection->controller) && participants_.start_children(uri, RunParticipants::Which::Others))
        {
            ctx_.controls.set_phase(Phase::Waiting);
            if (start_.wait_for_start(*connection->controller, *connection->transport))
            {
                ran = true;
                scenario_.start(*connection->controller, outcome_);
                const RunWatch::Watched watched = watch_.watch(*connection->controller);
                interrupts_at_end_ = watch_.settle(*connection->controller, watched);
                if (outcome_.simulation != SimulationEnd::Aborted && outcome_.interrupted_by.empty())
                {
                    participants_.note_bridges_ended_with_run();
                }
                control_.note_late_values(outcome_, *connection->transport);
                control_.note_late_maps(outcome_);
            }
        }
        // No time command from here on, also if the simulation did not start: a stop accepted from
        // now on would only cut short the end of the participants.
        ctx_.commands.detach();
        if (!ran)
        {
            // As RunWatch::settle() does: an interrupt from here on comes while the participants end (a stop
            // accepted before the detach counts as one of the run).
            interrupts_at_end_ = ctx_.interrupts();
        }
        // What is known for good when the simulation is over (why it ended), before the
        // participants end.
        publish_findings(false);
        ctx_.controls.set_phase(Phase::Stopping);
        participants_.end_children(ran || start_.before_start_failed(), interrupts_at_end_);
        participants_.end_manual(*connection->controller, ran || start_.before_start_failed(), interrupts_at_end_);
        // Their output is read to the end: the line connect-failed and connect-lost quote.
        participants_.note_last_lines();
        // The agents have said their last word and waited for this side's answer (their processes
        // have ended); what is still open in the hub (of a process that was killed, or whose last
        // word did not arrive within its wait) is settled here. No sample is taken any more: the
        // tap goes before the transport does (a run that did not get to its watch still has it).
        samples_.untap();
        // From here on the connection is the closing thread's: nothing of it is named after this.
        close_connection(std::move(connection));
        return outcome_;
    }

    /// run --events: the run's findings that have not been written yet. Before the participants
    /// have ended (`final` false) only those that their end cannot change (settled_findings()):
    /// what is written as an event is in the result as it was written.
    void publish_findings(bool final) { notes_.publish_findings(outcome_, final); }

    /// run --timeline: writes the timeline file of the run that is over, with `findings` (the
    /// check's and the run's), and says where it is. Nothing if the file was not opened (no
    /// --timeline, or the run ended before it got there). If it cannot be written: why (also in
    /// the outcome: RunOutcome::timeline_error), else "".
    std::string write_timeline(const CheckReport& check, std::vector<Finding> findings)
    {
        if (!timeline_file_.is_open())
        {
            return {};
        }
        const ScenarioCatalog catalog = scenario_catalog(check);
        TimelineInput input;
        input.file = check.file;
        input.variant = ctx_.config.variant;
        input.params = check.scenario->params;
        input.time_step_ms = outcome_.timing.time_step_ms;
        input.scenario = ctx_.scenario;
        input.run = &*outcome_.scenario;
        input.catalog = &catalog;
        input.record = outcome_.timeline_record.get();
        input.findings = std::move(findings);
        const std::string text = format_timeline_json(input);
        timeline_file_.write(text.data(), static_cast<std::streamsize>(text.size()));
        timeline_file_.close();
        if (timeline_file_.fail())
        {
            outcome_.timeline_error = "writing " + std::to_string(text.size()) + " bytes to it failed (is the disk full?)";
            return outcome_.timeline_error;
        }
        const std::uint64_t dropped = outcome_.timeline_record ? outcome_.timeline_record->dropped() : 0;
        out_.say("timeline: " + outcome_.timeline +
                 (dropped > 0 ? " (without " + std::to_string(dropped) + (dropped == 1 ? " older point" : " older points") +
                                    ", dropped at its limits: limits.dropped in the file)"
                              : ""));
        return {};
    }

private:
    /// run --timeline: opens the timeline file, before anything starts. false (with
    /// outcome_.setup_error) if it cannot be written.
    [[nodiscard]] bool open_timeline()
    {
        if (ctx_.options.timeline.empty() || ctx_.scenario == nullptr)
        {
            return true;
        }
        std::error_code ec;
        if (ctx_.options.timeline.has_parent_path())
        {
            std::filesystem::create_directories(ctx_.options.timeline.parent_path(), ec);
        }
        timeline_file_.open(ctx_.options.timeline, std::ios::binary | std::ios::trunc);
        if (!timeline_file_)
        {
            outcome_.setup_error = "cannot write the timeline " + ctx_.options.timeline_shown +
                                   ": check that --timeline names a file in a directory you can write to (the file is opened "
                                   "before anything starts, so that a run is not lost for it)";
            return false;
        }
        outcome_.timeline = ctx_.options.timeline_shown;
        return true;
    }

    /// The end of what the run has of SIL Kit: destroys the hub's transport, the controller and
    /// the registry, in this order, and waits for that close_limit_ at most (one more interrupt
    /// ends the wait). SIL Kit joins the thread of its connections when a participant is
    /// destroyed, and a connection that does not close would keep the run from its result for
    /// ever (seen on Windows before its connections were TCP ones, with a participant whose
    /// threads were suspended: docs/dependencies.md). So the three are destroyed on a thread of
    /// their own (detail::close_connection_within()), which takes over `connection`, the only
    /// hold of them; if it has not ended in time, it is left with them and with its share of the
    /// hub (RunOutcome::connection_not_closed_ms), and the run goes on to its result.
    void close_connection(std::shared_ptr<RunConnection> connection)
    {
        // The tap reads the controller's time and belongs to this Runner, which may be destroyed
        // while the closing thread still closes: it is off before the thread has the connection.
        assert(!samples_.tapped());
        const int interrupts = ctx_.interrupts();
        // The thread keeps the connection, and reaches nothing else of this run (the list at the
        // top of this file): the Runner may be destroyed while the thread is still closing.
        // `interrupted` is asked on this thread, during the wait only.
        const auto left = detail::close_connection_within(std::move(connection), close_limit_, ctx_.options.poll,
                                                          [this, interrupts] { return ctx_.interrupts() > interrupts; });
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
    /// Without RunOptions::controls: what ctx_ refers to, kept here for the same reason as check_.
    std::unique_ptr<OwnControls> own_controls_;
    /// What ctx_ refers to (the check with the bridges of --connect, which is not run_file()'s report).
    const CheckReport check_;
    const RunContext ctx_;
    RunOutcome outcome_; ///< the main thread's: the other threads of the run never see it
    RunOutput out_;
    /// Makes and holds the hub of this run; the RunConnection of run() and whoever was given the
    /// hub by a callback of RunOptions hold it too. Before samples_ and scenario_, which use the
    /// hub until they are destroyed (the tap, the scenario's engine and port).
    RunControl control_;
    RunSamples samples_; ///< after control_ (its tap is on the hub), before scenario_ (which takes from it)
    RunNotes notes_;
    RunScenario scenario_;
    RunStart start_;
    RunWatch watch_;
    bool prepared_ = false;     ///< prepare() got through: run() goes on
    int interrupts_at_end_ = 0; ///< ctx_.interrupts() when the simulation had ended
    std::ofstream timeline_file_; ///< run --timeline: open from open_timeline() to write_timeline(); the main thread's
    static constexpr std::chrono::milliseconds kCloseMargin{5000};
    std::chrono::milliseconds close_limit_{0}; ///< close_connection(): how long the SIL Kit objects may take to go
    // Last, so that it goes first: a Process ends its reader thread, which uses the output (out_),
    // also when run() leaves with an exception.
    RunParticipants participants_;
};

} // namespace

RunReport run_file(const std::filesystem::path& path, const std::string& shown_as, const RunOptions& options)
{
    if (options.controls != nullptr)
    {
        if (options.stops != nullptr && options.stops != &options.controls->stops())
        {
            throw std::invalid_argument(
              "RunOptions: both `controls` and `stops` are set, and the run reads only the stop requests of `controls`; "
              "make the RunControls on the same StopRequests and leave `stops` null.");
        }
        if (options.events != nullptr && (!options.controls->has_phase_observer() || !options.controls->has_command_observer() ||
                                          !options.controls->has_state_observer()))
        {
            throw std::invalid_argument(
              "RunOptions: `controls` and `events` are both set, but the controls have no observer for the "
              "events the run leaves to them; set RunControls::Observers::on_phase to call EventStream::phase(), "
              "Observers::on_command to call EventStream::command() and Observers::on_state to call "
              "EventStream::state(), or leave `events` null.");
        }
    }
    const auto event = [&options](auto emit) {
        if (options.events != nullptr)
        {
            emit(*options.events);
        }
    };
    // The controls of this run: the caller's, or its own, which go with the Runner once there is one.
    std::unique_ptr<OwnControls> own = options.controls == nullptr ? std::make_unique<OwnControls>(options) : nullptr;
    RunControls& controls = own ? own->controls : *options.controls;
    RunReport report;
    report.strict = options.check.strict;
    controls.set_phase(Phase::Checking);
    // The check judges the time by what this run does: its speed and --start-paused are written
    // in RunOptions only. A test's speed is its default, not one the user asked for.
    CheckOptions check = options.check;
    check.run_speed = check.fastest_by_default ? std::optional<std::optional<double>>{} : options.speed;
    check.start_paused = options.start_paused;
    report.check = check_file(path, shown_as, check);
    event([&report](EventStream& e) { e.check(report.check); });
    if (!report.check.config || !report.check.passed())
    {
        controls.set_phase(Phase::Ended);
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
    // Destroyed when run_file() returns, also if the run's closing thread was left at its limit
    // (RunOutcome::connection_not_closed_ms): that thread uses nothing of the Runner.
    Runner runner(detail::with_bridges(report.check), base, options, controls, std::move(own));
    // The control server between the Runner's two stages: after the run's files are open, before
    // the registry listens. The Runner does not know the server; it is told what became of it.
    ControlServerStart server;
    try
    {
        if (const std::shared_ptr<control::Hub> hub = runner.prepare(); hub && options.control_endpoint != nullptr)
        {
            try
            {
                server.listening = options.control_endpoint->start(hub);
            }
            catch (const std::exception& e)
            {
                server.error = e.what();
            }
        }
    }
    catch (...)
    {
        // What leaves here does not reach run(), whose guard ends the time commands: they end
        // with the run also then (they are refused from now on), as for every other way out.
        controls.time().detach();
        throw;
    }
    report.run = runner.run(server);
    report.findings = assess_run(*report.run);
    {
        // run --timeline: the file has the scenario's findings of the check and of the run.
        std::vector<Finding> findings = report.check.findings;
        findings.insert(findings.end(), report.findings.begin(), report.findings.end());
        if (const std::string error = runner.write_timeline(report.check, std::move(findings)); !error.empty())
        {
            report.run->timeline_error = error;
            report.findings = assess_run(*report.run);
        }
    }
    runner.publish_findings(true);
    controls.set_phase(Phase::Ended);
    return report;
}

} // namespace vehicle_sil::launch
