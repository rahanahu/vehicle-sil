// SPDX-License-Identifier: MIT
//
// Reading the simulation configuration (vehicle_sil::launch::read_config()): a correct file in
// YAML and in JSON, every rule of the text with its message and line, many errors at once, the
// files that cannot be read; the executables (resolve_executable(), check_executable()); the
// placeholders; "did you mean".
#include "launch_test_util.hpp"

#include "vehicle_sil/launch/config.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using vehicle_sil::launch::test::check_text;
using vehicle_sil::launch::test::lines;

namespace {

constexpr const char* kExample = R"(# two ECUs
vehicle_sil_simulation: 1
stop_at_ms: 5000
start_timeout_ms: 20000
networks:
  - name: J1939_CAN
    kind: can
  - name: Body_LIN
    kind: lin
participants:
  - name: Engine
    command: [engine/engine_ecu]
    step_ms: 10
  - name: Logger
    command: [tools/logger, --registry-uri, "{registry_uri}", --name, "{name}"]
    describe: false
    required: false
)";

/// The findings of `text` as lines (lines()).
std::string read(const std::string& text)
{
    return lines(read_config(text).findings);
}

/// A minimal correct file with `participants` as its participants list.
std::string with_participants(const std::string& participants)
{
    return "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n" + participants;
}

fs::path scratch(const std::string& name)
{
    const fs::path dir = fs::path(VEHICLE_SIL_LAUNCH_SCRATCH) / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE(reads_a_correct_file)
{
    const ConfigRead read = read_config(kExample);
    check_text(lines(read.findings), "");
    CHECK(read.config.has_value());
    const SimulationConfig& c = *read.config;
    CHECK(c.stop_at_ms == 5000);
    CHECK_EQ(c.start_timeout_ms, 20000);
    CHECK_EQ(c.networks.size(), std::size_t{2});
    CHECK(c.networks[0].name == "J1939_CAN" && c.networks[0].kind == NetworkKind::Can && c.networks[0].line == 6);
    CHECK(c.networks[1].name == "Body_LIN" && c.networks[1].kind == NetworkKind::Lin && c.networks[1].line == 8);
    CHECK(c.networks[0].valid && c.networks[1].valid);
    CHECK_EQ(c.participants.size(), std::size_t{2});
    const ParticipantConfig& engine = c.participants[0];
    CHECK(engine.name == "Engine" && engine.command == std::vector<std::string>{"engine/engine_ecu"});
    CHECK(engine.describe && engine.required && engine.step_ms == 10 && engine.valid);
    CHECK(engine.line == 11 && engine.command_line == 12 && engine.step_line == 13);
    const ParticipantConfig& logger = c.participants[1];
    CHECK((logger.command == std::vector<std::string>{"tools/logger", "--registry-uri", "{registry_uri}", "--name", "{name}"}));
    CHECK(!logger.describe && !logger.required && !logger.step_ms && logger.valid);
    CHECK(logger.line == 14 && logger.command_line == 15 && logger.step_line == 14);
}

TEST_CASE(defaults_of_the_optional_keys)
{
    const ConfigRead read = read_config(with_participants("  - name: A\n    command: a\n"));
    check_text(lines(read.findings), "");
    CHECK(!read.config->stop_at_ms);
    CHECK_EQ(read.config->start_timeout_ms, 30000);
    // A command written as one text is the executable alone (not split at spaces).
    CHECK(read.config->participants[0].command == std::vector<std::string>{"a"});
    CHECK(read_config(with_participants("  - name: A\n    command: dir with spaces/a b\n"))
              .config->participants[0]
              .command == std::vector<std::string>{"dir with spaces/a b"});
}

TEST_CASE(reads_the_same_file_as_json)
{
    const std::string json = R"({
  "vehicle_sil_simulation": 1,
  "stop_at_ms": 5000,
  "start_timeout_ms": 20000,
  "networks": [{"name": "J1939_CAN", "kind": "can"}, {"name": "Body_LIN", "kind": "lin"}],
  "participants": [
    {"name": "Engine", "command": ["engine/engine_ecu"], "step_ms": 10},
    {"name": "Logger", "command": ["tools/logger", "--registry-uri", "{registry_uri}", "--name", "{name}"],
     "describe": false, "required": false}
  ]
})";
    const ConfigRead fromJson = read_config(json);
    const ConfigRead fromYaml = read_config(kExample);
    check_text(lines(fromJson.findings), "");
    CHECK(fromJson.config.has_value());
    CHECK(fromJson.config->stop_at_ms == fromYaml.config->stop_at_ms);
    CHECK_EQ(fromJson.config->networks.size(), std::size_t{2});
    CHECK(fromJson.config->networks[1].name == "Body_LIN" && fromJson.config->networks[1].kind == NetworkKind::Lin);
    CHECK(fromJson.config->participants[1].command == fromYaml.config->participants[1].command);
    CHECK(!fromJson.config->participants[1].describe && fromJson.config->participants[0].step_ms == 10);
    // Lines in JSON too.
    CHECK_EQ(fromJson.config->participants[0].line, std::size_t{7});
    // A JSON error has its line.
    check_text(lines(read_config(R"({"vehicle_sil_simulation": 1, "networks": [], "participants": [
  {"name": "A", "command": "a", "step_ms": "10"}]})")
                         .findings),
               "E 2 value: participant 'A': step_ms must be a whole number (the step the executable reports), not the text "
               "\"10\": write the number without quotes\n");
}

TEST_CASE(a_byte_order_mark_is_skipped)
{
    const ConfigRead read = read_config("\xEF\xBB\xBF" + std::string(kExample));
    check_text(lines(read.findings), "");
    CHECK(read.config.has_value());
}

TEST_CASE(unknown_keys_are_errors_with_a_suggestion)
{
    check_text(read(std::string(kExample) + "stop_at: 3\nextra: 1\n"),
               "E 18 unknown-key: unknown key 'stop_at': did you mean 'stop_at_ms'? (the keys here: "
               "vehicle_sil_simulation, stop_at_ms, start_timeout_ms, stop_timeout_ms, registry_uri, time_step_ms, speed, "
               "late_tolerance_ms, networks, participants)\n"
               "E 19 unknown-key: unknown key 'extra': remove it, or use one of the keys here (vehicle_sil_simulation, "
               "stop_at_ms, start_timeout_ms, stop_timeout_ms, registry_uri, time_step_ms, speed, late_tolerance_ms, "
               "networks, participants)\n");
    check_text(read(with_participants("  - name: A\n    command: a\n    requird: false\n    Step_ms: 5\n")),
               "E 6 unknown-key: participant 'A': unknown key 'requird': did you mean 'required'? (the keys here: name, "
               "command, step_ms, describe, required, enabled, manual)\n"
               "E 7 unknown-key: participant 'A': unknown key 'Step_ms': did you mean 'step_ms'? (the keys here: name, "
               "command, step_ms, describe, required, enabled, manual)\n");
    check_text(read("vehicle_sil_simulation: 1\nnetworks:\n  - name: N\n    kind: can\n    bitrate: 250000\n"
                    "participants:\n  - name: A\n    command: a\n"),
               "E 5 unknown-key: network 'N': unknown key 'bitrate': remove it, or use one of the keys here (name, kind)\n");
}

