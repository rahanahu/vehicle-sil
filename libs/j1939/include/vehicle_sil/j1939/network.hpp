// SPDX-License-Identifier: MIT
//
// One J1939 network (one CAN channel) of the J1939 stack and its nodes (vehicle_sil::j1939).
// No threads, no clock, no SIL Kit: a vehicle_sil::can::Host steps it.
#pragma once

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/j1939/name.hpp"
#include "vehicle_sil/j1939/timer_set.hpp"
#include "vehicle_sil/j1939/types.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

namespace vehicle_sil::j1939 {

class Network;
class Node;
class Observer;
struct Event;
enum class DropReason : std::uint8_t;
class Subscription;
class Timer;
class Transport;

namespace detail {
/// The tests' way into the internals of a Network (defined only in the tests).
struct NetworkTestAccess;
} // namespace detail

/// The longest message the transport protocol carries.
inline constexpr std::size_t kMaxTransportLength = 1785;

/// Outcome of Node::send() and Node::request().
enum class SendStatus : std::uint8_t
{
    Ok,              ///< Queued; leaves at the end of the step (a transfer starts there; during
                     ///< the wait after the claim, both when the wait ends).
    QueueFull,       ///< The network's data queue (or, for a transfer, its transmit sessions,
                     ///< the node's hold area or the response queue) is full; nothing was queued.
    TooLong,         ///< More than kMaxTransportLength bytes.
    InvalidArgument, ///< Invalid PGN, priority > 7, a single-frame PDU2 group to one
                     ///< destination, or a group to the null address.
    NoAddress,       ///< The node lost its address (address claiming); it cannot send.
    Busy,            ///< A transfer of the node that the new one would run beside is still
                     ///< going: its broadcast (BAM), or its transfer to the same destination.
};

/// Reason byte of a transport Connection Abort.
enum class AbortReason : std::uint8_t
{
    None = 0,              ///< No abort (TransferEnd of a transfer that was not aborted).
    Busy = 1,              ///< Already in a connection with the sender; cannot take another one.
    ResourcesNeeded = 2,   ///< The system needs the resources of the session.
    Timeout = 3,
    CtsWhileSending = 4,   ///< A clear-to-send came before the packets of the last one were sent.
    RetransmitLimit = 5,   ///< Too many retransmission requests.
    UnexpectedData = 6,    ///< A data packet came when none was expected.
    BadSequence = 7,       ///< A sequence number that cannot be recovered from.
    DuplicateSequence = 8,
    TooLarge = 9,          ///< The message is longer than the receiver can take.
    Other = 250,           ///< Any other error.
};

/// What Node::send() does with a transfer (a message longer than 8 bytes) that cannot start yet
/// because the node's transfer it would run beside is still going (or no transmit session is
/// free).
enum class WhenBusy : std::uint8_t
{
    Fail,  ///< Return SendStatus::Busy (or QueueFull) and send nothing.
    Queue, ///< Wait in the network's response queue and start when it can, like a long
           ///< RequestContext::respond() (QueueFull if the queue is full).
};

/// How a transfer of a node (a message longer than 8 bytes) ended.
enum class TransferResult : std::uint8_t
{
    Completed,     ///< RTS/CTS: the receiver acknowledged it. BAM: the last packet was sent.
    AbortedByPeer, ///< The receiver sent a Connection Abort (TransferEnd::reason).
    Aborted,       ///< This node sent a Connection Abort (TransferEnd::reason).
    Cancelled,     ///< Ended without a frame: the node lost or changed its address, the
                   ///< destination address was claimed by another NAME or given up, or (rare)
                   ///< the transfer held during the wait after the claim found the control
                   ///< queue full when the wait ended.
    Lost,          ///< Receive side only (Observer): a broadcast (BAM) missed a packet or timed
                   ///< out; nothing is sent.
};

/// Identifies one transfer (a message longer than 8 bytes) of the nodes of a network: what
/// Node::last_transfer_id() gives after the send that accepted it, and TransferEnd::id at its
/// end. Unique within the network. Empty (false) for no transfer.
struct TransferId
{
    std::uint64_t value = 0;
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    [[nodiscard]] friend constexpr bool operator==(TransferId, TransferId) noexcept = default;
};

/// The end of a transfer, passed to Node::on_transfer_end().
struct TransferEnd
{
    Pgn pgn;
    Address destination;      ///< kGlobalAddress for a broadcast (BAM).
    std::size_t length;
    TransferResult result;
    AbortReason reason;       ///< AbortReason::None unless aborted.
    SimTime time;             ///< The time of the cause: the frame's time, or the step's time.
    TransferId id{};          ///< The transfer (Node::last_transfer_id() when it was accepted).
    /// Every data packet left (always for Completed): the receiver may have the whole message
    /// even if the transfer did not complete (e.g. its acknowledgement did not come). false: it
    /// cannot have it (the transfer was refused, cancelled or timed out before the last packet,
    /// or never started).
    bool all_packets_sent = false;
};

/// Whether an IncomingTransfer reports the start or the end of a transfer.
enum class TransferStage : std::uint8_t
{
    Started, ///< The connection (RTS) to the node was accepted, or a broadcast was announced (BAM).
    Ended,
};

/// A transfer (a message longer than 8 bytes) that a node receives, passed to
/// Node::on_incoming_transfer(): its start and its end, with the same id. Every transfer
/// reported as started is reported as ended exactly once (unless the network fails or is
/// destroyed first). The message of a Completed transfer is delivered right after its end.
struct IncomingTransfer
{
    TransferStage stage = TransferStage::Started;
    TransferId id{};                      ///< Unique within the network (with the sent ones).
    Pgn pgn{};
    Address source = kNullAddress;
    Address destination = kGlobalAddress; ///< The node (RTS/CTS), or kGlobalAddress (BAM).
    std::size_t length = 0;
    /// Ended: Completed; AbortedByPeer (the originator aborted); Aborted (this node aborted, e.g.
    /// after a timeout); Lost (a broadcast missed a packet or timed out); Cancelled (an address
    /// changed, the sender started the same transfer again, or the clear-to-send could not be
    /// queued).
    TransferResult result = TransferResult::Completed;
    AbortReason reason = AbortReason::None;
    SimTime time{};                       ///< The time of the frame or the timeout that caused it.
};

/// Where a node is in address claiming.
enum class AddressState : std::uint8_t
{
    Fixed,       ///< No NAME: the node uses its configured address without claiming it.
    NotStarted,  ///< Has a NAME; claims its address in its first step.
    Claiming,    ///< Sent its Address Claimed, in the 250 ms after it (messages are held unless
                 ///< the node is one of the exceptions that do not wait).
    Claimed,     ///< Owns its address.
    CannotClaim, ///< Has no address (254): lost it to a node with a higher-priority NAME and could
                 ///< not move to another one.
};

/// A change of a node's address or address state, passed to Node::on_address_change().
struct AddressChange
{
    AddressState state;     ///< The new state.
    Address address;        ///< The new address (kNullAddress without one).
    AddressState previous_state;
    Address previous_address;
    SimTime time;           ///< The time of the cause: the frame's time, or the step's time.
};

/// The delay before a node that lost its address sends Cannot Claim: 0 .. 153 ms in steps of
/// 0.6 ms, a pseudo-random function of the NAME (the same NAME always gives the same delay).
[[nodiscard]] SimDuration cannot_claim_delay(Name name) noexcept;

/// Control byte of an Acknowledgement (PGN 59392).
enum class AckCode : std::uint8_t
{
    Positive = 0,
    Negative = 1,
    AccessDenied = 2,
    CannotRespond = 3,
};

/// An Acknowledgement (PGN 59392) as received; parse_acknowledgement() reads it.
struct Acknowledgement
{
    /// The control byte. Values other than those of AckCode are kept as they came.
    AckCode code = AckCode::Positive;
    std::uint8_t group_function = 0xFF;
    /// The address acknowledged: the requester the answer is for.
    Address address = kGlobalAddress;
    /// The PGN acknowledged (the 3 bytes as received).
    Pgn pgn{};
};

/// The fields of a received Acknowledgement (bytes: control, group function, 2 reserved, the
/// address acknowledged, the PGN in 3 bytes, least significant first, as Node::acknowledge()
/// sends them). Empty if `message` is not PGN 59392 or has fewer than 8 bytes.
[[nodiscard]] std::optional<Acknowledgement> parse_acknowledgement(const Message& message) noexcept;

/// Handle of a registered handler or request responder.
struct HandlerId
{
    std::uint64_t value = 0;
    [[nodiscard]] friend constexpr bool operator==(HandlerId, HandlerId) noexcept = default;
};

/// A Request (PGN 59904) that a node received, kept to be answered later with Node::respond()
/// or Node::acknowledge() (RequestContext::defer()). A plain value: copy it, keep it as long
/// as needed. It is answered from the node's address; once the node lost or changed its address
/// after the request, the answer is refused (the request was for the old address).
class PendingRequest
{
public:
    /// The requested PGN (the first 3 bytes of the request, as received).
    [[nodiscard]] Pgn pgn() const noexcept { return pgn_; }
    [[nodiscard]] Address requester() const noexcept { return requester_; }
    /// true if the request was sent to this node, false if it was sent to all nodes.
    [[nodiscard]] bool is_destination_specific() const noexcept { return specific_; }
    [[nodiscard]] SimTime time() const noexcept { return time_; }

private:
    friend class Node;
    PendingRequest(const Node* node, std::uint32_t epoch, Pgn pgn, std::uint32_t rawPgn, Address requester, bool specific,
                   SimTime time) noexcept
      : node_(node), epoch_(epoch), pgn_(pgn), raw_pgn_(rawPgn), requester_(requester), specific_(specific), time_(time)
    {
    }

