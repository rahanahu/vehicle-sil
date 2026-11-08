// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/broadcast_suppression.hpp"

#include <array>
#include <stdexcept>

namespace vehicle_sil::j1939 {

namespace {

constexpr std::uint8_t kStop = 0x0;
constexpr std::uint8_t kStart = 0x1;
constexpr auto kHoldTime = std::chrono::seconds(6);
constexpr std::uint8_t kSuspendAll = 0x2;
constexpr std::uint8_t kSuspendSome = 0x3;
constexpr std::int64_t kMaxSuspendSeconds = 64255;

[[nodiscard]] std::uint8_t command_of(std::span<const std::uint8_t> data, Dm13Network network) noexcept
{
    const auto n = static_cast<unsigned>(network);
    return static_cast<std::uint8_t>((data[n / 4u] >> (2u * (n % 4u))) & 0x3u);
}

} // namespace

BroadcastSuppression& BroadcastSuppression::of(Node& node)
{
    if (auto* existing = node.find<BroadcastSuppression>())
    {
        return *existing;
    }
    return node.attach<BroadcastSuppression>(Key{}, BroadcastSuppressionOptions{});
}

BroadcastSuppression& BroadcastSuppression::attach(Node& node, BroadcastSuppressionOptions options)
{
    return node.attach<BroadcastSuppression>(Key{}, options);
}

BroadcastSuppression::BroadcastSuppression(Node& node, Key /*key*/, BroadcastSuppressionOptions options)
  : node_(node),
    options_(options),
    hold_(node, TimerKind::Timeout, [this] { stopped_ = false; }),
    own_end_(node, TimerKind::Scheduled, [this] { own_ = false; }),
    announce_(node, TimerKind::Scheduled, [this] {
        const std::array<std::uint8_t, 8> data{0xFF,
                                               0xFF,
                                               0xFF,
                                               static_cast<std::uint8_t>(0xF0u | announce_signal_),
                                               static_cast<std::uint8_t>(announce_duration_),
                                               static_cast<std::uint8_t>(announce_duration_ >> 8),
                                               0xFF,
                                               0xFF};
        if (node_.send(kPgnDm13, data) != SendStatus::Ok)
        {
            ++send_failures_;
        }
    })
{
    if (static_cast<unsigned>(options_.network) > static_cast<unsigned>(Dm13Network::J1939Network3))
    {
        throw std::invalid_argument("vehicle_sil::j1939::BroadcastSuppression: unknown network");
    }
    if (node.find<BroadcastSuppression>() != nullptr)
    {
        throw std::logic_error("vehicle_sil::j1939::BroadcastSuppression: the node already has one");
    }
    dm13_ = Subscription{node, node.on_message(kPgnDm13, [this](const Message& m) { on_dm13(m); })};
}

void BroadcastSuppression::on_dm13(const Message& message)
{
    if (message.data.size() < 8)
    {
        return;
    }
    std::uint8_t command = command_of(message.data, options_.network);
    if (command != kStop && command != kStart)
    {
        command = command_of(message.data, Dm13Network::CurrentDataLink);
    }
    if (command == kStart)
    {
        stopped_ = false;
        hold_.cancel();
        return;
    }
    if (command == kStop)
    {
        stopped_ = true;
    }
    if (stopped_)
    {
        // Any DM13 (a stop, a hold signal, a command for another network) keeps the stop.
        hold_.start_after(kHoldTime);
    }
}

void BroadcastSuppression::suspend(std::optional<std::chrono::seconds> duration, SuspendScope scope)
{
    if (duration && (duration->count() < 0 || duration->count() > kMaxSuspendSeconds))
    {
        throw std::invalid_argument("vehicle_sil::j1939::BroadcastSuppression: the suspension lasts 0 .. 64255 s");
    }
    own_ = true;
    announce_signal_ = scope == SuspendScope::All ? kSuspendAll : kSuspendSome;
    announce_duration_ = duration ? static_cast<std::uint16_t>(duration->count()) : 0xFFFF;
    if (duration)
    {
        own_end_.start_after(*duration);
    }
    else
    {
        own_end_.cancel();
    }
    announce_.start_after(SimDuration::zero());
}

void BroadcastSuppression::resume() noexcept
{
    own_ = false;
    own_end_.cancel();
    announce_.cancel(); // a suspension not announced yet is not announced any more
}

} // namespace vehicle_sil::j1939
