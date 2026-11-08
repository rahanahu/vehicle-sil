// SPDX-License-Identifier: MIT
//
// A protocol of one's own as its developer tests it: the parameter service of
// examples/j1939/param_service (built on the Exchange part and typed messages) between an engine
// ECU and service tools on the local virtual bus. The normal flow, and what goes wrong on a real
// vehicle: an expired session, a second tool, a silent ECU, an ECU or a tool that changes its
// address or loses it, refusals, and parts destroyed while they work. Everything in virtual time.

#include "param_service.hpp"
#include "test_harness.hpp"
#include "vehicle_sil/testkit/local_can.hpp"
#include "vehicle_sil/testkit/recorders.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <vector>

using namespace std::chrono_literals;
using namespace param_service;
using vehicle_sil::SimTime;
using vehicle_sil::can::TapRecord;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Node;
using vehicle_sil::testkit::FrameLog;
using vehicle_sil::testkit::LocalCan;

namespace {

const Name kEngineName = Name{}.set_identity_number(1).set_manufacturer_code(0x7F0).set_function(0);
const Name kMovingEngineName = Name{kEngineName}.set_arbitrary_address_capable(true);
const Name kToolName = Name{}.set_identity_number(2).set_manufacturer_code(0x7F0).set_function(129);
const Name kMovingToolName = Name{kToolName}.set_arbitrary_address_capable(true);
const Name kSecondToolName = Name{}.set_identity_number(3).set_manufacturer_code(0x7F0).set_function(129);
/// Lower than every NAME above: wins any address it claims.
const Name kRivalName = Name{}.set_identity_number(9).set_manufacturer_code(1);

constexpr std::uint16_t kIdle = 0x0100;  // writable, 500 .. 1200
constexpr std::uint16_t kRated = 0x0101; // read only


struct Setup
{
    Name engine_name = kEngineName;
    vehicle_sil::j1939::Address engine_address = 0x00;
    Name tool_name = kToolName;
    vehicle_sil::j1939::Address tool_address = 0xF9;
    ParamClientOptions client{};
};

/// An engine ECU with the service and a tool with a client, on two networks of one bus.
struct Workshop
{
    explicit Workshop(Setup setup = {})
      : engine(bus.add_j1939("engine", 10ms).add_node({.address = setup.engine_address, .name = setup.engine_name})),
        tool(bus.add_j1939("tool", 10ms).add_node({.address = setup.tool_address, .name = setup.tool_name})),
        engine_name(setup.engine_name)
    {
        table.parameters = {{kIdle, Parameter{.value = 750, .writable = true, .min = 500, .max = 1200}},
                            {kRated, Parameter{.value = 2200, .writable = false}}};
        table.blocks[1].resize(64);
        std::iota(table.blocks[1].begin(), table.blocks[1].end(), std::uint8_t{0});
        server = std::make_unique<ParamServer>(engine, table);
        client = std::make_unique<ParamClient>(tool, engine_name, setup.client);
    }

    /// An Address Claimed of kRivalName for `address`, injected at `time`: the node there loses it.
    void rival_claims(vehicle_sil::j1939::Address address, SimTime time)
    {
        vehicle_sil::can::Frame f;
        f.id = vehicle_sil::j1939::Identifier{6, vehicle_sil::j1939::kPgnAddressClaimed, vehicle_sil::j1939::kGlobalAddress,
                                              address}
                   .encode();
        f.length = 8;
        const auto bytes = kRivalName.to_bytes();
        std::copy(bytes.begin(), bytes.end(), f.data.begin());
        bus.inject(time, f);
    }

