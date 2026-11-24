// SPDX-License-Identifier: MIT
//
// The outputs of the check (check_participants() and its report): the text for people and the JSON,
// with every table and every key, and check_file() with a disabled participant. The rules
// themselves are in tests/config/test_check.cpp.
#include "check_setup.hpp"
#include "launch_test_util.hpp"

#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/launch/error_result.hpp"
#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/process/paths.hpp"
#include "vehicle_sil/scenario/scenario.hpp"
#include "vehicle_sil/scenario/scenario_check.hpp"
#include "usecases/sha256.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using namespace vehicle_sil::scenario;
using namespace vehicle_sil::config;
using vehicle_sil::config::test::check_text;
using vehicle_sil::config::test::lines;
using namespace vehicle_sil::config::test::check_setup;

TEST_CASE(variables_that_are_not_offered_are_a_note_and_another_version_of_the_messages_a_warning)
{
    // Variables, offered in the version of the messages this launcher speaks: nothing to say.
    Setup s = good_pair();
    s.results[0].ecu->description.variables = {variable("x")};
    // No variables: nothing to offer, whatever the executable says.
    s.results[1].ecu->description.control.reset();
    s.results[0].ecu->description.control = ControlOffer{"silkit", 2, 1};
    check_text(s.check(), "");
    CHECK_EQ(control::wire::kVersion, std::uint16_t{2});
    CHECK_EQ(control::wire::kRevision, std::uint8_t{1});
    // An older revision of the same version: the variables work, without what the newer added
    // (a note, the check passes); a newer one: nothing to say (this side skips what it added).
    s.results[0].ecu->description.control = ControlOffer{"silkit", 2, 0};
    check_text(s.check(), "N 6 control-link-revision: participant 'Engine': it offers its variables as silkit messages of "
                          "revision 0, older than revision 1 of this vehicle_sil_launch: its variables are read and written "
                          "as usual, but none of them can report its availability, so every one of them looks Active: if "
                          "it should report one, build the executable with this vehicle-sil and declare those variables "
                          "with VariableOptions::availability\n");
    s.results[0].ecu->description.control = ControlOffer{"silkit", 2, 2};
    check_text(s.check(), "");
    s.results[0].ecu->description.control = ControlOffer{"silkit", 2, 1};
    // Variables that the executable does not offer (--no-control, an earlier vehicle-sil).
    s.results[1].ecu->description.variables = {variable("y")};
    check_text(s.check(), "N 9 control-link-not-offered: participant 'Dashboard': it describes variables, but does not offer "
                          "them to the simulation (--no-control, or an executable of a vehicle-sil that does not yet): "
                          "vehicle_sil_launch run cannot read or write them\n");
    // Offered in another version: known before anything is started, and the fix depends on which
    // side is the older one.
    s.results[1].ecu->description.control = ControlOffer{"silkit", 1};
    check_text(s.check(), "W 9 control-link-version: participant 'Dashboard': it offers its variables as silkit messages of "
                          "version 1, and this vehicle_sil_launch speaks version 2, so vehicle_sil_launch run cannot read or "
                          "write them: the ECU is of an older vehicle-sil; build it again with the vehicle-sil of this "
                          "vehicle_sil_launch\n");
    s.results[1].ecu->description.control = ControlOffer{"silkit", 3};
    check_text(s.check(), "W 9 control-link-version: participant 'Dashboard': it offers its variables as silkit messages of "
                          "version 3, and this vehicle_sil_launch speaks version 2, so vehicle_sil_launch run cannot read or "
                          "write them: the ECU is of a newer vehicle-sil; use a vehicle_sil_launch of its vehicle-sil or a "
                          "newer one\n");
    // ... or in another transport, whatever its version: not a matter of versions.
    for (const int version : {1, 2, 3})
    {
        s.results[1].ecu->description.control = ControlOffer{"other", version};
        check_text(s.check(), "W 9 control-link-version: participant 'Dashboard': it offers its variables over the transport "
                              "other, and this vehicle_sil_launch reads and writes variables over silkit only, so "
                              "vehicle_sil_launch run cannot read or write them: make the executable offer them over silkit "
                              "(as ecu_main does), or use a vehicle_sil_launch that knows the transport other\n");
    }
    s.results[0].ecu->description.control = ControlOffer{"silkit", 2, 1};
    s.results[1].ecu->description.control = ControlOffer{"silkit", 3};
    // The check's JSON says what each executable offers.
    Checked r;
    r.file = "sim.yaml";
    r.config = s.config;
    r.participants = s.results;
    const std::string json = format_json(r);
    // The check's JSON says the version and the revision (0: an executable that did not say it).
    CHECK(json.find("      \"control\": {\n        \"transport\": \"silkit\",\n        \"wire_version\": 2,\n"
                    "        \"wire_revision\": 1\n      }\n") != std::string::npos);
    CHECK(json.find("\"wire_version\": 3,\n        \"wire_revision\": 0\n") != std::string::npos);
    // A disabled participant is not in the simulation: nothing about its variables.
    s.config.participants[1].enabled = false;
    CHECK(s.check().find("control-link") == std::string::npos);
}

TEST_CASE(a_scenario_may_write_what_the_hub_takes_of_a_converted_variable)
{
    // The seen ends of a converted range are products, and may be off the number as written by a
    // rounding: 4.35 * 100 is 434.99999999999994, 3 * 0.1 is 0.30000000000000004. The scenario's
    // check asks the map, as the run's hub does.
    control::VariableInfo bar = variable("pressure_bar");
    bar.min = 0.0;
    bar.max = 4.35;
    control::VariableInfo raw = variable("raw");
    raw.type = control::ValueType::Integer;
    raw.min = 3;
    raw.max = 10;
    Setup s = good_pair();
    s.results[0].ecu->description.variables = {bar, raw};
    s.results[0].ecu->variables_reported = true;
    s.results[0].ecu->description.control = ControlOffer{"silkit", static_cast<int>(control::wire::kVersion),
                                                                static_cast<int>(control::wire::kRevision)};
    control::VariableMapping pressure;
    pressure.name = "pressure";
    pressure.from = "pressure_bar";
    pressure.scale = 100.0;
    control::VariableMapping level;
    level.name = "level";
    level.from = "raw";
    level.scale = 0.1;
    s.config.participants[0].variables = {pressure, level};
    s.config.participants[0].variable_lines = {20, 21};
    check_text(s.check(), "");
    Checked report;
    report.file = "x.yaml";
    report.config = s.config;
    report.participants = s.results;
    const ScenarioCatalog catalog = scenario_catalog(report);
    const auto errors = [&catalog](const std::string& steps) {
        return lines(read_scenario("vehicle_sil_scenario: 1\nsteps:\n" + steps, catalog).findings);
    };
    check_text(errors("  - set: { Engine.pressure: 435, Engine.level: 0.3 }\n"
                      "  - set: { Engine.pressure: 0, Engine.level: 1 }\n"
                      "  - ramp: { Engine.pressure: { from: 435, to: 0, over_ms: 20 } }\n"
                      "  - ramp: { Engine.level: { from: 1, to: 0.3, over_ms: 20 } }\n"
                      // The executable's integer is the nearest one: 0.26 is 3, as the hub writes it.
                      "  - set: { Engine.level: 0.26 }\n"),
               "");
    // Beyond the ends: refused, with the range in the numbers one would write.
    check_text(errors("  - set: { Engine.pressure: 435.01 }\n"
                      "  - set: { Engine.level: 0.24 }\n"
                      "  - ramp: { Engine.pressure: { to: -0.01, over_ms: 20 } }\n"
                      "  - ramp: { Engine.level: { from: 1.06, to: 1, over_ms: 20 } }\n"),
               "E 3 scenario-value: the value of Engine.pressure (real) is 435.01, outside its range (from 0 to 435)\n"
               "E 4 scenario-value: the value of Engine.level (real) is 0.24, outside its range (from 0.3 to 1)\n"
               "E 5 scenario-value: to of the ramp of Engine.pressure is -0.01, outside its range (from 0 to 435)\n"
               "E 6 scenario-value: from of the ramp of Engine.level is 1.06, outside its range (from 0.3 to 1)\n");
}

