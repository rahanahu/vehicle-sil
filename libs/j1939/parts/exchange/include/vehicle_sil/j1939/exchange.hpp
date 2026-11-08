// SPDX-License-Identifier: MIT
//
// A protocol part of the J1939 stack for protocols of one's own: sends a command to one control
// function and waits for its reply, with a timeout, retries, interim replies and cancellation,
// following the node's and the counterpart's addresses. Built on the node's public API and the
// toolbox only.
#pragma once

#include "vehicle_sil/j1939/toolbox.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace vehicle_sil::j1939 {

/// What a received message is to an exchange (ReplyMatcher).
enum class ReplyMatch : std::uint8_t
{
    Ignore,  ///< Not an answer to this command.
    Pending, ///< An interim answer (the counterpart is working on it): wait `timeout` again from
             ///< this message, and do not send the command again.
    Final,   ///< The answer: the exchange ends with ExchangeOutcome::Reply.
};

/// Tells what a message from the counterpart is to the exchange. Called in the step for every
/// message from the counterpart's address (to this node or to all nodes) while the exchange
/// waits for its answer.
using ReplyMatcher = std::function<ReplyMatch(const Message&)>;

/// How an exchange ended.
enum class ExchangeOutcome : std::uint8_t
{
    Reply,    ///< The matcher took a message as the answer (ExchangeResult::reply).
    Timeout,  ///< No answer within `timeout` after the last attempt (or after the last Pending).
    NotSent,  ///< The command could not be sent: Node::send() refused it (send_status; NoAddress
              ///< once the node cannot claim an address), or its transfer (a command longer than
              ///< 8 bytes) failed on the last attempt (transfer, reason).
    PeerLost, ///< The counterpart has no address: its NAME (given to start()) was not claimed
              ///< within `timeout` of when the command was due, or, while the exchange ran, it
              ///< moved to another address, gave its address up (Cannot Claim) or another NAME
              ///< took its address.
};

/// What an exchange's callback gets.
struct ExchangeResult
{
    ExchangeOutcome outcome = ExchangeOutcome::Timeout;
    Pgn pgn{};                       ///< The command's PGN.
    /// The counterpart's address when the command was last sent (kNullAddress if it never was).
    Address peer = kNullAddress;
    /// The counterpart's NAME: the one given to start(), or the one at its address when the
    /// command was first sent (empty if none was known).
    std::optional<Name> peer_name{};
    Message reply{};                 ///< Reply: the answer (its data valid during the callback only).
    SendStatus send_status = SendStatus::Ok;            ///< NotSent by send().
    TransferResult transfer = TransferResult::Completed; ///< NotSent by the transfer: how it ended.
    AbortReason reason = AbortReason::None;              ///< NotSent by the transfer.
    std::uint32_t attempts = 0;      ///< Commands sent for it.
    bool pending = false;            ///< A Pending answer came before the end.
    /// Reply: the answer's time. Timeout: the deadline. NotSent: the step's time or the end of
    /// the transfer. PeerLost: the time of the Address Claimed (or Cannot Claim) that showed it,
    /// or the step's time.
    SimTime time{};
};

using ExchangeCallback = std::function<void(const ExchangeResult&)>;

/// Identifies an exchange (Exchange::cancel()); false if start() refused it.
struct ExchangeId
{
    std::uint64_t value = 0;
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    [[nodiscard]] friend constexpr bool operator==(ExchangeId, ExchangeId) noexcept = default;
};

struct ExchangeOptions
{
    /// How long to wait for the answer: from the step that sent the command (a command longer
    /// than 8 bytes: from the end of its transfer), and again from each Pending answer.
    SimDuration timeout = std::chrono::milliseconds(1250);
    /// Commands sent again after a timeout or a failed transfer, before any Pending answer came.
    /// 0 by default: a command is sent again only if the protocol allows it (idempotent, or the
    /// counterpart recognises a repeated one).
    std::uint32_t retries = 0;
    /// Exchanges that may run at the same time.
    std::size_t capacity = 4;
    /// The longest command in bytes (1 .. kMaxTransportLength); each exchange keeps a copy.
    std::size_t max_length = 8;
};

