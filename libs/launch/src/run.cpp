// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/run.hpp"

#include "check_format.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/core/simulation_watch.hpp"
#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <silkit/config/IParticipantConfiguration.hpp>
#include <silkit/services/orchestration/string_utils.hpp>
#include <silkit/vendor/CreateSilKitRegistry.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace vehicle_sil::launch {

namespace {

constexpr std::string_view kLauncherTag = "[vehicle_sil_launch] ";

std::string prefix_of(const ParticipantRun& p) { return "participant '" + p.name + "': "; }

std::string hex_code(int code)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(code));
    return text;
}

/// "exited with code 7", "was ended by signal SIGSEGV".
std::string exit_text(const process::ExitStatus& exit)
{
    if (exit.signal)
    {
        return "was ended by signal " + process::signal_name(*exit.signal);
    }
    // A Windows NTSTATUS (0xC0000005 an access violation, 0xC000013A Ctrl+C) reads better in hex.
    return "exited with code " + (exit.code < 0 ? hex_code(exit.code) : std::to_string(exit.code));
}

/// The cell of the participants' table.
std::string ending_cell(const ParticipantRun& p)
{
    const auto status = [&p] {
        if (!p.exit)
        {
            return std::string{};
        }
        return p.exit->signal ? "signal " + process::signal_name(*p.exit->signal)
                              : "exit code " + (p.exit->code < 0 ? hex_code(p.exit->code) : std::to_string(p.exit->code));
    };
    switch (p.ending)
    {
    case Ending::NotStarted:
        return "not started";
    case Ending::ExitedBeforeStart:
    case Ending::Exited:
        return status();
    case Ending::Stopped:
        return "stopped (" + std::string(stop_request_name()) + "), " + status();
    case Ending::Killed:
        return "killed";
    }
    return "-";
}

bool ended_by_itself(const ParticipantRun& p)
{
    return p.ending == Ending::Exited || p.ending == Ending::ExitedBeforeStart;
}

bool failed_exit(const ParticipantRun& p)
{
    return p.exit && ended_by_itself(p) && (p.exit->code != 0 || p.exit->signal);
}

/// "its process exited with code 2", or what is known without its status.
std::string process_text(const ParticipantRun& p)
{
    return p.exit ? "its process " + exit_text(*p.exit) : std::string("its process ended");
}

std::string lines_hint(const ParticipantRun& p) { return "the lines [" + p.name + "] above show what it wrote"; }

const char* ending_name(Ending ending)
{
    switch (ending)
    {
    case Ending::NotStarted: return "not_started";
    case Ending::ExitedBeforeStart:
    case Ending::Exited: return "exited";
    case Ending::Stopped: return "stopped";
    case Ending::Killed: return "killed";
    }
    return "not_started";
}

const char* simulation_name(SimulationEnd end)
{
    switch (end)
    {
    case SimulationEnd::NotStarted: return "not_started";
    case SimulationEnd::ReachedStopAt: return "reached_stop_at";
    case SimulationEnd::Ended: return "ended";
    case SimulationEnd::Aborted: return "aborted";
    case SimulationEnd::Interrupted: return "interrupted";
    }
    return "not_started";
}

const char* result_name(RunResult result)
{
    switch (result)
    {
    case RunResult::Passed: return "passed";
    case RunResult::Failed: return "failed";
    case RunResult::Interrupted: return "interrupted";
    case RunResult::CheckFailed: return "check_failed";
    case RunResult::Unreadable: return "unreadable";
    case RunResult::SetupFailed: return "setup_failed";
    case RunResult::Usage: return "usage";
    }
    return "failed";
}

std::size_t count(const std::vector<Finding>& findings, Severity severity)
{
    return static_cast<std::size_t>(
        std::count_if(findings.begin(), findings.end(), [severity](const Finding& f) { return f.severity == severity; }));
}

} // namespace

std::string_view stop_request_name()
{
#ifdef _WIN32
    return "Ctrl+Break";
#else
    return "SIGTERM";
#endif
}