    const Node* node_;
    std::uint32_t epoch_; ///< The node's address epoch when the request came.
    Pgn pgn_;
    std::uint32_t raw_pgn_;
    Address requester_;
    bool specific_;
    SimTime time_;
};

/// A Request (PGN 59904) one node received, handed to its request responder.
///
/// Valid only during the responder call. The responder answers with respond() (sends the
/// requested group) or acknowledge(), or does neither (no frame). To answer later (after work
/// that takes steps), it keeps defer() and answers with Node::respond() or Node::acknowledge().
class RequestContext
{
public:
    RequestContext(const RequestContext&) = delete;
    RequestContext& operator=(const RequestContext&) = delete;

    /// The requested PGN (the first 3 bytes of the request, as received).
    [[nodiscard]] Pgn pgn() const noexcept { return request_.pgn(); }
    [[nodiscard]] Address requester() const noexcept { return request_.requester(); }
    /// true if the request was sent to this node, false if it was sent to all nodes.
    [[nodiscard]] bool is_destination_specific() const noexcept { return request_.is_destination_specific(); }
    [[nodiscard]] SimTime time() const noexcept { return request_.time(); }

    /// Sends the requested group with `data`. Up to 8 bytes: one frame; a PDU1 group requested
    /// by a destination-specific request goes to the requester, otherwise to all nodes. Longer
    /// (up to kMaxTransportLength): the transport protocol, to the requester (RTS/CTS) for a
    /// destination-specific request and to all nodes (BAM) otherwise; if the node's transfer
    /// that it would run beside is still going (or no transmit session is free), the response
    /// waits in the network's response queue and starts when it can (QueueFull if the queue
    /// is full).
    SendStatus respond(std::span<const std::uint8_t> data, Priority priority = kDefaultPriority);

