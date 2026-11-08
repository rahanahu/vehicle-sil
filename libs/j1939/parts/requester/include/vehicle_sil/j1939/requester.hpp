// SPDX-License-Identifier: MIT
//
// A protocol part of the J1939 stack: sends Requests (PGN 59904) for a node and reports the
// answer (the requested group, an acknowledgement, or a timeout), with retries. Built on the
// node's public API and the toolbox only.
#pragma once

#include "vehicle_sil/j1939/toolbox.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace vehicle_sil::j1939 {

/// How a request ended (or, for a request to all nodes, what came).
enum class RequestOutcome : std::uint8_t
{
    Response,     ///< The requested group arrived (RequestResult::data).
    Acknowledged, ///< An Acknowledgement for it arrived (RequestResult::ack: positive or not).
    Timeout,      ///< To one address: no answer after the last attempt. To all nodes: the wait
                  ///< ended (the responses came before, with last == false).
    NotSent,      ///< The Request could not be sent (RequestResult::send_status; NoAddress once
                  ///< the node cannot claim an address).
};

/// What a request's callback gets.
struct RequestResult
{
    RequestOutcome outcome = RequestOutcome::Timeout;
    Pgn pgn{};                                ///< The requested PGN.
    Address destination = kGlobalAddress;     ///< The address the Request went to.
    Address source = kNullAddress;            ///< Who answered (Response, Acknowledged).
    std::span<const std::uint8_t> data{};     ///< Response: valid during the callback only.
    AckCode ack = AckCode::Positive;          ///< Acknowledged.
    SendStatus send_status = SendStatus::Ok;  ///< NotSent.
    std::uint32_t attempts = 0;               ///< Requests sent for it so far.
    /// false only for a response to a request to all nodes: more may follow until the Timeout.
    bool last = true;
    /// The answer's time (Response, Acknowledged), the deadline (Timeout), the step's time
    /// (NotSent).
    SimTime time{};
};

using RequestCallback = std::function<void(const RequestResult&)>;

/// Identifies a request (Requester::cancel()); false if request() refused it.
struct RequestId
{
    std::uint64_t value = 0;
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    [[nodiscard]] friend constexpr bool operator==(RequestId, RequestId) noexcept = default;
};

struct RequesterOptions
{
    /// How long to wait for the answer after each Request, counted from the step that sent it.
    SimDuration timeout = std::chrono::milliseconds(1250);
    /// Requests sent again after a timeout (requests to one address only).
    std::uint32_t retries = 2;
    /// The least time between two Requests for the same PGN to the same address (0: none).
    /// A request that would come sooner waits; an answer that arrives meanwhile serves it.
    SimDuration min_interval = SimDuration::zero();
    /// Requests that may wait at the same time (allocated when the requester is created).
    std::size_t capacity = 8;
};

struct RequesterStatistics
{
    std::uint64_t requests_sent = 0; ///< Requests (PGN 59904) the node sent.
    std::uint64_t joined = 0;        ///< Requests that waited for the same PGN and address as
                                     ///< another one and got its answer (no Request of their own).
    std::uint64_t refused = 0;       ///< request() calls refused because capacity was reached.
};

