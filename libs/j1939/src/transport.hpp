// SPDX-License-Identifier: MIT
//
// The transport protocol of a J1939 network (internal to vehicle_sil::j1939): broadcast
// (BAM) and connection (RTS/CTS) sessions, sending and receiving messages of 9 .. 1785 bytes.
#pragma once

#include "vehicle_sil/j1939/network.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vehicle_sil::j1939 {

inline constexpr Pgn kPgnTransportControl{0xEC00u};
inline constexpr Pgn kPgnTransportData{0xEB00u};
inline constexpr Priority kTransportPriority = 7;

/// Owned by its Network; uses the private parts of Network and Node.
class Transport
{
public:
    Transport(Network& network, const NetworkOptions& options);

    /// The number of sessions (each needs one timer).
    [[nodiscard]] std::size_t session_count() const noexcept { return sessions_.size(); }

    /// Node::send() of 9 .. kMaxTransportLength bytes (already checked).
    SendStatus send(Node& node, Pgn pgn, std::span<const std::uint8_t> data, Address destination);
    /// RequestContext::respond() of a long message: sends it, or queues it if the node's
    /// transfer it would run beside is going or no session is free.
    SendStatus respond(Node& node, Pgn pgn, std::span<const std::uint8_t> data, Address destination);

    /// A TP.CM or TP.DT frame (from a local node `sender`, or -1).
    void on_frame(const Message& message, std::int32_t sender);
    /// The session `tag` still has `generation` (its control frames may leave).
    [[nodiscard]] bool is_current(std::uint32_t tag, std::uint32_t generation) noexcept
    {
        return find(tag, generation) != nullptr;
    }
    /// A tagged control frame left the control queue (accepted, or failed and dropped).
    void on_control_sent(std::uint32_t session, std::uint32_t generation);
    /// A held control frame of `session` did not fit the control queue: a receive session is
    /// released (no timer would end it).
    void on_control_dropped(std::uint32_t session, std::uint32_t generation);
    /// The node released what it held: the waiting transfer `session` starts now.
    void start_waiting(std::uint32_t session, std::uint32_t generation);
    /// The node is about to lose or change its address: its sessions and queued responses end
    /// without a frame.
    void close_node_sessions(std::size_t node, SimTime time);
    /// Another control function left `address` (or another NAME took it): the sessions with it
    /// end without a frame.
    void close_peer_sessions(Address address, SimTime time);

    /// Starts the queued responses that can start (at the beginning of flush()).
    void pump_responses();
    /// Sends the transport data packets (after the queues): at most `limit` frames.
    void flush_data(can::Port& port, std::size_t limit);

private:
    enum class Kind : std::uint8_t
    {
        BamTx,
        RtsTx,
        BamRx,
        RtsRx,
    };
    enum class State : std::uint8_t
    {
        Free,
        // Transmit
        Waiting,     ///< The node waits after its claim; the transfer starts when it ends.
        BamAnnounce, ///< The announcement is queued.
        BamSending,  ///< `ready`: the next packet leaves at the next flush.
        BamFinished, ///< The last packet left; ends (Completed) at the next step.
        RtsSent,     ///< The RTS is queued (no timer) or sent (T3).
        Sending,     ///< Packets next .. window_end leave at the next flush.
        WaitCts,     ///< A window was sent; T3 for the next clear-to-send.
        Hold,        ///< The receiver holds the connection; T4.
        WaitEoma,    ///< Every packet was sent; T3 for the acknowledgement.
        // Receive
        BamReceiving, ///< T1.
        CtsPending,   ///< A clear-to-send is queued (or held); T2 starts when it leaves.
        WaitData,     ///< T2 for the first packet of the window.
        Receiving,    ///< T1 between packets.
    };

    struct Session final : TimerTarget
    {
        Transport* transport = nullptr;
        std::uint32_t index = 0;
        std::uint32_t generation = 1;
        Kind kind = Kind::BamTx;
        State state = State::Free;
        std::int32_t node = -1;   ///< The local node (originator or responder); -1 for BamRx.
        Address source = 0;       ///< The originator's address.
        Address destination = 0;  ///< The responder's address, or kGlobalAddress.
        std::uint32_t raw_pgn = 0;
        Priority priority = kTransportPriority; ///< Receive: the priority of the RTS or announcement.
        std::uint16_t length = 0;
        std::uint8_t packets = 0;
        std::uint16_t next = 1;         ///< Transmit: next packet to send. Receive: next expected.
        std::uint16_t window_start = 1;
        std::uint16_t window_end = 0;
        std::uint16_t sent_high = 0;    ///< Transmit: highest packet sent.
        std::uint8_t rts_limit = 0xFF;  ///< Byte 5 of the RTS.
        std::uint8_t retries = 0;       ///< Receive: retransmission requests since the last full window.
        bool gap = false;               ///< Receive: a packet was missed in this window.
        bool ready = false;             ///< BamSending: the interval has passed.
        std::uint64_t order = 0;        ///< Start order (transmit packets leave in this order).
        std::uint64_t transfer = 0;     ///< The TransferId (receive: once announced to the nodes).
        std::int32_t rx_sender = -1;    ///< BamRx: the local node that sent it, or -1.
        bool announced = false;         ///< Receive: the nodes were told it started.
        std::uint32_t rx_nodes = 0;     ///< BamRx: the nodes 0 .. rx_nodes-1 were told (the end goes to them).
        TimerId timer{};
        SimTime deadline{};             ///< The deadline of `timer`.
        std::vector<std::uint8_t> buffer;

