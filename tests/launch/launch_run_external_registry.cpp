// SPDX-License-Identifier: MIT
//
// The launch_run scenario for `--external-registry <silkit://loopback:port>`:
//
//   external_registry  consecutive `run`, persistent `serve` (prepare + 2 runs), and `test`
//                      against a persistent external sil-kit-registry on a free loopback port
//                      (confirming distinct `/vsil-<16hex>` simulation paths per run and J1939
//                      traffic), unreachable external registry setup failure (exit 3, zero
//                      spawned participants), and CLI / config `registry_uri` usage errors (exit 4).
#include "launch_run_support.hpp"

#include <cctype>
#include <string>
#include <vector>

namespace {

std::string extract_json_registry_uri(const std::string& json_text)
{
    const std::string key = "\"registry_uri\": \"";
    const std::size_t pos = json_text.find(key);
    require(pos != std::string::npos, "missing registry_uri in JSON:\n" + json_text);
    const std::size_t start = pos + key.size();
    const std::size_t end = json_text.find('"', start);
    require(end != std::string::npos, "unterminated registry_uri in JSON:\n" + json_text);
    return json_text.substr(start, end - start);
}

void require_scoped_external_uri(const std::string& actual, const std::string& base_uri, const std::string& what)
{
    const std::string expected_prefix = base_uri + "/vsil-";
    require(actual.rfind(expected_prefix, 0) == 0,
            what + ": expected prefix '" + expected_prefix + "', got '" + actual + "'");
    const std::string hex = actual.substr(expected_prefix.size());
    require(hex.size() == 16, what + ": expected 16 hex chars after /vsil-, got '" + hex + "'");
    for (const char c : hex)
    {
        require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'),
                what + ": expected lowercase hex in '" + actual + "'");
    }
}

std::vector<std::string> extract_registry_events(const std::string& stdout_ndjson)
{
    std::vector<std::string> uris;
    std::istringstream in(stdout_ndjson);
    std::string line;
    const std::string marker = "\"event\":\"registry\"";
    const std::string uri_key = "\"uri\":\"";
    while (std::getline(in, line))
    {
        if (line.find(marker) == std::string::npos)
        {
            continue;
        }
        const std::size_t pos = line.find(uri_key);
        require(pos != std::string::npos, "registry event missing uri:\n" + line);
        const std::size_t start = pos + uri_key.size();
        const std::size_t end = line.find('"', start);
        require(end != std::string::npos, "unterminated uri in registry event:\n" + line);
        uris.push_back(line.substr(start, end - start));
    }
    return uris;
}

} // namespace

