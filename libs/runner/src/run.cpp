// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch run: the order of a run from its setup to the end of the participants
// (Runner::run(), detail/runner.hpp, which forwards to its RunnerImpl: runner.hpp). run_file() drives
// a Runner (run_file.cpp). The parts of a run are in the files beside this one:
//
//   run_plan.*           RunPlan, plan_run(), planned_outcome()   what is settled before the run: a value
//   run_context.hpp      RunContext                 what the run is given (the plan, the observers,
//                        the controls it borrows); never written
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
//                        (the outcome: run_outcome.hpp; the tools the parts share: run_detail.hpp)
//   run_connect.cpp      with_bridges()   run --connect: the bridges as participants of the run only
//   run_outside.cpp      read_outside_counts(), OutsideNote, OutsideState   run --connect: the counts in a
//                        bridge's statistics lines, its link line, and when the event "outside" goes out
//   timeline.cpp         TimelineRecord   run --timeline: what the scenario's engine notes
//   timeline_format.cpp  format_timeline_json()   the file made of it (run_file(); the Runner opens the file
//                        and writes the text: open_timeline(), write_timeline())
//
// The threads of a run, and what each may touch:
// - The main thread (run_file()'s caller): everything, and alone the RunOutcome, RunParticipants,
//   RunNotes, RunWatch and RunStart's own members.
// - The start window's thread (one that SimulationController::start() makes for the window, not
//   its SIL Kit thread; RunStartWindow::run()): the StartShared, RunScenario::begin(),
//   RunControl::expected_agents() and hub(), TimeCommands::hold_before_start(), and RunOutput;
//   it reads the peers' states (gone_before_start(): the required participants and the bridges of
//   --connect, whose mark RunPlan::members is fixed before the run).
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
// - The closing thread (RunnerImpl::close_connection(), at the end of run()): destroys the hub's
//   transport, the controller and the registry (the RunConnection, which it holds from then on),
//   and touches nothing else of the run. The main thread waits for it with a limit. What a closing
//   thread that was left at the limit may still reach is listed below.
// - Whoever applies a time command (the thread that reads commands, a control server's):
//   TimeCommands.
//
// The locks, in the only orders they are taken together (a lock not named under another is never
// taken while that one is held):
// - TimeCommands' mutex, then the event stream's, the controller's time gate's, or what
//   the caller's callbacks take (on_detach, the `accepted` of a stop).
// - RunOutput's mutex, then the event stream's (and RunObservers::write_line).
// - RunOutput's mutex, then TimeCommands' (RunOutput::tell_outside() calls RunControls::set_outside(),
//   which tells the state of the bridges). The reverse is never taken: what runs under TimeCommands'
//   mutex (OnCommand, OnChange, the state's observer, `accepted`, on_detach) writes events and
//   does not call RunOutput (checked: the only callers of say(), hold(), on_line(), outside_*()
//   are the run's own threads, none of them from such a callback).
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
// What a run is given is of three kinds:
// - The request (RunRequest) and the limits (RunLimits), of which run_file() makes the plan
//   (RunPlan, by plan_run() before the Runner): values only. The Runner keeps the plan, by value,
//   and no thread writes it, so every thread of the run reads it without a lock. The request
//   itself is not kept: no thread of the run reads it, so a run whose closing thread was left
//   (OutcomeClose::not_closed_ms) holds nothing of it.
// - The observers (RunObservers: write_line, interrupt_name, before_start): the caller's
//   functions, of which the Runner keeps a copy. The threads that call each, and the locks held
//   then, are those of the lists above (RunOutput's mutex for write_line and the lines' events,
//   TimeCommands' mutex for interrupt_name, the window's thread for before_start).
// - What it borrows: the controls (RunControls, the caller's or run_file()'s own), and what it
//   tells through (RunEvents: the stream of RunLent::events, or none), which outlive the Runner.
//   The Runner holds a reference to each and never destroys them.
//
// A control server (RunHost::control) is run_file()'s, not the Runner's. run_file() makes it
// before the check, and before the phase "checking" is told, from what the request names
// (make_control_server_of_run(): the address, the files of TLS, the credentials, the audit log);
// a request that is wrong there is a usage error of the run, and nothing is checked or started.
// It starts the server on the main thread between the Runner's two stages (prepare(), then
// run()), on the hub that prepare() gives it, and tells run() how that went (ControlServerStart);
// the Runner never calls the server. When run_file() returns or throws, after the phase "ended"
// has been told and the Runner has gone, it ends the server: every call of a client returns and
// none comes any more (control_grpc::ControlServer::shutdown(): gRPC's Shutdown(), which cancels
// the calls that are still open after 2 s, then Wait(), which waits for their handlers to return
// and has no limit of its own; the handlers leave when they are cancelled or the server stops).
// That is before the caller writes the result, as it always was.
// run_file() does not destroy the server: the report holds it (RunReport::ended_control_server),
// and the caller lets it go after it has written the result; in a run whose closing thread was
// left (OutcomeClose::not_closed_ms), before it ends the process without destructors. Why not
// here, before the result: the server's destruction frees the last gRPC object of the process,
// which may run the shutdown of gRPC itself on this thread (its threads, its I/O), and how long
// that may take is not established. Whatever it takes, the result is out by then. The rest of
// the destruction is short: the second shutdown() returns at once; the server's monitor joins a
// thread that sleeps on a condition variable or is in one look at the time
// (TimeCommands::inspect(), which returns at once once the run has detached its controller); the
// server's share of the hub goes (in a run whose closing thread was left, that thread's
// RunConnection has one too, so the hub stays); the audit log's file is closed, each line having
// been flushed when it was written.
// A server that the caller lends instead (RunLent::control_endpoint; the tests') is started at
// the same point and stays the caller's, to end and to destroy.
// The server has threads of its own. Of the run they touch the
// hub (thread safe; its subscriptions are bounded and never make the simulation wait; the server
// holds the hub itself, so it may use it after the run) and the TimeCommands. With a scenario the
// server refuses its clients' calls that would write a variable, before they reach the hub; the
// time operations go to the TimeCommands as in any run.
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
// is inside SIL Kit): OutcomeClose::not_closed_ms.
#include "runner_impl.hpp"

