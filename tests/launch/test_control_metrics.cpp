// SPDX-License-Identifier: MIT
//
// Opt-in Control Hub transport metrics (`run --control-metrics`): pure JSON formatting across
// available and unavailable lifecycles (control_metrics_format.hpp) and atomic file writing
// (probe.hpp). No SIL Kit participant or child process.

#include "launch_test_util.hpp"

#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/launch/control_metrics_format.hpp"
#include "vehicle_sil/launch/probe.hpp"
#include "vehicle_sil/launch/run.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace vehicle_sil::launch;
using namespace vehicle_sil::runner;
using namespace vehicle_sil::config;
using namespace vehicle_sil::config::test;

RunReport base_report()
{
    RunReport r;
    r.check.file = "simulation.yaml";
    r.check.config.emplace();
    r.check.config->participants.push_back(ParticipantConfig{});
    return r;
}

std::string read_all(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    CHECK(static_cast<bool>(in));
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::size_t count_writing_temps(const std::filesystem::path& dir)
{
    std::size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (entry.path().filename().string().find(".writing-") != std::string::npos)
        {
            ++n;
        }
    }
    return n;
}

} // namespace

TEST_CASE(control_metrics_formats_available_report_and_large_uint64_counters)
{
    RunReport r = base_report();
    r.check.config->variant = "bench";
    RunOutcome outcome;
    outcome.end.simulation = SimulationEnd::ReachedStopAt;
    outcome.timing.sim_ms = 1000;
    outcome.timing.wall_ms = 42;
    outcome.timing.time_step_ms = 1;
    ControlTransportStats stats;
    stats.messages = 5000;
    stats.bytes = 10'000'000'000'000'000'000ULL; // > INT64_MAX
    stats.handler_calls = 5015;
    stats.handler_time_ns = 18423900;
    stats.max_handler_time_ns = 148200;
    stats.hellos = 5;
    stats.last_words = 5;
    stats.late_messages = 1;
    stats.undecodable = 2;
    stats.unknown = 3;
    stats.refused_hellos = 4;
    outcome.control.transport_stats = stats;
    r.run = outcome;

    const std::string v = vehicle_sil::version();
    const std::string got = format_control_metrics_json(r);
    CHECK(got.find('\r') == std::string::npos);
    check_text(got, R"({
  "vehicle_sil_control_metrics": 1,
  "vehicle_sil": ")" + v + R"(",
  "file": "simulation.yaml",
  "variant": "bench",
  "status": "available",
  "unavailable_reason": null,
  "source": {
    "transport": "silkit",
    "collector": "HubTransport::stats",
    "scope": "cumulative_hub_transport_lifetime",
    "collection_point": "before_transport_close"
  },
  "run": {
    "result": "passed",
    "end": "completed",
    "simulation": "reached_stop_at",
    "ok": true,
    "sim_ms": 1000,
    "wall_ms": 42,
    "time_step_ms": 1,
    "interrupted_by": null
  },
  "counters": {
    "messages": 5000,
    "bytes": 10000000000000000000,
    "handler_calls": 5015,
    "handler_time_ns": 18423900,
    "max_handler_time_ns": 148200,
    "hellos": 5,
    "last_words": 5,
    "late_messages": 1,
    "undecodable": 2,
    "unknown": 3,
    "refused_hellos": 4
  }
}
)");
}

TEST_CASE(control_metrics_emits_null_timing_on_not_started_and_before_start_error)
{
    // Transport created, participant crashed before start (NotStarted).
    {
        RunReport r = base_report();
        RunOutcome outcome;
        outcome.end.simulation = SimulationEnd::NotStarted;
        outcome.timing.time_step_ms = 10;
        outcome.control.transport_stats = ControlTransportStats{};
        r.run = outcome;
        const std::string got = format_control_metrics_json(r);
        CHECK(got.find("\"status\": \"available\"") != std::string::npos);
        CHECK(got.find("\"sim_ms\": null") != std::string::npos);
        CHECK(got.find("\"wall_ms\": null") != std::string::npos);
        CHECK(got.find("\"time_step_ms\": 10") != std::string::npos);
    }
    // before_start_error sets SimulationEnd::Aborted without calling watch_.settle() / note_timing().
    {
        RunReport r = base_report();
        RunOutcome outcome;
        outcome.end.simulation = SimulationEnd::Aborted;
        outcome.start.before_start_error = "observer threw";
        outcome.timing.time_step_ms = 5;
        outcome.control.transport_stats = ControlTransportStats{};
        r.run = outcome;
        const std::string got = format_control_metrics_json(r);
        CHECK(got.find("\"status\": \"available\"") != std::string::npos);
        CHECK(got.find("\"result\": \"failed\"") != std::string::npos);
        CHECK(got.find("\"simulation\": \"aborted\"") != std::string::npos);
        CHECK(got.find("\"sim_ms\": null") != std::string::npos);
        CHECK(got.find("\"wall_ms\": null") != std::string::npos);
        CHECK(got.find("\"time_step_ms\": 5") != std::string::npos);
    }
}

