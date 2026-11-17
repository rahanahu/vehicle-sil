// SPDX-License-Identifier: MIT
//
// A command of a run (stop, pause, resume, step, run_until, speed) as it was read from standard
// input or came from a control server (Command). Read by parse_command() (events.hpp), decided and
// done by TimeCommands (time_commands.hpp), written as the event "command" (events.hpp). Internal;
// the commands: docs/simulation-events.md.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace vehicle_sil::launch {

/// The largest "id" of a command: 2^53, which every JSON reader keeps exactly.
inline constexpr std::int64_t kMaxCommandId = std::int64_t{1} << 53;

/// One line of standard input, read as a command.
struct Command
{
    /// The value of "command" if the line is a JSON object whose "command" is a string.
    std::optional<std::string> name{};
    bool accepted = false;
    /// Why it was not accepted (one line), or "".
    std::string message{};
    /// Not accepted: the code of why (reason::* of control_rules.hpp; the command event's
    /// "reason"), or "".
    std::string reason{};
    /// The line's "id" if it has one (a whole number from 0 to kMaxCommandId): the sender's number
    /// for this command, written back in its command event so that the sender finds its answer.
    std::optional<std::int64_t> id{};
    /// step: its "ms" as read; once applied (TimeCommands), how far the simulation goes (the
    /// command event's "ms").
    std::optional<std::int64_t> ms{};
    /// speed: its "value" (empty: "max").
    std::optional<double> speed{};
    /// run_until (from standard input or a control server) and pause at a time (a control server
    /// only): pause at, and run_until, this virtual time; once applied
    /// (TimeCommands), where the simulation will stand (the command event's "at_ms"), a whole
    /// number of the controller's steps.
    std::optional<std::int64_t> at_ms{};

    [[nodiscard]] bool is_stop() const { return accepted && name == "stop"; }
    /// pause, resume, step, run_until or speed (TimeCommands applies it).
    [[nodiscard]] bool is_time() const { return accepted && name && *name != "stop"; }
};

} // namespace vehicle_sil::launch
