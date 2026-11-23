// SPDX-License-Identifier: MIT
//
// Bounded parallel --describe execution in check_file(): deterministic ordering, bounded
// concurrency, sequential fallback for non-parallel sources and single tasks, exception join
// safety, and live speedup with multiple child processes.
#include "launch_test_util.hpp"
#include "test_timeout_scale.hpp"
#include "usecases/check_describe.hpp"

#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/launch/probe.hpp"
#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::config;
using namespace vehicle_sil::launch;

namespace {

fs::path scratch(const std::string& name)
{
    const fs::path dir = fs::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "check_parallel" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::string yaml_quote(const std::string& text)
{
    std::string out = "'";
    for (const char c : text)
    {
        out += c == '\'' ? std::string("''") : std::string(1, c);
    }
    return out + "'";
}

std::string ecu_json(const std::string& name, int step_ms, const std::string& network, int address)
{
    return "{\"vehicle_sil_ecu\": 1, \"name\": \"" + name + "\", \"step_ns\": " + std::to_string(step_ms * 1000000) +
           ", \"networks\": [{\"name\": \"" + network + "\", \"kind\": \"can\", \"use\": \"j1939\", "
           "\"nodes\": [{\"address\": " + std::to_string(address) +
           ", \"j1939_name\": null, \"arbitrary_address_capable\": false}]}]}";
}

std::vector<ParticipantConfig> make_participants(std::size_t count)
{
    std::vector<ParticipantConfig> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        ParticipantConfig p;
        p.name = "Ecu" + std::to_string(i);
        p.command = {"ecu_" + std::to_string(i)};
        p.command_line = static_cast<int>(10 + i * 2);
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<launch::detail::DescribeTask> make_tasks(const std::vector<ParticipantConfig>& participants)
{
    std::vector<launch::detail::DescribeTask> tasks;
    tasks.reserve(participants.size());
    for (std::size_t i = 0; i < participants.size(); ++i)
    {
        tasks.push_back(launch::detail::DescribeTask{i, &participants[i], fs::path(participants[i].command.front()),
                                             {"--describe", "--name", participants[i].name}});
    }
    return tasks;
}

void write_file(const fs::path& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary);
    out << content;
    CHECK(static_cast<bool>(out));
}

} // namespace

TEST_CASE(check_parallel_worker_count_is_bounded)
{
    CHECK_EQ(launch::detail::describe_worker_count(0, 16), std::size_t{0});
    CHECK_EQ(launch::detail::describe_worker_count(1, 0), std::size_t{1});
    CHECK_EQ(launch::detail::describe_worker_count(1, 16), std::size_t{1});
    // Unknown hardware_concurrency() (0) falls back safely to 1.
    CHECK_EQ(launch::detail::describe_worker_count(8, 0), std::size_t{1});
    CHECK_EQ(launch::detail::describe_worker_count(8, 1), std::size_t{1});
    CHECK_EQ(launch::detail::describe_worker_count(8, 2), std::size_t{2});
    CHECK_EQ(launch::detail::describe_worker_count(8, 4), std::size_t{4});
    // Never more workers than runnable tasks.
    CHECK_EQ(launch::detail::describe_worker_count(3, 24), std::size_t{3});
    // Always capped at kMaxDescribeConcurrency.
    CHECK_EQ(launch::detail::describe_worker_count(10, 24), launch::detail::kMaxDescribeConcurrency);
    CHECK_EQ(launch::detail::describe_worker_count(64, 128), launch::detail::kMaxDescribeConcurrency);
}

TEST_CASE(check_parallel_bounded_concurrency_and_slot_order)
{
    const auto participants = make_participants(12);
    const auto tasks = make_tasks(participants);

    const auto check_cap = [&](std::size_t override_cap, std::size_t expected_max) {
        std::atomic<std::size_t> in_flight{0};
        std::atomic<std::size_t> peak{0};
        const DescribeSource source = [&](const ParticipantConfig& p, const fs::path&,
                                          const std::vector<std::string>&) -> std::optional<DescribeRun> {
            const std::size_t current = in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
            std::size_t observed = peak.load(std::memory_order_relaxed);
            while (current > observed && !peak.compare_exchange_weak(observed, current, std::memory_order_relaxed))
            {
            }
            // Staggered delays so later indices finish before earlier ones.
            const auto delay = std::chrono::milliseconds(10 + (12 - (p.command_line - 10) / 2) * 3);
            std::this_thread::sleep_for(delay);
            DescribeRun run;
            run.exit_code = 0;
            run.standard_output = "out:" + p.name;
            in_flight.fetch_sub(1, std::memory_order_acq_rel);
            return run;
        };

        const std::vector<launch::detail::DescribeSlot> slots =
            launch::detail::run_describe_tasks(tasks, source, false, true, override_cap);
        CHECK_EQ(slots.size(), tasks.size());
        CHECK_EQ(in_flight.load(), std::size_t{0});
        CHECK(peak.load() <= expected_max);
        if (expected_max >= 2)
        {
            CHECK(peak.load() >= std::size_t{2});
        }
        for (std::size_t i = 0; i < tasks.size(); ++i)
        {
            CHECK(slots[i].run.has_value());
            vehicle_sil::config::test::check_text(slots[i].run->standard_output, "out:" + participants[i].name);
        }
    };

    check_cap(3, 3);
    // Overrides above kMaxDescribeConcurrency are still clamped to kMaxDescribeConcurrency.
    check_cap(100, launch::detail::kMaxDescribeConcurrency);
    const std::size_t default_cap = launch::detail::describe_worker_count(tasks.size(), std::thread::hardware_concurrency());
    check_cap(0, default_cap);
}

TEST_CASE(check_parallel_sequential_source_runs_on_calling_thread_without_races)
{
    const auto participants = make_participants(8);
    const auto tasks = make_tasks(participants);
    const std::thread::id caller = std::this_thread::get_id();

    // Deliberately non-thread-safe state (plain vector + int, no mutex or atomic): safe only if
    // run_describe_tasks() executes strictly sequentially on the calling thread.
    std::vector<std::string> visited;
    int calls = 0;
    const DescribeSource custom = [&](const ParticipantConfig& p, const fs::path&,
                                      const std::vector<std::string>&) -> std::optional<DescribeRun> {
        CHECK(std::this_thread::get_id() == caller);
        visited.push_back(p.name);
        ++calls;
        DescribeRun run;
        run.exit_code = 0;
        run.standard_output = p.name;
        return run;
    };

    const std::vector<launch::detail::DescribeSlot> slots = launch::detail::run_describe_tasks(tasks, custom, false, false);
    CHECK_EQ(calls, 8);
    CHECK_EQ(visited.size(), std::size_t{8});
    for (std::size_t i = 0; i < participants.size(); ++i)
    {
        vehicle_sil::config::test::check_text(visited[i], participants[i].name);
        CHECK(slots[i].run.has_value() && slots[i].run->standard_output == participants[i].name);
    }

    // A single runnable task also stays on the calling thread even when allow_parallel is true.
    const std::vector<launch::detail::DescribeTask> single{tasks.front()};
    visited.clear();
    calls = 0;
    const std::vector<launch::detail::DescribeSlot> single_slots = launch::detail::run_describe_tasks(single, custom, false, true);
    CHECK_EQ(calls, 1);
    CHECK_EQ(single_slots.size(), std::size_t{1});
    CHECK(single_slots[0].run.has_value() && single_slots[0].run->standard_output == participants[0].name);
}

TEST_CASE(check_parallel_exception_joins_all_workers_cleanly)
{
    const auto participants = make_participants(8);
    const auto tasks = make_tasks(participants);
    std::atomic<int> active{0};
    const DescribeSource throwing = [&](const ParticipantConfig& p, const fs::path&,
                                        const std::vector<std::string>&) -> std::optional<DescribeRun> {
        active.fetch_add(1, std::memory_order_acq_rel);
        struct Guard
        {
            std::atomic<int>& count;
            ~Guard() { count.fetch_sub(1, std::memory_order_acq_rel); }
        } guard{active};
        if (p.name == "Ecu2")
        {
            std::this_thread::sleep_for(5ms);
            throw std::runtime_error("unexpected describe error");
        }
        std::this_thread::sleep_for(25ms);
        DescribeRun run;
        run.exit_code = 0;
        return run;
    };

    bool caught = false;
    try
    {
        static_cast<void>(launch::detail::run_describe_tasks(tasks, throwing, false, true, 4));
    }
    catch (const std::runtime_error& e)
    {
        caught = std::string(e.what()) == "unexpected describe error";
    }
    CHECK(caught);
    // Every worker thread must already have finished and joined before the exception escaped.
    CHECK_EQ(active.load(), 0);
}

TEST_CASE(check_parallel_live_mixed_outcomes_match_sequential_byte_for_byte)
{
    const fs::path dir = scratch("mixed");
    const std::string helper = yaml_quote(process::shown_path(fs::path(VEHICLE_SIL_PROCESS_HELPER)));
    const fs::path config_path = dir / "sim.yaml";
    const fs::path saved_path = dir / "saved.json";

    // 10 participants mixing success (with staggered delays so later entries finish first),
    // missing executable, non-zero exit, disabled, invalid JSON, describe: false, timeout, and
    // unsupported format version.
    write_file(config_path,
               "vehicle_sil_simulation: 2\n"
               "networks:\n"
               "  - name: BusA\n"
               "    kind: can\n"
               "participants:\n"
               "  - name: GoodSlow\n"
               "    command: [" + helper + ", --sleep-ms, \"60\", --out, " + yaml_quote(ecu_json("GoodSlow", 10, "BusA", 0)) + "]\n"
               "    step_ms: 10\n"
               "  - name: Missing\n"
               "    command: [missing/ecu]\n"
               "  - name: BadExit\n"
               "    command: [" + helper + ", --sleep-ms, \"15\", --err, \"bad flag\\n\", --exit, \"4\"]\n"
               "  - name: Disabled\n"
               "    command: [missing/disabled]\n"
               "    enabled: false\n"
               "  - name: InvalidJson\n"
               "    command: [" + helper + ", --out, \"not-json\\n\"]\n"
               "  - name: Unchecked\n"
               "    command: [" + helper + ", --uri, \"{registry_uri}\"]\n"
               "    describe: false\n"
               "  - name: TimedOut\n"
               "    command: [" + helper + ", --err, \"hanging\\n\", --sleep-ms, \"60000\"]\n"
               "  - name: WrongVersion\n"
               "    command: [" + helper + ", --out, '{\"vehicle_sil_ecu\": 2}']\n"
               "  - name: GoodFast\n"
               "    command: [" + helper + ", --out, " + yaml_quote(ecu_json("GoodFast", 5, "BusA", 1)) + "]\n"
               "    step_ms: 5\n"
               "  - name: GoodMedium\n"
               "    command: [" + helper + ", --sleep-ms, \"30\", --out, " + yaml_quote(ecu_json("GoodMedium", 20, "BusA", 2)) + "]\n"
               "    step_ms: 20\n");

    const std::chrono::milliseconds timeout = vehicle_sil::test::scaled_timeout(500ms);
    RunRequest request;
    request.host.config = process::utf8_from_path(config_path);
    request.host.describe_timeout = timeout;
    request.host.write_descriptions = process::utf8_from_path(saved_path);

    const Checked parallel = check_file(request);
    CHECK(parallel.config.has_value());
    CHECK_EQ(parallel.participants.size(), std::size_t{10});
    CHECK_EQ(check_exit_code(parallel), 1);

    // Exact status at every participant index.
    CHECK(parallel.participants[0].status == ParticipantStatus::Described);
    CHECK(parallel.participants[1].status == ParticipantStatus::Failed);
    CHECK(parallel.participants[2].status == ParticipantStatus::Failed);
    CHECK(parallel.participants[3].status == ParticipantStatus::Disabled);
    CHECK(parallel.participants[4].status == ParticipantStatus::Failed);
    CHECK(parallel.participants[5].status == ParticipantStatus::NotChecked);
    CHECK(parallel.participants[6].status == ParticipantStatus::Failed);
    CHECK(parallel.participants[7].status == ParticipantStatus::Failed);
    CHECK(parallel.participants[8].status == ParticipantStatus::Described);
    CHECK(parallel.participants[9].status == ParticipantStatus::Described);

    // Build the exact sequential reference over the same runnable tasks using run_describe_tasks(..., false)
    // and verify every DescribeRun field matches byte-for-byte on all deterministic tasks, and that
    // repeated live parallel check_file() calls yield byte-identical JSON and text reports.
    const std::string json_first = format_json(parallel);
    const std::string text_first = format_text(parallel);
    const SavedDescriptions saved = read_saved_descriptions(saved_path);
    CHECK(saved.problem.empty());
    CHECK_EQ(saved.entries.size(), std::size_t{3});
    vehicle_sil::config::test::check_text(saved.entries[0].name, "GoodSlow");
    vehicle_sil::config::test::check_text(saved.entries[1].name, "GoodFast");
    vehicle_sil::config::test::check_text(saved.entries[2].name, "GoodMedium");

    // Verify exact findings sequence (errors in line order, then notes in line order).
    std::string codes;
    for (const Finding& f : parallel.findings)
    {
        codes += f.code + "(" + f.participant + ") ";
    }
    vehicle_sil::config::test::check_text(
        codes,
        "executable(Missing) describe-failed(BadExit) describe-output(InvalidJson) "
        "describe-timeout(TimedOut) describe-version(WrongVersion) "
        "participant-disabled(Disabled) not-checked(Unchecked) ");

    // Repeated live parallel run must produce byte-identical JSON and text output.
    request.host.write_descriptions.clear();
    const Checked parallel_second = check_file(request);
    vehicle_sil::config::test::check_text(format_json(parallel_second), json_first);
    vehicle_sil::config::test::check_text(format_text(parallel_second), text_first);
    CHECK_EQ(check_exit_code(parallel_second), check_exit_code(parallel));

    // Direct parallel vs sequential comparison on the real describe_by_running() source for every
    // runnable participant (including staggered success, non-zero exit, invalid JSON, wrong version,
    // and timeout).
    std::vector<launch::detail::DescribeTask> live_tasks;
    for (std::size_t i = 0; i < parallel.config->participants.size(); ++i)
    {
        const ParticipantConfig& p = parallel.config->participants[i];
        if (!p.enabled || !p.valid || !p.describe || parallel.participants[i].executable.empty())
        {
            continue;
        }
        std::vector<std::string> args =
            config::expand_placeholders(std::vector<std::string>(p.command.begin() + 1, p.command.end()), "", "");
        args.insert(args.end(), {"--describe", "--name", p.name});
        live_tasks.push_back(launch::detail::DescribeTask{i, &p, parallel.participants[i].executable, std::move(args)});
    }
    const DescribeSource live_source = describe_by_running(timeout, fs::absolute(dir));
    const std::vector<launch::detail::DescribeSlot> seq_slots =
        launch::detail::run_describe_tasks(live_tasks, live_source, true, false);
    const std::vector<launch::detail::DescribeSlot> par_slots =
        launch::detail::run_describe_tasks(live_tasks, live_source, true, true);
    CHECK_EQ(par_slots.size(), seq_slots.size());
    for (std::size_t i = 0; i < seq_slots.size(); ++i)
    {
        CHECK(seq_slots[i].run.has_value() && par_slots[i].run.has_value());
        CHECK(seq_slots[i].before == par_slots[i].before);
        CHECK(seq_slots[i].after == par_slots[i].after);
        CHECK(seq_slots[i].run->exit_code == par_slots[i].run->exit_code);
        CHECK(seq_slots[i].run->signal == par_slots[i].run->signal);
        vehicle_sil::config::test::check_text(par_slots[i].run->command_text, seq_slots[i].run->command_text);
        vehicle_sil::config::test::check_text(par_slots[i].run->standard_output, seq_slots[i].run->standard_output);
        vehicle_sil::config::test::check_text(par_slots[i].run->standard_error, seq_slots[i].run->standard_error);
        CHECK(seq_slots[i].run->start_error == par_slots[i].run->start_error);
    }

    // Saved descriptions (--descriptions) stays on the sequential non-spawning path.
    RunRequest from_saved;
    from_saved.host.config = process::utf8_from_path(config_path);
    from_saved.host.descriptions = process::utf8_from_path(saved_path);
    const Checked saved_report = check_file(from_saved);
    CHECK(saved_report.participants[0].status == ParticipantStatus::Described);
    CHECK(saved_report.participants[8].status == ParticipantStatus::Described);
    CHECK(saved_report.participants[9].status == ParticipantStatus::Described);
    CHECK(saved_report.participants[2].status == ParticipantStatus::NotDescribed);
    CHECK(saved_report.participants[6].status == ParticipantStatus::NotDescribed);
}

TEST_CASE(check_parallel_live_processes_run_faster_than_serial)
{
    const fs::path dir = scratch("speedup");
    const std::string helper = yaml_quote(process::shown_path(fs::path(VEHICLE_SIL_PROCESS_HELPER)));
    const fs::path config_path = dir / "slow_six.yaml";

    constexpr int kCount = 6;
    constexpr int kSleepMs = 200;
    std::string yaml = "vehicle_sil_simulation: 2\nnetworks:\n  - name: BusA\n    kind: can\nparticipants:\n";
    for (int i = 0; i < kCount; ++i)
    {
        const std::string name = "Ecu" + std::to_string(i);
        yaml += "  - name: " + name + "\n"
                "    command: [" + helper + ", --sleep-ms, \"" + std::to_string(kSleepMs) + "\", --out, " +
                yaml_quote(ecu_json(name, 10, "BusA", i)) + "]\n"
                "    step_ms: 10\n";
    }
    write_file(config_path, yaml);

    RunRequest request;
    request.host.config = process::utf8_from_path(config_path);

    const auto start = std::chrono::steady_clock::now();
    const Checked report = check_file(request);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    CHECK(report.passed());
    CHECK_EQ(report.participants.size(), std::size_t{kCount});
    for (int i = 0; i < kCount; ++i)
    {
        CHECK(report.participants[static_cast<std::size_t>(i)].status == ParticipantStatus::Described);
    }

    // Serial execution takes at least kCount * kSleepMs (1200 ms). With >= 2 hardware threads and
    // unscaled (non-sanitizer) execution, parallel execution finishes well below serial time.
    const std::size_t workers = launch::detail::describe_worker_count(kCount, std::thread::hardware_concurrency());
    if (workers >= 2 && vehicle_sil::test::timeout_scale() <= 1.0)
    {
        const auto serial_floor_ms = std::chrono::milliseconds(kCount * kSleepMs);
        CHECK(elapsed < serial_floor_ms - 150ms);
    }
}