TEST_CASE(the_speed_the_step_of_the_controller_and_the_tolerance)
{
    const auto config = [](const std::string& keys) {
        return read_config("vehicle_sil_simulation: 1\n" + keys + "networks: []\nparticipants:\n  - name: A\n    command: a\n");
    };
    // Not written: as fast as possible, steps of 10 ms, a tolerance of one step.
    ConfigRead r = config("");
    CHECK(!r.config->speed && !r.config->time_step_ms && !r.config->late_tolerance_ms);
    CHECK_EQ(time_step_ms(*r.config), 10);
    CHECK_EQ(late_tolerance_ms(*r.config), 10);
    r = config("speed: 0.5\ntime_step_ms: 20\nlate_tolerance_ms: 50\nstop_at_ms: 1000\n");
    check_text(lines(r.findings), "");
    CHECK(r.config->speed == 0.5);
    CHECK_EQ(time_step_ms(*r.config), 20);
    CHECK_EQ(late_tolerance_ms(*r.config), 50);
    CHECK(!config("speed: max\n").config->speed);
    CHECK(!config("speed: \"max\"\n").config->speed);
    CHECK(config("speed: 1e3\n").config->speed == 1000.0);
    CHECK(config("speed: 2\n").config->speed == 2.0);
    // Without time_step_ms, the step divides stop_at_ms: the simulation stops at stop_at_ms itself.
    CHECK_EQ(time_step_ms(*config("stop_at_ms: 1005\n").config), 5);
    CHECK_EQ(time_step_ms(*config("stop_at_ms: 1001\n").config), 1);
    CHECK_EQ(time_step_ms(*config("stop_at_ms: 3000\n").config), 10);
    // The tolerance does not follow the step: wall-clock time, 10 ms unless written.
    CHECK_EQ(late_tolerance_ms(*config("stop_at_ms: 1005\n").config), 10);
    CHECK_EQ(late_tolerance_ms(*config("stop_at_ms: 1001\ntime_step_ms: 1\n").config), 10);
    CHECK_EQ(late_tolerance_ms(*config("time_step_ms: 50\n").config), 10);
    // Written, it must divide stop_at_ms.
    check_text(lines(config("stop_at_ms: 1005\ntime_step_ms: 10\n").findings),
               "E 3 value: stop_at_ms (1005) is not a multiple of time_step_ms (10): the launcher's controller can stop the "
               "simulation only at one of its steps; make stop_at_ms a multiple of time_step_ms, or leave time_step_ms out\n");
    const std::string speed =
        "speed must be simulation seconds per wall-clock second from 0.000001 to 1000000 (1: real time, 2: twice as fast, "
        "0.5: half as fast), or max (as fast as possible), not ";
    check_text(lines(config("speed: 0\ntime_step_ms: 0\nlate_tolerance_ms: x\n").findings),
               "E 2 value: " + speed + "'0'\n"
               "E 3 value: time_step_ms is 0, but it must be from 1 to 1000000000000 (milliseconds of virtual time between two "
               "steps of the launcher's controller)\n"
               "E 4 value: late_tolerance_ms must be a whole number (wall-clock milliseconds a step may begin late before it "
               "counts as late), not 'x'\n");
    for (const std::string bad : {"-1", "fast", "inf", "nan", "2000000", ".5", "1/2", "Max", "0.0000001"})
    {
        check_text(lines(config("speed: " + bad + "\n").findings), "E 2 value: " + speed + "'" + bad + "'\n");
    }
    check_text(lines(config("speed: \"2\"\n").findings), "E 2 value: " + speed + "the text \"2\": write the number without quotes\n");
    check_text(lines(config("speed: [1]\n").findings), "E 2 value: " + speed + "a list\n");
    // The text for people and the JSON.
    check_text(speed_text(std::nullopt), "max");
    check_text(speed_text(0.25), "0.25");
    check_text(speed_text(1.0), "1");
}

TEST_CASE(an_unknown_key_does_not_stop_the_participant_from_being_checked)
{
    const ConfigRead read = read_config(with_participants("  - name: A\n    command: a\n    requird: false\n"));
    CHECK(read.config->participants[0].valid);
}

TEST_CASE(a_key_twice_is_an_error)
{
    check_text(read(with_participants("  - name: A\n    command: a\n    command: b\n")),
               "E 6 duplicate-key: participant 'A': 'command' appears twice (first at line 5); keep one of them\n");
    check_text(read("vehicle_sil_simulation: 1\nstop_at_ms: 1\nstop_at_ms: 2\nnetworks: []\nparticipants:\n  - name: A\n    "
                    "command: a\n"),
               "E 3 duplicate-key: 'stop_at_ms' appears twice (first at line 2); keep one of them\n");
}

TEST_CASE(missing_keys_are_errors)
{
    check_text(read("stop_at_ms: 1\n"),
               "E 0 missing-key: the file has no vehicle_sil_simulation (the format version): add "
               "vehicle_sil_simulation: 1\n"
               "E 0 missing-key: the file has no networks: add networks, a list of the networks with name and kind (can "
               "or lin)\n"
               "E 0 missing-key: the file has no participants: add participants, a list of the participants with name "
               "and command\n");
    check_text(read("vehicle_sil_simulation: 1\nnetworks:\n  - kind: can\n  - name: N\nparticipants:\n  - command: [a]\n  - "
                    "name: B\n"),
               "E 3 missing-key: networks[0] has no name: add name: <the network's name>\n"
               "E 4 missing-key: network 'N': no kind: add kind: can or kind: lin\n"
               "E 6 missing-key: participants[0] has no name: add name: <the participant's name> (an ecu_main executable "
               "gets it with --name)\n"
               "E 7 missing-key: participant 'B': no command: add command: [<path of the executable>]\n");
}

