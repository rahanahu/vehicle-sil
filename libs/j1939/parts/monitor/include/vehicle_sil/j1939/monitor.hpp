// SPDX-License-Identifier: MIT
//
// A protocol part of the J1939 stack: the latest message of one parameter group a node
// receives, and whether its sender went silent. Built on the node's public API and the toolbox
// only.
#pragma once

#include "vehicle_sil/j1939/name.hpp"
#include "vehicle_sil/j1939/toolbox.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace vehicle_sil::j1939 {

enum class MonitorState : std::uint8_t
{
    Waiting,   ///< No message yet.
    Receiving, ///< The last message came less than `timeout` ago (or there is no timeout).
    Lost,      ///< No message for `timeout` since the last one.
};

/// A change of a Monitor's state, passed to MonitorOptions::on_state.
struct MonitorChange
{
    MonitorState state;
    MonitorState previous;
    /// Receiving: the message's time. Lost: the deadline (the last message's time + timeout).
    SimTime time;
};

struct MonitorOptions
{
    /// Only messages from this address. Empty: from any address (unless `name` is set).
    std::optional<Address> source{};
    /// Only messages from the control function with this NAME, from the address it has at the
    /// time of the message (looked up in the network's address table and its own nodes).
    std::optional<Name> name{};
    /// Lost after this long without a message, counted from the last one (from the first one
    /// on: before it the monitor is Waiting). Empty: never Lost.
    std::optional<SimDuration> timeout{};
    /// The bytes kept of the latest message (allocated when the monitor is created); a longer
    /// message is kept cut to this length (length() has its full length).
    std::size_t max_length = 8;
    /// Called for each accepted message, after the monitor took it.
    std::function<void(const Message&)> on_message{};
    /// Called after each change of state().
    std::function<void(const MonitorChange&)> on_state{};
};

/// Watches one parameter group a node receives:
///
///     Monitor eec1(brake, kPgnEec1, {.name = kEngineName, .timeout = 100ms});
///     ...
///     if (eec1.state() == MonitorState::Receiving) { use(eec1.data()); }
///
/// It keeps the latest accepted message (data, time, source) and counts the messages and the
/// losses. Timeouts count in the messages' times (event time), so the results do not depend on
/// the step size; a message exactly `timeout` after the one before still counts as in time.
/// Messages are those the node receives: sent to its address or to all nodes.
///
/// Read only: it sends nothing. Uses one Timer of the network if `timeout` is set. Create it
/// with node.attach<Monitor>(pgn, options) or owned by the application (destroy it before the
/// network). The state is read on the thread that steps the network.
class Monitor
{
public:
    /// std::invalid_argument for an invalid PGN, both `source` and `name`, timeout <= 0 or
    /// max_length 0; std::length_error if the network has no timer left.
    Monitor(Node& node, Pgn pgn, MonitorOptions options = {});
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    [[nodiscard]] MonitorState state() const noexcept { return state_; }
    /// The latest message's data (up to max_length bytes); empty before the first.
    [[nodiscard]] std::span<const std::uint8_t> data() const noexcept;
    /// The latest message's full length.
    [[nodiscard]] std::size_t length() const noexcept { return length_; }
    /// The latest message's time; empty before the first.
    [[nodiscard]] std::optional<SimTime> time() const noexcept { return time_; }
    /// The latest message's source address (kNullAddress before the first).
    [[nodiscard]] Address source() const noexcept { return source_; }
    /// Accepted messages so far.
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    /// Times the monitor went Lost.
    [[nodiscard]] std::uint64_t losses() const noexcept { return losses_; }
    [[nodiscard]] Pgn pgn() const noexcept { return pgn_; }

private:
    [[nodiscard]] bool accepts(Address source) const noexcept;
    void on_message(const Message& message);
    void on_timeout();
    void change(MonitorState state, SimTime time);

    Node& node_;
    Pgn pgn_;
    MonitorOptions options_;
    std::vector<std::uint8_t> buffer_;
    std::size_t length_ = 0;
    std::optional<SimTime> time_;
    Address source_ = kNullAddress;
    std::uint64_t count_ = 0;
    std::uint64_t losses_ = 0;
    MonitorState state_ = MonitorState::Waiting;
    std::optional<Timer> timeout_;
    Subscription messages_;
};

} // namespace vehicle_sil::j1939
