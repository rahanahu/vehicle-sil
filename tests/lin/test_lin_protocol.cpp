// SPDX-License-Identifier: MIT
//
// LIN protocol behaviour at its edges, with real SIL Kit LIN and time synchronisation
// (in-process registry): schedule timing at the identifier and length limits, checksum models,
// colliding responses, diagnostic frames (master request 0x3C, slave response 0x3D) including a
// multi-frame exchange, sleep and wakeup corner cases, the bus idle timeout and a full slave
// inbox.
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
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::test::scaled_timeout;
using namespace vehicle_sil;
using namespace vehicle_sil::lin;
namespace orch = SilKit::Services::Orchestration;
using vehicle_sil::unit::coordinated_options;
using vehicle_sil::unit::Failure;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::start_together;
using vehicle_sil::unit::throws;

SimTime at(std::chrono::milliseconds ms)
{
    return SimTime{} + ms;
}

long long ms_of(SimTime t)
{
    return static_cast<long long>(t.time_since_epoch() / 1ms);
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

void require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw Failure(what);
    }
}

bool contains(const std::vector<std::string>& lines, const std::string& line)
{
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

/// Lines of `lines` that start with `prefix`.
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
    LinSlaveOptions options{{}, std::nullopt};
    SimDuration step = 2ms;
    std::chrono::microseconds busy{0};
};

/// A LIN cluster of one master and any number of slaves, each a SIL Kit participant with its own
/// step size, driven by scripts.
struct Cluster
{
    LinScheduleTable schedule;
    SimDuration master_step = 5ms;
    LinMasterOptions master;
    std::chrono::microseconds master_busy{0};
    std::vector<SlaveSpec> slaves;
    SimDuration stop_at = 300ms;

    /// Called in the master's step before master.step(now); return false to skip step(now).
    std::function<bool(SimTime, LinMaster&)> master_script;
    /// Called in a slave's step before slave.step(now); return false to skip step(now).
    std::function<bool(std::size_t, SimTime, LinSlave&)> slave_script;
    /// Called for every frame a slave's step(now) hands to the application.
    std::function<void(std::size_t, SimTime, const LinReceivedFrame&, LinSlave&)> slave_frame;
    /// Called in a slave's step after slave.step(now).
    std::function<void(std::size_t, SimTime, LinSlave&)> slave_after_step;
    /// The tests that overflow an inbox on purpose turn the check off.
    bool expect_no_overflow = true;
};

/// What the nodes observed, as records that depend only on simulation time and data.
struct Trace
{
    std::vector<LinScheduleResult> results;
    std::vector<std::string> master; ///< results and states
    std::vector<std::vector<std::string>> slaves; ///< frames ("frame t=<header> rx=<step> ...") and states
    LinMasterStatistics master_stats;
    std::vector<LinSlaveStatistics> slave_stats;
};

