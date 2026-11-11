// SPDX-License-Identifier: MIT
//
// LinMaster and LinSlave with real SIL Kit LIN and time synchronisation (in-process registry).
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/lin/lin_master.hpp"
#include "vehicle_sil/lin/lin_slave.hpp"
#include "vehicle_sil/lin_silkit/silkit_lin_port.hpp"

#include <silkit/services/lin/all.hpp>

#include <array>
#include <chrono>
#include <cstdio>
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
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::start_together;
using vehicle_sil::unit::throws;

constexpr LinId kSensorId = 0x10;
constexpr LinId kStatusId = 0x11;
constexpr LinId kUnansweredId = 0x12;

struct RunOptions
{
    SimDuration master_step = 5ms;
    SimDuration slave_step = 2ms;
    std::chrono::microseconds slave_busy{0};
    std::chrono::microseconds master_busy{0};
    SimDuration stop_at = 200ms;
    /// Slots of the sensor, status and unanswered entries.
    std::array<SimDuration, 3> slots{5ms, 5ms, 10ms};
};

struct RunResult
{
    std::vector<LinScheduleResult> results;
    LinMasterStatistics master;
    LinSlaveStatistics slave;
};

/// Sensor value the slave publishes at its step `t`: the time in units of 100 us.
std::array<std::uint8_t, 2> sensor_value(SimTime t)
{
    const auto v = static_cast<std::uint16_t>(t.time_since_epoch() / 100us);
    return {static_cast<std::uint8_t>(v & 0xFFu), static_cast<std::uint8_t>(v >> 8)};
}

RunResult run(const RunOptions& o)
{
    InProcessRegistry registry;

    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = {"LinMaster", "LinSensor"};
    controllerOptions.stop_at = o.stop_at;
    SimulationController controller(controllerOptions);

    auto make_runtime = [&](const std::string& name, SimDuration step) {
        return std::make_unique<ParticipantRuntime>(
            coordinated_options(name, registry.uri, VirtualTimeOptions{step, std::nullopt}));
    };

    RunResult out;
    std::mutex outMutex;

    // Master: sensor frame and status frame every 10 ms each, plus an identifier nobody answers.
    auto masterRuntime = make_runtime("LinMaster", o.master_step);
    auto* masterController = masterRuntime->participant().CreateLinController("LIN1", "LIN_VT");
    SilKitLinPort masterPort(*masterController, LinRole::Master);
    LinMaster master(masterPort,
                     {{kSensorId, 2, LinChecksumModel::Enhanced, o.slots[0]},
                      {kStatusId, 1, LinChecksumModel::Enhanced, o.slots[1]},
                      {kUnansweredId, 1, LinChecksumModel::Classic, o.slots[2]}},
                     o.master_step);
    master.on_result([&](const LinScheduleResult& r) {
        const std::lock_guard lock(outMutex);
        if (r.header_time < SimTime{} + o.stop_at - 2 * o.slots[2])
        {
            out.results.push_back(r);
        }
    });
    masterRuntime->set_communication_ready_handler([&] { master.init(); });
    masterRuntime->set_step_handler([&](SimTime now, SimDuration) {
        master.step(now);
        if (o.master_busy.count() > 0)
        {
            std::this_thread::sleep_for(o.master_busy);
        }
    });

    // Sensor slave: sets its value every step; the status frame only from 50 ms on.
    auto slaveRuntime = make_runtime("LinSensor", o.slave_step);
    auto* slaveController = slaveRuntime->participant().CreateLinController("LIN1", "LIN_VT");
    SilKitLinPort slavePort(*slaveController, LinRole::Slave);
    LinSlave slave(slavePort);
    slaveRuntime->set_communication_ready_handler([&] { slave.init(); });
    slaveRuntime->set_step_handler([&](SimTime now, SimDuration) {
        slave.step(now);
        if (o.slave_busy.count() > 0)
        {
            std::this_thread::sleep_for(o.slave_busy);
        }
        const auto value = sensor_value(now);
        slave.set_response(now, kSensorId, value);
        if (now >= SimTime{} + 50ms)
        {
            const std::array<std::uint8_t, 1> status{0xA5};
            slave.set_response(now, kStatusId, status);
        }
    });

    start_together(controller, {masterRuntime.get(), slaveRuntime.get()}, 30s);
    CHECK(controller.wait_until_finished(60s) == orch::ParticipantState::Shutdown);
    CHECK(masterRuntime->wait_until_finished(30s) == orch::ParticipantState::Shutdown);
    CHECK(slaveRuntime->wait_until_finished(30s) == orch::ParticipantState::Shutdown);
    CHECK(masterRuntime->step_error().empty());
    CHECK(slaveRuntime->step_error().empty());
    out.master = master.statistics();
    out.slave = slave.statistics();
    return out;
}

bool same(const LinScheduleResult& a, const LinScheduleResult& b)
{
    return a.header_time == b.header_time && a.id == b.id && a.status == b.status && a.data_length == b.data_length &&
           a.data == b.data;
}

/// Newest slave step strictly before t on the slave's step grid.
SimTime newest_step_before(SimTime t, SimDuration step)
{
    const auto n = (t.time_since_epoch() - SimDuration(1)) / step;
    return SimTime{} + step * n;
}