namespace {

/// A report with a described, a failed, a skipped and an unchecked participant.
Checked mixed_report()
{
    Setup s = good_pair();
    s.network("Body", NetworkKind::Lin, 5);

    s.config.participants.push_back(participant("Cluster \xC3\xA9", 12));
    s.results.push_back(ParticipantResult{ParticipantStatus::Failed, std::nullopt});
    s.config.participants.push_back(participant("", 15));
    s.results.push_back(ParticipantResult{ParticipantStatus::Skipped, std::nullopt});
    s.unchecked("Logger", 18);
    s.results[0].ecu->description.networks.push_back(NetworkDescription{"Body", NetworkUse::LinMaster, {}});
    // On Body as CAN: not on the LIN network Body (the tables count as the rules do).
    s.results[1].ecu->description.networks.push_back(NetworkDescription{"Body", NetworkUse::Raw, {}});
    Checked r;
    r.file = "dir/simulation.yaml";
    r.stop_at_ms = 1000;
    r.config = s.config;
    r.participants = s.results;
    r.findings = {Finding{Severity::Error, "describe-failed", "participant 'Cluster \xC3\xA9': it failed", 13, "Cluster \xC3\xA9", "",
                          "the end of its standard error:\nboom\nbang"},
                  Finding{Severity::Error, "missing-key", "participants[3] has no name", 15, "", ""},
                  Finding{Severity::Warning, "single-participant", "network 'Body': only Engine", 5, "", "Body"},
                  Finding{Severity::Warning, "file-level", "no line", 0, "", ""},
                  Finding{Severity::Note, "not-checked", "participant 'Logger': not checked", 18, "Logger", ""}};
    return r;
}

} // namespace

TEST_CASE(the_text_report_shows_tables_then_the_findings)
{
    check_text(format_text(mixed_report()),
               "dir/simulation.yaml: 5 participants, 2 networks\n"
               "\n"
               "participant      step   networks\n"
               "Engine           10 ms  J1939_CAN (j1939 0x00), Body (lin_master)\n"
               "Dashboard        5 ms   J1939_CAN (j1939 0x28), Body (raw)\n"
               "Cluster \xC3\xA9        -      no description (see the errors)\n"
               "participants[3]  -      not read (see the errors)\n"
               "Logger           -      not checked (describe: false)\n"
               "\n"
               "network    kind  participants\n"
               "J1939_CAN  can   Engine, Dashboard\n"
               "Body       lin   Engine\n"
               "\n"
               "error: dir/simulation.yaml:13: participant 'Cluster \xC3\xA9': it failed [describe-failed]\n"
               "    the end of its standard error:\n"
               "    boom\n"
               "    bang\n"
               "error: dir/simulation.yaml:15: participants[3] has no name [missing-key]\n"
               "warning: dir/simulation.yaml:5: network 'Body': only Engine [single-participant]\n"
               "warning: dir/simulation.yaml: no line [file-level]\n"
               "note: dir/simulation.yaml:18: participant 'Logger': not checked [not-checked]\n"
               "\n"
               "failed: 2 errors, 2 warnings, 1 note\n");
}

TEST_CASE(the_text_report_summary_line)
{
    Checked r = mixed_report();
    r.findings.erase(r.findings.begin(), r.findings.begin() + 2);
    const std::string passed = format_text(r);
    check_text(passed.substr(passed.rfind('\n', passed.size() - 2) + 1), "passed: 0 errors, 2 warnings, 1 note\n");
    r.strict = true;
    const std::string strict = format_text(r);
    check_text(strict.substr(strict.rfind('\n', strict.size() - 2) + 1),
               "failed: 0 errors, 2 warnings, 1 note (--strict)\n");
    // A note does not fail --strict.
    r.findings.erase(r.findings.begin(), r.findings.begin() + 2);
    CHECK(r.passed());
    const std::string noted = format_text(r);
    check_text(noted.substr(noted.rfind('\n', noted.size() - 2) + 1), "passed: 0 errors, 0 warnings, 1 note\n");
    r.findings.clear();
    const std::string clean = format_text(r);
    check_text(clean.substr(clean.rfind("\n\n") + 2), "passed: 0 errors, 0 warnings, 0 notes\n");
    Checked unreadable;
    unreadable.file = "x.yaml";
    unreadable.findings = {Finding{Severity::Error, "syntax", "syntax error: missing terminating ]", 2, "", "",
                                   "2:9: networks: [a\n            ^"}};
    check_text(format_text(unreadable), "error: x.yaml:2: syntax error: missing terminating ] [syntax]\n"
                                        "    2:9: networks: [a\n"
                                        "                ^\n"
                                        "\n"
                                        "cannot read x.yaml: 1 error, 0 warnings, 0 notes\n");
}

