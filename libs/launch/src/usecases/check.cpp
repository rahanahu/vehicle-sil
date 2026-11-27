// SPDX-License-Identifier: MIT
//
// check_file(): the steps of the check. Reads the configuration (config.cpp), runs --describe of
// each enabled participant (or takes its saved output: --descriptions) and reads its output
// (describe_read.cpp), compares the descriptions with the file (check_rules.cpp), reads the
// scenario (scenario.cpp). What it needs from the computer (the files, the executables and their
// output, the environment) it gets from probe.cpp, in the order of the steps; the executables only
// through Executables. The outputs are in check_format.cpp; the counts of the report in
// check_report.cpp.
#include "format/exit_codes.hpp"
#include "usecases/check_describe.hpp"
#include "usecases/sha256.hpp"

#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/launch/probe.hpp"
#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/config/time_plan.hpp"
#include "vehicle_sil/config/detail/seen_ecu.hpp"

#include "vehicle_sil/core/registry_uri.hpp"
#include "vehicle_sil/process/paths.hpp"
#include "vehicle_sil/runner/detail/run_detail.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace vehicle_sil::launch {

namespace {

/// The names of the networks in `outside`.
std::vector<std::string> outside_names(const std::vector<config::NetworkOutside>& outside)
{
    std::vector<std::string> names;
    for (const config::NetworkOutside& network : outside)
    {
        names.push_back(network.network);
    }
    return names;
}

/// Empties NetworkOutside::arguments of a network that an error about connecting it is about: a
/// command that a run cannot use is not shown.
void drop_unusable_commands(config::CheckReport& report)
{
    for (config::NetworkOutside& outside : report.outside)
    {
        if (std::any_of(report.findings.begin(), report.findings.end(), [&](const config::Finding& f) {
                return f.severity == config::Severity::Error && f.network == outside.network && f.code.rfind("connect-", 0) == 0;
            }))
        {
            outside.arguments.reset();
        }
    }
}

/// What the check asks of executables: each participant's --describe, and the bridges of --connect
/// (looked for and asked --describe). Made once, from the request: with saved descriptions
/// (RunHost::descriptions) neither run_tool() nor bridge_tools() is called to make it, so such a
/// check has nothing that could start a process.
struct Executables
{
    DescribeSource describe;
    /// Empty: --connect is checked for its form and its names only (check_connections() without tools).
    std::optional<config::ConnectTools> bridges;
    /// RunHost::descriptions could not be used: why (the note descriptions-not-used).
    std::string unusable{};
};

Executables executables_for(const RunHost& host, const std::filesystem::path& directory)
{
    if (host.descriptions.empty())
    {
        return Executables{describe_by_running(host.describe_timeout, directory),
                           bridge_tools(host.tool_directory, host.describe_timeout), {}};
    }
    SavedDescriptions saved = read_saved_descriptions(process::path_from_utf8(host.descriptions));
    return Executables{describe_from_saved(std::move(saved.entries)), std::nullopt, std::move(saved.problem)};
}

} // namespace

