// SPDX-License-Identifier: MIT
//
// Process-level integration scenarios for examples/standard_silkit_participant under
// vehicle_sil_launch:
//   - asymmetric_bidirectional_max: 10 ms vehicle-sil ECU <-> 25 ms standard SIL Kit participant
//     with --speed max, verifying bidirectional CAN, virtual-time causality, and strict-before
//     deferral of simultaneous-timestamp frames (never dropped).
//   - simultaneous_timestamp_determinism: --speed max vs --speed 1 produce byte-for-byte identical
//     CAN traces on both participants (no wall-clock speed assertions).
//   - cross_run_isolation: restarting participants for Run 2 starts cleanly at t = 0 with zero
//     residual frames from Run 1.
//   - example_config_smoke: shipped examples/standard_silkit_participant/simulation.yaml passes
//     vehicle_sil_launch check and run --speed max.

#include "launcher_support.hpp"
#include "test_timeout_scale.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace vehicle_sil::test;
using vehicle_sil::process::Captured;
using vehicle_sil::process::shown_path;

namespace {

struct Options
{
    fs::path launch;
    fs::path standard_peer;
    fs::path test_ecu;
    fs::path example_dir;
    fs::path work_dir;
    std::string scenario;
};

struct CommRow
{
    char dir = 'T';
    std::int64_t tx_ns = 0;
    std::int64_t tx_ms = 0;
    std::int64_t rx_step_ns = -1;
    std::int64_t rx_step_ms = -1;
    std::uint32_t can_id = 0;
    std::uint16_t seq = 0;
    std::string payload;
};

void require(bool cond, const std::string& msg)
{
    if (!cond)
    {
        throw std::runtime_error(msg);
    }
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

std::string yaml_quote(const std::string& text)
{
    std::string out = "'";
    for (char c : text)
    {
        out += c == '\'' ? "''" : std::string(1u, c);
    }
    return out + "'";
}

void write_file(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(out), "cannot write " + shown_path(path));
    out << text;
}

std::string read_text(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    require(static_cast<bool>(in), "cannot read " + shown_path(path));
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

Captured run_launch(const Options& o, const std::vector<std::string>& args, const fs::path& dir)
{
    vehicle_sil::process::CaptureOptions opt;
    opt.working_directory = dir;
    opt.timeout = scaled_timeout(120s);
    return vehicle_sil::process::run_captured(o.launch, args, opt);
}

void expect_ok(const Captured& r, const std::string& what)
{
    require(r.exit_code.has_value() && *r.exit_code == 0,
            what + " failed (code=" + (r.exit_code ? std::to_string(*r.exit_code) : std::string("timeout")) +
                ")\n--- stdout ---\n" + r.standard_output + "\n--- stderr ---\n" + r.standard_error);
}

std::vector<CommRow> parse_tsv(const fs::path& path)
{
    const std::string text = read_text(path);
    std::istringstream in(text);
    std::string line;
    std::vector<CommRow> rows;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty() || line.rfind("dir\t", 0) == 0)
        {
            continue;
        }
        std::vector<std::string> cols;
        std::size_t start = 0;
        while (true)
        {
            const std::size_t tab = line.find('\t', start);
            if (tab == std::string::npos)
            {
                cols.push_back(line.substr(start));
                break;
            }
            cols.push_back(line.substr(start, tab - start));
            start = tab + 1u;
        }
        require(cols.size() == 8u, "unexpected TSV column count in " + shown_path(path) + ": " + line);
        CommRow row;
        row.dir = cols[0].at(0);
        row.tx_ns = std::stoll(cols[1]);
        row.tx_ms = std::stoll(cols[2]);
        row.rx_step_ns = (cols[3] == "-") ? -1 : std::stoll(cols[3]);
        row.rx_step_ms = (cols[4] == "-") ? -1 : std::stoll(cols[4]);
        row.can_id = static_cast<std::uint32_t>(std::stoul(cols[5], nullptr, 0));
        row.seq = static_cast<std::uint16_t>(std::stoul(cols[6], nullptr, 10));
        row.payload = cols[7];
        rows.push_back(std::move(row));
    }
    return rows;
}