TEST_CASE(the_json_report_has_every_key_in_a_fixed_order)
{
    Checked r = mixed_report();
    r.config->participants.resize(3);
    r.participants.resize(3);
    r.config->participants[1].command = {"dash board", "--x"};
    r.findings = {r.findings[0], r.findings[1], r.findings[4]};
    // Engine's variables as its description has them; Dashboard says it has none; Cluster failed.
    control::VariableInfo mode;
    mode.name = "engine.mode";
    mode.type = control::ValueType::Enum;
    mode.causality = control::Causality::Parameter;
    mode.min = 0;
    mode.description = "how it runs";
    mode.items = {{"off", 0}, {"on", 1}};
    r.participants[0].ecu->description.variables = {mode};
    r.participants[0].ecu->variables_reported = true;
    r.participants[1].ecu->variables_reported = true;
    const std::string v = version();
    check_text(format_json(r), R"({
  "vehicle_sil_check": 1,
  "vehicle_sil": ")" + v + R"(",
  "file": "dir/simulation.yaml",
  "result": "failed",
  "strict": false,
  "errors": 2,
  "warnings": 0,
  "notes": 1,
  "stop_at_ms": 1000,
  "time_step_ms": null,
  "start_timeout_ms": null,
  "stop_timeout_ms": null,
  "registry_uri": null,
  "commands": null,
  "participants": [
    {
      "name": "Engine",
      "line": 6,
      "command": [
        "Engine"
      ],
      "describe": true,
      "required": true,
      "enabled": true,
      "manual": false,
      "role": "ecu",
      "step_ms": 10,
      "status": "described",
      "reported": {
        "vehicle_sil": "1.0.0",
        "name": "Engine",
        "step_ns": 10000000,
        "networks": [
          {
            "name": "J1939_CAN",
            "kind": "can",
            "use": "j1939",
            "nodes": [
              {
                "address": 0,
                "j1939_name": "0x00000000FE000001",
                "arbitrary_address_capable": false
              }
            ],
            "mac": null
          },
          {
            "name": "Body",
            "kind": "lin",
            "use": "lin_master",
            "nodes": [],
            "mac": null
          }
        ]
      },
      "variables": [
        {
          "name": "engine.mode",
          "type": "enum",
          "causality": "parameter",
          "unit": "",
          "min": 0,
          "max": null,
          "step": null,
          "description": "how it runs",
          "items": [
            {
              "name": "off",
              "value": 0
            },
            {
              "name": "on",
              "value": 1
            }
          ],
          "builtin": false
        }
      ],
      "network_map": null,
      "j1939_nodes": null,
      "control": null
    },
    {
      "name": "Dashboard",
      "line": 9,
      "command": [
        "dash board",
        "--x"
      ],
      "describe": true,
      "required": true,
      "enabled": true,
      "manual": false,
      "role": "ecu",
      "step_ms": null,
      "status": "described",
      "reported": {
        "vehicle_sil": "1.0.0",
        "name": "Dashboard",
        "step_ns": 5000000,
        "networks": [
          {
            "name": "J1939_CAN",
            "kind": "can",
            "use": "j1939",
            "nodes": [
              {
                "address": 40,
                "j1939_name": null,
                "arbitrary_address_capable": false
              }
            ],
            "mac": null
          },
          {
            "name": "Body",
            "kind": "can",
            "use": "raw",
            "nodes": [],
            "mac": null
          }
        ]
      },
      "variables": [],
      "network_map": null,
      "j1939_nodes": null,
      "control": null
    },
    {
      "name": "Cluster é",
      "line": 12,
      "command": [
        "Cluster é"
      ],
      "describe": true,
      "required": true,
      "enabled": true,
      "manual": false,
      "role": "ecu",
      "step_ms": null,
      "status": "failed",
      "reported": null,
      "variables": null,
      "network_map": null,
      "j1939_nodes": null,
      "control": null
    }
  ],
  "networks": [
    {
      "name": "J1939_CAN",
      "kind": "can",
      "line": 3,
      "participants": [
        "Engine",
        "Dashboard"
      ],
      "outside": null
    },
    {
      "name": "Body",
      "kind": "lin",
      "line": 5,
      "participants": [
        "Engine"
      ],
      "outside": null
    }
  ],
  "time_lock": {
    "locked": false,
    "because": []
  },
  "findings": [
    {
      "severity": "error",
      "code": "describe-failed",
      "file": "dir/simulation.yaml",
      "line": 13,
      "participant": "Cluster é",
      "network": null,
      "message": "participant 'Cluster é': it failed",
      "detail": "the end of its standard error:\nboom\nbang"
    },
    {
      "severity": "error",
      "code": "missing-key",
      "file": "dir/simulation.yaml",
      "line": 15,
      "participant": null,
      "network": null,
      "message": "participants[3] has no name",
      "detail": null
    },
    {
      "severity": "note",
      "code": "not-checked",
      "file": "dir/simulation.yaml",
      "line": 18,
      "participant": "Logger",
      "network": null,
      "message": "participant 'Logger': not checked",
      "detail": null
    }
  ],
  "file_sha256": null,
  "hot_eligibility": {
    "eligible": true,
    "reason": null,
    "message": null
  }
}
)");
    // A description without "variables" (an older ECU): not known.
    r.participants[1].ecu->variables_reported = false;
    CHECK(format_json(r).find("\n        ]\n      },\n      \"variables\": null,\n      \"network_map\": null,\n      \"j1939_nodes\": null,\n      \"control\": null\n    },\n    {\n      \"name\": \"Cluster") !=
          std::string::npos);
}

TEST_CASE(the_json_report_has_the_time_lock_and_what_it_is_because_of)
{
    Checked r;
    r.file = "x.yaml";
    r.config = SimulationConfig{};
    r.time_lock = TimeLock{true, {"ETH", "CAN1"}, {"Gateway"}};
    CHECK(format_json(r).find("  \"networks\": [],\n"
                              "  \"time_lock\": {\n"
                              "    \"locked\": true,\n"
                              "    \"because\": [\n"
                              "      \"ETH\",\n"
                              "      \"CAN1\",\n"
                              "      \"Gateway\"\n"
                              "    ]\n"
                              "  },\n"
                              "  \"findings\": [],\n") != std::string::npos);
    // Not locked (--time-policy free), but connected outside: the names all the same.
    r.time_lock.locked = false;
    CHECK(format_json(r).find("  \"time_lock\": {\n    \"locked\": false,\n    \"because\": [\n      \"ETH\",\n") != std::string::npos);
}

TEST_CASE(the_json_report_of_a_file_that_cannot_be_read)
{
    Checked r;
    r.file = "x.yaml";
    r.findings = {Finding{Severity::Error, "file", "the file does not exist: check the path", 0, "", ""}};
    check_text(format_json(r), R"({
  "vehicle_sil_check": 1,
  "vehicle_sil": ")" + std::string(version()) +
                                   R"(",
  "file": "x.yaml",
  "result": "unreadable",
  "strict": false,
  "errors": 1,
  "warnings": 0,
  "notes": 0,
  "stop_at_ms": null,
  "time_step_ms": null,
  "start_timeout_ms": null,
  "stop_timeout_ms": null,
  "registry_uri": null,
  "commands": null,
  "participants": [],
  "networks": [],
  "time_lock": null,
  "findings": [
    {
      "severity": "error",
      "code": "file",
      "file": "x.yaml",
      "line": null,
      "participant": null,
      "network": null,
      "message": "the file does not exist: check the path",
      "detail": null
    }
  ],
  "file_sha256": null,
  "hot_eligibility": {
    "eligible": false,
    "reason": "config-unreadable",
    "message": "the file does not exist: check the path"
  }
}
)");
}

TEST_CASE(the_json_report_of_a_wrong_command_line)
{
    Checked r;
    r.usage = true;
    r.findings = {Finding{Severity::Error, "usage", "unknown option --bogus", 0, "", ""}};
    CHECK(!r.passed());
    const std::string json = format_json(r);
    check_text(json.substr(json.find("\n  \"file\"")), R"(
  "file": null,
  "result": "usage",
  "strict": false,
  "errors": 1,
  "warnings": 0,
  "notes": 0,
  "stop_at_ms": null,
  "time_step_ms": null,
  "start_timeout_ms": null,
  "stop_timeout_ms": null,
  "registry_uri": null,
  "commands": null,
  "participants": [],
  "networks": [],
  "time_lock": null,
  "findings": [
    {
      "severity": "error",
      "code": "usage",
      "file": null,
      "line": null,
      "participant": null,
      "network": null,
      "message": "unknown option --bogus",
      "detail": null
    }
  ],
  "file_sha256": null,
  "hot_eligibility": {
    "eligible": false,
    "reason": "usage",
    "message": "unknown option --bogus"
  }
}
)");
}

TEST_CASE(the_json_report_writes_null_for_what_is_not_there)
{
    SimulationConfig config;
    config.participants.push_back(participant("", 4));
    Checked r;
    r.file = "x.yaml";
    r.config = config;
    r.participants = {ParticipantResult{ParticipantStatus::Skipped, std::nullopt}};
    const std::string json = format_json(r);
    CHECK(json.find("\n      \"name\": null,\n      \"line\": 4,\n") != std::string::npos);
    r.participants = {ParticipantResult{ParticipantStatus::Described, ecu("", 10ms, {}, "")}};
    const std::string reported = format_json(r);
    CHECK(reported.find("\n      \"reported\": {\n        \"vehicle_sil\": null,\n        \"name\": null,\n") != std::string::npos);
}