void check_results(const RunResult& r, const RunOptions& o)
{
    // Cyclic schedule, e.g. slots 5 + 5 + 10 ms: headers at 0, 5, 10, 20, 25, 30, ...
    CHECK(r.results.size() >= 10u);
    SimTime expectedTime{};
    const std::array<LinId, 3> ids{kSensorId, kStatusId, kUnansweredId};
    const auto& slots = o.slots;
    for (std::size_t i = 0; i < r.results.size(); ++i)
    {
        const auto& result = r.results[i];
        CHECK(result.header_time == expectedTime);
        CHECK(result.id == ids[i % 3]);
        expectedTime += slots[i % 3];

        if (result.id == kUnansweredId)
        {
            CHECK(result.status == LinResultStatus::NoResponse);
            continue;
        }
        if (result.id == kStatusId)
        {
            // The status value exists from the slave's step at 50 ms on, i.e. for headers after 50 ms.
            if (result.header_time > SimTime{} + 50ms)
            {
                CHECK(result.status == LinResultStatus::Ok);
                CHECK_EQ(result.data[0], 0xA5);
            }
            else
            {
                CHECK(result.status == LinResultStatus::NoResponse);
            }
            continue;
        }
        if (result.header_time == SimTime{})
        {
            CHECK(result.status == LinResultStatus::NoResponse); // no value set before T = 0
            continue;
        }
        if (result.status != LinResultStatus::Ok)
        {
            throw vehicle_sil::unit::Failure("sensor header at " + std::to_string(to_ns(result.header_time)) +
                                             " ns: status " + std::to_string(static_cast<int>(result.status)));
        }
        CHECK_EQ(result.data_length, 2u);
        const auto expected = sensor_value(newest_step_before(result.header_time, o.slave_step));
        if (result.data[0] != expected[0] || result.data[1] != expected[1])
        {
            throw vehicle_sil::unit::Failure("header at " + std::to_string(to_ns(result.header_time)) +
                                             " ns got sensor value " + std::to_string(result.data[0] | (result.data[1] << 8)) +
                                             ", expected " + std::to_string(expected[0] | (expected[1] << 8)));
        }
    }
}

} // namespace

TEST_CASE(lin_schedule_responses_follow_virtual_time)
{
    RunOptions o;
    const auto r = run(o);
    check_results(r, o);
    std::printf("%zu results, master: headers=%llu ok=%llu no_response=%llu errors=%llu, slave: sent=%llu ignored=%llu\n",
                r.results.size(), static_cast<unsigned long long>(r.master.headers_sent),
                static_cast<unsigned long long>(r.master.responses_ok),
                static_cast<unsigned long long>(r.master.no_responses), static_cast<unsigned long long>(r.master.errors),
                static_cast<unsigned long long>(r.slave.responses_sent),
                static_cast<unsigned long long>(r.slave.headers_ignored));
    CHECK_EQ(r.master.errors, 0u);
}

TEST_CASE(lin_schedule_is_independent_of_wall_clock_speed)
{
    RunOptions fast;
    RunOptions slowSlave;
    slowSlave.slave_busy = 1500us;
    RunOptions slowMaster;
    slowMaster.master_busy = 4ms;
    const auto a = run(fast);
    const auto b = run(slowSlave);
    const auto c = run(slowMaster);
    check_results(a, fast);
    CHECK_EQ(a.results.size(), b.results.size());
    CHECK_EQ(a.results.size(), c.results.size());
    for (std::size_t i = 0; i < a.results.size(); ++i)
    {
        CHECK(same(a.results[i], b.results[i]));
        CHECK(same(a.results[i], c.results[i]));
    }
}

TEST_CASE(lin_schedule_with_equal_steps_on_coinciding_times)
{
    // Master and slave both step every 5 ms and headers go out every 10 ms: every header
    // coincides with a slave step, the case in which a static TX buffer would be
    // non-deterministic. The slave's step (5 ms) is below the slot (10 ms).
    RunOptions o;
    o.slave_step = 5ms;
    o.slots = {10ms, 10ms, 10ms};
    const auto r = run(o);
    check_results(r, o);
}

TEST_CASE(lin_schedule_rejects_invalid_configuration)
{
    InProcessRegistry registry;
    ParticipantRuntimeOptions options;
    options.participant_name = "Invalid";
    options.registry_uri = registry.uri;
    ParticipantRuntime runtime(options);
    auto* controller = runtime.participant().CreateLinController("LIN1", "LIN_X");

    SilKitLinPort masterPort(*controller, LinRole::Master);
    auto rejects = [&](std::vector<LinScheduleEntry> schedule, SimDuration step) {
        return throws<std::invalid_argument>([&] { LinMaster master(masterPort, std::move(schedule), step); });
    };
    CHECK(rejects({}, 5ms));
    CHECK(rejects({{0x10, 2, LinChecksumModel::Enhanced, 7ms}}, 5ms)); // slot off the step grid
    CHECK(rejects({{0x40, 2, LinChecksumModel::Enhanced, 5ms}}, 5ms)); // id > 63
    CHECK(rejects({{0x10, 9, LinChecksumModel::Enhanced, 5ms}}, 5ms)); // > 8 bytes
    CHECK(rejects({{0x10, 2, LinChecksumModel::Enhanced, 5ms}}, 0ms));

    auto* slaveController = runtime.participant().CreateLinController("LIN2", "LIN_X");
    SilKitLinPort slavePort(*slaveController, LinRole::Slave);
    LinSlave slave(slavePort);
    const std::array<std::uint8_t, 9> tooLong{};
    CHECK(throws<std::invalid_argument>([&] { slave.set_response(SimTime{}, 0x10, tooLong); }));
    const std::array<std::uint8_t, 1> one{1};
    slave.set_response(SimTime{} + 10ms, 0x10, one);
    CHECK(throws<std::logic_error>([&] { slave.set_response(SimTime{} + 5ms, 0x10, one); }));
}