    /// Sends an Acknowledgement with `code` to all nodes, naming the requester. Only for a
    /// destination-specific request; for a request to all nodes nothing is sent and false is
    /// returned. Also false if the control queue was full (the frame is dropped and counted).
    bool acknowledge(AckCode code);

    /// The request as a value, to answer it later (Node::respond(), Node::acknowledge()).
    /// Without an answer now the responder sends nothing for it (also no NACK).
    [[nodiscard]] PendingRequest defer() const noexcept { return request_; }

private:
    friend class Node;
    RequestContext(Node& node, const PendingRequest& request) noexcept : node_(node), request_(request) {}

    Node& node_;
    PendingRequest request_;
};

using MessageHandler = std::function<void(const Message&)>;
using RequestResponder = std::function<void(RequestContext&)>;
using AddressHandler = std::function<void(const AddressChange&)>;
using TransferHandler = std::function<void(const TransferEnd&)>;
using IncomingTransferHandler = std::function<void(const IncomingTransfer&)>;

/// The control functions on the bus that are not nodes of this network: the NAME each address
/// was last claimed with. Updated from the Address Claimed messages the network receives (not
/// from its own nodes, and not from frames shorter than 8 bytes).
/// - A NAME that claims a new address leaves its old one (the control function moved).
/// - Cannot Claim (source 254) removes its NAME from the table.
/// - Of two NAMEs claiming the same address, the table keeps the one with priority (the lower),
///   whatever order the claims come in; a claim for the address of a local node that has the
///   lower NAME is not entered (that control function loses and has to move).
/// - When a local node claims an address, a higher NAME the table has there is removed (when the
///   claim is queued, even if it does not leave).
class AddressTable
{
public:
    static constexpr std::size_t kCapacity = 254;

    /// The NAME at `address` (0..253), if a control function claimed it.
    [[nodiscard]] std::optional<Name> name_at(Address address) const noexcept;
    /// The address `name` claimed, if it is in the table.
    [[nodiscard]] std::optional<Address> address_of(Name name) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    friend class Network;
    friend class Node;
    void set(Address address, Name name) noexcept;
    void erase(Address address) noexcept;

    std::array<Name, kCapacity> names_{};
    std::array<bool, kCapacity> used_{};
    std::size_t size_ = 0;
};

/// Creation parameters of a node.
struct NodeOptions
{
    /// The node's source address (0..253): the address it claims if it has a NAME, otherwise
    /// the address it uses from the first step without claiming it (for tests). A node whose
    /// NAME is arbitrary address capable may give kNullAddress: it claims the first free address
    /// of NetworkOptions::arbitrary_address_first..last.
    Address address = kNullAddress;
    /// With a NAME the node claims `address` in its first step (AddressState).
    std::optional<Name> name{};
};

/// One control function of this process on a network: its address, its handlers and request
/// responders, and the sending API. Owned by its Network; valid as long as the Network.
///
/// Everything is called from the network's step: from handlers, responders, timers or the
/// host's application stage. Sending outside a step throws std::logic_error.
///
/// Address claiming (nodes with a NAME): in its first step the node sends Address Claimed for
/// its address and then waits 250 ms; messages it sends during the wait (also responses and
/// acknowledgements) are held (NetworkOptions::hold_capacity) and leave when the wait ends,
/// from the address it then has. A node that is not arbitrary address capable and claims an
/// address of 0..127 or 248..253 does not wait. If another node claims the same address with a
/// lower NAME (or the same NAME), the node loses the address and drops what it holds; an
/// arbitrary address capable node then claims the first free address of the network's
/// arbitrary address range and waits again, any other node (or one that finds no free address)
/// sends Cannot Claim after cannot_claim_delay(). With a higher NAME, the node claims again. A
/// Request for Address Claimed is answered with Address Claimed, or Cannot Claim after the
/// delay. Another sender using the node's address makes it claim again (an address
/// violation). Commanded Address moves an arbitrary address capable node.
class Node final : private TimerTarget
{
public:
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    /// The current source address (kNullAddress after losing it).
    [[nodiscard]] Address address() const noexcept { return address_; }
    [[nodiscard]] AddressState address_state() const noexcept { return state_; }
    [[nodiscard]] const std::optional<Name>& name() const noexcept { return name_; }
    [[nodiscard]] Network& network() noexcept { return network_; }
    /// true while what the node sends leaves at once from its address: a node without a NAME,
    /// a Claimed one, and a Claiming one that does not wait after its claim (see the class
    /// comment). false while it waits after its claim (sends are held), before its first claim
    /// and without an address. Parts that send on their own start and stop with it (it changes
    /// only with an address change, see on_address_change()).
    [[nodiscard]] bool can_send_now() const noexcept
    {
        return state_ == AddressState::Fixed || (holds_address() && !holding_);
    }