Trace run(Cluster c)
{
    InProcessRegistry registry;
    std::vector<std::string> names{"Master"};
    for (std::size_t i = 0; i < c.slaves.size(); ++i)
    {
        names.push_back("Slave" + std::to_string(i));
    }

    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = names;
    controllerOptions.stop_at = c.stop_at;
    SimulationController controller(controllerOptions);

    auto make_runtime = [&](const std::string& name, SimDuration step) {
        return std::make_unique<ParticipantRuntime>(
            coordinated_options(name, registry.uri, VirtualTimeOptions{step, std::nullopt}));
    };

    Trace out;
    out.slaves.resize(c.slaves.size());
    // Results of slots that end at or after stop_at are never reported: keep only records well
    // before it, so that every trace is complete.
    const SimTime keepBefore = SimTime{} + c.stop_at - 20ms;

    auto masterRuntime = make_runtime("Master", c.master_step);
    SilKitLinPort masterPort(*masterRuntime->participant().CreateLinController("LIN1", "LIN_CLUSTER"), LinRole::Master);
    LinMaster master(masterPort, c.schedule, c.master_step, c.master);
    master.on_result([&](const LinScheduleResult& r) {
        if (r.header_time < keepBefore)
        {
            out.results.push_back(r);
            out.master.push_back("result " + describe(r));
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
        if (!c.master_script || c.master_script(now, master))
        {
            master.step(now);
        }
        if (c.master_busy.count() > 0)
        {
            vehicle_sil::unit::busy_for(c.master_busy);
        }
    });

    std::vector<std::unique_ptr<ParticipantRuntime>> slaveRuntimes;
    std::vector<std::unique_ptr<SilKitLinPort>> slavePorts;
    std::vector<std::unique_ptr<LinSlave>> slaves;
    std::vector<SimTime> slaveNow(c.slaves.size());
    for (std::size_t i = 0; i < c.slaves.size(); ++i)
    {
        slaveRuntimes.push_back(make_runtime(names[i + 1], c.slaves[i].step));
        auto& runtime = *slaveRuntimes.back();
        auto& trace = out.slaves[i];
        slavePorts.push_back(
            std::make_unique<SilKitLinPort>(*runtime.participant().CreateLinController("LIN1", "LIN_CLUSTER"), LinRole::Slave));
        slaves.push_back(std::make_unique<LinSlave>(*slavePorts.back(), c.slaves[i].options));
        slaves.back()->on_frame([&, i](const LinReceivedFrame& f) {
            if (f.header_time < keepBefore)
            {
                trace.push_back("frame t=" + std::to_string(ms_of(f.header_time)) + " rx=" + std::to_string(ms_of(slaveNow[i])) +
                                " id=" + std::to_string(f.id) + " data=" + hex(f.data.data(), f.data_length));
            }
            if (c.slave_frame)
            {
                c.slave_frame(i, slaveNow[i], f, *slaves[i]);
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
            if (c.slaves[i].busy.count() > 0)
            {
                vehicle_sil::unit::busy_for(c.slaves[i].busy);
            }
            slaveNow[i] = now;
            if (!c.slave_script || c.slave_script(i, now, slave))
            {
                slave.step(now);
            }
            if (c.slave_after_step)
            {
                c.slave_after_step(i, now, slave);
            }
        });
    }

    // On a failure path (a CHECK throws) stop the participants before the objects their handlers
    // use are destroyed. Declared last: destroyed first.
    std::vector<ParticipantRuntime*> runtimes{masterRuntime.get()};
    for (const auto& runtime : slaveRuntimes)
    {
        runtimes.push_back(runtime.get());
    }
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
    } stopAll{runtimes};

    start_together(controller, runtimes, 30s);
    CHECK(controller.wait_until_finished(scaled_timeout(120s)) == orch::ParticipantState::Shutdown);
    for (auto* runtime : runtimes)
    {
        CHECK(runtime->wait_until_finished(scaled_timeout(30s)) == orch::ParticipantState::Shutdown);
        require(runtime->step_error().empty(), "step error: " + runtime->step_error());
    }
    out.master_stats = master.statistics();
    for (const auto& slave : slaves)
    {
        out.slave_stats.push_back(slave->statistics());
    }
    if (c.expect_no_overflow)
    {
        CHECK_EQ(out.master_stats.inbox_overflows, 0u);
        for (const auto& stats : out.slave_stats)
        {
            CHECK_EQ(stats.inbox_overflows, 0u);
        }
    }
    return out;
}

/// Runs `make()` with the slaves slowed down and with the master slowed down, and requires the
/// same traces as without delays.
template <typename Make>
Trace run_at_every_speed(Make make)
{
    const Trace fast = run(make());
    Cluster slowSlaves = make();
    for (auto& slave : slowSlaves.slaves)
    {
        slave.busy = 1500us;
    }
    Cluster slowMaster = make();
    slowMaster.master_busy = 4ms;
    for (const auto& other : {run(slowSlaves), run(slowMaster)})
    {
        require(other.master == fast.master, "master trace depends on wall-clock speed");
        for (std::size_t s = 0; s < fast.slaves.size(); ++s)
        {
            require(other.slaves[s] == fast.slaves[s], "slave " + std::to_string(s) + " trace depends on wall-clock speed");
        }
    }
    return fast;
}

std::vector<LinScheduleResult> results_for(const Trace& t, LinId id)
{
    std::vector<LinScheduleResult> out;
    std::copy_if(t.results.begin(), t.results.end(), std::back_inserter(out), [id](const auto& r) { return r.id == id; });
    return out;
}

// ---------------------------------------------------------------------------------------------
// Schedule
// ---------------------------------------------------------------------------------------------

TEST_CASE(lin_schedule_keeps_mixed_slots_at_the_identifier_and_length_limits)
{
    // Identifier 0 (master, 1 byte), 0x3B (slave, 8 bytes, the highest unconditional-frame
    // identifier), 0x3F (slave, 1 byte); slots of 5, 15 and 10 ms: one cycle is 30 ms.
    Cluster c;
    c.schedule = LinScheduleTable{{0x00, 1, LinChecksumModel::Classic, 5ms, LinPublisher::Master},
                  {0x3B, 8, LinChecksumModel::Enhanced, 15ms, LinPublisher::Slave},
                  {0x3F, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave}};
    c.slaves = {SlaveSpec{LinSlaveOptions{{0x00}, std::nullopt}}};
    c.master_script = [](SimTime now, LinMaster& m) {
        const std::array<std::uint8_t, 1> value{static_cast<std::uint8_t>(ms_of(now))};
        m.set_frame_data(0x00, value);
        return true;
    };
    c.slave_after_step = [](std::size_t, SimTime now, LinSlave& s) {
        const std::array<std::uint8_t, 8> eight{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0xFF};
        const std::array<std::uint8_t, 1> one{0xA5};
        s.set_response(now, 0x3B, eight);
        s.set_response(now, 0x3F, one);
    };
    const auto t = run(c);

    require(t.results.size() >= 24u, std::to_string(t.results.size()) + " results");
    const std::array<long long, 3> offsets{0, 5, 20};
    for (std::size_t i = 0; i < t.results.size(); ++i)
    {
        const auto& r = t.results[i];
        const long long expected = static_cast<long long>(i / 3) * 30 + offsets[i % 3];
        require(ms_of(r.header_time) == expected && r.id == c.schedule[i % 3].id,
                "result " + std::to_string(i) + ": " + describe(r));
        require(r.status == LinResultStatus::Ok, "not answered: " + describe(r));
        switch (r.id)
        {
        case 0x00:
            require(r.data_length == 1 && r.data[0] == static_cast<std::uint8_t>(expected), describe(r));
            // The slave received it at its first step after the header.
            require(contains(t.slaves[0], "frame t=" + std::to_string(expected) + " rx=" + std::to_string(expected + 2) +
                                              " id=0 data=" + hex(r.data.data(), 1)),
                    "slave missed " + describe(r));
            break;
        case 0x3B:
            require(r.data_length == 8 && hex(r.data.data(), 8) == "00112233445566FF", describe(r));
            break;
        default:
            require(r.data_length == 1 && r.data[0] == 0xA5, describe(r));
            break;
        }
    }
    CHECK_EQ(t.master_stats.errors, 0u);
    CHECK_EQ(t.master_stats.no_responses, 0u);
}

TEST_CASE(lin_schedule_switch_requested_twice_in_a_slot_takes_the_last_table)
{
    Cluster c;
    c.schedule = LinScheduleTable{{0x10, 1, LinChecksumModel::Enhanced, 20ms, LinPublisher::Slave}};
    c.slaves = {SlaveSpec{}};
    c.master_script = [](SimTime now, LinMaster& m) {
        if (now == at(25ms)) // inside the slot that started at 20 ms
        {
            m.set_schedule({{0x11, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave}});
        }
        if (now == at(30ms))
        {
            m.set_schedule({{0x12, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave}});
        }
        return true;
    };
    c.slave_after_step = [](std::size_t, SimTime now, LinSlave& s) {
        const std::array<std::uint8_t, 1> one{0x01};
        for (const LinId id : {0x10, 0x11, 0x12})
        {
            s.set_response(now, id, one);
        }
    };
    const auto t = run(c);
    std::vector<std::string> got;
    for (const auto& r : t.results)
    {
        if (r.header_time <= at(60ms))
        {
            got.push_back(describe(r));
        }
    }
    // Nothing was set before the first header.
    const std::vector<std::string> expected{"t=0 id=16 no_response data=", "t=20 id=16 ok data=01", "t=40 id=18 ok data=01",
                                            "t=50 id=18 ok data=01", "t=60 id=18 ok data=01"};
    require(got == expected, "results differ: first " + (got.empty() ? std::string("(none)") : got.front()));
}

// ---------------------------------------------------------------------------------------------
// Checksum models and colliding responses
// ---------------------------------------------------------------------------------------------

TEST_CASE(lin_checksum_model_mismatch_is_an_error_in_both_directions)
{
    constexpr LinId kClassicId = 0x20;  // classic in the schedule, the slave answers enhanced
    constexpr LinId kEnhancedId = 0x21; // enhanced in the schedule, the slave answers classic
    constexpr LinId kMatchingId = 0x22;
    Cluster c;
    c.schedule = LinScheduleTable{{kClassicId, 2, LinChecksumModel::Classic, 10ms, LinPublisher::Slave},
                  {kEnhancedId, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave},
                  {kMatchingId, 2, LinChecksumModel::Classic, 10ms, LinPublisher::Slave}};
    c.slaves = {SlaveSpec{}};
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
            continue; // nothing set before the first header
        }
        const auto expected = r.id == kMatchingId ? LinResultStatus::Ok : LinResultStatus::Error;
        require(r.status == expected, "unexpected " + describe(r));
        if (expected == LinResultStatus::Error)
        {
            require(r.data_length == 0, "an erroneous response must not carry data: " + describe(r));
        }
    }
}