TEST_CASE(control_metrics_formats_all_unavailable_reasons_with_null_counters)
{
    // 1. unreadable
    {
        RunReport r;
        r.check.file = "missing.yaml";
        const std::string got = format_control_metrics_json(r);
        CHECK(got.find("\"status\": \"unavailable\"") != std::string::npos);
        CHECK(got.find("\"unavailable_reason\": \"unreadable\"") != std::string::npos);
        CHECK(got.find("\"collection_point\": null") != std::string::npos);
        CHECK(got.find("\"result\": \"unreadable\"") != std::string::npos);
        CHECK(got.find("\"end\": null") != std::string::npos);
        CHECK(got.find("\"simulation\": null") != std::string::npos);
        CHECK(got.find("\"ok\": false") != std::string::npos);
        CHECK(got.find("\"sim_ms\": null") != std::string::npos);
        CHECK(got.find("\"wall_ms\": null") != std::string::npos);
        CHECK(got.find("\"time_step_ms\": null") != std::string::npos);
        CHECK(got.find("\"counters\": null") != std::string::npos);
    }
    // 2. check_failed
    {
        RunReport r = base_report();
        Finding f;
        f.severity = Severity::Error;
        f.code = "check";
        f.message = "bad config";
        r.check.findings.push_back(f);
        const std::string got = format_control_metrics_json(r);
        CHECK(got.find("\"status\": \"unavailable\"") != std::string::npos);
        CHECK(got.find("\"unavailable_reason\": \"check_failed\"") != std::string::npos);
        CHECK(got.find("\"result\": \"check_failed\"") != std::string::npos);
        CHECK(got.find("\"time_step_ms\": null") != std::string::npos);
        CHECK(got.find("\"counters\": null") != std::string::npos);
    }
    // 3. stopped_before_setup (operator stop during Checking before prepare())
    {
        RunReport r = base_report();
        RunOutcome outcome;
        outcome.end.simulation = SimulationEnd::NotStarted;
        outcome.end.interrupted_by = "stop command";
        outcome.end.interrupted_kind = StopKind::Operator;
        outcome.timing.time_step_ms = 10;
        r.run = outcome;
        const std::string got = format_control_metrics_json(r);
        CHECK(got.find("\"status\": \"unavailable\"") != std::string::npos);
        CHECK(got.find("\"unavailable_reason\": \"stopped_before_setup\"") != std::string::npos);
        CHECK(got.find("\"collection_point\": null") != std::string::npos);
        CHECK(got.find("\"result\": \"stopped\"") != std::string::npos);
        CHECK(got.find("\"end\": \"stopped\"") != std::string::npos);
        CHECK(got.find("\"simulation\": \"not_started\"") != std::string::npos);
        CHECK(got.find("\"ok\": true") != std::string::npos);
        CHECK(got.find("\"sim_ms\": null") != std::string::npos);
        CHECK(got.find("\"wall_ms\": null") != std::string::npos);
        CHECK(got.find("\"time_step_ms\": 10") != std::string::npos);
        CHECK(got.find("\"interrupted_by\": \"stop command\"") != std::string::npos);
        CHECK(got.find("\"counters\": null") != std::string::npos);
    }
    // 4. interrupted_before_setup (signal/EOF during Checking before prepare())
    {
        RunReport r = base_report();
        RunOutcome outcome;
        outcome.end.simulation = SimulationEnd::NotStarted;
        outcome.end.interrupted_by = "SIGINT";
        outcome.end.interrupted_kind = StopKind::Signal;
        outcome.timing.time_step_ms = 10;
        r.run = outcome;
        const std::string got = format_control_metrics_json(r);
        CHECK(got.find("\"status\": \"unavailable\"") != std::string::npos);
        CHECK(got.find("\"unavailable_reason\": \"interrupted_before_setup\"") != std::string::npos);
        CHECK(got.find("\"result\": \"interrupted\"") != std::string::npos);
        CHECK(got.find("\"end\": \"interrupted\"") != std::string::npos);
        CHECK(got.find("\"interrupted_by\": \"SIGINT\"") != std::string::npos);
        CHECK(got.find("\"counters\": null") != std::string::npos);
    }
    // 5. setup_failed
    {
        RunReport r = base_report();
        RunOutcome outcome;
        outcome.setup.error = "registry cannot listen";
        outcome.end.simulation = SimulationEnd::NotStarted;
        outcome.timing.time_step_ms = 10;
        r.run = outcome;
        const std::string got = format_control_metrics_json(r);
        CHECK(got.find("\"status\": \"unavailable\"") != std::string::npos);
        CHECK(got.find("\"unavailable_reason\": \"setup_failed\"") != std::string::npos);
        CHECK(got.find("\"result\": \"setup_failed\"") != std::string::npos);
        CHECK(got.find("\"end\": \"setup_failed\"") != std::string::npos);
        CHECK(got.find("\"time_step_ms\": 10") != std::string::npos);
        CHECK(got.find("\"counters\": null") != std::string::npos);
    }
    // 6. internal_error
    {
        const std::string got = format_control_metrics_internal_error_json("sim.yaml", "bench");
        CHECK(got.find("\"status\": \"unavailable\"") != std::string::npos);
        CHECK(got.find("\"unavailable_reason\": \"internal_error\"") != std::string::npos);
        CHECK(got.find("\"collection_point\": null") != std::string::npos);
        CHECK(got.find("\"file\": \"sim.yaml\"") != std::string::npos);
        CHECK(got.find("\"variant\": \"bench\"") != std::string::npos);
        CHECK(got.find("\"result\": null") != std::string::npos);
        CHECK(got.find("\"end\": null") != std::string::npos);
        CHECK(got.find("\"simulation\": null") != std::string::npos);
        CHECK(got.find("\"ok\": false") != std::string::npos);
        CHECK(got.find("\"sim_ms\": null") != std::string::npos);
        CHECK(got.find("\"wall_ms\": null") != std::string::npos);
        CHECK(got.find("\"time_step_ms\": null") != std::string::npos);
        CHECK(got.find("\"interrupted_by\": null") != std::string::npos);
        CHECK(got.find("\"counters\": null") != std::string::npos);
    }
}