        void on_timer(std::uint32_t tag) override;
    };

    struct Response
    {
        bool used = false;
        std::uint64_t order = 0;
        std::uint64_t transfer = 0; ///< The TransferId.
        std::uint32_t node = 0;
        Address source = kNullAddress; ///< The node's address when it was queued.
        Pgn pgn{};
        Address destination = kGlobalAddress;
        std::uint16_t length = 0;
        std::vector<std::uint8_t> data;
    };

    [[nodiscard]] Session* find(std::uint32_t tag, std::uint32_t generation) noexcept;
    [[nodiscard]] Session* allocate(Kind kind) noexcept;
    [[nodiscard]] Session* find_rx(Kind kind, Address source, Address destination) noexcept;
    /// A transmit session of `node` that conflicts with a new transfer to `destination`.
    [[nodiscard]] bool busy(std::size_t node, Address destination) const noexcept;
    /// The local node that answers an RTS to `address` (not `sender`), or -1.
    [[nodiscard]] std::int32_t responder_for(Address address, std::int32_t sender) const noexcept;
    [[nodiscard]] Node& node_of(const Session& session) const;
    /// `destination` is kGlobalAddress or the address of an active local node other than
    /// `sender` (frames to other nodes on the bus are not counted as ignored).
    [[nodiscard]] bool for_us(Address destination, std::int32_t sender) const noexcept;
    /// The transmit session of a local node at `local` (not `sender`) with `peer` for `raw_pgn`.
    [[nodiscard]] Session* find_tx(Address local, Address peer, std::uint32_t rawPgn, std::int32_t sender) noexcept;
    /// Completes a receive session: delivers its message (and closes the session).
    void deliver(Session& session, SimTime time, std::int32_t sender);

    /// Creates a transmit session and starts it (or, while the node waits after its claim,
    /// holds its start). QueueFull: no free session, or the frame or hold area is full.
    /// `transfer`: its TransferId.
    SendStatus open_transmit(Node& node, Pgn pgn, std::span<const std::uint8_t> data, Address destination,
                             std::uint64_t transfer);
    /// Sends the announcement or the RTS. false if the frame could not be queued.
    bool begin_transmit(Session& session);

    void arm(Session& session, TimerKind kind, SimTime deadline);
    void disarm(Session& session) noexcept;
    /// Frees the session (no frame, no notification).
    void release(Session& session) noexcept;
    /// Observer: TransferStarted of a receive session.
    void report_receive_start(const Session& session, SimTime time);
    /// Ends a receive session: TransferEnded to the observer, then release().
    void end_receive(Session& session, TransferResult result, AbortReason reason, SimTime time);
    /// The observer's part of end_receive() and the release.
    void end_receive_released(Session& session, TransferResult result, AbortReason reason, SimTime time);
    /// Ends a transmit session and tells its node.
    void finish(Session& session, TransferResult result, AbortReason reason, SimTime time);
    /// Sends a Connection Abort from `node` to `destination` for `raw_pgn`.
    void send_abort(std::size_t node, Address destination, std::uint32_t rawPgn, AbortReason reason);
    /// Aborts a session (sends the abort, ends it; transmit sessions notify Aborted).
    void abort(Session& session, AbortReason reason, SimTime time);

    void on_timer(Session& session);
    void on_rts(const Message& message, std::int32_t sender);
    void on_cts(const Message& message, std::int32_t sender);
    void on_eoma(const Message& message, std::int32_t sender);
    void on_bam(const Message& message, std::int32_t sender);
    /// Tells the nodes that receive `session` that it started (IncomingTransfer).
    void announce_receive(Session& session, SimTime time);
    /// Tells the nodes that received `transfer` (a released session's copy) that it ended.
    /// `nodes`: a broadcast goes to the nodes before this index (those told of its start).
    void notify_receive(const IncomingTransfer& transfer, std::int32_t node, std::int32_t sender, std::size_t nodes);
    void on_abort(const Message& message, std::int32_t sender);
    void on_data(const Message& message, std::int32_t sender);
    void on_rts_data(Session& session, const Message& message, std::int32_t sender);
    void on_bam_data(Session& session, const Message& message, std::int32_t sender);
    /// Sends a clear-to-send for the packets from `session.next`.
    void send_cts(Session& session);
    void request_retransmit(Session& session, SimTime time);
    /// Copies packet `sequence` into the buffer. false if the frame is too short for it.
    bool store(Session& session, std::uint8_t sequence, std::span<const std::uint8_t> data) noexcept;
    /// The frame of packet `sequence` of a transmit session.
    [[nodiscard]] can::Frame data_frame(const Session& session, std::uint8_t sequence) const;

    Network& network_;
    const NetworkOptions& options_;
    std::vector<Session> sessions_;
    std::vector<Response> responses_;
    std::uint64_t next_order_ = 1;
    std::uint64_t next_transfer_ = 1;
};

} // namespace vehicle_sil::j1939
