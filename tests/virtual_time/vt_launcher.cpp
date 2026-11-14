// SPDX-License-Identifier: MIT
//
// Virtual-time integration test. Runs a real sil-kit-registry, vehicle_sil_sim_controller and
// four time-synchronised peers with different fixed steps as separate processes:
//
//   Physics         1 ms   external physics simulator, publishes the vehicle state
//   Ecu            10 ms   existing ECU model (shared library, 10 ms tick()) + J1939
//   LinSensor       2 ms   LIN slave, reports a reading derived from the vehicle state
//   BodyController  5 ms   LIN master (10 ms slots) + J1939 monitor
//
// Every scenario first runs the simulation as fast as possible (the reference run) and checks
// the traces against each other: every peer executed exactly its steps, and every value crossed
// from one peer to another at the virtual time the synchronisation rules predict. A scenario
// with a different speed then runs the same simulation again and requires byte-identical
// traces, plus a wall-clock duration that matches the speed:
//
//   fast        reference only: must run faster than real time
//   slow-tick   the ECU's tick() takes 25 ms of wall time per 10 ms tick: everyone waits for the
//               slowest peer, so the simulation runs slower than real time
//   paced-fast  all peers paced to 4x real time
//   paced-slow  only the LIN sensor paced to 0.5x real time: the others follow it
//   peer-failure  the physics model throws at 300 ms: the controller aborts the stalled
//               simulation and every process exits with 1
#include "launcher_support.hpp"
#include "simulation_run.hpp"
#include "test_timeout_scale.hpp"

#include "vehicle_sil/cli/args.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace vehicle_sil::test;