    /// Sends a message from this node's address.
    /// - Up to 8 bytes: one frame. `destination` must be kGlobalAddress for a PDU2 group.
    /// - 9 .. kMaxTransportLength bytes: the transport protocol (frames of priority 7;
    ///   `priority` is only checked). To kGlobalAddress a broadcast (BAM), to one address a
    ///   connection (RTS/CTS), also for a PDU2 group. One broadcast per node and one connection
    ///   per node and destination at a time: Busy, or with WhenBusy::Queue the transfer waits in
    ///   the network's response queue (shared with RequestContext::respond()) and starts when
    ///   the one before it ended. The end is reported to on_transfer_end().
    SendStatus send(Pgn pgn, std::span<const std::uint8_t> data, Address destination = kGlobalAddress,
                    Priority priority = kDefaultPriority, WhenBusy whenBusy = WhenBusy::Fail);

    /// The transfer that the last send() or respond() of this node (also RequestContext::respond())
    /// accepted, to find its end in on_transfer_end() (TransferEnd::id). Empty if that call sent
    /// a single frame or was refused. Read it right after the call.
    [[nodiscard]] TransferId last_transfer_id() const noexcept { return last_transfer_; }

    /// Answers a request kept with RequestContext::defer() as RequestContext::respond() would
    /// have. NoAddress if the node lost or changed its address since the request.
    /// std::invalid_argument for a request of another node.
    SendStatus respond(const PendingRequest& request, std::span<const std::uint8_t> data,
                       Priority priority = kDefaultPriority);
    /// Acknowledges a request kept with RequestContext::defer() as RequestContext::acknowledge()
    /// would have. false also if the node lost or changed its address since the request.
    /// std::invalid_argument for a request of another node.
    bool acknowledge(const PendingRequest& request, AckCode code);

    /// Sends a Request (PGN 59904, 3 bytes, priority 6) for `pgn` to `destination`. A node
    /// without an address may only request Address Claimed (from address 254). A node with a
    /// NAME answers its own Request for Address Claimed to all nodes or to itself.
    SendStatus request(Pgn pgn, Address destination = kGlobalAddress);

    /// Calls `handler` for every received message of `pgn` sent to this node's address or to
    /// all nodes (also Requests and Acknowledgements).
    HandlerId on_message(Pgn pgn, MessageHandler handler);
    /// Calls `handler` for every received message sent to this node's address or to all nodes.
    HandlerId on_any_message(MessageHandler handler);
    /// Answers Requests for `pgn` (see RequestContext). With several responders for the same
    /// PGN, the first registered one answers.
    HandlerId on_request(Pgn pgn, RequestResponder responder);
    /// Answers Requests for PGNs that have no responder of their own.
    HandlerId on_any_request(RequestResponder responder);
    /// Calls `handler` after every change of the node's address or address state (in the step,
    /// like a message handler).
    HandlerId on_address_change(AddressHandler handler);
    /// Calls `handler` once at the end of every transfer (message longer than 8 bytes) of this
    /// node that send() or RequestContext::respond() accepted (in the step, like a message
    /// handler).
    HandlerId on_transfer_end(TransferHandler handler);
    /// Calls `handler` at the start and at the end of every transfer this node receives: a
    /// connection (RTS/CTS) to its address, and a broadcast (BAM) of another sender (every node
    /// of the network but the sender). In the step, like a message handler.
    HandlerId on_incoming_transfer(IncomingTransferHandler handler);
    /// Removes a handler or responder. false if `id` is not registered. Removed while a message
    /// or a notification is being delivered, it is not called any more (also not for the rest
    /// of that delivery); the handler object itself is destroyed after the delivery.
    bool remove_handler(HandlerId id);

    /// Creates a protocol part T(*this, args...) that the network owns from now on. The parts
    /// of all nodes are destroyed first when the network is destroyed, the last attached first
    /// (a part that uses another one attaches it before itself). A part can also be created
    /// without attach() and owned by the application (destroy it before the network, and not
    /// in a handler of the network). Allowed everywhere, also in handlers.
    template <typename T, typename... Args>
    T& attach(Args&&... args);
    /// The part of type T attached to this node first; nullptr if none.
    template <typename T>
    [[nodiscard]] T* find() noexcept;

private:
    friend class Network;
    friend class RequestContext;
    friend class Transport;

    enum class EntryKind : std::uint8_t
    {
        Message,
        Request,
        AddressChange,
        TransferEnd,
        IncomingTransfer,
    };
    struct Entry
    {
        HandlerId id;
        EntryKind kind;
        bool any;
        bool removed = false; ///< Removed during a delivery: not called, erased after it.
        Pgn pgn;
        MessageHandler handler;
        RequestResponder responder;
        AddressHandler address_handler;
        TransferHandler transfer_handler;
        IncomingTransferHandler incoming_handler{};
    };

    /// What a node sent during the wait after the claim.
    struct Held
    {
        enum class Kind : std::uint8_t
        {
            Message,         ///< A single-frame message.
            Acknowledgement,
            Control,         ///< A transport control frame (TP.CM) of `session`.
            Transfer,        ///< The start of the transfer `session` (a waiting transmit session).
        };
        Kind kind;
        Pgn pgn;
        Address destination;
        Priority priority;
        std::uint8_t length;
        std::array<std::uint8_t, 8> data;
        AckCode code;
        Address requester;
        std::uint32_t raw_pgn;
        std::uint32_t session = 0;    ///< Control, Transfer: transport session index + 1.
        std::uint32_t generation = 0; ///< Control, Transfer: the session's generation.
    };
    /// Observer: MessageDropped of a held message or acknowledgement.
    void report_dropped(const Held& held, DropReason reason, SimTime time);

