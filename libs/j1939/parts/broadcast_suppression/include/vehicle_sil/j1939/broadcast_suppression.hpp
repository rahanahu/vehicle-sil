// SPDX-License-Identifier: MIT
//
// A protocol part of the J1939 stack: the broadcast suppression of one node, driven by
// DM13 (J1939-73, Stop Start Broadcast). Built on the node's public API and the toolbox only.
#pragma once

#include "vehicle_sil/j1939/toolbox.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace vehicle_sil::j1939 {

/// DM13, Stop Start Broadcast (PDU1).
inline constexpr Pgn kPgnDm13{0xDF00u};

/// The networks of a DM13 and the position of their 2-bit command: bits 2*(n % 4) .. +1 of byte
/// n / 4 (the value is n).
enum class Dm13Network : std::uint8_t
{
    J1939Network1 = 0,
    J1922 = 1,
    J1587 = 2,
    CurrentDataLink = 3, ///< The network the DM13 came on: applies to this node whatever its network.
    OtherManufacturerPort = 4,
    J1850 = 5,
    Iso9141 = 6,
    J1939Network2 = 7,
    J1939Network4 = 8,
    ProprietaryNetwork2 = 9,
    ProprietaryNetwork1 = 10,
    J1939Network3 = 11,
};

/// What this node's own suspension stops (the suspend signal of the DM13 it announces it with).
enum class SuspendScope : std::uint8_t
{
    All,  ///< All broadcasts of the node.
    Some, ///< Some of them.
};

struct BroadcastSuppressionOptions
{
    /// The network this node's DM13 commands are read for (besides CurrentDataLink).
    Dm13Network network = Dm13Network::J1939Network1;
};

/// Whether the broadcasts of one node are suspended now: one state per node, which every part
/// that broadcasts on its own reads (Diagnostics' DM1, a periodic sender) before it sends.
/// Node::send() does not read it: messages the application sends itself are not suppressed.
/// Responses to requests, address claiming, acknowledgements and the transport protocol are
/// never suppressed.
///
/// The broadcasts are suspended while either holds:
/// - Another node stopped them with DM13: a Stop for this node's network (or for the current data
///   link). They resume with a Start, or when no DM13 came for 6 s (each DM13 that does not start
///   them counts again from its time; a DM13 exactly 6 s after the last one still keeps them
///   stopped). The suspend signal and duration of a received DM13 are the sender's notice of
///   its own suspension, not a command, and are not used.
/// - This node suspended them itself (suspend()), until the duration passed or resume().
///
/// One per node, attached to it: create it with BroadcastSuppression::of(node) or
/// BroadcastSuppression::attach(node, options); it cannot be constructed otherwise (one of its
/// own would be a second state that of() and the parts do not see). Parts that read it hold a
/// reference and must be destroyed before it (attach them after it; of() in their constructor
/// does that).
class BroadcastSuppression
{
    struct Key
    {
        explicit Key() = default;
    };

public:
    /// The part of `node`, attached with the default options if it has none yet.
    static BroadcastSuppression& of(Node& node);
    /// Attaches the part of `node` with `options`. std::logic_error if the node has one already
    /// (attach it before the parts that use it).
    static BroadcastSuppression& attach(Node& node, BroadcastSuppressionOptions options);

    /// Only of() and attach() can call it (Key is private).
    BroadcastSuppression(Node& node, Key key, BroadcastSuppressionOptions options);
    BroadcastSuppression(const BroadcastSuppression&) = delete;
    BroadcastSuppression& operator=(const BroadcastSuppression&) = delete;

    /// true while the node's broadcasts are suspended (by DM13 or by suspend()).
    [[nodiscard]] bool suspended() const noexcept { return stopped_ || own_; }
    /// true while another node's DM13 keeps them stopped.
    [[nodiscard]] bool stopped_by_dm13() const noexcept { return stopped_; }

    /// Suspends this node's broadcasts for `duration` (whole seconds, at most 64255; empty: until
    /// resume()), and announces it at the next chance with a DM13 to all nodes (no command for
    /// any network, the suspend signal of `scope`, the duration in seconds or "not available").
    /// Allowed everywhere on the thread that steps the network (other threads: Host::post()),
    /// also outside a step. std::invalid_argument above 64255 s.
    void suspend(std::optional<std::chrono::seconds> duration, SuspendScope scope = SuspendScope::All);
    /// Ends this node's own suspension now (no frame; an announcement not sent yet is dropped).
    void resume() noexcept;

    /// DM13 that the node could not send (SendStatus other than Ok).
    [[nodiscard]] std::uint64_t send_failures() const noexcept { return send_failures_; }

private:
    void on_dm13(const Message& message);

    Node& node_;
    BroadcastSuppressionOptions options_;
    bool stopped_ = false;
    bool own_ = false;
    std::uint8_t announce_signal_ = 0;
    std::uint16_t announce_duration_ = 0xFFFF;
    std::uint64_t send_failures_ = 0;
    Timer hold_;     ///< 6 s after the last DM13 while stopped (Timeout).
    Timer own_end_;  ///< The end of the own suspension.
    Timer announce_; ///< Sends the DM13 of the own suspension.
    Subscription dm13_;
};

} // namespace vehicle_sil::j1939