TEST_CASE(the_reports_show_the_mac_address_of_an_ethernet_network)
{
    Setup s;
    s.network("Backbone", NetworkKind::Ethernet, 3)
        .described(participant("Camera", 6),
                   ecu("Camera", 5ms,
                       {NetworkDescription{"Backbone", NetworkUse::Ethernet, {}, eth::MacAddress{{0x02, 0, 0, 0, 0xAB, 0x01}}}}))
        .described(participant("Display", 9), ecu("Display", 5ms, {NetworkDescription{"Backbone", NetworkUse::Ethernet, {}}}));
    Checked r;
    r.file = "x.yaml";
    r.config = s.config;
    r.participants = s.results;
    const std::string text = format_text(r);
    CHECK(text.find("Camera       5 ms  Backbone (ethernet 02:00:00:00:ab:01)\n"
                    "Display      5 ms  Backbone (ethernet)\n") != std::string::npos);
    CHECK(text.find("network   kind      participants\n"
                    "Backbone  ethernet  Camera, Display\n") != std::string::npos);
    const std::string json = format_json(r);
    CHECK(json.find("\"kind\": \"ethernet\",\n            \"use\": \"ethernet\",\n            \"nodes\": [],\n"
                    "            \"mac\": \"02:00:00:00:ab:01\"\n") != std::string::npos);
    CHECK(json.find("\"nodes\": [],\n            \"mac\": null\n") != std::string::npos);
}

TEST_CASE(the_json_report_shows_the_seen_variables_and_the_map_of_an_entry_that_has_one)
{
    Checked r = mixed_report();
    control::VariableInfo rpm;
    rpm.name = "rpm_x10";
    rpm.type = control::ValueType::Integer;
    rpm.causality = control::Causality::Output;
    rpm.unit = "0.1 rpm";
    rpm.max = 80000;
    control::VariableInfo gear;
    gear.name = "gear_raw";
    gear.type = control::ValueType::Integer;
    r.participants[0].ecu->description.variables = {rpm, gear};
    r.participants[0].ecu->variables_reported = true;
    control::VariableMapping speed;
    speed.name = "engine.speed";
    speed.from = "rpm_x10";
    speed.scale = 0.5;
    speed.unit = "rpm";
    control::VariableMapping names;
    names.name = "engine.gear";
    names.from = "gear_raw";
    names.map = std::vector<control::EnumItem>{{"P", 0}, {"D", 3}};
    control::VariableMapping turbo;
    turbo.name = "engine.turbo";
    turbo.absent = true;
    r.config->participants[0].variables = {speed, names, turbo};
    r.config->participants[0].variable_lines = {7, 8, 9};
    const std::string json = format_json(r);
    const auto from = json.find("      \"variables\": [");
    const auto to = json.find("      \"control\":", from);
    CHECK(from != std::string::npos && to != std::string::npos);
    check_text(json.substr(from, to - from),
               R"(      "variables": [
        {
          "name": "engine.speed",
          "type": "real",
          "causality": "output",
          "unit": "rpm",
          "min": null,
          "max": 40000,
          "step": null,
          "description": "",
          "items": [],
          "builtin": false
        },
        {
          "name": "engine.gear",
          "type": "enum",
          "causality": "input",
          "unit": "",
          "min": null,
          "max": null,
          "step": null,
          "description": "",
          "items": [
            {
              "name": "P",
              "value": 0
            },
            {
              "name": "D",
              "value": 3
            }
          ],
          "builtin": false
        }
      ],
      "variable_map": [
        {
          "name": "engine.speed",
          "line": 7,
          "from": "rpm_x10",
          "absent": false,
          "scale": 0.5,
          "offset": 0,
          "unit": "rpm",
          "map": null
        },
        {
          "name": "engine.gear",
          "line": 8,
          "from": "gear_raw",
          "absent": false,
          "scale": null,
          "offset": null,
          "unit": null,
          "map": [
            {
              "name": "P",
              "value": 0
            },
            {
              "name": "D",
              "value": 3
            }
          ]
        },
        {
          "name": "engine.turbo",
          "line": 9,
          "from": null,
          "absent": true,
          "scale": null,
          "offset": null,
          "unit": null,
          "map": null
        }
      ],
      "network_map": null,
      "j1939_nodes": null,
)");
    // An entry without variables has no "variable_map" (the key says that the entry has a map).
    r.config->participants[0].variables.clear();
    CHECK(format_json(r).find("variable_map") == std::string::npos);
}

TEST_CASE(the_json_report_says_whether_the_commands_are_the_same)
{
    Checked r = mixed_report();
    const auto commands = [&r] {
        const std::string json = format_json(r);
        const std::string key = "\n  \"registry_uri\": null,\n  \"commands\": ";
        const auto at = json.find(key);
        if (at == std::string::npos)
        {
            return std::string("(none)");
        }
        const auto from = at + key.size();
        return json.substr(from, json.find('\n', from) - from);
    };
    CHECK(commands() == "null,");
    r.commands = CommandsCompared::Same;
    CHECK(commands() == "\"same\",");
    r.commands = CommandsCompared::Changed;
    CHECK(commands() == "\"changed\",");
    r.commands = CommandsCompared::NotCompared;
    CHECK(commands() == "\"not_compared\",");
    // The text for people has the note only.
    const std::string text = format_text(r);
    r.commands = CommandsCompared::NotAsked;
    CHECK(format_text(r) == text);
}

TEST_CASE(the_reports_show_a_participant_without_a_saved_description)
{
    Setup s = good_pair();
    s.results[1] = ParticipantResult{ParticipantStatus::NotDescribed, std::nullopt};
    Checked r;
    r.file = "sim.yaml";
    r.config = s.config;
    r.participants = s.results;
    r.findings = check_participants(s.config, s.results, "1.0.0");
    CHECK(format_text(r).find("Dashboard    -      not described (see the notes)\n") != std::string::npos);
    CHECK(format_json(r).find("      \"step_ms\": null,\n      \"status\": \"not_described\",\n      \"reported\": null,\n") !=
          std::string::npos);
}

TEST_CASE(the_reports_show_a_disabled_participant)
{
    const Setup s = without_dashboard();
    Checked r;
    r.file = "sim.yaml";
    r.config = s.config;
    r.participants = s.results;
    r.findings = check_participants(s.config, s.results, "1.0.0");
    sort_findings(r.findings);
    check_text(format_text(r),
               "sim.yaml: 2 participants (1 disabled), 1 network\n"
               "\n"
               "participant  step   networks\n"
               "Engine       10 ms  J1939_CAN (j1939 0x00)\n"
               "Dashboard    -      disabled\n"
               "\n"
               "network    kind  participants\n"
               "J1939_CAN  can   Engine\n"
               "\n"
               "warning: sim.yaml:3: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add "
               "the other participants of this network, or remove it; if it talks only to equipment outside, connect it outside to run it (external says what passes) [single-participant]\n"
               "note: sim.yaml:9: participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked "
               "nor started, and the other findings are about the simulation without it [participant-disabled]\n"
               "\n"
               "passed: 0 errors, 1 warning, 1 note\n");
    // The note does not fail --strict (the warning does).
    r.strict = true;
    r.findings.erase(r.findings.begin());
    CHECK(r.passed());
    const std::string json = format_json(r);
    CHECK(json.find("      \"required\": true,\n      \"enabled\": true,\n      \"manual\": false,\n"
                    "      \"role\": \"ecu\",\n      \"step_ms\": 10,\n      \"status\": \"described\",\n") != std::string::npos);
    CHECK(json.find("      \"required\": true,\n      \"enabled\": false,\n      \"manual\": false,\n"
                    "      \"role\": \"ecu\",\n      \"step_ms\": null,\n      \"status\": \"disabled\",\n"
                    "      \"reported\": null,\n      \"variables\": null,\n      \"network_map\": null,\n      \"j1939_nodes\": null,\n      \"control\": null\n") != std::string::npos);
    CHECK(json.find("      \"participants\": [\n        \"Engine\"\n      ],\n      \"outside\": null\n") != std::string::npos);
}

