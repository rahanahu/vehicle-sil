// SPDX-License-Identifier: MIT
//
// Process-level virtual-time runs shared by the launchers: a sil-kit-registry, the
// vehicle_sil_sim_controller and a set of peer processes that write trace files, plus the trace
// record format (see examples/virtual_time/common/vt_common.hpp).
#pragma once

#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace vehicle_sil::test {

/// What every run of one launcher shares.
struct SimulationSetup
{
    std::filesystem::path registry;   ///< sil-kit-registry
    std::filesystem::path controller; ///< vehicle_sil_sim_controller
    std::filesystem::path work_dir;   ///< each run gets the sub-directory <work_dir>/<label>
    long long end_ms = 1000;          ///< --end-ms of every peer and --stop-at-ms of the controller
    std::chrono::milliseconds timeout{120000};
};

struct PeerSpec
{
    std::string name;            ///< participant name
    std::filesystem::path exe;
    std::string trace;           ///< trace and log file stem
    std::vector<std::string> extra; ///< further command line arguments
};

struct RunResult
{
    long long wall_us = -1;                     ///< as the controller measured it
    std::map<std::string, std::string> traces; ///< trace stem -> content
};

/// Runs one simulation. Every peer gets --registry-uri, --participant, --end-ms, --timeout-ms,
/// --run-timeout-ms and --trace. With `expected_code`, every process must exit with that code (a
/// failure scenario; no traces are read); otherwise every process must exit 0. Throws
/// std::runtime_error (after dumping the logs) on unexpected exit codes.
RunResult run_simulation(const SimulationSetup& setup, const std::string& label, const std::vector<PeerSpec>& peers,
                         int expected_code = 0);

/// Requires every trace of `run` to equal the reference's.
void check_identical(const RunResult& reference, const RunResult& run, const std::string& label);

/// Throws std::runtime_error(what) unless `condition`.
void require(bool condition, const std::string& what);

/// One trace line: a record type followed by key=value fields.
struct Record
{
    std::string type;
    std::map<std::string, std::string> fields;

    [[nodiscard]] long long num(const std::string& key) const { return std::stoll(str(key), nullptr, 0); }
    [[nodiscard]] const std::string& str(const std::string& key) const;
};

/// The records of `type` in a trace, in file order.
std::vector<Record> parse_trace(const std::string& text, const std::string& type);

/// Upper-case hex digits of `bytes`, as the peers trace data.
std::string hex_bytes(std::initializer_list<unsigned> bytes);

} // namespace vehicle_sil::test