std::vector<Finding> assess_run(const RunOutcome& outcome)
{
    std::vector<Finding> out;
    const auto add = [&out](Severity severity, std::string code, std::string message, const ParticipantRun* p) {
        out.push_back(Finding{severity, std::move(code), std::move(message), p != nullptr ? p->line : 0,
                              p != nullptr ? p->name : std::string{}, ""});
    };
    if (!outcome.setup_error.empty())
    {
        add(Severity::Error, "setup", "cannot set up the simulation: " + outcome.setup_error, nullptr);
    }
    const bool ran = outcome.simulation != SimulationEnd::NotStarted;
    for (const ParticipantRun& p : outcome.participants)
    {
        const std::string pre = prefix_of(p);
        const Severity weight = p.required ? Severity::Error : Severity::Warning;
        if (!p.start_error.empty())
        {
            add(weight, "start", pre + p.start_error + ": check the command in the configuration file", &p);
            continue;
        }
        const bool blamed = !outcome.failed_participant.empty() && p.name == outcome.failed_participant;
        // An optional participant may end before or after the start (it does not hold the start
        // up): only its exit code counts, so that the same run gives the same result.
        if (p.ending == Ending::ExitedBeforeStart && p.required)
        {
            add(Severity::Error, "ended-before-start",
                pre + "it ended before the simulation started: " + process_text(p) +
                    ", so the simulation could not start; " + lines_hint(p),
                &p);
        }
        else if (blamed)
        {
            std::string what;
            if (p.exit && ended_by_itself(p))
            {
                what = "its process " + exit_text(*p.exit);
            }
            else if (!outcome.failure_reason.empty())
            {
                what = "it " + outcome.failure_reason;
            }
            else
            {
                what = "it left the simulation";
            }
            add(Severity::Error, "participant-failed",
                pre + "it failed while the simulation was running, and the launcher aborted the simulation: " + what +
                    "; " + lines_hint(p),
                &p);
        }
        else if (p.missing_at_start)
        {
            add(Severity::Error, "not-joined",
                pre + "it did not join the simulation within " + std::to_string(outcome.start_timeout_ms) +
                    " ms (start_timeout_ms), although its process was running: " +
                    (p.describe ? "it did not connect to the registry; " + lines_hint(p)
                                : "it must connect to the registry given with {registry_uri} as the participant '" +
                                      p.name + "' with a coordinated lifecycle (or set required: false)") +
                    "; if it is only slow, raise start_timeout_ms",
                &p);
        }
        else if (failed_exit(p))
        {
            add(weight, "exit-code", pre + "its process " + exit_text(*p.exit) + "; " + lines_hint(p), &p);
        }
        // Only a participant the launcher had to stop or end: one that ended by itself just
        // before the request did end.
        if (p.stopped_after_end && ran && (p.ending == Ending::Stopped || p.ending == Ending::Killed))
        {
            std::string how;
            if (p.stop_request_failed)
            {
                how = "could not ask it to stop (no console to send " + std::string(stop_request_name()) +
                      " through) and ended it with the processes it started";
            }
            else if (p.ending == Ending::Stopped)
            {
                how = "asked it to stop (" + std::string(stop_request_name()) + "), and " + process_text(p);
            }
            else
            {
                how = "asked it to stop (" + std::string(stop_request_name()) + "), and as it did not end within " +
                      std::to_string(outcome.stop_timeout_ms) + " ms either, ended it with the processes it started";
            }
            add(weight, "did-not-end",
                pre + "it was still running " + std::to_string(outcome.stop_timeout_ms) +
                    " ms (stop_timeout_ms) after the simulation ended: the launcher " + how +
                    "; a participant should end when the simulation ends (" + lines_hint(p) + ")",
                &p);
        }
        if (p.left_processes)
        {
            add(Severity::Warning, "left-processes",
                pre + "processes it started were still running after it had ended: the launcher ended them; end them "
                      "in the participant before it exits",
                &p);
        }
    }
    if (!outcome.start_error.empty())
    {
        add(Severity::Error, "simulation-start", "the simulation did not start: " + outcome.start_error, nullptr);
    }
    if (!outcome.controller_state.empty())
    {
        add(Severity::Error, "simulation",
            "the simulation ended in state " + outcome.controller_state +
                " instead of shutting down normally: the lines above show what the participants wrote",
            nullptr);
    }
    if (outcome.timed_out)
    {
        add(Severity::Error, "timeout",
            "the simulation did not end within " + std::to_string(outcome.timeout_ms.value_or(0)) +
                " ms of wall-clock time (--timeout-ms), so the launcher aborted it: a participant may hang in a step "
                "(the lines above show what the participants wrote); if it is only slow, raise --timeout-ms",
            nullptr);
    }
    else if (outcome.simulation == SimulationEnd::Aborted && outcome.failed_participant.empty())
    {
        add(Severity::Error, "simulation", "the simulation was aborted: the lines above show what the participants wrote",
            nullptr);
    }
    if (!outcome.interrupted_by.empty())
    {
        add(Severity::Note, "interrupted",
            "interrupted (" + outcome.interrupted_by + "): the launcher " +
                (outcome.interrupted_while_ending ? std::string("stopped the participants instead of waiting for them to end")
                 : ran                            ? std::string("stopped the simulation and the participants")
                                                  : std::string("stopped the participants before the simulation started")),
            nullptr);
    }
    if (outcome.simulation == SimulationEnd::Ended && outcome.stop_at_ms && outcome.controller_state.empty())
    {
        add(Severity::Note, "ended-before-stop-at",
            "the simulation ended before stop_at_ms (" + std::to_string(*outcome.stop_at_ms) +
                " ms): a participant stopped it; if that is not expected, the lines above show which one",
            nullptr);
    }
    sort_findings(out);
    return out;
}

