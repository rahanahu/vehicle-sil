// SPDX-License-Identifier: MIT
//
// A LIN cluster (one master, two slaves) with real SIL Kit LIN and time synchronisation
// (in-process registry): master frames, slave reception, sleep, wakeup by master and slave, bus
// idle timeout, schedule switch, and independence of wall-clock speed.
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/lin/lin_master.hpp"
#include "vehicle_sil/lin/lin_slave.hpp"
#include "vehicle_sil/lin_silkit/silkit_lin_port.hpp"

#include <silkit/services/lin/all.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::lin;
namespace orch = SilKit::Services::Orchestration;
using vehicle_sil::unit::coordinated_options;
using vehicle_sil::unit::Failure;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::start_together;

constexpr LinId kCommandId = 0x10; // published by the master
constexpr LinId kEchoId = 0x20;    // slave 0 echoes the newest command it received
constexpr LinId kStatusId = 0x21;  // slave 1 reports a constant
constexpr LinId kExtraId = 0x22;   // slave 1, only in the second schedule
constexpr std::uint8_t kStatusValue = 0x5A;
constexpr std::size_t kSlaves = 2;

LinScheduleTable default_schedule()
{
    return {{kCommandId, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master},
            {kEchoId, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave},
            {kStatusId, 1, LinChecksumModel::Classic, 10ms, LinPublisher::Slave}};
}

/// The command the master publishes from its step at `t`: the time in ms, little endian.
std::array<std::uint8_t, 2> command_at(SimTime t)
{
    const auto v = static_cast<std::uint16_t>(t.time_since_epoch() / 1ms);
    return {static_cast<std::uint8_t>(v & 0xFFu), static_cast<std::uint8_t>(v >> 8)};
}

