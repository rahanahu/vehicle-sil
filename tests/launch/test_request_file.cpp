// SPDX-License-Identifier: MIT
//
// The request file (run --request, check --request): what read_request_file() reads and refuses,
// how with_request_file() lays the arguments over it, how the rules and the findings about the
// time name a value that the file gave, and what the check does with the file's connect and
// scenario. No process.

#include "launch_test_util.hpp"

#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace vehicle_sil::launch;
using vehicle_sil::launch::test::check_text;

template <typename... Strings>
std::vector<std::string> list(const Strings&... items)
{
    return {std::string(items)...};
}

/// The file of `text`, which must read.
RequestFile file_of(const std::string& text, const std::string& shown = "bench.json")
{
    RequestFileRead read = read_request_file(text, shown);
    CHECK(read.findings.empty() && read.file.has_value());
    for (const Finding& f : read.findings)
    {
        std::fprintf(stderr, "unexpected: %s\n", f.message.c_str());
    }
    return read.file.value_or(RequestFile{});
}

/// The messages of the errors of `text`, one per line.
std::string errors_of(const std::string& text)
{
    const RequestFileRead read = read_request_file(text, "bench.json");
    CHECK(!read.file.has_value());
    std::string out;
    for (const Finding& f : read.findings)
    {
        CHECK(f.code == "usage" && f.severity == Severity::Error && f.file == "bench.json");
        CHECK(f.message.rfind(f.line > 0 ? "bench.json, line " + std::to_string(f.line) + ": " : std::string("bench.json: "), 0) == 0);
        out += f.message + "\n";
    }
    return out;
}

/// One key with the version.
std::string with(const std::string& member) { return "{ \"vehicle_sil_run_request\": 1, " + member + " }"; }

ParamValue argument(std::string name, std::string text)
{
    ParamValue p;
    p.name = std::move(name);
    p.text = std::move(text);
    p.source = ParamSource::CommandLine;
    return p;
}

std::string connect_text(const std::vector<ConnectRequest>& connect)
{
    std::string out;
    for (const ConnectRequest& c : connect)
    {
        out += c.network + "=" + c.target + " ";
    }
    return out;
}

std::string params_text(const std::vector<ParamValue>& params)
{
    std::string out;
    for (const ParamValue& p : params)
    {
        out += p.name + "=" + p.text + "(" + param_source_name(p.source) + ") ";
    }
    return out;
}

std::vector<std::string> messages(const RunRequest& r, RequestCommand command = RequestCommand::Run)
{
    std::vector<std::string> out;
    for (const Finding& f : validate_request(r, command))
    {
        out.push_back(f.message);
    }
    return out;
}

std::string lines(const std::vector<Finding>& findings)
{
    std::string out;
    for (const Finding& f : findings)
    {
        out += std::string(f.severity == Severity::Error ? "E " : f.severity == Severity::Warning ? "W " : "N ") + f.code + ": " +
               f.message + "\n";
    }
    return out;
}

} // namespace

TEST_CASE(a_request_file_has_every_field_of_the_intent)
{
    const RequestFile file = file_of(R"({ "vehicle_sil_run_request": 1,
  "variant": "left_hand", "params": {"target_speed": "80", "gear": 3, "lamp": true},
  "scenario": "scenarios/brake.yaml",
  "disable": ["B"], "enable": ["C"], "manual": ["A"],
  "connect": {"ETH_EXT": "enp3s0", "CAN1": "physical:1", "LIN1": "none"},
  "time_policy": "free",
  "speed": 2.5, "start_paused": true, "strict": true })",
                                     "dir/bench.json");
    const RunIntent& intent = file.intent;
    CHECK(file.shown == "dir/bench.json");
    CHECK(intent.variant == std::optional<std::string>("left_hand") && intent.scenario == std::optional<std::string>("scenarios/brake.yaml"));
    check_text(params_text(intent.params), "target_speed=80(request_file) gear=3(request_file) lamp=true(request_file) ");
    CHECK(intent.params.at(0).quoted && !intent.params.at(1).quoted && intent.params.at(0).line == 0);
    CHECK(intent.disable == list("B") && intent.enable == list("C") && intent.manual == list("A"));
    // Every entry, also the one that connects nothing: with_request_file() decides what counts.
    check_text(connect_text(intent.connect), "ETH_EXT=enp3s0 CAN1=physical:1 LIN1=none ");
    CHECK(intent.time_policy == TimePolicy::Free && intent.speed == std::optional<std::optional<double>>(2.5) &&
          intent.start_paused && intent.strict);
    CHECK(file.fields == list("variant", "params", "scenario", "disable", "enable", "manual", "connect", "time_policy", "speed",
                              "start_paused", "strict"));
    // Only the version: nothing is set. "max".
    const RequestFile bare = file_of("{\"vehicle_sil_run_request\": 1}");
    CHECK(bare.fields.empty() && !bare.intent.variant && !bare.intent.speed && bare.intent.connect.empty());
    const RequestFile fastest = file_of(with("\"speed\": \"max\""));
    CHECK(fastest.intent.speed.has_value() && !fastest.intent.speed->has_value() && fastest.fields == list("speed"));
    // A byte order mark first, as an editor on Windows writes it.
    CHECK(file_of("\xEF\xBB\xBF{\"vehicle_sil_run_request\": 1, \"strict\": true}").intent.strict);
}