fs::path write_pair_config(const Options& o, const fs::path& dir, const std::string& ecu_tag, const std::string& std_tag)
{
    const fs::path ecu_log = dir / "vehicle_ecu.comm.tsv";
    const fs::path ecu_sum = dir / "vehicle_ecu.summary.json";
    const fs::path std_log = dir / "std_peer.comm.tsv";
    const fs::path std_sum = dir / "std_peer.summary.json";

    std::ostringstream yaml;
    yaml << "vehicle_sil_simulation: 2\n"
         << "time_step_ms: 5\n"
         << "networks:\n"
         << "  - name: TEST_CAN\n"
         << "    kind: can\n"
         << "participants:\n"
         << "  - name: VehicleEcu\n"
         << "    step_ms: 10\n"
         << "    command:\n"
         << "      - " << yaml_quote(shown_path(o.test_ecu)) << "\n"
         << "      - --ecu-network\n"
         << "      - TEST_CAN\n"
         << "      - --ecu-step-ms\n"
         << "      - '10'\n"
         << "      - --ecu-tx-id\n"
         << "      - '0x100'\n"
         << "      - --ecu-tx-period-ms\n"
         << "      - '10'\n"
         << "      - --ecu-run-tag\n"
         << "      - " << yaml_quote(ecu_tag) << "\n"
         << "      - --ecu-log\n"
         << "      - " << yaml_quote(shown_path(ecu_log)) << "\n"
         << "      - --ecu-summary\n"
         << "      - " << yaml_quote(shown_path(ecu_sum)) << "\n"
         << "      - --no-control\n"
         << "  - name: StandardPeer\n"
         << "    describe: false\n"
         << "    command:\n"
         << "      - " << yaml_quote(shown_path(o.standard_peer)) << "\n"
         << "      - --name\n"
         << "      - '{name}'\n"
         << "      - --registry-uri\n"
         << "      - '{registry_uri}'\n"
         << "      - --network\n"
         << "      - TEST_CAN\n"
         << "      - --step-ms\n"
         << "      - '25'\n"
         << "      - --tx-id\n"
         << "      - '0x200'\n"
         << "      - --tx-period-ms\n"
         << "      - '25'\n"
         << "      - --run-tag\n"
         << "      - " << yaml_quote(std_tag) << "\n"
         << "      - --log\n"
         << "      - " << yaml_quote(shown_path(std_log)) << "\n"
         << "      - --summary\n"
         << "      - " << yaml_quote(shown_path(std_sum)) << "\n";

    const fs::path cfg = dir / "sim.yaml";
    write_file(cfg, yaml.str());
    return cfg;
}

std::uint64_t extract_json_u64(const std::string& json, const std::string& key)
{
    const std::string needle = "\"" + key + "\": ";
    const std::size_t pos = json.find(needle);
    require(pos != std::string::npos, "missing JSON key '" + key + "' in:\n" + json);
    const std::size_t start = pos + needle.size();
    std::size_t end = start;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9')
    {
        ++end;
    }
    require(end > start, "non-integer JSON value for '" + key + "' in:\n" + json);
    return static_cast<std::uint64_t>(std::stoull(json.substr(start, end - start)));
}

