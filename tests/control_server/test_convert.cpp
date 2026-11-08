// SPDX-License-Identifier: MIT
//
// The conversions of the Control API messages: values a newer peer may send are read safely.
#include "test_harness.hpp"

#include "convert.hpp"

namespace {
namespace v1 = vehicle_sil::control::v1;
using namespace vehicle_sil;
using namespace vehicle_sil::control;
using namespace vehicle_sil::control_grpc::detail;
} // namespace

TEST_CASE(control_server_unknown_enum_values_are_read_safely)
{
    v1::VariableInfo info;
    info.set_name("x");
    info.set_type(static_cast<v1::ValueType>(99));
    info.set_causality(static_cast<v1::Causality>(77));
    const auto v = from_proto(info);
    CHECK(v.type == ValueType::Real);
    CHECK(v.causality == Causality::Output); // unknown: not writable
    v1::CommandResult r;
    r.set_status(static_cast<v1::CommandStatus>(55));
    CHECK(!from_proto(r).has_value()); // unknown: not taken for any outcome
    r.set_status(v1::COMMAND_STATUS_UNSPECIFIED);
    CHECK(!from_proto(r).has_value());
    v1::Event e;
    e.set_level(static_cast<v1::EventLevel>(9));
    CHECK(from_proto(e).level == EventLevel::Info);
    // Round trips.
    CommandResult applied{7, CommandStatus::Applied, SimTime{} + std::chrono::milliseconds(5), false, {}};
    v1::CommandResult wire;
    to_proto(applied, wire);
    CHECK(wire.status() == v1::COMMAND_STATUS_APPLIED);
    CHECK(from_proto(wire)->status == CommandStatus::Applied);
    CHECK(from_proto(wire)->applied_at == applied.applied_at);
    CHECK(late_of(v1::LATE_POLICY_UNSPECIFIED) == LatePolicy::Reject);
}
