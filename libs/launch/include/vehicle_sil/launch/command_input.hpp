// SPDX-License-Identifier: MIT
//
// The commands of `run --stdin-commands` and of `serve` as they are read: one JSON object per line
// of standard input, each read into a Command (command.hpp; serve: a ServeCommand). Internal; the
// commands: docs/simulation-events.md.
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
[[nodiscard]] std::optional<runner::Command> parse_command(std::string_view line);

/// A line of standard input as serve reads it: the command, and what serve adds to it.
struct ServeCommand
{
    /// As parse_command() reads it, but "run" is a command too.
    runner::Command command{};
    /// "run": the ID of the run the command is for (as its "run_start" said it); empty if the
    /// line has none. Every command but run needs it (the launcher compares it).
    std::optional<std::string> run{};
    /// run: "request", the request file of the run (as run --request reads it); "" for none.
    std::string request{};
    /// run: "timeline", the file the run writes its timeline to (as run --timeline); "" for none.
    std::string timeline{};
};

/// Reads `line` as serve does: as parse_command(), and {"command":"run"} with "request" and
/// "timeline" (non-empty strings, each optional), and "run" (a non-empty string) on any command.
/// A line that is not accepted has its `reason` (its "run" too, if it could be read).
[[nodiscard]] std::optional<ServeCommand> parse_serve_command(std::string_view line);

/// Cuts the bytes of standard input into lines and reads each as a command (parse_command()). A
/// line longer than kMaxCommandLine is one command that is not accepted, and the rest of it up to
/// its line end is skipped. Not thread safe: one reader.
class CommandInput
{
public:
    /// The commands of serve (parse_serve_command()), for the constructor below.
    struct Serve
    {
    };

    explicit CommandInput(std::function<void(const runner::Command&)> on_command) : on_command_(std::move(on_command)) {}
    /// serve: each line as parse_serve_command() reads it (a line that is too long has no "run").
    CommandInput(Serve, std::function<void(const ServeCommand&)> on_command) : on_serve_(std::move(on_command)) {}
    /// What was read.
    void append(std::string_view bytes);
    /// The end of input: a last line without a line end is read too.
    void finish();

private:
    void line(std::string_view text);

    /// A line that is not accepted for its length (without "run" in serve).
    void too_long(const runner::Command& command);

    std::function<void(const runner::Command&)> on_command_;
    std::function<void(const ServeCommand&)> on_serve_;
    std::string pending_;
    bool skipping_ = false; ///< in a line that was too long, until its end
};

} // namespace vehicle_sil::launch