    ParameterTable table;
    LocalCan bus;
    Node& engine;
    Node& tool;
    Name engine_name;
    // Destroyed before the bus (declared after it): they may also be destroyed earlier.
    std::unique_ptr<ParamServer> server;
    std::unique_ptr<ParamClient> client;
};

/// What an operation's callback got, and when.
struct Answer
{
    std::optional<ParamStatus> status;
    std::uint32_t value = 0;
    std::vector<std::uint8_t> data;
    SimTime time{};
};

/// The callbacks record the time of the step they run in (the tool's network's).
ParamClient::ValueDone value_into(Answer& a, LocalCan& bus)
{
    return [&a, &bus](ParamStatus s, std::uint32_t v) { a = Answer{s, v, {}, *bus.network("tool").now()}; };
}
ParamClient::Done status_into(Answer& a, LocalCan& bus)
{
    return [&a, &bus](ParamStatus s) { a = Answer{s, 0, {}, *bus.network("tool").now()}; };
}

std::size_t frames_from_engine(const FrameLog& log, std::uint8_t operation)
{
    return log.count(FrameLog::Direction::Sent, [operation](const TapRecord& r) {
        const auto id = vehicle_sil::j1939::Identifier::decode(r.frame);
        return id && id->pgn == kPgn && r.frame.length > 0 && r.frame.data[0] == operation;
    });
}

} // namespace

TEST_CASE(the_service_reads_writes_and_closes_as_its_user_expects)
{
    Workshop w;
    FrameLog engineFrames(w.bus.tap("engine"));
    Answer read1, write1, read2, readOnly, outOfRange, block1, block2, closed;
    std::vector<std::uint8_t> blockData;
    w.client->read(kIdle, value_into(read1, w.bus));
    const SimTime writeQueued = w.bus.now();
    w.client->write(kIdle, 900, status_into(write1, w.bus));
    w.client->read(kIdle, value_into(read2, w.bus));
    w.client->write(kRated, 2500, status_into(readOnly, w.bus));
    w.client->write(kIdle, 5000, status_into(outOfRange, w.bus));
    w.client->read_block(1, [&](ParamStatus s, std::span<const std::uint8_t> d) {
        block1.status = s;
        blockData.assign(d.begin(), d.end());
    });
    const std::vector<std::uint8_t> curve(40, 0x5A);
    w.client->write_block(2, curve, status_into(block2, w.bus));
    w.client->close(status_into(closed, w.bus));
    CHECK_EQ(w.client->queued(), 8u);
    w.bus.run_for(3s);

    CHECK(read1.status == ParamStatus::Ok && read1.value == 750u);
    CHECK(write1.status == ParamStatus::Ok);
    CHECK(write1.time - writeQueued >= 300ms); // the write takes 300 ms; Pending keeps the tool waiting
    CHECK(frames_from_engine(engineFrames, 0x7F) >= 3u);
    CHECK(read2.status == ParamStatus::Ok && read2.value == 900u);
    CHECK(readOnly.status == ParamStatus::ReadOnly);
    CHECK(outOfRange.status == ParamStatus::OutOfRange);
    CHECK(block1.status == ParamStatus::Ok && blockData == w.table.blocks[1]);
    CHECK(block2.status == ParamStatus::Ok && w.table.blocks[2] == curve);
    CHECK(closed.status == ParamStatus::Ok);
    CHECK(!w.server->in_session() && !w.client->in_session());
    CHECK_EQ(w.client->queued(), 0u);
    CHECK_EQ(w.server->statistics().sessions, 1u);
    CHECK_EQ(w.server->statistics().errors, 2u);
    CHECK_EQ(w.table.parameters[kIdle].value, 900u);
    CHECK_EQ(w.client->statistics().sent, 9u); // Open, then one command per operation
    CHECK(!w.bus.network("engine").failed() && !w.bus.network("tool").failed());
}

TEST_CASE(the_same_inputs_give_the_same_frames)
{
    const auto run = [] {
        Workshop w;
        FrameLog frames(w.bus.tap("tool"));
        Answer a, b, c;
        w.client->write(kIdle, 800, status_into(a, w.bus));
        w.client->read_block(1, [&](ParamStatus s, std::span<const std::uint8_t>) { b.status = s; });
        w.client->read(kRated, value_into(c, w.bus));
        w.bus.run_for(2s);
        std::vector<std::pair<SimTime, vehicle_sil::can::Frame>> out;
        for (const auto& r : frames.all())
        {
            out.emplace_back(r.time, r.frame);
        }
        return out;
    };
    const auto first = run();
    CHECK(first.size() > 20u);
    CHECK(first == run());
}

