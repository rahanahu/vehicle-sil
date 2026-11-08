// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/periodic_sender.hpp"

#include "vehicle_sil/j1939/broadcast_suppression.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::j1939 {

namespace {

[[nodiscard]] PeriodicSenderOptions checked(Pgn pgn, SimDuration period, const PeriodicSenderOptions& options)
{
    if (!pgn.is_valid())
    {
        throw std::invalid_argument("vehicle_sil::j1939::PeriodicSender: invalid PGN");
    }
    if (period <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::j1939::PeriodicSender: period must be > 0");
    }
    if (options.priority > kMaxPriority)
    {
        throw std::invalid_argument("vehicle_sil::j1939::PeriodicSender: priority above 7");
    }
    if (pgn.is_pdu2() ? options.destination != kGlobalAddress : options.destination == kNullAddress)
    {
        throw std::invalid_argument("vehicle_sil::j1939::PeriodicSender: a PDU2 group goes to all nodes, a PDU1 group "
                                    "to kGlobalAddress or one address");
    }
    if (options.max_length == 0 || options.max_length > kMaxTransportLength)
    {
        throw std::invalid_argument("vehicle_sil::j1939::PeriodicSender: max_length must be 1 .. kMaxTransportLength");
    }
    return options;
}

} // namespace

PeriodicSender::PeriodicSender(Node& node, Pgn pgn, SimDuration period, std::span<const std::uint8_t> data,
                               PeriodicSenderOptions options)
  : PeriodicSender(node, pgn, period, options, Encoder{})
{
    set_data(data);
}

PeriodicSender::PeriodicSender(Node& node, Pgn pgn, SimDuration period, Encoder encoder, PeriodicSenderOptions options)
  : PeriodicSender(node, pgn, period, options, std::move(encoder))
{
    if (!encoder_)
    {
        throw std::invalid_argument("vehicle_sil::j1939::PeriodicSender: empty encoder");
    }
}

PeriodicSender::PeriodicSender(Node& node, Pgn pgn, SimDuration period, PeriodicSenderOptions options, Encoder encoder)
  : node_(node),
    pgn_(pgn),
    period_(period),
    options_(checked(pgn, period, options)), // before anything is attached to the node
    encoder_(std::move(encoder)),
    buffer_(options_.max_length),
    suppression_(options_.suppressible ? &BroadcastSuppression::of(node) : nullptr),
    timer_(node, TimerKind::Scheduled, [this] { on_deadline(); })
{
    address_ = Subscription{node, node.on_address_change([this](const AddressChange& change) {
        if (node_.can_send_now() && !paused_)
        {
            // Not again when a node that did not wait after its claim becomes Claimed.
            if (!timer_.running() || change.address != change.previous_address)
            {
                // The address handler runs in the step: the first message leaves in this step.
                send_or_suppress();
                restarted_ = false;
                timer_.start_after(period_);
            }
        }
        else
        {
            timer_.cancel();
        }
    })};
    if (node_.can_send_now())
    {
        restart();
    }
}

void PeriodicSender::set_data(std::span<const std::uint8_t> data)
{
    if (data.size() > buffer_.size())
    {
        throw std::length_error("vehicle_sil::j1939::PeriodicSender: data longer than max_length");
    }
    std::copy(data.begin(), data.end(), buffer_.begin());
    length_ = data.size();
}

bool PeriodicSender::trigger()
{
    if (paused_ || !node_.can_send_now())
    {
        return false;
    }
    restart();
    return true;
}

void PeriodicSender::pause() noexcept
{
    paused_ = true;
    timer_.cancel();
}

void PeriodicSender::resume()
{
    paused_ = false;
    if (node_.can_send_now() && !timer_.running())
    {
        restart();
    }
}

void PeriodicSender::restart()
{
    restarted_ = true;
    timer_.start_at(node_.network().event_time()); // the next chance
}

void PeriodicSender::send_or_suppress()
{
    if (suppression_ != nullptr && suppression_->suspended())
    {
        ++statistics_.suppressed;
    }
    else
    {
        send_now();
    }
}


void PeriodicSender::on_deadline()
{
    if (paused_ || !node_.can_send_now())
    {
        return; // the address handler starts it again
    }
    const SimTime deadline = *timer_.deadline();
    send_or_suppress();
    SimTime next = deadline + period_;
    const auto now = node_.network().now();
    if (std::exchange(restarted_, false))
    {
        // Started again (trigger(), resume(), creation) at a time already handled (the
        // application stage, outside a step): no deadline is due before this send, so the period
        // counts from this step if it already passed (nothing merged, nothing caught up).
        if (now && next <= *now)
        {
            next = *now + period_;
        }
    }
    else if (options_.missed_deadlines == MissedDeadlines::Coalesce && now && next <= *now)
    {
        // The deadlines up to the step's time would fire in this step too: this send serves them.
        const auto merged = (*now - deadline) / period_;
        statistics_.coalesced += static_cast<std::uint64_t>(merged);
        next = deadline + (merged + 1) * period_;
    }
    timer_.start_at(next);
}

void PeriodicSender::send_now()
{
    if (encoder_)
    {
        const std::size_t length = encoder_(std::span<std::uint8_t>(buffer_));
        if (length > buffer_.size())
        {
            throw std::length_error("vehicle_sil::j1939::PeriodicSender: the encoder wrote more than max_length");
        }
        length_ = length;
    }
    const SendStatus status = node_.send(pgn_, std::span<const std::uint8_t>(buffer_.data(), length_), options_.destination,
                                         options_.priority, WhenBusy::Fail);
    if (status == SendStatus::Ok)
    {
        ++statistics_.sent;
    }
    else
    {
        ++statistics_.failures;
        statistics_.last_failure = status;
    }
}

} // namespace vehicle_sil::j1939
