// SPDX-License-Identifier: MIT
//
// LinMaster and LinSlave on the virtual LIN buses of a LocalBench, written against Ecu: the
// schedule, master frames, the response rule (a value set at a step strictly before the header),
// independence of the order of the ECUs, no response, collisions, diagnostic frames, sleep and
// wakeup (also simultaneous pulses and no wakeup delay), bus idle, schedule switches, a slave's
// inbox overflow, and the rules of Ecu::lin_master() and lin_slave(). The scenarios are the main
// ones of the SIL Kit tests (test_lin_cluster.cpp, test_lin_protocol.cpp), with their expected
// records: the virtual bus must give the same results.
#include "test_harness.hpp"

#include "vehicle_sil/local/local_bench.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::lin;
using vehicle_sil::unit::Failure;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;

constexpr LinId kCommandId = 0x10; // published by the master
constexpr LinId kEchoId = 0x20;    // slave 0 echoes the newest command it received
constexpr LinId kStatusId = 0x21;  // slave 1 reports a constant
constexpr LinId kExtraId = 0x22;   // slave 1, only in a second schedule
constexpr std::uint8_t kStatusValue = 0x5A;
constexpr const char* kBus = "LIN_CLUSTER";

LinScheduleTable default_schedule()
{
    return {{kCommandId, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master},
            {kEchoId, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave},
            {kStatusId, 1, LinChecksumModel::Classic, 10ms, LinPublisher::Slave}};
}

SimTime at(std::chrono::milliseconds ms)
{
    return SimTime{} + ms;
}

long long ms_of(SimTime t)
{
    return static_cast<long long>(t.time_since_epoch() / 1ms);
}

/// The command the master publishes from its step at `t`: the time in ms, little endian.
std::array<std::uint8_t, 2> command_at(SimTime t)
{
    const auto v = static_cast<std::uint16_t>(t.time_since_epoch() / 1ms);
    return {static_cast<std::uint8_t>(v & 0xFFu), static_cast<std::uint8_t>(v >> 8)};
}

std::string hex(const std::uint8_t* data, std::size_t n)
{
    std::string s;
    char buf[4];
    for (std::size_t i = 0; i < n; ++i)
    {
        std::snprintf(buf, sizeof(buf), "%02X", data[i]);
        s += buf;
    }
    return s;
}

const char* status_name(LinResultStatus s)
{
    switch (s)
    {
    case LinResultStatus::Ok:
        return "ok";
    case LinResultStatus::NoResponse:
        return "no_response";
    case LinResultStatus::Error:
        return "error";
    }
    return "?";
}

std::string describe(const LinScheduleResult& r)
{
    return "t=" + std::to_string(ms_of(r.header_time)) + " id=" + std::to_string(r.id) + " " + status_name(r.status) +
           " data=" + hex(r.data.data(), r.data_length);
}

bool contains(const std::vector<std::string>& lines, const std::string& line)
{
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

void require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw Failure(what);
    }
}

std::vector<std::string> starting_with(const std::vector<std::string>& lines, const std::string& prefix)
{
    std::vector<std::string> out;
    for (const auto& line : lines)
    {
        if (line.rfind(prefix, 0) == 0)
        {
            out.push_back(line);
        }
    }
    return out;
}

struct SlaveSpec
{
    SimDuration step = 2ms;
    LinSlaveOptions options{{kCommandId}, std::nullopt};
};

/// A master ECU ("Master") and slave ECUs ("Slave0", ...) with one LIN node each on the bus
/// kBus of a LocalBench.
struct Cluster
{
    LinScheduleTable schedule = default_schedule();
    SimDuration master_step = 5ms;
    LinMasterOptions master;
    std::vector<SlaveSpec> slaves{SlaveSpec{}, SlaveSpec{}};
    SimDuration stop_at = 600ms;
    /// Add the slave ECUs before the master ECU (the steps of one time then run slaves first).
    bool slaves_first = false;
    /// The roles of the SIL Kit cluster test: the master publishes command_at(now) on
    /// kCommandId from `publish_from` on, slave 0 echoes the newest command on kEchoId, slave 1
    /// answers kStatusValue on kStatusId (classic) and kExtraId.
    bool cluster_roles = true;
    SimTime publish_from{};
    /// In the master ECU's step, before its LIN step.
    std::function<void(SimTime, LinMaster&)> master_script;
    /// In a slave ECU's step, before the cluster roles set their responses.
    std::function<void(std::size_t, SimTime, LinSlave&)> slave_script;
    /// In a slave ECU's step, after the cluster roles set their responses.
    std::function<void(std::size_t, SimTime, LinSlave&)> slave_after_step;
    /// In a slave's LIN step, for every frame it receives.
    std::function<void(std::size_t, SimTime, const LinReceivedFrame&, LinSlave&)> slave_frame;
    /// If set: one more slave ("Stalled", subscribed to `stalled_subscriptions`), added after
    /// the other ECUs. An Ecu steps its LIN nodes in every step, so this slave is a LinSlave on
    /// an endpoint of the bus, which its ECU (step 1 ms) steps only when this returns true.
    std::function<bool(SimTime)> stalled_steps;
    std::vector<LinId> stalled_subscriptions;
    /// The tests that overflow an inbox on purpose turn the check off.
    bool expect_no_overflow = true;
};

/// Everything the nodes and the bus observed, as text that depends only on simulation time and
/// data.
struct Trace
{
    std::vector<std::string> master;
    std::vector<std::vector<std::string>> slaves;
    std::vector<std::string> bus;
    std::vector<LinScheduleResult> results;
    LinMasterStatistics master_stats;
    std::vector<LinSlaveStatistics> slave_stats;
    /// Cluster::stalled_steps: the stalled slave's frames and statistics.
    std::vector<std::string> stalled;
    LinSlaveStatistics stalled_stats;
};

