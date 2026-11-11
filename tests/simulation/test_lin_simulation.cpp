// SPDX-License-Identifier: MIT
//
// The same LIN ECUs (a master and two slaves, written against Ecu& only) on the local virtual
// buses and on SIL Kit: the schedule's results (also a response of the wrong length and a slave
// answering a master frame), the frames the slaves received, the state changes and the
// statistics must be the same.
#include "test_harness.hpp"

#include "vehicle_sil/simulation/simulation.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::Ecu;
using vehicle_sil::SilKitOptions;
using vehicle_sil::SimTime;
using vehicle_sil::Simulation;
using vehicle_sil::unit::Failure;
using vehicle_sil::unit::throws_with;
namespace lin = vehicle_sil::lin;

constexpr lin::LinId kCommand = 0x10; // the master's frame
constexpr lin::LinId kEcho = 0x20;    // slave 0 echoes the newest command
constexpr lin::LinId kStatus = 0x21;  // slave 1, classic checksum
constexpr lin::LinId kBoth = 0x22;    // both slaves answer: a collision
constexpr lin::LinId kNobody = 0x23;  // nobody answers
constexpr lin::LinId kShort = 0x24;   // slave 1 answers with fewer bytes than the schedule's
// No slave may subscribe kClash: on SIL Kit a subscriber may or may not get the frame of a
// collision, so the records of the two runtimes could then differ.
constexpr lin::LinId kClash = 0x25;   // the master's frame, which slave 0 answers as well
constexpr std::uint8_t kNad = 0x0A;   // node address of slave 0
const std::array<std::uint8_t, 8> kRequest{kNad, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

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

const char* status_name(lin::LinResultStatus s)
{
    switch (s)
    {
    case lin::LinResultStatus::Ok:
        return "ok";
    case lin::LinResultStatus::NoResponse:
        return "no_response";
    case lin::LinResultStatus::Error:
        return "error";
    }
    return "?";
}

std::string state_line(SimTime t, lin::LinNodeState s)
{
    return "state t=" + std::to_string(ms_of(t)) + " " + lin::to_string(s);
}

lin::LinScheduleTable schedule()
{
    using lin::LinChecksumModel;
    using lin::LinPublisher;
    return {{kCommand, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master},
            {kEcho, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave},
            {kStatus, 1, LinChecksumModel::Classic, 10ms, LinPublisher::Slave},
            {kBoth, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave},
            {kNobody, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave},
            {kShort, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave},
            {kClash, 1, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master},
            {lin::kMasterRequestId, 8, LinChecksumModel::Classic, 10ms, LinPublisher::Master},
            {lin::kSlaveResponseId, 8, LinChecksumModel::Classic, 10ms, LinPublisher::Slave}};
}

/// The master: publishes its step's time and kClash, queues a diagnostic request at 100 ms,
/// sleeps at 200 and 500 ms, wakes the cluster at 550 ms. A slave wakes it in between.
class Body
{
public:
    explicit Body(Ecu& ecu) : master(ecu.lin_master("BODY_LIN", schedule()))
    {
        master.on_result([this](const lin::LinScheduleResult& r) {
            lines.push_back("result t=" + std::to_string(ms_of(r.header_time)) + " id=" + std::to_string(r.id) + " " +
                            status_name(r.status) + " data=" + hex(r.data.data(), r.data_length));
        });
        master.on_state([this](SimTime t, lin::LinNodeState s) { lines.push_back(state_line(t, s)); });
        ecu.on_step([this](SimTime now) {
            const auto ms = static_cast<std::uint16_t>(ms_of(now));
            master.set_frame_data(kCommand, std::array{static_cast<std::uint8_t>(ms & 0xFFu), static_cast<std::uint8_t>(ms >> 8)});
            master.set_frame_data(kClash, std::array<std::uint8_t, 1>{0x77});
            if (now == at(100ms))
            {
                static_cast<void>(master.send_master_request(kRequest));
            }
            if (now == at(200ms) || now == at(500ms))
            {
                master.request_sleep();
            }
            if (now == at(550ms))
            {
                master.request_wakeup();
            }
        });
    }

    lin::LinMaster& master;
    std::vector<std::string> lines;
};

/// A slave: 0 echoes the command, answers the diagnostic request and the master's kClash, 1
/// reports a status, answers kShort with one byte and sends a wakeup pulse at 302 ms. Both
/// answer kBoth.
class Node
{
public:
    Node(Ecu& ecu, std::size_t index) :
      slave(ecu.lin_slave("BODY_LIN", lin::LinSlaveOptions{{kCommand, lin::kMasterRequestId}, std::nullopt}))
    {
        slave.on_frame([this](const lin::LinReceivedFrame& f) {
            lines.push_back("frame t=" + std::to_string(ms_of(f.header_time)) + " rx=" + std::to_string(ms_of(now_)) +
                            " id=" + std::to_string(f.id) + " data=" + hex(f.data.data(), f.data_length));
            if (f.id == kCommand)
            {
                command_ = {f.data[0], f.data[1]};
            }
            if (f.id == lin::kMasterRequestId && f.data[0] == kNad && index_ == 0)
            {
                slave.set_slave_response(now_, std::array<std::uint8_t, 8>{kNad, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03});
            }
        });
        slave.on_state([this](SimTime t, lin::LinNodeState s) { lines.push_back(state_line(t, s)); });
        index_ = index;
        ecu.on_step([this](SimTime now) {
            now_ = now;
            if (index_ == 0)
            {
                slave.set_response(now, kEcho, command_);
                slave.set_response(now, kClash, std::array<std::uint8_t, 1>{0x66});
            }
            else
            {
                slave.set_response(now, kStatus, std::array<std::uint8_t, 1>{0x5A}, lin::LinChecksumModel::Classic);
                slave.set_response(now, kShort, std::array<std::uint8_t, 1>{0x11});
            }
            slave.set_response(now, kBoth, std::array<std::uint8_t, 1>{static_cast<std::uint8_t>(0xA0 + index_)});
            if (index_ == 1 && now == at(302ms))
            {
                slave.request_wakeup();
            }
        });
    }

    lin::LinSlave& slave;
    std::vector<std::string> lines;

private:
    std::size_t index_ = 0;
    SimTime now_{};
    std::array<std::uint8_t, 2> command_{};
};

struct Records
{
    std::vector<std::string> body;
    std::vector<std::string> door;
    std::vector<std::string> sensor;
};

Records run(std::optional<SilKitOptions> silkit)
{
    Simulation sim(silkit);
    auto& body = sim.add<Body>("Body", 5ms);
    auto& door = sim.add<Node>("Door", 4ms, std::size_t{0});
    auto& sensor = sim.add<Node>("Sensor", 2ms, std::size_t{1});
    sim.run_for(700ms);
    Records out{body.lines, door.lines, sensor.lines};
    const auto m = body.master.statistics();
    out.body.push_back("stats headers=" + std::to_string(m.headers_sent) + " ok=" + std::to_string(m.responses_ok) +
                       " no_response=" + std::to_string(m.no_responses) + " errors=" + std::to_string(m.errors) +
                       " sleep=" + std::to_string(m.sleep_commands) + " wakeups=" + std::to_string(m.wakeups_by_master) +
                       "/" + std::to_string(m.wakeups_by_slaves));
    for (auto [node, lines] : {std::pair{&door, &out.door}, std::pair{&sensor, &out.sensor}})
    {
        const auto s = node->slave.statistics();
        lines->push_back("stats responses=" + std::to_string(s.responses_sent) + " ignored=" + std::to_string(s.headers_ignored) +
                         " frames=" + std::to_string(s.frames_received) + " wakeups=" + std::to_string(s.wakeups_sent));
    }
    return out;
}

void require_same(const std::vector<std::string>& silkit, const std::vector<std::string>& local, const std::string& who)
{
    if (silkit == local)
    {
        return;
    }
    std::size_t i = 0;
    while (i < silkit.size() && i < local.size() && silkit[i] == local[i])
    {
        ++i;
    }
    throw Failure(who + ": SIL Kit and local differ at record " + std::to_string(i) + ": \"" +
                  (i < silkit.size() ? silkit[i] : std::string("(end)")) + "\" vs \"" +
                  (i < local.size() ? local[i] : std::string("(end)")) + "\"");
}

bool contains(const std::vector<std::string>& lines, const std::string& line)
{
    for (const auto& l : lines)
    {
        if (l == line)
        {
            return true;
        }
    }
    return false;
}

TEST_CASE(simulation_runs_the_same_lin_ecus_locally_and_on_silkit)
{
    const auto local = run(std::nullopt);
    const auto silkit = run(SilKitOptions{});

    // The scenario happened (on the local buses; SIL Kit must match).
    // kShort: a response of the wrong length is an error. kClash: slave 0 answering the master's
    // frame collides with it, an error.
    for (const char* line : {"result t=0 id=16 ok data=0000", "result t=10 id=32 ok data=0000", "result t=20 id=33 ok data=5A",
                             "result t=30 id=34 error data=", "result t=40 id=35 no_response data=",
                             "result t=50 id=36 error data=", "result t=60 id=37 error data=",
                             "result t=160 id=60 ok data=0A01020304050607", "result t=170 id=61 ok data=0A09080706050403",
                             "result t=80 id=61 no_response data=", "state t=200 sleep", "state t=305 operational",
                             "result t=405 id=16 ok data=9501", "state t=505 sleep", "state t=550 operational",
                             "result t=650 id=16 ok data=8A02"})
    {
        if (!contains(local.body, line))
        {
            throw Failure(std::string("local body lacks \"") + line + "\"");
        }
    }
    CHECK(contains(local.door, "frame t=160 rx=164 id=60 data=0A01020304050607"));
    CHECK(contains(local.sensor, "state t=302 operational"));
    CHECK(contains(local.door, "state t=302 operational"));

    require_same(silkit.body, local.body, "master");
    require_same(silkit.door, local.door, "slave 0");
    require_same(silkit.sensor, local.sensor, "slave 1");
}

TEST_CASE(simulation_refuses_a_network_name_of_two_kinds_locally_and_on_silkit)
{
    // A name is a CAN or a LIN network for all the ECUs of the simulation (on SIL Kit: of this
    // process), in both directions.
    for (const bool local : {true, false})
    {
        Simulation sim(local ? std::optional<SilKitOptions>{} : std::optional<SilKitOptions>{SilKitOptions{}});
        Ecu& a = sim.add_ecu("A", 5ms);
        Ecu& b = sim.add_ecu("B", 5ms);
        a.j1939("CAN1");
        static_cast<void>(a.lin_slave("LIN1"));
        const lin::LinScheduleTable table{{kCommand, 1, lin::LinChecksumModel::Enhanced, 10ms, lin::LinPublisher::Master}};
        CHECK(throws_with<std::invalid_argument>([&] { static_cast<void>(b.lin_master("CAN1", table)); }, "\"CAN1\""));
        CHECK(throws_with<std::invalid_argument>([&] { static_cast<void>(b.lin_slave("CAN1")); }, "CAN"));
        CHECK(throws_with<std::invalid_argument>([&] { b.j1939("LIN1"); }, "\"LIN1\""));
        CHECK(throws_with<std::invalid_argument>([&] { static_cast<void>(b.can("LIN1")); }, "LIN"));
        if (!local)
        {
            CHECK(throws_with<std::invalid_argument>([&] { b.j1939("LIN1"); }, "of the ECU \"A\""));
        }
        // The same kind is fine: both ECUs on one CAN and one LIN network.
        b.j1939("CAN1");
        static_cast<void>(b.lin_master("LIN1", table));
    }
}

} // namespace