TEST_CASE(the_reports_show_a_role_that_is_not_ecu)
{
    Setup s = good_pair();
    s.described(with_role(participant("Spy", 12), ParticipantRole::Test),
                ecu("Spy", 10ms, {NetworkDescription{"J1939_CAN", NetworkUse::Raw, {}}}));
    s.unchecked("Gateway", 15);
    s.config.participants.back().role = ParticipantRole::Bridge;
    Checked r;
    r.file = "sim.yaml";
    r.config = s.config;
    r.participants = s.results;
    const std::string text = format_text(r);
    CHECK(text.find("Engine                  10 ms  J1939_CAN (j1939 0x00)\n") != std::string::npos);
    CHECK(text.find("Spy (role: test)        10 ms  J1939_CAN (raw)\n") != std::string::npos);
    CHECK(text.find("Gateway (role: bridge)  -      not checked (describe: false)\n") != std::string::npos);
    // The network table names the participant only.
    CHECK(text.find("J1939_CAN  can   Engine, Dashboard, Spy\n") != std::string::npos);
    const std::string json = format_json(r);
    CHECK(json.find("      \"manual\": false,\n      \"role\": \"test\",\n      \"step_ms\": null,\n") != std::string::npos);
    CHECK(json.find("      \"manual\": false,\n      \"role\": \"bridge\",\n      \"step_ms\": null,\n") != std::string::npos);
}

TEST_CASE(check_file_neither_looks_for_nor_runs_a_disabled_executable)
{
    const auto dir = std::filesystem::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "disabled";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = dir / "sim.yaml";
    std::ofstream(file) << "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                           "  - name: Engine\n    command: [missing/engine]\n"
                           "  - name: Ghost\n    command: [missing/ghost]\n    enabled: false\n";
    const vehicle_sil::config::test::InDirectory there(dir); // the files by their names
    RunRequest plain;
    plain.host.config = "sim.yaml";
    const Checked r = check_file(plain);
    CHECK(r.config.has_value());
    CHECK(r.participants.size() == 2 && r.participants[0].status == ParticipantStatus::Failed &&
          r.participants[1].status == ParticipantStatus::Disabled && r.participants[1].executable.empty());
    std::string codes;
    for (const auto& f : r.findings)
    {
        codes += f.code + "(" + f.participant + ") ";
    }
    check_text(codes, "executable(Engine) participant-disabled(Ghost) ");
    // --disable and --enable change the file's choice; a name that the file does not have is a usage error.
    RunRequest swapped = plain;
    swapped.intent.disable = {"Engine"};
    swapped.intent.enable = {"Ghost"};
    const Checked s = check_file(swapped);
    CHECK(!s.usage);
    CHECK(s.participants[0].status == ParticipantStatus::Disabled && s.participants[1].status == ParticipantStatus::Failed);
    RunRequest wrong = plain;
    wrong.intent.disable = {"Gost"};
    const Checked u = check_file(wrong);
    CHECK(u.usage && !u.config && !u.passed());
    check_text(lines(u.findings), "E 0 usage: --disable Gost: the file has no participant with this name (did you mean "
                                  "'Ghost'?); its participants are Engine, Ghost\n");
    // The JSON of a usage error names the file.
    const std::string json = format_json(u);
    CHECK(json.find("\n  \"file\": \"sim.yaml\",\n  \"result\": \"usage\",\n") != std::string::npos);
    // A name given to both is a usage error before the file is read (here: there is none).
    RunRequest both;
    both.host.config = "none.yaml";
    both.intent.disable = {"Ghost"};
    both.intent.enable = {"Ghost"};
    const Checked b = check_file(both);
    CHECK(b.usage);
    check_text(lines(b.findings), "E 0 usage: --disable and --enable both name Ghost: give one of them\n");
}

TEST_CASE(check_file_a_disabled_entry_with_errors_and_an_unreadable_enabled)
{
    const auto dir = std::filesystem::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "disabled-errors";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = dir / "sim.yaml";
    // Ghost is disabled and has no command; Maybe's enabled cannot be read: neither is looked for.
    std::ofstream(file) << "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                           "  - name: Engine\n    command: [missing/engine]\n"
                           "  - name: Ghost\n    enabled: false\n"
                           "  - name: Maybe\n    command: [missing/maybe]\n    enabled: no\n";
    RunRequest request;
    request.host.config = process::utf8_from_path(file);
    const Checked r = check_file(request);
    CHECK(r.participants.size() == 3 && r.participants[1].status == ParticipantStatus::Disabled &&
          r.participants[2].status == ParticipantStatus::Skipped);
    std::string codes;
    for (const auto& f : r.findings)
    {
        codes += f.code + "(" + f.participant + ") ";
    }
    check_text(codes, "executable(Engine) missing-key(Ghost) value(Maybe) participant-disabled(Ghost) ");
    // The JSON says disabled for Ghost.
    CHECK(format_json(r).find("      \"enabled\": false,\n      \"manual\": false,\n      \"role\": \"ecu\",\n      \"step_ms\": null,\n      \"status\": \"disabled\",\n") !=
          std::string::npos);
}