TEST_CASE(a_request_file_with_a_mistake_is_refused_with_the_file_and_the_line)
{
    check_text(errors_of("{\n  \"vehicle_sil_run_request\": 1,\n  \"conect\": {}\n}"),
               "bench.json, line 3: unknown key 'conect': did you mean 'connect'? (the keys here: vehicle_sil_run_request, "
               "variant, params, scenario, disable, enable, manual, connect, time_policy, speed, start_paused, strict)\n");
    // What the caller decides is not in the file.
    CHECK(errors_of(with("\"timeline\": \"t.json\"")).find("unknown key 'timeline'") != std::string::npos);
    CHECK(errors_of(with("\"file\": \"sim.yaml\"")).find("unknown key 'file'") != std::string::npos);
    check_text(errors_of("{\n  \"vehicle_sil_run_request\": 1,\n  \"strict\": true,\n  \"strict\": false\n}"),
               "bench.json, line 4: 'strict' appears twice (first at line 3); keep one of them\n");
    check_text(errors_of("{\n \"vehicle_sil_run_request\": 1,\n \"connect\": {\n  \"CAN1\": \"a\",\n  \"CAN1\": \"b\"\n }\n}"),
               "bench.json, line 5: connect: 'CAN1' appears twice (first at line 4); keep one of them\n");
    // The version.
    check_text(errors_of("{ \"strict\": true }"),
               "bench.json, line 1: vehicle_sil_run_request is missing: write \"vehicle_sil_run_request\": 1 (it says that "
               "this is a request file, and of which version)\n");
    check_text(errors_of("{ \"vehicle_sil_run_request\": 2 }"),
               "bench.json, line 1: vehicle_sil_run_request must be 1 (the version of the request file that this "
               "vehicle_sil_launch reads), not '2'\n");
    CHECK(errors_of("{ \"vehicle_sil_run_request\": \"1\" }").find("must be 1") != std::string::npos);
    // Not a JSON object.
    check_text(errors_of(""), "bench.json: the file is empty: a request file is { \"vehicle_sil_run_request\": 1, ... } with "
                              "what to run (docs/simulation-run.md; schema/run-request.schema.json)\n");
    CHECK(errors_of("vehicle_sil_run_request: 1\n").rfind("bench.json: this is not a JSON object: a request file is", 0) == 0);
    CHECK(errors_of("{ \"vehicle_sil_run_request\": 1, ").rfind("bench.json", 0) == 0);
    CHECK(errors_of("{ \"vehicle_sil_run_request\": 1, ").find("syntax error: ") != std::string::npos);
    // The types, each key.
    const auto wrong = [](const std::string& member, const std::string& says) {
        const std::string text = errors_of(with(member));
        if (text.find(says) == std::string::npos)
        {
            std::fprintf(stderr, "for %s: %s\n", member.c_str(), text.c_str());
        }
        CHECK(text.find(says) != std::string::npos);
    };
    wrong("\"variant\": 5", "variant must be the name of a variant of the configuration as a text, not '5'");
    wrong("\"variant\": \"\"", "variant must be the name of a variant");
    wrong("\"scenario\": [\"a.yaml\"]", "scenario must be the path of a scenario file (from the directory of the configuration "
                                        "file) as a text, not a list");
    wrong("\"scenario\": \"\"", "scenario must be the path of a scenario file");
    wrong("\"params\": [1]", "params must be a mapping of parameter names to values");
    wrong("\"params\": {\"x\": [1]}", "params: x must be one value");
    wrong("\"disable\": \"B\"", "disable must be a list of participant names (disable: [\"Engine\"]), not the text \"B\"");
    wrong("\"enable\": [1]", "enable must be a list of participant names, each a text; one of them is '1'");
    wrong("\"manual\": {}", "manual must be a list of participant names");
    wrong("\"connect\": [\"CAN1=x\"]", "connect must be a mapping of network names to targets");
    wrong("\"connect\": {\"CAN1\": 1}", "connect: CAN1 must be a target as a text (\"CAN1\": \"physical:1\"; \"none\": SIL Kit "
                                        "only), not '1'");
    wrong("\"connect\": {\"CAN1\": null}", "connect: CAN1 must be a target as a text");
    wrong("\"time_policy\": \"warn\"", "time_policy must be \"realtime\" (a run that is connected outside the simulation goes "
                                       "at speed 1; the default) or \"free\" (its speed and time can be changed), not the text "
                                       "\"warn\"");
    wrong("\"time_policy\": true", "time_policy must be \"realtime\"");
    wrong("\"speed\": \"fast\"", "speed must be a number of simulation seconds per wall-clock second from 0.000001 to 1000000 "
                                 "(1: real time, 2: twice as fast) or \"max\" (as fast as possible), not the text \"fast\"");
    wrong("\"speed\": \"2\"", "speed must be a number");
    wrong("\"speed\": 0", "speed must be a number");
    wrong("\"speed\": null", "speed must be a number");
    // null is no value of any key: a key that is not meant is left out.
    wrong("\"variant\": null", "variant must be the name of a variant of the configuration as a text, not empty");
    wrong("\"scenario\": null", "scenario must be the path of a scenario file");
    wrong("\"params\": null", "params must be a mapping");
    wrong("\"disable\": null", "disable must be a list of participant names");
    wrong("\"connect\": null", "connect must be a mapping of network names to targets");
    wrong("\"time_policy\": null", "time_policy must be \"realtime\"");
    wrong("\"strict\": null", "strict must be true or false, not empty");
    wrong("\"start_paused\": \"true\"", "start_paused must be true or false, not the text \"true\"");
    wrong("\"strict\": 1", "strict must be true or false, not '1'");
    // The file says two things about one participant.
    check_text(errors_of(with("\"disable\": [\"A\", \"B\"], \"enable\": [\"A\"], \"manual\": [\"B\"]")),
               "bench.json: disable and enable both name A: keep one of them\n"
               "bench.json: disable and manual both name B: keep one of them (disable leaves it out, manual has you start it)\n");
    // Every mistake at once.
    const RequestFileRead two = read_request_file(with("\"strict\": 1, \"speed\": \"fast\""), "bench.json");
    CHECK(two.findings.size() == 2);
}