TEST_CASE(lin_two_slaves_answering_one_identifier_collide_at_every_speed)
{
    // On a real bus two responses to one header collide: the master sees a bit or checksum
    // error. With SIL Kit both responses arrive, in an order that depends on wall-clock speed, so
    // taking the first one would make the result depend on speed.
    auto make = [] {
        Cluster c;
        c.schedule = LinScheduleTable{{0x30, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave},
                      {0x31, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave}};
        c.slaves = {SlaveSpec{}, SlaveSpec{}};
        c.stop_at = 200ms;
        c.slave_after_step = [](std::size_t i, SimTime now, LinSlave& s) {
            const std::array<std::uint8_t, 1> value{static_cast<std::uint8_t>(0xA0 + i)};
            s.set_response(now, 0x30, value); // both
            if (i == 1)
            {
                s.set_response(now, 0x31, value); // slave 1 only
            }
        };
        return c;
    };
    const auto t = run_at_every_speed(make);
    require(t.results.size() >= 16u, std::to_string(t.results.size()) + " results");
    for (const auto& r : t.results)
    {
        if (r.header_time == SimTime{})
        {
            continue;
        }
        if (r.id == 0x30)
        {
            require(r.status == LinResultStatus::Error && r.data_length == 0, "collision not reported: " + describe(r));
        }
        else
        {
            require(r.status == LinResultStatus::Ok && r.data[0] == 0xA1, "unexpected " + describe(r));
        }
    }
}