TEST_CASE(an_expired_session_is_opened_again_without_the_user_noticing)
{
    Workshop w;
    Answer first, second;
    w.client->read(kIdle, value_into(first, w.bus));
    w.bus.run_for(500ms);
    CHECK(first.status == ParamStatus::Ok && w.server->in_session());
    w.bus.run_for(2s); // idle_timeout: 2 s without a command
    CHECK(!w.server->in_session());
    CHECK(w.client->in_session()); // the tool does not know yet
    w.client->read(kRated, value_into(second, w.bus));
    w.bus.run_for(500ms);
    CHECK(second.status == ParamStatus::Ok && second.value == 2200u);
    CHECK_EQ(w.server->statistics().expired, 1u);
    CHECK_EQ(w.server->statistics().sessions, 2u);
    CHECK_EQ(w.client->statistics().sent, 5u); // Open, Read, Read (no session), Open, Read
}

TEST_CASE(a_second_tool_is_busy_until_the_first_one_closes)
{
    Workshop w;
    Node& other = w.bus.add_j1939("tool2", 5ms).add_node({.address = 0xFA, .name = kSecondToolName});
    ParamClient second(other, kEngineName);
    Answer a, b, closed, c;
    w.client->read(kIdle, value_into(a, w.bus));
    w.bus.run_for(300ms);
    second.read(kIdle, value_into(b, w.bus));
    w.bus.run_for(300ms);
    CHECK(a.status == ParamStatus::Ok);
    CHECK(b.status == ParamStatus::Busy);
    w.client->close(status_into(closed, w.bus));
    w.bus.run_for(100ms);
    second.read(kIdle, value_into(c, w.bus));
    w.bus.run_for(300ms);
    CHECK(closed.status == ParamStatus::Ok);
    CHECK(c.status == ParamStatus::Ok && c.value == 750u);
    CHECK_EQ(w.server->client(), 0xFA);
}

TEST_CASE(a_silent_ecu_times_out_and_the_next_operation_recovers)
{
    Workshop w;
    Answer a, b, c;
    w.client->read(kIdle, value_into(a, w.bus));
    w.bus.run_for(300ms);
    CHECK(a.status == ParamStatus::Ok);
    // The engine is cut off the bus for 1 s.
    w.bus.endpoint("engine").add_outage(w.bus.now(), w.bus.now() + 1s);
    const SimTime asked = w.bus.now();
    w.client->read(kIdle, value_into(b, w.bus));
    w.bus.run_for(1s);
    CHECK(b.status == ParamStatus::Timeout);
    CHECK(b.time - asked >= 400ms); // two attempts of 200 ms
    w.client->read(kRated, value_into(c, w.bus));
    w.bus.run_for(500ms);
    CHECK(c.status == ParamStatus::Ok && c.value == 2200u);
    // The ECU kept the session (its idle timeout is longer): the tool's Open got it back.
    CHECK_EQ(w.server->statistics().sessions, 1u);
}

TEST_CASE(an_ecu_that_moves_ends_the_operation_and_the_next_one_follows_its_name)
{
    Workshop w(Setup{.engine_name = kMovingEngineName});
    Answer a, b, c;
    w.client->read(kIdle, value_into(a, w.bus));
    w.bus.run_for(600ms); // the engine waits 250 ms after its claim; the tool waits for it
    CHECK(a.status == ParamStatus::Ok);
    w.client->write(kIdle, 1000, status_into(b, w.bus)); // takes 300 ms
    w.bus.run_for(100ms);
    w.rival_claims(0x00, w.bus.now()); // the engine moves to 128
    w.bus.run_for(100ms);
    CHECK(w.engine.address() == 128);
    CHECK(b.status == ParamStatus::ServerLost);
    CHECK(!w.server->in_session());
    CHECK_EQ(w.server->statistics().lost, 1u);
    w.client->read(kIdle, value_into(c, w.bus)); // by its NAME: to 128, once it may send
    w.bus.run_for(1s);
    CHECK(c.status == ParamStatus::Ok && c.value == 750u); // the interrupted write was dropped
}