Trace run(const Cluster& c)
{
    LocalBench bench;
    Trace out;
    out.slaves.resize(c.slaves.size());
    // As in the SIL Kit tests: keep only records well before the end.
    const SimTime keepBefore = SimTime{} + c.stop_at - 20ms;
    std::vector<SimTime> slaveNow(c.slaves.size());
    std::vector<std::array<std::uint8_t, 2>> lastCommand(c.slaves.size());
    LinMaster* master = nullptr;
    std::vector<LinSlave*> slaves(c.slaves.size());

    auto add_master = [&] {
        Ecu& ecu = bench.add_ecu("Master", c.master_step);
        LinMaster& m = ecu.lin_master(kBus, c.schedule, c.master);
        master = &m;
        m.on_result([&](const LinScheduleResult& r) {
            if (r.header_time < keepBefore)
            {
                out.results.push_back(r);
                out.master.push_back("result " + describe(r));
            }
        });
        m.on_state([&](SimTime t, LinNodeState s) {
            if (t < keepBefore)
            {
                out.master.push_back("state t=" + std::to_string(ms_of(t)) + " " + to_string(s));
            }
        });
        ecu.on_step([&](SimTime now) {
            if (c.cluster_roles && now >= c.publish_from)
            {
                m.set_frame_data(kCommandId, command_at(now));
            }
            if (c.master_script)
            {
                c.master_script(now, m);
            }
        });
    };
    auto add_slave = [&](std::size_t i) {
        Ecu& ecu = bench.add_ecu("Slave" + std::to_string(i), c.slaves[i].step);
        LinSlave& s = ecu.lin_slave(kBus, c.slaves[i].options);
        slaves[i] = &s;
        auto& trace = out.slaves[i];
        s.on_frame([&, i](const LinReceivedFrame& f) {
            if (f.header_time < keepBefore)
            {
                trace.push_back("frame t=" + std::to_string(ms_of(f.header_time)) + " rx=" + std::to_string(ms_of(slaveNow[i])) +
                                " id=" + std::to_string(f.id) + " data=" + hex(f.data.data(), f.data_length));
            }
            if (f.id == kCommandId && f.data_length == 2)
            {
                lastCommand[i] = {f.data[0], f.data[1]};
            }
            if (c.slave_frame)
            {
                c.slave_frame(i, slaveNow[i], f, *slaves[i]);
            }
        });
        s.on_state([&](SimTime t, LinNodeState state) {
            if (t < keepBefore)
            {
                trace.push_back("state t=" + std::to_string(ms_of(t)) + " " + to_string(state));
            }
        });
        ecu.on_step([&, i](SimTime now) {
            slaveNow[i] = now;
            LinSlave& slave = *slaves[i];
            if (c.slave_script)
            {
                c.slave_script(i, now, slave);
            }
            if (c.cluster_roles && i == 0)
            {
                slave.set_response(now, kEchoId, lastCommand[0]);
            }
            else if (c.cluster_roles && i == 1)
            {
                const std::array<std::uint8_t, 1> status{kStatusValue};
                slave.set_response(now, kStatusId, status, LinChecksumModel::Classic);
                slave.set_response(now, kExtraId, status);
            }
            if (c.slave_after_step)
            {
                c.slave_after_step(i, now, slave);
            }
        });
    };
    if (!c.slaves_first)
    {
        add_master();
    }
    for (std::size_t i = 0; i < c.slaves.size(); ++i)
    {
        add_slave(i);
    }
    if (c.slaves_first)
    {
        add_master();
    }
    SimTime stalledNow{};
    LinSlave* stalled = nullptr;
    if (c.stalled_steps)
    {
        Ecu& ecu = bench.add_ecu("Stalled", 1ms);
        auto& endpoint = bench.lin_bus(kBus).attach(LinRole::Slave, "Stalled");
        LinSlave& s = ecu.emplace<LinSlave>(endpoint, LinSlaveOptions{c.stalled_subscriptions, std::nullopt});
        s.init();
        stalled = &s;
        s.on_frame([&](const LinReceivedFrame& f) {
            out.stalled.push_back("frame t=" + std::to_string(ms_of(f.header_time)) + " rx=" + std::to_string(ms_of(stalledNow)) +
                                  " id=" + std::to_string(f.id) + " data=" + hex(f.data.data(), f.data_length));
        });
        ecu.on_step([&](SimTime now) {
            stalledNow = now;
            if (c.stalled_steps(now))
            {
                stalled->step(now);
            }
        });
    }
    bench.lin_bus(kBus).add_observer([&](const LinBusEvent& e) {
        if (e.time >= keepBefore)
        {
            return;
        }
        const char* kinds[] = {"frame", "no_response", "collision", "go_to_sleep", "wakeup"};
        out.bus.push_back(std::string(kinds[static_cast<int>(e.kind)]) + " t=" + std::to_string(ms_of(e.time)) +
                          " from=" + std::string(e.sender) + " id=" + std::to_string(e.frame.id) +
                          " data=" + hex(e.frame.data.data(), e.frame.data_length));
    });

    bench.run_until(SimTime{} + c.stop_at);
    out.master_stats = master->statistics();
    for (const auto* s : slaves)
    {
        out.slave_stats.push_back(s->statistics());
    }
    if (stalled != nullptr)
    {
        out.stalled_stats = stalled->statistics();
    }
    if (c.expect_no_overflow)
    {
        CHECK_EQ(out.master_stats.inbox_overflows, 0u);
        for (const auto& stats : out.slave_stats)
        {
            CHECK_EQ(stats.inbox_overflows, 0u);
        }
        CHECK_EQ(out.stalled_stats.inbox_overflows, 0u);
    }
    return out;
}