TEST_CASE(a_renamed_network_is_under_the_simulations_name_in_every_rule)
{
    // Engine's code says J1939_CAN, the file says VehicleCAN, which is what Dashboard's code says.
    const auto pair = [](std::vector<std::pair<std::string, std::string>> names) {
        Setup s;
        s.network("VehicleCAN", NetworkKind::Can, 3)
            .described(named(participant("Engine", 6), 8, std::move(names)),
                       taking_map(ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)})})))
            .described(participant("Dashboard", 10), ecu("Dashboard", 10ms, {on_j1939("VehicleCAN", {node(0x28)})}));
        return s;
    };
    Setup s = pair({{"VehicleCAN", "J1939_CAN"}});
    check_text(s.check(), "");
    // seen_networks() is what every rule reads: the simulation's name, with the executable's beside it.
    const std::vector<SeenNetwork> seen = seen_networks(s.config.participants[0], *s.results[0].ecu);
    CHECK(seen.size() == 1 && seen[0].network.name == "VehicleCAN" && seen[0].executable_name == "J1939_CAN" &&
          seen[0].renamed() && seen[0].line == 8 && seen[0].network.nodes.size() == 1);
    // Without the line, the names are two networks, as before.
    CHECK(pair({}).check().find("E 6 unknown-network: participant 'Engine': the executable uses the network 'J1939_CAN' "
                                "(j1939), which is not in networks. Another network of kind can in this simulation is "
                                "'VehicleCAN' (line 3, used by Dashboard): if 'J1939_CAN' is meant to be that bus, say so in "
                                "this file: add network_names: { VehicleCAN: J1939_CAN } to the participant 'Engine' (") == 0);
    // A line whose two names are the same is no line.
    CHECK(pair({{"J1939_CAN", "J1939_CAN"}}).check() == pair({}).check());

    // The rules of the kind: the nodes of both executables are on the one network.
    s.results[1].ecu->description.networks[0].nodes[0].address = 0x00;
    check_text(s.check(), "E 3 address-conflict: network 'VehicleCAN': Engine and Dashboard both ask for the address 0x00 "
                          "(0), and Dashboard has no NAME, so it does not take part in the address claim and keeps sending "
                          "from that address: give each node its own address; " + kNoEntries + "\n");

    // The kind is compared under the simulation's name, and the message has both names.
    Setup k;
    k.network("VehicleCAN", NetworkKind::Can, 3)
        .network("Body", NetworkKind::Lin, 4)
        .described(named(participant("Engine", 6), 8, {{"Body", "J1939_CAN"}}),
                   taking_map(ecu("Engine", 10ms, {on_j1939("J1939_CAN", {})})));
    CHECK(k.check().find("E 6 kind-mismatch: participant 'Engine': the executable uses the network 'Body' ('J1939_CAN' in "
                         "the executable) as can (j1939), but networks says kind: lin (line 4): correct the kind, the line "
                         "of network_names (line 8), or the network in the ECU\n") == 0);
    CHECK(check_participants(k.config, k.results, "1.0.0")[0].network == "Body");

    // One master on a LIN network, whatever the executables call it.
    Setup l;
    l.network("Body", NetworkKind::Lin, 3)
        .described(named(participant("Door", 5), 7, {{"Body", "LIN_A"}}),
                   taking_map(ecu("Door", 10ms, {NetworkDescription{"LIN_A", NetworkUse::LinMaster, {}}})))
        .described(participant("Seat", 9), ecu("Seat", 10ms, {NetworkDescription{"Body", NetworkUse::LinMaster, {}}}));
    check_text(l.check(), "E 3 lin-masters: network 'Body': Door and Seat are both LIN masters, but a LIN network has one "
                          "master: make all but one of them slaves\n");

    // j1939() and can() of one name are both renamed; two names may be swapped.
    Setup w;
    w.network("A", NetworkKind::Can, 3)
        .network("B", NetworkKind::Can, 4)
        .described(named(participant("P", 6), 8, {{"A", "B"}, {"B", "A"}}),
                   taking_map(ecu("P", 10ms, {on_j1939("A", {node(0x10)}), raw("A"), raw("B")})))
        .described(participant("Q", 11), ecu("Q", 10ms, {on_j1939("B", {node(0x10)}), raw("A")}));
    check_text(w.check(), "E 4 address-conflict: network 'B': P and Q both ask for the address 0x10 (16), and P and Q have "
                          "no NAME, so they do not take part in the address claim and keep sending from that address: give "
                          "each node its own address; " + kNoEntries + "\n");
    Checked r;
    r.file = "sim.yaml";
    r.config = w.config;
    r.participants = w.results;
    CHECK(format_text(r).find("P            10 ms  B <- A (j1939 0x10), B <- A (raw), A <- B (raw)\n"
                              "Q            10 ms  B (j1939 0x10), A (raw)\n") != std::string::npos);
}

TEST_CASE(the_reports_show_both_names_of_a_renamed_network)
{
    Setup s;
    s.network("VehicleCAN", NetworkKind::Can, 3)
        .network("Body", NetworkKind::Lin, 4)
        .described(named(participant("Engine", 6), 8, {{"VehicleCAN", "J1939_CAN"}, {"Body", "Body"}}),
                   taking_map(ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)}),
                                                    NetworkDescription{"Body", NetworkUse::LinMaster, {}}})))
        .described(participant("Dashboard", 11), ecu("Dashboard", 10ms, {on_j1939("VehicleCAN", {node(0x28)})}))
        .unchecked("Door", 13);
    s.config.participants[2].network_names = {NetworkName{"Body", "DoorLIN", 15}};
    Checked r;
    r.file = "sim.yaml";
    r.config = s.config;
    r.participants = s.results;
    // For people: the simulation's name, then the executable's; the network table has the simulation's only.
    const std::string text = format_text(r);
    CHECK(text.find("participant  step   networks\n"
                    "Engine       10 ms  VehicleCAN <- J1939_CAN (j1939 0x00), Body (lin_master)\n"
                    "Dashboard    10 ms  VehicleCAN (j1939 0x28)\n"
                    "Door         -      not checked (describe: false)\n"
                    "\n"
                    "network     kind  participants\n"
                    "VehicleCAN  can   Engine, Dashboard\n"
                    "Body        lin   Engine\n") != std::string::npos);
    // For programs: "reported" is what the executable said, under its names; network_map has the
    // lines as written (also the one that renames nothing), null for an entry that has none; the
    // networks' participants are under the simulation's names.
    const std::string json = format_json(r);
    CHECK(json.find("        \"networks\": [\n"
                    "          {\n"
                    "            \"name\": \"J1939_CAN\",\n"
                    "            \"kind\": \"can\",\n"
                    "            \"use\": \"j1939\",\n") != std::string::npos);
    CHECK(json.find("      \"network_map\": [\n"
                    "        {\n"
                    "          \"name\": \"VehicleCAN\",\n"
                    "          \"line\": 8,\n"
                    "          \"from\": \"J1939_CAN\"\n"
                    "        },\n"
                    "        {\n"
                    "          \"name\": \"Body\",\n"
                    "          \"line\": 9,\n"
                    "          \"from\": \"Body\"\n"
                    "        }\n"
                    "      ],\n") != std::string::npos);
    CHECK(json.find("      \"network_map\": [\n"
                    "        {\n"
                    "          \"name\": \"Body\",\n"
                    "          \"line\": 15,\n"
                    "          \"from\": \"DoorLIN\"\n"
                    "        }\n"
                    "      ],\n") != std::string::npos);
    CHECK(json.find("      \"name\": \"VehicleCAN\",\n      \"kind\": \"can\",\n      \"line\": 3,\n      \"participants\": [\n"
                    "        \"Engine\",\n        \"Dashboard\"\n      ]") != std::string::npos);
    // An entry without network_names has the key, null: every participant has it.
    const auto count = [](const std::string& in, const std::string& what) {
        std::size_t n = 0;
        for (auto at = in.find(what); at != std::string::npos; at = in.find(what, at + 1))
        {
            ++n;
        }
        return n;
    };
    CHECK_EQ(count(json, "\"network_map\": ["), std::size_t{2});
    CHECK_EQ(count(json, "\"network_map\""), r.config->participants.size());
    const std::string mixed = format_json(mixed_report());
    CHECK(count(mixed, "\"network_map\": null") > 0);
    CHECK_EQ(count(mixed, "\"network_map\": null"), count(mixed, "\"network_map\""));
}

