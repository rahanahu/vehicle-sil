// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/periodic_sender.hpp"

#include "vehicle_sil/j1939/broadcast_suppression.hpp"
#include "vehicle_sil/j1939/detail/user_call.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::j1939 {

namespace {

[[nodiscard]] std::string hex(std::uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof text, "0x%X", static_cast<unsigned>(value));
    return text;
}

/// "vehicle_sil::j1939::PeriodicSender (PGN 0xFF20): " to start the messages of the errors.
[[nodiscard]] std::string who(Pgn pgn) { return "vehicle_sil::j1939::PeriodicSender (PGN " + hex(pgn.value()) + "): "; }

void check_pgn(Pgn pgn)
{
    if (!pgn.is_valid())
    {
        throw std::invalid_argument("vehicle_sil::j1939::PeriodicSender: invalid PGN " + hex(pgn.value()) +
                                    " (at most 18 bits; a PDU1 group has PS = 0)");
    }
}

void check_period(Pgn pgn, SimDuration period)
{
    if (period <= SimDuration::zero())
    {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(period).count();
        throw std::invalid_argument(who(pgn) + "the period must be > 0, not " + std::to_string(ns) + " ns");
    }
}

void check_priority(Pgn pgn, Priority priority)
{
    if (priority > kMaxPriority)
    {
        throw std::invalid_argument(who(pgn) + "priority " + std::to_string(priority) + " is above 7 (0 highest .. 7)");
    }
}

void check_destination(Pgn pgn, Address destination)
{
    if (pgn.is_pdu2() && destination != kGlobalAddress)
    {
        throw std::invalid_argument(who(pgn) + "destination " + hex(destination) +
                                    ": a PDU2 group goes to all nodes, use kGlobalAddress");
    }
    if (pgn.is_pdu1() && destination == kNullAddress)
    {
        throw std::invalid_argument(who(pgn) + "destination 0xFE (kNullAddress): a PDU1 group goes to kGlobalAddress or "
                                               "to one address 0x00 .. 0xFD");
    }
}

/// Everything the constructor checks, before it attaches or reserves anything. `data_length`:
/// the fixed data's (0 with an encoder).
[[nodiscard]] PeriodicSenderOptions checked(Pgn pgn, SimDuration period, const PeriodicSenderOptions& options,
                                            std::size_t data_length)
{
    check_pgn(pgn);
    check_period(pgn, period);
    check_priority(pgn, options.priority);
    check_destination(pgn, options.destination);
    if (options.max_length == 0 || options.max_length > kMaxTransportLength)
    {
        throw std::invalid_argument(who(pgn) + "max_length " + std::to_string(options.max_length) +
                                    " is outside 1 .. kMaxTransportLength (" + std::to_string(kMaxTransportLength) + ")");
    }
    if (data_length > options.max_length)
    {
        throw std::length_error(who(pgn) + "the data has " + std::to_string(data_length) + " bytes, more than max_length (" +
                                std::to_string(options.max_length) + "): raise PeriodicSenderOptions::max_length");
    }
    return options;
}

} // namespace

PeriodicSender::PeriodicSender(Node& node, Pgn pgn, SimDuration period, std::span<const std::uint8_t> data,
                               PeriodicSenderOptions options)
  : PeriodicSender(node, pgn, period, options, data, Encoder{}, VectorEncoder{})
{
}

PeriodicSender::PeriodicSender(Node& node, Pgn pgn, SimDuration period, Encoder encoder, PeriodicSenderOptions options)
  : PeriodicSender(node, pgn, period, options, {}, required(pgn, std::move(encoder)), VectorEncoder{})
{
}

PeriodicSender::Encoder PeriodicSender::required(Pgn pgn, Encoder encoder)
{
    if (!encoder)
    {
        throw std::invalid_argument(who(pgn) + "empty encoder");
    }
    return encoder;
}

PeriodicSender::VectorEncoder PeriodicSender::required(Pgn pgn, VectorEncoder encoder)
{
    if (!encoder)
    {
        throw std::invalid_argument(who(pgn) + "empty encoder");
    }
    return encoder;
}

