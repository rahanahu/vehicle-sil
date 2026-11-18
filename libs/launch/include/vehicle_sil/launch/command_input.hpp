// SPDX-License-Identifier: MIT
//
// The commands of `run --stdin-commands` as they are read: one JSON object per line of standard
// input, each read into a Command (command.hpp). Internal; the commands: docs/simulation-events.md.
#pragma once

#include "vehicle_sil/runner/command.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vehicle_sil::launch {

/// The longest command line read; a longer one is not accepted.
inline constexpr std::size_t kMaxCommandLine = 64 * 1024;

/// Reads `line` (without its line end; a "\r" at its end is ignored) as a command: {"command":
/// "stop"}, {"command":"pause"}, {"command":"resume"}, {"command":"step","ms":<1 to kMaxMillis>},
/// {"command":"speed","value":<a speed, parse_speed()>}, each with or without "id":<0 to
/// kMaxCommandId> (other keys are ignored). A line that is not accepted has its `reason`. Only the form
/// is read: whether the command can be done now is TimeCommands' part. Empty for a line of white
/// space only (it is skipped).
[[nodiscard]] std::optional<Command> parse_command(std::string_view line);

/// Cuts the bytes of standard input into lines and reads each as a command (parse_command()). A
/// line longer than kMaxCommandLine is one command that is not accepted, and the rest of it up to
/// its line end is skipped. Not thread safe: one reader.
class CommandInput
{
public:
    explicit CommandInput(std::function<void(const Command&)> on_command) : on_command_(std::move(on_command)) {}
    /// What was read.
    void append(std::string_view bytes);
    /// The end of input: a last line without a line end is read too.
    void finish();

private:
    void line(std::string_view text);

    std::function<void(const Command&)> on_command_;
    std::string pending_;
    bool skipping_ = false; ///< in a line that was too long, until its end
};

} // namespace vehicle_sil::launch
