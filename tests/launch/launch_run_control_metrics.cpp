// SPDX-License-Identifier: MIT
//
// The launch_run scenarios for opt-in Control Hub transport metrics (`run --control-metrics`):
//
//   control_metrics  zero-regression opt-in guarantee (text, --json, --scenario + --timeline),
//                    counter sanity & invariants, pause+stop & stop lifecycles, participant crash
//                    before start & runtime failure/timeout, check_failed / unreadable / setup_failed
//                    unavailable states, write failure semantics (passing -> exit 3, failing -> exit 1),
//                    and CLI rejection on invalid/non-run subcommands.
#include "launch_run_support.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

/// Parses a non-negative integer member `"<key>": <n>` from pretty-printed JSON.
std::uint64_t parse_u64(const std::string& json, const std::string& key)
{
    const std::string needle = "\"" + key + "\": ";
    const std::size_t pos = json.find(needle);
    require(pos != std::string::npos, "missing key \"" + key + "\" in:\n" + json);
    const char* start = json.c_str() + pos + needle.size();
    char* end = nullptr;
    const unsigned long long val = std::strtoull(start, &end, 10);
    require(end != start, "non-integer value for \"" + key + "\" in:\n" + json);
    return static_cast<std::uint64_t>(val);
}

void require_available_invariants(const std::string& text, const std::string& what)
{
    require(!contains(text, "\r"), what + ": LF line ends only");
    require(contains(text, "\"vehicle_sil_control_metrics\": 1,"), what + ": format version 1:\n" + text);
    require(contains(text, "\"status\": \"available\","), what + ": status available:\n" + text);
    require(contains(text, "\"unavailable_reason\": null,"), what + ": unavailable_reason null:\n" + text);
    require(contains(text, "\"transport\": \"silkit\","), what + ": source.transport:\n" + text);
    require(contains(text, "\"collector\": \"HubTransport::stats\","), what + ": source.collector:\n" + text);
    require(contains(text, "\"scope\": \"cumulative_hub_transport_lifetime\","), what + ": source.scope:\n" + text);
    require(contains(text, "\"collection_point\": \"before_transport_close\""), what + ": source.collection_point:\n" + text);

    const std::uint64_t messages = parse_u64(text, "messages");
    const std::uint64_t bytes = parse_u64(text, "bytes");
    const std::uint64_t calls = parse_u64(text, "handler_calls");
    const std::uint64_t time_ns = parse_u64(text, "handler_time_ns");
    const std::uint64_t max_ns = parse_u64(text, "max_handler_time_ns");
    const std::uint64_t hellos = parse_u64(text, "hellos");
    const std::uint64_t last_words = parse_u64(text, "last_words");
    static_cast<void>(parse_u64(text, "late_messages"));
    static_cast<void>(parse_u64(text, "undecodable"));
    static_cast<void>(parse_u64(text, "unknown"));
    static_cast<void>(parse_u64(text, "refused_hellos"));

    require(calls >= messages, what + ": handler_calls >= messages:\n" + text);
    require(calls >= hellos, what + ": handler_calls >= hellos:\n" + text);
    require(time_ns >= max_ns, what + ": handler_time_ns >= max_handler_time_ns:\n" + text);
    if (calls == 0)
    {
        require(bytes == 0 && messages == 0 && hellos == 0 && last_words == 0 && time_ns == 0 && max_ns == 0,
                what + ": zero calls implies zero counters:\n" + text);
    }
}

void require_unavailable(const std::string& text, const std::string& reason, const std::string& what)
{
    require(!contains(text, "\r"), what + ": LF line ends only");
    require(contains(text, "\"vehicle_sil_control_metrics\": 1,"), what + ": format version 1:\n" + text);
    require(contains(text, "\"status\": \"unavailable\","), what + ": status unavailable:\n" + text);
    require(contains(text, "\"unavailable_reason\": \"" + reason + "\","), what + ": reason " + reason + ":\n" + text);
    require(contains(text, "\"collection_point\": null"), what + ": collection_point null:\n" + text);
    require(contains(text, "\"counters\": null"), what + ": counters null (never zeros):\n" + text);
}

