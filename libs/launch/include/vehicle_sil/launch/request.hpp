// SPDX-License-Identifier: MIT
//
// What a check, a run or a test is asked to do (RunRequest): the user's intent (which variant,
// which parameters, which scenario, what is switched off, what is connected outside, the speed)
// and what the caller that starts the launcher decides (the file, where the tools and the logs
// are, the limits). validate_request() holds the rules about how the fields go together; the
// value of one field is read by whoever fills it (the command line). The intent can also be
// written as a JSON file (the request file of run --request and check --request;
// schema/run-request.schema.json): read_request_file() reads its text, with_request_file() lays
// the arguments over it. Values and pure functions: no state, no thread, no file, no process.
#pragma once

#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/config.hpp"
#include "vehicle_sil/launch/connect.hpp"
#include "vehicle_sil/launch/time_plan.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch {

/// What the user wants of the run: the same for every way to ask (the command line, and others).
struct RunIntent
{
    /// The variant of the file to run; empty: the file's own. (test takes its variants apart.)
    std::optional<std::string> variant{};
    /// The parameters of the scenario, each once (ParamSource::CommandLine, or RequestFile).
    std::vector<ParamValue> params{};
    /// The scenario file as the user named it; not set: none. Set but empty is a value that no
    /// reader accepts, which validate_request() does not see: it is the reader's to refuse. From
    /// the working directory; one of a request file (RunRequest::origin has "scenario") from the
    /// directory of the configuration file.
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
    /// The configuration file as the user gave it, in UTF-8 (process::path_from_utf8() makes the path).
    std::string config{};
    /// Where the launcher is, for the bridges of RunIntent::connect (empty: none is looked for next to it).
    std::filesystem::path tool_directory{};
    std::chrono::milliseconds describe_timeout = kDescribeTimeout;
    /// A file of each participant's output in this directory; empty: none.
    std::filesystem::path log_dir{};
    /// The file of the scenario's timeline as the user gave it; empty: none.
    std::string timeline{};
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
    /// What of `intent` a request file gave (with_request_file()); nothing: all of it is the caller's.
    RequestOrigin origin{};
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
/// A rule about a value that the request file gave (RunRequest::origin) names the key and the
/// file ("start_paused in bench.json"), not an option. That a paused start needs commands is a
/// rule of Run and Test only: a check starts nothing (and only a request file can ask a check
/// for a paused start).
[[nodiscard]] std::vector<Finding> validate_request(const RunRequest& request, RequestCommand command);

/// The version of the request file (its "vehicle_sil_run_request").
inline constexpr int kRunRequestFormat = 1;

/// A request file as it was read: the user's intent, written down (JSON; the keys are the ones of
/// schema/run-request.schema.json). It has nothing of RunHost: where the files are is the caller's.
struct RequestFile
{
    /// The file as the user named it, for the messages.
    std::string shown{};
    /// What the file sets; the rest is as in RunIntent{}. params: ParamSource::RequestFile.
    /// connect: every entry of the file, also one with the target kNoTarget.
    RunIntent intent{};
    /// The keys the file sets, in its order (a key with the value null is not set).
    std::vector<std::string> fields{};
};

struct RequestFileRead
{
    /// Empty if the text has an error.
    std::optional<RequestFile> file{};
    /// What is wrong with the text, every error at once: code "usage", `file` and `line` say where,
    /// and the message starts with them ("bench.json, line 3: ...").
    std::vector<Finding> findings{};
};

/// Reads the text of a request file named `shown`: a JSON object with "vehicle_sil_run_request": 1
/// and any of variant, params, scenario, disable, enable, manual, connect, time_policy, speed,
/// start_paused and strict. An unknown key, a key written twice, a value of the wrong type and
/// another version are errors. Only the form is read: whether the file has the variant, the
/// participants and the networks is the check's to say. No file system access.
[[nodiscard]] RequestFileRead read_request_file(std::string_view text, std::string_view shown);

/// The --connect requests of a run: those of the command line (`arguments`), then those of the
/// request file (`file`) for a network that the command line does not name and whose target is
/// not kNoTarget. So an argument replaces the file's target of its network, `<network>=none` on
/// the command line takes the file's away (the argument stays, as without a file), and "none" in
/// the file is as if the network was not written. `from_file`, if given, gets the networks taken
/// from `file`.
[[nodiscard]] std::vector<ConnectRequest> merge_connect(std::span<const ConnectRequest> arguments,
                                                        std::span<const ConnectRequest> file,
                                                        std::vector<std::string>* from_file = nullptr);

/// `arguments` (the request as the command line gave it) with what `file` adds: an argument wins,
/// field by field.
/// - variant, scenario, speed: the file's if the command line gave none. time_policy: the file's
///   unless `time_policy_given` (the command line named one, also realtime).
/// - start_paused, strict: true if either is (no argument says false).
/// - params: by name, the argument's value wins.
/// - connect: merge_connect().
/// - disable, enable, manual: both lists; a name that the command line gives the other way
///   (--enable X and the file's disable: [X]; --disable X and the file's enable or manual; --manual
///   X and the file's disable) is as the command line says.
/// RunRequest::origin of the result says what came from the file. RunHost is `arguments`'.
[[nodiscard]] RunRequest with_request_file(const RunRequest& arguments, const RequestFile& file, bool time_policy_given);

} // namespace vehicle_sil::launch