namespace detail {

std::size_t describe_worker_count(std::size_t task_count, unsigned int hardware_threads) noexcept
{
    if (task_count <= 1)
    {
        return task_count;
    }
    const std::size_t cap = std::clamp<std::size_t>(hardware_threads, 1, kMaxDescribeConcurrency);
    return std::min(task_count, cap);
}

std::vector<DescribeSlot> run_describe_tasks(const std::vector<DescribeTask>& tasks, const DescribeSource& describe,
                                             bool record_stamps, bool allow_parallel,
                                             std::size_t max_workers_override)
{
    std::vector<DescribeSlot> slots(tasks.size());
    const auto run_one = [&](std::size_t index) {
        const DescribeTask& task = tasks[index];
        DescribeSlot& slot = slots[index];
        if (record_stamps)
        {
            slot.before = executable_stamp(task.executable);
        }
        slot.run = describe(*task.participant, task.executable, task.arguments);
        if (record_stamps && slot.before && slot.run)
        {
            slot.after = executable_stamp(task.executable);
        }
    };

    const std::size_t worker_count = [&]() -> std::size_t {
        if (!allow_parallel || tasks.size() <= 1)
        {
            return 1;
        }
        if (max_workers_override > 0)
        {
            return std::min(tasks.size(), std::clamp<std::size_t>(max_workers_override, 1, kMaxDescribeConcurrency));
        }
        return describe_worker_count(tasks.size(), std::thread::hardware_concurrency());
    }();

    if (worker_count <= 1)
    {
        for (std::size_t i = 0; i < tasks.size(); ++i)
        {
            run_one(i);
        }
        return slots;
    }

    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::exception_ptr first_error;

    const auto worker = [&] {
        try
        {
            for (;;)
            {
                if (failed.load(std::memory_order_relaxed))
                {
                    return;
                }
                const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= tasks.size())
                {
                    return;
                }
                // run_one() runs run_captured() synchronously to completion on this thread before
                // claiming the next task or returning, preserving Linux PR_SET_PDEATHSIG.
                run_one(index);
            }
        }
        catch (...)
        {
            failed.store(true, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(error_mutex);
            if (!first_error)
            {
                first_error = std::current_exception();
            }
        }
    };

    struct JoinAll
    {
        std::vector<std::thread> threads;
        ~JoinAll()
        {
            for (std::thread& t : threads)
            {
                if (t.joinable())
                {
                    t.join();
                }
            }
        }
    };

    {
        JoinAll pool;
        // Spawn (worker_count - 1) background threads and run the calling thread as worker #0 so
        // total concurrency never exceeds worker_count and all tasks still finish if thread
        // creation fails partway through.
        pool.threads.reserve(worker_count - 1);
        for (std::size_t w = 1; w < worker_count; ++w)
        {
            try
            {
                pool.threads.emplace_back(worker);
            }
            catch (...)
            {
                break;
            }
        }
        worker();
    }

    if (first_error)
    {
        std::rethrow_exception(first_error);
    }
    return slots;
}

} // namespace detail

int check_exit_code(const config::CheckReport& report) { return exit_code_of(report); }

namespace {

/// The note for the params of a request file that nothing uses: one file serves several
/// scenarios, so they are left alone.
[[nodiscard]] config::Finding unused_request_params(const std::string& file, const std::vector<std::string>& names, const std::string& why)
{
    std::string list;
    for (const std::string& name : names)
    {
        list += (list.empty() ? "" : ", ") + name;
    }
    return config::Finding{config::Severity::Note, "request-params-unused",
                   std::string("params ") + list + " in " + file + " " + (names.size() == 1 ? "is" : "are") +
                       " not used: " + why + "; they are ignored",
                   0, "", ""};
}

/// --scenario: reads the scenario file and checks it against the participants of `report`.
/// @returns the usage errors that only the scenario can tell (ScenarioRead::usage_errors); then
/// `report` has no scenario.
[[nodiscard]] std::vector<config::Finding> check_scenario_file(Checked& report, const RunIntent& intent, const std::string& named,
                                                       bool params_for_many,
                                                       const config::RequestOrigin& origin)
{
    scenario::ScenarioCheck check;
    check.file = named;
    const auto add = [&](config::Finding finding) {
        if (finding.file.empty())
        {
            finding.file = check.file;
        }
        report.findings.push_back(std::move(finding));
    };
    const std::filesystem::path path = process::path_from_utf8(check.file);
    std::string text;
    const std::string problem = read_scenario_file(path, text);
    if (!problem.empty())
    {
        add(config::Finding{config::Severity::Error, "file", problem, 0, "", ""});
    }
    else
    {
        scenario::ScenarioContext context;
        context.variant = report.config->variant;
        context.variants = report.variants;
        context.params = report.config->params;
        context.params.insert(context.params.end(), intent.params.begin(), intent.params.end());
        context.params_for_many = params_for_many;
        if (origin.has("scenario"))
        {
            context.request_file = origin.file; // the file's own scenario: its params must fit it
        }
        context.scenario_file = check.file;
        context.read_file = files_beside(path);
        context.read_template = files_beside(path, "the template file");
        context.case_name = intent.scenario_case;
        scenario::ScenarioRead read = scenario::read_scenario(text, scenario::scenario_catalog(report), context);
        check.name = std::move(read.name);
        check.case_name = std::move(read.case_name);
        check.cases = std::move(read.cases);
        check.case_details = std::move(read.case_details);
        check.steps = read.steps;
        check.end_ms = read.end_ms;
        check.scenario = std::move(read.scenario);
        check.params = std::move(read.params);
        check.param_meta = std::move(read.param_meta);
        check.missing_params = std::move(read.missing_params);
        check.only = std::move(read.only);
        check.skipped = read.skipped;
        check.unchecked_expects = read.unchecked_expects;
        check.not_for_variant = read.not_for_variant;
        if (!read.usage_errors.empty())
        {
            return std::move(read.usage_errors);
        }
        if (!read.unused_request_params.empty())
        {
            report.findings.push_back(unused_request_params(origin.file, read.unused_request_params,
                                                            "the scenario has no parameter of that name"));
        }
        for (config::Finding& finding : read.findings)
        {
            add(std::move(finding));
        }
        if (check.scenario)
        {
            // What the run will judge with: how often the ECUs it reads send their values.
            for (config::Finding& finding : scenario::scenario_samples_findings(report, *check.scenario, version()))
            {
                add(std::move(finding));
            }
        }
    }
    report.scenario = std::move(check);
    return {};
}

/// check --compare-commands: compares what runs of `text` with what runs of the configuration file
/// `other` (read as the configuration file is, from the working directory): CheckReport::commands
/// and its note.
void compare_commands_with(Checked& report, std::string_view text, const std::string& other)
{
    std::string other_text;
    if (const std::string problem = read_config_file(process::path_from_utf8(other), other_text); !problem.empty())
    {
        report.commands = config::CommandsCompared::NotCompared;
        report.findings.push_back(config::commands_not_compared(other, problem));
        return;
    }
    config::CommandsComparison compared = config::compare_commands(text, other_text, other);
    report.commands = compared.result;
    if (compared.note)
    {
        report.findings.push_back(std::move(*compared.note));
    }
}

} // namespace

