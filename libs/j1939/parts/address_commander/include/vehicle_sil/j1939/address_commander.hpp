// SPDX-License-Identifier: MIT
//
// A protocol part of the J1939 stack: sends Commanded Address (PGN 65240) from a node and
// reports whether the commanded control function moved. Built on the node's public API and the
// toolbox only.
#pragma once

#include "vehicle_sil/j1939/name.hpp"
#include "vehicle_sil/j1939/toolbox.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace vehicle_sil::j1939 {

enum class CommandOutcome : std::uint8_t
{
    Moved,   ///< The target claimed the new address (Address Claimed from it, with its NAME).
    Timeout, ///< It did not within `timeout` after the command's transfer completed.
    NotSent, ///< The command could not be sent: send() refused it (CommandResult::send_status;
             ///< NoAddress once the node cannot claim an address), or its transfer ended
             ///< without completing twice (CommandResult::transfer and reason).
};

struct CommandResult
{
    CommandOutcome outcome = CommandOutcome::Timeout;
    Name target{};
    Address address = kNullAddress;          ///< The commanded address.
    SendStatus send_status = SendStatus::Ok; ///< NotSent by send().
    /// NotSent by the transfer: how the last one ended (Completed otherwise).
    TransferResult transfer = TransferResult::Completed;
    AbortReason reason = AbortReason::None;
    /// Moved: the Address Claimed's time. Timeout: the deadline. NotSent: the step's time, or
    /// the transfer's end.
    SimTime time{};
};

using CommandCallback = std::function<void(const CommandResult&)>;

struct AddressCommanderOptions
{
    /// How long to wait for the target's Address Claimed, counted from the end of the command's
    /// transfer (the command is 9 bytes: it travels with the transport protocol).
    SimDuration timeout = std::chrono::milliseconds(1250);
    /// Commands that may wait at the same time (allocated when the commander is created).
    std::size_t capacity = 4;
};

/// Moves another control function to a new address with Commanded Address:
///
///     AddressCommander tool(service, {});
///     tool.command(kBrakeName, 0x90, [](const CommandResult& r) { ... r.outcome ... });
///
/// - command() may be called everywhere (also outside a step and before the node has an
///   address). The command leaves at the next chance once the node can send
///   (Node::can_send_now()): 9 bytes
///   (the target's NAME, the new address) with the transport protocol, a broadcast (BAM) to
///   all nodes, or a connection (RTS/CTS) to `destination`; a transfer of the node that is still
///   going delays it (WhenBusy::Queue).
/// - The transfer's end (Node::on_transfer_end(), matched by TransferEnd::id) starts the wait.
///   A transfer that ends otherwise (cancelled when this node lost or changed its address,
///   aborted by the receiver, timed out) is sent again once; a second such end is NotSent.
/// - The target answers by claiming the new address: Moved when Address Claimed with its NAME
///   comes from that address (also if it was there already, and also before the transfer's end
///   was reported). Only arbitrary address capable control functions follow the command;
///   others ignore it and the outcome is Timeout.
/// - The callback runs in the step; it may call command() again. It must not destroy the
///   commander (the commander goes on with its other commands after the callback).
///
/// Uses kTimers (two) Timers of the network (one to send, one to wait; Network::timers_free()).
/// Create it with node.attach<AddressCommander>(options) or owned by the application. Not from
/// other threads (Host::post()).
class AddressCommander
{
public:
    /// The Timers of the network the part uses.
    static constexpr std::size_t kTimers = 2;
    /// std::invalid_argument for timeout <= 0 or capacity 0; std::length_error if the network
    /// has fewer than two timers left.
    explicit AddressCommander(Node& node, AddressCommanderOptions options = {});
    AddressCommander(const AddressCommander&) = delete;
    AddressCommander& operator=(const AddressCommander&) = delete;

    /// Commands the control function with NAME `target` to move to `address` (0..253). false
    /// (and no callback) if `capacity` commands are waiting (a command answered before its
    /// transfer ended keeps its place until the end). std::invalid_argument for an
    /// address outside 0..253 or destination kNullAddress. `done` may be empty.
    bool command(Name target, Address address, CommandCallback done = {}, Address destination = kGlobalAddress);
    /// Commands waiting to be sent or for their answer.
    [[nodiscard]] std::size_t pending() const noexcept;

private:
    enum class Phase : std::uint8_t
    {
        Free,
        Waiting,      ///< To be sent once the node can send.
        Transferring, ///< Sent; its transfer has not ended yet.
        Sent,         ///< Transfer completed; waiting for the target's claim until deadline.
        Answered,     ///< Moved before its transfer ended: reported, the slot waits for the end.
    };
    struct Command
    {
        Phase phase = Phase::Free;
        Name target{};
        Address address = kNullAddress;
        Address destination = kGlobalAddress;
        SimTime deadline{};
        std::uint64_t serial = 0;
        TransferId transfer{};        ///< Matches the transfer's end (Transferring, Answered).
        std::uint32_t failures = 0;   ///< Transfers that ended without completing.
        CommandCallback done;
    };

    void arm();
    void on_send_timer();
    /// Sends the waiting commands now (or ends them with NotSent without an address).
    void send_waiting();
    void on_wait_timer();
    void on_claim(const Message& message);
    void on_address(const AddressChange& change);
    void on_transfer_end(const TransferEnd& end);
    void finish(Command& command, CommandResult result);

    Node& node_;
    AddressCommanderOptions options_;
    std::vector<Command> commands_;
    std::uint64_t next_serial_ = 1;
    Timer send_;
    Timer wait_;
    Subscription claims_;
    Subscription address_;
    Subscription transfers_;
};

} // namespace vehicle_sil::j1939
