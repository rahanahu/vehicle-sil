// SPDX-License-Identifier: MIT
//
// Reading the simulation configuration (vehicle_sil::config::read_config()): a correct file in
// YAML and in JSON, every rule of the text with its message and line, many errors at once, the
// files that cannot be read; the placeholders; "did you mean". The executables are in
// test_config_executables.cpp.
#include "launch_test_util.hpp"

#include "vehicle_sil/config/config.hpp"
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
using namespace vehicle_sil::config;
using vehicle_sil::config::test::check_text;
using vehicle_sil::config::test::lines;

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
               "late_tolerance_ms, networks, participants, variants, params)\n"
               "E 19 unknown-key: unknown key 'extra': remove it, or use one of the keys here (vehicle_sil_simulation, "
               "stop_at_ms, start_timeout_ms, stop_timeout_ms, registry_uri, time_step_ms, speed, late_tolerance_ms, "
               "networks, participants, variants, params)\n");
    check_text(read(with_participants("  - name: A\n    command: a\n    requird: false\n    Step_ms: 5\n")),
               "E 6 unknown-key: participant 'A': unknown key 'requird': did you mean 'required'? (the keys here: name, "
               "command, step_ms, describe, required, enabled, manual, role, variables, network_names, j1939_nodes)\n"
               "E 7 unknown-key: participant 'A': unknown key 'Step_ms': did you mean 'step_ms'? (the keys here: name, "
               "command, step_ms, describe, required, enabled, manual, role, variables, network_names, j1939_nodes)\n");
    check_text(read("vehicle_sil_simulation: 1\nnetworks:\n  - name: N\n    kind: can\n    bitrate: 250000\n"
                    "participants:\n  - name: A\n    command: a\n"),
               "E 5 unknown-key: network 'N': unknown key 'bitrate': move it into external (external: { bitrate: ... }), "
               "which says how the network behaves when it is connected outside (the keys here: name, kind, external)\n");
    check_text(read("vehicle_sil_simulation: 1\nnetworks:\n  - name: N\n    kind: can\n    connect: eth0\n    nam: x\n"
                    "participants:\n  - name: A\n    command: a\n"),
               "E 5 unknown-key: network 'N': unknown key 'connect': where a network is connected is not written in the "
               "file, since it differs from PC to PC: remove connect, and give it when you start: vehicle_sil_launch check|run "
               "--connect N=<target> <file> (the keys here: name, kind, external)\n"
               "E 6 unknown-key: network 'N': unknown key 'nam': did you mean 'name'? (the keys here: name, kind, "
               "external)\n");
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
               "E 0 missing-key: the file has no networks: add networks, a list of the networks with name and kind (can, "
               "lin or ethernet)\n"
               "E 0 missing-key: the file has no participants: add participants, a list of the participants with name "
               "and command\n");
    check_text(read("vehicle_sil_simulation: 1\nnetworks:\n  - kind: can\n  - name: N\nparticipants:\n  - command: [a]\n  - "
                    "name: B\n"),
               "E 3 missing-key: networks[0] has no name: add name: <the network's name>\n"
               "E 4 missing-key: network 'N': no kind: add kind: can, kind: lin or kind: ethernet\n"
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
               "E 4 value: networks must be a list of networks (\"- name: <name>\" and \"kind: can\", \"kind: lin\" or \"kind: ethernet\"), "
               "not a mapping\n"
               "E 5 value: participants must be a list of participants (\"- name: <name>\" and \"command: "
               "[<executable>]\"), not 'x'\n");
    check_text(read(with_participants("  - name: A\n    command: a\n    step_ms: -5\n    describe: yes\n    required:\n")),
               "E 6 value: participant 'A': step_ms is -5, but it must be from 1 to 1000000000000 (the step the "
               "executable reports)\n"
               "E 7 value: participant 'A': describe must be true or false, not 'yes'\n"
               "E 8 value: participant 'A': required must be true or false, not empty\n");
    check_text(read("vehicle_sil_simulation: 1\nnetworks:\n  - name: A\n    kind: CAN\n  - name: B\n    kind: flexray\n  - "
                    "name: [x]\n    kind: can\n  - just text\nparticipants: []\n"),
               "E 4 value: network 'A': kind is 'CAN': write can (lower case)\n"
               "E 6 value: network 'B': kind is 'flexray': write can, lin or ethernet (the kinds of network there are)\n"
               "E 7 value: networks[2]: name must be the network's name, not a list\n"
               "E 9 value: networks[3] must be a network with name and kind (can, lin or ethernet), not 'just text'\n"
               "E 10 value: participants is empty: list the participants of the simulation\n");
}

TEST_CASE(every_kind_of_network_is_read_by_its_name)
{
    // kNetworkKinds: also a kind added later is read without a change here.
    for (const NetworkKind kind : kNetworkKinds)
    {
        const ConfigRead read = read_config(std::string("vehicle_sil_simulation: 1\nnetworks:\n  - name: N\n    kind: ") +
                                            to_string(kind) + "\nparticipants:\n  - name: A\n    command: a\n");
        CHECK(read.findings.empty());
        CHECK(read.config && read.config->networks.size() == 1 && read.config->networks[0].valid &&
              read.config->networks[0].kind == kind);
    }
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

/// A file with `networks` as its networks list and one participant.
std::string with_network_list(const std::string& networks)
{
    return "vehicle_sil_simulation: 1\nnetworks:\n" + networks + "participants:\n  - name: P\n    command: p\n";
}

TEST_CASE(external_of_a_network_is_read_for_its_kind_also_before_the_kind)
{
    const ConfigRead read = read_config(with_network_list(
        "  - name: CAN1\n    external: { bitrate: 250000 }\n    kind: can\n"
        "  - name: ETH1\n    kind: ethernet\n    external:\n      ethertypes: [0x88b5, 2048, \"0x86DD\", \"0x1\", 0o10]\n"
        "  - name: ETH2\n    external: { all_frames: true }\n    kind: ethernet\n"
        "  - name: ETH3\n    kind: ethernet\n    external: { all_frames: false }\n"
        "  - name: CAN2\n    kind: can\n    external: {}\n"
        "  - name: CAN3\n    kind: can\n"));
    CHECK(read.findings.empty());
    if (!(read.findings.empty()))
    {
        return;
    }
    const auto& networks = read.config->networks;
    CHECK(networks.size() == std::size_t{6});
    if (!(networks.size() == std::size_t{6}))
    {
        return;
    }
    CHECK(networks[0].external);
    if (!(networks[0].external))
    {
        return;
    }
    CHECK(networks[0].external->bitrate == std::optional<std::uint32_t>(250000));
    CHECK_EQ(networks[0].external->line, std::size_t{4});
    CHECK(networks[1].external);
    if (!(networks[1].external))
    {
        return;
    }
    const std::vector<std::uint16_t> ethertypes{0x88b5, 0x0800, 0x86dd, 0x0001, 8};
    CHECK(networks[1].external->ethertypes == ethertypes);
    CHECK(!networks[1].external->all_frames && !networks[1].external->bitrate);
    CHECK(networks[2].external && networks[2].external->all_frames && networks[2].external->ethertypes.empty());
    // all_frames: false is the same as no all_frames; an empty external is the same as none but for its line.
    CHECK(networks[3].external && *networks[3].external == NetworkExternal{.line = 15});
    CHECK(networks[4].external && *networks[4].external == NetworkExternal{.line = 18});
    CHECK(!networks[5].external);

    // In JSON, an EtherType in hexadecimal is a text.
    const ConfigRead json = read_config(R"({"vehicle_sil_simulation": 1, "networks": [
  {"name": "ETH1", "kind": "ethernet", "external": {"ethertypes": ["0x88b5", 34997]}}],
  "participants": [{"name": "P", "command": "p"}]})");
    CHECK(lines(json.findings) == "E 2 value: network 'ETH1': external.ethertypes lists 0x88b5 twice: keep one\n");
}

TEST_CASE(external_of_the_wrong_form_is_an_error_and_says_what_to_write)
{
    check_text(read(with_network_list("  - name: BODY_LIN\n    kind: lin\n    external: { bitrate: 19200 }\n"
                                  "  - name: CAN1\n    kind: can\n    external: 500000\n"
                                  "  - name: ETH1\n    kind: ethernet\n    external:\n")),
               "E 5 value: network 'BODY_LIN': external is for a network that can be connected outside (can, ethernet); "
               "there is no bridge for a lin network, so it runs on SIL Kit only: remove external\n"
               "E 8 value: network 'CAN1': external must be a mapping (the keys of a can network: bitrate), e.g. "
               "external: { bitrate: 500000 }, not '500000'\n"
               "E 11 value: network 'ETH1': external must be a mapping (the keys of an ethernet network: ethertypes, "
               "all_frames), e.g. external: { ethertypes: [0x88b5] }, not empty\n");
    // Keys of the other kind, where the network is connected, a key that is not there, a key twice.
    check_text(read(with_network_list("  - name: CAN1\n    kind: can\n    external: { ethertypes: [0x88b5], bitrat: 1 }\n"
                                  "  - name: ETH1\n    kind: ethernet\n    external: { bitrate: 1, connect: eth0 }\n"
                                  "  - name: ETH2\n    kind: ethernet\n    external: { all_frames: true, all_frames: true }\n")),
               "E 5 unknown-key: network 'CAN1': external: unknown key 'ethertypes': it is for an ethernet network, and "
               "this one is can (the keys here: bitrate)\n"
               "E 5 unknown-key: network 'CAN1': external: unknown key 'bitrat': did you mean 'bitrate'? (the keys here: "
               "bitrate)\n"
               "E 8 unknown-key: network 'ETH1': external: unknown key 'bitrate': it is for a can network, and this one "
               "is ethernet (the keys here: ethertypes, all_frames)\n"
               "E 8 unknown-key: network 'ETH1': external.connect: where a network is connected is not written in the "
               "file, since it differs from PC to PC: remove connect, and give it when you start: vehicle_sil_launch check|run "
               "--connect ETH1=<target> <file> (the keys here: ethertypes, all_frames)\n"
               "E 11 duplicate-key: network 'ETH2': external: 'all_frames' appears twice (first at line 11); keep one of "
               "them\n");
    // A network whose kind cannot be read: its external is not read.
    const ConfigRead noKind = read_config(with_network_list("  - name: X\n    kind: flexray\n    external: { zz: 1 }\n"));
    CHECK(lines(noKind.findings) ==
          "E 4 value: network 'X': kind is 'flexray': write can, lin or ethernet (the kinds of network there are)\n");
}

