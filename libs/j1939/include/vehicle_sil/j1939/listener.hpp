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
    /// own nodes are not reported by on_transfer() either way.
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
/// 11-bit frames, and frames the Network did not take (NetworkOptions::receive_capacity:
/// NetworkStatistics::receive_overflows). A broadcast the Network had no free session for is
/// not assembled (NetworkStatistics::transport_rejected). Connections the sniffer had no place
/// for or lost track of are counted in sniffer_statistics() (no_session, incomplete, replaced,
/// lost): publish those counters, so that a test sees when the Listener missed something.
///
/// The message of a connection between two other ECUs is, for a transfer whose
/// SniffedTransfer::conflicting_packets is 0, the message the receiving node of this
/// repository's engine gets for the same frames: group, priority, source, destination, time and
/// bytes. Code that reads a Message in a node's handler can read it here.
///
/// Addresses of this Network's nodes: a frame from or to an address that a node of this Network
/// uses (a node without a NAME, or one that is claiming or has claimed the address) is not a
/// conversation between other ECUs. The Network handles it, and the sniffer does not follow it;
/// this is decided frame by frame. If a node takes an address while a connection from or to
/// that address is followed, that transfer ends as Lost with the next frame from or to the
/// address. After the node lost the address, its conversations are followed again.
///
/// Order, for one frame: the on_frame() handlers, then the messages the frame completed, then
/// the on_transfer() handlers. For one message: the Network's Observer, then the Listener, then
/// the handlers of the Network's nodes. Handlers of one kind run in the order they were
/// registered (on_message() and on_any_message() are one list).
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
/// handlers; it does not create one. Handlers cannot be removed (destroy the Listener).
/// Create and destroy it outside a step (it may be destroyed from a node's handler, but not from
/// one of its own handlers), before or after the Network. Not thread safe: use it from the
/// thread that steps the Network.
class Listener final
{
public:
    using MessageHandler = std::function<void(const Message&)>;
    using TransferHandler = std::function<void(const SniffedTransfer&)>;
    using FrameHandler = std::function<void(const ReceivedFrame&)>;

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
    void on_message(Pgn pgn, MessageHandler handler);
    void on_any_message(MessageHandler handler);
    /// Adds a handler for the end of every connection between two other ECUs. A Completed one
    /// was handed to the message handlers just before. The same conditions as on_message().
    void on_transfer(TransferHandler handler);
    /// Adds a handler for every frame. The same conditions as on_message().
    void on_frame(FrameHandler handler);

    /// The connections between other ECUs that are followed right now.
    [[nodiscard]] std::size_t open_transfers() const noexcept;
    /// What the sniffer counted. `lost` here: connections that ended because a node of this
    /// Network took their address.
    [[nodiscard]] const SnifferStatistics& sniffer_statistics() const noexcept;
    [[nodiscard]] const ListenerOptions& options() const noexcept;

private:
    class Seat;
    std::unique_ptr<Seat> seat_;
};

} // namespace vehicle_sil::j1939