TEST_CASE(an_argument_wins_over_the_request_file_field_by_field)
{
    const RequestFile file = file_of(with("\"variant\": \"summer\", \"scenario\": \"s/brake.yaml\", \"speed\": 4, "
                                          "\"time_policy\": \"free\", \"start_paused\": true, \"strict\": true"));
    // Nothing on the command line: the file's values, and the origin names them.
    RunRequest none;
    none.host.config = "sim.yaml";
    const RunRequest all = with_request_file(none, file, false);
    CHECK(all.intent.variant == std::optional<std::string>("summer") &&
          all.intent.scenario == std::optional<std::string>("s/brake.yaml") &&
          all.intent.speed == std::optional<std::optional<double>>(4.0) && all.intent.time_policy == TimePolicy::Free &&
          all.intent.start_paused && all.intent.strict);
    CHECK(all.origin.file == "bench.json" &&
          all.origin.fields == list("variant", "scenario", "speed", "time_policy", "start_paused", "strict") &&
          all.origin.connected.empty());
    CHECK(all.host.config == "sim.yaml"); // the caller's part stays
    // variant, scenario, speed, time_policy: the argument, and the origin no longer has the key.
    RunRequest given = none;
    given.intent.variant = "winter";
    given.intent.scenario = "other.yaml";
    given.intent.speed.emplace(std::optional<double>{}); // --speed max
    given.intent.time_policy = TimePolicy::Realtime;     // --time-policy realtime
    const RunRequest mine = with_request_file(given, file, true);
    CHECK(mine.intent.variant == std::optional<std::string>("winter") &&
          mine.intent.scenario == std::optional<std::string>("other.yaml") && mine.intent.speed.has_value() &&
          !mine.intent.speed->has_value() && mine.intent.time_policy == TimePolicy::Realtime);
    CHECK(mine.origin.fields == list("start_paused", "strict"));
    // --time-policy not given: the file's, also over the default.
    CHECK(with_request_file(given, file, false).intent.time_policy == TimePolicy::Free);
    // start_paused, strict: true if either says so; the origin has the key only if the file alone did.
    const RequestFile plain = file_of(with("\"start_paused\": false, \"strict\": false"));
    given = none;
    given.intent.start_paused = true;
    given.intent.strict = true;
    const RunRequest flags = with_request_file(given, plain, false);
    CHECK(flags.intent.start_paused && flags.intent.strict && flags.origin.fields.empty());
    CHECK(with_request_file(given, file, false).origin.has("speed") && !with_request_file(given, file, false).origin.has("strict") &&
          !with_request_file(given, file, false).origin.has("start_paused"));
    CHECK(!with_request_file(none, plain, false).intent.start_paused && !with_request_file(none, plain, false).intent.strict);
}

TEST_CASE(a_parameter_of_the_command_line_replaces_the_request_files_by_name)
{
    const RequestFile file = file_of(with("\"params\": {\"rpm\": 650, \"depth\": \"2\"}"));
    RunRequest given;
    given.intent.params.push_back(argument("depth", "9"));
    given.intent.params.push_back(argument("fill", "1"));
    const RunRequest merged = with_request_file(given, file, false);
    check_text(params_text(merged.intent.params), "depth=9(command_line) fill=1(command_line) rpm=650(request_file) ");
    CHECK(merged.origin.has("params"));
    // Every one replaced: nothing is the file's.
    given.intent.params.push_back(argument("rpm", "700"));
    CHECK(!with_request_file(given, file, false).origin.has("params"));
}