TEST_CASE(values_of_the_wrong_type_or_range_are_errors)
{
    check_text(read("vehicle_sil_simulation: 1\nstop_at_ms: 0\nstart_timeout_ms: 1.5\nnetworks: {}\nparticipants: x\n"),
               "E 2 value: stop_at_ms is 0, but it must be from 1 to 1000000000000 (milliseconds of virtual time; leave "
               "stop_at_ms out to run until stopped)\n"
               "E 3 value: start_timeout_ms must be a whole number (milliseconds to wait for the participants), not '1.5'\n"
               "E 4 value: networks must be a list of networks (\"- name: <name>\" and \"kind: can\" or \"kind: lin\"), "
               "not a mapping\n"
               "E 5 value: participants must be a list of participants (\"- name: <name>\" and \"command: "
               "[<executable>]\"), not 'x'\n");
    check_text(read(with_participants("  - name: A\n    command: a\n    step_ms: -5\n    describe: yes\n    required:\n")),
               "E 6 value: participant 'A': step_ms is -5, but it must be from 1 to 1000000000000 (the step the "
               "executable reports)\n"
               "E 7 value: participant 'A': describe must be true or false, not 'yes'\n"
               "E 8 value: participant 'A': required must be true or false, not empty\n");
    check_text(read("vehicle_sil_simulation: 1\nnetworks:\n  - name: A\n    kind: CAN\n  - name: B\n    kind: ethernet\n  - "
                    "name: [x]\n    kind: can\n  - just text\nparticipants: []\n"),
               "E 4 value: network 'A': kind is 'CAN': write can (lower case)\n"
               "E 6 value: network 'B': kind is 'ethernet': write can or lin (the kinds of network there are)\n"
               "E 7 value: networks[2]: name must be the network's name, not a list\n"
               "E 9 value: networks[3] must be a network with name and kind (can or lin), not 'just text'\n"
               "E 10 value: participants is empty: list the participants of the simulation\n");
}

TEST_CASE(a_bad_describe_value_stops_the_participant_from_being_checked)
{
    const ConfigRead read = read_config(with_participants("  - name: A\n    command: a\n    describe: 1\n"));
    CHECK(!read.config->participants[0].valid);
    const ConfigRead stepBad = read_config(with_participants("  - name: A\n    command: a\n    step_ms: x\n"));
    CHECK(stepBad.config->participants[0].valid);
    CHECK(!stepBad.config->participants[0].step_ms);
}

TEST_CASE(a_network_without_a_valid_kind_stays_listed_but_invalid)
{
    const ConfigRead read =
        read_config("vehicle_sil_simulation: 1\nnetworks:\n  - name: A\n    kind: x\nparticipants:\n  - name: P\n    command: p\n");
    CHECK_EQ(read.config->networks.size(), std::size_t{1});
    CHECK(read.config->networks[0].name == "A" && !read.config->networks[0].valid);
}

TEST_CASE(names_twice_and_reserved_names_are_errors)
{
    check_text(read("vehicle_sil_simulation: 1\nnetworks:\n  - name: N\n    kind: can\n  - name: N\n    kind: lin\n"
                    "participants:\n  - name: A\n    command: a\n  - name: A\n    command: b\n  - name: SimulationController\n"
                    "    command: c\n  - name: --x\n    command: d\n  - name: \"\"\n    command: e\n"),
               "E 5 duplicate-name: network 'N' is listed twice (first at line 3); remove one, or give it another name\n"
               "E 10 duplicate-name: participant 'A' is listed twice (first at line 8); give each participant its own name "
               "(for a second copy of an ECU, e.g. A_2)\n"
               "E 12 reserved-name: participant 'SimulationController': the name SimulationController is the controller's, "
               "which vehicle_sil_launch runs: choose another name\n"
               "E 14 value: participant '--x': a participant's name must not start with '-' (the executable would read it "
               "as an option)\n"
               "E 16 value: participants[4]: name must be the participant's name, not the text \"\"\n");
    const ConfigRead r = read_config(with_participants("  - name: A\n    command: a\n  - name: A\n    command: b\n"));
    CHECK(r.config->participants[0].valid && !r.config->participants[1].valid);
}

TEST_CASE(commands_must_be_texts)
{
    check_text(read(with_participants("  - name: A\n    command: []\n  - name: B\n    command: [b, {registry_uri}]\n"
                                      "  - name: C\n    command: [\"\", x]\n  - name: D\n    command:\n")),
               "E 5 value: participant 'A': command must be the executable and its arguments, e.g. command: "
               "[engine/engine_ecu], not a list\n"
               "E 7 value: participant 'B': command[1] must be a text, not a mapping: write \"{registry_uri}\" in quotes "
               "(without them, YAML reads {...} as a mapping)\n"
               "E 9 value: participant 'C': command must be the executable and its arguments, e.g. command: "
               "[engine/engine_ecu], not a list\n"
               "E 11 value: participant 'D': command must be the executable and its arguments, e.g. command: "
               "[engine/engine_ecu], not empty\n");
}

TEST_CASE(an_ecu_main_command_has_no_launcher_argument_and_no_placeholder)
{
    check_text(read(with_participants("  - name: A\n    command: [a, --name, X, \"--registry-uri={registry_uri}\", -h, --verbose]\n")),
               "E 5 launcher-argument: participant 'A': the command has '--name', but vehicle_sil_launch gives an "
               "ecu_main executable --name itself: remove it (or set describe: false for an executable not made with "
               "ecu_main)\n"
               "E 5 launcher-argument: participant 'A': the command has '--registry-uri={registry_uri}', but "
               "vehicle_sil_launch gives an ecu_main executable --registry-uri itself: remove it (or set describe: false "
               "for an executable not made with ecu_main)\n"
               "E 5 launcher-argument: participant 'A': the command has {registry_uri}, but only a command with "
               "describe: false has placeholders (vehicle_sil_launch gives an ecu_main executable --name and "
               "--registry-uri itself): remove it, or set describe: false\n"
               "E 5 launcher-argument: participant 'A': the command has '-h', but vehicle_sil_launch gives an ecu_main "
               "executable -h itself: remove it (or set describe: false for an executable not made with ecu_main)\n");
    const ConfigRead r = read_config(with_participants("  - name: A\n    command: [a, --describe]\n  - name: B\n    command: [b, --verbose]\n"));
    CHECK(!r.config->participants[0].valid);
    CHECK(r.config->participants[1].valid);
}

TEST_CASE(other_commands_have_known_placeholders_and_the_registry)
{
    check_text(read(with_participants("  - name: L\n    describe: false\n    command: [l, --uri, \"{registry}\", \"{name}\", "
                                      "\"{x}\", \"json {\\\"a\\\": 1}\"]\n")),
               "E 6 unknown-placeholder: participant 'L': unknown placeholder {registry} in the command: did you mean "
               "{registry_uri}? (the placeholders: {registry_uri}, {name}; write {{registry}} to pass {registry} as it "
               "is)\n"
               "E 6 unknown-placeholder: participant 'L': unknown placeholder {x} in the command (the placeholders: "
               "{registry_uri}, {name}; write {{x}} to pass {x} as it is)\n"
               "N 6 no-registry-uri: participant 'L': the command has no {registry_uri}, so the executable is not told "
               "where the simulation's registry is: if it needs it, add its option for the registry URI with "
               "\"{registry_uri}\" as the value\n");
    // "{{" and "}}" are braces: no placeholder.
    check_text(read(with_participants("  - name: L\n    describe: false\n    command: [l, \"--uri={registry_uri}\", "
                                      "\"--pattern={{id}}\", \"${{HOME}}\", \"{{{name}}}\"]\n")),
               "");
    // The executable's path is not expanded: any {word} there is an error; {{word}} is the word.
    check_text(read(with_participants("  - name: L\n    describe: false\n    command: [\"{name}/l\", \"{registry_uri}\"]\n"
                                      "  - name: M\n    describe: false\n    command: [\"{odd}/m\", \"{registry_uri}\"]\n"
                                      "  - name: N\n    describe: false\n    command: [\"{{odd}}/n\", \"{registry_uri}\"]\n")),
               "E 6 value: participant 'L': the executable's path has {name}, but a path has no placeholders: write "
               "{{name}} for a literal {name}, or write the path\n"
               "E 9 value: participant 'M': the executable's path has {odd}, but a path has no placeholders: write "
               "{{odd}} for a literal {odd}, or write the path\n");
}

