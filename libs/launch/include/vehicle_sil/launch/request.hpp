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

#include "vehicle_sil/config/config.hpp"
#include "vehicle_sil/config/connect.hpp"
#include "vehicle_sil/config/time_plan.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch {

/// How long one --describe may take unless the request says otherwise (RunHost::describe_timeout).
inline constexpr std::chrono::milliseconds kDescribeTimeout{10000};
/// Maximum virtual-time post-failure tail recording duration in milliseconds (10 minutes).
using config::kMaxRecordAfterFailMs;

/// What the user wants of the run: the same for every way to ask (the command line, and others).
struct RunIntent
{
    /// The variant of the file to run; empty: the file's own. (test takes its variants apart.)
    std::optional<std::string> variant{};
    /// The parameters of the scenario, each once (ParamSource::CommandLine, or RequestFile).
    std::vector<config::ParamValue> params{};
    /// The scenario file as the user named it; not set: none. Set but empty is a value that no
    /// reader accepts, which validate_request() does not see: it is the reader's to refuse. From
    /// the working directory; one of a request file (RunRequest::origin has "scenario") from the
    /// directory of the configuration file.
    std::optional<std::string> scenario{};
    /// Selected case name from `cases:` (--case, or "case" in a request file / serve intent).
    std::optional<std::string> scenario_case{};
    std::vector<std::string> disable{};
    std::vector<std::string> enable{};
    std::vector<std::string> manual{};
    /// The networks connected outside the simulation, each once.
    std::vector<config::ConnectRequest> connect{};
    config::TimePolicy time_policy = config::TimePolicy::Realtime;
    /// The inner empty is fastest; the outer empty uses the launcher's default.
    std::optional<std::optional<double>> speed{};
    bool start_paused = false;
    bool strict = false;
    /// Automatic stop in virtual milliseconds, or no automatic stop.
    std::optional<std::int64_t> stop_at_ms{};
    /// Wall-clock milliseconds of lateness before a step counts as late.
    std::optional<std::int64_t> late_tolerance_ms{};
    /// What to do after the scenario passes: stop (default) or pause for manual handover.
    config::AfterScenario after_scenario = config::AfterScenario::Stop;
    /// Extra ECU variables to record in the timeline ("<participant>.<variable>"), independent of
    /// scenario assertions. Empty by default (never records all signals by default).
    std::vector<std::string> record{};
    /// Virtual milliseconds to keep recording after a normal scenario assertion or wait-timeout
    /// failure (1..kMaxRecordAfterFailMs); omitted = off (stop immediately at failure).
    std::optional<std::int64_t> record_after_fail_ms{};
    /// Record virtual-timestamped structured ECU control events (control::Agent::log / Ecu::log)
    /// into the timeline (opt-in; false by default).
    bool record_events = false;
};

/// run --control: where the run's control server listens and the files it reads, as the caller
/// gave them (UTF-8). run_file() reads and checks them before it checks anything else.
struct ControlHost
{
    /// <port> (on 127.0.0.1; 0: any free port), <host>:<port> or unix:<path>.
    std::string listen{};
    /// The file of tokens and roles; empty: every client may call.
    std::string credentials{};
    /// TLS: the files of the PEM certificate chain and of the PEM private key; both or neither.
    std::string tls_cert{};
    std::string tls_key{};
    /// The file that who changed what is appended to; empty: none.
    std::string audit_log{};
    /// The ID of the run this server belongs to (an opaque string the launcher gives; empty:
    /// none). Every call but Ping that carries the run's metadata with another ID is refused, so
    /// that a client of an earlier run cannot reach this one at the same address. Not a credential.
    std::string run_id{};
    /// Every call but Ping must carry `run_id` (serve). false: a call without it is taken (run).
    bool run_id_required = false;
};

/// What the caller that starts the launcher decides: where things are and how long they may take.
struct RunHost
{
    /// The configuration file as the user gave it, in UTF-8 (process::path_from_utf8() makes the path).
    std::string config{};
    /// Where the launcher is, for the bridges of RunIntent::connect (empty: none is looked for next to it).
    std::filesystem::path tool_directory{};
    /// Time limit per executable, from 1 to config::kMaxMillis milliseconds.
    std::chrono::milliseconds describe_timeout = kDescribeTimeout;
    /// A file of each participant's output in this directory; empty: none.
    std::filesystem::path log_dir{};
    /// The file of the scenario's timeline as the user gave it; empty: none.
    std::string timeline{};
    /// run --control-metrics: the JSON file that receives the Control Hub SIL Kit transport
    /// counters (HubTransport::stats()) at the end of the run; empty: none.
    std::string control_metrics{};
    /// The longest the simulation may run (wall clock), from 1 to config::kMaxMillis ms; empty: no limit.
    std::optional<std::chrono::milliseconds> timeout{};
    std::int64_t start_timeout_ms = 30000;
    std::int64_t stop_timeout_ms = 10000;
    /// A control server is asked for (only in a build with VEHICLE_SIL_BUILD_CONTROL_SERVER:
    /// run_file() throws std::invalid_argument in one without); empty: none.
    std::optional<ControlHost> control{};
    /// The time commands are read from standard input.
    bool stdin_commands = false;
    /// check --text-from-stdin: the configuration's text is read from standard input; `config`
    /// only says where it is (its relative paths, a request file's scenario) and names it in the
    /// findings, and need not exist.
    bool config_from_stdin = false;
    /// check --descriptions: the file of saved --describe outputs (check --write-descriptions)
    /// that the check takes them from; it then starts no process. Empty: each executable is run.
    std::string descriptions{};
    /// check --write-descriptions: the file that the --describe outputs the check read are
    /// written to; empty: none.
    std::string write_descriptions{};
    /// check --compare-commands: the configuration file whose commands that run the check compares
    /// with those of `config` (CheckReport::commands); it only reads it. Empty: none.
    std::string compare_commands{};
    /// run/serve/test --external-registry: loopback `silkit://<host>:<port>` of a user-started
    /// external registry (e.g. official sil-kit-registry with --dashboard-uri); empty: in-process.
    std::string external_registry_uri{};
};