TEST_CASE(the_values_of_external_are_checked)
{
    check_text(read(with_network_list("  - name: C0\n    kind: can\n    external: { bitrate: 0 }\n"
                                  "  - name: C1\n    kind: can\n    external: { bitrate: 1000001 }\n"
                                  "  - name: C2\n    kind: can\n    external: { bitrate: \"500000\" }\n")),
               "E 5 value: network 'C0': external.bitrate must be the bit rate in bit/s, a whole number from 1 to "
               "1000000 (e.g. 500000), not '0'\n"
               "E 8 value: network 'C1': external.bitrate must be the bit rate in bit/s, a whole number from 1 to "
               "1000000 (e.g. 500000), not '1000001'\n"
               "E 11 value: network 'C2': external.bitrate must be the bit rate in bit/s, a whole number from 1 to "
               "1000000 (e.g. 500000), not the text \"500000\"\n");
    const ConfigRead ends = read_config(with_network_list("  - name: C\n    kind: can\n    external: { bitrate: 1 }\n"
                                                      "  - name: D\n    kind: can\n    external: { bitrate: 1000000 }\n"
                                                      "  - name: E\n    kind: ethernet\n    external: { ethertypes: [0, 0xFFFF] }\n"));
    CHECK(ends.findings.empty());
    check_text(read(with_network_list("  - name: E0\n    kind: ethernet\n    external: { ethertypes: [] }\n"
                                  "  - name: E1\n    kind: ethernet\n    external: { ethertypes: 0x88b5 }\n"
                                  "  - name: E2\n    kind: ethernet\n    external:\n      ethertypes: [0x10000, \"0x10000\", "
                                  "\"34997\", -1, \"0x\", 0X88b5, 0x88b5, 0x88B5]\n"
                                  "  - name: E3\n    kind: ethernet\n    external: { ethertypes: [0x0800], all_frames: true }\n"
                                  "  - name: E4\n    kind: ethernet\n    external: { all_frames: 1 }\n")),
               "E 5 value: network 'E0': external.ethertypes is empty: list the EtherTypes to pass, e.g. ethertypes: "
               "[0x88b5], or remove ethertypes\n"
               "E 8 value: network 'E1': external.ethertypes must be a list of EtherTypes (0x88b5, 34997, or \"0x88b5\" in "
               "quotes), e.g. ethertypes: [0x88b5, 0x0800], not '0x88b5'\n"
               "E 12 value: network 'E2': external.ethertypes[0] must be an EtherType from 0 to 0xFFFF (0x88b5, 34997, or "
               "\"0x88b5\" in quotes), not '0x10000'\n"
               "E 12 value: network 'E2': external.ethertypes[1] must be an EtherType from 0 to 0xFFFF (0x88b5, 34997, or "
               "\"0x88b5\" in quotes), not the text \"0x10000\"\n"
               "E 12 value: network 'E2': external.ethertypes[2] must be an EtherType from 0 to 0xFFFF (0x88b5, 34997, or "
               "\"0x88b5\" in quotes), not the text \"34997\"\n"
               "E 12 value: network 'E2': external.ethertypes[3] must be an EtherType from 0 to 0xFFFF (0x88b5, 34997, or "
               "\"0x88b5\" in quotes), not '-1'\n"
               "E 12 value: network 'E2': external.ethertypes[4] must be an EtherType from 0 to 0xFFFF (0x88b5, 34997, or "
               "\"0x88b5\" in quotes), not the text \"0x\"\n"
               "E 12 value: network 'E2': external.ethertypes[5] must be an EtherType from 0 to 0xFFFF (0x88b5, 34997, or "
               "\"0x88b5\" in quotes), not '0X88b5'\n"
               "E 12 value: network 'E2': external.ethertypes lists 0x88b5 twice: keep one\n"
               "E 15 value: network 'E3': external.all_frames: true passes every frame, so it cannot go with ethertypes: "
               "keep one or the other\n"
               "E 18 value: network 'E4': external.all_frames must be true or false, not '1'\n");
    // A network whose external is wrong has none.
    const ConfigRead wrong = read_config(with_network_list("  - name: C\n    kind: can\n    external: { bitrate: 0 }\n"));
    CHECK(wrong.config && wrong.config->networks.size() == 1 && !wrong.config->networks[0].external);
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
               "command, step_ms, describe, required, enabled, manual, role, variables, network_names, j1939_nodes)\n"
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
    CHECK(vehicle_sil::config::detail::closest("comand", keys) == "command");
    CHECK(vehicle_sil::config::detail::closest("STEP_MS", keys) == "step_ms");
    CHECK(vehicle_sil::config::detail::closest("step", keys) == "step_ms");     // a beginning
    CHECK(vehicle_sil::config::detail::closest("nmae", keys) == "name");        // two edits
    CHECK(vehicle_sil::config::detail::closest("bitrate", keys).empty());
    CHECK(vehicle_sil::config::detail::closest("x", keys).empty());
    CHECK_EQ(vehicle_sil::config::detail::edit_distance("kitten", "sitting"), std::size_t{3});
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
               "does not authenticate); run gives the participants it starts VEHICLE_SIL_LISTEN_ON_NETWORK=1 and shows it with "
               "the command of a participant you start yourself: set it for every participant on this computer that connects "
               "to the registry at a loopback address; to keep the simulation on this computer, use 127.0.0.1 or leave "
               "registry_uri out\n");
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

TEST_CASE(run_decides_the_listen_variable_of_its_participants_and_notes_one_it_inherited)
{
    const auto config_with = [](const std::string& registry) {
        return *read_config("vehicle_sil_simulation: 1\n" + registry + "networks: []\nparticipants:\n  - name: A\n    command: a\n")
                    .config;
    };
    const SimulationConfig unwritten = config_with("");
    const SimulationConfig local = config_with("registry_uri: silkit://127.0.0.1:8500\n");
    const SimulationConfig open = config_with("registry_uri: silkit://0.0.0.0:8500\n");
    CHECK(!registry_on_network(unwritten) && !registry_on_network(local) && registry_on_network(open));
    CHECK(listen_variable_value(unwritten) == "0" && listen_variable_value(local) == "0" && listen_variable_value(open) == "1");
    // Not set, or the value run gives anyway: nothing to say.
    CHECK(!inherited_listen_variable(local, nullptr) && !inherited_listen_variable(local, "0"));
    CHECK(!inherited_listen_variable(open, nullptr) && !inherited_listen_variable(open, "1"));
    check_text(lines({*inherited_listen_variable(local, "1")}),
               "N 0 listen-variable-ignored: VEHICLE_SIL_LISTEN_ON_NETWORK=1 in the environment of vehicle_sil_launch is not "
               "used: the registry is on a loopback address, so run gives the participants it starts "
               "VEHICLE_SIL_LISTEN_ON_NETWORK=0 and they listen on the loopback address only; to let other computers join, "
               "write registry_uri with an address that they reach (e.g. silkit://0.0.0.0:8500)\n");
    check_text(lines({*inherited_listen_variable(open, "yes")}),
               "N 0 listen-variable-ignored: VEHICLE_SIL_LISTEN_ON_NETWORK=yes in the environment of vehicle_sil_launch is not "
               "used: registry_uri silkit://0.0.0.0:8500 is not a loopback address, so run gives the participants it starts "
               "VEHICLE_SIL_LISTEN_ON_NETWORK=1 and they listen on every address\n");
}