    enum TimerTag : std::uint32_t
    {
        kClaimWait = 1,
        kCannotClaim = 2,
    };

    Node(Network& network, std::size_t index, const NodeOptions& options);

    void on_timer(std::uint32_t tag) override;
    void start_claim();
    void handle_claim(const Message& message);
    void handle_commanded_address(const Message& message);
    void handle_violation(SimTime time);
    /// Sends Address Claimed again, unless a claim already answered a cause with this time.
    void claim_again(SimTime cause);
    void lose_address(SimTime since);
    /// Claims `address` (a new one, or the first one) and starts the wait after the claim.
    void claim(Address address, SimTime since);
    void schedule_cannot_claim(SimTime since);
    void send_claim(Address source);
    void release_held();
    /// Drops what the node holds (counted) without calling a handler: the address change that
    /// follows ends a waiting transfer (Transport::close_node_sessions()).
    void drop_held(SimTime time);
    /// Calls the address handlers if the state or the address differs from the previous ones.
    void notify_change(AddressState previousState, Address previousAddress, SimTime time);
    [[nodiscard]] bool holds_address() const noexcept
    {
        return state_ == AddressState::Claiming || state_ == AddressState::Claimed;
    }

    HandlerId add_entry(Entry entry);
    void add_part(const std::type_info& type, void* part, void (*destroy)(void*) noexcept);
    [[nodiscard]] void* find_part(const std::type_info& type) const noexcept;
    void deliver(const Message& message);
    void handle_request(const Message& message);
    void apply_pending();
    /// `queue_if_busy`: a transfer that cannot start now waits in the response queue
    /// (RequestContext::respond()).
    SendStatus send_from(Pgn pgn, std::span<const std::uint8_t> data, Address destination, Priority priority,
                         bool queue_if_busy = false);
    bool send_acknowledgement(AckCode code, Address requester, std::uint32_t rawPgn);
    SendStatus request_from(Pgn pgn, Address destination);
    /// Observer: SendRequested (`wait`: the node could not send now when it was asked).
    void report_send(Pgn pgn, Priority priority, Address destination, std::span<const std::uint8_t> data,
                     SendStatus status, bool wait);
    /// Queues (or, during the wait after the claim, holds) a transport control frame (TP.CM,
    /// priority 7) for `destination`. false if it was dropped (queue or hold area full).
    bool send_transport_control(Address destination, const std::array<std::uint8_t, 8>& data, std::uint32_t session,
                                std::uint32_t generation);
    /// `source`: the address the transfer was sent from (for the observer).
    void notify_transfer_end(const TransferEnd& end, Address source);
    void notify_incoming(const IncomingTransfer& transfer);

    Network& network_;
    std::size_t index_;
    Address address_;
    std::optional<Name> name_;
    AddressState state_;
    bool active_ = true;      ///< false until the network's next begin() if added during a step.
    /// The time of the cause of the node's last claim: the frame it lost to or the command (a
    /// move), the contention it won, the violation, or the step time (the first claim). One claim
    /// answers every cause with the same time.
    std::optional<SimTime> last_claim_cause_;
    /// Changes with every change of the address: queued frames of an older epoch are dropped
    /// (also if the node came back to the same address).
    std::uint32_t epoch_ = 0;
    bool holding_;
    TransferId last_transfer_{}; ///< See last_transfer_id().
    std::vector<Held> held_;
    TimerId claim_wait_{};
    TimerId cannot_claim_timer_{};
    std::vector<Entry> entries_;
    std::vector<Entry> pending_add_;
    bool pending_remove_ = false; ///< Entries were marked removed during a delivery.
};

/// Creation parameters and resource limits of a network. Everything is allocated when the
/// network is created.
struct NetworkOptions
{
    /// The most frames the network takes per step from its port (the rest is dropped and
    /// counted). Size it like the port's inbox: at least twice the most frames one step can
    /// bring.
    std::size_t receive_capacity = 1024;
    std::size_t max_nodes = 16;
    /// Single-frame messages of the applications waiting to be sent (Node::send()).
    std::size_t data_queue_capacity = 256;
    /// The most frames from the data queue sent in one step; the rest waits for the next step.
    /// Control frames (acknowledgements) do not count against it.
    std::size_t max_frames_per_step = 1024;
    /// The timers of address claiming (2 per node with a NAME, reserved by add_node()) and of
    /// the Timers of the applications and protocol parts (1 each, reserved when the Timer is
    /// created).
    std::size_t timer_capacity = 64;
    /// Messages one node may send during the wait after its address claim (Node).
    std::size_t hold_capacity = 16;