TEST_CASE(a_connect_argument_replaces_the_request_files_target_of_its_network_and_none_takes_it_away)
{
    const std::vector<ConnectRequest> file{{"ETH_EXT", "enp3s0"}, {"CAN1", "physical:1"}, {"CAN2", "none"}, {"CAN3", "virtual:1"}};
    std::vector<std::string> fromFile;
    // Nothing given: the file's, without the one that connects nothing.
    check_text(connect_text(merge_connect({}, file, &fromFile)), "ETH_EXT=enp3s0 CAN1=physical:1 CAN3=virtual:1 ");
    CHECK(fromFile == list("ETH_EXT", "CAN1", "CAN3"));
    // --connect CAN1=none takes the file's CAN1 away (the argument stays, as without a file);
    // --connect ETH_EXT=eth9 replaces the file's; --connect CAN2=virtual:2 over the file's none.
    const std::vector<ConnectRequest> given{{"CAN1", "none"}, {"ETH_EXT", "eth9"}, {"CAN2", "virtual:2"}};
    fromFile.clear();
    check_text(connect_text(merge_connect(given, file, &fromFile)), "CAN1=none ETH_EXT=eth9 CAN2=virtual:2 CAN3=virtual:1 ");
    CHECK(fromFile == list("CAN3"));
    // In a request: the origin has the networks that are the file's.
    const RequestFile bench = file_of(with("\"connect\": {\"ETH_EXT\": \"enp3s0\", \"CAN1\": \"physical:1\"}"));
    RunRequest r;
    r.intent.connect.push_back({"CAN1", "none"});
    const RunRequest merged = with_request_file(r, bench, false);
    check_text(connect_text(merged.intent.connect), "CAN1=none ETH_EXT=enp3s0 ");
    CHECK(merged.origin.connected == list("ETH_EXT") && merged.origin.has("connect"));
    r.intent.connect.push_back({"ETH_EXT", "none"});
    CHECK(with_request_file(r, bench, false).origin.connected.empty() && !with_request_file(r, bench, false).origin.has("connect"));
}

TEST_CASE(the_lists_of_participants_add_up_and_the_command_line_wins_where_they_disagree)
{
    const RequestFile file = file_of(with("\"disable\": [\"B\", \"X\", \"M\"], \"enable\": [\"C\", \"Y\"], \"manual\": [\"A\", \"Z\"]"));
    RunRequest given;
    given.intent.disable = {"D", "Y", "Z", "B"}; // Y, Z: the file takes them in or has them started by hand
    given.intent.enable = {"X"};                 // the file leaves X out
    given.intent.manual = {"M"};                 // the file leaves M out
    const RunRequest merged = with_request_file(given, file, false);
    CHECK(merged.intent.disable == list("D", "Y", "Z", "B"));
    CHECK(merged.intent.enable == list("X", "C"));
    CHECK(merged.intent.manual == list("M", "A"));
    CHECK(merged.origin.fields == list("enable", "manual"));
    // The names that are the file's alone, with their key: a message about one names the file.
    CHECK(merged.origin.participants == list("enable C", "manual A"));
    CHECK(override_conflicts(EnabledOverrides{merged.intent.disable, merged.intent.enable, merged.intent.manual, {}}).empty());
    // Nothing given: the file's lists.
    const RunRequest all = with_request_file(RunRequest{}, file, false);
    CHECK(all.intent.disable == list("B", "X", "M") && all.intent.enable == list("C", "Y") && all.intent.manual == list("A", "Z") &&
          all.origin.fields == list("disable", "enable", "manual"));
}

TEST_CASE(a_rule_about_a_value_of_the_request_file_names_the_key_and_the_file)
{
    RunRequest none;
    // params without a scenario.
    RunRequest r = with_request_file(none, file_of(with("\"params\": {\"rpm\": 650}")), false);
    // The file's params wait for a run with a scenario: one file serves runs with and without.
    CHECK(messages(r).empty() && messages(r, RequestCommand::Check).empty());
    r.intent.params.push_back(argument("x", "1")); // one of the command line needs its scenario
    CHECK(messages(r) == list("--param sets a parameter of a scenario: give --scenario <file> too, or leave --param out"));
    r.intent.scenario = "drive.yaml";
    CHECK(messages(r).empty());
    // A scenario and a paused start, each from where it is.
    const RequestFile both = file_of(with("\"scenario\": \"drive.yaml\", \"start_paused\": true"));
    RunRequest commands;
    commands.host.stdin_commands = true;
    const std::string rest = ": the scenario writes its values for 0 before the first step and runs by itself; pause and step "
                             "it with --stdin-commands or from a Control Server client (--control) if needed";
    CHECK(messages(with_request_file(commands, both, false)) ==
          list("scenario in bench.json cannot be used with start_paused in bench.json" + rest));
    RunRequest scenario = commands;
    scenario.intent.scenario = "other.yaml";
    CHECK(messages(with_request_file(scenario, both, false)) ==
          list("--scenario cannot be used with start_paused in bench.json" + rest));
    RunRequest paused = commands;
    paused.intent.start_paused = true;
    CHECK(messages(with_request_file(paused, both, false)) == list("scenario in bench.json cannot be used with --start-paused" + rest));
    // A paused start needs commands: a run's rule. A check of the same file starts nothing.
    const RequestFile hold = file_of(with("\"start_paused\": true"));
    CHECK(messages(with_request_file(none, hold, false)) ==
          list("start_paused in bench.json needs --stdin-commands or --control: without commands nothing could make the "
               "simulation go on (resume, step)"));
    CHECK(messages(with_request_file(none, hold, false), RequestCommand::Check).empty());
    CHECK(messages(with_request_file(commands, hold, false)).empty());
}