TEST_CASE(a_registry_uri_must_be_a_silkit_uri_with_a_port)
{
    const std::string expected = "registry_uri must be where the registry of the simulation listens, silkit://<host>:<port> "
                                 "with a port from 0 to 65535 (e.g. silkit://127.0.0.1:8500, or silkit://0.0.0.0:8500 for "
                                 "other computers too; port 0: a free port, chosen when the run starts), not ";
    const std::string tail = "; leave registry_uri out for a free port on 127.0.0.1\n";
    for (const auto& [value, shown] : std::vector<std::pair<std::string, std::string>>{
             {"http://localhost:8500", "the text \"http://localhost:8500\""},
             {"silkit://localhost", "the text \"silkit://localhost\""},
             {"silkit://localhost:-1", "the text \"silkit://localhost:-1\""},
             {"silkit://localhost:-0", "the text \"silkit://localhost:-0\""},
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
    // Port 0: the registry takes a free port when the run starts.
    for (const std::string uri : {"silkit://127.0.0.1:0", "silkit://localhost:65535"})
    {
        check_text(read("vehicle_sil_simulation: 1\nregistry_uri: " + uri + "\nnetworks: []\nparticipants:\n  - name: A\n    command: a\n"),
                   "", uri.c_str());
    }
    check_text(read("vehicle_sil_simulation: 1\nstop_timeout_ms: 0\nnetworks: []\nparticipants:\n  - name: A\n    command: a\n"),
               "E 2 value: stop_timeout_ms is 0, but it must be from 1 to 1000000000000 (milliseconds to wait for each "
               "participant to end)\n");
}

TEST_CASE(required_false_is_only_for_executables_that_are_not_ecu_main)
{
    check_text(read(with_participants("  - name: A\n    command: a\n    required: false\n"
                                      "  - name: B\n    command: b\n    describe: false\n    required: false\n"
                                      "  - name: C\n    command: c\n    required: true\n"
                                      "  - name: D\n    command: d\n    role: bridge\n    required: false\n")),
               "E 6 not-required-ecu: participant 'A': required: false is only for executables with describe: false and "
               "for bridges (role: bridge), which join on their own: an ecu_main executable, also one marked role: "
               "bridge, joins as a coordinated participant, which SIL Kit starts only among the required participants; remove required: false (to run the simulation without this ECU, write "
               "enabled: false instead, or give vehicle_sil_launch --disable A)\n"
               "N 8 no-registry-uri: participant 'B': the command has no {registry_uri}, so the executable is not told "
               "where the simulation's registry is: if it needs it, add its option for the registry URI with "
               "\"{registry_uri}\" as the value\n");
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
               "command, step_ms, describe, required, enabled, manual, role, variables, network_names, j1939_nodes)\n"
               "E 7 launcher-argument: participant 'B': the command has '--local', but vehicle_sil_launch gives an ecu_main "
               "executable --local itself: remove it (or set describe: false for an executable not made with ecu_main)\n"
               "E 9 not-required-ecu: participant 'B': required: false is only for executables with describe: false and "
               "for bridges (role: bridge), which join on their own: an ecu_main executable, also one marked role: "
               "bridge, joins as a coordinated participant, which SIL Kit starts only among the required participants; remove required: false (to run the simulation without this ECU, write "
               "enabled: false instead, or give vehicle_sil_launch --disable B)\n"
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

TEST_CASE(the_command_line_cannot_name_the_bridge_of_a_connected_network)
{
    const std::string text = "vehicle_sil_simulation: 1\nnetworks:\n  - { name: ETH, kind: ethernet }\nparticipants:\n"
                             "  - name: Engine\n    command: e\n";
    EnabledOverrides overrides;
    overrides.disable = {"bridge.ETH"};
    overrides.enable = {"bridge.ETH"};
    overrides.manual = {"bridge.ETH", "bridge.CAN"};
    const ConfigRead read = read_config(text, overrides);
    check_text(lines(read.override_errors),
               "E 0 usage: --disable and --enable both name bridge.ETH: give one of them\n"
               "E 0 usage: --disable bridge.ETH: bridge.ETH is the bridge that --connect starts for the network ETH, not "
               "a participant of the file: to run ETH on SIL Kit only, give --connect ETH=none or leave --connect out\n"
               "E 0 usage: --enable bridge.ETH: bridge.ETH is the bridge that --connect starts for the network ETH, not a "
               "participant of the file: to run ETH on SIL Kit only, give --connect ETH=none or leave --connect out\n"
               "E 0 usage: --manual bridge.ETH: bridge.ETH is the bridge that --connect starts for the network ETH, not a "
               "participant of the file: to run ETH on SIL Kit only, give --connect ETH=none or leave --connect out\n"
               "E 0 usage: --manual bridge.CAN: the file has no participant with this name; its participants are Engine\n");
}

TEST_CASE(a_request_file_that_connects_the_network_names_the_file_for_its_bridge)
{
    const std::string text = "vehicle_sil_simulation: 1\nnetworks:\n  - { name: ETH, kind: ethernet }\nparticipants:\n"
                             "  - name: Engine\n    command: e\n";
    EnabledOverrides overrides;
    overrides.disable = {"bridge.ETH"};
    overrides.origin.file = "bench.json";
    overrides.origin.connected = {"ETH"};
    check_text(lines(read_config(text, overrides).override_errors),
               "E 0 usage: --disable bridge.ETH: bridge.ETH is the bridge that connect in bench.json starts for the network "
               "ETH, not a participant of the file: to run ETH on SIL Kit only, take ETH out of connect in bench.json\n");
}

TEST_CASE(a_participant_has_a_role_that_only_the_check_reads)
{
    const std::string text = with_participants("  - name: Engine\n    command: e\n"
                                               "  - name: Cluster\n    command: c\n    role: ecu\n"
                                               "  - name: Spy\n    command: s\n    role: test\n"
                                               "  - name: Gateway\n    command: [g, \"{registry_uri}\"]\n    describe: false\n"
                                               "    role: \"bridge\"\n");
    const ConfigRead r = read_config(text);
    check_text(lines(r.findings), "");
    const auto& p = r.config->participants;
    CHECK(p[0].role == ParticipantRole::Ecu && !p[0].role_written);
    CHECK(p[1].role == ParticipantRole::Ecu && p[1].role_written);
    CHECK(p[2].role == ParticipantRole::Test && p[2].role_written);
    CHECK(p[3].role == ParticipantRole::Bridge && p[3].role_written);
    CHECK(std::string(to_string(ParticipantRole::Test)) == "test");
    CHECK(participant_role_named("bridge") == ParticipantRole::Bridge && !participant_role_named("Bridge"));

    // A role that is none of them is an error that says which there are; in capitals, the
    // lower case. The participant stays usable (an ECU).
    const ConfigRead bad = read_config(with_participants("  - name: Spy\n    command: s\n    role: spy\n"
                                                         "  - name: Tool\n    command: t\n    role: Test\n"
                                                         "  - name: Logger\n    command: l\n    role: [test]\n"));
    check_text(lines(bad.findings),
               "E 6 value: participant 'Spy': role is 'spy': write ecu, test or bridge (ecu: an ECU of the vehicle, the "
               "default; test: a participant for testing, such as a spy or a tool that talks to the ECUs, not counted as "
               "an ECU; bridge: it carries networks to real buses)\n"
               "E 9 value: participant 'Tool': role is 'Test': write test (lower case)\n"
               "E 12 value: participant 'Logger': role is a list: write ecu, test or bridge (ecu: an ECU of the vehicle, "
               "the default; test: a participant for testing, such as a spy or a tool that talks to the ECUs, not counted "
               "as an ECU; bridge: it carries networks to real buses)\n");
    CHECK(bad.config->participants[0].valid && bad.config->participants[0].role == ParticipantRole::Ecu &&
          !bad.config->participants[0].role_written);

    // A variant cannot change it (like describe and required).
    EnabledOverrides chosen;
    chosen.variant = "a";
    check_text(lines(read_config(with_participants("  - name: Spy\n    command: s\n") +
                                     "variants:\n  a:\n    participants:\n      Spy: { role: test }\n",
                                 chosen)
                         .findings),
               "E 9 unknown-key: variant 'a': participant 'Spy': unknown key 'role': remove it, or use one of the keys here "
               "(command, variables, network_names, j1939_nodes, enabled)\n");
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

// --- a participant's variable map -------------------------------------------------------------

TEST_CASE(a_participants_variables_are_mappings_in_one_of_four_forms)
{
    const ConfigRead r = read_config(with_participants("  - name: Engine\n"
                                                       "    command: e\n"
                                                       "    variables:\n"
                                                       "      engine.throttle: throttle_pct\n"
                                                       "      engine.speed: { from: rpm_x10, scale: 0.1, unit: rpm }\n"
                                                       "      coolant.temperature: { from: temp_k, offset: -273.15 }\n"
                                                       "      engine.gear:\n"
                                                       "        from: gear_raw\n"
                                                       "        map: { P: 0, R: 1, N: -2 }\n"
                                                       "      engine.turbo: { absent: true }\n"
                                                       "      engine.lamp: { from: \"lamp 1\", unit: \"\" }\n"
                                                       "  - name: Plain\n"
                                                       "    command: p\n"));
    check_text(lines(r.findings), "");
    const ParticipantConfig& engine = r.config->participants[0];
    CHECK(engine.valid);
    CHECK_EQ(engine.variables.size(), std::size_t{6});
    CHECK((engine.variable_lines == std::vector<std::size_t>{7, 8, 9, 10, 13, 14}));
    control::VariableMapping throttle;
    throttle.name = "engine.throttle";
    throttle.from = "throttle_pct";
    CHECK(engine.variables[0] == throttle);
    control::VariableMapping speed;
    speed.name = "engine.speed";
    speed.from = "rpm_x10";
    speed.scale = 0.1;
    speed.unit = "rpm";
    CHECK(engine.variables[1] == speed);
    CHECK(!engine.variables[2].scale && engine.variables[2].offset == -273.15 && engine.variables[2].linear());
    CHECK((engine.variables[3].map == std::vector<control::EnumItem>{{"P", 0}, {"R", 1}, {"N", -2}}));
    CHECK(engine.variables[3].from == "gear_raw" && !engine.variables[3].linear());
    control::VariableMapping turbo;
    turbo.name = "engine.turbo";
    turbo.absent = true;
    CHECK(engine.variables[4] == turbo);
    CHECK(engine.variables[5].from == "lamp 1" && engine.variables[5].unit == std::string());
    CHECK(r.config->participants[1].variables.empty());
}

TEST_CASE(a_mapping_that_is_not_one_of_the_forms_is_an_error_and_is_left_out)
{
    const std::string forms =
        "the executable's name of the variable (engine.speed: rpm), { from: rpm, scale: 0.1, offset: 0 } (the value seen "
        "is the executable's value * scale + offset), { from: gear_raw, map: { P: 0, R: 1 } } (names for its values) or "
        "{ absent: true } (this executable does not have it)";
    const ConfigRead r = read_config(with_participants("  - name: A\n"
                                                       "    command: a\n"
                                                       "    variables:\n"
                                                       "      ok: fine\n"
                                                       "      a: [x]\n"
                                                       "      b:\n"
                                                       "      c: { from: x, scale: 0 }\n"
                                                       "      d: { from: x, scale: fast, offset: \"1\" }\n"
                                                       "      e: { from: x, factor: 2 }\n"
                                                       "      f: { scale: 2 }\n"
                                                       "      g: { from: x, scale: 2, map: { on: 1 } }\n"
                                                       "      h: { absent: true, from: x }\n"
                                                       "      i: { absent: false }\n"
                                                       "      j: { from: x, map: { on: 1, yes: 1, half: 0.5 } }\n"
                                                       "      k: { from: x, map: [1, 2] }\n"
                                                       "      l: { from: [x] }\n"
                                                       "      ok: again\n"
                                                       "      m: { from: x, from: y }\n"
                                                       "      n: { from: x, unit: [rpm] }\n"
                                                       "      o: { absent: maybe }\n"));
    const std::string p = "participant 'A': variables: ";
    check_text(lines(r.findings),
               "E 8 value: " + p + "a: must be " + forms + ", not a list\n"
               "E 9 value: " + p + "b: must be " + forms + ", not empty\n"
               "E 10 value: " + p + "c: scale must be a number other than 0 (the value seen is the executable's value * "
               "scale + offset), not '0'\n"
               "E 11 value: " + p + "d: scale must be a number other than 0 (the value seen is the executable's value * "
               "scale + offset), not 'fast'\n"
               "E 11 value: " + p + "d: offset must be a number (the value seen is the executable's value * scale + "
               "offset), not the text \"1\"\n"
               "E 12 unknown-key: " + p + "e: unknown key 'factor': remove it, or use one of the keys here (from, scale, "
               "offset, map, unit, absent)\n"
               "E 13 value: " + p + "f: it does not name the executable's variable: add from: <the executable's name of it>\n"
               "E 14 value: " + p + "g: scale and offset cannot be combined with map: use one of the two (scale and offset "
               "convert a number, map names the values of an integer or an enum)\n"
               "E 15 value: " + p + "h: absent: true says that this executable does not have the variable, so it takes no "
               "other key: remove the others, or absent\n"
               "E 16 value: " + p + "i: absent: false says nothing: remove it (a variable that is not listed is seen under "
               "its own name)\n"
               "E 17 value: " + p + "j: map: on and yes are both 1: give each value one name\n"
               "E 17 value: " + p + "j: map: half must be the executable's value for that name, a whole number, not '0.5'\n"
               "E 18 value: " + p + "k: map must give the names of the executable's values ({ P: 0, R: 1 }), not a list\n"
               "E 19 value: " + p + "l: from must be the executable's name of the variable, not a list\n"
               "E 20 duplicate-key: " + p + "'ok' appears twice (first at line 7); keep one of them\n"
               "E 21 duplicate-key: " + p + "m: 'from' appears twice (first at line 21); keep one of them\n"
               "E 22 value: " + p + "n: unit must be the unit of the value seen (a text such as rpm), not a list\n"
               "E 23 value: " + p + "o: absent must be true or false, not 'maybe'\n");
    // Only the mapping that is a form is kept; the entry is checked all the same.
    CHECK_EQ(r.config->participants[0].variables.size(), std::size_t{1});
    CHECK(r.config->participants[0].variables[0].name == "ok" && r.config->participants[0].variables[0].from == "fine");
    CHECK(r.config->participants[0].valid);

    check_text(read(with_participants("  - name: A\n    command: a\n    variables: [x]\n")),
               "E 6 value: participant 'A': variables must be a mapping of the names the simulation uses to the "
               "executable's variables (engine.speed: rpm), not a list\n");
    // Without --describe there is nothing to compare the map with before the run.
    check_text(read(with_participants("  - name: A\n    command: [a, --registry-uri, \"{registry_uri}\"]\n    describe: false\n"
                                      "    variables:\n      speed: rpm\n")),
               "N 8 variable-map-not-checked: participant 'A': variables is not compared for a participant with describe: "
               "false (the launcher does not know its variables before the run): the mappings apply when its control "
               "agent connects, and vehicle_sil_launch run reports those that do not fit\n");
}

namespace {

/// A file with two variants over a common part.
constexpr const char* kVariants = R"(vehicle_sil_simulation: 1
networks: []
participants:
  - name: Engine
    command: [./engine_small, --mode, a]
    variables:
      engine.speed: { from: rpm_x10, scale: 0.1 }
      engine.turbo: { absent: true }
  - name: Dashboard
    command: ./dashboard
  - name: Logger
    command: [./logger, "{registry_uri}"]
    describe: false
    required: false
    enabled: false
params: { idle_rpm: 800, gear: "D" }
variants:
  small: {}
  medium:
    participants:
      Engine:
        command: [./engine_medium]
        variables: { engine.turbo: turbo_bar, engine.load: load_pct }
      Dashboard: { enabled: false }
      Logger: { enabled: true }
    params: { idle_rpm: 650, boost: 1.5 }
  plain:
)";

EnabledOverrides variant(const std::string& name)
{
    EnabledOverrides out;
    out.variant = name;
    return out;
}

std::string params_text(const std::vector<ParamValue>& params)
{
    std::string out;
    for (const auto& p : params)
    {
        out += p.name + "=" + (p.quoted ? "\"" + p.text + "\"" : p.text) + " " + param_source_name(p.source) + " " +
               std::to_string(p.line) + "\n";
    }
    return out;
}

} // namespace

TEST_CASE(a_variant_is_laid_over_the_common_part)
{
    // The common part as it is.
    const ConfigRead small = read_config(kVariants, variant("small"));
    check_text(lines(small.findings), "");
    CHECK(small.override_errors.empty());
    CHECK(small.config->variant == "small");
    CHECK_EQ(small.config->variants.size(), std::size_t{3});
    CHECK(small.config->participants[0].command == std::vector<std::string>({"./engine_small", "--mode", "a"}));
    CHECK(small.config->participants[1].enabled && !small.config->participants[2].enabled);
    check_text(params_text(small.config->params), "idle_rpm=800 configuration 16\ngear=\"D\" configuration 16\n");
    // "plain:" with nothing is the same.
    const ConfigRead plain = read_config(kVariants, variant("plain"));
    CHECK(plain.override_errors.empty() && plain.config->variant == "plain");
    CHECK(plain.config->participants[0].command == small.config->participants[0].command);
    CHECK(plain.config->participants[0].variables == small.config->participants[0].variables);
    // The four things a variant changes: the command, the variable map (mapping by mapping),
    // enabled, and the parameters (by name).
    const ConfigRead medium = read_config(kVariants, variant("medium"));
    CHECK(medium.override_errors.empty());
    const ParticipantConfig& engine = medium.config->participants[0];
    CHECK(engine.command == std::vector<std::string>({"./engine_medium"}));
    CHECK_EQ(engine.command_line, std::size_t{22});
    CHECK_EQ(engine.variables.size(), std::size_t{3});
    CHECK(engine.variables[0].name == "engine.speed" && engine.variables[0].from == "rpm_x10" && engine.variables[0].scale == 0.1);
    CHECK(engine.variables[1].name == "engine.turbo" && engine.variables[1].from == "turbo_bar" && !engine.variables[1].absent);
    CHECK(engine.variables[2].name == "engine.load" && engine.variables[2].from == "load_pct");
    CHECK(engine.variable_lines == std::vector<std::size_t>({7, 23, 23}));
    CHECK(!medium.config->participants[1].enabled && medium.config->participants[2].enabled);
    // Who left a participant out: the variant (Dashboard), not the common part (Logger in small).
    CHECK(medium.config->participants[1].left_out_by_variant && !medium.config->participants[2].left_out_by_variant);
    CHECK(!small.config->participants[2].enabled && !small.config->participants[2].left_out_by_variant);
    check_text(params_text(medium.config->params), "idle_rpm=650 variant 26\ngear=\"D\" configuration 16\nboost=1.5 variant 26\n");
    // The command line comes after the variant.
    EnabledOverrides both = variant("medium");
    both.enable = {"Dashboard"};
    both.disable = {"Logger"};
    const ConfigRead overridden = read_config(kVariants, both);
    CHECK(overridden.override_errors.empty());
    CHECK(overridden.config->participants[1].enabled && !overridden.config->participants[2].enabled);
    // Taken in again, it is not the variant's any more; --disable is not the variant either.
    CHECK(!overridden.config->participants[1].left_out_by_variant && !overridden.config->participants[2].left_out_by_variant);
    // The rules that depend on enabled are those of the variant: here nothing required is left.
    const std::string lonely = "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n  - name: A\n    command: a\n"
                               "variants:\n  with: {}\n  without: { participants: { A: { enabled: false } } }\n";
    CHECK(lines(read_config(lonely, variant("with")).findings).empty());
    CHECK(lines(read_config(lonely, variant("without")).findings).find("E 3 no-required-participant: every participant is disabled") == 0);
}

TEST_CASE(a_file_with_variants_needs_one_and_says_which_there_are)
{
    const ConfigRead none = read_config(kVariants);
    check_text(lines(none.override_errors),
               "E 0 usage: the file defines variants, and a check or a run is of one of them: give --variant <name> with one of "
               "small, medium, plain (none of them is the default)\n");
    CHECK(none.config.has_value() && none.config->variant.empty());
    check_text(lines(read_config(kVariants, variant("medum")).override_errors),
               "E 0 usage: --variant medum: the file has no variant with this name (did you mean 'medium'?); its variants are "
               "small, medium, plain\n");
    check_text(lines(read_config(kExample, variant("medium")).override_errors),
               "E 0 usage: --variant medium: the file defines no variants: leave --variant out, or add variants to the file "
               "(docs/simulation-config.md)\n");
    // An error in the text may be why the name is not there: the text first.
    const ConfigRead broken = read_config(std::string(kExample) + "variants: 3\n", variant("medium"));
    CHECK(broken.override_errors.empty());
    CHECK_EQ(broken.findings.size(), std::size_t{1});
    // But a name that is none of the variants the file has is said whatever else is wrong: the
    // common part alone is no variant, and a check of it would say nothing about --variant.
    const std::string wrong = std::string(kVariants) + "  large: { step_ms: 5 }\n";
    check_text(lines(read_config(wrong, variant("medum")).override_errors),
               "E 0 usage: --variant medum: the file has no variant with this name (did you mean 'medium'?); its variants are "
               "small, medium, plain, large; the file has errors too, which a check with one of these variants reports\n");
    const ConfigRead right = read_config(wrong, variant("medium"));
    CHECK(right.override_errors.empty() && right.config->variant == "medium");
    CHECK(lines(right.findings).find("unknown-key: variant 'large': unknown key 'step_ms'") != std::string::npos);
}

TEST_CASE(every_variant_is_checked_and_only_its_keys_are_read)
{
    const std::string head = "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n  - name: Engine\n    command: ./engine\n"
                             "  - name: Tool\n    command: [./tool, \"{registry_uri}\"]\n    describe: false\n    required: false\n";
    const auto errors = [&head](const std::string& rest, const std::string& chosen = "a") {
        return lines(read_config(head + rest, variant(chosen)).findings);
    };
    check_text(errors("variants: [a, b]\n"),
               "E 10 value: variants must be a mapping of variant names to what each changes of the common part (variants: { "
               "small: {}, medium: { participants: { Engine: { command: [./engine_medium] } }, params: { idle_rpm: 650 } } }), "
               "not a list; for a simulation without variants, remove variants\n");
    check_text(errors("variants: {}\n"),
               "E 10 value: variants must be a mapping of variant names to what each changes of the common part (variants: { "
               "small: {}, medium: { participants: { Engine: { command: [./engine_medium] } }, params: { idle_rpm: 650 } } }), "
               "not an empty mapping; for a simulation without variants, remove variants\n");
    // The variant that is not chosen is checked too.
    check_text(errors("variants:\n"
                      "  a: {}\n"
                      "  -b: {}\n"
                      "  c: 3\n"
                      "  d:\n"
                      "    step_ms: 5\n"
                      "    participants:\n"
                      "      Engin: { enabled: false }\n"
                      "      Engine: { step_ms: 5, required: false, enabled: maybe, command: [./e, --name, x] }\n"
                      "      Tool: { command: [./tool, \"{nam}\"], variables: { a: { from: b, scal: 2 } } }\n"
                      "    params: { 1st: 1, idle: [1] }\n"
                      "  e:\n"
                      "    participants: [Engine]\n"),
               "E 12 value: variants: a variant's name must be a text on one line that does not start with '-' (it is given with "
               "--variant), not '-b'\n"
               "E 13 value: variant 'c': must be a mapping of what the variant changes (participants, params), or {} for the "
               "common part as it is, not '3'\n"
               "E 15 unknown-key: variant 'd': unknown key 'step_ms': remove it, or use one of the keys here (participants, "
               "params)\n"
               "E 17 variant-participant: variant 'd': participants has 'Engin', but the file's participants has no participant "
               "with this name (did you mean 'Engine'?): a variant changes the participants of the common part (for a "
               "participant of this variant only, list it there with enabled: false and write enabled: true here); the "
               "participants are Engine, Tool\n"
               "E 18 unknown-key: variant 'd': participant 'Engine': unknown key 'step_ms': remove it, or use one of the keys "
               "here (command, variables, network_names, j1939_nodes, enabled)\n"
               "E 18 unknown-key: variant 'd': participant 'Engine': unknown key 'required': remove it, or use one of the keys "
               "here (command, variables, network_names, j1939_nodes, enabled)\n"
               "E 18 value: variant 'd': participant 'Engine': enabled must be true or false, not 'maybe'\n"
               "E 18 launcher-argument: variant 'd': participant 'Engine': the command has '--name', but vehicle_sil_launch "
               "gives an ecu_main executable --name itself: remove it (or set describe: false for an executable not made with "
               "ecu_main)\n"
               "E 19 unknown-placeholder: variant 'd': participant 'Tool': unknown placeholder {nam} in the command: did you "
               "mean {name}? (the placeholders: {registry_uri}, {name}; write {{nam}} to pass {nam} as it is)\n"
               "N 19 no-registry-uri: variant 'd': participant 'Tool': the command has no {registry_uri}, so the executable is "
               "not told where the simulation's registry is: if it needs it, add its option for the registry URI with "
               "\"{registry_uri}\" as the value\n"
               "E 19 unknown-key: variant 'd': participant 'Tool': variables: a: unknown key 'scal': did you mean 'scale'? (the "
               "keys here: from, scale, offset, map, unit, absent)\n"
               "E 20 value: variant 'd': params: '1st' cannot be a parameter's name: use letters, digits and _ (not a digit "
               "first), as a scenario writes it in ${...}\n"
               "E 20 value: variant 'd': params: idle must be one value (a number, true or false, or a text), not a list\n"
               "E 22 value: variant 'e': participants must be a mapping of the participants' names to what the variant changes "
               "of each (participants: { Engine: { command: [./engine_medium] } }), not a list\n");
    // A variant whose command has an error cannot be described or started.
    const ConfigRead bad = read_config(head + "variants:\n  a: { participants: { Engine: { command: [./e, --name, x] } } }\n", variant("a"));
    CHECK(!bad.config->participants[0].valid && bad.config->participants[1].valid);
    // The common params are read as a variant's.
    check_text(lines(read_config(head + "params: 3\n").findings),
               "E 10 value: params must be a mapping of parameter names to values (params: { idle_rpm: 800 }), not '3'\n");
    check_text(lines(read_config(head + "params: { a: 1, a: 2, b: }\n").findings),
               "E 10 duplicate-key: params: 'a' appears twice (first at line 10); keep one of them\n"
               "E 10 value: params: b must be one value (a number, true or false, or a text), not empty\n");
}

// --- network_names ---------------------------------------------------------------------------

namespace {

/// A file with the networks VehicleCAN, BodyCAN (can) and Body (lin), and `participants`.
std::string with_networks(const std::string& participants, const std::string& more = {})
{
    return "vehicle_sil_simulation: 1\n"                    // 1
           "networks:\n"                                    // 2
           "  - { name: VehicleCAN, kind: can }\n"          // 3
           "  - { name: BodyCAN, kind: can }\n"             // 4
           "  - { name: Body, kind: lin }\n"                // 5
           "participants:\n" +                              // 6
           participants + more;
}

std::string names_text(const ParticipantConfig& p)
{
    std::string out;
    for (const NetworkName& n : p.network_names)
    {
        out += n.name + "<-" + n.from + "@" + std::to_string(n.line) + " ";
    }
    return out;
}

std::string words(const std::vector<std::string>& list)
{
    std::string out;
    for (const std::string& word : list)
    {
        out += "[" + word + "]";
    }
    return out;
}

} // namespace

TEST_CASE(network_names_say_what_the_executable_calls_a_network_of_the_file)
{
    // The simulation's name is the key, the executable's the value; the lines stay in the file's
    // order, and run gives them to the executable in that order and in that direction.
    const ConfigRead r = read_config(with_networks("  - name: Engine\n"            // 7
                                                   "    command: [engine]\n"       // 8
                                                   "    network_names:\n"          // 9
                                                   "      VehicleCAN: J1939_CAN\n" // 10
                                                   "      Body: Body\n"            // 11
                                                   "      BodyCAN: \"CAN 2\"\n"    // 12
                                                   "  - name: Dashboard\n"
                                                   "    command: [dashboard]\n"));
    check_text(lines(r.findings), "");
    const ParticipantConfig& engine = r.config->participants[0];
    CHECK(engine.valid);
    check_text(names_text(engine), "VehicleCAN<-J1939_CAN@10 Body<-Body@11 BodyCAN<-CAN 2@12 ");
    // A line whose two names are the same stands for no line.
    CHECK_EQ(renamed_networks(engine).size(), std::size_t{2});
    check_text(words(map_network_arguments(engine)), "[--map-network][VehicleCAN=J1939_CAN][--map-network][BodyCAN=CAN 2]");
    CHECK(r.config->participants[1].network_names.empty());
    CHECK(map_network_arguments(r.config->participants[1]).empty());
    // Two participants may swap two names, and one participant may too.
    check_text(read(with_networks("  - name: A\n    command: a\n    network_names: { VehicleCAN: BodyCAN, BodyCAN: VehicleCAN }\n")), "");
}

TEST_CASE(network_names_of_the_wrong_form_are_errors_and_the_participant_is_not_checked)
{
    const ConfigRead r = read_config(with_networks("  - name: A\n"                      // 7
                                                   "    command: a\n"                   // 8
                                                   "    network_names: [VehicleCAN]\n"  // 9
                                                   "  - name: B\n"                      // 10
                                                   "    command: b\n"                   // 11
                                                   "    network_names:\n"               // 12
                                                   "      VehicleCAN: [J1939_CAN]\n"    // 13
                                                   "      BodyCAN:\n"                   // 14
                                                   "      Body: LIN\n"                  // 15
                                                   "      Body: LIN2\n"                 // 16
                                                   "      \"a=b\": X\n"                 // 17
                                                   "      \"-v\": \"-v\"\n"             // 18
                                                   "  - name: C\n"
                                                   "    command: c\n"));
    check_text(lines(r.findings),
               "E 9 value: participant 'A': network_names must be a mapping of the simulation's network names (the names "
               "in networks) to the names the executable's code gives them (network_names: { VehicleCAN: J1939_CAN }), "
               "not a list\n"
               "E 13 value: participant 'B': network_names: 'VehicleCAN' must have the name the executable's code gives "
               "this network (a text, as in VehicleCAN: J1939_CAN), not a list\n"
               "E 14 value: participant 'B': network_names: 'BodyCAN' must have the name the executable's code gives "
               "this network (a text, as in VehicleCAN: J1939_CAN), not empty\n"
               "E 16 duplicate-key: participant 'B': network_names: 'Body' appears twice (first at line 15); keep one of "
               "them\n"
               "E 17 value: participant 'B': network_names: the key 'a=b' cannot be given to the executable (run passes "
               "--map-network <key>=<value>, which is split at the first '='): a network that is named here needs a name "
               "that is not empty, has no '=' and does not start with '-'; change the name in networks\n"
               "E 18 value: participant 'B': network_names: the key '-v' cannot be given to the executable (run passes "
               "--map-network <key>=<value>, which is split at the first '='): a network that is named here needs a name "
               "that is not empty, has no '=' and does not start with '-'; change the name in networks\n");
    // Neither described nor started with names the file does not mean; the next entry is.
    CHECK(!r.config->participants[0].valid && !r.config->participants[1].valid && r.config->participants[2].valid);
    check_text(names_text(r.config->participants[1]), "Body<-LIN@15 ");
}

TEST_CASE(a_key_of_network_names_is_a_network_of_the_file_and_an_executables_network_has_one_name)
{
    // networks may come after participants: the keys are compared when the whole file is read.
    const std::string file = "vehicle_sil_simulation: 1\n"            // 1
                             "participants:\n"                        // 2
                             "  - name: Engine\n"                     // 3
                             "    command: engine\n"                  // 4
                             "    network_names:\n"                   // 5
                             "      VehicleCAM: J1939_CAN\n"          // 6
                             "      J1939_LIN: Body\n"                // 7
                             "      Other: Other\n"                   // 8
                             "      Unknown: X\n"                     // 9
                             "  - name: Dashboard\n"                  // 10
                             "    command: dashboard\n"               // 11
                             "    network_names:\n"                   // 12
                             "      VehicleCAN: CAN\n"                // 13
                             "      Body: LIN\n"                      // 14
                             "      BodyCAN: CAN\n"                   // 15
                             "networks:\n"
                             "  - { name: VehicleCAN, kind: can }\n"
                             "  - { name: BodyCAN, kind: can }\n"
                             "  - { name: Body, kind: lin }\n";
    const ConfigRead r = read_config(file);
    check_text(lines(r.findings),
               "E 6 network-map-undeclared: participant 'Engine': network_names has the key 'VehicleCAM', which is not in "
               "networks: the key is the simulation's name (a name from networks), the value is the executable's name "
               "(did you mean 'VehicleCAN'?)\n"
               // The two names the wrong way round is the mistake to expect: said with the line to write.
               "E 7 network-map-undeclared: participant 'Engine': network_names has the key 'J1939_LIN', which is not in "
               "networks: the key is the simulation's name (a name from networks), the value is the executable's name. "
               "'Body' is in networks, so the two are the wrong way round: write Body: J1939_LIN\n"
               "E 9 network-map-undeclared: participant 'Engine': network_names has the key 'Unknown', which is not in "
               "networks: the key is the simulation's name (a name from networks), the value is the executable's name; "
               "correct the key, or add 'Unknown' to networks\n"
               "E 15 network-map-collision: participant 'Dashboard': network_names gives the executable's network 'CAN' "
               "two names, 'VehicleCAN' (line 13) and 'BodyCAN' (line 15), but a network of an ECU is on one bus: keep "
               "one of the two lines\n");
    CHECK(r.findings[0].participant == "Engine" && r.findings[0].network == "VehicleCAM");
    CHECK(r.findings[3].participant == "Dashboard" && r.findings[3].network == "BodyCAN");
    CHECK(!r.config->participants[0].valid && !r.config->participants[1].valid);
}

TEST_CASE(a_variant_lays_its_network_names_over_the_common_ones)
{
    const std::string file = with_networks("  - name: Engine\n"                      // 7
                                           "    command: engine\n"                   // 8
                                           "    network_names:\n"                    // 9
                                           "      VehicleCAN: J1939_CAN\n"           // 10
                                           "      Body: LIN\n"                       // 11
                                           "  - name: Dashboard\n"                   // 12
                                           "    command: dashboard\n",               // 13
                                           "variants:\n"                             // 14
                                           "  plain: {}\n"                           // 15
                                           "  other:\n"                              // 16
                                           "    participants:\n"                     // 17
                                           "      Engine:\n"                         // 18
                                           "        network_names:\n"                // 19
                                           "          Body: Body\n"                  // 20
                                           "          VehicleCAN: CAN_A\n"           // 21
                                           "          BodyCAN: CAN_B\n"              // 22
                                           "      Dashboard:\n"                      // 23
                                           "        network_names: { VehicleCAN: CAN }\n" // 24
                                           "  twice:\n"                              // 25
                                           "    participants:\n"                     // 26
                                           "      Engine: { network_names: { BodyCAN: J1939_CAN } }\n" // 27
                                           "  typo:\n"                               // 28
                                           "    participants:\n"                     // 29
                                           "      Dashboard: { network_names: { VehicleCAM: CAN } }\n"); // 30
    EnabledOverrides choice;
    choice.variant = "plain";
    const ConfigRead plain = read_config(file, choice);
    // Every variant is checked, also the ones that are not chosen; what the common part says is
    // not said again for a variant.
    const std::string findings =
        "E 27 network-map-collision: variant 'twice': participant 'Engine': network_names gives the executable's network "
        "'J1939_CAN' two names, 'VehicleCAN' (line 10, the common part) and 'BodyCAN' (line 27, this variant), but a "
        "network of an ECU is on one bus: to have it on 'BodyCAN' in this variant, add the line VehicleCAN: VehicleCAN to "
        "the variant's network_names of 'Engine' (a line whose two names are the same takes the common line away), or "
        "remove the variant's line\n"
        "E 30 network-map-undeclared: variant 'typo': participant 'Dashboard': network_names has the key 'VehicleCAM', which "
        "is not in networks: the key is the simulation's name (a name from networks), the value is the executable's name "
        "(did you mean 'VehicleCAN'?)\n";
    check_text(lines(plain.findings), findings);
    check_text(names_text(plain.config->participants[0]), "VehicleCAN<-J1939_CAN@10 Body<-LIN@11 ");
    CHECK(plain.config->participants[0].valid && plain.config->participants[1].valid);
    // A line of the same key takes the common line's place (with the same name twice: takes it
    // away), a new key goes after them.
    choice.variant = "other";
    const ConfigRead other = read_config(file, choice);
    check_text(lines(other.findings), findings);
    check_text(names_text(other.config->participants[0]), "VehicleCAN<-CAN_A@21 Body<-Body@20 BodyCAN<-CAN_B@22 ");
    check_text(words(map_network_arguments(other.config->participants[0])),
               "[--map-network][VehicleCAN=CAN_A][--map-network][BodyCAN=CAN_B]");
    check_text(names_text(other.config->participants[1]), "VehicleCAN<-CAN@24 ");
    CHECK(other.config->participants[0].valid && other.config->participants[1].valid);
    // The participant is not checked in the variant whose names are wrong.
    choice.variant = "twice";
    const ConfigRead twice = read_config(file, choice);
    CHECK(!twice.config->participants[0].valid && twice.config->participants[1].valid);
    choice.variant = "typo";
    CHECK(!read_config(file, choice).config->participants[1].valid);
}

TEST_CASE(map_network_is_written_as_network_names_not_in_a_command)
{
    // An ecu_main command: the launcher gives the argument itself, and the message says what to write.
    check_text(read(with_networks("  - name: A\n    command: [a, --map-network, VehicleCAN=J1939_CAN]\n"
                                  "  - name: B\n    command: [b, --map-network=BodyCAN=CAN_2]\n"
                                  "  - name: C\n    command: [c, --map-network]\n")),
               "E 8 launcher-argument: participant 'A': the command has '--map-network', but vehicle_sil_launch gives an "
               "ecu_main executable --map-network itself, from the participant's network_names: remove it from the command "
               "and write network_names: { VehicleCAN: J1939_CAN } for this participant\n"
               "E 10 launcher-argument: participant 'B': the command has '--map-network=BodyCAN=CAN_2', but "
               "vehicle_sil_launch gives an ecu_main executable --map-network itself, from the participant's network_names: "
               "remove it from the command and write network_names: { BodyCAN: CAN_2 } for this participant\n"
               "E 12 launcher-argument: participant 'C': the command has '--map-network', but vehicle_sil_launch gives an "
               "ecu_main executable --map-network itself, from the participant's network_names: remove it from the command "
               "and write network_names: { <simulation's name>: <executable's name> } for this participant\n");
    // Any other command may have it (the executable is on another computer, say), unless the entry
    // has network_names too: then run would give it twice.
    const std::string remote = "  - name: Remote\n"                                                            // 7
                               "    command: [remote, --registry-uri, \"{registry_uri}\", --map-network, VehicleCAN=CAN]\n" // 8
                               "    describe: false\n";                                                        // 9
    check_text(read(with_networks(remote)), "");
    const ConfigRead both = read_config(with_networks(remote + "    network_names: { BodyCAN: CAN_2, Body: Body }\n")); // 10
    check_text(lines(both.findings),
               "E 8 launcher-argument: participant 'Remote': the command has --map-network, and the participant has "
               "network_names, for which vehicle_sil_launch gives the executable --map-network itself (--map-network "
               "BodyCAN=CAN_2): remove --map-network from the command and write every mapping in network_names "
               "(network_names: { <simulation's name>: <executable's name> })\n"
               "N 10 network-map-not-checked: participant 'Remote': network_names of a participant that is not described "
               "(describe: false) cannot be checked: run gives the executable --map-network BodyCAN=CAN_2 after its "
               "command (the command to start it by hand shows them too), which only an executable made with ecu_main "
               "that lists \"map-network\" in the features of its --describe takes. For any other executable (a bridge, a "
               "tool), remove network_names and give the simulation's network name with the executable's own argument or "
               "settings\n");
    CHECK(!both.config->participants[0].valid);
    // A line that renames nothing asks for no argument: no error and no note.
    check_text(read(with_networks(remote + "    network_names: { Body: Body }\n")), "");
    // A variant that brings the two together is the one that is wrong.
    const std::string variants = "variants:\n"                                                                  // 10
                                 "  plain: {}\n"                                                                // 11
                                 "  named: { participants: { Remote: { network_names: { BodyCAN: CAN_2 } } } }\n"; // 12
    EnabledOverrides choice;
    choice.variant = "plain";
    const ConfigRead plain = read_config(with_networks(remote, variants), choice);
    CHECK(plain.config->participants[0].valid);
    CHECK(lines(plain.findings).find("E 8 launcher-argument: variant 'named': participant 'Remote': the command has "
                                     "--map-network, and the participant has network_names") == 0);
    // The note says what this run gives the executable: none for the variant that has no line.
    CHECK(lines(plain.findings).find("network-map-not-checked") == std::string::npos);
    choice.variant = "named";
    const ConfigRead named = read_config(with_networks(remote, variants), choice);
    CHECK(!named.config->participants[0].valid);
    CHECK(lines(named.findings).find("\nN 12 network-map-not-checked: participant 'Remote': network_names of a participant "
                                     "that is not described (describe: false) cannot be checked: run gives the executable "
                                     "--map-network BodyCAN=CAN_2 after its command") != std::string::npos);
}

TEST_CASE(the_note_of_network_names_that_are_not_checked_says_what_the_chosen_variant_gives)
{
    // Far is not described, so the note is the one place that says what its executable is given:
    // the lines of the chosen variant laid over the common ones, as run builds the arguments.
    const std::string file = with_networks("  - name: Far\n"                                    // 7
                                           "    command: [far, \"{registry_uri}\"]\n"            // 8
                                           "    describe: false\n"                               // 9
                                           "    network_names: { VehicleCAN: Can_A }\n"          // 10
                                           "  - name: Near\n"                                    // 11
                                           "    command: near\n",                                // 12
                                           "variants:\n"                                         // 13
                                           "  common: {}\n"                                      // 14
                                           "  other:\n"                                          // 15
                                           "    participants:\n"                                 // 16
                                           "      Far:\n"                                        // 17
                                           "        network_names:\n"                            // 18
                                           "          VehicleCAN: VehicleCAN\n"                  // 19
                                           "          BodyCAN: Can_B\n"                          // 20
                                           "  none: { participants: { Far: { network_names: { VehicleCAN: VehicleCAN } } } }\n"); // 21
    const auto note = [&file](const EnabledOverrides& choice) {
        const ConfigRead r = read_config(file, choice);
        std::string out;
        for (const Finding& f : r.findings)
        {
            if (f.code == "network-map-not-checked")
            {
                const std::string from = "run gives the executable ";
                const auto at = f.message.find(from) + from.size();
                out += std::to_string(f.line) + " " + f.participant + ": " + f.message.substr(0, f.message.find(':')) + ": " +
                       f.message.substr(at, f.message.find(" after its command") - at) + "\n";
                // The same words as the arguments of the run.
                std::string given;
                for (const std::string& word : map_network_arguments(r.config->participants[0]))
                {
                    given += (given.empty() ? "" : " ") + word;
                }
                CHECK(f.message.substr(at, f.message.find(" after its command") - at) == given);
            }
        }
        return out;
    };
    EnabledOverrides choice;
    choice.variant = "common";
    check_text(note(choice), "10 Far: participant 'Far': --map-network VehicleCAN=Can_A\n");
    choice.variant = "other";
    check_text(note(choice), "20 Far: participant 'Far': --map-network BodyCAN=Can_B\n");
    // The variant takes every line away: nothing is given, and nothing is noted.
    choice.variant = "none";
    check_text(note(choice), "");
    // A participant that is left out is given nothing either.
    choice.variant = "common";
    choice.disable = {"Far"};
    check_text(note(choice), "");
}

TEST_CASE(a_variant_that_moves_a_network_is_told_how_to_take_the_common_line_away)
{
    // The common part puts the executable's J1939_CAN on VehicleCAN; the variant wants it on
    // BodyCAN. Its line alone gives the network two names; the message has the line to add.
    const std::string head = "  - name: Third\n"                           // 7
                             "    command: third\n"                        // 8
                             "    network_names: { VehicleCAN: J1939_CAN }\n"; // 9
    const std::string moved = "variants:\n"                                // 10
                              "  plain: {}\n"                              // 11
                              "  moved:\n"                                 // 12
                              "    participants:\n"                        // 13
                              "      Third:\n"                             // 14
                              "        network_names:\n"                   // 15
                              "          BodyCAN: J1939_CAN\n";            // 16
    EnabledOverrides choice;
    choice.variant = "moved";
    const ConfigRead wrong = read_config(with_networks(head, moved), choice);
    check_text(lines(wrong.findings),
               "E 16 network-map-collision: variant 'moved': participant 'Third': network_names gives the executable's "
               "network 'J1939_CAN' two names, 'VehicleCAN' (line 9, the common part) and 'BodyCAN' (line 16, this "
               "variant), but a network of an ECU is on one bus: to have it on 'BodyCAN' in this variant, add the line "
               "VehicleCAN: VehicleCAN to the variant's network_names of 'Third' (a line whose two names are the same "
               "takes the common line away), or remove the variant's line\n");
    CHECK(!wrong.config->participants[0].valid);
    // With the line the message gives, the variant is right, and run gives the one name.
    const ConfigRead right = read_config(with_networks(head, moved + "          VehicleCAN: VehicleCAN\n"), choice);
    check_text(lines(right.findings), "");
    check_text(words(map_network_arguments(right.config->participants[0])), "[--map-network][BodyCAN=J1939_CAN]");
    // Two lines of one variant, or of the common part, are as before: one of them goes.
    const ConfigRead both = read_config(with_networks(head, "variants:\n  two: { participants: { Third: { network_names: "
                                                            "{ VehicleCAN: VehicleCAN, BodyCAN: X, Body: X } } } }\n"),
                                        EnabledOverrides{{}, {}, {}, "two"});
    CHECK(lines(both.findings).find("'BodyCAN' (line 11) and 'Body' (line 11), but a network of an ECU is on one bus: keep "
                                    "one of the two lines\n") != std::string::npos);
}

TEST_CASE(network_names_of_a_disabled_participant_are_read_and_checked_like_any_other)
{
    const std::string file = with_networks("  - name: A\n    command: a\n    network_names: { VehicleCAM: CAN, BodyCAN: CAN_2 }\n" // 9
                                           "  - name: B\n    command: b\n");
    EnabledOverrides overrides;
    overrides.disable = {"A"};
    const ConfigRead r = read_config(file, overrides);
    CHECK(r.override_errors.empty());
    CHECK(!r.config->participants[0].enabled);
    CHECK(lines(r.findings).find("E 9 network-map-undeclared: participant 'A': ") == 0);
    check_text(names_text(r.config->participants[0]), "VehicleCAM<-CAN@9 BodyCAN<-CAN_2@9 ");
}

TEST_CASE(a_renamed_network_has_ascii_names_where_the_arguments_come_in_the_code_page)
{
    // ecu_main on Windows refuses a --map-network that is not ASCII (its arguments are not UTF-8
    // there), so the launcher of a Windows build says it before anything is started. Elsewhere the
    // names go through as they are.
    const std::string file = "vehicle_sil_simulation: 1\nnetworks:\n  - { name: \xE8\xBB\x8A\xE4\xB8\xA1""CAN, kind: can }\n"
                             "  - { name: Body, kind: can }\n"
                             "participants:\n  - name: A\n    command: a\n"
                             "    network_names: { \xE8\xBB\x8A\xE4\xB8\xA1""CAN: J1939_CAN, Body: \xE8\xBB\x8A, J1939_CAN: J1939_CAN }\n"; // 8
    const ConfigRead r = read_config(file);
#ifdef _WIN32
    check_text(lines(r.findings),
               "E 8 network-map-not-ascii: participant 'A': network_names: '\xE8\xBB\x8A\xE4\xB8\xA1""CAN: J1939_CAN' has "
               "characters that are not ASCII, which an ECU executable on Windows does not take with --map-network (its "
               "arguments arrive in the code page, not in UTF-8): use ASCII names for a network that is renamed (change "
               "the name in networks, or have the ECU's author change the name in its code)\n"
               "E 8 network-map-not-ascii: participant 'A': network_names: 'Body: \xE8\xBB\x8A' has characters that are "
               "not ASCII, which an ECU executable on Windows does not take with --map-network (its arguments arrive in "
               "the code page, not in UTF-8): use ASCII names for a network that is renamed (change the name in networks, "
               "or have the ECU's author change the name in its code)\n");
    CHECK(!r.config->participants[0].valid);
#else
    check_text(lines(r.findings), "");
    CHECK(r.config->participants[0].valid);
    check_text(words(map_network_arguments(r.config->participants[0])),
               "[--map-network][\xE8\xBB\x8A\xE4\xB8\xA1""CAN=J1939_CAN][--map-network][Body=\xE8\xBB\x8A]");
#endif
}

// --- j1939_nodes ---------------------------------------------------------------------------------

namespace {

/// The entries of the participant's j1939_nodes: "<network>=<settings>@<line> " each.
std::string nodes_text(const ParticipantConfig& p)
{
    std::string out;
    for (const J1939NodeEntry& e : p.j1939_nodes)
    {
        out += e.network + "=" + j1939::format_node_override(e.node) + "@" + std::to_string(e.line) + " ";
    }
    return out;
}

} // namespace

TEST_CASE(j1939_nodes_are_read_with_an_optional_from_and_hexadecimal_numbers)
{
    const ConfigRead r = read_config(with_networks(
        "  - name: Engine\n"                                                                     // 7
        "    command: engine\n"                                                                  // 8
        "    j1939_nodes:\n"                                                                     // 9
        "      - { network: VehicleCAN, from: 0x00, address: 0x01, identity_number: 2 }\n"      // 10
        "      - network: BodyCAN\n"                                                             // 11
        "        address: 254\n"                                                                 // 12
        "        identity_number: 2097151\n"                                                     // 13
        "        function_instance: 0x1F\n"                                                      // 14
        "        ecu_instance: 7\n"                                                              // 15
        "  - name: Gateway\n"                                                                    // 16
        "    command: gateway\n"                                                                 // 17
        "    j1939_nodes: [{ network: VehicleCAN, from: 0xfe, address: 0, identity_number: 0, function_instance: 0, "
        "ecu_instance: 0 }, { network: BodyCAN, from: 0x21 }]\n"                                 // 18
        // YAML 1.2 integers, as the schema's editors read them: 010 is ten, +1 one, 0o10 eight.
        "  - name: Door\n"                                                                       // 19
        "    command: door\n"                                                                    // 20
        "    j1939_nodes: [{ network: VehicleCAN, from: 010, address: +1, identity_number: 0o10 }]\n")); // 21
    check_text(lines(r.findings), "");
    const ParticipantConfig& engine = r.config->participants[0];
    const ParticipantConfig& gateway = r.config->participants[1];
    CHECK(engine.valid && gateway.valid);
    check_text(nodes_text(engine), "VehicleCAN=from:0x00,address:0x01,identity_number:2@10 "
                                   "BodyCAN=address:254,identity_number:2097151,function_instance:31,ecu_instance:7@11 ");
    check_text(nodes_text(gateway), "VehicleCAN=from:254,address:0x00,identity_number:0,function_instance:0,ecu_instance:0@18 "
                                    "BodyCAN=from:0x21@18 ");
    // run gives the entries that change something, in the file's order.
    check_text(words(j1939_node_arguments(engine)),
               "[--j1939-node][VehicleCAN=from:0x00,address:0x01,identity_number:2][--j1939-node]"
               "[BodyCAN=address:254,identity_number:2097151,function_instance:31,ecu_instance:7]");
    check_text(words(j1939_node_arguments(gateway)),
               "[--j1939-node][VehicleCAN=from:254,address:0x00,identity_number:0,function_instance:0,ecu_instance:0]");
    CHECK(j1939_node_arguments(r.config->participants[0]).size() == 4);
    check_text(nodes_text(r.config->participants[2]), "VehicleCAN=from:0x0A,address:0x01,identity_number:8@21 ");
}

TEST_CASE(j1939_nodes_of_the_wrong_form_are_errors_and_the_participant_is_not_checked)
{
    const ConfigRead r = read_config(with_networks("  - name: A\n"                                               // 7
                                                   "    command: a\n"                                            // 8
                                                   "    j1939_nodes: { network: VehicleCAN }\n"                  // 9
                                                   "  - name: B\n"                                               // 10
                                                   "    command: b\n"                                            // 11
                                                   "    j1939_nodes:\n"                                          // 12
                                                   "      - VehicleCAN\n"                                        // 13
                                                   "      - { address: 1 }\n"                                    // 14
                                                   "      - { network: \"\", address: 1 }\n"                     // 15
                                                   "      - { network: \"a=b\", address: 1 }\n"                  // 16
                                                   "      - { network: \"-v\", address: 1 }\n"                   // 17
                                                   "      - { network: VehicleCAN, adress: 1 }\n"                // 18
                                                   "      - { network: VehicleCAN, address: 255 }\n"             // 19
                                                   "      - { network: VehicleCAN, address: \"0x01\" }\n"        // 20
                                                   "      - { network: VehicleCAN, identity_number: 0x200000 }\n" // 21
                                                   "      - { network: VehicleCAN, function_instance: 32 }\n"    // 22
                                                   "      - { network: VehicleCAN, ecu_instance: -1 }\n"         // 23
                                                   "      - { network: VehicleCAN, from: 0X10 }\n"               // 24
                                                   "      - { network: [VehicleCAN], address: 1 }\n"             // 25
                                                   "      - { network: BodyCAN, address: 1 }\n"                  // 26
                                                   "      - { network: BodyCAN, address: 0b11 }\n"               // 27
                                                   "      - { network: BodyCAN, address: 1_0 }\n"                // 28
                                                   "      - { network: BodyCAN, address: +0x10 }\n"              // 29
                                                   "      - { network: BodyCAN, address: 0O10 }\n"               // 30
                                                   "  - name: C\n"                                               // 31
                                                   "    command: c\n"));
    const std::string form = "{ network: VehicleCAN, from: 0x00, address: 0x01, identity_number: 2 }";
    const std::string comma = "(run passes --j1939-node <network>=<settings>, which is split at the first '='): a network "
                              "with j1939_nodes needs a name that has no '=' and does not start with '-'; change the name "
                              "in networks\n";
    check_text(lines(r.findings),
               "E 9 value: participant 'A': j1939_nodes must be a list of the J1939 nodes to change, each with its network "
               "and the settings to change (j1939_nodes: [" + form + "]), not a mapping\n"
               "E 13 value: participant 'B': j1939_nodes: an entry must be a mapping with network and the settings to "
               "change (" + form + "), not 'VehicleCAN'\n"
               "E 14 missing-key: participant 'B': j1939_nodes: an entry has no network: add network: <the simulation's "
               "name of the node's CAN network> (" + form + ")\n"
               "E 15 value: participant 'B': j1939_nodes: network must be the simulation's name of the node's CAN network "
               "(a name in networks), not the text \"\"\n"
               "E 16 value: participant 'B': j1939_nodes: the network 'a=b' cannot be given to the executable " + comma +
               "E 17 value: participant 'B': j1939_nodes: the network '-v' cannot be given to the executable " + comma +
               "E 18 unknown-key: participant 'B': j1939_nodes: unknown key 'adress': did you mean 'address'? (the keys "
               "here: network, from, address, identity_number, function_instance, ecu_instance)\n"
               "E 19 value: participant 'B': j1939_nodes: address must be the node's new address, a number from 0 to 254 "
               "(decimal, or 0x.. in hexadecimal; 254: any free address), not '255'\n"
               "E 20 value: participant 'B': j1939_nodes: address must be the node's new address, a number from 0 to 254 "
               "(decimal, or 0x.. in hexadecimal; 254: any free address), not the text \"0x01\"\n"
               "E 21 value: participant 'B': j1939_nodes: identity_number must be a number from 0 to 2097151 (decimal, or "
               "0x.. in hexadecimal), not '0x200000'\n"
               "E 22 value: participant 'B': j1939_nodes: function_instance must be a number from 0 to 31 (decimal, or "
               "0x.. in hexadecimal), not '32'\n"
               "E 23 value: participant 'B': j1939_nodes: ecu_instance must be a number from 0 to 7 (decimal, or 0x.. in "
               "hexadecimal), not '-1'\n"
               "E 24 value: participant 'B': j1939_nodes: from must be the address of the node in the executable's code, "
               "a number from 0 to 254 (decimal, or 0x.. in hexadecimal; 254: a node that takes any free address), not "
               "'0X10'\n"
               "E 25 value: participant 'B': j1939_nodes: network must be the simulation's name of the node's CAN network "
               "(a name in networks), not a list\n"
               // Texts in YAML 1.2, as for the schema.
               "E 27 value: participant 'B': j1939_nodes: address must be the node's new address, a number from 0 to 254 "
               "(decimal, or 0x.. in hexadecimal; 254: any free address), not '0b11'\n"
               "E 28 value: participant 'B': j1939_nodes: address must be the node's new address, a number from 0 to 254 "
               "(decimal, or 0x.. in hexadecimal; 254: any free address), not '1_0'\n"
               "E 29 value: participant 'B': j1939_nodes: address must be the node's new address, a number from 0 to 254 "
               "(decimal, or 0x.. in hexadecimal; 254: any free address), not '+0x10'\n"
               "E 30 value: participant 'B': j1939_nodes: address must be the node's new address, a number from 0 to 254 "
               "(decimal, or 0x.. in hexadecimal; 254: any free address), not '0O10'\n");
    CHECK(r.findings[3].participant == "B" && r.findings[4].network == "a=b" && r.findings[7].network == "VehicleCAN");
    // Neither described nor started with values the file does not mean; the next entry is.
    CHECK(!r.config->participants[0].valid && !r.config->participants[1].valid && r.config->participants[2].valid);
    check_text(nodes_text(r.config->participants[1]), "BodyCAN=address:0x01@26 ");
}

TEST_CASE(j1939_nodes_name_a_can_network_of_the_simulation)
{
    const ConfigRead r = read_config(with_networks("  - name: Engine2\n"                                   // 7
                                                   "    command: engine\n"                                 // 8
                                                   "    network_names: { VehicleCAN: J1939_CAN }\n"        // 9
                                                   "    j1939_nodes:\n"                                    // 10
                                                   "      - { network: J1939_CAN, address: 1 }\n"          // 11
                                                   "      - { network: VehicleCAM, from: 0, address: 1 }\n" // 12
                                                   "      - { network: Body, address: 1 }\n"               // 13
                                                   "      - { network: Wheel, address: 1 }\n"              // 14
                                                   "      - { network: VehicleCAN, address: 1 }\n"));      // 15
    check_text(lines(r.findings),
               "E 11 j1939-node-undeclared: participant 'Engine2': j1939_nodes (line 11) names the network 'J1939_CAN', "
               "which is the executable's name; network_names (line 9) calls it 'VehicleCAN' in the simulation: write "
               "network: VehicleCAN\n"
               "E 12 j1939-node-undeclared: participant 'Engine2': j1939_nodes (line 12) names the network 'VehicleCAM', "
               "which is not in networks: network is the simulation's name of a CAN network (did you mean 'VehicleCAN'?)\n"
               "E 13 j1939-node-undeclared: participant 'Engine2': j1939_nodes (line 13) names the network 'Body', which "
               "is a lin network (line 5): j1939_nodes is for J1939 nodes on a CAN network\n"
               "E 14 j1939-node-undeclared: participant 'Engine2': j1939_nodes (line 14) names the network 'Wheel', which "
               "is not in networks: network is the simulation's name of a CAN network\n");
    CHECK(r.findings[0].participant == "Engine2" && r.findings[0].network == "J1939_CAN");
    CHECK(!r.config->participants[0].valid);
    // The simulation's name of a renamed network, and a name that is both: no error.
    const ConfigRead swapped = read_config(with_networks("  - name: A\n    command: a\n"
                                                         "    network_names: { VehicleCAN: BodyCAN, BodyCAN: VehicleCAN }\n"
                                                         "    j1939_nodes: [{ network: BodyCAN, address: 1 }]\n"));
    check_text(lines(swapped.findings), "");
}

TEST_CASE(j1939_nodes_have_one_entry_per_node)
{
    const std::string file = with_networks("  - name: Engine2\n"                                             // 7
                                           "    command: engine\n"                                           // 8
                                           "    j1939_nodes:\n"                                              // 9
                                           "      - { network: VehicleCAN, from: 0x00, address: 0x01 }\n"    // 10
                                           "      - { network: BodyCAN, address: 0x05 }\n"                   // 11
                                           "      - { network: VehicleCAN, from: 0, identity_number: 2 }\n"  // 12
                                           "      - { network: BodyCAN, from: 0x21 }\n"                      // 13
                                           "  - name: Gateway\n"                                             // 14
                                           "    command: gateway\n"                                          // 15
                                           "    j1939_nodes: [{ network: VehicleCAN, address: 0x01 }]\n",    // 16
                                           "variants:\n"                                                     // 17
                                           "  plain: {}\n"                                                   // 18
                                           "  other:\n"                                                      // 19
                                           "    participants:\n"                                             // 20
                                           "      Gateway:\n"                                                // 21
                                           "        j1939_nodes: [{ network: VehicleCAN, from: 0x00, address: 0x02 }]\n" // 22
                                           "  twice:\n"                                                      // 23
                                           "    participants:\n"                                             // 24
                                           "      Gateway:\n"                                                // 25
                                           "        j1939_nodes:\n"                                          // 26
                                           "          - { network: BodyCAN, from: 0x10, address: 0x11 }\n"   // 27
                                           "          - { network: BodyCAN, from: 16, address: 0x12 }\n"     // 28
                                           "  mixed:\n"                                                      // 29
                                           "    participants:\n"                                             // 30
                                           "      Gateway:\n"                                                // 31
                                           "        j1939_nodes:\n"                                          // 32
                                           "          - { network: BodyCAN, address: 0x01 }\n"               // 33
                                           "          - { network: BodyCAN, from: 0x00, identity_number: 2 }\n"); // 34
    EnabledOverrides choice;
    choice.variant = "plain";
    const ConfigRead plain = read_config(file, choice);
    // An entry that changes nothing (line 13) counts too. Every variant is checked; what the
    // common part says is not said again for a variant.
    const std::string findings =
        "E 12 j1939-node-duplicate: participant 'Engine2': j1939_nodes has two entries (lines 10 and 12) for the node "
        "that the executable's code puts at 0x00 on 'VehicleCAN': write its settings in one entry\n"
        "E 13 j1939-node-duplicate: participant 'Engine2': j1939_nodes has an entry without from (line 11) for 'BodyCAN', "
        "which stands for the only node of the executable there, and another entry for that network (line 13): give "
        "every entry of this network a from (the address in the executable's code)\n"
        // The common entry has no from and the variant's has one: two entries, not one in place of the other.
        "E 22 j1939-node-duplicate: variant 'other': participant 'Gateway': j1939_nodes has an entry without from (line "
        "16) for 'VehicleCAN', which stands for the only node of the executable there, and another entry for that "
        "network (line 22): give every entry of this network a from (the address in the executable's code); an entry "
        "of a variant replaces the common entry only if both have the same network and the same from (or both have no "
        "from)\n"
        "E 28 j1939-node-duplicate: variant 'twice': participant 'Gateway': j1939_nodes has two entries (lines 27 and 28) "
        "for the node that the executable's code puts at 0x10 on 'BodyCAN': write its settings in one entry\n"
        // The variant's own entries clash: said once, not again with the common ones laid under them.
        "E 34 j1939-node-duplicate: variant 'mixed': participant 'Gateway': j1939_nodes has an entry without from (line "
        "33) for 'BodyCAN', which stands for the only node of the executable there, and another entry for that network "
        "(line 34): give every entry of this network a from (the address in the executable's code)\n";
    check_text(lines(plain.findings), findings);
    CHECK(!plain.config->participants[0].valid && plain.config->participants[1].valid);
    choice.variant = "other";
    const ConfigRead other = read_config(file, choice);
    check_text(lines(other.findings), findings);
    CHECK(!other.config->participants[1].valid);
    choice.variant = "twice";
    CHECK(!read_config(file, choice).config->participants[1].valid);
}

TEST_CASE(a_variant_lays_its_j1939_nodes_over_the_common_ones)
{
    const std::string file = with_networks("  - name: Engine2\n"                                                       // 7
                                           "    command: engine\n"                                                     // 8
                                           "    j1939_nodes:\n"                                                        // 9
                                           "      - { network: VehicleCAN, from: 0x00, address: 0x01, identity_number: 2 }\n" // 10
                                           "      - { network: BodyCAN, address: 0x05 }\n",                            // 11
                                           "variants:\n"                                                               // 12
                                           "  plain: {}\n"                                                             // 13
                                           "  other:\n"                                                                // 14
                                           "    participants:\n"                                                       // 15
                                           "      Engine2:\n"                                                          // 16
                                           "        j1939_nodes:\n"                                                    // 17
                                           "          - { network: VehicleCAN, from: 0x00, address: 0x03 }\n"          // 18
                                           "          - { network: VehicleCAN, from: 0x21, address: 0x22 }\n"          // 19
                                           "          - { network: BodyCAN }\n");                                      // 20
    EnabledOverrides choice;
    choice.variant = "plain";
    const ConfigRead plain = read_config(file, choice);
    check_text(lines(plain.findings), "");
    check_text(nodes_text(plain.config->participants[0]),
               "VehicleCAN=from:0x00,address:0x01,identity_number:2@10 BodyCAN=address:0x05@11 ");
    // The same node: the variant's entry whole (the common identity_number is not kept); a new node
    // goes after them; an entry that changes nothing takes the common one away.
    choice.variant = "other";
    const ConfigRead other = read_config(file, choice);
    check_text(lines(other.findings), "");
    CHECK(other.config->participants[0].valid);
    check_text(nodes_text(other.config->participants[0]),
               "VehicleCAN=from:0x00,address:0x03@18 BodyCAN=@20 VehicleCAN=from:0x21,address:0x22@19 ");
    check_text(words(j1939_node_arguments(other.config->participants[0])),
               "[--j1939-node][VehicleCAN=from:0x00,address:0x03][--j1939-node][VehicleCAN=from:0x21,address:0x22]");
}

TEST_CASE(j1939_node_is_written_as_j1939_nodes_not_in_a_command)
{
    // An ecu_main command: the launcher gives the argument itself, and the message says what to write.
    check_text(read(with_networks("  - name: A\n    command: [a, --j1939-node, \"VehicleCAN=from:0x00,address:0x01\"]\n"
                                  "  - name: B\n    command: [b, \"--j1939-node=BodyCAN=address:254,identity_number:2\"]\n"
                                  "  - name: C\n    command: [c, --j1939-node, VehicleCAN]\n")),
               "E 8 launcher-argument: participant 'A': the command has '--j1939-node', but vehicle_sil_launch gives an "
               "ecu_main executable --j1939-node itself, from the participant's j1939_nodes: remove it from the command "
               "and write j1939_nodes: [{ network: VehicleCAN, from: 0x00, address: 0x01 }] for this participant\n"
               "E 10 launcher-argument: participant 'B': the command has '--j1939-node=BodyCAN=address:254,identity_number:2', "
               "but vehicle_sil_launch gives an ecu_main executable --j1939-node itself, from the participant's "
               "j1939_nodes: remove it from the command and write j1939_nodes: [{ network: BodyCAN, address: 254, "
               "identity_number: 2 }] for this participant\n"
               "E 12 launcher-argument: participant 'C': the command has '--j1939-node', but vehicle_sil_launch gives an "
               "ecu_main executable --j1939-node itself, from the participant's j1939_nodes: remove it from the command "
               "and write the node in j1939_nodes for this participant\n");
    // Any other command may have it, unless the entry has j1939_nodes that change something too:
    // then run would give a node twice.
    const std::string remote = "  - name: Remote\n"                                                                      // 7
                               "    command: [remote, --registry-uri, \"{registry_uri}\", --j1939-node, \"VehicleCAN=address:0x05\"]\n" // 8
                               "    describe: false\n";                                                                  // 9
    check_text(read(with_networks(remote)), "");
    const ConfigRead both = read_config(with_networks(remote + "    j1939_nodes: [{ network: VehicleCAN, from: 0x00, address: 0x01 }]\n")); // 10
    check_text(lines(both.findings),
               "E 8 launcher-argument: participant 'Remote': the command has --j1939-node, and the participant has "
               "j1939_nodes, for which vehicle_sil_launch gives the executable --j1939-node itself (--j1939-node "
               "VehicleCAN=from:0x00,address:0x01): remove --j1939-node from the command and write every node in "
               "j1939_nodes\n"
               "N 10 j1939-node-not-checked: participant 'Remote': j1939_nodes of a participant that is not described "
               "(describe: false) is not checked; run starts it with --j1939-node VehicleCAN=from:0x00,address:0x01, which "
               "only an ecu_main executable with \"j1939-node\" in its features takes\n");
    CHECK(!both.config->participants[0].valid);
    // An entry that changes nothing asks for no argument: no error and no note.
    check_text(read(with_networks(remote + "    j1939_nodes: [{ network: VehicleCAN, from: 0x00 }]\n")), "");
}

TEST_CASE(the_note_of_j1939_nodes_that_are_not_checked_says_what_run_gives)
{
    const std::string file = with_networks("  - name: Far\n"                                                              // 7
                                           "    command: [far, \"{registry_uri}\"]\n"                                     // 8
                                           "    describe: false\n"                                                        // 9
                                           "    j1939_nodes: [{ network: VehicleCAN, from: 0x00, address: 0x01 }, "
                                           "{ network: BodyCAN, identity_number: 3 }]\n"                                  // 10
                                           "  - name: Near\n"                                                             // 11
                                           "    command: near\n",                                                         // 12
                                           "variants:\n"                                                                  // 13
                                           "  common: {}\n"                                                               // 14
                                           "  none: { participants: { Far: { j1939_nodes: [{ network: VehicleCAN, from: 0 }, "
                                           "{ network: BodyCAN }] } } }\n");                                               // 15
    EnabledOverrides choice;
    choice.variant = "common";
    const ConfigRead common = read_config(file, choice);
    check_text(lines(common.findings),
               "N 10 j1939-node-not-checked: participant 'Far': j1939_nodes of a participant that is not described "
               "(describe: false) is not checked; run starts it with --j1939-node VehicleCAN=from:0x00,address:0x01 "
               "--j1939-node BodyCAN=identity_number:3, which only an ecu_main executable with \"j1939-node\" in its "
               "features takes\n");
    // The variant takes every entry away: nothing is given, and nothing is noted.
    choice.variant = "none";
    check_text(lines(read_config(file, choice).findings), "");
    // A participant that is left out is given nothing either.
    choice.variant = "common";
    choice.disable = {"Far"};
    check_text(lines(read_config(file, choice).findings), "");
}

TEST_CASE(j1939_node_arguments_leave_out_entries_that_change_nothing)
{
    ParticipantConfig p;
    p.name = "Engine2";
    p.j1939_nodes = {
        J1939NodeEntry{{"VehicleCAN", j1939::NodeOverride{0x0A, 0xFE, 2097151, 31, 7}}, 3},
        J1939NodeEntry{{"BodyCAN", j1939::NodeOverride{0x21, {}, {}, {}, {}}}, 4},
        J1939NodeEntry{{"Body CAN", j1939::NodeOverride{{}, 0x00, {}, {}, {}}}, 5},
        J1939NodeEntry{{"Other", j1939::NodeOverride{{}, {}, {}, {}, 0}}, 6},
    };
    // The settings in their fixed order: an address as 0x.. (254 in decimal), the others in decimal.
    check_text(words(j1939_node_arguments(p)),
               "[--j1939-node][VehicleCAN=from:0x0A,address:254,identity_number:2097151,function_instance:31,ecu_instance:7]"
               "[--j1939-node][Body CAN=address:0x00][--j1939-node][Other=ecu_instance:0]");
    // What an executable must say it takes: the settings of the entries that are given.
    const std::vector<J1939NodeFeatureNeed> needs = required_j1939_node_features(p);
    CHECK(needs.size() == 1 && needs[0].feature == "j1939-node" && needs[0].setting == "from" && needs[0].line == 3);
    const J1939NodeSettingFeature table[] = {{"from", "j1939-node"}, {"ecu_instance", "j1939-node.ecu_instance"}};
    const std::vector<J1939NodeFeatureNeed> more = required_j1939_node_features(p, table);
    CHECK(more.size() == 2 && more[1].feature == "j1939-node.ecu_instance" && more[1].setting == "ecu_instance" &&
          more[1].line == 3);
    p.j1939_nodes.erase(p.j1939_nodes.begin());
    CHECK(required_j1939_node_features(p, table).back().line == 6);
    check_text(j1939_node_yaml(p.j1939_nodes[0]), "{ network: BodyCAN, from: 0x21 }");
}

TEST_CASE(j1939_nodes_have_ascii_names_where_the_arguments_come_in_the_code_page)
{
    // ecu_main on Windows refuses a --j1939-node that is not ASCII (its arguments are not UTF-8
    // there), so the launcher of a Windows build says it before anything is started.
    const std::string file = "vehicle_sil_simulation: 1\nnetworks:\n  - { name: \xE8\xBB\x8A\xE4\xB8\xA1""CAN, kind: can }\n"
                             "participants:\n  - name: A\n    command: a\n"
                             "    j1939_nodes: [{ network: \xE8\xBB\x8A\xE4\xB8\xA1""CAN, address: 1 }]\n"; // 7
    const ConfigRead r = read_config(file);
#ifdef _WIN32
    check_text(lines(r.findings),
               "E 7 j1939-node-not-ascii: participant 'A': j1939_nodes (line 7) names the network '\xE8\xBB\x8A\xE4\xB8\xA1"
               "CAN', which has characters that are not ASCII, which an ECU executable on Windows does not take with "
               "--j1939-node (its arguments arrive in the code page, not in UTF-8): use ASCII names for a network with "
               "j1939_nodes (change the name in networks)\n");
    CHECK(!r.config->participants[0].valid);
#else
    check_text(lines(r.findings), "");
    CHECK(r.config->participants[0].valid);
    check_text(words(j1939_node_arguments(r.config->participants[0])),
               "[--j1939-node][\xE8\xBB\x8A\xE4\xB8\xA1""CAN=address:0x01]");
#endif
}
