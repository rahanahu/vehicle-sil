// SPDX-License-Identifier: MIT
//
// Listener: hears what the other participants say on a J1939 bus, also what is not addressed to
// a node of its Network: single frames, broadcasts (BAM) and the connections (RTS/CTS) between
// two other ECUs. It only listens: it sends nothing, claims no address and answers nothing.
//
// A participant that only listens has a Network without a node and a Listener on it. A
// participant that also talks adds nodes to the same Network (see ListenerOptions::hear_local
// for what the Listener hears of them).
//
// NOT COMPARED WITH THE STANDARD. The connections between other ECUs are followed by a
// TransferSniffer (transfer_sniffer.hpp), whose rules follow the receiver of this repository's
// engine; its values can be changed in ListenerOptions::sniffer.
#pragma once

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/j1939/transfer_sniffer.hpp"
#include "vehicle_sil/j1939/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace vehicle_sil::j1939 {

class Network;

/// One frame the Network took from the bus, with its identifier decoded: TP.CM and TP.DT and
/// frames to other addresses included. For code that assembles or checks frames itself.
struct ReceivedFrame
{
    SimTime time{};
    can::Frame frame{};
    Pgn pgn{};
    Priority priority = kDefaultPriority;
    Address source = kNullAddress;
    Address destination = kGlobalAddress;
    /// Sent by a node of the Listener's own Network. Always false unless
    /// ListenerOptions::hear_local is set.
    bool local = false;
    std::span<const std::uint8_t> data{}; ///< The frame's data bytes, valid during the handler call only.
};

struct ListenerOptions
{
    /// How the connections between other ECUs are followed. `broadcasts` is not read: the
    /// Network assembles the broadcasts itself and the Listener hands each one on once.
    SnifferOptions sniffer{};
    /// Also hand on what the nodes of the Listener's own Network sent to each other. Off by
    /// default: the Listener hears the other participants only.
    ///
    /// On, the Listener gets the frames and messages of its Network's nodes that the Network
    /// loops back to its other nodes. That loopback exists only in a Network with two or more
    /// nodes: with a single node nothing of it is heard, whatever this says. So this is not
    /// the way to count what a node sent; use the node's own results for that
    /// (Node::on_transfer_end(), the result of a Requester). The connections of the Network's
    /// own nodes are not reported by on_transfer() either way, and a connection (RTS/CTS) from
    /// a node of this Network to another participant gives its frames only, no message: the
    /// Network assembles only what is addressed to its nodes, and the frames of its own nodes
    /// are not followed.
    bool hear_local = false;
};