struct ExchangeStatistics
{
    std::uint64_t sent = 0;      ///< Commands sent (also again).
    std::uint64_t replies = 0;   ///< Exchanges that ended with Reply.
    std::uint64_t timeouts = 0;  ///< ... with Timeout.
    std::uint64_t not_sent = 0;  ///< ... with NotSent.
    std::uint64_t peer_lost = 0; ///< ... with PeerLost.
    std::uint64_t refused = 0;   ///< start() calls refused because capacity was reached.
};

/// Sends commands of a protocol to one control function at a time and waits for the answers:
///
///     Exchange exchange(node, {.timeout = 200ms, .retries = 1});
///     exchange.start(kPgnMine, command, kServerName,
///                    [](const Message& m) { return m.data[0] == 0x81 ? ReplyMatch::Final : ReplyMatch::Ignore; },
///                    [](const ExchangeResult& r) { if (r.outcome == ExchangeOutcome::Reply) { use(r.reply.data); } });
///
/// - start() may be called everywhere (also outside a step and before the node has an
///   address). The command leaves at the next chance once the node can send
///   (Node::can_send_now()): in a handler in this step, in the application stage or outside a
///   step in the next one. A PDU1 command goes to the counterpart's address, a PDU2 command to
///   all nodes. Longer than 8 bytes it travels with the transport protocol (WhenBusy::Queue: a
///   transfer of the node to the same destination still going delays it); the wait starts when
///   its transfer completed (TransferEnd::id).
/// - The counterpart is an address, or a NAME looked up (Network::address_of()) each time the
///   command is sent. A NAME that no control function holds yet when the command is first due
///   (at start-up) is waited for: the command leaves when its Address Claimed comes, or the
///   exchange ends with PeerLost after `timeout`. The matcher sees the messages from the
///   counterpart's address, of every PGN (check the PGN: M::decode() looks at the bytes only;
///   codec::decode<M>(message) checks both); the oldest exchange whose matcher does not ignore a
///   message gets it (two equal commands to the same counterpart get their answers in order).
///   A matcher that cannot tell the answer to an earlier command (one sent again, or cancelled)
///   from the answer to its own may take it: give the commands something to tell them apart
///   (an item, a sequence number) where answers can come late.
/// - Without an answer within `timeout` the command is sent again, `retries` times, then the
///   outcome is Timeout. A transfer that the receiver aborted or that timed out counts as an
///   attempt the same way. After a Pending answer the command is not sent again. The wait does
///   not know about an answer still being received with the transport protocol: allow for the
///   time of a long answer in `timeout`.
/// - When the node loses or changes its address, a command already sent is sent again once the
///   node can send (the answer would go to the old address; this does not use up `retries`,
///   also after a Pending answer); when it cannot claim one, the exchanges end with NotSent.
/// - PeerLost when the counterpart's NAME (given, or known at its address when the command was
///   first sent) moves, gives its address up or loses it to another NAME, as soon as the
///   Address Claimed (or Cannot Claim) is received, also with retries left (a counterpart that
///   moved may have lost the state of the exchange; the next exchange finds it at its new
///   address). Without a known NAME only the timeout ends an exchange with a silent counterpart.
/// - The callback is called once per exchange, never for a cancelled one, in the step (a
///   handler or a timer of the network). The matcher and the callback may call start() and
///   cancel(), and may destroy the Exchange (it calls nothing after that).
/// - Destroying the Exchange ends its exchanges without calling back. It may outlive the
///   network (it then does nothing).
///
/// Uses two Timers of the network. Create it with node.attach<Exchange>(options) or owned by a
/// part or the application (members of a part of one's own). Not from other threads
/// (Host::post()).
class Exchange
{
public:
    /// std::invalid_argument for timeout <= 0, capacity 0 or max_length outside 1 ..
    /// kMaxTransportLength; std::length_error if the network has fewer than two timers left.
    explicit Exchange(Node& node, ExchangeOptions options = {});
    ~Exchange();
    Exchange(const Exchange&) = delete;
    Exchange& operator=(const Exchange&) = delete;

