// SPDX-License-Identifier: MIT
//
// Typed J1939 messages (codec.hpp) on a node: send one, receive them, send one periodically.
// Thin adapters over Node::send(), Node::on_message() and PeriodicSender; the codec itself
// stays free of the node.
#pragma once

#include "vehicle_sil/j1939/codec.hpp"
#include "vehicle_sil/j1939/network.hpp"
#include "vehicle_sil/j1939/periodic_sender.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace vehicle_sil::j1939::codec {

/// Sends `message` from `node` with its PGN and priority (Node::send(): in a step only).
template <MessageType M>
SendStatus send(Node& node, const M& message, Address destination = kGlobalAddress,
                WhenBusy whenBusy = WhenBusy::Fail)
{
    const auto bytes = message_bytes(message);
    return node.send(M::kPgn, bytes, destination, M::kPriority, whenBusy);
}

/// Decodes `message` as M if it has M's PGN (M::decode() looks at the bytes only). For messages
/// that may come with other PGNs, such as those an Exchange's matcher sees.
template <MessageType M>
[[nodiscard]] std::optional<M> decode(const Message& message)
{
    if (message.pgn != M::kPgn)
    {
        return std::nullopt;
    }
    return M::decode(message.data);
}

/// Calls `handler(const M&)` or `handler(const M&, const Message&)` for every received message of
/// M's PGN that decodes as M (Node::on_message()). Messages shorter than M::kLength, and those
/// whose selector belongs to another message type, are not passed (to see them, use
/// Node::on_message() and M::decode()). The returned id unregisters it (Subscription).
template <MessageType M, typename Handler>
    requires std::is_invocable_v<Handler&, const M&> || std::is_invocable_v<Handler&, const M&, const Message&>
HandlerId on_message(Node& node, Handler handler)
{
    return node.on_message(M::kPgn, [handler = std::move(handler)](const Message& raw) mutable {
        const auto message = M::decode(raw.data);
        if (!message)
        {
            return;
        }
        if constexpr (std::is_invocable_v<Handler&, const M&, const Message&>)
        {
            handler(*message, raw);
        }
        else
        {
            handler(*message);
        }
    });
}

/// A PeriodicSender::Encoder that, at each send, starts from a new M (every signal not
/// available, or raw 0 without such a range), lets `fill(M&)` set its signals and writes it.
template <MessageType M, typename Fill>
    requires std::is_invocable_v<Fill&, M&>
PeriodicSender::Encoder encoder(Fill fill)
{
    return [fill = std::move(fill)](std::span<std::uint8_t> out) mutable {
        M message;
        fill(message);
        return message.encode(out);
    };
}

/// Attaches a PeriodicSender that sends M every `period` (see encoder()), with the priority of the
/// definition (M::kPriority) and a buffer of M::kLength bytes at least. options.priority must be
/// M::kPriority or left at kDefaultPriority (std::invalid_argument otherwise: to send with another
/// priority, change the definition).
template <MessageType M, typename Fill>
    requires std::is_invocable_v<Fill&, M&>
PeriodicSender& attach_sender(Node& node, SimDuration period, Fill fill, PeriodicSenderOptions options = {})
{
    if (options.priority != kDefaultPriority && options.priority != M::kPriority)
    {
        throw std::invalid_argument("vehicle_sil::j1939::codec::attach_sender: the message's definition gives priority " +
                                    std::to_string(M::kPriority) + ", options.priority is " +
                                    std::to_string(options.priority));
    }
    options.priority = M::kPriority;
    options.max_length = (std::max)(options.max_length, M::kLength);
    return node.attach<PeriodicSender>(M::kPgn, period, encoder<M>(std::move(fill)), std::move(options));
}

} // namespace vehicle_sil::j1939::codec