void external_registry(const Options& o)
{
    require(!o.registry.empty(), "external_registry scenario requires --registry <sil-kit-registry>");
    const fs::path ex = example_copy(o);
    const fs::path reg_dir = o.work_dir / "ext_reg";
    fs::create_directories(reg_dir);

    std::string ext_uri;
    {
        RegistryProcess reg(o.registry, reg_dir);
        ext_uri = reg.uri();
        require(ext_uri.rfind("silkit://127.0.0.1:", 0) == 0, "expected loopback registry URI, got: " + ext_uri);

        // 1. Two consecutive `run --external-registry` invocations on the same persistent registry:
        //    distinct `/vsil-<16hex>` simulation paths, J1939 traffic, exit 0.
        const Captured run1 =
            launch(o, {"run", "--external-registry", ext_uri, "--json", "--stop-at-ms", "100", "simulation.yaml"}, ex);
        expect_code(run1, 0, "first run with --external-registry");
        const std::string uri1 = extract_json_registry_uri(run1.standard_output);
        require_scoped_external_uri(uri1, ext_uri, "run 1");
        require(contains(lf(run1.standard_error), "[vehicle_sil_launch] registry at " + uri1),
                "run 1 stderr announces scoped registry URI:\n" + run1.standard_error);
        require(contains(run1.standard_output, "\"result\": \"passed\""), "run 1 passed:\n" + run1.standard_output);

        const Captured run2 =
            launch(o, {"run", "--external-registry", ext_uri, "--json", "--stop-at-ms", "100", "simulation.yaml"}, ex);
        expect_code(run2, 0, "second run with --external-registry");
        const std::string uri2 = extract_json_registry_uri(run2.standard_output);
        require_scoped_external_uri(uri2, ext_uri, "run 2");
        require(uri1 != uri2, "consecutive runs must mint distinct simulation paths: " + uri1 + " vs " + uri2);

        // 2. Persistent `serve --external-registry`: prepare stays Hot-Start eligible, and two sequential
        //    runs receive distinct `/vsil-<16hex>` URIs without cross-run collision.
        write_file(ex / "req100.json", "{\"vehicle_sil_run_request\": 1, \"stop_at_ms\": 100}\n");
        std::mutex serve_mu;
        std::condition_variable serve_cv;
        std::string serve_stdout;
        int prepares_seen = 0;
        int results_seen = 0;
        vehicle_sil::process::ProcessOptions serve_opts;
        serve_opts.working_directory = ex;
        serve_opts.input = true;
        serve_opts.max_line = 65536;
        serve_opts.on_line = [&](vehicle_sil::process::Stream stream, std::string_view line) {
            if (stream != vehicle_sil::process::Stream::Output)
            {
                return;
            }
            const std::lock_guard lock(serve_mu);
            serve_stdout.append(line);
            serve_stdout.push_back('\n');
            if (line.find("\"event\":\"prepare\"") != std::string_view::npos)
            {
                ++prepares_seen;
            }
            if (line.find("\"event\":\"result\"") != std::string_view::npos)
            {
                ++results_seen;
            }
            serve_cv.notify_all();
        };
        vehicle_sil::process::Process serve_proc(
            o.launch, {"serve", "--external-registry", ext_uri, "--", "simulation.yaml"}, serve_opts);
        const auto wait_timeout = vehicle_sil::test::scaled_timeout(60s);
        require(serve_proc.write_input("{\"command\":\"prepare\",\"id\":1}\n"), "write prepare to serve");
        {
            std::unique_lock lock(serve_mu);
            require(serve_cv.wait_for(lock, wait_timeout, [&] { return prepares_seen >= 1; }),
                    "timed out waiting for serve prepare:\n" + serve_stdout);
        }
        require(serve_proc.write_input("{\"command\":\"run\",\"request\":\"req100.json\",\"id\":2}\n"),
                "write run 1 to serve");
        {
            std::unique_lock lock(serve_mu);
            require(serve_cv.wait_for(lock, wait_timeout, [&] { return results_seen >= 1; }),
                    "timed out waiting for serve run 1 result:\n" + serve_stdout);
        }
        require(serve_proc.write_input("{\"command\":\"run\",\"request\":\"req100.json\",\"id\":3}\n"),
                "write run 2 to serve");
        {
            std::unique_lock lock(serve_mu);
            require(serve_cv.wait_for(lock, wait_timeout, [&] { return results_seen >= 2; }),
                    "timed out waiting for serve run 2 result:\n" + serve_stdout);
        }
        serve_proc.close_input();
        const auto serve_exit = serve_proc.wait_for(wait_timeout);
        require(serve_proc.finish_output(vehicle_sil::test::scaled_timeout(10s)), "serve output closed cleanly");
        require(serve_exit.has_value() && serve_exit->code == 0,
                "persistent serve exited with 0, got " + code_text(serve_exit) + ":\n" + serve_stdout);
        require(contains(serve_stdout, "\"event\":\"prepare\"") && contains(serve_stdout, "\"accepted\":true") &&
                    contains(serve_stdout, "\"launcher_state\":\"ready\""),
                "serve prepare accepted and ready:\n" + serve_stdout);
        const std::vector<std::string> serve_uris = extract_registry_events(serve_stdout);
        require(serve_uris.size() == 2,
                "expected 2 registry events in serve output, got " + std::to_string(serve_uris.size()) + ":\n" +
                    serve_stdout);
        require_scoped_external_uri(serve_uris[0], ext_uri, "serve run 1");
        require_scoped_external_uri(serve_uris[1], ext_uri, "serve run 2");
        require(serve_uris[0] != serve_uris[1],
                "serve consecutive runs must mint distinct simulation paths: " + serve_uris[0] + " vs " + serve_uris[1]);

        // 3. `test --external-registry` across multiple scenarios back-to-back on the persistent registry.
        fs::create_directories(ex / "scenarios");
        write_file(ex / "scenarios" / "smoke1.yaml",
                   "vehicle_sil_scenario: 1\n"
                   "name: smoke 1\n"
                   "steps:\n"
                   "  - at_ms: 0\n"
                   "    set: { Engine.engine.throttle: 50 }\n"
                   "  - wait_until: { Engine.engine.speed: { ge: 1200 } }\n"
                   "    timeout_ms: 500\n");
        write_file(ex / "scenarios" / "smoke2.yaml",
                   "vehicle_sil_scenario: 1\n"
                   "name: smoke 2\n"
                   "steps:\n"
                   "  - at_ms: 0\n"
                   "    set: { Engine.engine.throttle: 30 }\n"
                   "  - wait_until: { Dashboard.dashboard.engine_speed: { ge: 900 } }\n"
                   "    timeout_ms: 500\n");
        const Captured test_out =
            launch(o, {"test", "--external-registry", ext_uri, "--json", "simulation.yaml", "--scenarios", "scenarios"}, ex);
        expect_code(test_out, 0, "test with --external-registry");
        require(contains(test_out.standard_output, "\"result\": \"passed\"") &&
                    contains(test_out.standard_output, "\"passed\": 2"),
                "test passed 2 scenarios:\n" + test_out.standard_output);

        reg.stop();
    }

    // 4. Unreachable external registry fails setup (exit 3) before starting any child participant.
    const Captured unreachable = launch(
        o,
        {"run", "--external-registry", ext_uri, "--json", "--stop-at-ms", "100", "simulation.yaml"},
        ex);
    expect_code(unreachable, 3, "unreachable --external-registry exits with SetupFailed (3)");
    require(contains(unreachable.standard_output, "\"result\": \"setup_failed\""),
            "unreachable JSON result is setup_failed:\n" + unreachable.standard_output);
    require(contains(unreachable.standard_output, "cannot connect to the external registry at " + ext_uri) &&
                contains(unreachable.standard_output, "sil-kit-registry --listen-uri"),
            "unreachable error message gives actionable sil-kit-registry hint:\n" + unreachable.standard_output);
    require(!contains(lf(unreachable.standard_error), "starting Engine"),
            "no child participant started when external registry is unreachable:\n" + unreachable.standard_error);

    // 5. CLI & configuration conflict usage rejections (exit 4).
    expect_code(launch(o, {"check", "--external-registry", ext_uri, "simulation.yaml"}, ex), 4,
                "check rejects --external-registry");
    expect_code(launch(o, {"run", "--external-registry", "silkit://127.0.0.1:0", "simulation.yaml"}, ex), 4,
                "run rejects port 0 in --external-registry");
    expect_code(launch(o, {"run", "--external-registry", "silkit://0.0.0.0:8500", "simulation.yaml"}, ex), 4,
                "run rejects non-loopback in --external-registry");
    expect_code(launch(o, {"run", "--external-registry", "silkit://127.0.0.1:8500/sim", "simulation.yaml"}, ex), 4,
                "run rejects path in --external-registry");

    write_file(ex / "with_fixed_registry.yaml",
               read_text(ex / "simulation.yaml") + "\nregistry_uri: silkit://127.0.0.1:8500\n");
    const Captured conflict =
        launch(o, {"run", "--external-registry", ext_uri, "--json", "with_fixed_registry.yaml"}, ex);
    expect_code(conflict, 4, "config registry_uri + --external-registry exits 4");
    require(contains(conflict.standard_output, "cannot be used when the configuration file sets registry_uri"),
            "conflict message names registry_uri:\n" + conflict.standard_output);
}