std::string strip_registry_and_timing(std::string text)
{
    text = without_timing(std::move(text));
    // Normalize ephemeral registry port in stderr lines: "registry at silkit://127.0.0.1:<port>"
    std::size_t pos = 0;
    const std::string prefix = "silkit://127.0.0.1:";
    while ((pos = text.find(prefix, pos)) != std::string::npos)
    {
        const std::size_t start = pos + prefix.size();
        std::size_t end = start;
        while (end < text.size() && text[end] >= '0' && text[end] <= '9')
        {
            ++end;
        }
        if (end > start)
        {
            text.replace(start, end - start, "<port>");
            pos = start + 6;
        }
        else
        {
            pos = start;
        }
    }
    return text;
}

} // namespace

void control_metrics(const Options& o)
{
    // 1. Zero-regression opt-in guarantee & real-run counter sanity on examples/ecu_main (no --control/gRPC).
    {
        const fs::path ex = example_copy(o);
        const Captured base_text = launch(o, {"run", "simulation.yaml", "--stop-at-ms", "100"}, ex);
        expect_code(base_text, 0, "baseline text run");
        const Captured metrics_text =
            launch(o, {"run", "simulation.yaml", "--stop-at-ms", "100", "--control-metrics", "out/metrics.json"}, ex);
        expect_code(metrics_text, 0, "text run with --control-metrics");
        expect_text(metrics_text.standard_output, base_text.standard_output, "stdout unchanged by --control-metrics");
        require(!contains(lf(metrics_text.standard_error), "out/metrics.json") &&
                    !contains(lf(metrics_text.standard_error), "control metrics") &&
                    !contains(lf(metrics_text.standard_error), "control-metrics"),
                "stderr has no extra control-metrics noise:\n" + metrics_text.standard_error);

        const std::string m = read_text(ex / "out" / "metrics.json");
        require_available_invariants(m, "normal stop_at_ms run");
        require(parse_u64(m, "messages") > 0, "normal run recorded control messages:\n" + m);
        require(parse_u64(m, "bytes") > 0, "normal run recorded control bytes:\n" + m);
        require(parse_u64(m, "hellos") >= 2, "normal run recorded both ECU hellos:\n" + m);
        require(parse_u64(m, "last_words") >= 2, "normal run recorded both ECU last words:\n" + m);
        require(contains(m, "\"result\": \"passed\","), "normal run result:\n" + m);
        require(contains(m, "\"end\": \"completed\","), "normal run end:\n" + m);
        require(contains(m, "\"simulation\": \"reached_stop_at\","), "normal run simulation:\n" + m);
        require(contains(m, "\"ok\": true,"), "normal run ok:\n" + m);

        // --json stdout byte-for-byte unchanged (modulo timing/port).
        const Captured base_json = launch(o, {"run", "--json", "simulation.yaml", "--stop-at-ms", "100"}, ex);
        expect_code(base_json, 0, "baseline --json run");
        const Captured metrics_json = launch(
            o, {"run", "--json", "simulation.yaml", "--stop-at-ms", "100", "--control-metrics", "out/metrics2.json"}, ex);
        expect_code(metrics_json, 0, "--json run with --control-metrics");
        expect_text(strip_registry_and_timing(metrics_json.standard_output),
                    strip_registry_and_timing(base_json.standard_output), "run --json unchanged by --control-metrics");
        require(!contains(metrics_json.standard_output, "control_metrics"),
                "run --json does not add control_metrics fields:\n" + metrics_json.standard_output);

        // Orthogonality & determinism with --scenario + --timeline.
        const fs::path sc = scenario_ecus(o, "",
                                          "vehicle_sil_scenario: 1\n"
                                          "name: metrics check\n"
                                          "steps:\n"
                                          "  - set: { Body.level: 20, Door.mode: off }\n"
                                          "  - at_ms: 50\n"
                                          "    expect: { Body.level: 20, Door.mode_now: off }\n");
        const Captured tl_without =
            launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--timeline", "tl_a.json"}, sc);
        expect_code(tl_without, 0, "scenario+timeline without --control-metrics");
        const Captured tl_with = launch(
            o, {"run", "sim.yaml", "--scenario", "s.yaml", "--timeline", "tl_b.json", "--control-metrics", "sc_metrics.json"},
            sc);
        expect_code(tl_with, 0, "scenario+timeline with --control-metrics");
        expect_text(read_text(sc / "tl_b.json"), read_text(sc / "tl_a.json"),
                    "timeline output byte-for-byte identical with --control-metrics");
        require_available_invariants(read_text(sc / "sc_metrics.json"), "scenario run");
    }

    // 2. Pause + stop lifecycle via --start-paused --stdin-commands.
    {
        const fs::path dir = scenario_dir(o,
                                          "vehicle_sil_simulation: 2\n"
                                          "networks: []\n"
                                          "participants:\n"
                                          "  - name: Engine\n"
                                          "    command: [" + ecu(o) + "]\n",
                                          {{"Engine", "pid"}});
        std::mutex mutex;
        std::condition_variable changed;
        std::string err;
        bool running = false;
        vehicle_sil::process::ProcessOptions opts;
        opts.working_directory = dir;
        opts.input = true;
        opts.on_line = [&](vehicle_sil::process::Stream stream, std::string_view line) {
            const std::lock_guard lock(mutex);
            if (stream == vehicle_sil::process::Stream::Error)
            {
                err += std::string(line) + "\n";
                running = running || line == "[vehicle_sil_launch] simulation running";
                changed.notify_all();
            }
        };
        vehicle_sil::process::Process proc(
            o.launch, {"run", "--start-paused", "--stdin-commands", "--control-metrics", "paused_stop.json", "sim.yaml"},
            std::move(opts));
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, scaled_timeout(120s), [&] { return running || proc.poll().has_value(); }) && running,
                    "paused run did not start:\n" + err);
        }
        require(proc.write_input("{\"command\":\"stop\"}\n"), "write stop command");
        proc.close_input();
        const auto status = proc.wait_for(scaled_timeout(120s));
        proc.finish_output(10s);
        require(status && status->code == 0, "pause+stop exit code 0, got " + code_text(status) + "\n" + err);
        const std::string m = read_text(dir / "paused_stop.json");
        require_available_invariants(m, "pause+stop run");
        require(contains(m, "\"result\": \"stopped\","), "pause+stop result:\n" + m);
        require(contains(m, "\"end\": \"stopped\","), "pause+stop end:\n" + m);
        require(contains(m, "\"ok\": true,"), "pause+stop ok:\n" + m);
        require(contains(m, "\"interrupted_by\": \"stop command\""), "pause+stop interrupted_by:\n" + m);
    }

    // 3. Participant crash before start (transport created -> available, not_started, null sim_ms/wall_ms).
    {
        const fs::path dir = scenario_dir(o,
                                          "vehicle_sil_simulation: 2\n"
                                          "networks: []\n"
                                          "participants:\n"
                                          "  - name: Quitter\n"
                                          "    command: [" + helper(o) + ", --exit, \"9\", --uri, \"{registry_uri}\"]\n"
                                          "    describe: false\n");
        const Captured res = launch(o, {"run", "sim.yaml", "--control-metrics", "before_start.json"}, dir);
        expect_code(res, 1, "crash before start");
        const std::string m = read_text(dir / "before_start.json");
        require_available_invariants(m, "crash before start");
        require(contains(m, "\"result\": \"failed\","), "before_start result:\n" + m);
        require(contains(m, "\"simulation\": \"not_started\","), "before_start simulation:\n" + m);
        require(contains(m, "\"sim_ms\": null,"), "before_start sim_ms null:\n" + m);
        require(contains(m, "\"wall_ms\": null,"), "before_start wall_ms null:\n" + m);
    }

    // 4. Runtime failure / abort & --timeout-ms.
    {
        const fs::path dir = scenario_dir(o,
                                          "vehicle_sil_simulation: 2\n"
                                          "# run-option --stop-at-ms 1000\n"
                                          "networks: []\n"
                                          "participants:\n"
                                          "  - name: Engine\n"
                                          "    command: [" + ecu(o) + "]\n",
                                          {{"Engine", "exit-at-ms 50 7"}});
        const Captured res = launch(o, {"run", "sim.yaml", "--control-metrics", "aborted.json"}, dir);
        expect_code(res, 1, "runtime crash");
        const std::string m = read_text(dir / "aborted.json");
        require_available_invariants(m, "runtime crash");
        require(contains(m, "\"result\": \"failed\","), "runtime crash result:\n" + m);
        require(contains(m, "\"simulation\": \"aborted\","), "runtime crash simulation:\n" + m);

        // --timeout-ms abort: transport created -> available, simulation aborted, failed, exit 1.
        const fs::path timeout_dir = scenario_dir(o,
                                                  "vehicle_sil_simulation: 2\n"
                                                  "networks: []\n"
                                                  "participants:\n"
                                                  "  - name: Engine\n"
                                                  "    command: [" + ecu(o) + "]\n",
                                                  {{"Engine", "hang-at-ms 20"}});
        const Captured to_res =
            launch(o, {"run", "sim.yaml", "--timeout-ms", "150", "--control-metrics", "timeout.json"}, timeout_dir);
        expect_code(to_res, 1, "--timeout-ms abort");
        const std::string tm = read_text(timeout_dir / "timeout.json");
        require_available_invariants(tm, "--timeout-ms abort");
        require(contains(tm, "\"result\": \"failed\","), "--timeout-ms result:\n" + tm);
        require(contains(tm, "\"simulation\": \"aborted\","), "--timeout-ms simulation:\n" + tm);
    }

    // 5. Unavailable states: unreadable (exit 3), check_failed (exit 1), setup_failed (exit 3).
    {
        const fs::path dir = o.work_dir / "unavailable";
        fs::create_directories(dir);
        const Captured unreadable = launch(o, {"run", "missing.yaml", "--control-metrics", "unreadable.json"}, dir);
        expect_code(unreadable, 3, "unreadable config");
        const std::string u = read_text(dir / "unreadable.json");
        require_unavailable(u, "unreadable", "unreadable config");
        require(contains(u, "\"result\": \"unreadable\","), "unreadable result:\n" + u);
        require(contains(u, "\"time_step_ms\": null,"), "unreadable time_step_ms null:\n" + u);

        write_file(dir / "bad.yaml",
                   "vehicle_sil_simulation: 2\n"
                   "networks: []\n"
                   "participants:\n"
                   "  - name: Missing\n"
                   "    command: [./does_not_exist_ecu]\n");
        const Captured check_fail = launch(o, {"run", "bad.yaml", "--control-metrics", "check_failed.json"}, dir);
        expect_code(check_fail, 1, "check failed");
        const std::string cf = read_text(dir / "check_failed.json");
        require_unavailable(cf, "check_failed", "check failed");
        require(contains(cf, "\"result\": \"check_failed\","), "check_failed result:\n" + cf);
        require(contains(cf, "\"time_step_ms\": null,"), "check_failed time_step_ms null:\n" + cf);

        // Setup failure before transport creation: --log-dir pointing to an existing regular file.
        const fs::path setup_dir = scenario_dir(o,
                                                "vehicle_sil_simulation: 2\n"
                                                "networks: []\n"
                                                "participants:\n"
                                                "  - name: Engine\n"
                                                "    command: [" + ecu(o) + "]\n",
                                                {{"Engine", "pid"}});
        const Captured setup_fail =
            launch(o, {"run", "sim.yaml", "--log-dir", "sim.yaml", "--control-metrics", "setup_failed.json"}, setup_dir);
        expect_code(setup_fail, 3, "setup failed");
        const std::string sf = read_text(setup_dir / "setup_failed.json");
        require_unavailable(sf, "setup_failed", "setup failed");
        require(contains(sf, "\"result\": \"setup_failed\","), "setup_failed result:\n" + sf);
        require(contains(sf, "\"end\": \"setup_failed\","), "setup_failed end:\n" + sf);
        require(contains(sf, "\"simulation\": \"not_started\","), "setup_failed simulation:\n" + sf);
        require(contains(sf, "\"sim_ms\": null,"), "setup_failed sim_ms null:\n" + sf);
        require(contains(sf, "\"wall_ms\": null,"), "setup_failed wall_ms null:\n" + sf);
        require(contains(sf, "\"time_step_ms\": 10,"), "setup_failed time_step_ms:\n" + sf);
    }

    // 6. Write failure semantics: passing run -> exit 3 with untouched run --json; failing run -> exit 1 preserved.
    {
        const fs::path ex = example_copy(o);
        // Existing regular file as parent dir ("simulation.yaml/metrics.json") and existing directory (".").
        const Captured pass_bad_parent = launch(
            o, {"run", "--json", "simulation.yaml", "--stop-at-ms", "50", "--control-metrics", "simulation.yaml/metrics.json"},
            ex);
        expect_code(pass_bad_parent, 3, "passing run with unwritable metrics path exits 3");
        require(contains(pass_bad_parent.standard_output, "\"result\": \"passed\"") &&
                    contains(pass_bad_parent.standard_output, "\"ok\": true"),
                "passing run --json stdout untouched on metrics write failure:\n" + pass_bad_parent.standard_output);
        require(contains(pass_bad_parent.standard_error, "error: cannot write control metrics to simulation.yaml/metrics.json:"),
                "stderr reports control metrics write failure:\n" + pass_bad_parent.standard_error);

        const Captured pass_dir =
            launch(o, {"run", "--json", "simulation.yaml", "--stop-at-ms", "50", "--control-metrics", "."}, ex);
        expect_code(pass_dir, 3, "passing run with directory as metrics path exits 3");
        require(contains(pass_dir.standard_error, "error: cannot write control metrics to .: "),
                "stderr reports directory rejection:\n" + pass_dir.standard_error);

        // Failing run (exit 1) + unwritable metrics path: preserves exit code 1!
        const fs::path fail_dir = scenario_dir(o,
                                               "vehicle_sil_simulation: 2\n"
                                               "# run-option --stop-at-ms 1000\n"
                                               "networks: []\n"
                                               "participants:\n"
                                               "  - name: Engine\n"
                                               "    command: [" + ecu(o) + "]\n",
                                               {{"Engine", "exit-at-ms 50 7"}});
        const Captured fail_bad_path =
            launch(o, {"run", "--json", "sim.yaml", "--control-metrics", "sim.yaml/metrics.json"}, fail_dir);
        expect_code(fail_bad_path, 1, "failing run with unwritable metrics path preserves exit 1");
        require(contains(fail_bad_path.standard_output, "\"result\": \"failed\"") &&
                    contains(fail_bad_path.standard_output, "\"ok\": false"),
                "failing run --json stdout untouched on metrics write failure:\n" + fail_bad_path.standard_output);
        require(contains(fail_bad_path.standard_error, "error: cannot write control metrics to sim.yaml/metrics.json:"),
                "stderr reports control metrics write failure on failing run:\n" + fail_bad_path.standard_error);
    }

    // 7. CLI rejection: empty value and non-run subcommands exit 4 without writing any file.
    {
        const fs::path dir = o.work_dir / "cli_reject";
        fs::create_directories(dir);
        write_file(dir / "sim.yaml", "vehicle_sil_simulation: 2\nnetworks: []\nparticipants: []\n");
        expect_code(launch(o, {"run", "--control-metrics", "", "sim.yaml"}, dir), 4, "empty --control-metrics");
        expect_code(launch(o, {"check", "--control-metrics", "x.json", "sim.yaml"}, dir), 4, "check --control-metrics");
        expect_code(launch(o, {"test", "--control-metrics", "x.json", "sim.yaml", "--scenarios", "s.yaml"}, dir), 4,
                    "test --control-metrics");
        expect_code(launch(o, {"serve", "--control-metrics", "x.json", "sim.yaml"}, dir), 4, "serve --control-metrics");
        expect_code(launch(o, {"serve", "--once", "--control-metrics", "x.json", "sim.yaml"}, dir), 4,
                    "serve --once --control-metrics");
        require(!fs::exists(dir / "x.json"), "rejected CLI invocations must not create x.json");
    }
}