TEST_CASE(the_way_out_of_the_time_lock_names_the_request_file_for_the_networks_it_connects)
{
    const std::string freely = "set time_policy free in bench.json if what is connected there can take messages that do not come "
                               "in real time (the bridge still carries frames both ways)";
    const std::optional<std::optional<double>> two(2.0);
    RequestOrigin origin;
    origin.file = "bench.json";
    origin.fields = {"connect", "speed"};
    origin.connected = {"CAN", "ETH"};
    // Every connected network is the file's: the user typed no --connect to leave out.
    check_text(lines(time_lock_findings(TimeLock{true, {"CAN", "ETH"}, {}}, two, 1.0, true, origin)),
               "E outside-real-time: connect in bench.json connects CAN and ETH to a real bus, which does not wait for the virtual "
               "time, so the run goes at speed 1: speed 2 in bench.json is refused. Take speed out of bench.json, or pass "
               "--speed 1; to go at another speed, take CAN and ETH out of connect in bench.json, or pass --connect CAN=none "
               "--connect ETH=none, or " + freely + "\n"
               "E outside-real-time: connect in bench.json connects CAN and ETH to a real bus, which does not stand still: "
               "--start-paused is refused. To stop the simulation and look at it, take CAN and ETH out of connect in "
               "bench.json, or pass --connect CAN=none --connect ETH=none, or " + freely + "\n");
    // The note about the file's speed of the configuration says the same.
    origin.fields = {"connect"};
    origin.connected = {"CAN"};
    check_text(lines(time_lock_findings(TimeLock{true, {"CAN"}, {}}, {}, 2.0, false, origin)),
               "N outside-speed-set: connect in bench.json connects CAN to a real bus, which does not wait for the virtual time: the "
               "run goes at speed 1 (real time), not at the speed 2 of the file. To go at another speed, take CAN out of "
               "connect in bench.json, or pass --connect CAN=none, or " + freely + "\n");
    // One network of the file, one of the command line, and a bridge of the configuration.
    check_text(lines(time_lock_findings(TimeLock{true, {"ETH", "CAN"}, {"Gateway"}}, {}, 2.0, false, origin)),
               "N outside-speed-set: connect in bench.json and --connect connect ETH and CAN to a real bus, and participant 'Gateway' is a bridge "
               "(role: bridge) to something outside the simulation, which does not wait for the virtual time: the run goes "
               "at speed 1 (real time), not at the speed 2 of the file. To go at another speed, take CAN out of connect in "
               "bench.json, or pass --connect CAN=none, and leave the other --connect out, and run with --disable Gateway, "
               "or " + freely + "\n");
    // A network of the file that an argument replaced is the command line's: its words.
    origin.connected = {};
    check_text(lines(time_lock_findings(TimeLock{true, {"CAN"}, {}}, {}, 2.0, false, origin)),
               "N outside-speed-set: --connect connects CAN to a real bus, which does not wait for the virtual time: the "
               "run goes at speed 1 (real time), not at the speed 2 of the file. To go at another speed, run without "
               "--connect, or " + freely + "\n");
    // No request file at all: the options.
    check_text(lines(time_lock_findings(TimeLock{true, {"CAN"}, {}}, {}, 2.0, false, RequestOrigin{})),
               "N outside-speed-set: --connect connects CAN to a real bus, which does not wait for the virtual time: the "
               "run goes at speed 1 (real time), not at the speed 2 of the file. To go at another speed, run without "
               "--connect, or pass --time-policy free if what is connected there can take messages that do not come in real "
               "time (the bridge still carries frames both ways)\n");
}