namespace {

struct Options
{
    std::map<std::string, fs::path> exe; // registry, controller, physics, ecu, lin-sensor, body-controller
    fs::path workDir;
    std::string scenario;
    long long endMs = 1000;
    std::chrono::milliseconds timeout{120000};
};

Options parse(int argc, char** argv)
{
    const vehicle_sil::cli::Args args(argc, argv);
    Options o;
    for (const char* name : {"registry", "controller", "physics", "ecu", "lin-sensor", "body-controller"})
    {
        o.exe[name] = args.required(name);
    }
    o.workDir = args.required("work-dir");
    o.scenario = args.required("scenario");
    o.endMs = static_cast<long long>(args.u64("end-ms", 1000, 1'000'000));
    // An upper bound of waiting: scaled by VEHICLE_SIL_TEST_TIMEOUT_SCALE.
    o.timeout = vehicle_sil::test::scaled_timeout(args.millis("timeout-ms", o.timeout));
    return o;
}

// --- cross-peer checks of the reference run ---------------------------------------------------

void check_reference(const Options& o, const RunResult& run)
{
    const long long endUs = o.endMs * 1000;

    // Physics: one state per 1 ms step, 0 .. end.
    const auto states = parse_trace(run.traces.at("physics"), "state");
    require(static_cast<long long>(states.size()) == o.endMs, "physics traced " + std::to_string(states.size()) +
                                                                  " steps, expected " + std::to_string(o.endMs));
    std::map<long long, const Record*> physicsAt; // t_us -> state
    for (std::size_t i = 0; i < states.size(); ++i)
    {
        require(states[i].num("t_us") == static_cast<long long>(i) * 1000, "physics step grid broken at index " + std::to_string(i));
        physicsAt[states[i].num("t_us")] = &states[i];
    }
    require(states.back().num("speed") > 0 && states.back().num("distance") > 0, "physics model did not move");
    // The latest vehicle state a peer can see at its step t was published at t - 1 ms.
    auto physicsBefore = [&](long long tUs) -> const Record* {
        return tUs < 1000 ? nullptr : physicsAt.at(tUs - 1000);
    };

    // ECU: one tick per 10 ms; the tick's input is the physics state published 1 ms earlier.
    const auto ticks = parse_trace(run.traces.at("ecu"), "tick");
    require(static_cast<long long>(ticks.size()) == o.endMs / 10, "ECU ran " + std::to_string(ticks.size()) + " ticks, expected " +
                                                                      std::to_string(o.endMs / 10));
    for (std::size_t i = 0; i < ticks.size(); ++i)
    {
        const long long t = ticks[i].num("t_ms");
        require(t == static_cast<long long>(i) * 10 && ticks[i].num("n") == static_cast<long long>(i) + 1,
                "ECU tick grid broken at index " + std::to_string(i));
        const Record* p = physicsBefore(t * 1000);
        const long long expected = p ? p->num("speed") : 0;
        require(ticks[i].num("speed_in") == expected, "ECU tick at " + std::to_string(t) + " ms saw speed " +
                                                          ticks[i].str("speed_in") + ", physics had " +
                                                          std::to_string(expected) + " at the previous 1 ms step");
    }
    // Address Claimed goes out in the ECU's first step; the address is its own 250 ms later (the
    // NAME is not arbitrary address capable and 0x80 is not one of the addresses claimed without
    // the wait).
    const auto claim = parse_trace(run.traces.at("ecu"), "claim");
    const auto claimed = parse_trace(run.traces.at("ecu"), "claimed");
    require(claim.size() == 1 && claim[0].str("address") == "0x80" && claim[0].num("t_ms") == 0,
            "ECU did not send its claim for address 0x80 in its first step");
    require(claimed.size() == 1 && claimed[0].str("address") == "0x80" &&
                claimed[0].num("t_ms") == claim[0].num("t_ms") + 250,
            "ECU did not own address 0x80 250 ms after its claim");
    require(parse_trace(run.traces.at("ecu"), "address").empty(), "ECU's address changed after the claim");

    // LIN sensor: one reading per 2 ms step, derived from the physics state 1 ms earlier.
    const auto sets = parse_trace(run.traces.at("lin_sensor"), "set");
    require(static_cast<long long>(sets.size()) == o.endMs / 2, "LIN sensor ran " + std::to_string(sets.size()) +
                                                                    " steps, expected " + std::to_string(o.endMs / 2));
    std::map<long long, const Record*> sensorAt;
    for (std::size_t i = 0; i < sets.size(); ++i)
    {
        const long long t = sets[i].num("t_us");
        require(t == static_cast<long long>(i) * 2000 && sets[i].num("counter") == static_cast<long long>(i % 256),
                "LIN sensor step grid broken at index " + std::to_string(i));
        const Record* p = physicsBefore(t);
        require(sets[i].num("distance_cm") == (p ? p->num("distance") / 10 : 0),
                "LIN sensor at " + std::to_string(t) + " us did not use the physics state of the previous 1 ms step");
        sensorAt[t] = &sets[i];
    }

    // Body controller, LIN: headers every 10 ms (distance, status, distance, ...); a response
    // carries the sensor value set at the newest sensor step strictly before the header.
    const auto lin = parse_trace(run.traces.at("body_controller"), "lin");
    require(static_cast<long long>(lin.size()) == o.endMs / 10 - 1, "body controller reported " +
                                                                         std::to_string(lin.size()) + " LIN results, expected " +
                                                                         std::to_string(o.endMs / 10 - 1));
    for (std::size_t i = 0; i < lin.size(); ++i)
    {
        const long long header = lin[i].num("header_us");
        const bool distanceFrame = (i % 2) == 0;
        require(header == static_cast<long long>(i) * 10000 && lin[i].str("id") == (distanceFrame ? "0x20" : "0x21"),
                "LIN schedule broken at index " + std::to_string(i));
        if (header == 0)
        {
            require(lin[i].str("status") == "no_response", "LIN header at 0 must find no value set before it");
            continue;
        }
        require(lin[i].str("status") == "ok", "LIN header at " + std::to_string(header) + " us: " + lin[i].str("status"));
        const Record& s = *sensorAt.at(header - 2000);
        const auto cm = static_cast<unsigned>(s.num("distance_cm"));
        const std::string expected = distanceFrame ? hex_bytes({cm & 0xFFu, cm >> 8, static_cast<unsigned>(s.num("counter"))})
                                                   : hex_bytes({static_cast<unsigned>(s.num("moving"))});
        require(lin[i].str("data") == expected, "LIN header at " + std::to_string(header) + " us carried " +
                                                    lin[i].str("data") + ", the sensor set " + expected +
                                                    " at its step before the header");
    }

    // Body controller, J1939: every CCVS the ECU sent arrives with the ECU's step time as
    // timestamp, at the body controller's first step after it, with the ECU's value.
    const auto can = parse_trace(run.traces.at("body_controller"), "can");
    std::vector<const Record*> ccvsRx;
    int claimFrames = 0;
    for (const auto& r : can)
    {
        const long long ts = r.num("ts_us");
        const long long rx = r.num("rx_us");
        require(rx == (ts / 5000 + 1) * 5000, "CAN frame stamped " + std::to_string(ts) + " us released at " +
                                                  std::to_string(rx) + " us, not at the next 5 ms step");
        if (r.str("id") == "0x18EEFF80")
        {
            ++claimFrames;
            require(ts == claim[0].num("t_ms") * 1000, "address claim frame time differs from the ECU's claim step");
        }
        if (r.str("id") == "0x18FEF180")
        {
            ccvsRx.push_back(&r);
        }
    }
    require(claimFrames == 1, "body controller saw the ECU's address claim " + std::to_string(claimFrames) + " times, expected once");
    std::vector<const Record*> ccvsTx;
    const auto ccvs = parse_trace(run.traces.at("ecu"), "ccvs");
    for (const auto& r : ccvs)
    {
        // The CCVS of a tick carries the model's speed (raw = mm/s * 9216 / 10000, legacy_ecu.c)
        // for the input of that tick: the input reached the model, not only the trace.
        const auto tick = static_cast<std::size_t>(r.num("t_ms") / 10);
        require(tick < ticks.size() && r.num("raw") == ticks[tick].num("speed_in") * 9216 / 10000,
                "CCVS at " + r.str("t_ms") + " ms carries raw=" + r.str("raw") + ", not the model's value for the tick's input");
        // The ECU sends once its address is its own, and then every CCVS.
        require(r.num("sent") == (r.num("t_ms") >= claimed[0].num("t_ms") ? 1 : 0),
                "CCVS at " + r.str("t_ms") + " ms: sent=" + r.str("sent") + " (address claimed at " +
                    claimed[0].str("t_ms") + " ms)");
        // A frame sent at t is seen at t + 5 ms; only those before `end` are traced.
        if (r.num("sent") == 1 && r.num("t_ms") * 1000 + 5000 < endUs)
        {
            ccvsTx.push_back(&r);
        }
    }
    require(!ccvsTx.empty(), "the ECU sent no CCVS message");
    require(ccvsTx.size() == ccvsRx.size(), "ECU sent " + std::to_string(ccvsTx.size()) + " CCVS, body controller saw " +
                                                std::to_string(ccvsRx.size()));
    for (std::size_t i = 0; i < ccvsTx.size(); ++i)
    {
        const auto raw = static_cast<unsigned>(ccvsTx[i]->num("raw"));
        require(ccvsRx[i]->num("ts_us") == ccvsTx[i]->num("t_ms") * 1000 &&
                    ccvsRx[i]->str("data") == "FF" + hex_bytes({raw & 0xFFu, raw >> 8}) + "FFFFFFFFFF",
                "CCVS " + std::to_string(i) + " does not match the ECU's output");
    }
    log("CHECK reference traces consistent: " + std::to_string(states.size()) + " physics steps, " +
        std::to_string(ticks.size()) + " ECU ticks, " + std::to_string(sets.size()) + " LIN sensor steps, " +
        std::to_string(lin.size()) + " LIN results, " + std::to_string(ccvsRx.size()) + " CCVS frames, address claim at " +
        claim[0].str("t_ms") + " ms, claimed at " + claimed[0].str("t_ms") + " ms -> ok");
}

void check_wall(const std::string& label, long long wallUs, long long minUs, long long maxUs)
{
    const bool ok = wallUs >= minUs && wallUs <= maxUs;
    log("CHECK run '" + label + "' wall time " + std::to_string(wallUs / 1000) + " ms within [" +
        std::to_string(minUs / 1000) + ", " + std::to_string(maxUs / 1000) + "] ms -> " + (ok ? "ok" : "MISMATCH"));
    require(ok, "wall time of run '" + label + "' out of range");
}

SimulationSetup setup_of(const Options& o)
{
    return {o.exe.at("registry"), o.exe.at("controller"), o.workDir, o.endMs, o.timeout};
}

std::vector<PeerSpec> peers(const Options& o, const std::vector<std::string>& physicsExtra = {}, const std::vector<std::string>& ecuExtra = {},
                            const std::vector<std::string>& sensorExtra = {}, const std::vector<std::string>& bodyExtra = {})
{
    return {{"Physics", o.exe.at("physics"), "physics", physicsExtra},
            {"Ecu", o.exe.at("ecu"), "ecu", ecuExtra},
            {"LinSensor", o.exe.at("lin-sensor"), "lin_sensor", sensorExtra},
            {"BodyController", o.exe.at("body-controller"), "body_controller", bodyExtra}};
}

int run_scenario(const Options& o)
{
    const long long simUs = o.endMs * 1000;
    const RunResult reference = run_simulation(setup_of(o), "reference", peers(o));
    check_reference(o, reference);

    if (o.scenario == "fast")
    {
        // As fast as possible: faster than real time.
        check_wall("reference", reference.wall_us, 0, simUs - 1);
        return 0;
    }
    if (o.scenario == "slow-tick")
    {
        // 100 ticks of 25 ms each before `end`: at least 2.5 x the simulated time.
        const RunResult slow = run_simulation(setup_of(o), "slow-tick", peers(o, {}, {"--tick-cost-us", "25000"}));
        check_identical(reference, slow, "slow-tick");
        check_wall("slow-tick", slow.wall_us, (o.endMs / 10) * 25000, simUs * 10);
        require(slow.wall_us > simUs, "a slow tick() must make the simulation slower than real time");
        return 0;
    }
    if (o.scenario == "paced-fast")
    {
        const std::vector<std::string> pace{"--real-time-factor", "4"};
        const RunResult paced = run_simulation(setup_of(o), "paced-fast", peers(o, pace, pace, pace, pace));
        check_identical(reference, paced, "paced-fast");
        // The controller's clock starts at its own first step, which may begin some ms after the
        // paced peers' first steps (about 20 ms seen in sanitizer builds): allow 50 ms below.
        // The pace is a ceiling on the speed, not a promise of it: a machine that needs longer than a quarter of
        // the simulated time without a pace (a sanitizer build on a slow machine) needs as long with one.
        const long long unhurried = std::max(simUs / 4, reference.wall_us);
        check_wall("paced-fast", paced.wall_us, simUs / 4 - 50000, unhurried + simUs / 16 + 200000);
        return 0;
    }
    if (o.scenario == "paced-slow")
    {
        const RunResult paced = run_simulation(setup_of(o), "paced-slow", peers(o, {}, {}, {"--real-time-factor", "0.5"}));
        check_identical(reference, paced, "paced-slow");
        check_wall("paced-slow", paced.wall_us, simUs * 2 - 50000, simUs * 2 + simUs / 2 + 200000);
        require(paced.wall_us > simUs, "pacing one peer to 0.5 x must make the simulation slower than real time");
        return 0;
    }
    if (o.scenario == "peer-failure")
    {
        // The physics model throws at 300 ms. SIL Kit only stalls virtual time; the controller must
        // notice the failed participant and abort, and every process must report the failure (the
        // others because they did not reach --end-ms) instead of waiting for a timeout.
        const RunResult failed = run_simulation(setup_of(o), "peer-failure", peers(o, {"--fail-at-ms", "300"}), 1);
        check_wall("peer-failure", failed.wall_us, 0, 30'000'000);
        return 0;
    }
    throw std::invalid_argument("unknown scenario " + o.scenario);
}

} // namespace

int main(int argc, char** argv)
{
    set_log_tag("vt-launcher");
    try
    {
        const Options options = parse(argc, argv);
        fs::create_directories(options.workDir);
        log("scenario " + options.scenario + ", " + std::to_string(options.endMs) + " ms of simulation time");
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
