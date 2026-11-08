// SPDX-License-Identifier: MIT
#include "simulation_run.hpp"

#include "launcher_support.hpp"

#include <algorithm>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace vehicle_sil::test {

void require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

RunResult run_simulation(const SimulationSetup& setup, const std::string& label, const std::vector<PeerSpec>& peers,
                         int expected_code)
{
    const fs::path dir = setup.work_dir / label;
    fs::remove_all(dir);
    fs::create_directories(dir);
    log("run '" + label + "', logs and traces in " + dir.string());

    RegistryProcess registry(setup.registry, dir);
    const std::string& uri = registry.uri();
    const std::string end = std::to_string(setup.end_ms);
    const std::string timeout = std::to_string(std::max<long long>(setup.timeout.count() - 10000, 5000));

    std::string required;
    for (const auto& peer : peers)
    {
        required += (required.empty() ? "" : ",") + peer.name;
    }
    const auto started = std::chrono::steady_clock::now();
    ChildProcess controller(setup.controller,
                            {"--registry-uri", uri, "--required", required, "--stop-at-ms", end, "--timeout-ms", timeout},
                            dir / "controller.log");
    std::vector<std::unique_ptr<ChildProcess>> processes;
    for (const auto& peer : peers)
    {
        std::vector<std::string> args{"--registry-uri", uri,      "--participant", peer.name,
                                      "--end-ms",       end,      "--timeout-ms",  timeout,
                                      "--run-timeout-ms", timeout,
                                      "--trace",        (dir / (peer.trace + ".trace")).string()};
        args.insert(args.end(), peer.extra.begin(), peer.extra.end());
        processes.push_back(std::make_unique<ChildProcess>(peer.exe, args, dir / (peer.trace + ".log")));
    }

    const auto deadline = started + setup.timeout;
    std::string failures;
    const auto controllerCode = wait_or_kill(controller, deadline);
    if (controllerCode != expected_code)
    {
        failures += " controller=" + exit_code_text(controllerCode);
    }
    for (std::size_t i = 0; i < peers.size(); ++i)
    {
        const auto code = wait_or_kill(*processes[i], deadline);
        if (code != expected_code)
        {
            failures += " " + peers[i].name + "=" + exit_code_text(code);
        }
    }
    registry.stop();

    if (!failures.empty())
    {
        dump_log(registry.log_file());
        dump_log(dir / "controller.log");
        for (const auto& peer : peers)
        {
            dump_log(dir / (peer.trace + ".log"));
        }
        throw std::runtime_error("run '" + label + "': unexpected exit codes (expected " + std::to_string(expected_code) +
                                 "):" + failures);
    }
    RunResult result;
    result.wall_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    if (expected_code != 0)
    {
        return result;
    }

    const std::string controllerLog = read_file(dir / "controller.log");
    std::smatch match;
    const std::regex wallPattern(R"(result sim_ms=(\d+) wall_us=(\d+))");
    require(std::regex_search(controllerLog, match, wallPattern), "controller did not report the wall time");
    result.wall_us = std::stoll(match[2].str());
    for (const auto& peer : peers)
    {
        result.traces[peer.trace] = read_file(dir / (peer.trace + ".trace"));
    }
    log("run '" + label + "': simulated " + std::to_string(setup.end_ms) + " ms in " +
        std::to_string(result.wall_us / 1000) + " ms wall time");
    return result;
}

void check_identical(const RunResult& reference, const RunResult& run, const std::string& label)
{
    for (const auto& [stem, text] : reference.traces)
    {
        require(run.traces.at(stem) == text, "trace '" + stem + "' of run '" + label + "' differs from the reference run");
    }
    log("CHECK traces of run '" + label + "' identical to the reference run -> ok");
}

const std::string& Record::str(const std::string& key) const
{
    const auto it = fields.find(key);
    if (it == fields.end())
    {
        throw std::runtime_error("trace record '" + type + "' has no field " + key);
    }
    return it->second;
}

std::vector<Record> parse_trace(const std::string& text, const std::string& type)
{
    std::vector<Record> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
    {
        std::istringstream tokens(line);
        Record record;
        tokens >> record.type;
        if (record.type != type)
        {
            continue;
        }
        std::string token;
        while (tokens >> token)
        {
            const auto eq = token.find('=');
            if (eq != std::string::npos)
            {
                record.fields[token.substr(0, eq)] = token.substr(eq + 1);
            }
        }
        out.push_back(std::move(record));
    }
    return out;
}

std::string hex_bytes(std::initializer_list<unsigned> bytes)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned b : bytes)
    {
        out += kDigits[(b >> 4) & 0xF];
        out += kDigits[b & 0xF];
    }
    return out;
}

} // namespace vehicle_sil::test