TEST_CASE(an_ecu_that_loses_its_address_is_lost_to_the_tool)
{
    Workshop w;
    Answer a, b, c;
    w.client->read(kIdle, value_into(a, w.bus));
    w.bus.run_for(300ms);
    w.client->write(kIdle, 1000, status_into(b, w.bus));
    w.bus.run_for(100ms);
    w.rival_claims(0x00, w.bus.now());
    w.bus.run_for(400ms); // the engine sends Cannot Claim after its delay
    CHECK(w.engine.address_state() == AddressState::CannotClaim);
    CHECK(b.status == ParamStatus::ServerLost);
    w.client->read(kIdle, value_into(c, w.bus));
    w.bus.run_for(500ms);
    CHECK(c.status == ParamStatus::ServerLost); // its NAME is not claimed within the timeout
}

TEST_CASE(a_tool_that_moves_gets_a_new_session)
{
    Workshop w(Setup{.tool_name = kMovingToolName, .tool_address = 0x80});
    Answer a, b;
    w.client->read(kIdle, value_into(a, w.bus));
    w.bus.run_for(600ms);
    CHECK(a.status == ParamStatus::Ok);
    w.rival_claims(0x80, w.bus.now()); // the tool moves to 129
    w.bus.run_for(100ms);
    CHECK(!w.server->in_session()); // the ECU saw the tool's NAME leave its address
    CHECK_EQ(w.server->statistics().lost, 1u);
    w.client->read(kRated, value_into(b, w.bus)); // the old session: "no session", then a new one
    w.bus.run_for(1s);
    CHECK(w.tool.address() == 129);
    CHECK(b.status == ParamStatus::Ok && b.value == 2200u);
    CHECK_EQ(w.server->client(), 129);
    CHECK_EQ(w.server->statistics().sessions, 2u);
}

TEST_CASE(a_tool_that_cannot_claim_an_address_cannot_send)
{
    Workshop w;
    Answer a, b;
    w.client->read(kIdle, value_into(a, w.bus));
    w.bus.run_for(300ms);
    w.client->write(kIdle, 1000, status_into(b, w.bus));
    w.bus.run_for(100ms);
    w.rival_claims(0xF9, w.bus.now());
    w.bus.run_for(500ms);
    CHECK(w.tool.address_state() == AddressState::CannotClaim);
    CHECK(b.status == ParamStatus::NotSent);
}

TEST_CASE(refusals_reach_the_user_as_statuses)
{
    Workshop w(Setup{.client = ParamClientOptions{.write_access = false, .queue_capacity = 3}});
    Answer a, b, c;
    CHECK(w.client->write(kIdle, 900, status_into(a, w.bus)));
    CHECK(w.client->read(0x0999, value_into(b, w.bus)));
    CHECK(w.client->read_block(7, [&](ParamStatus s, std::span<const std::uint8_t>) { c.status = s; }));
    CHECK(!w.client->read(kIdle, [](ParamStatus, std::uint32_t) {})); // the queue is full
    const std::vector<std::uint8_t> tooLong(300, 0);
    CHECK(!w.client->write_block(2, tooLong, [](ParamStatus) {}));
    w.bus.run_for(1s);
    CHECK(a.status == ParamStatus::Denied);
    CHECK(b.status == ParamStatus::UnknownParameter);
    CHECK(c.status == ParamStatus::UnknownParameter);
    CHECK_EQ(w.table.parameters[kIdle].value, 750u);
}

