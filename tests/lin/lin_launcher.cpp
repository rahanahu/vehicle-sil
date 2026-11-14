// SPDX-License-Identifier: MIT
//
// LIN cluster integration test. Runs a real sil-kit-registry, vehicle_sil_sim_controller and
// three LIN peers as separate processes, time-synchronised with different steps:
//
//   LinMaster  5 ms  master (10 ms in cluster-master-10ms): schedule with 10 ms slots (command, echo, status), publishes the
//                    command, sends the cluster to sleep at 200 and 600 ms, wakes it at 400 ms
//   LinEcho    2 ms  slave: echoes the newest command; goes to sleep after 60 ms of bus silence
//   LinStatus  4 ms  slave: counts the commands; wakes the cluster at 700 ms
//
// Scenarios:
//   cluster        checks the traces against each other: schedule timing, master frames at
//                  both slaves, slave responses at the master, sleep and wakeup of every node
//   cluster-paced  as cluster, then runs again with the echo slave paced to real time and
//                  requires byte-identical traces
//   cluster-master-10ms  as cluster-paced with the master stepping once per slot: a frame the
//                  master publishes at T must still reach the slaves before their steps after T
#include "launcher_support.hpp"
#include "simulation_run.hpp"
#include "test_timeout_scale.hpp"

#include "vehicle_sil/cli/args.hpp"

#include <exception>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace vehicle_sil::test;