TEST_CASE(an_ecu_main_command_has_no_braced_words_but_other_braces_pass)
{
    check_text(read(with_participants("  - name: A\n    command: [a, \"--pattern={id}\", \"{{name}}\", "
                                      "'{\"a\":{\"b\":1}}', \"a}}b{{\"]\n")),
               "E 5 unknown-placeholder: participant 'A': the command has {id}, but an ecu_main command has no "
               "placeholders: write {{id}} to pass {id} as it is\n");
}

TEST_CASE(arguments_are_not_checked_while_describe_is_not_known)
{
    // describe: flase is an error; the arguments are not judged as if it were true.
    check_text(read(with_participants("  - name: L\n    describe: flase\n    command: [l, \"{name}\", \"{registry_uri}\"]\n")),
               "E 5 value: participant 'L': describe must be true or false, not 'flase'\n");
}

TEST_CASE(step_ms_of_a_participant_that_is_not_described_is_noted)
{
    check_text(read(with_participants("  - name: L\n    describe: false\n    command: [l, \"{registry_uri}\"]\n    step_ms: 5\n")),
               "N 7 step-not-checked: participant 'L': step_ms is not compared for a participant with describe: false "
               "(it does not report its step): remove it, or set describe: true for an ecu_main executable\n");
}

TEST_CASE(names_are_text_on_one_line)
{
    check_text(read("vehicle_sil_simulation: 1\nnetworks:\n  - name: \"N\\tX\"\n    kind: can\nparticipants:\n"
                    "  - name: \"A\\nB\"\n    command: a\n  - name: \"C\\x01\"\n    command: c\n"),
               "E 3 value: networks[0]: the network's name must be text on one line (it has the control character "
               "U+0009): write the name without it\n"
               "E 6 value: participants[0]: the participant's name must be text on one line (it has the control "
               "character U+000A): write the name without it\n"
               "E 8 value: participants[1]: the participant's name must be text on one line (it has the control "
               "character U+0001): write the name without it\n");
}

TEST_CASE(anchors_aliases_and_more_documents_are_refused)
{
    const ConfigRead aliased = read_config("vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n"
                                           "  - name: &n Engine\n    command: [a]\n  - name: *n\n    command: [b]\n");
    CHECK(!aliased.config.has_value());
    check_text(lines(aliased.findings),
               "E 4 unsupported-yaml: anchors and aliases (&name, *name, <<) are not read: write the values out (a value that "
               "starts with * or & must be in quotes)\n"
               "E 6 unsupported-yaml: anchors and aliases (&name, *name, <<) are not read: write the values out (a value that "
               "starts with * or & must be in quotes)\n");
    const ConfigRead merged = read_config("vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n"
                                          "  - name: A\n    <<: {command: [a]}\n");
    CHECK(!merged.config.has_value());
    // A "---" at the end is no second document.
    const ConfigRead tail = read_config(std::string(kExample) + "---\n");
    CHECK(tail.config.has_value() && tail.findings.empty());
    const ConfigRead two = read_config(std::string(kExample) + "---\nvehicle_sil_simulation: 1\n");
    CHECK(two.config.has_value());
    CHECK_EQ(two.config->participants.size(), std::size_t{2});
    CHECK_EQ(two.findings.size(), std::size_t{1});
    CHECK(two.findings[0].code == "unsupported-yaml");
    CHECK_EQ(two.findings[0].line, std::size_t{19});
    check_text(two.findings[0].message, "a second document (after ---): a file has one configuration; only the first is "
                                        "read, so remove the others or put them in files of their own");
}

TEST_CASE(a_file_that_is_not_utf8_is_refused)
{
    const ConfigRead utf16 = read_config(std::string("\xFF\xFEv\0e\0", 6));
    CHECK(!utf16.config.has_value());
    check_text(lines(utf16.findings), "E 0 encoding: the file is not UTF-8 text (it looks like UTF-16, or has NUL bytes): "
                                      "save it as UTF-8\n");
}

TEST_CASE(numbers_too_large_are_out_of_range)
{
    check_text(read(with_participants("  - name: A\n    command: a\n    step_ms: 99999999999999999999\n")),
               "E 6 value: participant 'A': step_ms is 99999999999999999999, but it must be from 1 to 1000000000000 (the "
               "step the executable reports)\n");
}

TEST_CASE(many_errors_are_reported_at_once_in_the_order_of_the_file)
{
    const std::string text = "vehicle_sil_simulation: 1\n"  // 1
                             "stop_at_ms: never\n"          // 2
                             "networks:\n"                  // 3
                             "  - name: N\n"                // 4
                             "    kind: CAN\n"              // 5
                             "participants:\n"              // 6
                             "  - name: A\n"                // 7
                             "    command: [a, --local]\n"  // 8
                             "    step_ms: 0\n"             // 9
                             "  - name: A\n"                // 10
                             "    commands: [b]\n";         // 11
    check_text(read(text),
               "E 2 value: stop_at_ms must be a whole number (milliseconds of virtual time; leave stop_at_ms out to run "
               "until stopped), not 'never'\n"
               "E 5 value: network 'N': kind is 'CAN': write can (lower case)\n"
               "E 9 value: participant 'A': step_ms is 0, but it must be from 1 to 1000000000000 (the step the executable "
               "reports)\n"
               "E 8 launcher-argument: participant 'A': the command has '--local', but vehicle_sil_launch gives an "
               "ecu_main executable --local itself: remove it (or set describe: false for an executable not made with "
               "ecu_main)\n"
               "E 11 unknown-key: participant 'A': unknown key 'commands': did you mean 'command'? (the keys here: name, "
               "command, step_ms, describe, required, enabled, manual)\n"
               "E 10 missing-key: participant 'A': no command: add command: [<path of the executable>]\n"
               "E 10 duplicate-name: participant 'A' is listed twice (first at line 7); give each participant its own name "
               "(for a second copy of an ECU, e.g. A_2)\n");
}