TEST_CASE(a_late_answer_to_a_command_sent_again_is_not_taken_for_the_next_one)
{
    // The ECU's ReadOk answers come 250 ms late (the tool waits 200 ms, then sends again). The
    // late answer to the first Read comes while the next Read (another parameter) waits: the
    // answers name their parameter, so it is not taken for the next one's.
    Workshop w;
    Answer a, b, c;
    w.client->read(kIdle, value_into(a, w.bus));
    w.bus.run_for(300ms);
    CHECK(a.status == ParamStatus::Ok);
    w.bus.endpoint("engine").set_fault_injector([](const vehicle_sil::can::TimedFrame& f) {
        return f.frame.length > 0 && f.frame.data[0] == 0x91 ? vehicle_sil::can::FaultAction::delay_by(250ms)
                                                             : vehicle_sil::can::FaultAction::deliver();
    });
    w.client->read(kIdle, value_into(b, w.bus));
    w.client->read(kRated, value_into(c, w.bus));
    w.bus.run_for(1500ms);
    CHECK(b.status == ParamStatus::Ok && b.value == 750u);
    CHECK(c.status == ParamStatus::Ok && c.value == 2200u);
}

// --- lifetime: parts destroyed while they work

TEST_CASE(a_client_destroyed_while_it_works_calls_nothing)
{
    Workshop w;
    int calls = 0;
    w.client->read(kIdle, [&](ParamStatus, std::uint32_t) { ++calls; });
    w.client->write(kIdle, 900, [&](ParamStatus) { ++calls; });
    w.client->read_block(1, [&](ParamStatus, std::span<const std::uint8_t>) { ++calls; });
    w.bus.run_for(30ms); // the Open is under way
    w.client.reset();
    w.bus.run_for(3s);
    CHECK_EQ(calls, 0);
    CHECK_EQ(w.server->statistics().expired, 1u); // the ECU closes the abandoned session itself
    CHECK(!w.bus.network("tool").failed());
}

TEST_CASE(a_client_destroyed_in_its_own_callback_calls_nothing_more)
{
    Workshop w;
    int calls = 0;
    w.client->read(kIdle, [&](ParamStatus, std::uint32_t) {
        ++calls;
        w.client.reset(); // the next operation has already started: it is dropped
    });
    w.client->read(kRated, [&](ParamStatus, std::uint32_t) { ++calls; });
    w.bus.run_for(1s);
    CHECK_EQ(calls, 1);
    CHECK(w.client == nullptr);
    CHECK(!w.bus.network("tool").failed());
}

TEST_CASE(a_server_destroyed_while_it_writes_leaves_the_tool_to_time_out)
{
    Workshop w;
    Answer a, b;
    w.client->read(kIdle, value_into(a, w.bus));
    w.bus.run_for(300ms);
    w.client->write(kIdle, 1000, status_into(b, w.bus));
    w.bus.run_for(100ms);
    w.server.reset(); // its timers (Pending, the write) go with it
    w.bus.run_for(1s);
    CHECK(b.status == ParamStatus::Timeout);
    CHECK_EQ(w.table.parameters[kIdle].value, 750u);
    CHECK(!w.bus.network("engine").failed());
}

TEST_CASE(the_bus_destroyed_during_a_transfer_takes_the_attached_parts_with_it)
{
    int calls = 0;
    {
        ParameterTable table;
        table.blocks[1].assign(200, 0x11);
        LocalCan bus;
        Node& engine = bus.add_j1939("engine", 10ms).add_node({.address = 0x00, .name = kEngineName});
        Node& tool = bus.add_j1939("tool", 10ms).add_node({.address = 0xF9, .name = kToolName});
        engine.attach<ParamServer>(table);
        auto& client = tool.attach<ParamClient>(kEngineName);
        client.read_block(1, [&](ParamStatus, std::span<const std::uint8_t>) { ++calls; });
        bus.run_for(60ms); // the block's transfer has started
    }
    CHECK_EQ(calls, 0);
}
