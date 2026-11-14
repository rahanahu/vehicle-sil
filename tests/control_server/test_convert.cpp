// SPDX-License-Identifier: MIT
//
// The conversions of the Control API messages: what a client may send is read safely.
#include "test_harness.hpp"

#include "convert.hpp"

namespace {
namespace v1 = vehicle_sil::control::v1;
using namespace vehicle_sil;
using namespace vehicle_sil::control;
using namespace vehicle_sil::control_grpc::detail;
} // namespace

TEST_CASE(control_server_an_unknown_late_policy_rejects)
{
    // An unset or unknown policy from a client is the safe one.
    CHECK(late_of(v1::LATE_POLICY_UNSPECIFIED) == LatePolicy::Reject);
    CHECK(late_of(static_cast<v1::LatePolicy>(42)) == LatePolicy::Reject);
    CHECK(late_of(v1::LATE_POLICY_APPLY_NEXT) == LatePolicy::ApplyNext);
}

TEST_CASE(control_server_time_state_tells_not_started_from_ended)
{
    control_grpc::TimeView before;
    before.available = true;
    v1::TimeState wire;
    time_to_proto(before, wire);
    CHECK(wire.available() && !wire.running() && !wire.ended());
    control_grpc::TimeView over = before;
    over.now = SimTime{} + std::chrono::milliseconds(30);
    over.pause_target = SimTime{} + std::chrono::milliseconds(40);
    over.speed = 2.0;
    over.ended = true;
    time_to_proto(over, wire);
    CHECK(!wire.running() && wire.ended());
    CHECK(wire.now_ns() == 30'000'000 && wire.pause_target_ns() == 40'000'000 && wire.speed() == 2.0);
}

TEST_CASE(control_server_peer_info_tells_whether_it_has_an_agent)
{
    PeerInfo engine;
    engine.participant = "Engine";
    engine.incarnation = "1";
    engine.connected = true;
    control_grpc::SimulationSnapshot snapshot;
    snapshot.participants = {{"Engine", "Running"}, {"Body", "ReadyToRun"}};
    v1::PeerInfo wire;
    to_proto(engine, wire); // a peer of the hub has an agent
    CHECK(wire.has_agent() && wire.simulation_state().empty());
    wire.Clear();
    peer_to_proto("Engine", &engine, &snapshot, wire);
    CHECK(wire.has_agent() && wire.connected() && wire.simulation_state() == "Running");
    wire.Clear();
    peer_to_proto("Body", nullptr, &snapshot, wire);
    CHECK(!wire.has_agent() && !wire.connected() && wire.participant() == "Body" && wire.simulation_state() == "ReadyToRun");
    wire.Clear();
    peer_to_proto("Engine", &engine, nullptr, wire); // no simulation coordinated
    CHECK(wire.has_agent() && wire.simulation_state().empty());
}

TEST_CASE(control_server_peer_info_names_the_source_of_a_mapped_variable)
{
    // The hub's seen schema, and for a mapped variable the executable's own name of it.
    PeerInfo engine;
    engine.participant = "Engine";
    VariableInfo speed;
    speed.name = "engine.speed";
    speed.unit = "rpm";
    speed.max = 8000.0;
    VariableInfo lamp;
    lamp.name = "lamp";
    engine.schema = {speed, lamp};
    engine.sources = {"rpm_x10", ""};
    v1::PeerInfo wire;
    to_proto(engine, wire);
    CHECK_EQ(wire.schema_size(), 2);
    CHECK(wire.schema(0).name() == "engine.speed" && wire.schema(0).source() == "rpm_x10" && wire.schema(0).max() == 8000.0);
    CHECK(wire.schema(1).name() == "lamp" && wire.schema(1).source().empty());
    // Without a map the hub gives no sources: none is named.
    engine.sources.clear();
    wire.Clear();
    to_proto(engine, wire);
    CHECK(wire.schema(0).source().empty() && wire.schema(1).source().empty());
}

TEST_CASE(control_server_write_request_round_trips)
{
    OperationRequest request;
    request.writes = {{"Engine", "engine.level", std::int64_t{7}}, {"Body", "cabin.lamp", true}};
    request.at = SimTime{} + std::chrono::milliseconds(105);
    request.late = LatePolicy::ApplyNext;
    v1::WriteRequest wire;
    to_proto(request, wire);
    OperationRequest back;
    CHECK(from_proto(wire, back));
    CHECK(back.writes.size() == 2 && back.writes[1].peer == "Body" && back.writes[1].name == "cabin.lamp");
    CHECK(std::get<std::int64_t>(back.writes[0].value) == 7 && std::get<bool>(back.writes[1].value));
    CHECK(back.at == request.at && back.late == LatePolicy::ApplyNext);
    wire.mutable_writes(0)->clear_value(); // a write without a value
    OperationRequest refused;
    CHECK(!from_proto(wire, refused));
}

TEST_CASE(control_convert_names_both_reasons_when_the_simulation_cannot_be_read)
{
    control_grpc::SimulationSnapshot s;
    s.participants_error = "the registry is gone";
    s.time_error = "the controller stopped";
    v1::SimulationState out;
    to_proto(&s, out);
    CHECK(out.error() == "cannot read the participants' states: the registry is gone; cannot read the time: the controller stopped");
    s.participants_error.clear();
    to_proto(&s, out);
    CHECK(out.error() == "cannot read the time: the controller stopped");
}