TEST_CASE(control_metrics_error_does_not_alter_run_report_outputs_or_exit_code)
{
    RunReport clean = base_report();
    RunOutcome outcome;
    outcome.end.simulation = SimulationEnd::ReachedStopAt;
    outcome.timing.sim_ms = 100;
    outcome.timing.wall_ms = 10;
    outcome.timing.time_step_ms = 10;
    clean.run = outcome;

    RunReport with_err = clean;
    with_err.control_metrics_error = "permission denied";

    CHECK_EQ(with_err.exit_code(), clean.exit_code());
    CHECK_EQ(with_err.ok(), clean.ok());
    CHECK_EQ(with_err.errors(), clean.errors());
    CHECK(with_err.result() == clean.result());
    check_text(format_run_json(with_err), format_run_json(clean));
    check_text(format_run_text(with_err), format_run_text(clean));
}

TEST_CASE(write_control_metrics_file_writes_atomically_and_rejects_bad_paths)
{
    const std::filesystem::path dir = std::filesystem::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "control_metrics_write";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path target = dir / "nested" / "sub" / "metrics.json";
    CHECK(write_control_metrics_file(target, "{\"a\": 1}\n").empty());
    check_text(read_all(target), "{\"a\": 1}\n");
    CHECK_EQ(count_writing_temps(target.parent_path()), std::size_t{0});

    // Atomically replaces an existing file.
    CHECK(write_control_metrics_file(target, "{\"a\": 2}\n").empty());
    check_text(read_all(target), "{\"a\": 2}\n");
    CHECK_EQ(count_writing_temps(target.parent_path()), std::size_t{0});

    // Rejects empty path, directory paths, and existing directories without creating temp files.
    CHECK(!write_control_metrics_file(std::filesystem::path{}, "{}\n").empty());
    CHECK(!write_control_metrics_file(dir / "sub" / "", "{}\n").empty());
    CHECK(!write_control_metrics_file(std::filesystem::path("."), "{}\n").empty());
    CHECK(!write_control_metrics_file(std::filesystem::path(".."), "{}\n").empty());
    CHECK(!write_control_metrics_file(dir, "{}\n").empty());
    CHECK_EQ(count_writing_temps(dir), std::size_t{0});

    // Parent path is an existing regular file: fails cleanly without corrupting the file.
    const std::filesystem::path blocker = dir / "regular.txt";
    {
        std::ofstream out(blocker, std::ios::binary);
        out << "keep-me\n";
    }
    CHECK(!write_control_metrics_file(blocker / "metrics.json", "{}\n").empty());
    check_text(read_all(blocker), "keep-me\n");
    CHECK_EQ(count_writing_temps(dir), std::size_t{0});
}