/// Sends Requests for a node and calls back with the answer:
///
///     Requester::of(ecu).request(kPgnSoftwareId, 0x00, [&](const RequestResult& r) {
///         if (r.outcome == RequestOutcome::Response) { model.set_software_id(r.data); }
///     });
///
/// - request() may be called everywhere (also outside a step and before the node has an
///   address). The Request leaves at the next chance (in a handler: in this step; in a Timer's
///   callback: in this step or the next; in the application stage or outside a step: in the next
///   one) once the node can send
///   (Node::can_send_now(); not during the wait after its claim); the wait counts from the step
///   that sent it.
/// - To one address: the first message of the requested PGN from that address, or an
///   Acknowledgement from it naming this node and the PGN, ends the request. Without an answer
///   within `timeout` the Request is sent again, `retries` times, then the outcome is Timeout.
/// - To all nodes (kGlobalAddress): every response within `timeout` is reported (last ==
///   false), then Timeout (last == true). Not sent again.
/// - Requests for the same PGN and address share one transaction: one Request, one answer for
///   all of them (statistics().joined). min_interval spaces the Requests of one PGN and address.
///   An answer that arrives while a request waits to be sent (or sent again) serves it, as does
///   a message of the PGN broadcast for another reason: it is the latest value either way.
/// - When the node loses or changes its address, sent requests are sent again once it can send
///   (the answer would go to the old address; this does not use up `retries`); when it cannot
///   claim one, they end with NotSent.
/// - The callback runs in the step (a handler or a timer of the network); it may call request()
///   and cancel(). It is called once per request (and per response to all nodes), never for a
///   cancelled one.
///
/// One per node, attached to it and shared by the application and the parts (the transactions
/// and min_interval are the node's): Requester::of(node), or Requester::attach(node, options)
/// to choose the options (before anything uses of()). It cannot be constructed otherwise.
/// Uses two Timers of the network. Not from other threads (Host::post()).
class Requester
{
    struct Key
    {
        explicit Key() = default;
    };

public:
    /// The requester of `node`, attached with the default options if it has none yet.
    static Requester& of(Node& node);
    /// Attaches the requester of `node` with `options`. std::logic_error if the node has one
    /// already (of() attached one, or attach() was called before). std::invalid_argument for
    /// timeout <= 0, min_interval < 0 or capacity 0; std::length_error if the network has fewer
    /// than two timers left.
    static Requester& attach(Node& node, RequesterOptions options);

    /// Only of() and attach() can call it (Key is private).
    Requester(Node& node, Key key, RequesterOptions options);
    Requester(const Requester&) = delete;
    Requester& operator=(const Requester&) = delete;

    /// Requests `pgn` from `destination` (one address, or kGlobalAddress) and calls `callback`
    /// with the answer. An empty RequestId (and no callback) if `capacity` requests are waiting.
    /// std::invalid_argument for an invalid PGN, destination kNullAddress or an empty callback.
    RequestId request(Pgn pgn, Address destination, RequestCallback callback);
    /// Forgets a request: its callback is not called. false if it is not waiting (it ended).
    bool cancel(RequestId id) noexcept;
    /// Requests waiting for their answer (or to be sent).
    [[nodiscard]] std::size_t pending() const noexcept;

    [[nodiscard]] const RequesterOptions& options() const noexcept { return options_; }
    [[nodiscard]] const RequesterStatistics& statistics() const noexcept { return statistics_; }

private:
    enum class Phase : std::uint8_t
    {
        Free,
        Waiting, ///< To be sent at send_at, once the node owns its address.
        Sent,    ///< Waiting for the answer until deadline.
    };
    struct Transaction
    {
        Phase phase = Phase::Free;
        Pgn pgn{};
        Address destination = kGlobalAddress;
        SimTime time{}; ///< Waiting: send_at. Sent: the deadline.
        std::uint32_t attempts = 0;
        std::uint32_t timeouts = 0;   ///< Waits that ended without an answer (for `retries`).
        std::uint32_t generation = 0; ///< Counts the uses of the slot (the waiters name it).
        std::uint64_t serial = 0;     ///< Creation order: loops skip those created during them.
    };
    struct Waiter
    {
        std::uint64_t id = 0; ///< 0: free.
        std::size_t transaction = 0;
        std::uint32_t generation = 0;
        RequestCallback callback;
    };
    struct Recent
    {
        Pgn pgn{};
        Address destination = kGlobalAddress;
        SimTime sent{};
        bool used = false;
    };

    [[nodiscard]] SimTime earliest_send(Pgn pgn, Address destination, SimTime from) const noexcept;
    void remember_sent(Pgn pgn, Address destination, SimTime time) noexcept;
    void arm();
    void on_send_timer();
    void on_wait_timer();
    /// Sends the Request of a waiting transaction now (or ends it with NotSent).
    void send(std::size_t index);
    void on_message(const Message& message);
    void on_address(const AddressChange& change);
    /// Ends the transaction (unless !last) and calls its waiters with `result`.
    void report(std::size_t index, RequestResult result);

    Node& node_;
    RequesterOptions options_;
    std::vector<Transaction> transactions_;
    std::vector<Waiter> waiters_;
    std::vector<Recent> recent_;
    std::uint64_t next_id_ = 1;
    std::uint64_t next_serial_ = 1;
    RequesterStatistics statistics_;
    Timer send_;
    Timer wait_;
    Subscription messages_;
    Subscription address_;
};

} // namespace vehicle_sil::j1939