TEST_CASE(lin_slaves_taking_turns_on_one_identifier_keep_getting_headers)
{
    // Slave 0 answers until 100 ms, slave 1 from 100 ms on, nobody from 200 ms on. Each answer
    // makes SIL Kit record the slave as a responder of the identifier; the headers must still go
    // out once both have answered (SIL Kit patch 0001).
    Cluster c;
    c.schedule = LinScheduleTable{{0x18, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave}};
    c.slaves = {SlaveSpec{}, SlaveSpec{}};
    c.slave_after_step = [](std::size_t i, SimTime now, LinSlave& s) {
        const bool mine = i == 0 ? now < at(100ms) : now >= at(100ms) && now < at(200ms);
        if (mine)
        {
            const std::array<std::uint8_t, 1> value{static_cast<std::uint8_t>(0xB0 + i)};
            s.set_response(now, 0x18, value);
        }
        else
        {
            s.clear_response(now, 0x18);
        }
    };
    const auto t = run(c);
    require(t.results.size() >= 27u, std::to_string(t.results.size()) + " results");
    for (const auto& r : t.results)
    {
        const auto ms = ms_of(r.header_time);
        const std::string expected = ms == 0 || ms > 200 ? "no_response data="
                                     : ms <= 100        ? "ok data=B0"
                                                        : "ok data=B1";
        require(describe(r) == "t=" + std::to_string(ms) + " id=24 " + expected, "unexpected " + describe(r));
    }
}

TEST_CASE(lin_slave_answering_a_master_frame_collides_with_the_master_at_every_speed)
{
    // A slave configured to answer an identifier the master publishes itself: both send a
    // response to the same header.
    auto make = [] {
        Cluster c;
        c.schedule = LinScheduleTable{{0x05, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master}};
        c.slaves = {SlaveSpec{}};
        c.stop_at = 200ms;
        c.master_script = [](SimTime, LinMaster& m) {
            const std::array<std::uint8_t, 1> value{0x11};
            m.set_frame_data(0x05, value);
            return true;
        };
        c.slave_after_step = [](std::size_t, SimTime now, LinSlave& s) {
            if (now >= at(50ms))
            {
                const std::array<std::uint8_t, 1> value{0x22};
                s.set_response(now, 0x05, value);
            }
        };
        return c;
    };
    const auto t = run_at_every_speed(make);
    require(t.results.size() >= 17u, std::to_string(t.results.size()) + " results");
    for (const auto& r : t.results)
    {
        const std::string expected = r.header_time <= at(50ms) ? "ok data=11" : "error data=";
        require(describe(r) == "t=" + std::to_string(ms_of(r.header_time)) + " id=5 " + expected, "unexpected " + describe(r));
    }
    CHECK(t.master_stats.errors >= 11u);
}

// ---------------------------------------------------------------------------------------------
// Diagnostic frames
// ---------------------------------------------------------------------------------------------

constexpr std::uint8_t kNad0 = 0x0A; // node address of slave 0
constexpr std::uint8_t kNad1 = 0x0B; // node address of slave 1
constexpr LinId kCommandId = 0x10;

/// A normal frame and the two diagnostic frames, 10 ms each.
LinScheduleTable diagnostic_schedule()
{
    return {{kCommandId, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master},
            {kMasterRequestId, 8, LinChecksumModel::Classic, 10ms, LinPublisher::Master},
            {kSlaveResponseId, 8, LinChecksumModel::Classic, 10ms, LinPublisher::Slave}};
}

/// A slave diagnostic node: answers a single-frame request addressed to its NAD with a positive
/// response (SID + 0x40) that echoes the request's first two data bytes.
void answer_single_frame(std::uint8_t nad, SimTime now, const LinReceivedFrame& f, LinSlave& s)
{
    if (f.id != kMasterRequestId || f.data[0] != nad || (f.data[1] & 0xF0u) != 0x00u)
    {
        return;
    }
    const std::array<std::uint8_t, 8> response{nad, 0x03, static_cast<std::uint8_t>(f.data[2] + 0x40), f.data[3], f.data[4],
                                               0xFF, 0xFF, 0xFF};
    s.set_slave_response(now, response);
}

TEST_CASE(lin_diagnostic_request_is_sent_once_and_answered_once)
{
    // Read by identifier (SID 0xB2) to slave 0 at 45 ms; the master request slot comes at 70 ms,
    // the slave response slot at 80 ms. Slave 1 has another NAD and stays silent.
    const std::array<std::uint8_t, 8> request{kNad0, 0x06, 0xB2, 0x00, 0x34, 0x12, 0xFF, 0xFF};
    Cluster c;
    c.schedule = diagnostic_schedule();
    c.slaves = {SlaveSpec{LinSlaveOptions{{kMasterRequestId}, std::nullopt}},
                SlaveSpec{LinSlaveOptions{{kMasterRequestId}, std::nullopt}}};
    c.master_script = [&request](SimTime now, LinMaster& m) {
        const std::array<std::uint8_t, 1> command{0x01};
        m.set_frame_data(kCommandId, command);
        if (now == at(45ms))
        {
            require(!m.master_request_pending(), "a request is pending before the first one");
            require(m.send_master_request(request), "request refused");
            require(m.master_request_pending(), "request not pending");
            require(!m.send_master_request(request), "a second request was accepted while the first was pending");
        }
        if (now == at(75ms))
        {
            require(!m.master_request_pending(), "request still pending after its slot");
        }
        return true;
    };
    c.slave_frame = [](std::size_t i, SimTime now, const LinReceivedFrame& f, LinSlave& s) {
        answer_single_frame(i == 0 ? kNad0 : kNad1, now, f, s);
    };
    const auto t = run(c);

    // Without a request, the master request slot stays silent: no header, no result.
    const auto requests = results_for(t, kMasterRequestId);
    require(requests.size() == 1u, std::to_string(requests.size()) + " master requests");
    require(describe(requests[0]) == "t=70 id=60 ok data=0A06B2003412FFFF", describe(requests[0]));
    const auto responses = results_for(t, kSlaveResponseId);
    require(responses.size() >= 9u, std::to_string(responses.size()) + " slave response slots");
    for (const auto& r : responses)
    {
        if (r.header_time == at(80ms))
        {
            require(describe(r) == "t=80 id=61 ok data=0A03F20034FFFFFF", describe(r));
        }
        else
        {
            require(r.status == LinResultStatus::NoResponse, "slave response sent more than once: " + describe(r));
        }
    }
    // Both slaves received the request, stamped with its header time, at their next step.
    for (std::size_t s = 0; s < 2; ++s)
    {
        require(starting_with(t.slaves[s], "frame") == std::vector<std::string>{"frame t=70 rx=72 id=60 data=0A06B2003412FFFF"},
                "slave " + std::to_string(s) + " frames differ");
    }
    CHECK_EQ(t.slave_stats[0].responses_sent, 1u);
    CHECK_EQ(t.slave_stats[1].responses_sent, 0u);
}