TEST_CASE(a_participant_of_the_request_file_that_the_configuration_lacks_is_an_error_that_names_the_file)
{
    const char* const sim = "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n  - name: A\n    command: [missing/a]\n";
    const RequestFile bench = *read_request_file(with("\"manual\": [\"Engine\"], \"disable\": [\"A\"]"), "bench.json").file;
    RunRequest given;
    given.intent.enable = {"Motor"};
    const RunRequest merged = with_request_file(given, bench, false);
    EnabledOverrides overrides;
    overrides.disable = merged.intent.disable;
    overrides.enable = merged.intent.enable;
    overrides.manual = merged.intent.manual;
    overrides.origin = merged.origin;
    check_text(lines(read_config(sim, overrides).override_errors),
               "E usage: --enable Motor: the file has no participant with this name; its participants are A\n"
               "E usage: manual Engine in bench.json: the configuration file has no participant with this name; its "
               "participants are A\n");
    // The same names on the command line: the options, as before.
    overrides.origin = {};
    check_text(lines(read_config(sim, overrides).override_errors),
               "E usage: --enable Motor: the file has no participant with this name; its participants are A\n"
               "E usage: --manual Engine: the file has no participant with this name; its participants are A\n");
}

TEST_CASE(the_findings_about_the_time_name_the_request_file_for_what_it_gave)
{
    const std::string freely = "set time_policy free in bench.json if what is connected there can take messages that do not come "
                               "in real time (the bridge still carries frames both ways)";
    const TimeLock locked{true, {"ETH"}, {}};
    RequestOrigin origin;
    origin.file = "bench.json";
    origin.fields = {"speed", "start_paused"};
    const std::optional<std::optional<double>> five(5.0);
    check_text(lines(time_lock_findings(locked, five, 1.0, true, origin)),
               "E outside-real-time: --connect connects ETH to a real bus, which does not wait for the virtual time, so the "
               "run goes at speed 1: speed 5 in bench.json is refused. Take speed out of bench.json, or pass --speed 1; to "
               "go at another speed, run without --connect, or " + freely + "\n"
               "E outside-real-time: --connect connects ETH to a real bus, which does not stand still: start_paused in "
               "bench.json is refused. To stop the simulation and look at it, run without --connect, or " + freely + "\n");
    // The file's free policy, with a speed and a paused start of the command line, and of the file.
    const TimeLock free{false, {"ETH"}, {}};
    origin.fields = {"time_policy"};
    check_text(lines(time_lock_findings(free, five, 1.0, true, origin)),
               "N outside-not-real-time: --connect connects ETH to a real bus, which does not wait for the virtual time, "
               "and with time_policy free in bench.json --speed 5 runs the simulation at that speed: what is connected "
               "there gets the messages at the wrong times, and the bridge still carries frames both ways. To run in real "
               "time, leave time_policy free in bench.json and --speed out\n"
               "N outside-not-real-time: --connect connects ETH to a real bus, which does not stand still, and with "
               "time_policy free in bench.json --start-paused makes the simulation wait before its first step: the bridge "
               "still carries frames both ways, so frames from outside pile up while the simulation stands still. To run in "
               "real time, leave time_policy free in bench.json and --start-paused out\n");
    origin.fields = {"time_policy", "speed", "start_paused"};
    check_text(lines(time_lock_findings(free, std::optional<std::optional<double>>(std::optional<double>{}), 1.0, true, origin)),
               "N outside-not-real-time: --connect connects ETH to a real bus, which does not wait for the virtual time, "
               "and with time_policy free in bench.json speed max in bench.json runs the simulation as fast as possible: "
               "what is connected there gets the messages at the wrong times, and the bridge still carries frames both "
               "ways. To run in real time, leave time_policy free in bench.json and speed in bench.json out\n"
               "N outside-not-real-time: --connect connects ETH to a real bus, which does not stand still, and with "
               "time_policy free in bench.json start_paused in bench.json makes the simulation wait before its first step: "
               "the bridge still carries frames both ways, so frames from outside pile up while the simulation stands "
               "still. To run in real time, leave time_policy free in bench.json and start_paused in bench.json out\n");
    // The file's free policy with the file's own speed of the configuration: only the policy is named.
    origin.fields = {"time_policy"};
    check_text(lines(time_lock_findings(free, {}, 2.0, false, origin)),
               "N outside-not-real-time: --connect connects ETH to a real bus, which does not wait for the virtual time, "
               "and with time_policy free in bench.json the simulation runs at the speed 2 of the file: what is connected "
               "there gets the messages at the wrong times, and the bridge still carries frames both ways. To run in real "
               "time, leave time_policy free in bench.json out\n");
    // A file that gave none of them: the words of the command line, except the way to the free policy,
    // which a user of the request file gives in the file.
    origin.fields = {"variant"};
    const auto words = [&](const TimeLock& lock, const RequestOrigin& from) {
        std::string text = lines(time_lock_findings(lock, five, 1.0, true, from));
        const std::string file = "set time_policy free in bench.json";
        for (std::size_t at = text.find(file); at != std::string::npos; at = text.find(file, at))
        {
            text.replace(at, file.size(), "pass --time-policy free");
        }
        return text;
    };
    CHECK(words(locked, origin) == words(locked, RequestOrigin{}));
    CHECK(words(free, origin) == words(free, RequestOrigin{}));
    CHECK(lines(time_lock_findings(locked, five, 1.0, true, origin)).find("set time_policy free in bench.json") != std::string::npos);
}