    /// Sends `command` (copied) with `pgn` to the control function at `peer` (0..253) and calls
    /// `done` with the outcome. An empty ExchangeId (and no callback) if `capacity` exchanges
    /// run. std::invalid_argument for an invalid PGN, priority > 7, `peer` outside 0..253, an
    /// empty `command`, a command longer than max_length, or an empty matcher or callback.
    ExchangeId start(Pgn pgn, std::span<const std::uint8_t> command, Address peer, ReplyMatcher match,
                     ExchangeCallback done, Priority priority = kDefaultPriority);
    /// The same with the counterpart given by its NAME (see the class comment: it may wait for
    /// the NAME's Address Claimed).
    ExchangeId start(Pgn pgn, std::span<const std::uint8_t> command, Name peer, ReplyMatcher match,
                     ExchangeCallback done, Priority priority = kDefaultPriority);
    /// Forgets an exchange: its callback is not called. false if it is not running. A command
    /// already sent (or a transfer under way) is not taken back: its answer may still come.
    bool cancel(ExchangeId id) noexcept;
    /// Forgets every exchange (no callbacks).
    void cancel_all() noexcept;
    [[nodiscard]] bool running(ExchangeId id) const noexcept;
    /// Exchanges running (waiting to be sent or for their answer).
    [[nodiscard]] std::size_t pending() const noexcept;

    [[nodiscard]] Node& node() const noexcept { return node_; }
    [[nodiscard]] const ExchangeOptions& options() const noexcept { return options_; }
    [[nodiscard]] const ExchangeStatistics& statistics() const noexcept { return statistics_; }

private:
    enum class Phase : std::uint8_t
    {
        Free,
        Waiting,      ///< To be sent at `time` once the node can send.
        Resolving,    ///< Given by a NAME not claimed yet: waits for it until `time`.
        Transferring, ///< A long command whose transfer has not ended.
        Sent,         ///< Waiting for the answer until `time`.
    };
    struct Slot
    {
        Phase phase = Phase::Free;
        std::uint64_t id = 0; ///< ExchangeId; also the creation order.
        Pgn pgn{};
        Priority priority = kDefaultPriority;
        std::vector<std::uint8_t> command; ///< max_length bytes.
        std::size_t length = 0;
        Address to = kNullAddress;     ///< Given by address.
        std::optional<Name> to_name{}; ///< Given by NAME.
        Address peer = kNullAddress;   ///< Where the command went last.
        std::optional<Name> peer_name{};
        SimTime time{};                ///< Waiting: send at. Sent: the deadline.
        std::uint32_t attempts = 0;
        std::uint32_t failures = 0;    ///< Timeouts and failed transfers (for `retries`).
        bool pending = false;
        TransferId transfer{};
        std::optional<SimTime> resolve_by{}; ///< The end of the wait for the NAME (Resolving).
        ReplyMatcher match;
        ExchangeCallback done;
    };
    /// Set while user code runs: tells the caller that the Exchange was destroyed.
    struct Guard;

    ExchangeId add(Pgn pgn, std::span<const std::uint8_t> command, Address to, std::optional<Name> toName,
                   ReplyMatcher match, ExchangeCallback done, Priority priority);
    void arm();
    /// The next running slot after `after` in creation order with id < `limit`, or nullptr.
    [[nodiscard]] Slot* next_slot(std::uint64_t after, std::uint64_t limit) noexcept;
    /// false if the Exchange was destroyed meanwhile.
    bool on_send_timer();
    bool on_wait_timer();
    bool send_due(SimTime upTo);
    /// Sends the slot's command now, or ends it. false if the Exchange was destroyed.
    bool send(Slot& slot);
    bool on_message(const Message& message);
    bool on_address(const AddressChange& change);
    bool on_transfer_end(const TransferEnd& end);
    /// true if the counterpart of a slot that has sent no longer holds its address.
    [[nodiscard]] bool peer_lost(const Slot& slot) const noexcept;
    /// Frees the slot and calls its callback. false if the Exchange was destroyed.
    bool finish(Slot& slot, ExchangeResult result);

    Node& node_;
    ExchangeOptions options_;
    std::vector<Slot> slots_;
    std::uint64_t next_id_ = 1;
    ExchangeStatistics statistics_;
    bool* gone_ = nullptr;
    Timer send_;
    Timer wait_;
    Subscription messages_;
    Subscription address_;
    Subscription transfers_;
};

} // namespace vehicle_sil::j1939
