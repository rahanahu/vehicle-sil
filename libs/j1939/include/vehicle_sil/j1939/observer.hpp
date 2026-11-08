// SPDX-License-Identifier: MIT
//
// Read-only observation of what a J1939 network does (vehicle_sil::j1939): address changes,
// send requests, frames handed to the port, received messages, transfers, port events.
#pragma once

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/can/port.hpp"
#include "vehicle_sil/j1939/network.hpp"
#include "vehicle_sil/j1939/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace vehicle_sil::j1939 {

/// What an Event reports.
enum class EventKind : std::uint8_t
{
    /// A node's address or address state changed (`address`), before its address handlers run.
    /// time: AddressChange::time.
    AddressChanged,
    /// Node::send(), Node::request() or a response to a request (Node::respond(),
    /// RequestContext::respond(); NoAddress for a request to an address the node no longer
    /// has) returned `status`. pgn, priority, destination, size and data are what was asked
    /// (for request(): the Request, PGN 59904, with the requested PGN as data). `held`: Ok, but
    /// the node is in the wait after its claim; the message (or the transfer) leaves when the
    /// wait ends. Acknowledgements (acknowledge()) are not reported here: see FrameSent.
    /// time: the step's time.
    SendRequested,
    /// A message the node was going to send is not sent (`drop_reason`): a message or an
    /// acknowledgement held during the wait after the claim, or a frame queued before the node
    /// lost or changed its address. pgn, destination, size (held ones; an acknowledgement has
    /// PGN 59392) or `frame` (queued frames). time: the cause's time (held ones: the frame that
    /// took the address, or the step's time when the wait ended), the step's time (queued
    /// frames).
    MessageDropped,
    /// The network handed `frame` to the port (`send_result`). node: the node that sent it.
    /// time: the step's time (the frame's TimedFrame::time).
    FrameSent,
    /// A message (a single frame, or a message a transfer delivered) is handed to the nodes,
    /// before any handler runs. pgn, priority, source, destination, size, data. `local`: sent by
    /// node `node` of this network (the next step's loopback); otherwise node is -1. Transport
    /// frames (TP.CM, TP.DT) are not reported here: see TransferStarted and TransferEnded.
    /// time: the message's time.
    MessageReceived,
    /// A transfer (a message longer than 8 bytes) starts. Send: send() or respond() accepted it
    /// (it may wait in the response queue or after the claim). Receive: an RTS to a local node
    /// or a BAM was accepted (node -1 for a BAM). pgn, source, destination, size, direction.
    /// time: the step's time (send), the RTS or BAM's time (receive).
    TransferStarted,
    /// A transfer ended: `result` and `reason`. Every transfer that started ends once (unless
    /// the network fails or is destroyed first). Receive: Completed (the message follows as
    /// MessageReceived), AbortedByPeer (the originator sent a Connection Abort; reason is its
    /// reason), Aborted (this node sent one, e.g. after a timeout), Lost (a BAM missed a packet,
    /// reason BadSequence, or timed out, reason Timeout; nothing is sent), Cancelled (the
    /// originator's address was claimed by another NAME or given up, the local node lost its
    /// address, a new RTS or BAM from the same sender replaced it, the clear-to-send could not
    /// be queued, or the transfer carried Address Claimed or a Request, which are single-frame
    /// groups). Send: as TransferEnd; source is the address the transfer was sent from (it
    /// differs from TransferStarted's if the node moved before a held transfer began).
    /// time: TransferEnd::time (the cause).
    TransferEnded,
    /// The port reported `port_event` (Network::on_port_event()). time: the event's time.
    PortEvent,
};

/// Which side of a transfer an Event is about.
enum class TransferDirection : std::uint8_t
{
    Send,
    Receive,
};

/// Why a message was not sent (EventKind::MessageDropped).
enum class DropReason : std::uint8_t
{
    /// The node lost or changed its address before it could send it.
    AddressChanged,
    /// Held during the wait after the claim; when the wait ended the data queue (or, for an
    /// acknowledgement, the control queue) was full.
    QueueFull,
};

/// One thing a network did. Which fields are set depends on `kind` (see EventKind); the others
/// have their default values. `data` is valid for the call only.
struct Event
{
    EventKind kind = EventKind::PortEvent;
    SimTime time{};
    std::int32_t node = -1; ///< Index of the local node (Network::node()), -1 if none.
    Pgn pgn{};
    Priority priority = kDefaultPriority;
    Address source = kNullAddress;
    Address destination = kGlobalAddress;
    std::size_t size = 0;                   ///< Bytes of the message or transfer.
    std::span<const std::uint8_t> data{};   ///< SendRequested, MessageReceived.
    SendStatus status = SendStatus::Ok;     ///< SendRequested.
    bool held = false;                      ///< SendRequested.
    bool local = false;                     ///< MessageReceived.
    can::Frame frame{};                     ///< FrameSent, MessageDropped of a queued frame.
    can::SendResult send_result = can::SendResult::Accepted; ///< FrameSent.
    TransferDirection direction = TransferDirection::Send;   ///< TransferStarted, TransferEnded.
    TransferResult result = TransferResult::Completed;       ///< TransferEnded.
    AbortReason reason = AbortReason::None;                  ///< TransferEnded.
    AddressChange address{AddressState::NotStarted, kNullAddress, AddressState::NotStarted, kNullAddress, SimTime{}}; ///< AddressChanged.
    DropReason drop_reason = DropReason::AddressChanged;     ///< MessageDropped.
    can::PortEvent port_event{};                             ///< PortEvent.
};

/// Watches one Network (Network::set_observer()) without taking part: the network behaves the
/// same with or without it.
///
/// Read only: from on_event(), sending (send, request, respond, acknowledge), registering or
/// removing handlers, adding nodes or parts, starting or cancelling a Timer of the network and
/// calling its step functions are errors: they throw std::logic_error (removing a handler and
/// cancelling a timer return false instead, since destructors use them) and the network
/// becomes failed. An exception from on_event() also fails the network.
///
/// on_event() runs on the driver's thread, inside the step, synchronously, at the point the
/// EventKind describes. It must return quickly; copy what it wants to keep.
///
/// Lifetime: an Observer watches at most one network. Destroying the observer detaches it;
/// destroying the network first is safe too. Do not destroy it from on_event().
class Observer
{
public:
    virtual void on_event(const Event& event) = 0;

protected:
    Observer() = default;
    Observer(const Observer&) = delete;
    Observer& operator=(const Observer&) = delete;
    virtual ~Observer();

private:
    friend class Network;
    Network* network_ = nullptr;
};

} // namespace vehicle_sil::j1939