TEST_CASE(files_that_cannot_be_read)
{
    const auto unreadable = [](const std::string& text) {
        const ConfigRead r = read_config(text);
        CHECK(!r.config.has_value());
        return lines(r.findings);
    };
    check_text(unreadable("  \n\n"), "E 0 not-a-configuration: the file is empty: write vehicle_sil_simulation: 1, networks "
                                     "and participants (docs/simulation-config.md)\n");
    check_text(unreadable("- a\n- b\n"),
               "E 1 not-a-configuration: the file is not a configuration: it must be a mapping with "
               "vehicle_sil_simulation: 1, networks and participants (docs/simulation-config.md)\n");
    // An unknown version: nothing else is reported (the keys may mean something else there).
    check_text(unreadable("unknown: 1\nvehicle_sil_simulation: 2\nnetworks: x\n"),
               "E 2 format-version: vehicle_sil_simulation is 2, but this vehicle_sil_launch reads format 1 only: use the "
               "vehicle-sil that the file was written for\n");
    const ConfigRead syntax = read_config("vehicle_sil_simulation: 1\nnetworks: [a\nparticipants: []\n");
    CHECK(!syntax.config.has_value());
    CHECK_EQ(syntax.findings.size(), std::size_t{1});
    CHECK(syntax.findings[0].code == "syntax" && syntax.findings[0].line >= 2);
    CHECK(syntax.findings[0].message.rfind("syntax error: ", 0) == 0);
    // The line of a syntax error, and the parser's view of the place.
    const ConfigRead brace = read_config(with_participants("  - name: A\n    command: [a, --x={y}]\n"));
    CHECK(!brace.config.has_value());
    check_text(lines(brace.findings), "E 5 syntax: syntax error: invalid character: '{'\n"
                                      "  | 5:22:     command: [a, --x={y}]\n"
                                      "                           ^~~~\n");
    // A version that is not a number is an error, but the file is read.
    const ConfigRead quoted = read_config("vehicle_sil_simulation: \"1\"\nnetworks: []\nparticipants:\n  - name: A\n    command: a\n");
    CHECK(quoted.config.has_value());
    check_text(lines(quoted.findings), "E 1 format-version: vehicle_sil_simulation must be the format version 1, not the "
                                       "text \"1\": write vehicle_sil_simulation: 1\n");
}

TEST_CASE(expands_the_placeholders_in_any_argument)
{
    CHECK((expand_placeholders({"--uri", "{registry_uri}", "--uri={registry_uri}", "{name}-{name}", "{other}", "{name"},
                               "silkit://localhost:8500", "Engine") ==
           std::vector<std::string>{"--uri", "silkit://localhost:8500", "--uri=silkit://localhost:8500", "Engine-Engine",
                                    "{other}", "{name"}));
}

TEST_CASE(suggests_close_names_only)
{
    const std::vector<std::string> keys{"name", "command", "step_ms", "describe", "required"};
    CHECK(launch::detail::closest("comand", keys) == "command");
    CHECK(launch::detail::closest("STEP_MS", keys) == "step_ms");
    CHECK(launch::detail::closest("step", keys) == "step_ms");     // a beginning
    CHECK(launch::detail::closest("nmae", keys) == "name");        // two edits
    CHECK(launch::detail::closest("bitrate", keys).empty());
    CHECK(launch::detail::closest("x", keys).empty());
    CHECK_EQ(launch::detail::edit_distance("kitten", "sitting"), std::size_t{3});
}

TEST_CASE(resolves_the_executable_from_the_directory_of_the_file)
{
    const fs::path base = scratch("resolve");
    CHECK(resolve_executable("engine/engine_ecu", base) == base / "engine" / "engine_ecu");
    // Not normalized: ".." keeps its meaning across a symbolic link.
    CHECK(resolve_executable("./a/../b", base) == base / "." / "a" / ".." / "b");
    const fs::path absolute = base / "abs";
    CHECK(resolve_executable(process::utf8_from_path(absolute), "/elsewhere") == absolute);
#ifdef _WIN32
    std::ofstream(base / "tool.exe") << "x";
    std::ofstream(base / "engine.v2.exe") << "x";
    CHECK(resolve_executable("tool", base) == base / "tool.exe");
    CHECK(resolve_executable("engine.v2", base) == base / "engine.v2.exe");
#endif
}

TEST_CASE(checks_that_the_executable_is_an_executable_file)
{
    const fs::path base = scratch("executable");
    ParticipantConfig p;
    p.name = "Engine";
    p.command = {"engine/engine_ecu"};
    p.command_line = 12;
    const fs::path missing = base / "engine" / "engine_ecu";
    const auto none = check_executable(p, missing);
    CHECK(none.has_value());
    check_text(lines({*none}), "E 12 executable: participant 'Engine': the executable engine/engine_ecu does not exist "
                               "(looked for " +
                                   process::shown_path(missing) +
                                   "): build it, or correct the path (a relative path is from the directory of the "
                                   "configuration file)\n");
    CHECK(none->participant == "Engine");
    fs::create_directories(missing);
    check_text(lines({*check_executable(p, missing)}),
               "E 12 executable: participant 'Engine': the executable engine/engine_ecu is a directory (" +
                   process::shown_path(missing) + "): name the executable file in it\n");
    const fs::path file = base / "file";
    std::ofstream(file) << "#!/bin/sh\n";
#ifndef _WIN32
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write);
    check_text(lines({*check_executable(p, file)}),
               "E 12 executable: participant 'Engine': the executable engine/engine_ecu is not executable (" +
                   process::shown_path(file) + "): make it executable (chmod +x), or name the right file\n");
    fs::permissions(file, fs::perms::owner_exec, fs::perm_options::add);
#endif
    CHECK(!check_executable(p, file).has_value());
}

TEST_CASE(a_missing_executable_gets_a_hint_for_path_and_arguments)
{
    const fs::path base = scratch("hints");
    ParticipantConfig p;
    p.name = "Tool";
    p.command_line = 4;
    p.command = {"python3", "tool.py"};
    const auto bare = check_executable(p, resolve_executable("python3", base));
    CHECK(bare.has_value());
    CHECK(bare->message.ends_with("; it is not on PATH either"));
    p.command = {"tools/engine --fast"};
    const auto spaced = check_executable(p, resolve_executable(p.command.front(), base));
    CHECK(spaced.has_value());
    CHECK(spaced->message.ends_with("; if the text after a space is an argument, write the command as a list, e.g. "
                                    "command: [tools/engine, ...]"));
    p.command = {"tools/engine"};
    CHECK(check_executable(p, resolve_executable(p.command.front(), base))->message.ends_with("configuration file)"));
}