// Verify all steps up to `verified_until_ms` (inclusive). Callers pass `--stop-at-ms` set one
// `time_step_ms` (5 ms) past `verified_until_ms` (e.g. 505 for 500) so every verified step is
// strictly < stop_at_ms per SimulationController's completion contract, avoiding stop-boundary races.
void verify_asymmetric_run(const fs::path& dir, std::int64_t verified_until_ms, const std::string& expected_ecu_tag_hex,
                           const std::string& expected_std_tag_hex)
{
    const std::string std_sum = read_text(dir / "std_peer.summary.json");
    const std::string ecu_sum = read_text(dir / "vehicle_ecu.summary.json");

    const std::int64_t expected_std_steps = (verified_until_ms / 25) + 1;
    const std::int64_t expected_ecu_steps = (verified_until_ms / 10) + 1;
    const std::int64_t expected_std_rx = verified_until_ms / 10; // ECU frames at 0, 10, ..., verified_until_ms - 10
    const std::int64_t expected_ecu_rx = verified_until_ms / 25; // StdPeer frames at 0, 25, ..., verified_until_ms - 25

    require(contains(std_sum, "\"final_state\": \"Shutdown\""), "StandardPeer did not shut down cleanly:\n" + std_sum);
    require(contains(std_sum, "\"first_step_ns\": 0,"), "StandardPeer did not start at t=0:\n" + std_sum);
    require(contains(std_sum, "\"last_step_ns\": " + std::to_string(verified_until_ms * 1'000'000LL) + ","),
            "StandardPeer last_step_ns mismatch:\n" + std_sum);
    require(contains(std_sum, "\"steps\": " + std::to_string(expected_std_steps) + ","),
            "StandardPeer step count mismatch:\n" + std_sum);
    require(contains(std_sum, "\"non_monotonic_steps\": 0,"), "StandardPeer had non-monotonic steps:\n" + std_sum);
    require(contains(std_sum, "\"wrong_delta_steps\": 0,"), "StandardPeer had wrong step delta:\n" + std_sum);
    require(contains(std_sum, "\"rx_causality_violations\": 0"), "StandardPeer had causality violations:\n" + std_sum);

    // Post-shutdown teardown must stop callbacks before counting so totals are exact.
    const std::uint64_t std_rx_processed = extract_json_u64(std_sum, "rx_processed");
    const std::uint64_t std_rx_queued = extract_json_u64(std_sum, "rx_queued_at_shutdown");
    const std::uint64_t std_rx_callbacks = extract_json_u64(std_sum, "rx_callback_total");
    require(std_rx_processed + std_rx_queued == std_rx_callbacks,
            "StandardPeer rx_processed + rx_queued_at_shutdown != rx_callback_total:\n" + std_sum);

    require(contains(ecu_sum, "\"first_step_ns\": 0,"), "VehicleEcu did not start at t=0:\n" + ecu_sum);
    require(contains(ecu_sum, "\"last_step_ns\": " + std::to_string(verified_until_ms * 1'000'000LL) + ","),
            "VehicleEcu last_step_ns mismatch:\n" + ecu_sum);
    require(contains(ecu_sum, "\"steps\": " + std::to_string(expected_ecu_steps) + ","),
            "VehicleEcu step count mismatch:\n" + ecu_sum);
    require(contains(ecu_sum, "\"non_monotonic_steps\": 0,"), "VehicleEcu had non-monotonic steps:\n" + ecu_sum);
    require(contains(ecu_sum, "\"wrong_delta_steps\": 0,"), "VehicleEcu had wrong step delta:\n" + ecu_sum);
    require(contains(ecu_sum, "\"rx_causality_violations\": 0"), "VehicleEcu had causality violations:\n" + ecu_sum);

    const auto std_rows = parse_tsv(dir / "std_peer.comm.tsv");
    const auto ecu_rows = parse_tsv(dir / "vehicle_ecu.comm.tsv");

    std::vector<CommRow> std_tx, std_rx, ecu_tx, ecu_rx;
    for (const auto& r : std_rows)
    {
        (r.dir == 'T' ? std_tx : std_rx).push_back(r);
    }
    for (const auto& r : ecu_rows)
    {
        (r.dir == 'T' ? ecu_tx : ecu_rx).push_back(r);
    }

    require(static_cast<std::int64_t>(std_tx.size()) == expected_std_steps,
            "StandardPeer TX count mismatch: got " + std::to_string(std_tx.size()));
    require(static_cast<std::int64_t>(std_rx.size()) == expected_std_rx,
            "StandardPeer RX count mismatch: got " + std::to_string(std_rx.size()) + ", expected " +
                std::to_string(expected_std_rx));
    require(static_cast<std::int64_t>(ecu_tx.size()) == expected_ecu_steps,
            "VehicleEcu TX count mismatch: got " + std::to_string(ecu_tx.size()));
    require(static_cast<std::int64_t>(ecu_rx.size()) == expected_ecu_rx,
            "VehicleEcu RX count mismatch: got " + std::to_string(ecu_rx.size()) + ", expected " +
                std::to_string(expected_ecu_rx));

    // Check StandardPeer RX: strict causality (tx_ns < rx_step_ns), exact sequence, tag, and
    // simultaneous-timestamp deferral (frames sent at multiples of 50 ms are queued and processed at +25 ms).
    std::map<std::int64_t, std::int64_t> std_rx_step_by_tx_ms;
    for (std::size_t i = 0; i < std_rx.size(); ++i)
    {
        const auto& r = std_rx[i];
        require(r.can_id == 0x100u, "StandardPeer received unexpected CAN ID");
        require(r.seq == static_cast<std::uint16_t>(i),
                "StandardPeer RX sequence gap/order mismatch at index " + std::to_string(i));
        require(r.tx_ns < r.rx_step_ns,
                "StandardPeer strict-before violated: tx_ns=" + std::to_string(r.tx_ns) +
                    " rx_step_ns=" + std::to_string(r.rx_step_ns));
        require(r.payload.size() >= 2u && r.payload.substr(0, 2) == expected_ecu_tag_hex,
                "StandardPeer RX tag mismatch: got " + r.payload);
        std_rx_step_by_tx_ms[r.tx_ms] = r.rx_step_ms;
    }

    for (std::int64_t t = 0; t < verified_until_ms; t += 50)
    {
        const auto it = std_rx_step_by_tx_ms.find(t);
        require(it != std_rx_step_by_tx_ms.end(),
                "Simultaneous-timestamp frame at t=" + std::to_string(t) + " ms was dropped by StandardPeer");
        require(it->second == t + 25,
                "Simultaneous-timestamp frame at t=" + std::to_string(t) + " ms processed at " +
                    std::to_string(it->second) + " ms instead of " + std::to_string(t + 25) + " ms");
    }

    // Check VehicleEcu RX: causality, exact sequence, and expected StandardPeer run tag.
    for (std::size_t i = 0; i < ecu_rx.size(); ++i)
    {
        const auto& r = ecu_rx[i];
        require(r.can_id == 0x200u, "VehicleEcu received unexpected CAN ID");
        require(r.seq == static_cast<std::uint16_t>(i),
                "VehicleEcu RX sequence gap/order mismatch at index " + std::to_string(i));
        require(r.tx_ns < r.rx_step_ns,
                "VehicleEcu causality violated: tx_ns=" + std::to_string(r.tx_ns) +
                    " rx_step_ns=" + std::to_string(r.rx_step_ns));
        require(r.payload.size() >= 2u && r.payload.substr(0, 2) == expected_std_tag_hex,
                "VehicleEcu RX tag mismatch: got " + r.payload);
    }
}

