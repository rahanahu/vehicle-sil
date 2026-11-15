// SPDX-License-Identifier: MIT
//
// What went over the networks of a run (RunTraffic, the result's "traffic"): per network, the
// participants known to be on it with their J1939 counters, and those the launcher cannot place.
// Part of the run's result (run.hpp). Internal; the output: docs/simulation-run.md.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::launch {

/// Why a participant's frames on a network were not counted (TrafficParticipant).
enum class TrafficNotCounted
{
    None,          ///< counted
    NoControlLink, ///< no control agent of it said hello: nothing came from it
    Restarted,     ///< more than one process ran under its name: its counters began again
    RealTime,      ///< it does not run in virtual time (or its samples are far ahead of the run's time)
    NoCounters,    ///< its control agent has no J1939 counters for the network (another use of it)
    Mapped,        ///< the entry's `variables` map the counters: their values are not the executable's
    NoSample,      ///< no sample of a step before the end came
};
/// "no_control_link", "restarted", "real_time", "no_counters", "mapped", "no_sample" (the
/// result's "not_counted"); None: "".
[[nodiscard]] const char* traffic_not_counted_name(TrafficNotCounted why);

/// One participant on one network: the J1939 frames it had sent and received there, as its last
/// step that began before the run's end said (RunTraffic).
struct TrafficParticipant
{
    std::string name;
    TrafficNotCounted not_counted = TrafficNotCounted::NoControlLink;
    /// Whether it runs in virtual time; empty if no control agent of it said hello.
    std::optional<bool> virtual_time{};
    /// Counted only: the start of the step the numbers are of, and the numbers. `frames_sent`:
    /// the frames the port had accepted before that step (not: that were on the bus);
    /// `frames_received`: the J1939 frames of other participants taken from the port.
    std::optional<SimTime> at{};
    std::optional<std::int64_t> frames_sent{};
    std::optional<std::int64_t> frames_received{};
    [[nodiscard]] bool counted() const { return not_counted == TrafficNotCounted::None; }
};

/// Why the launcher cannot say whether a participant of the run is on a network (TrafficUnplaced).
enum class TrafficUnplacedReason
{
    /// No control agent of it had said hello when the run ended. It has none (a tool, a bridge,
    /// --no-control), or its hello did not come in this run: the two cannot be told apart.
    NoControlLink,
    /// Its control agent has no J1939 counters for the network (for a network of the
    /// configuration, in RunTraffic::unplaced). The same in every run of the configuration.
    NoCounters,
};
/// "no_control_link", "no_counters" (the result's "reason").
[[nodiscard]] const char* traffic_unplaced_reason_name(TrafficUnplacedReason why);

/// A participant of the run without --describe that may be on a network without the launcher
/// knowing: only J1939 counters place such a participant, and it may use a network in a way
/// that has none (raw CAN, LIN, Ethernet).
struct TrafficUnplaced
{
    std::string name;
    TrafficUnplacedReason reason = TrafficUnplacedReason::NoControlLink;
    [[nodiscard]] bool operator==(const TrafficUnplaced&) const = default;
};

/// One network of the configuration and the participants the launcher knows to be on it.
struct TrafficNetwork
{
    std::string name;
    std::string kind; ///< "can", "lin", "ethernet"
    std::vector<TrafficParticipant> participants{}; ///< in the order of the configuration; may be empty
    /// The participants of the run that may be on it without the launcher knowing, in the order
    /// of the configuration: those without --describe that are not in `participants`. One that
    /// its counters place on another network is here too (it may use this one as raw CAN).
    std::vector<TrafficUnplaced> unplaced{};
    /// How many of `participants` were counted, and how many of those had sent and had received a frame.
    [[nodiscard]] std::size_t counted() const;
    [[nodiscard]] std::size_t senders() const;
    [[nodiscard]] std::size_t receivers() const;
    /// Whether what was sent arrived, as far as the counters say. true: a counted participant
    /// sent, and another counted participant received (a participant's own frames are not in its
    /// frames_received). false: a counted participant sent, no counted participant received,
    /// every participant known to be on the network is counted, and `unplaced` is empty. Empty
    /// (cannot be said): nobody is counted, nobody sent, the one sender is the one receiver (it
    /// heard someone the launcher does not count), or no counted participant received while one
    /// on the network is not counted or `unplaced` is not empty (one of those may have).
    [[nodiscard]] std::optional<bool> delivered() const;
};

/// What went over the networks of a run that started: per network of the configuration, the
/// participants of the run that are known to be on it (a `describe: true` participant: what
/// --describe says, under the simulation's names; another one: the networks its control agent has
/// J1939 counters for), each with its counters. A participant's numbers are those of its
/// last step that began before `before_ms`: the samples of the steps from then on come or do not
/// come before the run is over, so they are left out, and a run that ends at the same virtual
/// time with the same steps has the same numbers.
struct RunTraffic
{
    std::int64_t before_ms = 0; ///< the run's end (RunTiming::sim_ms)
    std::vector<TrafficNetwork> per_network{}; ///< in the order of the configuration
    /// The participants of the run that the launcher knows on no network (`describe: false`, and
    /// no control agent with J1939 counters for a network of the configuration: a bridge, a
    /// tool), in the order of the configuration, each with the reason. They are in the
    /// `unplaced` of every network.
    std::vector<TrafficUnplaced> unplaced{};
};

} // namespace vehicle_sil::launch
