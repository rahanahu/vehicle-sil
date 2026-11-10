// SPDX-License-Identifier: MIT
//
// Conversions between the control types and the Control API v1 messages (internal).
#pragma once

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_grpc/simulation_monitor.hpp"
#include "vehicle_sil/control/v1/control.pb.h"

#include <chrono>
#include <cstdint>
#include <optional>

namespace vehicle_sil::control_grpc::detail {

namespace v1 = vehicle_sil::control::v1;
using namespace vehicle_sil::control;

inline std::int64_t ns_of(SimTime t)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

inline SimTime time_of(std::int64_t ns)
{
    return SimTime{} + std::chrono::nanoseconds(ns);
}

inline void to_proto(const Value& in, v1::Value& out)
{
    if (const auto* b = std::get_if<bool>(&in))
    {
        out.set_b(*b);
    }
    else if (const auto* i = std::get_if<std::int64_t>(&in))
    {
        out.set_i(*i);
    }
    else
    {
        out.set_d(std::get<double>(in));
    }
}

/// @returns false for an empty value.
inline bool from_proto(const v1::Value& in, Value& out)
{
    switch (in.kind_case())
    {
    case v1::Value::kB:
        out = in.b();
        return true;
    case v1::Value::kI:
        out = static_cast<std::int64_t>(in.i());
        return true;
    case v1::Value::kD:
        out = in.d();
        return true;
    default:
        return false;
    }
}

// The wire enums follow the C++ enums (shifted by the UNSPECIFIED value 0 where noted).
static_assert(static_cast<int>(TargetStatus::Unknown) + 1 == v1::TARGET_STATUS_UNKNOWN);
static_assert(static_cast<int>(TargetStatus::Sent) + 1 == v1::TARGET_STATUS_SENT);
static_assert(static_cast<int>(CommandStatus::Cancelled) + 1 == v1::COMMAND_STATUS_CANCELLED);
static_assert(static_cast<int>(CommandStatus::Applied) + 1 == v1::COMMAND_STATUS_APPLIED);
static_assert(static_cast<int>(ValueType::Enum) + 1 == v1::VALUE_TYPE_ENUM);
static_assert(static_cast<int>(Causality::Parameter) + 1 == v1::CAUSALITY_PARAMETER);
static_assert(static_cast<int>(EventLevel::Error) + 1 == v1::EVENT_LEVEL_ERROR);

inline void to_proto(const VariableInfo& in, v1::VariableInfo& out)
{
    out.set_name(in.name);
    out.set_type(static_cast<v1::ValueType>(static_cast<int>(in.type) + 1));
    out.set_causality(static_cast<v1::Causality>(static_cast<int>(in.causality) + 1));
    out.set_unit(in.unit);
    if (in.min)
    {
        out.set_min(*in.min);
    }
    if (in.max)
    {
        out.set_max(*in.max);
    }
    if (in.step)
    {
        out.set_step(*in.step);
    }
    out.set_description(in.description);
    for (const auto& item : in.items)
    {
        auto* e = out.add_items();
        e->set_name(item.name);
        e->set_value(item.value);
    }
}

inline VariableInfo from_proto(const v1::VariableInfo& in)
{
    VariableInfo out;
    out.name = in.name();
    // Unknown values (from a newer peer) fall back to the safest meaning.
    const int type = static_cast<int>(in.type());
    out.type = type >= v1::VALUE_TYPE_BOOL && type <= v1::VALUE_TYPE_ENUM ? static_cast<ValueType>(type - 1) : ValueType::Real;
    const int causality = static_cast<int>(in.causality());
    out.causality = causality >= v1::CAUSALITY_INPUT && causality <= v1::CAUSALITY_PARAMETER
                        ? static_cast<Causality>(causality - 1)
                        : Causality::Output;
    out.unit = in.unit();
    if (in.has_min())
    {
        out.min = in.min();
    }
    if (in.has_max())
    {
        out.max = in.max();
    }
    if (in.has_step())
    {
        out.step = in.step();
    }
    out.description = in.description();
    for (const auto& item : in.items())
    {
        out.items.push_back({item.name(), item.value()});
    }
    return out;
}

inline void to_proto(const Snapshot& in, v1::Snapshot& out)
{
    out.set_time_ns(ns_of(in.time));
    out.set_sequence(in.sequence);
    for (const auto& v : in.values)
    {
        to_proto(v, *out.add_values());
    }
}

inline Snapshot from_proto(const v1::Snapshot& in)
{
    Snapshot out;
    out.time = time_of(in.time_ns());
    out.sequence = in.sequence();
    for (const auto& v : in.values())
    {
        Value value;
        if (!from_proto(v, value))
        {
            value = false;
        }
        out.values.push_back(value);
    }
    return out;
}

inline void to_proto(const Event& in, v1::Event& out)
{
    out.set_time_ns(ns_of(in.time));
    out.set_level(static_cast<v1::EventLevel>(static_cast<int>(in.level) + 1));
    out.set_text(in.text);
}

inline Event from_proto(const v1::Event& in)
{
    Event out;
    out.time = time_of(in.time_ns());
    const int level = static_cast<int>(in.level());
    out.level = level >= v1::EVENT_LEVEL_INFO && level <= v1::EVENT_LEVEL_ERROR ? static_cast<EventLevel>(level - 1) : EventLevel::Info;
    out.text = in.text();
    return out;
}

inline void to_proto(const Command& in, v1::Command& out)
{
    out.set_seq(in.seq);
    for (const auto& w : in.writes)
    {
        auto* pw = out.add_writes();
        pw->set_name(w.name);
        to_proto(w.value, *pw->mutable_value());
    }
    if (in.at)
    {
        out.set_at_ns(ns_of(*in.at));
    }
    out.set_late(in.late == LatePolicy::ApplyNext ? v1::LATE_POLICY_APPLY_NEXT : v1::LATE_POLICY_REJECT);
}

inline LatePolicy late_of(v1::LatePolicy p)
{
    return p == v1::LATE_POLICY_APPLY_NEXT ? LatePolicy::ApplyNext : LatePolicy::Reject;
}

/// @returns false if a value is empty.
inline bool from_proto(const v1::Command& in, Command& out)
{
    out.seq = in.seq();
    for (const auto& w : in.writes())
    {
        Value value;
        if (!from_proto(w.value(), value))
        {
            return false;
        }
        out.writes.push_back({w.name(), value});
    }
    if (in.has_at_ns())
    {
        out.at = time_of(in.at_ns());
    }
    out.late = late_of(in.late());
    return true;
}

inline void to_proto(const CommandResult& in, v1::CommandResult& out)
{
    out.set_seq(in.seq);
    out.set_status(static_cast<v1::CommandStatus>(static_cast<int>(in.status) + 1));
    if (in.applied_at)
    {
        out.set_applied_at_ns(ns_of(*in.applied_at));
    }
    out.set_late(in.late);
    out.set_reason(in.reason);
}

/// @returns nullopt for a status this side does not know (UNSPECIFIED, or from a newer peer):
/// it must not be taken for any outcome.
inline std::optional<CommandResult> from_proto(const v1::CommandResult& in)
{
    CommandResult out;
    out.seq = in.seq();
    const int status = static_cast<int>(in.status());
    if (status < v1::COMMAND_STATUS_APPLIED || status > v1::COMMAND_STATUS_CANCELLED)
    {
        return std::nullopt;
    }
    out.status = static_cast<CommandStatus>(status - 1);
    if (in.has_applied_at_ns())
    {
        out.applied_at = time_of(in.applied_at_ns());
    }
    out.late = in.late();
    out.reason = in.reason();
    return out;
}

/// A peer of the hub: it has (or had) an agent.
inline void to_proto(const PeerInfo& in, v1::PeerInfo& out)
{
    out.set_participant(in.participant);
    out.set_has_agent(true);
    out.set_incarnation(in.incarnation);
    out.set_connected(in.connected);
    for (const auto& v : in.schema)
    {
        to_proto(v, *out.add_schema());
    }
    out.mutable_status()->set_failed(in.status.failed);
    out.mutable_status()->set_error(in.status.error);
}

inline void to_proto(const OperationRequest& in, v1::WriteRequest& out)
{
    for (const auto& w : in.writes)
    {
        auto* pw = out.add_writes();
        pw->set_peer(w.peer);
        pw->set_name(w.name);
        to_proto(w.value, *pw->mutable_value());
    }
    if (in.at)
    {
        out.set_at_ns(ns_of(*in.at));
    }
    out.set_late(in.late == LatePolicy::ApplyNext ? v1::LATE_POLICY_APPLY_NEXT : v1::LATE_POLICY_REJECT);
}

/// A client's Write (without the client's name). @returns false if a write has no value.
inline bool from_proto(const v1::WriteRequest& in, OperationRequest& out)
{
    for (const auto& w : in.writes())
    {
        Value value;
        if (!from_proto(w.value(), value))
        {
            return false;
        }
        out.writes.push_back({w.peer(), w.name(), value});
    }
    if (in.has_at_ns())
    {
        out.at = time_of(in.at_ns());
    }
    out.late = late_of(in.late());
    return true;
}

inline void to_proto(const OperationResult& in, v1::Operation& out)
{
    out.set_seq(in.seq);
    to_proto(in.request, *out.mutable_request());
    out.set_client(in.request.client);
    for (const auto& t : in.targets)
    {
        auto* pt = out.add_targets();
        pt->set_peer(t.peer);
        pt->set_status(static_cast<v1::TargetStatus>(static_cast<int>(t.status) + 1));
        if (t.applied_at)
        {
            pt->set_applied_at_ns(ns_of(*t.applied_at));
        }
        pt->set_late(t.late);
        pt->set_reason(t.reason);
    }
    out.set_done(in.done());
}

inline void time_to_proto(const control_grpc::TimeView& in, v1::TimeState& out)
{
    out.set_available(in.available);
    out.set_now_ns(ns_of(in.now));
    out.set_paused(in.paused);
    if (in.pause_target)
    {
        out.set_pause_target_ns(ns_of(*in.pause_target));
    }
    if (in.speed)
    {
        out.set_speed(*in.speed);
    }
    out.set_running(in.running);
    out.set_ended(in.ended);
}

/// A peer as ListPeers and Watch report it: the hub's peer (`agent`, if it has one) with its SIL
/// Kit state from the monitor's snapshot (`simulation`, nullptr without one).
inline void peer_to_proto(const std::string& participant, const PeerInfo* agent, const control_grpc::SimulationSnapshot* simulation,
                          v1::PeerInfo& out)
{
    if (agent != nullptr)
    {
        to_proto(*agent, out);
    }
    else
    {
        out.set_participant(participant);
        out.set_has_agent(false);
    }
    out.set_simulation_state(simulation != nullptr ? simulation->state_of(participant) : std::string());
}

/// The simulation part of a monitor's snapshot; `in` nullptr: the server does not coordinate a
/// simulation.
inline void to_proto(const control_grpc::SimulationSnapshot* in, v1::SimulationState& out)
{
    out.set_available(in != nullptr);
    if (in == nullptr)
    {
        return;
    }
    for (const auto& [name, state] : in->participants)
    {
        auto* p = out.add_participants();
        p->set_participant(name);
        p->set_state(state);
    }
    if (!in->participants_error.empty())
    {
        out.set_error("cannot read the participants' states: " + in->participants_error);
    }
    else if (!in->time_error.empty())
    {
        out.set_error("cannot read the time: " + in->time_error);
    }
    if (in->failure)
    {
        out.set_failed_participant(in->failure->participant);
        out.set_failed_state(in->failure->state);
        out.set_failure_reason(in->failure->reason);
    }
    out.set_ended(in->ended);
}

} // namespace vehicle_sil::control_grpc::detail