/// Hears the messages and frames of the other participants on the bus of `network`.
///
/// What it hands on:
/// - on_message() / on_any_message(): every message, whoever it is addressed to. A single
///   frame and a broadcast (BAM): as the Network received it. A message to a node of this
///   Network over a connection: as the Network assembled it. A connection between two other
///   ECUs: as the TransferSniffer assembled it from the frames. Each message once.
/// - on_transfer(): how each connection between two other ECUs ended, also those that gave no
///   message (aborted, timed out, missed packets).
/// - on_frame(): every frame, before anything else that follows from it.
///
/// What it does not hear: what the nodes of its own Network sent (ListenerOptions::hear_local),
/// frames the Network ignores (NetworkStatistics::frames_ignored: 11-bit frames, frames from
/// address 255, frames from address 254 but Address Claimed and Request, and frames it cannot
/// read), and frames the Network did not take (NetworkOptions::receive_capacity:
/// NetworkStatistics::receive_overflows). A broadcast the Network had no free session for is
/// not assembled, and neither is a connection to a node of this Network that the node refused
/// for lack of a session (both NetworkStatistics::transport_rejected). Connections the sniffer
/// had no place for or lost track of are counted in sniffer_statistics() (no_session,
/// incomplete, replaced, lost): publish those counters, so that a test sees when the Listener
/// missed something (vehicle_sil::spy::Spy publishes them).
///
/// The message of a connection between two other ECUs is, for a transfer whose
/// SniffedTransfer::conflicting_packets is 0, the message the receiving node of this
/// repository's engine gets for the same frames: group, priority, source, destination, time and
/// bytes. Code that reads a Message in a node's handler can read it here. A Message does not
/// say whether its transfer had conflicting packets (a packet seen twice with different bytes:
/// the receiver may have taken the other one). That is known from the on_transfer() handlers,
/// which are called for the same transfer right after the message handlers
/// (SniffedTransfer::conflicting_packets), and from sniffer_statistics().conflicting_packets,
/// which counts them all: while it is 0, every message handed on was the receiver's. Publish
/// that counter with the others, or take a value only when on_transfer() confirmed it.
/// With SniffDelivery::WhenAllPacketsSeen the receiver may also have dropped a transfer that is
/// handed on here (it had no chance to say so; e.g. the sender's address changed hands, which
/// the sniffer does not read).
///
/// Addresses of this Network's nodes: a frame from or to an address that a node of this Network
/// uses (a node without a NAME, or one that is claiming or has claimed the address) is not a
/// conversation between other ECUs. The Network handles it, and the sniffer does not follow it;
/// this is decided frame by frame. If a node takes an address while a connection from or to
/// that address is followed, that transfer ends as Lost with the next frame from or to the
/// address. After the node lost the address, its conversations are followed again.
///
/// Order, for one frame: first the on_frame() handlers. Then what the frame's time or address
/// ended of other transfers (TimedOut, Incomplete, Lost): their on_transfer() handlers. Then
/// what the frame itself brings: a connection between other ECUs that it completes is handed to
/// the message handlers and right after to the on_transfer() handlers (any other end, such as
/// an abort: the on_transfer() handlers only); a single frame, or a message the Network
/// assembled with it, is handed to the message handlers. So the end of another transfer can be
/// reported before the message of the frame that made the sniffer notice it: compare the times.
/// For one message: the Network's Observer, then the Listener, then the handlers of the
/// Network's nodes. Handlers of one kind run in the order they were registered (on_message()
/// and on_any_message() are one list).
///
/// Times. Message::time of a sniffed connection is the time of the data packet with which the
/// receiver had the message, but by default the handler runs when the receiver acknowledged it
/// (SniffDelivery::WhenAcknowledged), which is later. So a handler can be called with a time
/// earlier than that of the message before: code that keeps "the latest" or measures a period
/// compares Message::time. What follows from frames (a message, an abort) does not depend on
/// the step size of the Listener's participant. What follows from time passing (TimedOut, an
/// acknowledgement while packets were missing) is decided at the end of the step's frames,
/// before the Timers due at the step time fire; its `time` does not depend on the step size,
/// but the step in which the handler runs does: compare `time`, not the order of the calls.
/// With a bridge to a real bus on the network neither times nor the order of frames is exact.
///
/// Handlers only read. They run inside the Network's step: calling into the Network or its
/// nodes (sending, registering a handler, adding a node, starting a Timer) throws
/// std::logic_error and fails the Network, and so does an exception a handler lets out. A
/// handler that catches such an exception does not keep the step alive. Update variables in the
/// handlers and send from the application's step. References and spans are valid during the
/// call only.
///
/// A Network has one Listener. Code that watches something takes a Listener& and adds its
/// handlers; it does not create one.
///
/// Lifetime of the handlers. A handler stays registered, and is called, until it is removed or
/// the Listener is destroyed: whatever it captured must live that long. Code that may go away
/// before the Listener keeps each registration in a ListenerSubscription member, which removes
/// the handler when it is destroyed (the same pattern as Subscription for a Node's handlers):
///     ListenerSubscription sub{listener, listener.on_message(pgn, [this](const Message& m) { ... })};
/// A registration whose id is dropped stays for the Listener's life.
///
/// Create and destroy the Listener outside a step (it may be destroyed from a node's handler,
/// but not from one of its own handlers), before or after the Network. Not thread safe: use it
/// from the thread that steps the Network.
class Listener final
{
public:
    using MessageHandler = std::function<void(const Message&)>;
    using TransferHandler = std::function<void(const SniffedTransfer&)>;
    using FrameHandler = std::function<void(const ReceivedFrame&)>;