namespace {

const char* const kSim = "vehicle_sil_simulation: 1\nnetworks:\n  - name: ETH1\n    kind: ethernet\nparticipants:\n"
                         "  - name: A\n    command: [missing/a]\n    describe: false\n";
const char* const kDrive = "vehicle_sil_scenario: 1\nsteps:\n  - at_ms: 0\n    set: { A.level: 1 }\n";

/// sim.yaml and its scenario in <scratch>/request_file/project, and another directory to stand in.
// A directory of its own for each test: ctest runs the tests of this file at the same time.
std::filesystem::path project(std::string_view test)
{
    const std::filesystem::path dir = std::filesystem::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "request_file" / test;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "project" / "scenarios");
    std::filesystem::create_directories(dir / "elsewhere");
    std::ofstream(dir / "project" / "sim.yaml", std::ios::binary) << kSim;
    std::ofstream(dir / "project" / "scenarios" / "drive.yaml", std::ios::binary) << kDrive;
    return dir;
}

std::string codes(const CheckReport& report, std::string_view prefix)
{
    std::string out;
    for (const Finding& f : report.findings)
    {
        if (f.code.rfind(prefix, 0) == 0)
        {
            out += f.code + " ";
        }
    }
    return out;
}

} // namespace

TEST_CASE(a_network_of_the_request_files_connect_that_the_configuration_lacks_is_left_out_with_a_note)
{
    const ConfigRead read = read_config(kSim);
    CHECK(read.config.has_value());
    if (!read.config)
    {
        return;
    }
    const std::vector<ConnectRequest> connect{{"CAN9", "none"}, {"ETH1", "enp3s0"}, {"CAN1", "physical:1"}, {"ETX", "eth0"}};
    RequestOrigin origin;
    origin.file = "bench.json";
    origin.connected = {"ETH1", "CAN1"};
    const ConfigConnect counted = connect_for_config(*read.config, connect, origin);
    // The file's CAN1 is left out; the command line's CAN9 and ETX stay for the check to refuse.
    check_text(connect_text(counted.connect), "CAN9=none ETH1=enp3s0 ETX=eth0 ");
    CHECK(counted.notes.size() == 1);
    if (counted.notes.size() == 1)
    {
        const Finding& note = counted.notes[0];
        CHECK(note.severity == Severity::Note && note.code == "connect-network-unused" && note.network == "CAN1" && note.line == 0);
        check_text(note.message, "the network CAN1 of connect in bench.json is not in this configuration: it is left out");
    }
    // Without a file nothing is left out.
    CHECK(connect_for_config(*read.config, connect, RequestOrigin{}).connect == connect &&
          connect_for_config(*read.config, connect, RequestOrigin{}).notes.empty());

    // In a check: the note, no error, and the network does not lock the time; the same request
    // on the command line is the error connect-network.
    const std::filesystem::path dir = project("unused_network");
    const vehicle_sil::launch::test::InDirectory there(dir / "project");
    const CheckOptions fileOnly{.params_for_many = false, .file_only = true, .fastest_by_default = false};
    RunRequest given;
    given.host.config = "sim.yaml";
    const RequestFile bench = *read_request_file(with("\"connect\": {\"CAN1\": \"physical:1\"}"), "bench.json").file;
    for (const CheckOptions& options : {CheckOptions{}, fileOnly})
    {
        const Checked fromFile = check_file(with_request_file(given, bench, false), options);
        check_text(codes(fromFile, "connect-"), "connect-network-unused ");
        CHECK(!fromFile.time_lock.locked && fromFile.time_lock.connected.empty() && fromFile.outside.empty());
        CHECK(!options.file_only || (fromFile.errors() == 0 && fromFile.passed()));
        RunRequest typed = given;
        typed.intent.connect.push_back({"CAN1", "physical:1"});
        const Checked fromArguments = check_file(typed, options);
        check_text(codes(fromArguments, "connect-"), "connect-network ");
        CHECK(!fromArguments.passed() && fromArguments.time_lock.locked);
    }
    // With --strict a note does not fail the check.
    RunRequest strict = with_request_file(given, bench, false);
    strict.intent.strict = true;
    CHECK(check_file(strict, fileOnly).passed());
}