/// Runs `c` with the master's ECU added first and last; the nodes' traces must be the same. On
/// the bus, events of one time from different ECUs follow the order of their steps, so only the
/// set of events must be the same.
Trace run_in_both_orders(Cluster c)
{
    c.slaves_first = false;
    const auto masterFirst = run(c);
    c.slaves_first = true;
    const auto slavesFirst = run(c);
    require(slavesFirst.master == masterFirst.master, "master trace depends on the order of the ECUs");
    require(slavesFirst.slaves == masterFirst.slaves, "slave traces depend on the order of the ECUs");
    require(slavesFirst.stalled == masterFirst.stalled, "stalled slave's trace depends on the order of the ECUs");
    auto sorted = [](std::vector<std::string> lines) {
        std::sort(lines.begin(), lines.end());
        return lines;
    };
    require(sorted(slavesFirst.bus) == sorted(masterFirst.bus), "bus events depend on the order of the ECUs");
    return masterFirst;
}

/// Checks one result of the default schedule while the cluster is awake (as the SIL Kit test).
void check_default_result(const LinScheduleResult& r)
{
    switch (r.id)
    {
    case kCommandId:
    {
        require(r.publisher == LinPublisher::Master && r.status == LinResultStatus::Ok,
                "command at " + std::to_string(ms_of(r.header_time)) + " ms not sent");
        const auto expected = command_at(r.header_time);
        require(r.data_length == 2 && r.data[0] == expected[0] && r.data[1] == expected[1],
                "command at " + std::to_string(ms_of(r.header_time)) + " ms carried the wrong data");
        break;
    }
    case kEchoId:
        if (r.header_time > SimTime{} + 10ms)
        {
            const auto expected = command_at(r.header_time - 10ms);
            require(r.status == LinResultStatus::Ok && r.data[0] == expected[0] && r.data[1] == expected[1],
                    "echo: " + describe(r));
        }
        break;
    case kStatusId:
        require(r.status == LinResultStatus::Ok && r.data_length == 1 && r.data[0] == kStatusValue, "status: " + describe(r));
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------------------------
// Schedule, frames and the response rule
// ---------------------------------------------------------------------------------------------

TEST_CASE(lin_local_master_frames_reach_the_slaves_and_responses_reach_the_master)
{
    Cluster c;
    c.stop_at = 300ms;
    const auto t = run_in_both_orders(c);

    require(t.results.size() >= 25u, std::to_string(t.results.size()) + " results");
    for (std::size_t i = 0; i < t.results.size(); ++i)
    {
        const auto& r = t.results[i];
        require(r.header_time == SimTime{} + 10ms * static_cast<int>(i), "schedule timing broken at " + std::to_string(i));
        require(r.id == std::array<LinId, 3>{kCommandId, kEchoId, kStatusId}[i % 3], "schedule order broken");
        check_default_result(r);
    }
    for (std::size_t s = 0; s < 2; ++s)
    {
        for (std::size_t i = 0; i < t.results.size(); i += 3)
        {
            const auto h = t.results[i].header_time;
            const auto cmd = command_at(h);
            require(contains(t.slaves[s], "frame t=" + std::to_string(ms_of(h)) + " rx=" + std::to_string(ms_of(h) + 2) +
                                              " id=16 data=" + hex(cmd.data(), 2)),
                    "slave " + std::to_string(s) + " missed the command at " + std::to_string(ms_of(h)) + " ms");
        }
        for (const auto& line : t.slaves[s])
        {
            require(line.rfind("frame", 0) == 0 && line.find(" id=16 ") != std::string::npos, "unexpected record " + line);
        }
    }
    // The observers see each header once, with the responder.
    require(t.bus.size() >= 25u && t.bus[0] == "frame t=0 from=Master id=16 data=0000" &&
                t.bus[1] == "frame t=10 from=Slave0 id=32 data=0000" && t.bus[2] == "frame t=20 from=Slave1 id=33 data=5A" &&
                t.bus[3] == "frame t=30 from=Master id=16 data=1E00" && t.bus[4] == "frame t=40 from=Slave0 id=32 data=1E00",
            "bus trace: " + (t.bus.empty() ? std::string("empty") : t.bus[0]));
    CHECK_EQ(t.master_stats.errors, 0u);
}

TEST_CASE(lin_local_slave_answers_with_the_value_set_strictly_before_the_header)
{
    // Every step of the slave sets the step's time (ms) as its response, and the slave steps on
    // the master's grid: at each header time both have a step. The header at T must carry the
    // value of the slave's step before T, whether the slave's step at T ran before the master's
    // (slaves first) or after it.
    Cluster c;
    c.schedule = {{0x30, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave}};
    c.slaves = {SlaveSpec{5ms}};
    c.cluster_roles = false;
    c.stop_at = 100ms;
    c.slave_after_step = [](std::size_t, SimTime now, LinSlave& s) {
        const std::array<std::uint8_t, 1> value{static_cast<std::uint8_t>(ms_of(now))};
        s.set_response(now, 0x30, value);
    };
    const auto t = run_in_both_orders(c);
    require(t.results.size() == 8u, std::to_string(t.results.size()) + " results");
    for (const auto& r : t.results)
    {
        const auto ms = ms_of(r.header_time);
        char expected[3];
        std::snprintf(expected, sizeof(expected), "%02X", static_cast<unsigned>(ms - 5));
        require(describe(r) == "t=" + std::to_string(ms) + " id=48 " + (ms == 0 ? std::string("no_response data=") : "ok data=" + std::string(expected)),
                "unexpected " + describe(r));
    }
}

TEST_CASE(lin_local_master_reports_errors_and_cleared_responses)
{
    constexpr LinId kShortId = 0x23; // 2 bytes in the schedule, slave 1 answers with 1
    Cluster c;
    c.stop_at = 300ms;
    c.master_script = [](SimTime now, LinMaster& m) {
        if (now == SimTime{})
        {
            m.set_schedule({{kStatusId, 1, LinChecksumModel::Classic, 10ms, LinPublisher::Slave},
                            {kShortId, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave}});
        }
    };
    c.slave_after_step = [](std::size_t i, SimTime now, LinSlave& s) {
        if (i != 1)
        {
            return;
        }
        const std::array<std::uint8_t, 1> one{0x01};
        s.set_response(now, kShortId, one);
        if (now >= at(100ms))
        {
            s.clear_response(now, kStatusId); // replaces the value set earlier in this step
        }
    };
    const auto t = run_in_both_orders(c);
    require(t.results.size() >= 20u, std::to_string(t.results.size()) + " results");
    for (const auto& r : t.results)
    {
        if (r.id == kShortId)
        {
            require(r.status == LinResultStatus::Error, "short response not an error: " + describe(r));
            continue;
        }
        require(r.id == kStatusId, "unexpected id " + std::to_string(r.id));
        const bool answered = r.header_time > SimTime{} && r.header_time <= at(100ms);
        require(r.status == (answered ? LinResultStatus::Ok : LinResultStatus::NoResponse), "status: " + describe(r));
    }
    require(t.master_stats.errors > 0, "no errors counted");
    require(contains(t.bus, "no_response t=120 from=Master id=33 data="), "bus did not see the missing response");
}

TEST_CASE(lin_local_checksum_model_mismatch_is_an_error)
{
    constexpr LinId kClassicId = 0x20;  // classic in the schedule, the slave answers enhanced
    constexpr LinId kEnhancedId = 0x21; // enhanced in the schedule, the slave answers classic
    constexpr LinId kMatchingId = 0x22;
    Cluster c;
    c.schedule = {{kClassicId, 2, LinChecksumModel::Classic, 10ms, LinPublisher::Slave},
                  {kEnhancedId, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave},
                  {kMatchingId, 2, LinChecksumModel::Classic, 10ms, LinPublisher::Slave}};
    c.slaves = {SlaveSpec{}};
    c.cluster_roles = false;
    c.slave_after_step = [](std::size_t, SimTime now, LinSlave& s) {
        const std::array<std::uint8_t, 2> two{0x12, 0x34};
        s.set_response(now, kClassicId, two, LinChecksumModel::Enhanced);
        s.set_response(now, kEnhancedId, two, LinChecksumModel::Classic);
        s.set_response(now, kMatchingId, two, LinChecksumModel::Classic);
    };
    const auto t = run(c);
    require(t.results.size() >= 24u, std::to_string(t.results.size()) + " results");
    for (const auto& r : t.results)
    {
        if (r.header_time == SimTime{})
        {
            continue;
        }
        const auto expected = r.id == kMatchingId ? "ok data=1234" : "error data=";
        require(describe(r) == "t=" + std::to_string(ms_of(r.header_time)) + " id=" + std::to_string(r.id) + " " + expected,
                "unexpected " + describe(r));
    }
}

TEST_CASE(lin_local_responses_collide_and_slaves_take_turns)
{
    Cluster c;
    c.schedule = {{0x30, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave},
                  {0x31, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave},
                  {0x18, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave}};
    c.cluster_roles = false;
    c.stop_at = 300ms;
    c.slave_after_step = [](std::size_t i, SimTime now, LinSlave& s) {
        const std::array<std::uint8_t, 1> value{static_cast<std::uint8_t>(0xA0 + i)};
        s.set_response(now, 0x30, value); // both: a collision
        if (i == 1)
        {
            s.set_response(now, 0x31, value); // slave 1 only
        }
        // 0x18: slave 0 until 100 ms, slave 1 until 200 ms, then nobody.
        const bool mine = i == 0 ? now < at(100ms) : now >= at(100ms) && now < at(200ms);
        if (mine)
        {
            const std::array<std::uint8_t, 1> turn{static_cast<std::uint8_t>(0xB0 + i)};
            s.set_response(now, 0x18, turn);
        }
        else
        {
            s.clear_response(now, 0x18);
        }
    };
    const auto t = run_in_both_orders(c);
    require(t.results.size() >= 27u, std::to_string(t.results.size()) + " results");
    for (const auto& r : t.results)
    {
        const auto ms = ms_of(r.header_time);
        if (ms == 0)
        {
            continue;
        }
        std::string expected;
        if (r.id == 0x30)
        {
            expected = "error data=";
        }
        else if (r.id == 0x31)
        {
            expected = "ok data=A1";
        }
        else
        {
            expected = ms > 200 ? "no_response data=" : ms <= 100 ? "ok data=B0" : "ok data=B1";
        }
        require(describe(r) == "t=" + std::to_string(ms) + " id=" + std::to_string(r.id) + " " + expected,
                "unexpected " + describe(r));
    }
    require(contains(t.bus, "collision t=30 from=Master id=48 data="), "bus did not see the collision");
}

TEST_CASE(lin_local_slave_answering_a_master_frame_collides_with_the_master)
{
    Cluster c;
    c.schedule = {{0x05, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master}};
    c.slaves = {SlaveSpec{}, SlaveSpec{2ms, LinSlaveOptions{{0x05}, std::nullopt}}};
    c.cluster_roles = false;
    c.stop_at = 200ms;
    c.master_script = [](SimTime, LinMaster& m) {
        const std::array<std::uint8_t, 1> value{0x11};
        m.set_frame_data(0x05, value);
    };
    c.slave_after_step = [](std::size_t i, SimTime now, LinSlave& s) {
        if (i == 0 && now >= at(50ms))
        {
            const std::array<std::uint8_t, 1> value{0x22};
            s.set_response(now, 0x05, value);
        }
    };
    const auto t = run_in_both_orders(c);
    require(t.results.size() >= 17u, std::to_string(t.results.size()) + " results");
    for (const auto& r : t.results)
    {
        const std::string expected = r.header_time <= at(50ms) ? "ok data=11" : "error data=";
        require(describe(r) == "t=" + std::to_string(ms_of(r.header_time)) + " id=5 " + expected, "unexpected " + describe(r));
    }
    CHECK(t.master_stats.errors >= 11u);
    // Slave 1 (subscribed) received the master's frame until the collisions began.
    require(starting_with(t.slaves[1], "frame").size() == 6u && t.slaves[1].back() == "frame t=50 rx=52 id=5 data=11",
            "slave 1 frames: " + std::to_string(t.slaves[1].size()));
}

// ---------------------------------------------------------------------------------------------
// Diagnostic frames
// ---------------------------------------------------------------------------------------------

constexpr std::uint8_t kNad0 = 0x0A; // node address of slave 0
constexpr std::uint8_t kNad1 = 0x0B; // node address of slave 1

LinScheduleTable diagnostic_schedule()
{
    return {{kCommandId, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master},
            {kMasterRequestId, 8, LinChecksumModel::Classic, 10ms, LinPublisher::Master},
            {kSlaveResponseId, 8, LinChecksumModel::Classic, 10ms, LinPublisher::Slave}};
}

TEST_CASE(lin_local_diagnostic_request_is_sent_once_and_answered_once)
{
    // A request to slave 0 queued at 45 ms; the master request slot comes at 70 ms, the slave
    // response slot at 80 ms. Slave 1 has another node address and stays silent. The bytes after
    // the node address are opaque to LinMaster and LinSlave.
    const std::array<std::uint8_t, 8> request{kNad0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    Cluster c;
    c.schedule = diagnostic_schedule();
    c.cluster_roles = false;
    c.slaves = {SlaveSpec{2ms, LinSlaveOptions{{kMasterRequestId}, std::nullopt}},
                SlaveSpec{2ms, LinSlaveOptions{{kMasterRequestId}, std::nullopt}}};
    c.master_script = [&request](SimTime now, LinMaster& m) {
        const std::array<std::uint8_t, 1> command{0x01};
        m.set_frame_data(kCommandId, command);
        if (now == at(45ms))
        {
            require(m.send_master_request(request), "request refused");
            require(!m.send_master_request(request), "a second request was accepted while the first was pending");
        }
        if (now == at(75ms))
        {
            require(!m.master_request_pending(), "request still pending after its slot");
        }
    };
    c.slave_frame = [](std::size_t i, SimTime now, const LinReceivedFrame& f, LinSlave& s) {
        if (f.id == kMasterRequestId && f.data[0] == (i == 0 ? kNad0 : kNad1))
        {
            const std::array<std::uint8_t, 8> response{f.data[0], 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03};
            s.set_slave_response(now, response);
        }
    };
    const auto t = run_in_both_orders(c);

    std::vector<std::string> requests;
    std::vector<std::string> responses;
    for (const auto& r : t.results)
    {
        if (r.id == kMasterRequestId)
        {
            requests.push_back(describe(r));
        }
        if (r.id == kSlaveResponseId)
        {
            responses.push_back(describe(r));
        }
    }
    // Without a request, the master request slot stays silent: no header, no result.
    require(requests == std::vector<std::string>{"t=70 id=60 ok data=0A01020304050607"}, "master requests differ");
    require(responses.size() >= 9u, std::to_string(responses.size()) + " slave response slots");
    for (const auto& r : responses)
    {
        require(r == (r.rfind("t=80 ", 0) == 0 ? "t=80 id=61 ok data=0A09080706050403" : r.substr(0, r.find(' ')) + " id=61 no_response data="),
                "slave response: " + r);
    }
    for (std::size_t s = 0; s < 2; ++s)
    {
        require(starting_with(t.slaves[s], "frame") == std::vector<std::string>{"frame t=70 rx=72 id=60 data=0A01020304050607"},
                "slave " + std::to_string(s) + " frames differ");
    }
    CHECK_EQ(t.slave_stats[0].responses_sent, 1u);
    CHECK_EQ(t.slave_stats[1].responses_sent, 0u);
}

// ---------------------------------------------------------------------------------------------
// Sleep and wakeup
// ---------------------------------------------------------------------------------------------

TEST_CASE(lin_local_master_sleeps_and_wakes_the_cluster)
{
    Cluster c;
    c.master_script = [](SimTime now, LinMaster& m) {
        if (now == at(100ms))
        {
            m.request_sleep();
        }
        if (now == at(300ms))
        {
            m.request_wakeup();
        }
    };
    const auto t = run_in_both_orders(c);
    require(contains(t.master, "state t=100 sleep"), "master did not sleep at 100 ms");
    require(contains(t.master, "state t=300 operational"), "master did not wake at 300 ms");
    for (std::size_t s = 0; s < 2; ++s)
    {
        require(contains(t.slaves[s], "state t=100 sleep"), "slave " + std::to_string(s) + " did not sleep at 100 ms");
        require(contains(t.slaves[s], "state t=300 operational"), "slave " + std::to_string(s) + " did not wake at 300 ms");
    }
    bool restarted = false;
    SimTime previous{};
    for (const auto& r : t.results)
    {
        require(r.header_time < at(100ms) || r.header_time >= at(400ms), "header while asleep: " + describe(r));
        if (r.header_time >= at(400ms) && !restarted)
        {
            restarted = true;
            require(r.header_time == at(400ms) && r.id == kCommandId, "schedule did not restart at 400 ms");
        }
        else if (r.header_time >= at(400ms))
        {
            require(r.header_time == previous + 10ms, "schedule timing broken after the restart");
        }
        previous = r.header_time;
        check_default_result(r);
    }
    require(restarted, "schedule did not restart");
    require(contains(t.bus, "go_to_sleep t=100 from=Master id=0 data=") && contains(t.bus, "wakeup t=300 from=Master id=0 data="),
            "bus did not see the sleep and the wakeup");
    CHECK_EQ(t.master_stats.sleep_commands, 1u);
    CHECK_EQ(t.master_stats.wakeups_by_master, 1u);
    CHECK_EQ(t.master_stats.wakeups_by_slaves, 0u);
}

TEST_CASE(lin_local_slave_wakes_the_cluster)
{
    Cluster c;
    c.master_script = [](SimTime now, LinMaster& m) {
        if (now == at(100ms))
        {
            m.request_sleep();
        }
    };
    c.slave_script = [](std::size_t i, SimTime now, LinSlave& s) {
        if (i == 1 && now == at(202ms))
        {
            s.request_wakeup();
        }
    };
    const auto t = run_in_both_orders(c);
    // The pulse at 202 ms; the master wakes at its first step after it (205 ms) and restarts at
    // the first step at least 100 ms after the pulse (305 ms).
    require(contains(t.slaves[1], "state t=202 operational"), "slave 1 did not wake itself");
    require(contains(t.slaves[0], "state t=202 operational"), "slave 0 did not wake on the pulse");
    require(contains(t.master, "state t=205 operational"), "master did not wake at 205 ms");
    const auto restart = std::find_if(t.results.begin(), t.results.end(),
                                      [](const LinScheduleResult& r) { return r.header_time > at(100ms); });
    require(restart != t.results.end() && restart->header_time == at(305ms) && restart->id == kCommandId,
            "schedule did not restart at 305 ms");
    CHECK_EQ(t.master_stats.wakeups_by_slaves, 1u);
    CHECK_EQ(t.slave_stats[1].wakeups_sent, 1u);
}

TEST_CASE(lin_local_go_to_sleep_wins_over_a_wakeup_pulse_at_the_same_time)
{
    // As the SIL Kit test: the master wakes at 200 ms, restarts at 300 ms, and a sleep requested
    // at 250 ms sends the go-to-sleep command at 300 ms. The slaves sleep from 260 ms (bus idle)
    // and slave 1 sends a pulse at 300 ms. The command wins: everyone ends asleep, whichever ECU
    // steps first at 300 ms.
    Cluster c;
    c.stop_at = 400ms;
    c.slaves[0].options.bus_idle_timeout = 60ms;
    c.slaves[1].options.bus_idle_timeout = 60ms;
    c.master_script = [](SimTime now, LinMaster& m) {
        if (now == at(100ms) || now == at(250ms))
        {
            m.request_sleep();
        }
        if (now == at(200ms))
        {
            m.request_wakeup();
        }
    };
    c.slave_script = [](std::size_t i, SimTime now, LinSlave& s) {
        if (i == 1 && now == at(300ms))
        {
            s.request_wakeup();
        }
    };
    const auto t = run_in_both_orders(c);
    require(starting_with(t.master, "state") ==
                std::vector<std::string>{"state t=100 sleep", "state t=200 operational", "state t=300 sleep"},
            "master states differ");
    const std::vector<std::string> slave{"state t=100 sleep",       "state t=200 operational", "state t=260 sleep",
                                         "state t=300 operational", "state t=300 sleep"};
    for (std::size_t s = 0; s < 2; ++s)
    {
        require(starting_with(t.slaves[s], "state") == slave, "slave " + std::to_string(s) + " states differ");
    }
    CHECK_EQ(t.master_stats.wakeups_by_slaves, 0u);
    CHECK_EQ(t.slave_stats[1].wakeups_sent, 1u);
}

TEST_CASE(lin_local_simultaneous_wakeup_pulses_wake_the_master_once)
{
    // As the SIL Kit test: both slaves send a pulse at 100 ms.
    Cluster c;
    c.schedule = LinScheduleTable{{0x10, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master}};
    c.cluster_roles = false;
    c.stop_at = 300ms;
    c.slaves = {SlaveSpec{}, SlaveSpec{4ms, LinSlaveOptions{{}, std::nullopt}}};
    c.master_script = [](SimTime now, LinMaster& m) {
        const std::array<std::uint8_t, 1> value{0x01};
        m.set_frame_data(0x10, value);
        if (now == at(50ms))
        {
            m.request_sleep();
        }
    };
    c.slave_script = [](std::size_t, SimTime now, LinSlave& s) {
        if (now == at(100ms))
        {
            s.request_wakeup(); // both slaves, at the same time
        }
    };
    const auto t = run_in_both_orders(c);
    require(starting_with(t.master, "state") == std::vector<std::string>{"state t=50 sleep", "state t=105 operational"},
            "master states differ");
    CHECK_EQ(t.master_stats.wakeups_by_slaves, 1u);
    for (std::size_t s = 0; s < 2; ++s)
    {
        require(starting_with(t.slaves[s], "state") == std::vector<std::string>{"state t=50 sleep", "state t=100 operational"},
                "slave " + std::to_string(s) + " states differ");
        CHECK_EQ(t.slave_stats[s].wakeups_sent, 1u);
    }
    // The schedule restarts 100 ms after the pulses.
    const auto restart = std::find_if(t.results.begin(), t.results.end(),
                                      [](const LinScheduleResult& r) { return r.header_time > at(50ms); });
    require(restart != t.results.end() && restart->header_time == at(200ms), "schedule did not restart at 200 ms");
}

TEST_CASE(lin_local_master_drops_requests_that_do_not_apply_and_restarts_at_once_without_a_delay)
{
    // As the SIL Kit test, with wakeup_delay 0.
    Cluster c;
    c.schedule = LinScheduleTable{{0x10, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master}};
    c.cluster_roles = false;
    c.stop_at = 300ms;
    c.master.wakeup_delay = SimDuration::zero();
    c.slaves = {SlaveSpec{}};
    c.master_script = [](SimTime now, LinMaster& m) {
        const std::array<std::uint8_t, 1> value{0x01};
        m.set_frame_data(0x10, value);
        if (now == at(20ms))
        {
            m.request_wakeup(); // operational: dropped, no pulse
        }
        if (now == at(45ms))
        {
            m.request_wakeup(); // dropped; the sleep below still applies at 50 ms
            m.request_sleep();
        }
        if (now == at(120ms))
        {
            m.request_wakeup();
        }
    };
    const auto t = run_in_both_orders(c);
    require(starting_with(t.master, "state") == std::vector<std::string>{"state t=50 sleep", "state t=120 operational"},
            "master states differ");
    CHECK_EQ(t.master_stats.wakeups_by_master, 1u);
    // wakeup_delay 0: the first header goes out in the step that sends the pulse.
    const auto restart = std::find_if(t.results.begin(), t.results.end(),
                                      [](const LinScheduleResult& r) { return r.header_time > at(50ms); });
    require(restart != t.results.end() && restart->header_time == at(120ms), "schedule did not restart at 120 ms");
    require(contains(t.bus, "wakeup t=120 from=Master id=0 data="), "bus did not see the pulse at 120 ms");
}

// ---------------------------------------------------------------------------------------------
// Resource exhaustion
// ---------------------------------------------------------------------------------------------

TEST_CASE(lin_local_slave_inbox_overflow_is_counted_and_the_slave_recovers)
{
    // As the SIL Kit test: the stalled slave does not step from 10 ms to 4.5 s. A header and a
    // frame every 2 ms fill its inbox (4096 events) after about 4.1 s; the events after that are
    // lost. Slave 0 steps normally.
    Cluster c;
    c.master_step = 1ms;
    c.schedule = LinScheduleTable{{0x10, 1, LinChecksumModel::Enhanced, 2ms, LinPublisher::Master}};
    c.cluster_roles = false;
    c.slaves = {SlaveSpec{1ms, LinSlaveOptions{{0x10}, std::nullopt}}};
    c.stalled_steps = [](SimTime now) { return now < at(10ms) || now >= at(4500ms); };
    c.stalled_subscriptions = {0x10};
    c.stop_at = 4600ms;
    c.expect_no_overflow = false;
    c.master_script = [](SimTime now, LinMaster& m) {
        const std::array<std::uint8_t, 1> value{static_cast<std::uint8_t>(ms_of(now) / 2)};
        m.set_frame_data(0x10, value);
    };
    const auto t = run_in_both_orders(c);
    require(t.stalled_stats.inbox_overflows > 0u, "no overflow counted");
    CHECK_EQ(t.slave_stats[0].inbox_overflows, 0u);
    CHECK_EQ(t.master_stats.inbox_overflows, 0u);
    // After the stall, the slave receives every frame again, at its first step after the header.
    // The frame at 4500 ms is not checked: the master sent it at 4500 ms into the full inbox,
    // before the stalled slave's step at 4500 ms emptied it (it steps last here), so it was lost.
    // That holds in both orders only while the stalled ECU is added after the master: keep it
    // there, or the frame arrives in one order only and the comparison of the two orders fails.
    for (long long ms = 4502; ms < 4580; ms += 2)
    {
        const std::uint8_t value = static_cast<std::uint8_t>(ms / 2);
        require(contains(t.stalled, "frame t=" + std::to_string(ms) + " rx=" + std::to_string(ms + 1) +
                                        " id=16 data=" + hex(&value, 1)),
                "the stalled slave missed the frame at " + std::to_string(ms) + " ms after the stall");
    }
    // The overflow dropped the newest events: the oldest ones were kept and delivered in order.
    require(t.stalled.size() > 2u && t.stalled[1].rfind("frame t=2 rx=3 ", 0) == 0, "first frames differ");
}

TEST_CASE(lin_local_slave_sleeps_on_bus_idle_and_wakes_on_a_header)
{
    // The master stays silent until 150 ms: its first schedule has only a master request slot,
    // and no request is queued. At 150 ms it switches to the default schedule.
    Cluster c;
    c.stop_at = 300ms;
    c.schedule = {{kMasterRequestId, 8, LinChecksumModel::Classic, 10ms, LinPublisher::Master}};
    c.publish_from = at(150ms);
    c.slaves[0].options.bus_idle_timeout = 50ms;
    c.slaves[1].options.bus_idle_timeout = 50ms;
    c.master_script = [](SimTime now, LinMaster& m) {
        if (now == at(145ms))
        {
            m.set_schedule(default_schedule());
        }
    };
    const auto t = run_in_both_orders(c);
    for (std::size_t s = 0; s < 2; ++s)
    {
        require(contains(t.slaves[s], "state t=50 sleep"), "slave " + std::to_string(s) + " did not sleep at 50 ms");
        require(contains(t.slaves[s], "state t=150 operational"), "slave " + std::to_string(s) + " did not wake at 150 ms");
        const auto cmd = command_at(at(150ms));
        require(contains(t.slaves[s], "frame t=150 rx=152 id=16 data=" + hex(cmd.data(), 2)),
                "slave " + std::to_string(s) + " missed the command right after its idle sleep");
    }
    const auto status = std::find_if(t.results.begin(), t.results.end(),
                                     [](const LinScheduleResult& r) { return r.id == kStatusId; });
    require(status != t.results.end() && status->header_time == at(170ms) && status->status == LinResultStatus::Ok,
            "status after the idle sleep not answered");
}

TEST_CASE(lin_local_master_switches_the_schedule_at_a_slot_boundary_and_while_asleep)
{
    Cluster c;
    c.stop_at = 300ms;
    c.master_script = [](SimTime now, LinMaster& m) {
        if (now == at(50ms)) // a slot boundary
        {
            m.set_schedule({{kExtraId, 1, LinChecksumModel::Enhanced, 20ms, LinPublisher::Slave}});
        }
    };
    const auto t = run_in_both_orders(c);
    for (const auto& r : t.results)
    {
        if (r.header_time < at(50ms))
        {
            check_default_result(r);
            continue;
        }
        require(r.id == kExtraId && (r.header_time - at(50ms)) % 20ms == SimDuration::zero() &&
                    r.status == LinResultStatus::Ok && r.data[0] == kStatusValue,
                "unexpected " + describe(r));
    }
    require(!t.results.empty() && t.results.back().id == kExtraId, "schedule not switched");

    Cluster asleep;
    asleep.stop_at = 400ms;
    asleep.master_script = [](SimTime now, LinMaster& m) {
        if (now == at(100ms))
        {
            m.request_sleep();
        }
        if (now == at(150ms))
        {
            m.set_schedule({{kExtraId, 1, LinChecksumModel::Enhanced, 20ms, LinPublisher::Slave}});
        }
        if (now == at(200ms))
        {
            m.request_sleep(); // made while asleep: dropped
            m.request_wakeup();
        }
    };
    const auto u = run_in_both_orders(asleep);
    std::vector<long long> after;
    for (const auto& r : u.results)
    {
        if (r.header_time >= at(100ms))
        {
            require(r.id == kExtraId && r.status == LinResultStatus::Ok, "unexpected " + describe(r));
            after.push_back(ms_of(r.header_time));
        }
    }
    require(after.size() >= 3 && after[0] == 300 && after[1] == 320 && after[2] == 340,
            "new schedule did not start at the restart (300 ms)");
    require(!contains(u.master, "state t=300 sleep"), "a sleep request made while asleep survived the wakeup");
}

// ---------------------------------------------------------------------------------------------
// Ecu::lin_master() and Ecu::lin_slave() on a LocalBench
// ---------------------------------------------------------------------------------------------

TEST_CASE(lin_local_ecu_checks_names_roles_and_the_run)
{
    LocalBench bench;
    Ecu& a = bench.add_ecu("A", 5ms);
    Ecu& b = bench.add_ecu("B", 5ms);
    const LinScheduleTable schedule{{0x10, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master}};
    a.j1939("CAN1");

    CHECK(throws<std::invalid_argument>([&] { a.lin_master("", schedule); }));
    CHECK(throws<std::invalid_argument>([&] { a.lin_slave(""); }));
    // A CAN bus's name, of this ECU and of another one, in both directions.
    CHECK(throws_with<std::invalid_argument>([&] { a.lin_slave("CAN1"); }, "is a CAN bus"));
    CHECK(throws_with<std::invalid_argument>([&] { b.lin_master("CAN1", schedule); }, "is a CAN bus"));
    LinMaster& master = a.lin_master("LIN1", schedule);
    CHECK(throws_with<std::invalid_argument>([&] { a.j1939("LIN1"); }, "is a LIN bus"));
    CHECK(throws_with<std::invalid_argument>([&] { b.can("LIN1"); }, "is a LIN bus"));
    // One LIN node per bus and ECU; one master per bus (the message names both ECUs).
    CHECK(throws_with<std::invalid_argument>([&] { a.lin_slave("LIN1"); }, "one LIN node per LIN bus"));
    CHECK(throws_with<std::logic_error>([&] { b.lin_master("LIN1", schedule); }, "\"A\""));
    CHECK(throws_with<std::logic_error>([&] { b.lin_master("LIN1", schedule); }, "\"B\""));
    // Invalid schedules and options: nothing is attached, and no bus is made.
    CHECK(throws<std::invalid_argument>([&] { b.lin_master("LIN2", {}); }));
    CHECK(throws<std::invalid_argument>([&] { b.lin_master("LIN2", schedule, LinMasterOptions{-1ms}); }));
    CHECK(throws<std::invalid_argument>([&] { b.lin_slave("LIN2", LinSlaveOptions{{0x40}, std::nullopt}); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(bench.lin_bus("LIN2")); }));
    // After the failures, the same name works.
    static_cast<void>(b.lin_master("LIN2", schedule));
    static_cast<void>(a.lin_slave("LIN2", LinSlaveOptions{{0x10}, std::nullopt}));
    CHECK(throws<std::invalid_argument>([&] { b.lin_master("LIN1", {{0x10, 1, LinChecksumModel::Enhanced, 7ms}}); }));
    LinSlave& slave = b.lin_slave("LIN1", LinSlaveOptions{{0x10}, std::nullopt});
    const std::array<std::uint8_t, 1> one{0x42};
    master.set_frame_data(0x10, one);

    // Lookups.
    CHECK(&bench.lin_master("A", "LIN1") == &master);
    CHECK(&bench.lin_slave("B", "LIN1") == &slave);
    CHECK(throws_with<std::out_of_range>([&] { static_cast<void>(bench.lin_slave("A", "LIN1")); }, "\"LIN1\" (master)"));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(bench.lin_master("C", "LIN1")); }));
    CHECK(throws_with<std::out_of_range>([&] { static_cast<void>(bench.lin_bus("CAN1")); }, "is a CAN bus"));
    CHECK(throws_with<std::out_of_range>([&] { static_cast<void>(bench.bus("LIN1")); }, "is a LIN bus"));

    std::vector<std::string> frames;
    slave.on_frame([&](const LinReceivedFrame& f) { frames.push_back(std::to_string(ms_of(f.header_time)) + " " + hex(f.data.data(), f.data_length)); });
    bench.run_until(at(25ms));
    require(frames == std::vector<std::string>{"0 42", "10 42"}, "slave frames: " + std::to_string(frames.size()));

    // After the first run.
    CHECK(throws<std::logic_error>([&] { a.lin_master("LIN3", schedule); }));
    CHECK(throws<std::logic_error>([&] { b.lin_slave("LIN3"); }));
}

} // namespace