TEST_CASE(lin_diagnostic_multi_frame_exchange_is_independent_of_wall_clock_speed)
{
    // A 10-byte request (first frame + consecutive frame, ISO 17987-2 transport layer) to slave 1,
    // answered with a 6-byte single frame; then a single-frame request to slave 0. The master
    // sends the next frame as soon as the previous one has left; the slave answers when the
    // message is complete.
    struct MasterScript
    {
        std::vector<std::array<std::uint8_t, 8>> frames{
            {kNad1, 0x10, 0x0A, 0x2E, 0x01, 0x02, 0x03, 0x04}, // FF: 10 bytes, SID 0x2E
            {kNad1, 0x21, 0x05, 0x06, 0x07, 0x08, 0x09, 0xFF}, // CF 1
            {kNad0, 0x06, 0xB2, 0x00, 0x34, 0x12, 0xFF, 0xFF}, // SF to slave 0
        };
        std::size_t next = 0;
    };
    auto make = [] {
        auto script = std::make_shared<MasterScript>();
        // Per slave: bytes received of the current multi-frame request.
        auto assembled = std::make_shared<std::array<std::vector<std::uint8_t>, 2>>();
        Cluster c;
        c.schedule = diagnostic_schedule();
        c.slaves = {SlaveSpec{LinSlaveOptions{{kMasterRequestId}, std::nullopt}},
                    SlaveSpec{LinSlaveOptions{{kMasterRequestId}, std::nullopt}, 1ms}};
        c.stop_at = 400ms;
        c.master_script = [script](SimTime now, LinMaster& m) {
            const std::array<std::uint8_t, 1> command{0x01};
            m.set_frame_data(kCommandId, command);
            if (now >= at(20ms) && script->next < script->frames.size() && !m.master_request_pending())
            {
                require(m.send_master_request(script->frames[script->next]), "request refused");
                ++script->next;
            }
            return true;
        };
        c.slave_frame = [assembled](std::size_t i, SimTime now, const LinReceivedFrame& f, LinSlave& s) {
            const std::uint8_t nad = i == 0 ? kNad0 : kNad1;
            if (f.id != kMasterRequestId || f.data[0] != nad)
            {
                return;
            }
            auto& bytes = (*assembled)[i];
            const std::uint8_t pci = f.data[1];
            if ((pci & 0xF0u) == 0x00u)
            {
                answer_single_frame(nad, now, f, s);
            }
            else if ((pci & 0xF0u) == 0x10u)
            {
                bytes.assign(f.data.begin() + 3, f.data.end());
            }
            else if ((pci & 0xF0u) == 0x20u)
            {
                bytes.insert(bytes.end(), f.data.begin() + 2, f.data.end());
                if (bytes.size() >= 10u)
                {
                    // Positive response to SID 0x2E with the byte sum of the 9 data bytes.
                    std::uint8_t sum = 0;
                    for (std::size_t b = 1; b < 10; ++b)
                    {
                        sum = static_cast<std::uint8_t>(sum + bytes[b]);
                    }
                    const std::array<std::uint8_t, 8> response{nad, 0x05, 0x6E, 0x01, 0x02, sum, 0x00, 0xFF};
                    s.set_slave_response(now, response);
                    bytes.clear();
                }
            }
        };
        return c;
    };
    const auto t = run_at_every_speed(make);
    std::vector<std::string> diagnostic;
    for (const auto& r : t.results)
    {
        if ((r.id == kMasterRequestId || r.id == kSlaveResponseId) && r.status != LinResultStatus::NoResponse)
        {
            diagnostic.push_back(describe(r));
        }
    }
    const std::vector<std::string> expected{
        "t=40 id=60 ok data=0B100A2E01020304", "t=70 id=60 ok data=0B210506070809FF",
        "t=80 id=61 ok data=0B056E01022D00FF", "t=100 id=60 ok data=0A06B2003412FFFF",
        "t=110 id=61 ok data=0A03F20034FFFFFF",
    };
    std::string got;
    for (const auto& line : diagnostic)
    {
        got += "\n    " + line;
    }
    require(diagnostic == expected, "diagnostic exchange differs:" + got);
}