struct RunRequest
{
    RunIntent intent{};
    RunHost host{};
    /// What of `intent` a request file gave (with_request_file()); nothing: all of it is the caller's.
    config::RequestOrigin origin{};
};

/// The command the request is for: the rules differ in which fields the command has.
enum class RequestCommand
{
    Check,
    Run,
    Test,
};

/// Checks the ranges of timing values before any calculation or process is started. Empty on success.
[[nodiscard]] std::vector<config::Finding> validate_timing(const RunRequest& request);

/// The rules about fields that do not go together, as findings (code "usage"; the message says
/// which fields and what to do), in the order in which they are checked; empty: none broken.
/// Each rule looks only at the fields it names, and a field that is not set is no field: a caller
/// that fills the request in steps may call this after each step and refuse the first finding.
/// What depends on the file or on what runs (a variant, a network, a participant) is not here:
/// the check judges it.
/// A rule about a value that the request file gave (RunRequest::origin) names the key and the
/// file ("start_paused in bench.json"), not an option. The params of a request file need no
/// scenario (they wait for a run that has one); --param does. That a paused start needs commands is a
/// rule of Run and Test only: a check starts nothing (and only a request file can ask a check
/// for a paused start).
/// RunHost::config_from_stdin, descriptions, write_descriptions and compare_commands are for Check
/// only.
[[nodiscard]] std::vector<config::Finding> validate_request(const RunRequest& request, RequestCommand command);

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
    /// The keys the file sets, in its order.
    std::vector<std::string> fields{};
};

struct RequestFileRead
{
    /// Empty if the text has an error.
    std::optional<RequestFile> file{};
    /// What is wrong with the text, every error at once: code "usage", `file` and `line` say where,
    /// and the message starts with them ("bench.json, line 3: ...").
    std::vector<config::Finding> findings{};
};

/// Reads the text of a request file named `shown`: a JSON object with "vehicle_sil_run_request": 1
/// and any of variant, params, scenario, case, disable, enable, manual, connect, time_policy, speed,
/// start_paused, strict, stop_at_ms, late_tolerance_ms, after_scenario, record,
/// record_after_fail_ms and record_events. An unknown key, a key written twice, a value of the wrong type (null
/// too: a key that is not meant is left out) and another version are errors. The format is JSON;
/// the reader may take more than JSON (it is the YAML reader), which is no promise. Only the form is read: whether the file has the variant, the
/// participants and the networks is the check's to say. No file system access.
[[nodiscard]] RequestFileRead read_request_file(std::string_view text, std::string_view shown);

/// Reads inline run-request JSON ("intent" of serve's run command): same rules as
/// read_request_file(), except "vehicle_sil_run_request" is optional (validated == 1 if present)
/// and "cli_params" (mapping parameter names to string values, read as ParamSource::CommandLine
/// with from_cli = true) is accepted alongside typed "params".
[[nodiscard]] RequestFileRead read_request_intent(std::string_view text, std::string_view shown);

/// The --connect requests of a run: those of the command line (`arguments`), then those of the
/// request file (`file`) for a network that the command line does not name and whose target is
/// not kNoTarget. So an argument replaces the file's target of its network, `<network>=none` on
/// the command line takes the file's away (the argument stays, as without a file), and "none" in
/// the file is as if the network was not written. `from_file`, if given, gets the networks taken
/// from `file`.
[[nodiscard]] std::vector<config::ConnectRequest> merge_connect(std::span<const config::ConnectRequest> arguments,
                                                        std::span<const config::ConnectRequest> file,
                                                        std::vector<std::string>* from_file = nullptr);

/// `arguments` (the request as the command line gave it) with what `file` adds: an argument wins,
/// field by field.
/// - variant, scenario, speed, stop_at_ms, late_tolerance_ms, record_after_fail_ms: the file's if
///   the command line gave none. time_policy: the file's unless `time_policy_given` (the command
///   line named one, also realtime).
/// - start_paused, strict, record_events: true if either is (no argument says false).
/// - params: by name, the argument's value wins.
/// - connect: merge_connect().
/// - disable, enable, manual: both lists; a name that the command line gives the other way
///   (--enable X and the file's disable: [X]; --disable X and the file's enable or manual; --manual
///   X and the file's disable) is as the command line says.
/// - record: the file's entries followed by the command line's entries, deduplicated preserving
///   first-seen order.
/// RunRequest::origin of the result says what came from the file. RunHost is `arguments`'.
[[nodiscard]] RunRequest with_request_file(const RunRequest& arguments, const RequestFile& file, bool time_policy_given);

} // namespace vehicle_sil::launch