void asymmetric_bidirectional_max(const Options& o)
{
    const fs::path dir = o.work_dir / "asymmetric_max";
    const fs::path cfg = write_pair_config(o, dir, "0x11", "0xa1");

    const Captured chk = run_launch(o, {"check", shown_path(cfg)}, dir);
    expect_ok(chk, "vehicle_sil_launch check");

    // Stop at 505 ms (one time_step_ms past 500 ms) so all steps <= 500 ms finish before Stop().
    const Captured run = run_launch(o, {"run", shown_path(cfg), "--speed", "max", "--stop-at-ms", "505", "--json"}, dir);
    expect_ok(run, "vehicle_sil_launch run --speed max");
    require(contains(run.standard_output, "\"result\": \"passed\""), "launcher did not report passed:\n" + run.standard_output);

    verify_asymmetric_run(dir, 500, "11", "a1");
}

void simultaneous_timestamp_determinism(const Options& o)
{
    const fs::path dir_max = o.work_dir / "det_max";
    const fs::path dir_rt = o.work_dir / "det_rt";
    const fs::path cfg_max = write_pair_config(o, dir_max, "0x11", "0xa1");
    const fs::path cfg_rt = write_pair_config(o, dir_rt, "0x11", "0xa1");

    // Stop at 255 ms (one time_step_ms past 250 ms) so neither 10 ms nor 25 ms participant races
    // Stop(); no wall-clock elapsed time or speedup assertions, only identical virtual-time traces.
    const Captured r_max =
        run_launch(o, {"run", shown_path(cfg_max), "--speed", "max", "--stop-at-ms", "255", "--json"}, dir_max);
    expect_ok(r_max, "determinism run --speed max");
    require(contains(r_max.standard_output, "\"result\": \"passed\""),
            "determinism max run failed:\n" + r_max.standard_output);

    const Captured r_rt =
        run_launch(o, {"run", shown_path(cfg_rt), "--speed", "1", "--stop-at-ms", "255", "--json"}, dir_rt);
    expect_ok(r_rt, "determinism run --speed 1");
    require(contains(r_rt.standard_output, "\"result\": \"passed\""),
            "determinism rt run failed:\n" + r_rt.standard_output);

    verify_asymmetric_run(dir_max, 250, "11", "a1");
    verify_asymmetric_run(dir_rt, 250, "11", "a1");

    const std::string std_max_tsv = read_text(dir_max / "std_peer.comm.tsv");
    const std::string std_rt_tsv = read_text(dir_rt / "std_peer.comm.tsv");
    require(std_max_tsv == std_rt_tsv, "StandardPeer CAN trace differed between --speed max and --speed 1");

    const std::string ecu_max_tsv = read_text(dir_max / "vehicle_ecu.comm.tsv");
    const std::string ecu_rt_tsv = read_text(dir_rt / "vehicle_ecu.comm.tsv");
    require(ecu_max_tsv == ecu_rt_tsv, "VehicleEcu CAN trace differed between --speed max and --speed 1");
}