std::size_t RunReport::errors() const { return check.errors() + count(findings, Severity::Error); }
std::size_t RunReport::warnings() const { return check.warnings() + count(findings, Severity::Warning); }
std::size_t RunReport::notes() const { return check.notes() + count(findings, Severity::Note); }

RunResult RunReport::result() const
{
    if (usage || check.usage)
    {
        return RunResult::Usage;
    }
    if (!check.config)
    {
        return RunResult::Unreadable;
    }
    if (!run)
    {
        return RunResult::CheckFailed;
    }
    if (!run->interrupted_by.empty())
    {
        return RunResult::Interrupted;
    }
    if (!run->setup_error.empty())
    {
        return RunResult::SetupFailed;
    }
    const bool ended = run->simulation == SimulationEnd::ReachedStopAt || run->simulation == SimulationEnd::Ended;
    if (!ended || errors() > 0 || (strict && warnings() > 0))
    {
        return RunResult::Failed;
    }
    return RunResult::Passed;
}

int RunReport::exit_code() const
{
    switch (result())
    {
    case RunResult::Passed: return 0;
    case RunResult::Failed:
    case RunResult::CheckFailed: return 1;
    case RunResult::Unreadable:
    case RunResult::SetupFailed: return 3;
    case RunResult::Usage: return 4;
    case RunResult::Interrupted: return kExitInterrupted;
    }
    return 1;
}

std::string format_run_text(const RunReport& report)
{
    const RunResult result = report.result();
    if (result == RunResult::Unreadable || result == RunResult::Usage)
    {
        return format_text(report.check);
    }
    std::string out = detail::check_text_body(report.check);
    const std::string counts = detail::count_summary(report.errors(), report.warnings(), report.notes());
    if (!report.run)
    {
        out += "nothing was started: the check found " +
               std::string(report.check.errors() > 0 ? "errors" : "warnings, and --strict fails on them") +
               " (vehicle_sil_launch check " + report.check.file + " shows the same)\n\nfailed: " + counts + "\n";
        return out;
    }
    const RunOutcome& run = *report.run;
    std::string state;
    switch (run.simulation)
    {
    case SimulationEnd::NotStarted:
        state = !run.setup_error.empty()      ? "the simulation could not be set up"
                : !run.interrupted_by.empty() ? "interrupted (" + run.interrupted_by + ") before the simulation started"
                                              : "the simulation did not start";
        break;
    case SimulationEnd::ReachedStopAt:
        state = "the simulation ran to stop_at_ms (" + std::to_string(run.stop_at_ms.value_or(0)) + " ms) and ended";
        break;
    case SimulationEnd::Ended:
        state = run.controller_state.empty() ? "the simulation ran until a participant ended it"
                                             : "the simulation ended in state " + run.controller_state;
        break;
    case SimulationEnd::Aborted:
        state = run.timed_out ? "the simulation was aborted: it did not end within --timeout-ms (" +
                                    std::to_string(run.timeout_ms.value_or(0)) + " ms)"
                : run.failed_participant.empty() ? "the simulation was aborted"
                                                 : "the simulation was aborted: participant '" + run.failed_participant + "' failed";
        break;
    case SimulationEnd::Interrupted:
        state = "interrupted (" + run.interrupted_by + "): the simulation was stopped";
        break;
    }
    out += "run: " + state + "\n\n";
    if (!run.participants.empty())
    {
        std::vector<std::vector<std::string>> rows{{"participant", "required", "ended"}};
        for (const auto& p : run.participants)
        {
            rows.push_back({p.name, p.required ? "yes" : "no", ending_cell(p)});
        }
        out += detail::table_text(rows) + "\n";
    }
    out += detail::findings_text(report.check.file, report.findings);
    switch (result)
    {
    case RunResult::Passed: out += "passed: " + counts + "\n"; break;
    case RunResult::Interrupted: out += "interrupted: " + counts + "\n"; break;
    default:
        out += "failed: " + counts + (report.errors() == 0 && run.setup_error.empty() ? " (--strict)" : "") + "\n";
        break;
    }
    return out;
}