    // Transport protocol (messages of 9 .. kMaxTransportLength bytes). Each session has its
    // own buffer of kMaxTransportLength bytes and its own timer (on top of timer_capacity).
    /// Broadcasts (BAM) received at the same time (one per sender). A new announcement when all
    /// are in use is ignored.
    std::size_t bam_receive_sessions = 8;
    /// Connections (RTS/CTS) received at the same time (one per sender and destination). An RTS
    /// when all are in use is refused with a Connection Abort (AbortReason::Busy).
    std::size_t rts_receive_sessions = 8;
    /// Broadcasts this network's nodes send at the same time (one per node).
    std::size_t bam_transmit_sessions = 4;
    /// Connections this network's nodes send at the same time (one per node and destination).
    std::size_t rts_transmit_sessions = 4;
    /// Long responses (RequestContext::respond(), Node::respond()) and transfers sent with
    /// WhenBusy::Queue waiting for a transmit session.
    std::size_t response_queue_capacity = 8;
    /// The most packets one clear-to-send of this network's receivers allows (1..255).
    std::uint8_t cts_window = 16;
    /// Byte 5 of the RTS this network's nodes send: the most packets one clear-to-send may ask
    /// for (1..255; 255: no limit).
    std::uint8_t rts_max_packets_per_cts = 255;
    /// The time between the packets of a broadcast this network sends (10 .. 200 ms), counted
    /// from the step that sent the previous frame.
    SimDuration bam_packet_interval = std::chrono::milliseconds(50);
    /// The addresses an arbitrary address capable node chooses from (the lowest free one) when
    /// it loses its address or has none configured. Within 0..253, first <= last.
    Address arbitrary_address_first = 128;
    Address arbitrary_address_last = 247;
    /// Receives warnings (e.g. a step size the network should not run with). Empty: written to
    /// std::cerr.
    std::function<void(std::string_view)> warning;
};

/// Counters of a network.
struct NetworkStatistics
{
    std::uint64_t frames_received = 0;          ///< J1939 frames taken from the port.
    std::uint64_t frames_ignored = 0;           ///< 11-bit frames, frames with source 255, and frames
                                                ///< from 254 other than Address Claimed and Request.
    std::uint64_t receive_overflows = 0;        ///< Frames dropped because receive_capacity was reached.
    std::uint64_t frames_sent = 0;              ///< Frames the port accepted.
    std::uint64_t send_failures = 0;            ///< Frames the port could not send (dropped).
    std::uint64_t send_queue_full = 0;          ///< Flushes stopped because the port's queue was full.
    std::uint64_t frames_transmitted = 0;       ///< Port events: frames the backend reports as transmitted.
    std::uint64_t transmit_failures = 0;        ///< Port events: accepted frames the backend did not transmit.
    std::uint64_t port_receive_overflows = 0;   ///< Port events: frames the port dropped because its
                                                ///< inbox was full (as far as the port knows the number).
    std::uint64_t port_overflow_events = 0;     ///< Port events: ReceiveOverflow events.
    std::uint64_t bus_state_changes = 0;        ///< Port events: changes of the controller's error state.
    std::uint64_t error_frames = 0;             ///< Port events: error frames seen on the bus.
    std::uint64_t control_frames_dropped = 0;   ///< Control frames dropped because the control queue was full.
    std::uint64_t requests_malformed = 0;       ///< Requests shorter than 3 bytes.
    std::uint64_t held_dropped = 0;             ///< Messages and frames (also claims and acknowledgements)
                                                ///< dropped because their node lost or changed its
                                                ///< address (held or already queued), or because the
                                                ///< data queue was full when the wait after the claim
                                                ///< ended.
    std::uint64_t address_claims_malformed = 0; ///< Address Claimed shorter than 8 bytes (not a claim).
    std::uint64_t commanded_addresses_ignored = 0; ///< Commanded Address for a local NAME that was not
                                                   ///< followed: not 9 bytes, address not 0..253, the
                                                   ///< node not arbitrary address capable or not started.
    std::uint64_t address_violations = 0;       ///< Frames from another sender with the address of a
                                                ///< node that is Claiming or Claimed (other than a full
                                                ///< Address Claimed and Request for Address Claimed).
    std::uint64_t transport_messages_received = 0; ///< Messages received with the transport protocol.
    std::uint64_t transport_messages_sent = 0;     ///< Transfers of the nodes that completed.
    std::uint64_t transport_aborts_sent = 0;       ///< Connection Aborts sent (also refusals of an RTS).
    std::uint64_t transport_aborts_received = 0;   ///< Connection Aborts that ended a session.
    std::uint64_t transport_timeouts = 0;          ///< Sessions ended by a timeout (T1 .. T4).
    std::uint64_t transport_rejected = 0;          ///< RTS refused and BAM announcements ignored
                                                   ///< (invalid, or no free session).
    std::uint64_t transport_frames_ignored = 0;    ///< TP.CM and TP.DT frames that belong to no
                                                   ///< session or come in the wrong state, and
                                                   ///< malformed ones.
    std::uint64_t transport_retransmit_requests = 0; ///< Clear-to-sends asking for packets again.
    std::uint64_t transport_bam_dropped = 0;       ///< Received broadcasts dropped for a missing,
                                                   ///< repeated or out-of-order packet.
    std::uint64_t transport_sessions_cancelled = 0; ///< Sessions closed without a frame because an
                                                    ///< address changed (also queued responses).
    std::uint64_t transport_responses_dropped = 0;  ///< Long responses refused (response queue full).
    std::uint64_t transport_stale_dropped = 0;      ///< Queued TP.CM frames (announcement, RTS, CTS) not
                                                    ///< sent because their session ended or was
                                                    ///< replaced first.
};

/// One J1939 network: the nodes of this process on one CAN channel.
///
/// A vehicle_sil::can::Host steps it (begin, receive, process, the application, flush). One
/// step at `now`:
/// - process() handles the frames received before `now` and the frames the local nodes sent in
///   the previous step (local loopback; a node does not receive its own frames). Frames are
///   handled by time; frames with the same time keep the order of each sender (source address)
///   and are merged across senders by CAN identifier, lowest first, as CAN arbitration would.
///   Before each frame, the timers with a deadline before the frame's time fire; after the
///   last frame, the timers due at `now` fire (TimerKind).
/// - Handlers, responders and timers may send; the frames leave in flush(): control frames
///   first (address claims, acknowledgements, transport TP.CM), then the data queue, then the
///   transport data packets (TP.DT) made from the transmit sessions in start order (data queue
///   and packets together at most max_frames_per_step), in queue order. If the port's queue is
///   full, the frame and everything after it wait for the next step. A frame the port rejects
///   is dropped and counted.
/// - Messages of 9 .. kMaxTransportLength bytes travel with the transport protocol (BAM to all
///   nodes, RTS/CTS to one); TP.CM and TP.DT frames are not handed to the handlers, the
///   assembled message is.
///
/// Failure: an exception from a handler, a responder, a timer or the port, or a misuse (calling
/// a step function from a handler, time not increasing) makes the network failed; every later
/// step function throws std::logic_error.
///
/// Not thread safe: use it from the driver's thread only.
class Network final : public can::Steppable
{
public:
    explicit Network(NetworkOptions options = {});
    ~Network() override;
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    /// Adds a node. Outside a step, the node takes part from the next step. During a step (the
    /// host's application stage, or a handler of another network of the same host) it takes part
    /// from the next begin(): it receives nothing and does not claim before. From a handler, a
    /// responder or a timer of this network it throws std::logic_error and fails the network.
    /// std::length_error above max_nodes; std::invalid_argument if the address is not 0..253
    /// (254 is allowed for an arbitrary address capable NAME).
    Node& add_node(const NodeOptions& options);

    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] Node& node(std::size_t index) { return *nodes_.at(index); }