#include "vehicle_sil/runner/detail/run_detail.hpp"

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

namespace detail {

namespace {

std::vector<std::string> participant_names(const SimulationConfig& config)
{
    std::vector<std::string> names;
    for (const auto& p : config.participants)
    {
        names.push_back(p.name);
    }
    return names;
}

} // namespace

RunnerImpl::RunnerImpl(RunPlan plan, RunObservers observers, RunEvents* events, RunControls& controls) :
  plan_(std::move(plan)), observers_(std::move(observers)),
  ctx_{plan_.config(), plan_.check, plan_, observers_, events, plan_.scenario(), controls, controls.time()},
  outcome_(planned_outcome(plan_)), out_(ctx_), control_(ctx_, out_),
  samples_(participant_names(ctx_.config), detail::traffic_plan(ctx_.config, ctx_.check, ctx_.plan.members)), notes_(ctx_, participants_),
  scenario_(ctx_, out_, control_, samples_), start_(ctx_, out_, control_, notes_, scenario_, participants_, outcome_),
  watch_(ctx_, out_, notes_, samples_, scenario_, start_, participants_, outcome_),
  participants_(ctx_, out_, outcome_, plan_.base)
{
}

std::shared_ptr<control::Hub> RunnerImpl::prepare()
{
    if (ctx_.interrupts() > 0)
    {
        ctx_.note_interrupt(outcome_.end);
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

RunOutcome RunnerImpl::run(const ControlServerStart& server)
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
        RunnerImpl& runner;
        ~EndSamples() { runner.samples_.untap(); }
    } const endSamples{*this};
    struct EndScenario
    {
        RunnerImpl& runner;
        ~EndScenario() { runner.scenario_.end(); }
    } const endScenario{*this};
    if (!prepared_)
    {
        return outcome_;
    }
    const auto no_server = [this](const std::string& why) {
        outcome_.setup.error = "the control server cannot be started: " + one_line(why);
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
            out_.event([&at](RunEvents& e) { e.control_server(at.address, at.tls, at.credentials); });
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
        outcome_.setup.registry_uri = uri;
        out_.event([&uri](RunEvents& e) { e.registry(uri); });
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
        time.step = std::chrono::milliseconds(ctx_.plan.timing.time_step_ms);
        time.late_tolerance = std::chrono::milliseconds(ctx_.plan.timing.late_tolerance_ms);
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
        ctx_.commands.attach(*connection->controller, ctx_.plan.timing.time_step_ms, ctx_.plan.timing.speed);
        // Every sample the hub takes, in every run: for the traffic of the result, and for
        // the scenario.
        samples_.tap(control_.hub(), std::chrono::milliseconds(ctx_.plan.timing.time_step_ms),
                     [c = connection->controller.get()] { return c->runtime().now(); });
        if (ctx_.scenario != nullptr)
        {
            // The scenario decides at every step of the controller, from the one at 0 on.
            connection->controller->guard_from(SimTime{});
            scenario_.prepare(std::chrono::milliseconds(ctx_.plan.timing.time_step_ms));
        }
    }
    catch (const std::exception& e)
    {
        outcome_.setup.error = one_line(e.what());
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
            if (outcome_.end.simulation != SimulationEnd::Aborted && outcome_.end.interrupted_by.empty())
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

std::string RunnerImpl::write_timeline(const std::string& text)
{
    if (!timeline_file_.is_open())
    {
        return {};
    }
    timeline_file_.write(text.data(), static_cast<std::streamsize>(text.size()));
    timeline_file_.close();
    if (timeline_file_.fail())
    {
        outcome_.scenario.timeline_error = "writing " + std::to_string(text.size()) + " bytes to it failed (is the disk full?)";
        return outcome_.scenario.timeline_error;
    }
    const std::uint64_t dropped = outcome_.scenario.timeline_record ? outcome_.scenario.timeline_record->dropped() : 0;
    out_.say("timeline: " + outcome_.scenario.timeline +
             (dropped > 0 ? " (without " + std::to_string(dropped) + (dropped == 1 ? " older point" : " older points") +
                                ", dropped at its limits: limits.dropped in the file)"
                          : ""));
    return {};
}

bool RunnerImpl::open_timeline()
{
    if (ctx_.plan.timeline.empty() || ctx_.scenario == nullptr)
    {
        return true;
    }
    std::error_code ec;
    if (ctx_.plan.timeline.has_parent_path())
    {
        std::filesystem::create_directories(ctx_.plan.timeline.parent_path(), ec);
    }
    timeline_file_.open(ctx_.plan.timeline, std::ios::binary | std::ios::trunc);
    if (!timeline_file_)
    {
        outcome_.setup.error = "cannot write the timeline " + ctx_.plan.timeline_shown +
                               ": check that --timeline names a file in a directory you can write to (the file is opened "
                               "before anything starts, so that a run is not lost for it)";
        return false;
    }
    outcome_.scenario.timeline = ctx_.plan.timeline_shown;
    return true;
}

void RunnerImpl::close_connection(std::shared_ptr<RunConnection> connection)
{
    // The tap reads the controller's time and belongs to this Runner, which may be destroyed
    // while the closing thread still closes: it is off before the thread has the connection.
    assert(!samples_.tapped());
    const int interrupts = ctx_.interrupts();
    // The thread keeps the connection, and reaches nothing else of this run (the list at the
    // top of this file): the Runner may be destroyed while the thread is still closing.
    // `interrupted` is asked on this thread, during the wait only.
    const auto left = detail::close_connection_within(std::move(connection), close_limit_, ctx_.plan.poll,
                                                      [this, interrupts] { return ctx_.interrupts() > interrupts; });
    if (!left)
    {
        return;
    }
    outcome_.close.not_closed_ms = left->waited.count();
    outcome_.close.interrupted = left->interrupted;
    out_.say("the launcher's own connection to the simulation (SIL Kit) has not closed " + std::to_string(left->waited.count()) +
             " ms after the participants ended" +
             (left->interrupted ? " and the launcher was asked to stop once more" : std::string{}) +
             ": the launcher ends without closing it");
}

Runner::Runner(RunPlan plan, RunObservers observers, RunEvents* events, RunControls& controls) :
  impl_(std::make_unique<RunnerImpl>(std::move(plan), std::move(observers), events, controls))
{
}

Runner::~Runner() = default;

std::shared_ptr<control::Hub> Runner::prepare() { return impl_->prepare(); }

RunOutcome Runner::run(const ControlServerStart& server) { return impl_->run(server); }

void Runner::publish_findings(bool final) { impl_->publish_findings(final); }

const RunPlan& Runner::plan() const { return impl_->plan(); }

bool Runner::timeline_open() const { return impl_->timeline_open(); }

std::string Runner::write_timeline(const std::string& text) { return impl_->write_timeline(text); }

} // namespace detail

} // namespace vehicle_sil::launch