    /// Handle of a registered handler of this Listener (of any of the four kinds).
    struct HandlerId
    {
        std::uint64_t value = 0;
        [[nodiscard]] friend constexpr bool operator==(HandlerId, HandlerId) noexcept = default;
    };

    /// Starts listening to `network`. Only outside a step. std::logic_error if the Network has a
    /// Listener already (add the handlers to that one); std::invalid_argument for sniffer
    /// options that cannot work (see TransferSniffer). Reserves
    /// options.sniffer.sessions x kMaxTransportLength bytes; nothing is allocated in the steps.
    explicit Listener(Network& network, ListenerOptions options = {});
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    /// Adds a handler for the messages of `pgn` / for every message. Several handlers may be
    /// added, also for the same group. std::invalid_argument for an empty handler;
    /// std::logic_error from a handler of this Listener (register before the simulation starts).
    /// The id removes it again (remove_handler(), ListenerSubscription); dropped, the handler
    /// stays for the Listener's life.
    /// A message of a connection between two other ECUs has the receiver's bytes if its transfer
    /// had no conflicting packets, which the on_transfer() handlers are told right after (see
    /// the class comment).
    HandlerId on_message(Pgn pgn, MessageHandler handler);
    HandlerId on_any_message(MessageHandler handler);
    /// Adds a handler for the end of every connection between two other ECUs. A Completed one
    /// was handed to the message handlers just before. The same conditions as on_message().
    HandlerId on_transfer(TransferHandler handler);
    /// Adds a handler for every frame. The same conditions as on_message().
    HandlerId on_frame(FrameHandler handler);

    /// Removes a handler. false if `id` is not registered (any more). Allowed everywhere, also
    /// inside a step and in a handler of this Listener (the handler itself or another one): the
    /// removed handler is not called any more, not for the rest of that frame either, and the
    /// handler object is destroyed when the Listener is done with the frame, the message or the
    /// step end it is handling. Allocates nothing.
    bool remove_handler(HandlerId id) noexcept;

    /// The connections between other ECUs that are followed right now.
    [[nodiscard]] std::size_t open_transfers() const noexcept;
    /// What the sniffer counted. `lost` here: connections that ended because a node of this
    /// Network took their address.
    [[nodiscard]] const SnifferStatistics& sniffer_statistics() const noexcept;
    [[nodiscard]] const ListenerOptions& options() const noexcept;

private:
    friend class ListenerSubscription;
    class Seat;
    std::unique_ptr<Seat> seat_;
};

/// Owns one handler of a Listener (Listener::HandlerId) and removes it when destroyed or
/// reset(), as Subscription does for a handler of a Node:
///     ListenerSubscription sub{listener, listener.on_message(pgn, [this](const Message& m) { ... })};
/// After reset() or destruction the handler is not called any more, also when that happens in a
/// handler of the Listener (the handler itself or another one). Safe after the Listener was
/// destroyed (it then does nothing), whether or not the Network still exists. Moving keeps the
/// registration. Not thread safe: use it from the thread that steps the Network.
class ListenerSubscription
{
public:
    ListenerSubscription() = default;
    /// Takes over `id`, a registration of `listener`.
    ListenerSubscription(Listener& listener, Listener::HandlerId id) noexcept;
    ~ListenerSubscription() { reset(); }
    ListenerSubscription(ListenerSubscription&& other) noexcept;
    ListenerSubscription& operator=(ListenerSubscription&& other) noexcept;
    ListenerSubscription(const ListenerSubscription&) = delete;
    ListenerSubscription& operator=(const ListenerSubscription&) = delete;

    /// Removes the handler now (nothing if it is already removed).
    void reset() noexcept;
    /// true while it holds a registration (and the Listener exists).
    [[nodiscard]] bool active() const noexcept { return listener_ != nullptr && !alive_.expired(); }

private:
    Listener* listener_ = nullptr;
    Listener::HandlerId id_{};
    std::weak_ptr<const bool> alive_;
};

} // namespace vehicle_sil::j1939
