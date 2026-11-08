// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/address_commander.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::j1939 {

namespace {

[[nodiscard]] AddressCommanderOptions checked(const AddressCommanderOptions& options)
{
    if (options.timeout <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::j1939::AddressCommander: timeout must be > 0");
    }
    if (options.capacity == 0)
    {
        throw std::invalid_argument("vehicle_sil::j1939::AddressCommander: capacity must be > 0");
    }
    return options;
}

} // namespace

AddressCommander::AddressCommander(Node& node, AddressCommanderOptions options)
  : node_(node),
    options_(checked(options)),
    commands_(options_.capacity),
    send_(node, TimerKind::Scheduled, [this] { on_send_timer(); }),
    wait_(node, TimerKind::Timeout, [this] { on_wait_timer(); })
{
    claims_ = Subscription{node, node.on_message(kPgnAddressClaimed, [this](const Message& m) { on_claim(m); })};
    address_ = Subscription{node, node.on_address_change([this](const AddressChange& c) { on_address(c); })};
    transfers_ = Subscription{node, node.on_transfer_end([this](const TransferEnd& e) { on_transfer_end(e); })};
}

bool AddressCommander::command(Name target, Address address, CommandCallback done, Address destination)
{
    if (!is_unicast(address) || destination == kNullAddress)
    {
        throw std::invalid_argument("vehicle_sil::j1939::AddressCommander: address outside 0..253 or destination "
                                    "kNullAddress");
    }
    const auto free = std::find_if(commands_.begin(), commands_.end(), [](const Command& c) { return c.phase == Phase::Free; });
    if (free == commands_.end())
    {
        return false;
    }
    *free = Command{Phase::Waiting, target, address, destination, SimTime{}, next_serial_++, 0, 0, std::move(done)};
    arm();
    return true;
}

std::size_t AddressCommander::pending() const noexcept
{
    return static_cast<std::size_t>(
        std::count_if(commands_.begin(), commands_.end(),
                      [](const Command& c) { return c.phase != Phase::Free && c.phase != Phase::Answered; }));
}


void AddressCommander::arm()
{
    bool waiting = false;
    std::optional<SimTime> deadline;
    for (const Command& c : commands_)
    {
        waiting = waiting || c.phase == Phase::Waiting;
        if (c.phase == Phase::Sent && (!deadline || c.deadline < *deadline))
        {
            deadline = c.deadline;
        }
    }
    if (waiting && (node_.can_send_now() || node_.address_state() == AddressState::CannotClaim))
    {
        if (!send_.running())
        {
            send_.start_at(node_.network().event_time()); // the next chance
        }
    }
    else
    {
        send_.cancel();
    }
    if (deadline)
    {
        if (wait_.deadline() != deadline || !wait_.running())
        {
            wait_.start_at(*deadline);
        }
    }
    else
    {
        wait_.cancel();
    }
}

void AddressCommander::on_send_timer()
{
    if (node_.can_send_now() || node_.address_state() == AddressState::CannotClaim)
    {
        send_waiting();
    }
    arm();
}

void AddressCommander::send_waiting()
{
    const SimTime now = *node_.network().now();
    const bool cannotClaim = node_.address_state() == AddressState::CannotClaim;
    const std::uint64_t limit = next_serial_; // not the ones a callback adds
    for (Command& c : commands_)
    {
        if (c.phase != Phase::Waiting || c.serial >= limit)
        {
            continue;
        }
        std::array<std::uint8_t, 9> data{};
        const auto name = c.target.to_bytes();
        std::copy(name.begin(), name.end(), data.begin());
        data[8] = c.address;
        const SendStatus status = cannotClaim ? SendStatus::NoAddress
                                              : node_.send(kPgnCommandedAddress, data, c.destination, kDefaultPriority,
                                                           WhenBusy::Queue);
        if (status == SendStatus::Ok)
        {
            c.phase = Phase::Transferring; // the wait starts at the transfer's end
            c.sent_order = next_sent_++;
            continue;
        }
        CommandResult result;
        result.outcome = CommandOutcome::NotSent;
        result.send_status = status;
        result.time = now;
        finish(c, result);
    }
}

void AddressCommander::on_wait_timer()
{
    const SimTime deadline = *wait_.deadline();
    const std::uint64_t limit = next_serial_;
    for (Command& c : commands_)
    {
        if (c.phase == Phase::Sent && c.serial < limit && c.deadline <= deadline)
        {
            CommandResult result;
            result.outcome = CommandOutcome::Timeout;
            result.time = deadline;
            finish(c, result);
        }
    }
    arm();
}

void AddressCommander::on_claim(const Message& message)
{
    if (message.data.size() < 8)
    {
        return;
    }
    const Name name = Name::from_bytes(message.data);
    const std::uint64_t limit = next_serial_;
    bool finished = false;
    for (Command& c : commands_)
    {
        if ((c.phase == Phase::Sent || c.phase == Phase::Transferring) && c.serial < limit && c.target == name &&
            c.address == message.source)
        {
            CommandResult result;
            result.outcome = CommandOutcome::Moved;
            result.time = message.time;
            finish(c, result);
            finished = true;
        }
    }
    if (finished)
    {
        arm();
    }
}

void AddressCommander::on_address(const AddressChange& /*change*/)
{
    // Commands not sent yet go once the node can send (at once: the handler runs in the step),
    // or end when it cannot claim an address. Sent ones keep waiting: the target's claim answers
    // them whatever this node's address.
    if (node_.can_send_now() || node_.address_state() == AddressState::CannotClaim)
    {
        send_waiting();
    }
    arm();
}

void AddressCommander::on_transfer_end(const TransferEnd& end)
{
    if (end.pgn != kPgnCommandedAddress)
    {
        return;
    }
    // Transfers to one destination run one after the other, in the order sent.
    Command* first = nullptr;
    for (Command& c : commands_)
    {
        if ((c.phase == Phase::Transferring || c.phase == Phase::Answered) && c.destination == end.destination &&
            (first == nullptr || c.sent_order < first->sent_order))
        {
            first = &c;
        }
    }
    if (first == nullptr)
    {
        return;
    }
    if (first->phase == Phase::Answered)
    {
        first->phase = Phase::Free; // its end, after its answer
        return;
    }
    if (end.result == TransferResult::Completed)
    {
        first->phase = Phase::Sent;
        first->deadline = end.time + options_.timeout;
    }
    else if (++first->failures < 2)
    {
        first->phase = Phase::Waiting; // once the node can send again
    }
    else
    {
        CommandResult result;
        result.outcome = CommandOutcome::NotSent;
        result.transfer = end.result;
        result.reason = end.reason;
        result.time = end.time;
        finish(*first, result);
    }
    arm();
}

void AddressCommander::finish(Command& command, CommandResult result)
{
    result.target = command.target;
    result.address = command.address;
    CommandCallback done = std::move(command.done);
    command.done = nullptr;
    // Before the callback, which may command again. A transfer still under way keeps the slot
    // until its end, so that the ends match the commands in the order sent.
    command.phase = command.phase == Phase::Transferring ? Phase::Answered : Phase::Free;
    if (done)
    {
        done(result);
    }
}

} // namespace vehicle_sil::j1939