HotEligibility evaluate_hot_eligibility(const Checked& checked, bool trace_enabled, bool refuse_fixed_registry)
{
    const auto first_error_message = [](const std::vector<config::Finding>& findings, std::string_view fallback) {
        for (const config::Finding& f : findings)
        {
            if (f.severity == config::Severity::Error && !f.message.empty())
            {
                return f.message;
            }
        }
        return std::string(fallback);
    };
    HotEligibility out;
    if (checked.usage)
    {
        out.reason = "usage";
        out.message = first_error_message(checked.findings, "invalid prepare arguments");
        return out;
    }
    if (!checked.config)
    {
        out.reason = "config-unreadable";
        out.message = first_error_message(checked.findings, "configuration could not be read");
        return out;
    }
    if (refuse_fixed_registry && checked.config->registry_uri.has_value())
    {
        out.reason = "fixed-registry";
        out.message = "configuration uses a fixed registry_uri (" + *checked.config->registry_uri +
                      "): use run instead of serve";
        return out;
    }
    if (refuse_fixed_registry && config::waits_for_manual(*checked.config))
    {
        out.reason = "manual-participants";
        out.message = "configuration waits for manual participants: use run instead of serve";
        return out;
    }
    if (trace_enabled)
    {
        out.reason = "trace-enabled";
        out.message = "serve does not run with VEHICLE_SIL_TRACE set";
        return out;
    }
    out.eligible = true;
    return out;
}