    /// The other control functions on the bus and their addresses.
    [[nodiscard]] const AddressTable& address_table() const noexcept { return address_table_; }
    /// The address the control function with `name` holds now: a node of this network that is
    /// Claiming or Claimed with that NAME, otherwise the address table. Empty if neither has it.
    /// Changes when an Address Claimed or a Cannot Claim is received or a local node's address
    /// changes (before the handlers of that message or change are called).
    [[nodiscard]] std::optional<Address> address_of(Name name) const noexcept;
    /// The NAME of the control function at `address` now: a node of this network that is
    /// Claiming or Claimed there, otherwise the address table. Empty if neither has one (also for
    /// a node without a NAME).
    [[nodiscard]] std::optional<Name> name_at(Address address) const noexcept;

    // can::Steppable
    /// Checks the step size: above 50 ms a warning, above 200 ms std::invalid_argument (the
    /// network could not keep the 200 ms response and packet intervals).
    void start(std::optional<SimDuration> step_size) override;
    void begin(SimTime now) override;
    void receive(const can::TimedFrame& frame) override;
    /// Counts the port's events (NetworkStatistics) and keeps the latest bus state
    /// (bus_state()); the protocol does not use them.
    void on_port_event(const can::PortEvent& event) override;
    void process() override;
    void flush(can::Port& port) override;

