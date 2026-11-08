// SPDX-License-Identifier: MIT
//
// Small builders of fault injectors (vehicle_sil::can::FaultInjector) for the virtual bus
// (vehicle_sil::testkit::faults).
#pragma once

#include "vehicle_sil/can/virtual_can_bus.hpp"
#include "vehicle_sil/j1939/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

/// Fault injectors for VirtualCanBus::Endpoint::set_fault_injector(). They only decide what
/// happens to each frame an endpoint sends: the virtual bus does the rest. Each is
/// deterministic: its only state is how many matching frames it has seen. Copies of one
/// injector (nth(), sequence()) share that count: make one per endpoint.
namespace vehicle_sil::testkit::faults {

/// Selects frames. An empty predicate selects every frame.
using FramePredicate = std::function<bool(const can::TimedFrame&)>;

/// 29-bit frames whose J1939 PGN is `pgn` (PDU1: the PGN without the destination).
[[nodiscard]] FramePredicate j1939_pgn(j1939::Pgn pgn);
/// 29-bit frames from J1939 source address `source`.
[[nodiscard]] FramePredicate j1939_source(j1939::Address source);
/// Frames with this identifier and format.
[[nodiscard]] FramePredicate can_id(std::uint32_t id, can::IdFormat format);

/// `action` for the frames `predicate` selects, Deliver for the others.
[[nodiscard]] can::FaultInjector when(FramePredicate predicate, can::FaultAction action);
/// `action` for the selected frames sent in [from, until) (by TimedFrame::time).
[[nodiscard]] can::FaultInjector between(SimTime from, SimTime until, can::FaultAction action,
                                         FramePredicate predicate = {});
/// `action` for the n-th selected frame only (1 = the first).
[[nodiscard]] can::FaultInjector nth(std::size_t n, can::FaultAction action, FramePredicate predicate = {});
/// The actions in order for the selected frames, then Deliver.
[[nodiscard]] can::FaultInjector sequence(std::vector<can::FaultAction> actions, FramePredicate predicate = {});
/// Asks the injectors in order and returns the first action that is not Deliver (the ones
/// after it are not asked for that frame).
[[nodiscard]] can::FaultInjector first_of(std::vector<can::FaultInjector> injectors);

} // namespace vehicle_sil::testkit::faults