SimTime at(std::chrono::milliseconds ms)
{
    return SimTime{} + ms;
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

long long ms_of(SimTime t)
{
    return static_cast<long long>(t.time_since_epoch() / 1ms);
}

struct ClusterOptions
{
    SimDuration master_step = 5ms;
    SimDuration slave_step = 2ms;
    SimDuration stop_at = 600ms;
    std::chrono::microseconds master_busy{0};
    std::chrono::microseconds slave_busy{0};
    LinMasterOptions master;
    std::array<LinSlaveOptions, kSlaves> slaves{
        LinSlaveOptions{{kCommandId}, std::nullopt},
        LinSlaveOptions{{kCommandId}, std::nullopt},
    };
    /// Called in the master's step before master.step(now); return false to skip step(now).
    std::function<bool(SimTime, LinMaster&)> master_script;
    /// Called in a slave's step before slave.step(now).
    std::function<void(std::size_t, SimTime, LinSlave&)> slave_script;
    /// Called in a slave's step after it set its responses.
    std::function<void(std::size_t, SimTime, LinSlave&)> slave_after_step;
};

/// Everything the nodes observed, as text records that depend only on simulation time and data.
struct ClusterTrace
{
    std::vector<std::string> master;
    std::array<std::vector<std::string>, kSlaves> slaves;
    std::vector<LinScheduleResult> results;
    LinMasterStatistics master_stats;
    std::array<LinSlaveStatistics, kSlaves> slave_stats;
};

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

ClusterTrace run_cluster(ClusterOptions o)
{
    InProcessRegistry registry;
    const std::array<std::string, kSlaves> slaveNames{"Slave0", "Slave1"};

    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = {"Master", slaveNames[0], slaveNames[1]};
    controllerOptions.stop_at = o.stop_at;
    SimulationController controller(controllerOptions);

    auto make_runtime = [&](const std::string& name, SimDuration step) {
        return std::make_unique<ParticipantRuntime>(
            coordinated_options(name, registry.uri, VirtualTimeOptions{step, std::nullopt}));
    };

    ClusterTrace out;
    // Results reported at the end of a slot that ends at or after stop_at are not reported, and
    // steps run until stop_at: keep only records well before it.
    const SimTime keepBefore = SimTime{} + o.stop_at - 20ms;

    auto masterRuntime = make_runtime("Master", o.master_step);
    SilKitLinPort masterPort(*masterRuntime->participant().CreateLinController("LIN1", "LIN_CLUSTER"), LinRole::Master);
    LinMaster master(masterPort, default_schedule(), o.master_step, o.master);
    master.on_result([&](const LinScheduleResult& r) {
            if (r.header_time < keepBefore)
            {
                out.results.push_back(r);
                out.master.push_back("result t=" + std::to_string(ms_of(r.header_time)) + " id=" + std::to_string(r.id) +
                                     " status=" + status_name(r.status) + " data=" + hex(r.data.data(), r.data_length));
            }
        });
    master.on_state([&](SimTime t, LinNodeState s) {
            if (t < keepBefore)
            {
                out.master.push_back("state t=" + std::to_string(ms_of(t)) + " " + to_string(s));
            }
        });
    masterRuntime->set_communication_ready_handler([&] { master.init(); });
    masterRuntime->set_step_handler([&](SimTime now, SimDuration) {
        master.set_frame_data(kCommandId, command_at(now));
        if (!o.master_script || o.master_script(now, master))
        {
            master.step(now);
        }
        if (o.master_busy.count() > 0)
        {
            vehicle_sil::unit::busy_for(o.master_busy);
        }
    });

    std::vector<std::unique_ptr<ParticipantRuntime>> slaveRuntimes;
    std::vector<std::unique_ptr<SilKitLinPort>> slavePorts;
    std::vector<std::unique_ptr<LinSlave>> slaves;
    // Newest command each slave received; slave 0 echoes it.
    std::array<std::array<std::uint8_t, 2>, kSlaves> lastCommand{};
    // The step each slave is in: frames are traced with the step that handled them.
    std::array<SimTime, kSlaves> slaveNow{};
    for (std::size_t i = 0; i < kSlaves; ++i)
    {
        slaveRuntimes.push_back(make_runtime(slaveNames[i], o.slave_step));
        auto& runtime = *slaveRuntimes.back();
        auto& trace = out.slaves[i];
        slavePorts.push_back(
            std::make_unique<SilKitLinPort>(*runtime.participant().CreateLinController("LIN1", "LIN_CLUSTER"), LinRole::Slave));
        slaves.push_back(std::make_unique<LinSlave>(*slavePorts.back(), o.slaves[i]));
        slaves.back()->on_frame([&, i](const LinReceivedFrame& f) {
                if (f.header_time < keepBefore)
                {
                    trace.push_back("frame t=" + std::to_string(ms_of(f.header_time)) +
                                    " rx=" + std::to_string(ms_of(slaveNow[i])) + " id=" + std::to_string(f.id) +
                                    " data=" + hex(f.data.data(), f.data_length));
                }
                if (f.id == kCommandId && f.data_length == 2)
                {
                    lastCommand[i] = {f.data[0], f.data[1]};
                }
            });
        slaves.back()->on_state([&](SimTime t, LinNodeState s) {
                if (t < keepBefore)
                {
                    trace.push_back("state t=" + std::to_string(ms_of(t)) + " " + to_string(s));
                }
            });
        auto& slave = *slaves.back();
        runtime.set_communication_ready_handler([&slave] { slave.init(); });
        runtime.set_step_handler([&, i](SimTime now, SimDuration) {
            if (o.slave_busy.count() > 0)
            {
                vehicle_sil::unit::busy_for(o.slave_busy);
            }
            if (o.slave_script)
            {
                o.slave_script(i, now, slave);
            }
            slaveNow[i] = now;
            slave.step(now);
            if (i == 0)
            {
                slave.set_response(now, kEchoId, lastCommand[0]);
            }
            else
            {
                const std::array<std::uint8_t, 1> status{kStatusValue};
                slave.set_response(now, kStatusId, status, LinChecksumModel::Classic);
                slave.set_response(now, kExtraId, status);
            }
            if (o.slave_after_step)
            {
                o.slave_after_step(i, now, slave);
            }
        });
    }

    // On a failure path (a CHECK throws) stop the participants before the objects their handlers
    // use are destroyed. Declared last: destroyed first.
    struct StopAll
    {
        std::vector<ParticipantRuntime*> runtimes;
        ~StopAll()
        {
            for (auto* runtime : runtimes)
            {
                runtime->stop("test finished");
            }
        }
    } stopAll{{masterRuntime.get(), slaveRuntimes[0].get(), slaveRuntimes[1].get()}};

    start_together(controller, {masterRuntime.get(), slaveRuntimes[0].get(), slaveRuntimes[1].get()}, 30s);
    CHECK(controller.wait_until_finished(60s) == orch::ParticipantState::Shutdown);
    for (auto* runtime : {masterRuntime.get(), slaveRuntimes[0].get(), slaveRuntimes[1].get()})
    {
        CHECK(runtime->wait_until_finished(30s) == orch::ParticipantState::Shutdown);
        CHECK(runtime->step_error().empty());
    }
    out.master_stats = master.statistics();
    CHECK_EQ(out.master_stats.inbox_overflows, 0u);
    for (std::size_t i = 0; i < kSlaves; ++i)
    {
        out.slave_stats[i] = slaves[i]->statistics();
        CHECK_EQ(out.slave_stats[i].inbox_overflows, 0u);
    }
    return out;
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

/// Checks one result of the default schedule while the cluster is awake.
void check_default_result(const LinScheduleResult& r)
{
    switch (r.id)
    {
    case kCommandId:
    {
        // The master's own frame carries what it set in the same step.
        require(r.publisher == LinPublisher::Master && r.status == LinResultStatus::Ok,
                "command at " + std::to_string(ms_of(r.header_time)) + " ms not sent");
        const auto expected = command_at(r.header_time);
        require(r.data_length == 2 && r.data[0] == expected[0] && r.data[1] == expected[1],
                "command at " + std::to_string(ms_of(r.header_time)) + " ms carried the wrong data");
        break;
    }
    case kEchoId:
        // Slave 0 received the command of the previous slot (10 ms earlier) at its first step
        // after it and echoes it from then on.
        if (r.header_time > SimTime{} + 10ms)
        {
            const auto expected = command_at(r.header_time - 10ms);
            require(r.status == LinResultStatus::Ok && r.data[0] == expected[0] && r.data[1] == expected[1],
                    "echo at " + std::to_string(ms_of(r.header_time)) + " ms: " + status_name(r.status) + " " +
                        hex(r.data.data(), r.data_length));
        }
        break;
    case kStatusId:
        require(r.status == LinResultStatus::Ok && r.data_length == 1 && r.data[0] == kStatusValue,
                "status at " + std::to_string(ms_of(r.header_time)) + " ms: " + status_name(r.status));
        break;
    default:
        break;
    }
}

TEST_CASE(lin_master_frames_reach_the_slaves_and_responses_reach_the_master)
{
    ClusterOptions o;
    o.stop_at = 300ms;
    const auto t = run_cluster(o);

    // Headers every 10 ms, command / echo / status in turn.
    require(t.results.size() >= 25u, std::to_string(t.results.size()) + " results");
    for (std::size_t i = 0; i < t.results.size(); ++i)
    {
        const auto& r = t.results[i];
        require(r.header_time == SimTime{} + 10ms * static_cast<int>(i), "schedule timing broken at " + std::to_string(i));
        require(r.id == std::array<LinId, 3>{kCommandId, kEchoId, kStatusId}[i % 3], "schedule order broken");
        check_default_result(r);
    }
    // Both slaves received every command, stamped with its header time.
    for (std::size_t s = 0; s < kSlaves; ++s)
    {
        for (std::size_t i = 0; i < t.results.size(); i += 3)
        {
            const auto h = t.results[i].header_time;
            const auto c = command_at(h);
            // Handled at the slave's first step after the header (2 ms steps).
            require(contains(t.slaves[s], "frame t=" + std::to_string(ms_of(h)) + " rx=" + std::to_string(ms_of(h) + 2) +
                                              " id=16 data=" + hex(c.data(), 2)),
                    "slave " + std::to_string(s) + " missed the command at " + std::to_string(ms_of(h)) + " ms");
        }
        // Subscribed to the command only: no other frames, and no state changes.
        for (const auto& line : t.slaves[s])
        {
            require(line.rfind("frame", 0) == 0 && line.find(" id=16 ") != std::string::npos, "unexpected record " + line);
        }
    }
    CHECK_EQ(t.master_stats.errors, 0u);
}

TEST_CASE(lin_master_sleeps_and_wakes_the_cluster)
{
    ClusterOptions o;
    o.master_script = [](SimTime now, LinMaster& m) {
        if (now == at(100ms))
        {
            m.request_sleep();
        }
        if (now == at(300ms))
        {
            m.request_wakeup();
        }
        return true;
    };
    const auto t = run_cluster(o);

    require(contains(t.master, "state t=100 sleep"), "master did not sleep at 100 ms");
    require(contains(t.master, "state t=300 operational"), "master did not wake at 300 ms");
    for (std::size_t s = 0; s < kSlaves; ++s)
    {
        require(contains(t.slaves[s], "state t=100 sleep"), "slave " + std::to_string(s) + " did not sleep at 100 ms");
        require(contains(t.slaves[s], "state t=300 operational"), "slave " + std::to_string(s) + " did not wake at 300 ms");
    }
    // The slot before the command ended at 100 ms; nothing until the restart at 300 + 100 ms,
    // which begins with the first entry.
    SimTime previous{};
    bool restarted = false;
    for (const auto& r : t.results)
    {
        require(r.header_time < at(100ms) || r.header_time >= at(400ms),
                "header at " + std::to_string(ms_of(r.header_time)) + " ms while asleep");
        if (r.header_time >= at(400ms) && !restarted)
        {
            restarted = true;
            require(r.header_time == at(400ms) && r.id == kCommandId, "schedule did not restart at its first entry at 400 ms");
        }
        else if (r.header_time >= at(400ms))
        {
            require(r.header_time == previous + 10ms, "schedule timing broken after the restart");
        }
        previous = r.header_time;
        check_default_result(r);
    }
    require(restarted, "schedule did not restart");
    CHECK_EQ(t.master_stats.sleep_commands, 1u);
    CHECK_EQ(t.master_stats.wakeups_by_master, 1u);
    CHECK_EQ(t.master_stats.wakeups_by_slaves, 0u);
}

TEST_CASE(lin_slave_wakes_the_cluster)
{
    ClusterOptions o;
    o.master_script = [](SimTime now, LinMaster& m) {
        if (now == at(100ms))
        {
            m.request_sleep();
        }
        return true;
    };
    o.slave_script = [](std::size_t i, SimTime now, LinSlave& s) {
        if (i == 1 && now == at(202ms))
        {
            s.request_wakeup();
        }
    };
    const auto t = run_cluster(o);

    // Slave 1 sends the pulse at 202 ms; the master wakes at its first step after it (205 ms)
    // and restarts the schedule at the first step at least 100 ms after the pulse (305 ms).
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

TEST_CASE(lin_slave_sleeps_on_bus_idle_and_wakes_on_a_header)
{
    ClusterOptions o;
    o.stop_at = 300ms;
    o.slaves[0].bus_idle_timeout = 50ms;
    o.slaves[1].bus_idle_timeout = 50ms;
    // The master stays silent until 150 ms.
    o.master_script = [](SimTime now, LinMaster&) { return now >= at(150ms); };
    const auto t = run_cluster(o);

    for (std::size_t s = 0; s < kSlaves; ++s)
    {
        require(contains(t.slaves[s], "state t=50 sleep"), "slave " + std::to_string(s) + " did not sleep at 50 ms");
        require(contains(t.slaves[s], "state t=150 operational"), "slave " + std::to_string(s) + " did not wake at 150 ms");
        // The header woke the sleeping controller before the master's frame arrived.
        const auto c = command_at(at(150ms));
        require(contains(t.slaves[s], "frame t=150 rx=152 id=16 data=" + hex(c.data(), 2)),
                "slave " + std::to_string(s) + " missed the command right after its idle sleep");
    }
    // The header at 170 ms finds slave 1 awake (it answers headers in any case).
    const auto status = std::find_if(t.results.begin(), t.results.end(),
                                     [](const LinScheduleResult& r) { return r.id == kStatusId; });
    require(status != t.results.end() && status->header_time == at(170ms) && status->status == LinResultStatus::Ok,
            "status after the idle sleep not answered");
}

TEST_CASE(lin_master_switches_the_schedule_at_a_slot_boundary)
{
    ClusterOptions o;
    o.stop_at = 300ms;
    o.master_script = [](SimTime now, LinMaster& m) {
        if (now == at(50ms)) // a slot boundary
        {
            m.set_schedule({{kExtraId, 1, LinChecksumModel::Enhanced, 20ms, LinPublisher::Slave}});
        }
        return true;
    };
    const auto t = run_cluster(o);
    for (const auto& r : t.results)
    {
        if (r.header_time < at(50ms))
        {
            check_default_result(r);
            continue;
        }
        require(r.id == kExtraId && (r.header_time - at(50ms)) % 20ms == SimDuration::zero(),
                "unexpected header " + std::to_string(r.id) + " at " + std::to_string(ms_of(r.header_time)) + " ms");
        require(r.status == LinResultStatus::Ok && r.data[0] == kStatusValue, "extra frame not answered");
    }
    require(!t.results.empty() && t.results.back().id == kExtraId, "schedule not switched");
}

TEST_CASE(lin_cluster_is_independent_of_wall_clock_speed)
{
    // The master steps once per slot (10 ms), the tightest case: a frame the master publishes
    // at T must reach the slaves before their steps after T, although SIL Kit generates it only
    // when it delivers the header back to the master. Slave 0 echoes it in the next slot.
    auto scenario = [](std::chrono::microseconds master_busy, std::chrono::microseconds slave_busy) {
        ClusterOptions o;
        o.master_step = 10ms;
        o.stop_at = 500ms;
        o.master_busy = master_busy;
        o.slave_busy = slave_busy;
        o.slaves[0].bus_idle_timeout = 60ms;
        o.master_script = [](SimTime now, LinMaster& m) {
            if (now == at(100ms) || now == at(400ms))
            {
                m.request_sleep();
            }
            if (now == at(250ms))
            {
                m.request_wakeup();
            }
            return true;
        };
        o.slave_script = [](std::size_t i, SimTime now, LinSlave& s) {
            if (i == 0 && now == at(450ms))
            {
                s.request_wakeup();
            }
        };
        return run_cluster(o);
    };
    const auto fast = scenario(0us, 0us);
    const auto slowSlaves = scenario(0us, 1500us);
    const auto slowMaster = scenario(4ms, 0us);
    require(contains(fast.master, "state t=460 operational"), "master did not wake on slave 0's pulse");
    // Awake from 0 to 100 ms and from the restart at 350 ms to the sleep at 400 ms; the restart
    // after the pulse at 450 ms would be at 550 ms.
    require(fast.results.size() == 15u, std::to_string(fast.results.size()) + " results, expected 15");
    for (const auto& r : fast.results)
    {
        check_default_result(r);
    }
    for (const auto* other : {&slowSlaves, &slowMaster})
    {
        require(other->master == fast.master, "master trace depends on wall-clock speed");
        for (std::size_t s = 0; s < kSlaves; ++s)
        {
            require(other->slaves[s] == fast.slaves[s], "slave " + std::to_string(s) + " trace depends on wall-clock speed");
        }
    }
}

TEST_CASE(lin_go_to_sleep_wins_over_a_wakeup_pulse_at_the_same_time)
{
    // The master wakes at 200 ms and restarts the schedule at 300 ms; a sleep requested at 250 ms
    // makes it send the go-to-sleep command at 300 ms, without a header. Both slaves sleep from
    // 260 ms (60 ms of silence after the pulse at 200 ms), and slave 1 sends a pulse at 300 ms.
    // Everyone must end asleep, at every speed: the command wins over the pulse of the same time.
    auto scenario = [](std::chrono::microseconds master_busy, std::chrono::microseconds slave_busy) {
        ClusterOptions o;
        o.stop_at = 400ms;
        o.master_busy = master_busy;
        o.slave_busy = slave_busy;
        o.slaves[0].bus_idle_timeout = 60ms;
        o.slaves[1].bus_idle_timeout = 60ms;
        o.master_script = [](SimTime now, LinMaster& m) {
            if (now == at(100ms) || now == at(250ms))
            {
                m.request_sleep();
            }
            if (now == at(200ms))
            {
                m.request_wakeup();
            }
            return true;
        };
        o.slave_script = [](std::size_t i, SimTime now, LinSlave& s) {
            if (i == 1 && now == at(300ms))
            {
                s.request_wakeup();
            }
        };
        return run_cluster(o);
    };
    const auto fast = scenario(0us, 0us);
    const std::vector<std::string> master{"state t=100 sleep", "state t=200 operational", "state t=300 sleep"};
    std::vector<std::string> masterStates;
    for (const auto& line : fast.master)
    {
        if (line.rfind("state", 0) == 0)
        {
            masterStates.push_back(line);
        }
    }
    require(masterStates == master, "master states differ");
    const std::vector<std::string> slave{"state t=100 sleep",       "state t=200 operational", "state t=260 sleep",
                                         "state t=300 operational", "state t=300 sleep"};
    for (std::size_t s = 0; s < kSlaves; ++s)
    {
        std::vector<std::string> states;
        for (const auto& line : fast.slaves[s])
        {
            if (line.rfind("state", 0) == 0)
            {
                states.push_back(line);
            }
        }
        require(states == slave, "slave " + std::to_string(s) + " states differ");
    }
    CHECK_EQ(fast.master_stats.wakeups_by_slaves, 0u);
    CHECK_EQ(fast.slave_stats[1].wakeups_sent, 1u);
    for (const auto& other : {scenario(0us, 1500us), scenario(4ms, 0us)})
    {
        require(other.master == fast.master, "master trace depends on wall-clock speed");
        for (std::size_t s = 0; s < kSlaves; ++s)
        {
            require(other.slaves[s] == fast.slaves[s], "slave " + std::to_string(s) + " trace depends on wall-clock speed");
        }
    }
}

TEST_CASE(lin_master_reports_errors_and_cleared_responses)
{
    constexpr LinId kShortId = 0x23; // 2 bytes in the schedule, slave 1 answers with 1
    ClusterOptions o;
    o.stop_at = 300ms;
    o.master_script = [](SimTime now, LinMaster& m) {
        if (now == SimTime{})
        {
            m.set_schedule({{kStatusId, 1, LinChecksumModel::Classic, 10ms, LinPublisher::Slave},
                            {kShortId, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave}});
        }
        return true;
    };
    o.slave_after_step = [](std::size_t i, SimTime now, LinSlave& s) {
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
    const auto t = run_cluster(o);
    require(t.results.size() >= 20u, std::to_string(t.results.size()) + " results");
    for (const auto& r : t.results)
    {
        if (r.id == kShortId)
        {
            require(r.status == LinResultStatus::Error,
                    "short response at " + std::to_string(ms_of(r.header_time)) + " ms not reported as error");
            continue;
        }
        require(r.id == kStatusId, "unexpected id " + std::to_string(r.id));
        const bool answered = r.header_time > SimTime{} && r.header_time <= at(100ms);
        require(r.status == (answered ? LinResultStatus::Ok : LinResultStatus::NoResponse),
                "status at " + std::to_string(ms_of(r.header_time)) + " ms: " + status_name(r.status));
    }
    require(t.master_stats.errors > 0, "no errors counted");
}

TEST_CASE(lin_master_switches_the_schedule_while_asleep)
{
    ClusterOptions o;
    o.stop_at = 400ms;
    o.master_script = [](SimTime now, LinMaster& m) {
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
            m.request_sleep(); // made while asleep: dropped, not carried over to the restart
            m.request_wakeup();
        }
        return true;
    };
    const auto t = run_cluster(o);
    std::vector<long long> after;
    for (const auto& r : t.results)
    {
        if (r.header_time >= at(100ms))
        {
            require(r.id == kExtraId && r.status == LinResultStatus::Ok, "unexpected result after the switch");
            after.push_back(ms_of(r.header_time));
        }
    }
    require(after.size() >= 3 && after[0] == 300 && after[1] == 320 && after[2] == 340,
            "new schedule did not start at the restart (300 ms)");
    require(!contains(t.master, "state t=300 sleep"), "a sleep request made while asleep survived the wakeup");
}

TEST_CASE(lin_cluster_rejects_invalid_configuration)
{
    InProcessRegistry registry;
    ParticipantRuntimeOptions options;
    options.participant_name = "Invalid";
    options.registry_uri = registry.uri;
    ParticipantRuntime runtime(options);
    auto* controller = runtime.participant().CreateLinController("LIN1", "LIN_X");
    using vehicle_sil::unit::throws;

    // An identifier must keep its length, checksum and publisher throughout a table.
    CHECK(throws<std::invalid_argument>([&] {
        validate_schedule({{0x10, 2, LinChecksumModel::Enhanced, 5ms}, {0x10, 3, LinChecksumModel::Enhanced, 5ms}}, 5ms);
    }));
    CHECK(throws<std::invalid_argument>([&] {
        validate_schedule({{0x10, 2, LinChecksumModel::Enhanced, 5ms},
                           {0x10, 2, LinChecksumModel::Enhanced, 5ms, LinPublisher::Master}},
                          5ms);
    }));
    SilKitLinPort masterPort(*controller, LinRole::Master);
    LinMasterOptions negativeDelay{-1ms};
    CHECK(throws<std::invalid_argument>([&] { LinMaster m(masterPort, default_schedule(), 5ms, negativeDelay); }));
    CHECK(throws<std::invalid_argument>([&] { LinMaster m(masterPort, default_schedule(), 0ms); }));

    LinMaster master(masterPort, default_schedule(), 5ms);
    const std::array<std::uint8_t, 3> three{};
    CHECK(throws<std::invalid_argument>([&] { master.set_frame_data(kCommandId, three); })); // 2 bytes in the table
    CHECK(throws<std::invalid_argument>([&] { master.set_frame_data(0x40, three); }));
    CHECK(throws<std::invalid_argument>([&] { master.set_frame_data(0x30, three); })); // not in a schedule
    CHECK(throws<std::invalid_argument>([&] { master.set_frame_data(kEchoId, three); })); // a slave frame
    CHECK(throws<std::invalid_argument>([&] { master.set_schedule({}); }));
    // SIL Kit cannot reconfigure an identifier: a later schedule must keep its configuration.
    CHECK(throws<std::invalid_argument>(
        [&] { master.set_schedule({{kEchoId, 2, LinChecksumModel::Enhanced, 5ms, LinPublisher::Master}}); }));
    CHECK(throws<std::invalid_argument>([&] { master.set_schedule({{kStatusId, 1, LinChecksumModel::Enhanced, 5ms}}); }));
    // A new master frame becomes settable once a schedule contains it.
    master.set_schedule({{0x30, 3, LinChecksumModel::Enhanced, 5ms, LinPublisher::Master}});
    master.set_frame_data(0x30, three);

    auto* slaveController = runtime.participant().CreateLinController("LIN2", "LIN_X");
    SilKitLinPort slavePort(*slaveController, LinRole::Slave);
    CHECK(throws<std::invalid_argument>([&] { LinSlave s(slavePort, LinSlaveOptions{{0x40}, std::nullopt}); }));
    CHECK(throws<std::invalid_argument>([&] { LinSlave s(slavePort, LinSlaveOptions{{}, 0ms}); }));
    LinSlave slave(slavePort);
    CHECK(throws<std::invalid_argument>([&] { slave.clear_response(SimTime{}, 0x40); }));

    // The checks of the constructors, without a node (Peer runs them before it makes the
    // controller).
    LinMaster::validate(default_schedule(), 5ms);
    CHECK(throws<std::invalid_argument>([&] { LinMaster::validate({}, 5ms); }));
    CHECK(throws<std::invalid_argument>([&] { LinMaster::validate(default_schedule(), 0ms); }));
    CHECK(throws<std::invalid_argument>([&] { LinMaster::validate(default_schedule(), 5ms, negativeDelay); }));
    CHECK(throws<std::invalid_argument>([&] {
        LinMaster::validate({{0x10, 2, LinChecksumModel::Enhanced, 5ms}, {0x10, 3, LinChecksumModel::Enhanced, 5ms}}, 5ms);
    }));
    LinSlave::validate(LinSlaveOptions{{0x10}, std::nullopt});
    CHECK(throws<std::invalid_argument>([] { LinSlave::validate(LinSlaveOptions{{0x40}, std::nullopt}); }));
    CHECK(throws<std::invalid_argument>([] { LinSlave::validate(LinSlaveOptions{{}, 0ms}); }));

    // The SIL Kit port: a function of the other role, and the master's headers and frames with
    // an id above 63 or a length outside 1..8.
    using vehicle_sil::unit::throws_with;
    CHECK(throws_with<std::logic_error>([&] { slavePort.send_header(SimTime{}, 0x10, 1, LinChecksumModel::Enhanced); },
                                        "send_header() is a function of a master port"));
    CHECK(throws<std::logic_error>([&] { slavePort.send_frame(SimTime{}, LinFrame{0x10, LinChecksumModel::Enhanced, 1, {}}); }));
    CHECK(throws<std::logic_error>([&] { slavePort.send_go_to_sleep(SimTime{}); }));
    CHECK(throws_with<std::logic_error>([&] { masterPort.respond(SimTime{}, LinFrame{0x10, LinChecksumModel::Enhanced, 1, {}}); },
                                        "respond() is a function of a slave port"));
    CHECK(throws_with<std::invalid_argument>([&] { masterPort.send_header(SimTime{}, 64, 1, LinChecksumModel::Enhanced); },
                                             "id 64"));
    CHECK(throws_with<std::invalid_argument>([&] { masterPort.send_header(SimTime{}, 0x10, 0, LinChecksumModel::Enhanced); },
                                             "0 bytes"));
    CHECK(throws_with<std::invalid_argument>([&] { masterPort.send_header(SimTime{}, 0x10, 9, LinChecksumModel::Enhanced); },
                                             "9 bytes"));
    CHECK(throws<std::invalid_argument>([&] { masterPort.send_frame(SimTime{}, LinFrame{64, LinChecksumModel::Enhanced, 1, {}}); }));
    CHECK(throws<std::invalid_argument>([&] { masterPort.send_frame(SimTime{}, LinFrame{0x10, LinChecksumModel::Enhanced, 9, {}}); }));
}

} // namespace