std::string format_run_json(const RunReport& report)
{
    vehicle_sil::detail::JsonWriter w;
    w.begin_object();
    w.integer("vehicle_sil_run", kRunFormat);
    w.string("vehicle_sil", version());
    if (report.check.file.empty())
    {
        w.null("file");
    }
    else
    {
        w.string("file", report.check.file);
    }
    w.string("result", result_name(report.result()));
    w.boolean("strict", report.strict);
    w.integer("errors", static_cast<std::int64_t>(report.errors()));
    w.integer("warnings", static_cast<std::int64_t>(report.warnings()));
    w.integer("notes", static_cast<std::int64_t>(report.notes()));
    if (report.run)
    {
        w.string("simulation", simulation_name(report.run->simulation));
    }
    else
    {
        w.null("simulation");
    }
    if (report.run && !report.run->interrupted_by.empty())
    {
        w.string("interrupted_by", report.run->interrupted_by);
    }
    else
    {
        w.null("interrupted_by");
    }
    w.begin_array("participants");
    if (report.run)
    {
        for (const auto& p : report.run->participants)
        {
            w.begin_object();
            w.string("name", p.name);
            w.boolean("required", p.required);
            w.string("ended", ending_name(p.ending));
            if (p.exit && !p.exit->signal)
            {
                w.integer("exit_code", p.exit->code);
            }
            else
            {
                w.null("exit_code");
            }
            if (p.exit && p.exit->signal)
            {
                w.string("signal", process::signal_name(*p.exit->signal));
            }
            else
            {
                w.null("signal");
            }
            w.boolean("left_processes", p.left_processes);
            if (p.log.empty())
            {
                w.null("log");
            }
            else
            {
                w.string("log", p.log);
            }
            w.end_object();
        }
    }
    w.end_array();
    detail::write_findings_json(w, report.check.file, report.findings);
    detail::write_check_json(w, "check", report.check);
    w.end_object();
    std::string out = w.take();
    out += '\n';
    return out;
}

std::string format_internal_error_json(bool run, const std::string& message)
{
    CheckReport check;
    check.findings.push_back(Finding{Severity::Error, "internal", message, 0, "", ""});
    if (!run)
    {
        return format_json(check);
    }
    // The run's own findings: where a reader of the run's JSON looks first.
    RunReport report;
    report.findings = std::move(check.findings);
    return format_run_json(report);
}

namespace {

namespace orch = SilKit::Services::Orchestration;

} // namespace