TEST_CASE(a_batch_file_is_not_an_executable)
{
    const fs::path base = scratch("batch");
    std::ofstream(base / "run.CMD") << "@echo off\n";
#ifndef _WIN32
    fs::permissions(base / "run.CMD", fs::perms::owner_all);
#endif
    ParticipantConfig p;
    p.name = "Tool";
    p.command = {"run.CMD"};
    p.command_line = 4;
    const auto found = check_executable(p, resolve_executable("run.CMD", base));
    CHECK(found.has_value());
    check_text(found->message, "participant 'Tool': the executable run.CMD is a batch file (" +
                                   process::shown_path(base / "run.CMD") +
                                   "), which runs through cmd.exe with other quoting rules: name the executable it starts");
}

TEST_CASE(expands_escaped_braces)
{
    // Only "{{word}}" is an escape; any other brace stays, byte for byte.
    CHECK((expand_placeholders({"{{name}}", "x{{name}}{name}", "a}}b{{", "${{HOME}}", "{x}", "{\"a\":{\"b\":1}}",
                                "{{\"vehicle_sil_ecu\": 2}}", "{{{name}}}", "{}"},
                               "uri", "Engine") ==
           std::vector<std::string>{"{name}", "x{name}Engine", "a}}b{{", "${HOME}", "{x}", "{\"a\":{\"b\":1}}",
                                    "{{\"vehicle_sil_ecu\": 2}}", "{{name}}", "{}"}));
}

TEST_CASE(reads_where_the_registry_listens_and_how_long_to_wait_for_the_end)
{
    const ConfigRead read = read_config("vehicle_sil_simulation: 1\nstop_timeout_ms: 2500\nregistry_uri: silkit://0.0.0.0:8500\n"
                                        "networks: []\nparticipants:\n  - name: A\n    command: a\n");
    // Not a loopback address: a note.
    check_text(lines(read.findings),
               "N 3 registry-on-network: registry_uri silkit://0.0.0.0:8500 is not a loopback address: the registry and the "
               "participants listen on the network, where anyone who reaches this computer can join the simulation (SIL Kit "
               "does not authenticate); run sets VEHICLE_SIL_LISTEN_ON_NETWORK=1 for the participants it starts, set it also "
               "for the ones you start yourself on this computer; to keep the simulation on this computer, use 127.0.0.1 or "
               "leave registry_uri out\n");
    CHECK_EQ(read.config->stop_timeout_ms, 2500);
    CHECK(read.config->registry_uri == "silkit://0.0.0.0:8500");
    for (const char* uri : {"silkit://127.0.0.1:8500", "'silkit://[::1]:8500'", "silkit://localhost:8500"})
    {
        check_text(lines(read_config(std::string("vehicle_sil_simulation: 1\nregistry_uri: ") + uri +
                                     "\nnetworks: []\nparticipants:\n  - name: A\n    command: a\n")
                             .findings),
                   "");
    }
    // Without them: 10 s, and no URI (a free port).
    const ConfigRead defaults = read_config(with_participants("  - name: A\n    command: a\n"));
    CHECK_EQ(defaults.config->stop_timeout_ms, 10000);
    CHECK(!defaults.config->registry_uri.has_value());
    CHECK(read_config("vehicle_sil_simulation: 1\nregistry_uri: 'silkit://[::1]:65535'\nnetworks: []\nparticipants:\n"
                      "  - name: A\n    command: a\n")
              .config->registry_uri == "silkit://[::1]:65535");
}

TEST_CASE(a_registry_uri_must_be_a_silkit_uri_with_a_port)
{
    const std::string expected = "registry_uri must be where the registry of the simulation listens, silkit://<host>:<port> "
                                 "with a port from 1 to 65535 (e.g. silkit://127.0.0.1:8500, or silkit://0.0.0.0:8500 for "
                                 "other computers too), not ";
    const std::string tail = "; leave registry_uri out for a free port on 127.0.0.1\n";
    for (const auto& [value, shown] : std::vector<std::pair<std::string, std::string>>{
             {"http://localhost:8500", "the text \"http://localhost:8500\""},
             {"silkit://localhost", "the text \"silkit://localhost\""},
             {"silkit://localhost:0", "the text \"silkit://localhost:0\""},
             {"silkit://localhost:65536", "the text \"silkit://localhost:65536\""},
             {"silkit://localhost:85x0", "the text \"silkit://localhost:85x0\""},
             {"'silkit://local host:8500'", "the text \"silkit://local host:8500\""},
             {"8500", "the text \"8500\""}})
    {
        const std::string got = read("vehicle_sil_simulation: 1\nregistry_uri: " + value +
                                     "\nnetworks: []\nparticipants:\n  - name: A\n    command: a\n");
        check_text(got.substr(0, std::min(got.size(), std::string("E 2 value: ").size())), "E 2 value: ", value.c_str());
        CHECK(got.find(expected) != std::string::npos);
        CHECK(got.ends_with(tail));
    }
    check_text(read("vehicle_sil_simulation: 1\nstop_timeout_ms: 0\nnetworks: []\nparticipants:\n  - name: A\n    command: a\n"),
               "E 2 value: stop_timeout_ms is 0, but it must be from 1 to 1000000000000 (milliseconds to wait for each "
               "participant to end)\n");
}

TEST_CASE(required_false_is_only_for_executables_that_are_not_ecu_main)
{
    check_text(read(with_participants("  - name: A\n    command: a\n    required: false\n"
                                      "  - name: B\n    command: b\n    describe: false\n    required: false\n"
                                      "  - name: C\n    command: c\n    required: true\n")),
               "E 6 not-required-ecu: participant 'A': required: false is only for executables with describe: false: an "
               "ecu_main executable joins as a coordinated participant, which SIL Kit starts only among the required "
               "participants; remove required: false (to run the simulation without this ECU, write enabled: false "
               "instead, or give vehicle_sil_launch --disable A)\n"
               "N 8 no-registry-uri: participant 'B': the command has no {registry_uri}, so the executable is not told "
               "where the simulation's registry is: if it needs it, add its option for the registry URI with "
               "\"{registry_uri}\" as the value\n");
}