PeriodicSender::PeriodicSender(Node& node, Pgn pgn, SimDuration period, PeriodicSenderOptions options,
                               std::span<const std::uint8_t> data, Encoder encoder, VectorEncoder vector_encoder)
  : node_(node),
    pgn_(pgn),
    period_(period),
    options_(checked(pgn, period, options, data.size())), // before anything is attached to the node
    encoder_(std::move(encoder)),
    vector_encoder_(std::move(vector_encoder)),
    source_(encoder_ ? Source::Encoder : vector_encoder_ ? Source::Vector : Source::Data),
    suppression_(options_.suppressible.value_or(options_.destination == kGlobalAddress) ? &BroadcastSuppression::of(node)
                                                                                         : nullptr),
    timer_(node, TimerKind::Scheduled, [this] { on_deadline(); })
{
    if (vector_encoder_)
    {
        buffer_.reserve(options_.max_length);
    }
    else
    {
        buffer_.resize(options_.max_length);
        std::copy(data.begin(), data.end(), buffer_.begin());
        length_ = data.size();
    }

    address_ = Subscription{node, node.on_address_change([this](const AddressChange& change) {
        if (node_.can_send_now() && !paused_)
        {
            // Not again when a node that did not wait after its claim becomes Claimed.
            if (!timer_.running() || change.address != change.previous_address)
            {
                // The address handler runs in the step: the first message leaves in this step.
                if (!send_or_suppress())
                {
                    return; // the encoder destroyed the sender
                }
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
    if (data.size() > options_.max_length)
    {
        throw std::length_error(who(pgn_) + "set_data(): the data has " + std::to_string(data.size()) +
                                " bytes, more than max_length (" + std::to_string(options_.max_length) +
                                "): raise PeriodicSenderOptions::max_length");
    }
    if (source_ != Source::Data)
    {
        return; // the encoder wins
    }
    std::copy(data.begin(), data.end(), buffer_.begin());
    length_ = data.size();
}

PeriodicSender::~PeriodicSender() { detail::mark_gone(gone_); }

void PeriodicSender::set_period(SimDuration period)
{
    check_period(pgn_, period);
    // A start at the next chance that is still to come keeps its time; the period counts from it.
    // Restarted before the period is kept: if that throws (from an Observer), nothing changed.
    if (timer_.running() && !restarted_)
    {
        timer_.start_after(period);
    }
    period_ = period;
}

void PeriodicSender::set_destination(Address destination)
{
    check_destination(pgn_, destination);
    BroadcastSuppression* suppression = suppression_;
    if (!options_.suppressible && destination == kGlobalAddress && suppression == nullptr)
    {
        // Not attached here: it would take timers in a handler and would not have seen the DM13
        // that came before.
        suppression = node_.find<BroadcastSuppression>();
        if (suppression == nullptr)
        {
            throw std::logic_error(who(pgn_) +
                                   "set_destination(kGlobalAddress) needs the node's BroadcastSuppression to follow "
                                   "DM13: call BroadcastSuppression::of(node) at set-up, or set "
                                   "PeriodicSenderOptions::suppressible to true or false");
        }
    }
    suppression_ = suppression;
    options_.destination = destination;
}

void PeriodicSender::set_priority(Priority priority)
{
    check_priority(pgn_, priority);
    options_.priority = priority;
}

bool PeriodicSender::suppressible() const noexcept
{
    return suppression_ != nullptr && options_.suppressible.value_or(options_.destination == kGlobalAddress);
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

bool PeriodicSender::send_or_suppress()
{
    if (suppressible() && suppression_->suspended())
    {
        ++statistics_.suppressed;
        return true;
    }
    return send_now();
}


void PeriodicSender::on_deadline()
{
    if (paused_ || !node_.can_send_now())
    {
        return; // the address handler starts it again
    }
    const SimTime deadline = *timer_.deadline();
    if (!send_or_suppress())
    {
        return; // the encoder destroyed the sender
    }
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

bool PeriodicSender::send_now()
{
    if (source_ != Source::Data)
    {
        // The encoder may destroy the sender: it runs on the encoder and the buffer lent to it,
        // which go back to the sender afterwards unless it is gone.
        const detail::UserCall call(gone_);
        std::size_t length = 0;
        {
            detail::Lent<std::vector<std::uint8_t>> buffer(buffer_, call);
            if (source_ == Source::Encoder)
            {
                detail::Lent<Encoder> encoder(encoder_, call);
                length = encoder.value(std::span<std::uint8_t>(buffer.value));
            }
            else
            {
                buffer.value.clear();
                detail::Lent<VectorEncoder> encoder(vector_encoder_, call);
                encoder.value(buffer.value);
                length = buffer.value.size();
            }
        }
        if (call.gone())
        {
            return false;
        }
        if (length > options_.max_length)
        {
            throw std::length_error(who(pgn_) + (source_ == Source::Encoder ? "the encoder returned the length " : "the encoder wrote ") +
                                    std::to_string(length) + (source_ == Source::Encoder ? "" : " bytes") +
                                    ", more than max_length (" + std::to_string(options_.max_length) +
                                    "): raise PeriodicSenderOptions::max_length");
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
    return true;
}

} // namespace vehicle_sil::j1939