TEST_CASE(lin_slave_responses_can_be_replaced_withdrawn_and_collide)
{
    // Slave 0 sets A at 12 ms and B at 14 ms (header 20 ms: B), sets C at 32 ms and withdraws it
    // at 34 ms (header 50 ms: none), sets D at 60 ms (header 80 ms: D); both slaves set a
    // response at 92 ms (header 110 ms: collision). The master's request slot is never used.
    const std::array<std::uint8_t, 8> a{kNad0, 0x01, 0xAA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const std::array<std::uint8_t, 8> b{kNad0, 0x01, 0xBB, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const std::array<std::uint8_t, 8> d{kNad0, 0x01, 0xDD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const std::array<std::uint8_t, 8> e{kNad1, 0x01, 0xEE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    Cluster c;
    c.schedule = diagnostic_schedule();
    c.slaves = {SlaveSpec{}, SlaveSpec{}};
    c.stop_at = 200ms;
    c.master_script = [](SimTime, LinMaster& m) {
        const std::array<std::uint8_t, 1> command{0x01};
        m.set_frame_data(kCommandId, command);
        return true;
    };
    c.slave_after_step = [&](std::size_t i, SimTime now, LinSlave& s) {
        if (i == 0)
        {
            if (now == at(12ms))
            {
                s.set_slave_response(now, a);
            }
            if (now == at(14ms))
            {
                s.set_slave_response(now, b);
            }
            if (now == at(32ms))
            {
                s.set_slave_response(now, a);
            }
            if (now == at(34ms))
            {
                s.clear_response(now, kSlaveResponseId);
            }
            if (now == at(60ms))
            {
                s.set_slave_response(now, d);
            }
        }
        if (now == at(92ms))
        {
            s.set_slave_response(now, i == 0 ? d : e);
        }
    };
    const auto t = run(c);
    std::vector<std::string> got;
    for (const auto& r : results_for(t, kSlaveResponseId))
    {
        got.push_back(describe(r));
    }
    require(results_for(t, kMasterRequestId).empty(), "a master request slot was used without a request");
    const std::vector<std::string> expected{
        "t=20 id=61 ok data=0A01BBFFFFFFFFFF", "t=50 id=61 no_response data=", "t=80 id=61 ok data=0A01DDFFFFFFFFFF",
        "t=110 id=61 error data=",             "t=140 id=61 no_response data=", "t=170 id=61 no_response data=",
    };
    std::string text;
    for (const auto& line : got)
    {
        text += "\n    " + line;
    }
    require(got == expected, "slave responses differ:" + text);
}

TEST_CASE(lin_master_request_can_be_cancelled_before_its_slot)
{
    const std::array<std::uint8_t, 8> first{kNad0, 0x06, 0xB2, 0x00, 0x34, 0x12, 0xFF, 0xFF};
    const std::array<std::uint8_t, 8> second{kNad1, 0x06, 0xB2, 0x00, 0x34, 0x12, 0xFF, 0xFF};
    Cluster c;
    c.schedule = diagnostic_schedule();
    c.slaves = {SlaveSpec{LinSlaveOptions{{kMasterRequestId}, std::nullopt}}};
    c.stop_at = 200ms;
    c.master_script = [&](SimTime now, LinMaster& m) {
        const std::array<std::uint8_t, 1> command{0x01};
        m.set_frame_data(kCommandId, command);
        if (now == at(15ms))
        {
            require(m.send_master_request(first), "first request refused");
            m.cancel_master_request();
            require(!m.master_request_pending(), "cancelled request still pending");
        }
        if (now == at(50ms))
        {
            require(m.send_master_request(second), "second request refused");
        }
        return true;
    };
    const auto t = run(c);
    const auto requests = results_for(t, kMasterRequestId);
    require(requests.size() == 1u && describe(requests[0]) == "t=70 id=60 ok data=0B06B2003412FFFF",
            "only the second request must be sent, at 70 ms");
    require(starting_with(t.slaves[0], "frame") == std::vector<std::string>{"frame t=70 rx=72 id=60 data=0B06B2003412FFFF"},
            "slave frames differ");
}

TEST_CASE(lin_master_request_queued_while_asleep_is_sent_after_the_restart)
{
    const std::array<std::uint8_t, 8> request{kNad0, 0x06, 0xB2, 0x00, 0x34, 0x12, 0xFF, 0xFF};
    Cluster c;
    c.schedule = diagnostic_schedule();
    c.slaves = {SlaveSpec{LinSlaveOptions{{kMasterRequestId}, std::nullopt}}};
    c.master_script = [&request](SimTime now, LinMaster& m) {
        const std::array<std::uint8_t, 1> command{0x01};
        m.set_frame_data(kCommandId, command);
        if (now == at(30ms))
        {
            m.request_sleep();
        }
        if (now == at(50ms))
        {
            require(m.send_master_request(request), "request refused while asleep");
        }
        if (now == at(100ms))
        {
            m.request_wakeup();
        }
        return true;
    };
    c.slave_frame = [](std::size_t, SimTime now, const LinReceivedFrame& f, LinSlave& s) {
        answer_single_frame(kNad0, now, f, s);
    };
    const auto t = run(c);
    // Asleep from 30 to 100 ms; the schedule restarts at 200 ms with the command, then the
    // request at 210 ms and the response at 220 ms.
    const auto requests = results_for(t, kMasterRequestId);
    require(requests.size() == 1u && requests[0].header_time == at(210ms), "request not sent at 210 ms");
    const auto responses = results_for(t, kSlaveResponseId);
    const auto ok = std::find_if(responses.begin(), responses.end(),
                                 [](const auto& r) { return r.status == LinResultStatus::Ok; });
    require(ok != responses.end() && ok->header_time == at(220ms), "response not at 220 ms");
    // The go-to-sleep command (also identifier 0x3C) is not handed to the application.
    require(starting_with(t.slaves[0], "frame") == std::vector<std::string>{"frame t=210 rx=212 id=60 data=0A06B2003412FFFF"},
            "slave frames differ");
}

// ---------------------------------------------------------------------------------------------
// Sleep, wakeup and bus idle
// ---------------------------------------------------------------------------------------------

TEST_CASE(lin_slave_sleeps_exactly_at_the_default_bus_idle_timeout)
{
    // LIN 2.x: a slave goes to sleep after 4 s without bus activity. The master stops at 100 ms;
    // its last header is the one at 90 ms.
    Cluster c;
    c.schedule = LinScheduleTable{{0x10, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master}};
    c.slaves = {SlaveSpec{LinSlaveOptions{{0x10}}}}; // default timeout (4 s)
    c.stop_at = 4200ms;
    c.master_script = [](SimTime now, LinMaster& m) {
        const std::array<std::uint8_t, 1> value{0x01};
        m.set_frame_data(0x10, value);
        return now < at(100ms);
    };
    const auto t = run(c);
    require(starting_with(t.slaves[0], "state") == std::vector<std::string>{"state t=4090 sleep"},
            "slave did not sleep exactly 4 s after the last frame");
}

TEST_CASE(lin_simultaneous_wakeup_pulses_wake_the_master_once)
{
    Cluster c;
    c.schedule = LinScheduleTable{{0x10, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master}};
    c.slaves = {SlaveSpec{}, SlaveSpec{LinSlaveOptions{{}, std::nullopt}, 4ms}};
    c.master_script = [](SimTime now, LinMaster& m) {
        const std::array<std::uint8_t, 1> value{0x01};
        m.set_frame_data(0x10, value);
        if (now == at(50ms))
        {
            m.request_sleep();
        }
        return true;
    };
    c.slave_script = [](std::size_t, SimTime now, LinSlave& s) {
        if (now == at(100ms))
        {
            s.request_wakeup(); // both slaves, at the same time
        }
        return true;
    };
    const auto t = run(c);
    require(starting_with(t.master, "state") ==
                std::vector<std::string>{"state t=50 sleep", "state t=105 operational"},
            "master states differ");
    CHECK_EQ(t.master_stats.wakeups_by_slaves, 1u);
    for (std::size_t s = 0; s < 2; ++s)
    {
        require(starting_with(t.slaves[s], "state") ==
                    std::vector<std::string>{"state t=50 sleep", "state t=100 operational"},
                "slave " + std::to_string(s) + " states differ");
        CHECK_EQ(t.slave_stats[s].wakeups_sent, 1u);
    }
    // The schedule restarts 100 ms after the pulses.
    const auto restart = std::find_if(t.results.begin(), t.results.end(),
                                      [](const auto& r) { return r.header_time > at(50ms); });
    require(restart != t.results.end() && restart->header_time == at(200ms), "schedule did not restart at 200 ms");
}

TEST_CASE(lin_master_drops_requests_that_do_not_apply_and_restarts_at_once_without_a_delay)
{
    Cluster c;
    c.schedule = LinScheduleTable{{0x10, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master}};
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
        return true;
    };
    const auto t = run(c);
    require(starting_with(t.master, "state") ==
                std::vector<std::string>{"state t=50 sleep", "state t=120 operational"},
            "master states differ");
    CHECK_EQ(t.master_stats.wakeups_by_master, 1u);
    // wakeup_delay 0: the first header goes out in the step that sends the pulse.
    const auto restart = std::find_if(t.results.begin(), t.results.end(),
                                      [](const auto& r) { return r.header_time > at(50ms); });
    require(restart != t.results.end() && restart->header_time == at(120ms), "schedule did not restart at 120 ms");
}

// ---------------------------------------------------------------------------------------------
// Resource exhaustion
// ---------------------------------------------------------------------------------------------

TEST_CASE(lin_slave_inbox_overflow_is_counted_and_the_slave_recovers)
{
    // Slave 0 does not call step() from 10 ms to 4.5 s. A header and a frame every 2 ms fill its
    // inbox (4096 events) after about 4.1 s; the events after that are lost. Slave 1 steps
    // normally.
    Cluster c;
    c.master_step = 1ms;
    c.schedule = LinScheduleTable{{0x10, 1, LinChecksumModel::Enhanced, 2ms, LinPublisher::Master}};
    c.slaves = {SlaveSpec{LinSlaveOptions{{0x10}, std::nullopt}, 1ms}, SlaveSpec{LinSlaveOptions{{0x10}, std::nullopt}, 1ms}};
    c.stop_at = 4600ms;
    c.expect_no_overflow = false;
    c.master_script = [](SimTime now, LinMaster& m) {
        const std::array<std::uint8_t, 1> value{static_cast<std::uint8_t>(ms_of(now) / 2)};
        m.set_frame_data(0x10, value);
        return true;
    };
    c.slave_script = [](std::size_t i, SimTime now, LinSlave&) {
        return i != 0 || now < at(10ms) || now >= at(4500ms);
    };
    const auto t = run(c);
    // Slave 0's inbox holds 4096 events: 2048 headers and frames of the 2245 in 10..4500 ms.
    require(t.slave_stats[0].inbox_overflows > 0u, "no overflow counted");
    CHECK_EQ(t.slave_stats[1].inbox_overflows, 0u);
    // After the stall, slave 0 receives every frame again, at its first step after the header. Not
    // necessarily the one at 4500 ms: the master's step at 4500 ms may run, and its header reach
    // slave 0, before slave 0's step at 4500 ms has emptied the inbox, and an event that comes
    // while the inbox is full is lost. From 4502 ms on, the inbox has been emptied.
    for (long long ms = 4502; ms < 4580; ms += 2)
    {
        const std::uint8_t value = static_cast<std::uint8_t>(ms / 2);
        require(contains(t.slaves[0], "frame t=" + std::to_string(ms) + " rx=" + std::to_string(ms + 1) +
                                          " id=16 data=" + hex(&value, 1)),
                "slave 0 missed the frame at " + std::to_string(ms) + " ms after the stall");
    }
    // The overflow dropped the newest events: the oldest ones were kept and delivered in order.
    const auto frames = starting_with(t.slaves[0], "frame");
    require(frames.size() > 2u && frames[1].rfind("frame t=2 rx=3 ", 0) == 0, "first frames differ");
}

// ---------------------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------------------

TEST_CASE(lin_rejects_misused_diagnostic_frames)
{
    InProcessRegistry registry;
    ParticipantRuntimeOptions options;
    options.participant_name = "Invalid";
    options.registry_uri = registry.uri;
    ParticipantRuntime runtime(options);
    auto* masterController = runtime.participant().CreateLinController("LIN1", "LIN_X");
    auto* slaveController = runtime.participant().CreateLinController("LIN2", "LIN_X");

    // Diagnostic frames: 8 bytes, classic checksum, the master publishes 0x3C and a slave 0x3D.
    const LinScheduleEntry request{kMasterRequestId, 8, LinChecksumModel::Classic, 5ms, LinPublisher::Master};
    const LinScheduleEntry response{kSlaveResponseId, 8, LinChecksumModel::Classic, 5ms, LinPublisher::Slave};
    validate_schedule({request, response}, 5ms);
    for (auto bad : {request, response})
    {
        auto shortFrame = bad;
        shortFrame.data_length = 7;
        auto enhanced = bad;
        enhanced.checksum_model = LinChecksumModel::Enhanced;
        auto otherPublisher = bad;
        otherPublisher.publisher = bad.publisher == LinPublisher::Master ? LinPublisher::Slave : LinPublisher::Master;
        for (const auto& entry : {shortFrame, enhanced, otherPublisher})
        {
            CHECK(throws<std::invalid_argument>([&] { validate_schedule({entry}, 5ms); }));
        }
    }

    SilKitLinPort masterPort(*masterController, LinRole::Master);
    LinMaster master(masterPort, diagnostic_schedule(), 5ms);
    const std::array<std::uint8_t, 8> sleepCommand{0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const std::array<std::uint8_t, 8> request8{kNad0, 0x06, 0xB2, 0x00, 0x34, 0x12, 0xFF, 0xFF};
    const std::array<std::uint8_t, 7> seven{};
    // The master request goes through send_master_request(), sent once.
    CHECK(throws<std::invalid_argument>([&] { master.set_frame_data(kMasterRequestId, request8); }));
    // NAD 0 is the go-to-sleep command (request_sleep()).
    CHECK(throws<std::invalid_argument>([&] { master.send_master_request(sleepCommand); }));
    CHECK(throws<std::invalid_argument>([&] { master.send_master_request(seven); }));
    CHECK(!master.master_request_pending());

    SilKitLinPort slavePort(*slaveController, LinRole::Slave);
    LinSlave slave(slavePort);
    // The slave response goes through set_slave_response(), answered once; 0x3C is the master's.
    CHECK(throws<std::invalid_argument>([&] { slave.set_response(SimTime{}, kSlaveResponseId, request8, LinChecksumModel::Classic); }));
    CHECK(throws<std::invalid_argument>([&] { slave.set_response(SimTime{}, kMasterRequestId, request8, LinChecksumModel::Classic); }));
    CHECK(throws<std::invalid_argument>([&] { slave.set_slave_response(SimTime{}, seven); }));
    slave.set_slave_response(SimTime{}, request8);
    CHECK(throws<std::logic_error>([&] { slave.set_slave_response(SimTime{} - 1ms, request8); }));
}

} // namespace