namespace {

struct Options
{
    SimulationSetup setup;
    fs::path master;
    fs::path slave;
    std::string scenario;
    long long masterStepMs = 5;
};

Options parse(int argc, char** argv)
{
    const vehicle_sil::cli::Args args(argc, argv);
    Options o;
    o.setup.registry = args.required("registry");
    o.setup.controller = args.required("controller");
    o.setup.work_dir = args.required("work-dir");
    // An upper bound of waiting: scaled by VEHICLE_SIL_TEST_TIMEOUT_SCALE.
    o.setup.timeout = vehicle_sil::test::scaled_timeout(args.millis("timeout-ms", o.setup.timeout));
    o.master = args.required("master");
    o.slave = args.required("slave");
    o.scenario = args.required("scenario");
    // The scenario's times are fixed below: always 1000 ms.
    o.setup.end_ms = 1000;
    o.masterStepMs = o.scenario == "cluster-master-10ms" ? 10 : 5;
    return o;
}

std::vector<PeerSpec> peers(const Options& o, const std::vector<std::string>& echoExtra = {})
{
    std::vector<std::string> echo{"--role", "echo", "--step-us", "2000", "--bus-idle-timeout-ms", "60"};
    echo.insert(echo.end(), echoExtra.begin(), echoExtra.end());
    return {{"LinMaster",
             o.master,
             "master",
             {"--step-us", std::to_string(o.masterStepMs * 1000), "--sleep-at-ms", "200,600", "--wakeup-at-ms", "400"}},
            {"LinEcho", o.slave, "echo", echo},
            {"LinStatus", o.slave, "status", {"--role", "status", "--step-us", "4000", "--wakeup-at-ms", "700"}}};
}

/// The times at which the master sent headers: every 10 ms while the cluster is awake. After a
/// wakeup pulse at P the schedule restarts at the first master step at least 100 ms later.
std::vector<long long> expected_headers_ms()
{
    std::vector<long long> out;
    for (const auto& [from, to] : std::vector<std::pair<long long, long long>>{{0, 200}, {500, 600}, {800, 990}})
    {
        for (long long t = from; t < to; t += 10)
        {
            out.push_back(t);
        }
    }
    return out;
}

std::string command_hex(long long ms)
{
    return hex_bytes({static_cast<unsigned>(ms & 0xFF), static_cast<unsigned>((ms >> 8) & 0xFF)});
}

std::vector<std::string> states_of(const std::string& trace)
{
    std::vector<std::string> out;
    for (const auto& r : parse_trace(trace, "state"))
    {
        out.push_back(std::to_string(r.num("t_us") / 1000) + ":" + r.str("state"));
    }
    return out;
}

std::string joined(const std::vector<std::string>& items)
{
    std::string out;
    for (const auto& item : items)
    {
        out += (out.empty() ? "" : " ") + item;
    }
    return out;
}

void check_states(const RunResult& run, const std::string& stem, const std::vector<std::string>& expected)
{
    const auto actual = states_of(run.traces.at(stem));
    require(actual == expected, stem + " states [" + joined(actual) + "], expected [" + joined(expected) + "]");
    log("CHECK " + stem + " states [" + joined(actual) + "] -> ok");
}

void check_reference(const Options& o, const RunResult& run)
{
    // The master wakes on the status slave's pulse (700 ms) at its first step after it.
    const std::string masterWake = std::to_string(700 + o.masterStepMs) + ":operational";
    // Sleep and wakeup. The master sleeps at 200 ms and wakes at 400 ms. The echo slave's bus
    // is idle from 400 ms, so it sleeps at 460 ms until the header at 500 ms. The master sleeps
    // again at 600 ms. The status slave's pulse at 700 ms wakes the echo slave at 700 ms and the
    // master at its first step after the pulse (705 or 710 ms); the schedule restarts at 800 ms, so the
    // echo slave sleeps again from 760 ms until then.
    check_states(run, "master", {"200:sleep", "400:operational", "600:sleep", masterWake});
    check_states(run, "echo", {"200:sleep", "400:operational", "460:sleep", "500:operational", "600:sleep",
                               "700:operational", "760:sleep", "800:operational"});
    check_states(run, "status", {"200:sleep", "400:operational", "600:sleep", "700:operational"});
    const auto pulses = parse_trace(run.traces.at("status"), "wakeup");
    require(pulses.size() == 1 && pulses[0].num("t_us") == 700000, "status slave did not send exactly one pulse at 700 ms");
    require(parse_trace(run.traces.at("echo"), "wakeup").empty(), "echo slave sent a pulse");

    // Schedule: command, echo, status every 10 ms while awake, restarting with the command.
    const auto results = parse_trace(run.traces.at("master"), "result");
    const auto headers = expected_headers_ms();
    require(results.size() == headers.size(),
            "master reported " + std::to_string(results.size()) + " results, expected " + std::to_string(headers.size()));
    std::vector<long long> commands; // header times of the commands, in order
    for (std::size_t i = 0; i < results.size(); ++i)
    {
        const auto& r = results[i];
        const long long t = r.num("t_us") / 1000;
        require(t == headers[i], "header " + std::to_string(i) + " at " + std::to_string(t) + " ms, expected " +
                                     std::to_string(headers[i]) + " ms");
        // The schedule starts at its first entry after every restart.
        const std::size_t sinceRestart = static_cast<std::size_t>(
            (t - (t >= 800 ? 800 : t >= 500 ? 500 : 0)) / 10);
        const char* ids[] = {"0x10", "0x20", "0x21"};
        const std::string id = ids[sinceRestart % 3];
        require(r.str("id") == id, "header at " + std::to_string(t) + " ms is " + r.str("id") + ", expected " + id);
        require(r.str("status") == "ok", "header at " + std::to_string(t) + " ms: " + r.str("status"));
        if (id == "0x10")
        {
            // The master's own frame carries what it set in the same step.
            require(r.str("data") == command_hex(t), "command at " + std::to_string(t) + " ms carried " + r.str("data"));
            commands.push_back(t);
        }
        else if (id == "0x20")
        {
            // The echo slave received the command 10 ms earlier at its next step.
            require(r.str("data") == command_hex(t - 10), "echo at " + std::to_string(t) + " ms carried " + r.str("data"));
        }
        else
        {
            // The status slave counted every command up to the one 20 ms earlier.
            const auto counted = static_cast<unsigned>(commands.size() & 0xFF);
            require(r.str("data") == hex_bytes({counted}),
                    "status at " + std::to_string(t) + " ms carried " + r.str("data") + ", expected " + hex_bytes({counted}));
        }
    }
    log("CHECK master: " + std::to_string(results.size()) + " results, schedule and data -> ok");

    // Both slaves received every command, stamped with its header time, at their first step
    // after it, and nothing else.
    for (const auto& [stem, stepMs] : std::vector<std::pair<std::string, long long>>{{"echo", 2}, {"status", 4}})
    {
        const auto frames = parse_trace(run.traces.at(stem), "frame");
        // The last command (980 ms) is reported by the master at 990 ms; the slaves receive it too.
        require(frames.size() == commands.size(), stem + " received " + std::to_string(frames.size()) +
                                                      " frames, expected " + std::to_string(commands.size()));
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            const long long t = frames[i].num("t_us") / 1000;
            const long long rx = frames[i].num("rx_us") / 1000;
            require(t == commands[i] && frames[i].str("id") == "0x10" && frames[i].str("data") == command_hex(t),
                    stem + " frame " + std::to_string(i) + " at " + std::to_string(t) + " ms is wrong");
            require(rx == (t / stepMs + 1) * stepMs, stem + " handled the command of " + std::to_string(t) +
                                                         " ms at its step " + std::to_string(rx) + " ms");
        }
        log("CHECK " + stem + ": " + std::to_string(frames.size()) + " commands received at their header times -> ok");
    }
}

int run_scenario(const Options& o)
{
    const RunResult reference = run_simulation(o.setup, "reference", peers(o));
    check_reference(o, reference);
    if (o.scenario == "cluster")
    {
        return 0;
    }
    if (o.scenario == "cluster-paced" || o.scenario == "cluster-master-10ms")
    {
        const RunResult paced = run_simulation(o.setup, "paced", peers(o, {"--real-time-factor", "1"}));
        check_identical(reference, paced, "paced");
        return 0;
    }
    throw std::invalid_argument("unknown scenario " + o.scenario);
}

} // namespace

int main(int argc, char** argv)
{
    set_log_tag("lin-launcher");
    try
    {
        vehicle_sil::test::check_timeout_scale();
        const Options options = parse(argc, argv);
        fs::create_directories(options.setup.work_dir);
        log("scenario " + options.scenario);
        const int result = run_scenario(options);
        log("PASS");
        return result;
    }
    catch (const std::exception& e)
    {
        log(std::string("FAIL: ") + e.what());
        return 1;
    }
}
