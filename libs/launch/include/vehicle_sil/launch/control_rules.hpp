// SPDX-License-Identifier: MIT
//
// The rules of a run's controls: which operation from outside (pause, resume, step, run_until,
// speed, stop, a write of a variable) the run takes now, by where it came in (standard input, a
// control server) and by where the run stands; one table and one function that reads it (decide()).
// TimeCommands decides each command with it (time_commands.hpp), allowed() answers the same
// question without doing anything, and a control server that only shows the run is made from it
// (always_refused()). Pure: no clock, no lock, no SIL Kit. Internal.
#pragma once

#include "vehicle_sil/launch/command.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace vehicle_sil::launch {

/// What can be asked of a run from outside.
enum class Operation
{
    Pause,
    Resume,
    Step,
    RunUntil,
    Speed,
    Stop,
    Write,   ///< a write of a variable (a control server's; standard input has no such command)
    Unknown, ///< a command of another name
};
/// "pause", "resume", "step", "run_until", "speed", "stop", "write"; "" for Unknown.
[[nodiscard]] const char* operation_name(Operation operation);
/// The operation of a command's name as TimeCommands::apply() takes it: one of pause, resume,
/// step, run_until, speed and stop, else Unknown ("write" too: it is no command).
[[nodiscard]] Operation command_operation(std::string_view name);

/// Where an operation came in.
enum class Entry
{
    Stdin,         ///< run --stdin-commands
    ControlServer, ///< run --control
};
/// "stdin", "control_server": the `source` of the command events.
[[nodiscard]] const char* entry_name(Entry entry);
/// The entry of a `source` as TimeCommands::apply() is given it: ControlServer for
/// "control_server", else Stdin.
[[nodiscard]] Entry entry_of(std::string_view source);

/// Where the run stands for its time commands.
enum class ControlStage
{
    SettingUp, ///< before the controller is attached: the check, the start of the registry and the controller
    Attached,  ///< the controller takes the commands (also before its start and in the hold)
    Detached,  ///< the simulation is over, or did not start
};

/// What the rules look at.
struct ControlSituation
{
    ControlStage stage = ControlStage::SettingUp;
    /// A pause was asked for (or the run starts paused) and no resume since.
    bool paused = false;
    /// The run plays a scenario (run --scenario). Fixed for the run.
    bool scenario = false;
    /// Something that does not wait for the virtual time keeps the run to real time
    /// (TimeCommands::set_time_lock()). Fixed for the run once given; nothing gives it yet.
    bool time_locked = false;
};

/// The codes of the refusals (lower case with hyphens), for a program to tell them apart without
/// reading the message.
namespace reason {
inline constexpr const char* kScenarioRunning = "scenario-running";
inline constexpr const char* kNotRunning = "not-running";
inline constexpr const char* kNotATimeCommand = "not-a-time-command";
inline constexpr const char* kOutsideRealTime = "outside-real-time";
inline constexpr const char* kOutOfRange = "out-of-range";
inline constexpr const char* kMissingArgument = "missing-argument";
inline constexpr const char* kNotSetUp = "not-set-up";
inline constexpr const char* kNotPaused = "not-paused";
} // namespace reason

/// The answer of the rules.
struct Decision
{
    bool ok = true;
    std::string reason{};  ///< refused: one of reason::*; else ""
    std::string message{}; ///< refused: why, and what to do instead (one line, English); else ""
};

/// Whether the run takes `operation` from `entry` in `situation`: the first row of the table that
/// refuses it gives the answer (the rows and their order: control_rules.cpp).
///
/// `name`: the command's name as it came (the messages quote it; it tells nothing else).
/// `arguments`: the command with its arguments ("ms", "value", "at_ms"), checked too; or nullptr
/// for the question "would it be taken with arguments that are in range" (allowed()): then no
/// argument is looked at, a pause is one without a time and a run_until has one.
[[nodiscard]] Decision decide(const ControlSituation& situation, Entry entry, Operation operation, std::string_view name,
                              const Command* arguments);

/// The arguments an operation takes, from `min` to `max`. Step: milliseconds. Speed: simulation
/// seconds per wall-clock second ("max", as fast as possible, is taken besides unless min == max).
struct ArgumentRange
{
    double min = 0;
    double max = 0;
};

/// Whether an operation is taken now, for showing it (a state for clients, a button).
struct Allowed
{
    bool ok = true;
    std::string reason{};  ///< as Decision::reason
    std::string message{}; ///< as Decision::message
    /// Step and Speed, when taken: what the argument may be.
    std::optional<ArgumentRange> range{};
};

/// decide() without arguments, by the operation's own name, and the range of its argument.
[[nodiscard]] Allowed allowed(const ControlSituation& situation, Entry entry, Operation operation);

/// Whether `entry` is refused everything but a stop for the whole run (every stage, paused or not,
/// with or without the time lock), all for one reason: that refusal, else empty. A control server
/// asks it once, before the run, to refuse those calls itself in one place (it only shows the run).
[[nodiscard]] std::optional<Decision> always_refused(bool scenario, Entry entry);

} // namespace vehicle_sil::launch