std::string detail::log_name(const std::string& name)
{
    std::string out;
    for (const char c : name)
    {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                           c == '-' || c == '_';
        out += plain ? c : '_';
    }
    // Windows reads these names (also before an extension) as devices: the same rule everywhere,
    // so that one configuration gives the same files.
    static constexpr std::string_view kDevices[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                                                    "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                                    "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    std::string stem = out.substr(0, out.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (std::find(std::begin(kDevices), std::end(kDevices), stem) != std::end(kDevices))
    {
        out.insert(stem.size(), "_");
    }
    return out;
}

std::int64_t detail::ecu_start_timeout_ms(const SimulationConfig& config)
{
    // The launcher waits start_timeout_ms for the start and then stops the participants within
    // stop_timeout_ms; the ECU must not give up first (it would end, and the launcher would name it
    // instead of who did not join). 10 s more for a slow machine; never more than ecu_main accepts.
    return std::min(config.start_timeout_ms + config.stop_timeout_ms + 10000, kMaxMillis);
}

detail::StopRequest detail::record_stop_request(ParticipantRun& run, bool sent, const std::optional<process::ExitStatus>& status)
{
    if (sent)
    {
        return StopRequest::Sent;
    }
    if (status)
    {
        run.exit = status;
        run.ending = Ending::Exited;
        run.stopped_after_end = false;
        return StopRequest::HadEnded;
    }
    run.stop_request_failed = true;
    return StopRequest::CannotSend;
}

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

// A child's output must not move the cursor or end the line early on the launcher's terminal.
std::string detail::printable(std::string_view line)
{
    std::string out;
    out.reserve(line.size());
    for (std::size_t i = 0; i < line.size();)
    {
        const auto byte = static_cast<unsigned char>(line[i]);
        if (byte < 0x80)
        {
            out += (byte < 0x20 && byte != '\t') || byte == 0x7F ? '?' : static_cast<char>(byte);
            ++i;
            continue;
        }
        // A UTF-8 sequence: its length, the smallest value of that length, the value.
        const std::size_t length = byte >= 0xF5 ? 0 : byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC2 ? 2 : 0;
        std::uint32_t value = length == 4 ? byte & 0x07u : length == 3 ? byte & 0x0Fu : byte & 0x1Fu;
        bool valid = length != 0 && i + length <= line.size();
        for (std::size_t k = 1; valid && k < length; ++k)
        {
            const auto next = static_cast<unsigned char>(line[i + k]);
            valid = (next & 0xC0) == 0x80;
            value = (value << 6) | (next & 0x3Fu);
        }
        const std::uint32_t smallest = length == 4 ? 0x10000 : length == 3 ? 0x800 : 0x80;
        valid = valid && value >= smallest && value <= 0x10FFFF && (value < 0xD800 || value > 0xDFFF);
        if (!valid)
        {
            out += '?'; // not UTF-8: one '?' per byte that starts no character
            ++i;
            continue;
        }
        // The C1 control characters (U+0080 to U+009F) steer a terminal too.
        if (value <= 0x9F)
        {
            out += '?';
        }
        else
        {
            out.append(line.substr(i, length));
        }
        i += length;
    }
    return out;
}

namespace {

using detail::log_name;
using detail::printable;
using detail::uri_for_participants;

/// `path` without its "." parts ("dir/./engine" as "dir/engine"); ".." stays (it may cross a
/// symbolic link).
std::filesystem::path without_dots(const std::filesystem::path& path)
{
    std::filesystem::path out;
    for (const auto& part : path)
    {
        if (part != ".")
        {
            out /= part;
        }
    }
    return out;
}

std::string command_text(const std::filesystem::path& executable, const std::vector<std::string>& arguments)
{
    std::string out = process::shown_path(executable);
    for (const auto& argument : arguments)
    {
        out += ' ';
        const bool plain = !argument.empty() && argument.find_first_of(" \t\"'") == std::string::npos;
        out += plain ? argument : "'" + argument + "'";
    }
    return out;
}

class Runner
{
public:
    Runner(const SimulationConfig& config, const CheckReport& check, std::filesystem::path base, const RunOptions& options) :
      config_(config), check_(check), base_(std::move(base)), options_(options)
    {
        outcome_.start_timeout_ms = config.start_timeout_ms;
        outcome_.stop_timeout_ms = config.stop_timeout_ms;
        outcome_.stop_at_ms = config.stop_at_ms;
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
            run.line = p.line;
            outcome_.participants.push_back(std::move(run));
        }
        children_.resize(config.participants.size());
        logs_.resize(config.participants.size());
    }

    RunOutcome run()
    {
        if (interrupts() > 0)
        {
            outcome_.interrupted_by = interrupt_name();
            return outcome_;
        }
        if (!open_logs())
        {
            return outcome_;
        }
        std::unique_ptr<SilKit::Vendor::Vector::ISilKitRegistry> registry;
        std::unique_ptr<SimulationController> controller;
        std::string uri;
        try
        {
            registry = SilKit::Vendor::Vector::CreateSilKitRegistry(SilKit::Config::ParticipantConfigurationFromString("{}"));
            const std::string listen = config_.registry_uri.value_or("silkit://127.0.0.1:0");
            try
            {
                uri = uri_for_participants(registry->StartListening(listen));
            }
            catch (const std::exception& e)
            {
                throw std::runtime_error("the registry cannot listen at " + listen + ": " + one_line(e.what()) +
                                         (config_.registry_uri ? " (does another registry run there? choose another port in "
                                                                 "registry_uri, or leave it out for a free port)"
                                                               : ""));
            }
            say("registry at " + uri);
            SimulationControllerOptions c;
            c.participant_name = std::string(kControllerName);
            c.registry_uri = uri;
            for (const auto& p : config_.participants)
            {
                if (p.required)
                {
                    c.required_participants.push_back(p.name);
                }
            }
            if (config_.stop_at_ms)
            {
                c.stop_at = std::chrono::milliseconds(*config_.stop_at_ms);
            }
            controller = std::make_unique<SimulationController>(std::move(c));
        }
        catch (const std::exception& e)
        {
            outcome_.setup_error = one_line(e.what());
            return outcome_;
        }

        bool ran = false;
        if (start_children(uri) && wait_for_start(*controller))
        {
            ran = true;
            watch(*controller);
        }
        end_children(ran);
        controller.reset();
        registry.reset();
        return outcome_;
    }

private:
    [[nodiscard]] int interrupts() const { return options_.interrupts != nullptr ? options_.interrupts->load() : 0; }
    [[nodiscard]] std::string interrupt_name() const
    {
        return options_.interrupt_name ? options_.interrupt_name() : std::string("interrupt");
    }

    static std::string one_line(std::string text)
    {
        std::replace(text.begin(), text.end(), '\n', ' ');
        std::replace(text.begin(), text.end(), '\r', ' ');
        return text;
    }

    void write(const std::string& line)
    {
        if (options_.write_line)
        {
            options_.write_line(line);
        }
    }

    void say(const std::string& text)
    {
        const std::lock_guard lock(output_);
        write(std::string(kLauncherTag) + text);
    }

    bool open_logs()
    {
        if (options_.log_dir.empty())
        {
            return true;
        }
        std::error_code ec;
        std::filesystem::create_directories(options_.log_dir, ec);
        // Compared without case: on Windows and macOS "Engine.log" and "engine.log" are one file.
        std::set<std::string> used;
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        for (std::size_t i = 0; i < config_.participants.size(); ++i)
        {
            std::string name = log_name(config_.participants[i].name);
            for (int n = 2; !used.insert(lower(name)).second; ++n)
            {
                name = log_name(config_.participants[i].name) + "-" + std::to_string(n);
            }
            const auto file = options_.log_dir / process::path_from_utf8(name + ".log");
            outcome_.participants[i].log = process::shown_path(file);
            logs_[i] = std::make_unique<std::ofstream>(file, std::ios::binary | std::ios::trunc);
            if (!*logs_[i])
            {
                outcome_.setup_error = "cannot write the log " + process::shown_path(file) +
                                       ": check that --log-dir names a directory you can write to";
                return false;
            }
        }
        return true;
    }

    void on_line(std::size_t index, std::string_view line)
    {
        const std::lock_guard lock(output_);
        write("[" + config_.participants[index].name + "] " + printable(line));
        if (logs_[index])
        {
            *logs_[index] << line << '\n' << std::flush;
        }
    }

    /// Starts every participant in the file's order. false if a required one could not be started.
    bool start_children(const std::string& uri)
    {
        const auto workDir = without_dots(std::filesystem::absolute(base_));
        for (std::size_t i = 0; i < config_.participants.size(); ++i)
        {
            const ParticipantConfig& p = config_.participants[i];
            ParticipantRun& run = outcome_.participants[i];
            const std::vector<std::string> rest(p.command.begin() + 1, p.command.end());
            std::vector<std::string> arguments;
            if (p.describe)
            {
                arguments = expand_placeholders(rest, "", "");
                arguments.insert(arguments.end(), {"--name", p.name, "--registry-uri", uri, "--start-timeout-ms",
                                                   std::to_string(detail::ecu_start_timeout_ms(config_))});
            }
            else
            {
                arguments = expand_placeholders(rest, uri, p.name);
            }
            const std::filesystem::path executable = without_dots(std::filesystem::absolute(check_.participants[i].executable));
            say("starting " + p.name + ": " + command_text(executable, arguments));
            process::ProcessOptions options;
            options.working_directory = workDir;
            options.on_line = [this, i](process::Stream, std::string_view line) { on_line(i, line); };
            try
            {
                children_[i] = std::make_unique<process::Process>(executable, arguments, std::move(options));
            }
            catch (const std::exception& e)
            {
                run.start_error = one_line(e.what());
                say(p.name + " could not be started: " + run.start_error);
                if (p.required)
                {
                    return false;
                }
            }
        }
        return true;
    }

    /// Notes the participants whose process has ended since the last look; returns the first
    /// required one among them (or -1).
    int look_at_children(bool before_start)
    {
        int ended = -1;
        for (std::size_t i = 0; i < children_.size(); ++i)
        {
            ParticipantRun& run = outcome_.participants[i];
            if (!children_[i] || run.exit)
            {
                continue;
            }
            if (auto status = children_[i]->poll())
            {
                run.exit = status;
                run.ending = before_start ? Ending::ExitedBeforeStart : Ending::Exited;
                say(run.name + " " + exit_text(*status));
                if (run.required && ended < 0)
                {
                    ended = static_cast<int>(i);
                }
            }
        }
        return ended;
    }

    /// Waits until every required participant has joined, then starts the simulation. false if
    /// it did not start (outcome_ says why).
    bool wait_for_start(SimulationController& controller)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.start_timeout_ms);
        while (true)
        {
            if (interrupts() > 0)
            {
                outcome_.interrupted_by = interrupt_name();
                say("interrupted (" + outcome_.interrupted_by + ") before the simulation started");
                return false;
            }
            // Who has not joined is read before the ends of the processes: at the deadline, who
            // did not come is named even if a process ends in the same round.
            const std::vector<std::string> missing = controller.missing_participants();
            const bool late = !missing.empty() && std::chrono::steady_clock::now() >= deadline;
            const int ended = look_at_children(true);
            if (ended >= 0 && !late)
            {
                outcome_.failed_participant = outcome_.participants[static_cast<std::size_t>(ended)].name;
                return false;
            }
            if (missing.empty())
            {
                break;
            }
            if (late)
            {
                outcome_.start_timed_out = true;
                std::string names;
                for (auto& run : outcome_.participants)
                {
                    if (std::find(missing.begin(), missing.end(), run.name) != missing.end())
                    {
                        run.missing_at_start = true;
                        names += (names.empty() ? "" : ", ") + run.name;
                    }
                }
                say("the simulation did not start: not joined within " + std::to_string(config_.start_timeout_ms) +
                    " ms: " + names);
                return false;
            }
            std::this_thread::sleep_for(options_.poll);
        }
        try
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            controller.start(std::max(left, std::chrono::milliseconds(1000)));
        }
        catch (const std::exception& e)
        {
            outcome_.start_error = one_line(e.what());
            say("the simulation did not start: " + outcome_.start_error);
            return false;
        }
        say("simulation running");
        return true;
    }

    void watch(SimulationController& controller)
    {
        WatchOptions watch;
        watch.poll = options_.poll;
        watch.interrupted = [this] { return interrupts() > 0; };
        watch.failed = [this, &controller] {
            // A required participant whose process failed fails the run at once; one that exited
            // with 0 may have stopped the simulation (the controller sees that a moment later).
            // Once the simulation stops, participants leave on purpose (a coordinated participant
            // ends only after the controller has stopped): their exit codes are judged after it.
            const bool running = controller.runtime().state() == orch::ParticipantState::Running;
            std::vector<bool> known;
            for (const auto& run : outcome_.participants)
            {
                known.push_back(run.exit.has_value());
            }
            static_cast<void>(look_at_children(false));
            for (std::size_t i = 0; i < known.size(); ++i)
            {
                const ParticipantRun& run = outcome_.participants[i];
                if (running && !known[i] && run.required && failed_exit(run))
                {
                    outcome_.failed_participant = run.name;
                    return true;
                }
            }
            return false;
        };
        if (options_.timeout)
        {
            watch.deadline = std::chrono::steady_clock::now() + *options_.timeout;
        }
        watch.before_abort = [this](const WatchResult& why) {
            if (why.end == WatchEnd::TimedOut)
            {
                say("the simulation did not end within --timeout-ms (" + std::to_string(options_.timeout->count()) +
                    " ms): aborting it");
            }
            else
            {
                const std::string who = why.failure ? why.failure->participant : outcome_.failed_participant;
                say("participant '" + who + "' failed" + (why.failure ? ": it " + why.failure->reason : std::string{}) +
                    "; aborting the simulation");
            }
        };
        const WatchResult watched = watch_simulation(controller, watch);
        // An interrupt from here on comes while the launcher waits for the participants to end.
        interrupts_at_end_ = interrupts();
        switch (watched.end)
        {
        case WatchEnd::Ended:
            if (watched.state == orch::ParticipantState::Shutdown)
            {
                outcome_.simulation = config_.stop_at_ms && controller.wall_time_to_stop_at() ? SimulationEnd::ReachedStopAt
                                                                                               : SimulationEnd::Ended;
            }
            else
            {
                outcome_.simulation = SimulationEnd::Ended;
                outcome_.controller_state = watched.state ? orch::to_string(*watched.state) : std::string("unknown");
            }
            say("the simulation ended");
            break;
        case WatchEnd::TimedOut:
            outcome_.simulation = SimulationEnd::Aborted;
            outcome_.timed_out = true;
            say("the simulation was aborted: it did not end within --timeout-ms");
            break;
        case WatchEnd::Failed:
            outcome_.simulation = SimulationEnd::Aborted;
            if (watched.failure && outcome_.failed_participant.empty())
            {
                outcome_.failed_participant = watched.failure->participant;
                outcome_.failure_reason = watched.failure->reason;
            }
            say("the simulation was aborted: participant '" + outcome_.failed_participant + "' failed");
            break;
        case WatchEnd::Interrupted:
            outcome_.simulation = SimulationEnd::Interrupted;
            outcome_.interrupted_by = interrupt_name();
            say("interrupted (" + outcome_.interrupted_by + "): the simulation was stopped");
            break;
        }
    }

    [[nodiscard]] bool any_running(bool required_only)
    {
        static_cast<void>(look_at_children(false));
        for (std::size_t i = 0; i < children_.size(); ++i)
        {
            const ParticipantRun& run = outcome_.participants[i];
            if (children_[i] && !run.exit && run.ending != Ending::Killed && (!required_only || run.required))
            {
                return true;
            }
        }
        return false;
    }

    enum class Waited
    {
        Done,
        TimedOut,
        Interrupted,
    };

    /// Waits up to `timeout` until `done()`; stops waiting once more than `interrupts_allowed`
    /// interrupts have come.
    template <class Done>
    Waited wait_until(std::chrono::milliseconds timeout, int interrupts_allowed, Done done)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done())
        {
            if (interrupts() > interrupts_allowed)
            {
                return Waited::Interrupted;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return Waited::TimedOut;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return Waited::Done;
    }

    /// Ends every participant: after a simulation, first gives the required ones stop_timeout_ms
    /// to end by themselves; then asks the rest to stop, waits stop_timeout_ms, and ends what is
    /// left with the processes it started.
    void end_children(bool ran)
    {
        const std::chrono::milliseconds stopTimeout(config_.stop_timeout_ms);
        // An interrupt while the launcher waits for the participants to end by themselves stops
        // the waiting (a first one too: the simulation has ended already); a second interrupt in
        // all skips the wait after the stop requests as well.
        bool waitedInFull = false;
        if (ran)
        {
            const Waited waited = wait_until(stopTimeout, interrupts_at_end_, [this] { return !any_running(true); });
            waitedInFull = waited == Waited::TimedOut;
            if (waited == Waited::Interrupted && outcome_.interrupted_by.empty())
            {
                outcome_.interrupted_by = interrupt_name();
                outcome_.interrupted_while_ending = true;
                say("interrupted (" + outcome_.interrupted_by + "): no more waiting for the participants to end");
            }
        }
        std::vector<std::size_t> asked;
        for (std::size_t i = 0; i < children_.size(); ++i)
        {
            ParticipantRun& run = outcome_.participants[i];
            if (!children_[i] || run.exit)
            {
                continue;
            }
            run.stopped_after_end = waitedInFull && run.required;
            const bool sent = children_[i]->request_stop();
            switch (detail::record_stop_request(run, sent, children_[i]->poll()))
            {
            case detail::StopRequest::Sent:
                say("asking " + run.name + " to stop (" + std::string(stop_request_name()) + ")");
                asked.push_back(i);
                break;
            case detail::StopRequest::HadEnded:
                say(run.name + " " + exit_text(*run.exit));
                break;
            case detail::StopRequest::CannotSend:
                say("cannot ask " + run.name + " to stop: ending it");
                children_[i]->kill_tree();
                run.ending = Ending::Killed;
                break;
            }
        }
        if (!asked.empty())
        {
            // The first process of each and what it started: a wrapper (sh -c) may end before the
            // program it started has cleaned up.
            static_cast<void>(wait_until(stopTimeout, 1, [this, &asked] {
                return std::all_of(asked.begin(), asked.end(), [this](std::size_t i) {
                    return children_[i]->poll().has_value() && !children_[i]->others_running();
                });
            }));
        }
        for (const std::size_t i : asked)
        {
            ParticipantRun& run = outcome_.participants[i];
            if (auto status = children_[i]->poll())
            {
                run.exit = status;
                run.ending = Ending::Stopped;
            }
            else
            {
                say("ending " + run.name + " (it did not stop)");
                children_[i]->kill_tree();
                run.ending = Ending::Killed;
            }
        }
        for (std::size_t i = 0; i < children_.size(); ++i)
        {
            if (!children_[i])
            {
                continue;
            }
            ParticipantRun& run = outcome_.participants[i];
            if (run.ending != Ending::Killed && children_[i]->others_running())
            {
                run.left_processes = true;
                say(run.name + " left processes running: ending them");
                children_[i]->kill_tree();
            }
        }
        for (auto& child : children_)
        {
            if (child)
            {
                static_cast<void>(child->finish_output(std::chrono::milliseconds(2000)));
            }
        }
    }

    const SimulationConfig& config_;
    const CheckReport& check_;
    std::filesystem::path base_;
    const RunOptions& options_;
    RunOutcome outcome_;
    int interrupts_at_end_ = 0; ///< interrupts() when the simulation had ended
    std::vector<std::unique_ptr<std::ofstream>> logs_;
    std::mutex output_;
    // Last, so that it goes first: a Process ends its reader thread, which uses output_ and logs_,
    // also when run() leaves with an exception.
    std::vector<std::unique_ptr<process::Process>> children_;
};

} // namespace

RunReport run_file(const std::filesystem::path& path, const std::string& shown_as, const RunOptions& options)
{
    RunReport report;
    report.strict = options.check.strict;
    report.check = check_file(path, shown_as, options.check);
    if (!report.check.config || !report.check.passed())
    {
        return report;
    }
    const std::filesystem::path base = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    Runner runner(*report.check.config, report.check, base, options);
    report.run = runner.run();
    report.findings = assess_run(*report.run);
    return report;
}

} // namespace vehicle_sil::launch