TEST_CASE(the_reports_show_both_addresses_of_a_changed_node)
{
    Setup s = two_engines({{"J1939_CAN", change(std::nullopt, 0x01, 2)}});
    s.network("Other", NetworkKind::Can, 4);
    s.config.participants[2].network_names = {NetworkName{"J1939_CAN", "ENGINE_CAN", 13}};
    s.results[2].ecu->description.networks = {on_j1939("ENGINE_CAN", {node(0x00, kEngineName), node(j1939::kNullAddress, kArbitraryBit | 4)}),
                                              on_j1939("Other", {node(0x20, kArbitraryBit | 3)})};
    s.config.participants[2].j1939_nodes = {
        J1939NodeEntry{{"J1939_CAN", change(0x00, 0x01, 2)}, 14},
        J1939NodeEntry{{"J1939_CAN", change(0xFE, 0x30)}, 15},
        J1939NodeEntry{{"Other", change(0x20, 0xFE)}, 16},
        J1939NodeEntry{{"Other", change(0x77, 0x01)}, 17},
        J1939NodeEntry{{"Other", change(0x21, std::nullopt)}, 18},
    };
    Checked r;
    r.file = "sim.yaml";
    r.config = s.config;
    r.participants = s.results;
    // For people: the value in the simulation, then the one in the code, no space around the arrow.
    const std::string text = format_text(r);
    CHECK(text.find("Engine2      10 ms  J1939_CAN <- ENGINE_CAN (j1939 0x01<-0x00 0x30<-any), Other (j1939 any<-0x20)\n") !=
          std::string::npos);
    // For programs: the entries as written, and the node each takes (null for none, and for one
    // that changes nothing); "reported" is what the executable said.
    const std::string json = format_json(r);
    CHECK(json.find("      \"j1939_nodes\": [\n"
                    "        {\n"
                    "          \"network\": \"J1939_CAN\",\n"
                    "          \"line\": 14,\n"
                    "          \"from\": 0,\n"
                    "          \"address\": 1,\n"
                    "          \"identity_number\": 2,\n"
                    "          \"function_instance\": null,\n"
                    "          \"ecu_instance\": null,\n"
                    "          \"node\": {\n"
                    "            \"coded_address\": 0,\n"
                    "            \"address\": 1,\n"
                    "            \"j1939_name\": \"0x00000000FE000002\",\n"
                    "            \"arbitrary_address_capable\": false\n"
                    "          }\n"
                    "        },\n"
                    "        {\n"
                    "          \"network\": \"J1939_CAN\",\n"
                    "          \"line\": 15,\n"
                    "          \"from\": 254,\n"
                    "          \"address\": 48,\n"
                    "          \"identity_number\": null,\n"
                    "          \"function_instance\": null,\n"
                    "          \"ecu_instance\": null,\n"
                    "          \"node\": {\n"
                    "            \"coded_address\": 254,\n"
                    "            \"address\": 48,\n"
                    "            \"j1939_name\": \"0x8000000000000004\",\n"
                    "            \"arbitrary_address_capable\": true\n"
                    "          }\n"
                    "        },\n") != std::string::npos);
    CHECK(json.find("          \"network\": \"Other\",\n"
                    "          \"line\": 18,\n"
                    "          \"from\": 33,\n"
                    "          \"address\": null,\n"
                    "          \"identity_number\": null,\n"
                    "          \"function_instance\": null,\n"
                    "          \"ecu_instance\": null,\n"
                    "          \"node\": null\n") != std::string::npos);
    CHECK(json.find("          \"line\": 17,\n"
                    "          \"from\": 119,\n"
                    "          \"address\": 1,\n"
                    "          \"identity_number\": null,\n"
                    "          \"function_instance\": null,\n"
                    "          \"ecu_instance\": null,\n"
                    "          \"node\": null\n") != std::string::npos);
    CHECK(json.find("            \"name\": \"ENGINE_CAN\",\n"
                    "            \"kind\": \"can\",\n"
                    "            \"use\": \"j1939\",\n"
                    "            \"nodes\": [\n"
                    "              {\n"
                    "                \"address\": 0,\n"
                    "                \"j1939_name\": \"0x00000000FE000001\",\n") != std::string::npos);
    // Only the entry that has j1939_nodes has entries; the others have null.
    std::size_t count = 0;
    for (auto at = json.find("\"j1939_nodes\": ["); at != std::string::npos; at = json.find("\"j1939_nodes\": [", at + 1))
    {
        ++count;
    }
    CHECK_EQ(count, std::size_t{1});
    CHECK(format_json(mixed_report()).find("\"j1939_nodes\": [") == std::string::npos);
}

TEST_CASE(a_configuration_without_j1939_nodes_checks_as_before)
{
    // The tables and the JSON of a configuration without j1939_nodes have nothing of it (null in
    // the JSON); the three rules of the addresses and NAMEs end with how to fix it (here: in the
    // ECUs' code).
    const Checked r = mixed_report();
    CHECK(format_text(r).find("<-") == std::string::npos);
    const std::string json = format_json(r);
    CHECK(json.find("\"j1939_nodes\": [") == std::string::npos && json.find("\"j1939_nodes\": null,\n") != std::string::npos);
    for (const ParticipantConfig& p : r.config->participants)
    {
        CHECK(j1939_node_arguments(p).empty());
    }
    Setup s = good_pair();
    s.results[1].ecu->description.networks[0].nodes = {node(0x00, kEngineName)};
    CHECK(s.check().find("each node needs its own NAME; this file cannot change them") != std::string::npos);
}

TEST_CASE(time_policy_names)
{
    CHECK(std::string(to_string(TimePolicy::Realtime)) == "realtime" && std::string(to_string(TimePolicy::Free)) == "free");
    CHECK(time_policy_named("realtime") == TimePolicy::Realtime && time_policy_named("free") == TimePolicy::Free);
    CHECK(!time_policy_named("Realtime") && !time_policy_named("") && !time_policy_named("warn"));
    CHECK(RunIntent{}.time_policy == TimePolicy::Realtime);
}

TEST_CASE(a_shared_target_fails_the_check_with_strict_only)
{
    const Setup s = two_ethernets();
    FakeTools fake;
    const ConnectTools tools = fake.tools();
    Checked r;
    r.config = s.config;
    r.findings = check_connections(s.config, s.results, {{"A", "eth0"}, {"B", "eth0"}}, &tools).findings;
    CHECK(r.passed());
    r.strict = true;
    CHECK(!r.passed());
}

TEST_CASE(the_outputs_show_where_a_network_is_connected)
{
    const Setup s = connect_setup();
    Checked r;
    r.file = "simulation.yaml";
    r.config = s.config;
    r.participants = s.results;
    const std::string plainText = format_text(r);
    const std::string plainJson = format_json(r);
    // Without --connect: no column, and "outside": null on every network.
    CHECK(plainText.find("outside") == std::string::npos && plainJson.find("\"outside\": {") == std::string::npos &&
          plainJson.find("\"outside\": null") != std::string::npos);
    NetworkOutside eth{"ETH",
                       "vehicle_sil_ethernet_bridge",
                       "/opt/vsil/bin/vehicle_sil_ethernet_bridge",
                       "vEthernet (A)",
                       "bridge.ETH",
                       std::vector<std::string>{"--network", "ETH", "--interface", "vEthernet (A)", "--src-mac",
                                                "02:00:00:00:00:01"}};
    // Not found, and so no command (connect-tool-missing).
    NetworkOutside can{"CAN1", "vehicle_sil_vector_xl_bridge", {}, "physical:2", "bridge.CAN1", std::nullopt};
    r.outside = {eth, can};
    const std::string text = format_text(r);
    CHECK(text.find("network  kind      participants  outside\n"
                    "ETH      ethernet  Camera        vEthernet (A)\n"
                    "CAN1     can       Engine        physical:2\n"
                    "BODY     lin       -             -\n") != std::string::npos);
    const std::string json = format_json(r);
    CHECK(json.find("      \"participants\": [\n"
                    "        \"Camera\"\n"
                    "      ],\n"
                    "      \"outside\": {\n"
                    "        \"tool\": \"vehicle_sil_ethernet_bridge\",\n"
                    "        \"tool_path\": \"/opt/vsil/bin/vehicle_sil_ethernet_bridge\",\n"
                    "        \"target\": \"vEthernet (A)\",\n"
                    "        \"participant\": \"bridge.ETH\",\n"
                    "        \"command\": [\n"
                    "          \"vehicle_sil_ethernet_bridge\",\n"
                    "          \"--network\",\n"
                    "          \"ETH\",\n"
                    "          \"--interface\",\n"
                    "          \"vEthernet (A)\",\n"
                    "          \"--src-mac\",\n"
                    "          \"02:00:00:00:00:01\"\n"
                    "        ]\n"
                    "      }\n"
                    "    },\n") != std::string::npos);
    CHECK(json.find("        \"tool_path\": null,\n        \"target\": \"physical:2\",\n        \"participant\": "
                    "\"bridge.CAN1\",\n        \"command\": null\n      }\n") != std::string::npos);
    // A network that is not connected has "outside": null.
    CHECK(json.find("\"name\": \"BODY\",\n      \"kind\": \"lin\",\n      \"line\": 7,\n      \"participants\": [],\n"
                    "      \"outside\": null\n    }") != std::string::npos);
}