void cross_run_isolation(const Options& o)
{
    const fs::path dir1 = o.work_dir / "run1";
    const fs::path dir2 = o.work_dir / "run2";
    const fs::path cfg1 = write_pair_config(o, dir1, "0x11", "0xa1");
    const fs::path cfg2 = write_pair_config(o, dir2, "0x22", "0xa2");

    // Stop at 305 ms (one time_step_ms past 300 ms) so all steps <= 300 ms finish cleanly.
    const Captured r1 = run_launch(o, {"run", shown_path(cfg1), "--speed", "max", "--stop-at-ms", "305", "--json"}, dir1);
    expect_ok(r1, "cross-run isolation run 1");
    require(contains(r1.standard_output, "\"result\": \"passed\""), "run 1 did not pass:\n" + r1.standard_output);
    verify_asymmetric_run(dir1, 300, "11", "a1");

    const Captured r2 = run_launch(o, {"run", shown_path(cfg2), "--speed", "max", "--stop-at-ms", "305", "--json"}, dir2);
    expect_ok(r2, "cross-run isolation run 2");
    require(contains(r2.standard_output, "\"result\": \"passed\""), "run 2 did not pass:\n" + r2.standard_output);
    verify_asymmetric_run(dir2, 300, "22", "a2");
}

void example_config_smoke(const Options& o)
{
    const fs::path cfg = o.example_dir / "simulation.yaml";
    require(fs::is_regular_file(cfg), "missing built example config: " + shown_path(cfg));

    const Captured chk = run_launch(o, {"check", shown_path(cfg)}, o.example_dir);
    expect_ok(chk, "example simulation.yaml check");

    const Captured run =
        run_launch(o, {"run", shown_path(cfg), "--speed", "max", "--stop-at-ms", "305", "--json"}, o.example_dir);
    expect_ok(run, "example simulation.yaml run --speed max");
    require(contains(run.standard_output, "\"result\": \"passed\""),
            "example simulation.yaml did not pass:\n" + run.standard_output);
    // Verify StandardPeer actually received Engine's J1939 CAN frames with zero causality violations.
    require(contains(run.standard_error, "\"final_state\": \"Shutdown\""),
            "StandardPeer did not print Shutdown in example_config_smoke:\n" + run.standard_error);
    require(contains(run.standard_error, "\"rx_causality_violations\": 0"),
            "StandardPeer reported causality violations in example_config_smoke:\n" + run.standard_error);
    const std::uint64_t rx_processed = extract_json_u64(run.standard_error, "rx_processed");
    require(rx_processed >= 3u,
            "expected Engine -> StandardPeer J1939 CAN frames (>= 3) in example_config_smoke, got " +
                std::to_string(rx_processed) + "\n" + run.standard_error);
}

Options parse_options(int argc, char** argv)
{
    const vehicle_sil::cli::Args args(argc, argv);
    Options o;
    o.launch = args.required("launch");
    o.standard_peer = args.required("standard-peer");
    o.test_ecu = args.required("test-ecu");
    o.example_dir = args.required("example-dir");
    o.work_dir = args.required("work-dir");
    o.scenario = args.required("scenario");
    return o;
}

} // namespace

int main(int argc, char** argv)
{
    set_log_tag("std-silkit-peer");
    try
    {
        vehicle_sil::test::check_timeout_scale();
        const Options o = parse_options(argc, argv);
        fs::remove_all(o.work_dir);
        fs::create_directories(o.work_dir);
        log("scenario " + o.scenario);

        if (o.scenario == "asymmetric_bidirectional_max")
        {
            asymmetric_bidirectional_max(o);
        }
        else if (o.scenario == "simultaneous_timestamp_determinism")
        {
            simultaneous_timestamp_determinism(o);
        }
        else if (o.scenario == "cross_run_isolation")
        {
            cross_run_isolation(o);
        }
        else if (o.scenario == "example_config_smoke")
        {
            example_config_smoke(o);
        }
        else
        {
            throw std::invalid_argument("unknown scenario: " + o.scenario);
        }

        log("PASS");
        return 0;
    }
    catch (const std::exception& ex)
    {
        log(std::string("FAIL: ") + ex.what());
        return 1;
    }
}