Checked check_file(const RunRequest& request, const CheckOptions& options,
                       const std::function<void(const config::TimeLock& lock)>& on_time_lock)
{
    const RunIntent& intent = request.intent;
    const RunHost& host = request.host;
    const std::filesystem::path path = process::path_from_utf8(host.config);
    config::EnabledOverrides overrides;
    overrides.disable = intent.disable;
    overrides.enable = intent.enable;
    overrides.manual = intent.manual;
    overrides.variant = intent.variant;
    overrides.origin = request.origin;
    // A test's default speed does not count as a request that the time lock may refuse.
    const std::optional<std::optional<double>> requested_speed =
        options.fastest_by_default ? std::optional<std::optional<double>>{} : intent.speed;
    const std::optional<double> default_speed = options.fastest_by_default ? intent.speed.value_or(std::nullopt)
                                                                          : std::nullopt;
    // What of the request's connect counts for this file: known once it is read.
    std::vector<config::ConnectRequest> connect;
    Checked report;
    report.file = host.config;
    report.strict = intent.strict;
    const auto finalize = [&options](Checked& out) {
        config::sort_findings(out.findings);
        if (!out.hot_eligibility.eligible && out.hot_eligibility.reason.empty())
        {
            out.hot_eligibility =
                evaluate_hot_eligibility(out, options.trace_enabled, /*refuse_fixed_registry=*/true);
        }
    };
    // Known without the file.
    if (auto invalid = validate_timing(request); !invalid.empty())
    {
        report.usage = true;
        report.findings = std::move(invalid);
        finalize(report);
        return report;
    }
    if (auto conflicts = config::override_conflicts(overrides); !conflicts.empty())
    {
        report.usage = true;
        report.findings = std::move(conflicts);
        finalize(report);
        return report;
    }
    std::string text;
    if (const std::string problem = host.config_from_stdin ? read_config_stdin(text) : read_config_file(path, text);
        !problem.empty())
    {
        report.findings.push_back(config::Finding{config::Severity::Error, "file", problem, 0, "", ""});
        if (!host.compare_commands.empty())
        {
            report.commands = config::CommandsCompared::NotCompared;
            report.findings.push_back(config::commands_not_compared(host.compare_commands, config::kCheckedTextUnreadable));
        }
        finalize(report);
        return report;
    }
    report.file_sha256 = detail::sha256_hex(text);
    config::ConfigRead read = config::read_config(text, overrides);
    if (read.config)
    {
        for (const config::VariantConfig& variant : read.config->variants)
        {
            report.variants.push_back(variant.name);
        }
    }
    if (!read.override_errors.empty())
    {
        report.usage = true;
        report.findings = std::move(read.override_errors);
        finalize(report);
        return report;
    }
    report.findings = std::move(read.findings);
    report.config = std::move(read.config);
    // It only reads: also for file_only, and whatever the rest of the check finds.
    if (!host.compare_commands.empty())
    {
        compare_commands_with(report, text, host.compare_commands);
    }
    if (report.config)
    {
        config::ConfigConnect counted = config::connect_for_config(*report.config, intent.connect, request.origin);
        connect = std::move(counted.connect);
        report.findings.insert(report.findings.end(), counted.notes.begin(), counted.notes.end());
        // The file and the request decide it: before any executable is asked.
        report.time_lock = config::time_lock(*report.config, connect, intent.time_policy);
        const std::vector<config::Finding> time = config::time_lock_findings(
            report.time_lock, requested_speed, default_speed, intent.start_paused, request.origin,
            intent.after_scenario);
        report.findings.insert(report.findings.end(), time.begin(), time.end());
        if (on_time_lock)
        {
            on_time_lock(report.time_lock);
        }
        const HotEligibility hot =
            evaluate_hot_eligibility(report, options.trace_enabled, /*refuse_fixed_registry=*/true);
        if (options.refuse_fixed_registry &&
            (hot.reason == "fixed-registry" || hot.reason == "manual-participants"))
        {
            // serve: before any executable is looked for or described, so nothing is started.
            const bool written = hot.reason == "fixed-registry";
            config::Finding finding{config::Severity::Error, "usage",
                                    std::string("serve does not run a configuration whose registry listens at a fixed address (") +
                                      (written ? "registry_uri: " + *report.config->registry_uri
                                               : "a participant you start yourself (manual) waits at " +
                                                   std::string(runner::kManualRegistryUri)) +
                                      "): a participant of an earlier run that starts late could join the next run there, and the "
                                      "run ID does not reach SIL Kit. Run it with vehicle_sil_launch run (a new launcher for each run)" +
                                      (written ? ", or take registry_uri out of the file (a free port is chosen for each run)" : ""),
                                    0, "", ""};
            Checked usage;
            usage.file = std::move(report.file);
            usage.strict = report.strict;
            usage.variants = std::move(report.variants);
            usage.file_sha256 = std::move(report.file_sha256);
            usage.hot_eligibility = hot;
            usage.usage = true;
            usage.findings = {std::move(finding)};
            finalize(usage);
            return usage;
        }
    }
    if (report.config && options.file_only && !connect.empty())
    {
        // Only the form and the names: no tool is looked for.
        config::ConnectCheck checked = config::check_connections(*report.config, {}, connect, nullptr);
        report.findings.insert(report.findings.end(), checked.findings.begin(), checked.findings.end());
        report.outside = std::move(checked.outside);
        drop_unusable_commands(report);
    }
    if (!report.config || options.file_only)
    {
        finalize(report);
        return report;
    }
    const std::optional<std::string> listen = environment_value(kListenOnNetworkVariable);
    if (auto ignored = config::inherited_listen_variable(*report.config, listen ? listen->c_str() : nullptr))
    {
        report.findings.push_back(std::move(*ignored));
    }
    const std::filesystem::path base = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    // As vehicle_sil_launch run starts them: in the directory of the configuration file.
    const Executables executables = executables_for(host, std::filesystem::absolute(base));
    if (!executables.unusable.empty())
    {
        report.findings.push_back(config::Finding{config::Severity::Note, "descriptions-not-used",
                                          "the saved descriptions (--descriptions) are not used: " + executables.unusable +
                                              "; no participant is described: check without --descriptions to describe "
                                              "them, with --write-descriptions to save them again",
                                          0, "", ""});
    }
    // Phase A (sequential): resolve executables and collect runnable --describe tasks in
    // participant order, deferring findings emission to Phase C so findings and participants stay
    // byte-identical to sequential execution.
    struct PreparedParticipant
    {
        config::ParticipantResult result{};
        std::optional<config::Finding> finding{};
        std::optional<std::size_t> task_index{};
    };
    std::vector<PreparedParticipant> prepared;
    prepared.reserve(report.config->participants.size());
    std::vector<detail::DescribeTask> tasks;
    for (std::size_t i = 0; i < report.config->participants.size(); ++i)
    {
        const config::ParticipantConfig& p = report.config->participants[i];
        PreparedParticipant& entry = prepared.emplace_back();
        // Disabled whatever its entry's errors (they are findings).
        if (!p.enabled)
        {
            entry.result.status = config::ParticipantStatus::Disabled;
            continue;
        }
        if (!p.valid)
        {
            continue;
        }
        // "{{word}}" in the path is the literal "{word}" (a valid entry has no other "{word}").
        const std::string written = config::expand_placeholders({p.command.front()}, "", "").front();
        const FoundExecutable found = find_executable(written, base);
        const std::filesystem::path& executable = found.path;
        if (auto bad = config::executable_finding(p, executable, executable_status(executable)))
        {
            entry.finding = std::move(*bad);
            entry.result.status = config::ParticipantStatus::Failed;
            continue;
        }
        entry.result.executable = executable;
        if (found.on_path)
        {
            entry.finding = config::Finding{config::Severity::Note, "found-on-path",
                                            "participant '" + p.name + "': " + written +
                                                " is not in the directory of the configuration file; it runs " +
                                                process::shown_path(executable) + " (found on PATH)",
                                            p.command_line, p.name, ""};
        }
        if (!p.describe)
        {
            entry.result.status = config::ParticipantStatus::NotChecked;
            continue;
        }
        // "{{word}}" stands for "{word}" here too (an ecu_main command has no placeholders).
        std::vector<std::string> arguments =
            config::expand_placeholders(std::vector<std::string>(p.command.begin() + 1, p.command.end()), "", "");
        arguments.insert(arguments.end(), {"--describe", "--name", p.name});
        entry.task_index = tasks.size();
        tasks.push_back(detail::DescribeTask{i, &p, executable, std::move(arguments)});
    }
    // Phase B: run live --describe processes with bounded concurrency when more than one
    // participant is described; saved descriptions (--descriptions) stay sequential.
    const bool allow_parallel = host.descriptions.empty();
    const std::vector<detail::DescribeSlot> slots =
        detail::run_describe_tasks(tasks, executables.describe, !host.write_descriptions.empty(), allow_parallel);
    // Phase C (sequential): fold results in exact participant order 0..N-1.
    // What --write-descriptions saves: each output that was read.
    std::vector<SavedDescription> described;
    report.participants.reserve(prepared.size());
    for (std::size_t i = 0; i < prepared.size(); ++i)
    {
        const config::ParticipantConfig& p = report.config->participants[i];
        PreparedParticipant& entry = prepared[i];
        if (entry.finding)
        {
            report.findings.push_back(std::move(*entry.finding));
        }
        if (entry.task_index)
        {
            const detail::DescribeSlot& slot = slots[*entry.task_index];
            if (!slot.run)
            {
                entry.result.status = config::ParticipantStatus::NotDescribed;
            }
            else
            {
                config::DescribeOutcome outcome = config::interpret_describe(p, *slot.run);
                if (outcome.error)
                {
                    report.findings.push_back(std::move(*outcome.error));
                    entry.result.status = config::ParticipantStatus::Failed;
                }
                else
                {
                    entry.result.status = config::ParticipantStatus::Described;
                    entry.result.ecu = std::move(outcome.ecu);
                    if (slot.before && slot.before == slot.after)
                    {
                        SavedDescription& saved = described.emplace_back();
                        saved.name = p.name;
                        saved.command = p.command;
                        saved.executable = *slot.before;
                        saved.output = slot.run->standard_output;
                    }
                }
            }
        }
        report.participants.push_back(std::move(entry.result));
    }
    if (!host.write_descriptions.empty())
    {
        if (const std::string problem =
                write_saved_descriptions(process::path_from_utf8(host.write_descriptions), described);
            !problem.empty())
        {
            report.findings.push_back(config::Finding{config::Severity::Error, "descriptions-write",
                                              "cannot write the descriptions to " + host.write_descriptions +
                                                  " (--write-descriptions): " + problem +
                                                  "; the rest of this check is not affected: name a file in a directory "
                                                  "that exists and may be written",
                                              0, "", ""});
        }
    }
    if (!connect.empty())
    {
        config::ConnectCheck checked = config::check_connections(*report.config, report.participants, connect,
                                                 executables.bridges ? &*executables.bridges : nullptr);
        report.findings.insert(report.findings.end(), checked.findings.begin(), checked.findings.end());
        report.outside = std::move(checked.outside);
    }
    const std::vector<std::string> outside = outside_names(report.outside);
    auto compared = config::check_participants(*report.config, report.participants, version(), outside);
    report.findings.insert(report.findings.end(), compared.begin(), compared.end());
    drop_unusable_commands(report);
    report.run_timing = config::detail::plan_time_step(*report.config, report, intent.stop_at_ms);
    report.run_timing->speed = config::planned_speed(report.time_lock, requested_speed, default_speed);
    report.run_timing->late_tolerance_ms = intent.late_tolerance_ms.value_or(config::kDefaultLateToleranceMs);
    report.stop_at_ms = intent.stop_at_ms;
    report.start_timeout_ms = host.start_timeout_ms;
    report.stop_timeout_ms = host.stop_timeout_ms;
    report.checked_speed = intent.speed;
    report.checked_start_paused = intent.start_paused;
    report.checked_after_scenario = intent.after_scenario;
    if (auto f = config::detail::stop_not_on_step(intent.stop_at_ms, report.run_timing->time_step_ms))
    {
        report.findings.push_back(std::move(*f));
    }
    if (auto f = config::detail::coarse_time_step(*report.run_timing))
    {
        report.findings.push_back(std::move(*f));
    }
    if (intent.scenario && !intent.scenario->empty())
    {
        // A request file's scenario is beside the configuration, as the file's own paths are.
        std::string named = *intent.scenario;
        if (request.origin.has("scenario") && process::path_from_utf8(named).is_relative() && !path.parent_path().empty())
        {
            named = process::shown_path(path.parent_path() / process::path_from_utf8(named));
        }
        if (auto wrong = check_scenario_file(report, intent, named, options.params_for_many, request.origin); !wrong.empty())
        {
            // As for the other usage errors: the findings alone, and no configuration.
            Checked usage;
            usage.file = std::move(report.file);
            usage.strict = report.strict;
            usage.variants = std::move(report.variants);
            usage.file_sha256 = std::move(report.file_sha256);
            usage.usage = true;
            usage.findings = std::move(wrong);
            finalize(usage);
            return usage;
        }
    }
    else
    {
        std::vector<std::string> unused;
        for (const config::ParamValue& param : intent.params)
        {
            if (param.source == config::ParamSource::RequestFile)
            {
                unused.push_back(param.name);
            }
        }
        if (!unused.empty())
        {
            report.findings.push_back(unused_request_params(request.origin.file, unused, "this run has no scenario"));
        }
    }
    finalize(report);
    return report;
}

} // namespace vehicle_sil::launch