TEST_CASE(the_scenario_of_a_request_file_is_read_from_the_directory_of_the_configuration_file)
{
    const std::filesystem::path dir = project("scenario_base");
    // Standing somewhere else: the configuration is named from here, the scenario from the configuration.
    const vehicle_sil::launch::test::InDirectory there(dir / "elsewhere");
    RunRequest given;
    given.host.config = "../project/sim.yaml";
    const RequestFile bench = *read_request_file(with("\"scenario\": \"scenarios/drive.yaml\""), "bench.json").file;
    const Checked fromFile = check_file(with_request_file(given, bench, false));
    CHECK(fromFile.scenario.has_value() && codes(fromFile, "file").empty());
    if (fromFile.scenario)
    {
        // Shown as the path that works from here.
        CHECK(std::filesystem::path(fromFile.scenario->file) == std::filesystem::path("../project/scenarios/drive.yaml"));
        CHECK(fromFile.scenario->steps == 1);
    }
    // The same name as an argument is from the working directory: not found here.
    RunRequest typed = given;
    typed.intent.scenario = "scenarios/drive.yaml";
    const Checked fromArguments = check_file(typed);
    CHECK(fromArguments.scenario.has_value() && fromArguments.scenario->file == "scenarios/drive.yaml" &&
          codes(fromArguments, "file") == "file ");
    // An argument over the file's: the argument's base.
    typed.intent.scenario = "../project/scenarios/drive.yaml";
    const Checked both = check_file(with_request_file(typed, bench, false));
    CHECK(both.scenario.has_value() && both.scenario->file == "../project/scenarios/drive.yaml" && codes(both, "file").empty());
    // An absolute path in the file stays.
    const std::string absolute = vehicle_sil::process::utf8_from_path(dir / "project" / "scenarios" / "drive.yaml");
    RequestFile fixed = bench;
    fixed.intent.scenario = absolute;
    const Checked direct = check_file(with_request_file(given, fixed, false));
    CHECK(direct.scenario.has_value() && direct.scenario->file == absolute && codes(direct, "file").empty());
}

TEST_CASE(a_parameter_of_the_request_file_that_its_own_scenario_lacks_is_a_usage_error)
{
    const std::filesystem::path dir = project("params_own_scenario");
    const vehicle_sil::launch::test::InDirectory there(dir / "project");
    // The scenario has the parameter "level"; the file writes it wrong.
    std::ofstream(dir / "project" / "scenarios" / "drive.yaml", std::ios::binary)
        << "vehicle_sil_scenario: 1\nparams: { level: 1 }\nsteps:\n  - at_ms: 0\n    set: { A.level: \"${level}\" }\n";
    RunRequest given;
    given.host.config = "sim.yaml";
    const auto file = [](const char* params) {
        return *read_request_file(with(std::string("\"scenario\": \"scenarios/drive.yaml\", \"params\": ") + params),
                                  "bench.json")
                    .file;
    };
    // The file's own scenario: a mistake in its params is a usage error naming the file.
    const Checked wrong = check_file(with_request_file(given, file("{\"leve\": 2}"), false));
    CHECK(wrong.usage && wrong.findings.size() == 1);
    if (wrong.findings.size() == 1)
    {
        check_text(wrong.findings[0].message,
                   "params leve in bench.json: the scenario has no parameter leve (did you mean level?): its parameters are "
                   "level; remove it from params in bench.json or correct the name");
    }
    CHECK(wrong.exit_code() == 4);
    // A name that the scenario has is fine.
    const Checked right = check_file(with_request_file(given, file("{\"level\": 2}"), false));
    CHECK(!right.usage && right.scenario.has_value() && codes(right, "request-params") == "");
    // --scenario replaces the file's: no error, one note naming what was not used.
    RunRequest other = given;
    other.intent.scenario = "scenarios/drive.yaml";
    const Checked replaced = check_file(with_request_file(other, file("{\"leve\": 2, \"mode\": 1}"), false));
    CHECK(!replaced.usage && replaced.scenario.has_value());
    check_text(codes(replaced, "request-params"), "request-params-unused ");
    for (const Finding& f : replaced.findings)
    {
        if (f.code == "request-params-unused")
        {
            check_text(f.message,
                       "params leve, mode in bench.json are not used: the scenario has no parameter of that name; they are "
                       "ignored");
            CHECK(f.severity == Severity::Note);
        }
    }
    // A file without a scenario, the scenario from the arguments: the same note.
    const RequestFile bare = *read_request_file(with("\"params\": {\"leve\": 2}"), "bench.json").file;
    const Checked fromArguments = check_file(with_request_file(other, bare, false));
    CHECK(!fromArguments.usage);
    check_text(codes(fromArguments, "request-params"), "request-params-unused ");
    // No scenario at all: the note says so.
    const Checked none = check_file(with_request_file(given, bare, false));
    CHECK(!none.usage);
    check_text(codes(none, "request-params"), "request-params-unused ");
    // A parameter typed on the command line stays a usage error, as before, with its own words.
    RunRequest typed = other;
    typed.intent.params.push_back(argument("leve", "2"));
    const Checked fromOption = check_file(typed);
    CHECK(fromOption.usage && fromOption.findings.size() == 1);
    if (fromOption.findings.size() == 1)
    {
        check_text(fromOption.findings[0].message,
                   "--param leve=2: the scenario has no parameter leve (did you mean level?): its parameters are level; remove "
                   "the option or correct the name");
    }
}
