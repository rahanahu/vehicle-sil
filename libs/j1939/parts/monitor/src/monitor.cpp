// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/monitor.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::j1939 {

namespace {

[[nodiscard]] MonitorOptions checked(Pgn pgn, MonitorOptions options)
{
    if (!pgn.is_valid())
    {
        throw std::invalid_argument("vehicle_sil::j1939::Monitor: invalid PGN");
    }
    if (options.source && options.name)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Monitor: give the sender by address or by NAME, not both");
    }
    if (options.timeout && *options.timeout <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::j1939::Monitor: timeout must be > 0");
    }
    if (options.max_length == 0)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Monitor: max_length must be > 0");
    }
    return options;
}

} // namespace

Monitor::Monitor(Node& node, Pgn pgn, MonitorOptions options)
  : node_(node), pgn_(pgn), options_(checked(pgn, std::move(options))), buffer_(options_.max_length)
{
    if (options_.timeout)
    {
        timeout_.emplace(node, TimerKind::Timeout, [this] { on_timeout(); });
    }
    messages_ = Subscription{node, node.on_message(pgn, [this](const Message& m) { on_message(m); })};
}

std::span<const std::uint8_t> Monitor::data() const noexcept
{
    return {buffer_.data(), std::min(length_, buffer_.size())};
}

bool Monitor::accepts(Address source) const noexcept
{
    if (options_.source)
    {
        return source == *options_.source;
    }
    if (!options_.name)
    {
        return true;
    }
    // Also a node of the same network (not in the address table).
    const auto address = node_.network().address_of(*options_.name);
    return address && source == *address;
}

void Monitor::on_message(const Message& message)
{
    if (!accepts(message.source))
    {
        return;
    }
    if (options_.accept && !options_.accept(message))
    {
        ++rejected_;
        return;
    }
    const std::size_t kept = std::min(message.data.size(), buffer_.size());
    std::copy_n(message.data.begin(), kept, buffer_.begin());
    length_ = message.data.size();
    time_ = message.time;
    source_ = message.source;
    ++count_;
    if (timeout_)
    {
        timeout_->start_after(*options_.timeout); // from the message's time
    }
    if (state_ != MonitorState::Receiving)
    {
        change(MonitorState::Receiving, message.time);
    }
    if (options_.on_message)
    {
        options_.on_message(message);
    }
}

void Monitor::on_timeout()
{
    ++losses_;
    change(MonitorState::Lost, *timeout_->deadline());
}

void Monitor::change(MonitorState state, SimTime time)
{
    const MonitorChange c{state, state_, time};
    state_ = state; // before the callback, which may read it
    if (options_.on_state)
    {
        options_.on_state(c);
    }
}

} // namespace vehicle_sil::j1939
