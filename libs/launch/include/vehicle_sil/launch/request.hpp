// SPDX-License-Identifier: MIT
//
// What a check, a run or a test is asked to do (RunRequest): the user's intent (which variant,
// which parameters, which scenario, what is switched off, what is connected outside, the speed)
// and what the caller that starts the launcher decides (the file, where the tools and the logs
// are, the limits). validate_request() holds the rules about how the fields go together; the
// value of one field is read by whoever fills it (the command line). Values and pure functions:
// no state, no thread, no file, no process.
#pragma once

#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/config.hpp"
#include "vehicle_sil/launch/connect.hpp"
#include "vehicle_sil/launch/time_plan.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::launch {

/// What the user wants of the run: the same for every way to ask (the command line, and others).
struct RunIntent
{
    /// The variant of the file to run; empty: the file's own. (test takes its variants apart.)
    std::optional<std::string> variant{};
    /// The parameters of the scenario, each once (ParamSource::CommandLine).
    std::vector<ParamValue> params{};
    /// The scenario file as the user named it; not set: none. Set but empty is a value that no
    /// reader accepts, which validate_request() does not see: it is the reader's to refuse.
    std::optional<std::string> scenario{};
    std::vector<std::string> disable{};
    std::vector<std::string> enable{};
    std::vector<std::string> manual{};
    /// The networks connected outside the simulation, each once.
    std::vector<ConnectRequest> connect{};
    TimePolicy time_policy = TimePolicy::Realtime;
    /// The speed replaces the file's (the inner empty: as fast as possible); the outer empty: the
    /// file's speed counts.
    std::optional<std::optional<double>> speed{};
    bool start_paused = false;
    bool strict = false;
};

/// What the caller that starts the launcher decides: where things are and how long they may take.
struct RunHost
{
    /// The configuration file and its name as the user gave it.
    std::filesystem::path config{};
    std::string config_shown{};
    /// The scenario of RunIntent::scenario, as a path.
    std::filesystem::path scenario{};
    /// Where the launcher is, for the bridges of RunIntent::connect (empty: none is looked for next to it).
    std::filesystem::path tool_directory{};
    std::chrono::milliseconds describe_timeout{10000};
    /// A file of each participant's output in this directory; empty: none.
    std::filesystem::path log_dir{};
    /// The file of the scenario's timeline and its name as the user gave it; empty: none.
    std::filesystem::path timeline{};
    std::string timeline_shown{};
    /// The longest the simulation may run (wall clock); empty: no limit.
    std::optional<std::chrono::milliseconds> timeout{};
    /// A control server is asked for.
    bool control = false;
    /// The time commands are read from standard input.
    bool stdin_commands = false;
};

struct RunRequest
{
    RunIntent intent{};
    RunHost host{};
};

/// The command the request is for: the rules differ in which fields the command has.
enum class RequestCommand
{
    Check,
    Run,
    Test,
};

/// The rules about fields that do not go together, as findings (code "usage"; the message says
/// which fields and what to do), in the order in which they are checked; empty: none broken.
/// Each rule looks only at the fields it names, and a field that is not set is no field: a caller
/// that fills the request in steps may call this after each step and refuse the first finding.
/// What depends on the file or on what runs (a variant, a network, a participant) is not here:
/// the check judges it.
[[nodiscard]] std::vector<Finding> validate_request(const RunRequest& request, RequestCommand command);

/// The options of the check that `request` asks for. `host.config` is not in them: it is the path
/// the check is called with.
[[nodiscard]] CheckOptions check_options_of(const RunRequest& request);

} // namespace vehicle_sil::launch