TEST_CASE(file_only_check_computes_raw_sha256_eligibility_and_sorted_findings_without_running_ecus)
{
    const std::filesystem::path dir = std::filesystem::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "check_file_only";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);

    const std::string lf_text =
        "vehicle_sil_simulation: 2\n"
        "networks:\n"
        "  - name: CAN1\n"
        "    kind: can\n"
        "participants:\n"
        "  - name: Engine\n"
        "    command: [\"./does_not_exist_ecu\"]\n"
        "variants:\n"
        "  fast: {}\n";
    const std::string crlf_text =
        "vehicle_sil_simulation: 2\r\n"
        "networks:\n"
        "  - name: CAN1\r\n"
        "    kind: can\r\n"
        "participants:\n"
        "  - name: Engine\r\n"
        "    command: [\"./does_not_exist_ecu\"]\r\n"
        "variants:\r\n"
        "  fast: {}\r\n";

    const std::filesystem::path lf_path = dir / "lf.yaml";
    const std::filesystem::path crlf_path = dir / "crlf.yaml";
    {
        std::ofstream out(lf_path, std::ios::binary);
        out << lf_text;
    }
    {
        std::ofstream out(crlf_path, std::ios::binary);
        out << crlf_text;
    }

    // Standard check_file (file_only = false) also populates file_sha256 and hot_eligibility.
    RunRequest std_req;
    std_req.host.config = lf_path.string();
    std_req.intent.variant = "fast";
    const Checked std_checked = check_file(std_req);
    CHECK(std_checked.file_sha256 == vehicle_sil::launch::detail::sha256_hex(lf_text));
    CHECK(std_checked.hot_eligibility == (HotEligibility{true, "", ""}));
    const std::string std_json = format_json(std_checked);
    CHECK(std_json.find("  \"file_sha256\": \"" + vehicle_sil::launch::detail::sha256_hex(lf_text) + "\",\n") != std::string::npos);
    CHECK(std_json.find("  \"hot_eligibility\": {\n") != std::string::npos);

    // file_only = true passes even though ./does_not_exist_ecu does not exist, and hashes LF vs CRLF bytes.
    CheckOptions file_only_opts;
    file_only_opts.file_only = true;
    const Checked lf_checked = check_file(std_req, file_only_opts);
    CHECK(lf_checked.passed());
    CHECK(lf_checked.file_sha256 == vehicle_sil::launch::detail::sha256_hex(lf_text));
    CHECK(lf_checked.hot_eligibility == (HotEligibility{true, "", ""}));
    const std::string lf_json = format_json(lf_checked);
    CHECK(lf_json.find("  \"file_sha256\": \"" + vehicle_sil::launch::detail::sha256_hex(lf_text) + "\",\n") != std::string::npos);
    CHECK(lf_json.find("  \"hot_eligibility\": {\n"
                       "    \"eligible\": true,\n"
                       "    \"reason\": null,\n"
                       "    \"message\": null\n"
                       "  }\n") != std::string::npos);

    RunRequest crlf_req = std_req;
    crlf_req.host.config = crlf_path.string();
    const Checked crlf_checked = check_file(crlf_req, file_only_opts);
    CHECK(crlf_checked.passed());
    CHECK(crlf_checked.file_sha256 == vehicle_sil::launch::detail::sha256_hex(crlf_text));
    CHECK(crlf_checked.file_sha256 != lf_checked.file_sha256);

    // trace_enabled -> eligible=false, reason="trace-enabled"
    CheckOptions trace_opts = file_only_opts;
    trace_opts.trace_enabled = true;
    const Checked trace_checked = check_file(std_req, trace_opts);
    CHECK(!trace_checked.hot_eligibility.eligible);
    CHECK(trace_checked.hot_eligibility.reason == "trace-enabled");

    // Usage error with readable file preserves file_sha256 and sets reason="usage".
    RunRequest bad_var = std_req;
    bad_var.intent.variant = "unknown";
    const Checked var_checked = check_file(bad_var, file_only_opts);
    CHECK(var_checked.usage);
    CHECK(var_checked.file_sha256 == vehicle_sil::launch::detail::sha256_hex(lf_text));
    CHECK(!var_checked.hot_eligibility.eligible);
    CHECK(var_checked.hot_eligibility.reason == "usage");

    // Unreadable file sets file_sha256 to empty ("null" in JSON) and reason="config-unreadable".
    RunRequest missing_req;
    missing_req.host.config = (dir / "missing.yaml").string();
    const Checked missing_checked = check_file(missing_req, file_only_opts);
    CHECK(!missing_checked.config.has_value());
    CHECK(missing_checked.file_sha256.empty());
    CHECK(missing_checked.hot_eligibility.reason == "config-unreadable");
    const std::string missing_json = format_json(missing_checked);
    CHECK(missing_json.find("  \"file_sha256\": null,\n") != std::string::npos);

    // Syntax error in a readable file STILL populates file_sha256 while reporting config-unreadable.
    const std::filesystem::path bad_yaml = dir / "bad.yaml";
    const std::string bad_yaml_text = "vehicle_sil_simulation: [\n";
    {
        std::ofstream out(bad_yaml, std::ios::binary);
        out << bad_yaml_text;
    }
    RunRequest bad_yaml_req;
    bad_yaml_req.host.config = bad_yaml.string();
    const Checked bad_yaml_checked = check_file(bad_yaml_req, file_only_opts);
    CHECK(!bad_yaml_checked.config.has_value());
    CHECK(bad_yaml_checked.file_sha256 == vehicle_sil::launch::detail::sha256_hex(bad_yaml_text));
    CHECK(bad_yaml_checked.hot_eligibility.reason == "config-unreadable");

    // sort_findings runs before early return: errors precede warnings/notes.
    const std::filesystem::path warn_err_cfg = dir / "warn_err.yaml";
    {
        std::ofstream out(warn_err_cfg, std::ios::binary);
        out << "vehicle_sil_simulation: 2\n"
               "unknown_top_key: true\n"
               "networks:\n"
               "  - name: CAN1\n"
               "    kind: can\n"
               "participants:\n"
               "  - name: Engine\n"
               "    command: [\"./does_not_exist_ecu\"]\n";
    }
    RunRequest sorted_req;
    sorted_req.host.config = warn_err_cfg.string();
    sorted_req.intent.connect.push_back({"MISSING_NET", "none"});
    const Checked sorted_checked = check_file(sorted_req, file_only_opts);
    CHECK(!sorted_checked.findings.empty());
    CHECK(sorted_checked.findings.front().severity == Severity::Error);

    // format_usage_json(...) includes file_sha256: null and hot_eligibility.
    const std::string usage_json = format_usage_json(
        CommandKind::Check, "x.yaml", {Finding{Severity::Error, "usage", "bad flag", 0, "", ""}});
    CHECK(usage_json.find("  \"file_sha256\": null,\n") != std::string::npos);
    CHECK(usage_json.find("  \"hot_eligibility\": {\n"
                          "    \"eligible\": false,\n"
                          "    \"reason\": \"usage\",\n"
                          "    \"message\": \"bad flag\"\n"
                          "  }\n") != std::string::npos);
}