TEST_CASE(a_bare_name_that_is_not_next_to_the_file_is_looked_for_on_path)
{
    const fs::path base = scratch("path base");
    const fs::path bin = scratch("path bin");
    const fs::path other = scratch("path other");
    const auto make = [](const fs::path& file) {
        std::ofstream(file) << "#!/bin/sh\n";
        fs::permissions(file, fs::perms::owner_all);
    };
#ifdef _WIN32
    const std::string tool = "tool.exe";
    const char separator = ';';
#else
    const std::string tool = "tool";
    const char separator = ':';
#endif
    make(bin / tool);
    const std::string path = std::string("relative") + separator + other.string() + separator + bin.string();
#ifdef _WIN32
    _putenv_s("PATH", path.c_str());
#else
    ::setenv("PATH", path.c_str(), 1);
#endif
    FoundExecutable found = find_executable("tool", base);
    CHECK(found.on_path);
    CHECK(fs::equivalent(found.path, bin / tool));
    // Next to the file it wins over PATH.
    make(base / tool);
    found = find_executable("tool", base);
    CHECK(!found.on_path);
    CHECK(fs::equivalent(found.path, base / tool));
    // A path with a separator is never looked for on PATH (bin/sub/tool exists).
    fs::create_directories(bin / "sub");
    make(bin / "sub" / tool);
    found = find_executable("sub/tool", base);
    CHECK(!found.on_path);
    CHECK(process::shown_path(found.path) == process::shown_path(base / "sub/tool"));
    // Not anywhere: the path next to the file, for the message.
    found = find_executable("nothing", base);
    CHECK(!found.on_path);
    CHECK(process::shown_path(found.path) == process::shown_path(base / "nothing"));
#ifndef _WIN32
    // A file without the execute permission on PATH is not taken.
    std::ofstream(other / "plain") << "x";
    found = find_executable("plain", base);
    CHECK(!found.on_path);
#endif
}

TEST_CASE(a_simulation_needs_a_required_participant)
{
    check_text(read("vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n"
                    "  - name: A\n    command: a\n    describe: false\n    required: false\n"
                    "  - name: B\n    command: b\n    describe: false\n    required: false\n"),
               "N 5 no-registry-uri: participant 'A': the command has no {registry_uri}, so the executable is not told "
               "where the simulation's registry is: if it needs it, add its option for the registry URI with "
               "\"{registry_uri}\" as the value\n"
               "N 9 no-registry-uri: participant 'B': the command has no {registry_uri}, so the executable is not told "
               "where the simulation's registry is: if it needs it, add its option for the registry URI with "
               "\"{registry_uri}\" as the value\n"
               "E 3 no-required-participant: every participant has required: false, but the simulation starts when its "
               "required participants have joined, so it needs at least one: remove required: false from the "
               "participants it must wait for (an ecu_main executable is always required)\n");
    // One required participant is enough.
    check_text(read("vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n"
                    "  - name: A\n    command: a\n"
                    "  - name: B\n    command: [b, \"{registry_uri}\"]\n    describe: false\n    required: false\n"),
               "");
}

TEST_CASE(a_file_next_to_the_configuration_that_is_not_executable_does_not_hide_the_program_on_path)
{
#ifndef _WIN32
    const fs::path base = scratch("path data base");
    const fs::path bin = scratch("path data bin");
    std::ofstream(base / "tool") << "data, not a program";
    std::ofstream(bin / "tool") << "#!/bin/sh\n";
    fs::permissions(bin / "tool", fs::perms::owner_all);
    ::setenv("PATH", bin.string().c_str(), 1);
    const FoundExecutable found = find_executable("tool", base);
    CHECK(found.on_path);
    CHECK(fs::equivalent(found.path, bin / "tool"));
#else
    // Windows has no execute permission: a file next to the configuration is the program.
    throw vehicle_sil::unit::Skipped("every file is executable on Windows");
#endif
}

// --- enabled, --disable and --enable ----------------------------------------------------------

TEST_CASE(enabled_is_true_by_default_and_must_be_true_or_false)
{
    const ConfigRead r = read_config(with_participants("  - name: A\n    command: a\n"
                                                       "  - name: B\n    command: b\n    enabled: false\n"
                                                       "  - name: C\n    command: c\n    enabled: true\n"
                                                       "  - name: D\n    command: d\n    enabled: \"false\"\n"));
    check_text(lines(r.findings), "E 14 value: participant 'D': enabled must be true or false, not the text \"false\"\n");
    CHECK(r.config.has_value());
    CHECK(r.config->participants[0].enabled);
    CHECK(!r.config->participants[1].enabled);
    CHECK(r.config->participants[2].enabled);
    CHECK(r.config->participants[3].enabled); // not read: as if it were not there...
    CHECK(!r.config->participants[3].valid);  // ...but neither described nor started
    CHECK(r.config->participants[1].valid);   // a disabled entry is still usable
    CHECK(r.override_errors.empty());
}

TEST_CASE(a_disabled_participant_is_checked_like_any_other)
{
    // Its errors show now, not when it is taken in again.
    check_text(read(with_participants("  - name: A\n    command: a\n"
                                      "  - name: B\n    command: [b, --local]\n    enabled: false\n    required: false\n"
                                      "    bitrate: 1\n"
                                      "  - name: A\n    command: a\n    enabled: false\n")),
               "E 10 unknown-key: participant 'B': unknown key 'bitrate': remove it, or use one of the keys here (name, "
               "command, step_ms, describe, required, enabled, manual)\n"
               "E 7 launcher-argument: participant 'B': the command has '--local', but vehicle_sil_launch gives an ecu_main "
               "executable --local itself: remove it (or set describe: false for an executable not made with ecu_main)\n"
               "E 9 not-required-ecu: participant 'B': required: false is only for executables with describe: false: an "
               "ecu_main executable joins as a coordinated participant, which SIL Kit starts only among the required "
               "participants; remove required: false (to run the simulation without this ECU, write enabled: false "
               "instead, or give vehicle_sil_launch --disable B)\n"
               "E 11 duplicate-name: participant 'A' is listed twice (first at line 4); give each participant its own name "
               "(for a second copy of an ECU, e.g. A_2)\n");
}

TEST_CASE(only_the_enabled_participants_count_as_required)
{
    // The one required participant is disabled.
    check_text(read(with_participants("  - name: A\n    command: a\n    enabled: false\n"
                                      "  - name: B\n    command: [b, \"{registry_uri}\"]\n    describe: false\n"
                                      "    required: false\n")),
               "E 3 no-required-participant: every enabled participant has required: false (disabled: A), but the "
               "simulation starts when its required participants have joined, so it needs at least one: enable a required "
               "participant, or remove required: false from the participants it must wait for (an ecu_main executable is "
               "always required)\n");
    // Every participant disabled.
    check_text(read(with_participants("  - name: A\n    command: a\n    enabled: false\n"
                                      "  - name: B\n    command: b\n    enabled: false\n")),
               "E 3 no-required-participant: every participant is disabled (enabled: false, or --disable), so the "
               "simulation has no participant to wait for: enable at least one (enabled: true, or --enable <name>)\n");
    // Only the disabled participants that are required are named: enabling them would help.
    check_text(read(with_participants("  - name: A\n    command: [a, \"{registry_uri}\"]\n    describe: false\n"
                                      "    required: false\n"
                                      "  - name: B\n    command: [b, \"{registry_uri}\"]\n    describe: false\n"
                                      "    required: false\n    enabled: false\n"
                                      "  - name: C\n    command: c\n    enabled: false\n")),
               "E 3 no-required-participant: every enabled participant has required: false (disabled: C), but the "
               "simulation starts when its required participants have joined, so it needs at least one: enable a required "
               "participant, or remove required: false from the participants it must wait for (an ecu_main executable is "
               "always required)\n");
    // None of them: the message without disabled participants (enabling B would not help).
    check_text(read(with_participants("  - name: A\n    command: [a, \"{registry_uri}\"]\n    describe: false\n"
                                      "    required: false\n"
                                      "  - name: B\n    command: [b, \"{registry_uri}\"]\n    describe: false\n"
                                      "    required: false\n    enabled: false\n")),
               "E 3 no-required-participant: every participant has required: false, but the simulation starts when its "
               "required participants have joined, so it needs at least one: remove required: false from the "
               "participants it must wait for (an ecu_main executable is always required)\n");
    // A disabled optional participant beside a required one: fine.
    check_text(read(with_participants("  - name: A\n    command: a\n"
                                      "  - name: B\n    command: [b, \"{registry_uri}\"]\n    describe: false\n"
                                      "    required: false\n    enabled: false\n")),
               "");
}