    /// The earliest deadline of the running protocol timers. A Scheduled timer fires in the
    /// first step with now >= deadline, a Timeout in the first step with now > deadline.
    /// Work for the next flush without a timer (transport packets a clear-to-send asked for, a
    /// port that was full) is not included: a driver that skips to this deadline must still
    /// step while there is any (a RealTimeDriver does within its max_idle).
    [[nodiscard]] std::optional<SimTime> next_deadline() const noexcept override { return timers_.next_deadline(); }
    /// The latest bus state the port reported (PortEvent::BusStateChanged); empty if none.
    /// Only reported: the network keeps running as before in every state.
    [[nodiscard]] std::optional<can::BusState> bus_state() const noexcept { return bus_state_; }
    [[nodiscard]] const NetworkStatistics& statistics() const noexcept { return statistics_; }
    [[nodiscard]] bool failed() const noexcept { return phase_ == Phase::Failed; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    /// Time of the current (or last) step.
    [[nodiscard]] std::optional<SimTime> now() const noexcept { return now_; }
    /// The time of the event being handled, which Timer::start_after() counts from: the time of
    /// the message being delivered (handlers, responders), of the address change or transfer end
    /// being notified (AddressChange::time, TransferEnd::time), the deadline of the Timer firing;
    /// otherwise the step time (time 0 before the first step). Where the event has a time of its
    /// own (a frame, a deadline) it does not depend on the step size; in the application stage,
    /// outside a step and for events of the step itself (the end of the wait after a claim) it
    /// is the step time, which does.
    [[nodiscard]] SimTime event_time() const noexcept;

    /// Sets the Observer that watches this network (nullptr: none), replacing the previous one
    /// (an observer watching another network leaves it). Only outside a step and not from an
    /// observer (std::logic_error); after the network failed, anywhere. See Observer.
    void set_observer(Observer* observer);
    [[nodiscard]] Observer* observer() const noexcept { return observer_; }

private:
    friend class Node;
    friend class Observer;
    friend class RequestContext;
    friend class Subscription;
    friend class Timer;
    friend class Transport;
    friend struct detail::NetworkTestAccess;

    enum class Phase : std::uint8_t
    {
        Idle,        ///< Between steps.
        Receiving,   ///< After begin().
        Processing,  ///< Inside process().
        Application, ///< After process(), before flush().
        Flushing,
        Failed,
    };

    struct Received
    {
        SimTime time;
        std::uint32_t arrival;
        std::int32_t sender; ///< Local node index for looped-back frames, -1 for external ones.
        can::Frame frame;
    };

    struct Outgoing
    {
        can::Frame frame;
        std::int32_t sender;
        std::uint32_t epoch;          ///< The sender's address epoch when it was queued.
        std::uint32_t session = 0;    ///< Transport session index + 1 of a TP.CM frame, or 0.
        std::uint32_t generation = 0; ///< That session's generation.
    };

    /// A fixed-capacity FIFO.
    class Queue
    {
    public:
        explicit Queue(std::size_t capacity) : items_(capacity) {}
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
        [[nodiscard]] bool full() const noexcept { return size_ == items_.size(); }
        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        bool push(const Outgoing& item) noexcept;
        [[nodiscard]] const Outgoing& front() const noexcept { return items_[head_]; }
        void pop() noexcept;

    private:
        std::vector<Outgoing> items_;
        std::size_t head_ = 0;
        std::size_t size_ = 0;
    };

    /// A part owned by the network (Node::attach()).
    struct Attached
    {
        std::size_t node;
        std::type_index type;
        void* part;
        void (*destroy)(void*) noexcept;
    };

    /// Hands a complete message to the nodes as process() does with a received frame (the
    /// address table, address claiming, the handlers). For tests (NetworkTestAccess). Only in
    /// the host's application stage; `message.time` must not be after the step time. Messages
    /// that receive() would ignore (source 255, from 254 other than Address Claimed and Request,
    /// an invalid PGN) are ignored and counted (frames_ignored).
    void deliver_message(const Message& message);
    template <typename F>
    void guarded(F&& f);
    void fail(const std::string& error);
    void require_not_failed() const;
    void require_not_reentrant(const char* what);
    void require_in_step(const char* what);
    /// Throws std::logic_error (and fails the network) while an observer runs.
    void require_not_observing(const char* what);
    /// Fails the network if an observer runs (for the noexcept paths); returns true then.
    bool fail_if_observing(const char* what) noexcept;
    [[nodiscard]] bool observed() const noexcept { return observer_ != nullptr; }
    /// Calls the observer (if any) with `event`; see Observer.
    void observe(const Event& event);
    void warn(std::string_view text) const;

    void order_received();
    void dispatch(const Received& received);
    /// Hands a message to the nodes (except `sender`, a local node index or -1). `check_violation`:
    /// look for another sender using a node's address.
    void dispatch_message(const Message& message, std::int32_t sender, bool check_violation = true);
    void check_violations(const Message& message, std::int32_t sender);
    /// Erases `address` from the address table and closes the transport sessions with it
    /// (`time`: the time of the cause).
    void table_erase(Address address, SimTime time);
    /// Sets `name` at `address`; if another NAME was there, closes the sessions with it.
    void table_set(Address address, Name name, SimTime time);
    void update_address_table(const Message& message);
    /// The lowest address of the arbitrary address range that neither the table nor another
    /// local node uses; kNullAddress if there is none.
    [[nodiscard]] Address free_address(const Node& node) const noexcept;
    void apply_pending();
    void throw_if_failed_during_step() const;
    void fire_timers_before(SimTime time);
    void fire_due_timers();
    /// Sends the queue's frames until it is empty, `limit` frames were sent, or the port is
    /// full. false if the port was full. `sent`: the frames the port accepted.
    bool flush_queue(Queue& queue, can::Port& port, std::size_t limit, std::size_t& sent);
    /// Hands one frame of a local node to the port: counts it and keeps it for the local
    /// loopback if accepted.
    can::SendResult send_frame(const can::Frame& frame, std::int32_t sender, can::Port& port);

    bool enqueue_data(const can::Frame& frame, std::size_t sender) noexcept;
    bool enqueue_control(const can::Frame& frame, std::size_t sender, std::uint32_t session = 0,
                         std::uint32_t generation = 0) noexcept;

    NetworkOptions options_;
    /// Expires when the network is destroyed: Subscriptions and Timers that outlive it see it.
    std::shared_ptr<const bool> alive_;
    std::vector<std::unique_ptr<Node>> nodes_;
    AddressTable address_table_;
    TimerSet timers_;
    Queue control_;
    Queue data_;
    std::unique_ptr<Transport> transport_;

    std::vector<Received> received_;      ///< This step's frames (external and loopback).
    std::size_t external_received_ = 0;   ///< External frames in received_ this step.
    std::vector<Received> loopback_;      ///< Frames sent in the last flush, for the other local nodes.
    std::vector<std::uint32_t> order_;    ///< Indices into received_ in handling order.
    std::vector<std::int32_t> next_in_sender_;
    std::vector<std::uint8_t> active_senders_;
    std::int32_t sender_head_[256]{};
    std::int32_t sender_tail_[256]{};

    Phase phase_ = Phase::Idle;
    bool dispatching_ = false;
    std::optional<SimTime> now_;
    std::optional<SimTime> event_time_; ///< See event_time(); set while delivering and firing.
    std::uint64_t next_handler_id_ = 1;
    NetworkStatistics statistics_;
    std::optional<can::BusState> bus_state_;
    std::string error_;
    std::vector<Attached> attached_; ///< In attach order; destroyed last to first.
    Observer* observer_ = nullptr;
    bool observing_ = false; ///< Inside Observer::on_event().
};

template <typename T, typename... Args>
T& Node::attach(Args&&... args)
{
    auto part = std::make_unique<T>(*this, std::forward<Args>(args)...);
    add_part(typeid(T), part.get(), [](void* p) noexcept { delete static_cast<T*>(p); });
    return *part.release();
}

template <typename T>
T* Node::find() noexcept
{
    return static_cast<T*>(find_part(typeid(T)));
}

} // namespace vehicle_sil::j1939