TEST_CASE(the_command_line_disables_and_enables_participants)
{
    const std::string text = with_participants("  - name: Engine\n    command: e\n"
                                               "  - name: Brake\n    command: b\n    enabled: false\n"
                                               "  - name: Cluster\n    command: c\n");
    const ConfigRead r = read_config(text, EnabledOverrides{{"Engine", "Engine"}, {"Brake"}});
    check_text(lines(r.findings), "");
    CHECK(r.override_errors.empty());
    CHECK(!r.config->participants[0].enabled);
    CHECK(r.config->participants[1].enabled);
    CHECK(r.config->participants[2].enabled);
    // The rule of a required participant counts after the overrides.
    const ConfigRead none = read_config(text, EnabledOverrides{{"Engine", "Cluster"}, {}});
    check_text(lines(none.findings), "E 3 no-required-participant: every participant is disabled (enabled: false, or "
                                     "--disable), so the simulation has no participant to wait for: enable at least one "
                                     "(enabled: true, or --enable <name>)\n");
    const ConfigRead back = read_config(with_participants("  - name: A\n    command: a\n    enabled: false\n"),
                                        EnabledOverrides{{}, {"A"}});
    check_text(lines(back.findings), "");
    CHECK(back.config->participants[0].enabled);
}

TEST_CASE(the_command_line_must_name_participants_of_the_file_once)
{
    const std::string text = with_participants("  - name: Engine\n    command: e\n"
                                               "  - name: Brake\n    command: b\n    enabled: false\n");
    // Every name that does not fit, at once; nothing is applied.
    const ConfigRead typo = read_config(text, EnabledOverrides{{"Engin", "Brake", "Wheel", "Engin"}, {"Brak"}});
    check_text(lines(typo.override_errors),
               "E 0 usage: --disable Engin: the file has no participant with this name (did you mean 'Engine'?); its "
               "participants are Engine, Brake\n"
               "E 0 usage: --disable Wheel: the file has no participant with this name; its participants are Engine, Brake\n"
               "E 0 usage: --enable Brak: the file has no participant with this name (did you mean 'Brake'?); its "
               "participants are Engine, Brake\n");
    CHECK(typo.config->participants[0].enabled && !typo.config->participants[1].enabled);
    // Each name given to both, once.
    const ConfigRead both = read_config(text, EnabledOverrides{{"Engine", "Brake"}, {"Engine", "Brake", "Engine"}});
    check_text(lines(both.override_errors), "E 0 usage: --disable and --enable both name Engine: give one of them\n"
                                            "E 0 usage: --disable and --enable both name Brake: give one of them\n");
    CHECK(both.config->participants[0].enabled && !both.config->participants[1].enabled);
    check_text(lines(override_conflicts(EnabledOverrides{{"A", "B"}, {"B"}})),
               "E 0 usage: --disable and --enable both name B: give one of them\n");
    CHECK(override_conflicts(EnabledOverrides{{"A"}, {"B"}}).empty());
}

TEST_CASE(a_participant_started_by_hand_from_the_file_or_the_command_line)
{
    const std::string text = with_participants("  - name: Engine\n    command: e\n    manual: true\n"
                                               "  - name: Brake\n    command: b\n"
                                               "  - name: Cluster\n    command: c\n    manual: 1\n");
    const ConfigRead r = read_config(text, EnabledOverrides{{}, {}, {"Brake"}});
    check_text(lines(r.findings), "E 11 value: participant 'Cluster': manual must be true or false, not '1'\n");
    CHECK(r.config->participants[0].manual && r.config->participants[1].manual && !r.config->participants[2].manual);
    CHECK(waits_for_manual(*r.config));
    // --manual changes neither enabled nor required; the start waits only for an enabled required one.
    const ConfigRead off = read_config(with_participants("  - name: A\n    command: a\n    manual: true\n    enabled: false\n"
                                                         "  - name: B\n    command: b\n"),
                                       EnabledOverrides{{}, {}, {"B"}});
    CHECK(!off.config->participants[0].enabled && off.config->participants[1].manual);
    CHECK(waits_for_manual(*off.config));
    CHECK(!waits_for_manual(*read_config(with_participants("  - name: A\n    command: a\n    manual: true\n    enabled: false\n"
                                                           "  - name: B\n    command: b\n"))
                                 .config));
    // A name the file does not have, and one given to --disable as well.
    const std::string fine = with_participants("  - name: Engine\n    command: e\n  - name: Brake\n    command: b\n");
    check_text(lines(read_config(fine, EnabledOverrides{{}, {}, {"Brak"}}).override_errors),
               "E 0 usage: --manual Brak: the file has no participant with this name (did you mean 'Brake'?); its "
               "participants are Engine, Brake\n");
    check_text(lines(override_conflicts(EnabledOverrides{{"Brake"}, {}, {"Brake"}})),
               "E 0 usage: --disable and --manual both name Brake: give one of them (--disable leaves it out, --manual "
               "has you start it)\n");
}

TEST_CASE(an_error_in_the_file_comes_before_the_names_of_the_command_line)
{
    // The name Engine cannot be read: the file's error says so, not "no participant Engine".
    const std::string text = with_participants("  - name: [Engine]\n    command: e\n"
                                               "  - name: Brake\n    command: b\n");
    const ConfigRead r = read_config(text, EnabledOverrides{{"Engine", "Brake"}, {}});
    CHECK(r.override_errors.empty());
    CHECK(!r.findings.empty() && r.findings.front().code == "value");
    CHECK(!r.config->participants[1].enabled); // the names that the file has are applied
    // A name given to both is an error all the same (it needs no file).
    check_text(lines(read_config(text, EnabledOverrides{{"Brake"}, {"Brake"}}).override_errors),
               "E 0 usage: --disable and --enable both name Brake: give one of them\n");
}
