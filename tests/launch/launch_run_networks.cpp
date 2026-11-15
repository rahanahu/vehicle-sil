// SPDX-License-Identifier: MIT
//
// The launch_run scenarios about the traffic on the networks and the names of the networks in
// the configuration file.
#include "launch_run_support.hpp"

/// The event "participant" of `name` in `state` among the lines of run --events, or "".
std::string participant_event(const std::string& events, const std::string& name, const std::string& state)
{
    const std::string key = "\"event\":\"participant\"";
    const std::string who = "\"name\":\"" + name + "\",\"state\":\"" + state + "\"";
    std::istringstream in(events);
    for (std::string line; std::getline(in, line);)
    {
        if (contains(line, key) && contains(line, who))
        {
            return line;
        }
    }
    return {};
}

/// The part of the run's text from "traffic:" to the empty line after it (the line alone, or the
/// line, an empty line and the table).
std::string traffic_part(const std::string& text)
{
    const auto at = text.find("\ntraffic: ");
    require(at != std::string::npos, "the output has no traffic: line\n" + text);
    const std::string from = text.substr(at + 1);
    const bool table = from.find("\n\nnetwork ") == from.find('\n');
    const auto end = table ? from.find("\n\n", from.find('\n') + 2) : from.find("\n\n");
    require(end != std::string::npos, "nothing after the traffic\n" + text);
    return from.substr(0, end + 1);
}

void traffic(const Options& o)
{
    // The example: Engine sends, Dashboard only listens. The same output from run to run.
    const fs::path example = example_copy(o);
    const Captured first = launch(o, {"run", "simulation.yaml"}, example);
    expect_code(first, 0, "run simulation.yaml");
    expect_text(traffic_part(first.standard_output),
                "traffic: J1939_CAN: 1 participant sent, 1 received (2 counted) (steps before 1000 ms)\n"
                "\n"
                "network    participant  sent  received\n"
                "J1939_CAN  Engine         11         0\n"
                "J1939_CAN  Dashboard       0        11\n",
                "the traffic of the example");
    for (int again = 0; again < 3; ++again)
    {
        const Captured next = launch(o, {"run", "simulation.yaml"}, example);
        expect_code(next, 0, "run simulation.yaml again");
        expect_text(next.standard_output, first.standard_output, "the output of the example, run again");
    }
    const auto with_names = [](const std::string& more_networks, const std::string& engine, const std::string& dashboard) {
        return "vehicle_sil_simulation: 1\n"
               "stop_at_ms: 1000\n"
               "networks:\n"
               "  - { name: J1939_CAN, kind: can }\n" +
               more_networks +
               "participants:\n"
               "  - name: Engine\n"
               "    command: [engine/vehicle_sil_engine_ecu]\n" +
               engine +
               "  - name: Dashboard\n"
               "    command: [dashboard/vehicle_sil_dashboard_ecu]\n" +
               dashboard;
    };
    // The file gives Dashboard's bus another name: two buses with one participant each, and
    // nobody receives. The names are the simulation's.
    write_file(example / "apart.yaml",
               with_names("  - { name: VehicleCAN, kind: can }\n", "", "    network_names: { VehicleCAN: J1939_CAN }\n"));
    const Captured apart = launch(o, {"run", "apart.yaml"}, example);
    expect_code(apart, 0, "run apart.yaml");
    expect_text(traffic_part(apart.standard_output),
                "traffic: J1939_CAN: 1 participant sent, 0 received (1 counted); VehicleCAN: nothing sent (1 counted) (steps "
                "before 1000 ms)\n"
                "\n"
                "network     participant  sent  received\n"
                "J1939_CAN   Engine         11         0\n"
                "VehicleCAN  Dashboard       0         0\n",
                "the traffic of two buses that are one in the code");
    // Both renamed: the counters are found under the simulation's name, and the frames arrive.
    write_file(example / "renamed.yaml",
               with_names("  - { name: VehicleCAN, kind: can }\n", "    network_names: { VehicleCAN: J1939_CAN }\n",
                          "    network_names: { VehicleCAN: J1939_CAN }\n"));
    const Captured renamed = launch(o, {"run", "renamed.yaml"}, example);
    expect_code(renamed, 0, "run renamed.yaml");
    expect_text(traffic_part(renamed.standard_output),
                "traffic: J1939_CAN: not counted (no participant known to be on it); VehicleCAN: 1 participant sent, 1 received "
                "(2 counted) (steps before 1000 ms)\n"
                "\n"
                "network     participant  sent  received\n"
                "J1939_CAN   -               -         -  no participant known to be on it\n"
                "VehicleCAN  Engine         11         0\n"
                "VehicleCAN  Dashboard       0        11\n",
                "the traffic of a bus that both call by another name");

    // What a machine reads: delivered where the listener heard, not where the buses are apart.
    const auto delivered = [&](const char* file) {
        const Captured run = launch(o, {"run", file, "--json"}, example);
        expect_code(run, 0, std::string("run ") + file + " --json");
        const std::string& out = run.standard_output;
        std::string all;
        for (auto at = out.find("        \"delivered\": "); at != std::string::npos; at = out.find("        \"delivered\": ", at + 1))
        {
            all += out.substr(at + 21, out.find(',', at) - at - 21) + " ";
        }
        return all;
    };
    expect_text(delivered("simulation.yaml"), "true ", "delivered in the example");
    expect_text(delivered("apart.yaml"), "false null ", "delivered on two buses that are one in the code");
    expect_text(delivered("renamed.yaml"), "null true ", "delivered on a bus that both call by another name");

    // An ECU without a network.
    const fs::path alone = scenario_dir(o, "vehicle_sil_simulation: 1\nstop_at_ms: 100\nnetworks: []\nparticipants:\n"
                                           "  - name: Engine\n    command: [" + ecu(o) + "]\n");
    const Captured none = launch(o, {"run", "sim.yaml"}, alone);
    expect_code(none, 0, "run without a network");
    expect_text(traffic_part(none.standard_output), "traffic: no network in use\n", "the traffic without a network");
    const Captured none_json = launch(o, {"run", "sim.yaml", "--json"}, alone);
    expect_code(none_json, 0, "run without a network --json");
    require(contains(none_json.standard_output,
                     "  \"traffic\": {\n    \"before_ms\": 100,\n    \"unplaced\": [],\n    \"networks\": []\n  },\n"),
            "the JSON's traffic without a network:\n" + none_json.standard_output);

    // ECUs that declare no variable (the launcher does not wait for the hello of such a one), one
    // that uses the bus in another way, one without --describe whose counters the file maps, one
    // without --describe that is counted by what its control agent says, and one that is disabled.
    const std::string undescribed = ", --registry-uri, \"{registry_uri}\", --name, \"{name}\"]\n    describe: false\n";
    const fs::path mixed = scenario_dir(o,
                                        "vehicle_sil_simulation: 1\n"
                                        "stop_at_ms: 200\n"
                                        "networks:\n"
                                        "  - { name: BUS, kind: can }\n"
                                        "  - { name: Empty, kind: can }\n"
                                        "participants:\n"
                                        "  - name: A\n    command: [" + ecu(o) + "]\n"
                                        "  - name: B\n    command: [" + ecu(o) + "]\n"
                                        "  - name: Raw\n    command: [" + ecu(o) + "]\n"
                                        "  - name: Door\n    command: [" + ecu(o) + undescribed +
                                        "    variables:\n      door.sent: j1939.BUS.frames_sent\n"
                                        "  - name: Tool\n    command: [" + ecu(o) + undescribed +
                                        "  - name: Off\n    command: [" + ecu(o) + "]\n    enabled: false\n",
                                        {{"A", "j1939 BUS 10"},
                                         {"B", "j1939 BUS 11"},
                                         {"Raw", "talk BUS"},
                                         {"Door", "j1939 BUS 12"},
                                         {"Tool", "j1939 BUS 13"},
                                         {"Off", "j1939 BUS 14"}});
    const Captured mix = launch(o, {"run", "sim.yaml"}, mixed);
    expect_code(mix, 0, "run with ECUs that are counted and not counted");
    // Four of them send a frame in each of the 19 steps before the one at 190 ms.
    expect_text(traffic_part(mix.standard_output),
                "traffic: BUS: 3 participants sent, 3 received (3 counted, 2 not counted); Empty: not counted (no participant "
                "known to be on it, 2 unplaced) (steps before 200 ms)\n"
                "\n"
                "network  participant  sent  received\n"
                "BUS      A              19        76\n"
                "BUS      B              19        76\n"
                "BUS      Raw             -         -  not counted: no J1939 counters\n"
                "BUS      Door            -         -  not counted: counters mapped by variables\n"
                "BUS      Tool           19        76\n"
                "Empty    -               -         -  no participant known to be on it\n"
                // Without --describe, the counters say where a participant is, not where it is not.
                "Empty    Door            -         -  unplaced: no J1939 counters\n"
                "Empty    Tool            -         -  unplaced: no J1939 counters\n",
                "the traffic of ECUs that are counted and not counted");
    for (int again = 0; again < 3; ++again)
    {
        const Captured next = launch(o, {"run", "sim.yaml"}, mixed);
        expect_code(next, 0, "run with ECUs that are counted and not counted, again");
        expect_text(next.standard_output, mix.standard_output, "the output with ECUs that declare no variable, run again");
    }
    const Captured mix_json = launch(o, {"run", "sim.yaml", "--json"}, mixed);
    expect_code(mix_json, 0, "run with ECUs that are counted and not counted --json");
    const std::string& json = mix_json.standard_output;
    const auto from = json.find("  \"traffic\": {\n");
    const auto to = json.find("  \"findings\": [");
    require(from != std::string::npos && to != std::string::npos && from < to, "the JSON has no traffic before the findings:\n" + json);
    const auto member = [](const std::string& name, const std::string& why, const std::string& numbers) {
        return "          {\n            \"name\": \"" + name + "\",\n            \"counted\": " + (why.empty() ? "true" : "false") +
               ",\n            \"not_counted\": " + (why.empty() ? "null" : "\"" + why + "\"") +
               ",\n            \"virtual_time\": true,\n" + numbers + "          }";
    };
    const std::string counted = "            \"at_ms\": 190,\n            \"frames_sent\": 19,\n            \"frames_received\": 76\n";
    const std::string not_counted = "            \"at_ms\": null,\n            \"frames_sent\": null,\n            \"frames_received\": null\n";
    expect_text(json.substr(from, to - from),
                "  \"traffic\": {\n"
                "    \"before_ms\": 200,\n"
                "    \"unplaced\": [],\n"
                "    \"networks\": [\n"
                "      {\n"
                "        \"name\": \"BUS\",\n"
                "        \"kind\": \"can\",\n"
                "        \"counted\": 3,\n"
                "        \"senders\": 3,\n"
                "        \"receivers\": 3,\n"
                "        \"delivered\": true,\n"
                "        \"unplaced\": [],\n"
                "        \"participants\": [\n" +
                    member("A", "", counted) + ",\n" + member("B", "", counted) + ",\n" + member("Raw", "no_counters", not_counted) +
                    ",\n" + member("Door", "mapped", not_counted) + ",\n" + member("Tool", "", counted) +
                    "\n"
                    "        ]\n"
                    "      },\n"
                    "      {\n"
                    "        \"name\": \"Empty\",\n"
                    "        \"kind\": \"can\",\n"
                    "        \"counted\": 0,\n"
                    "        \"senders\": 0,\n"
                    "        \"receivers\": 0,\n"
                    "        \"delivered\": null,\n"
                    "        \"unplaced\": [\n"
                    "          {\n            \"participant\": \"Door\",\n            \"reason\": \"no_counters\"\n          },\n"
                    "          {\n            \"participant\": \"Tool\",\n            \"reason\": \"no_counters\"\n          }\n"
                    "        ],\n"
                    "        \"participants\": []\n"
                    "      }\n"
                    "    ]\n"
                    "  },\n",
                "the JSON's traffic");

    // A network that the participants use, none of them known to the launcher (no --describe, no
    // control agent): it is in the result, not counted. "no network in use" is for a file without one.
    write_file(mixed / "test-ecu-no-control", "");
    write_file(mixed / "unknown.yaml", "vehicle_sil_simulation: 1\nstop_at_ms: 100\nnetworks:\n  - { name: BUS, kind: can }\n"
                                       "participants:\n  - name: Door\n    command: [" + ecu(o) + undescribed +
                                       "  - name: Tool\n    command: [" + ecu(o) + undescribed);
    const Captured unknown = launch(o, {"run", "unknown.yaml"}, mixed);
    fs::remove(mixed / "test-ecu-no-control");
    expect_code(unknown, 0, "run with participants the launcher knows nothing of");
    expect_text(traffic_part(unknown.standard_output),
                "traffic: BUS: not counted (no participant known to be on it, 2 unplaced) (steps before 100 ms)\n"
                "\n"
                "network  participant  sent  received\n"
                "BUS      -               -         -  no participant known to be on it\n"
                "-        Door            -         -  unplaced: no control link\n"
                "-        Tool            -         -  unplaced: no control link\n",
                "the traffic of a network with participants the launcher knows nothing of");

    // A sender that nobody counted heard, and a bridge that the launcher knows on no network (no
    // --describe, no control agent) and that hears the bus as raw CAN: the frames may have
    // arrived, so the result does not say they did not. Without the bridge, it does.
    write_file(mixed / "Bridge.behaviour", "hear BUS\n");
    write_file(mixed / "bridged.yaml", "vehicle_sil_simulation: 1\nstop_at_ms: 200\nnetworks:\n  - { name: BUS, kind: can }\n"
                                       "participants:\n  - name: A\n    command: [" + ecu(o) + "]\n"
                                       "  - name: Bridge\n    command: [" + ecu(o) +
                                       ", --registry-uri, \"{registry_uri}\", --name, \"{name}\", --no-control]\n"
                                       "    describe: false\n");
    const Captured bridged = launch(o, {"run", "bridged.yaml"}, mixed);
    expect_code(bridged, 0, "run with a bridge");
    expect_text(traffic_part(bridged.standard_output),
                "traffic: BUS: 1 participant sent, 0 received (1 counted, 1 unplaced) (steps before 200 ms)\n"
                "\n"
                "network  participant  sent  received\n"
                "BUS      A              19         0\n"
                "-        Bridge          -         -  unplaced: no control link\n",
                "the traffic with a bridge the launcher knows on no network");
    const Captured bridged_json = launch(o, {"run", "bridged.yaml", "--json"}, mixed);
    expect_code(bridged_json, 0, "run with a bridge --json");
    require(contains(bridged_json.standard_output,
                     "    \"unplaced\": [\n      {\n        \"participant\": \"Bridge\",\n"
                     "        \"reason\": \"no_control_link\"\n      }\n    ],\n") &&
                contains(bridged_json.standard_output,
                         "        \"delivered\": null,\n        \"unplaced\": [\n          {\n"
                         "            \"participant\": \"Bridge\",\n            \"reason\": \"no_control_link\"\n          }\n"
                         "        ],\n"),
            "the JSON's traffic with a bridge:\n" + bridged_json.standard_output);
    const Captured alone_json = launch(o, {"run", "bridged.yaml", "--json", "--disable", "Bridge"}, mixed);
    expect_code(alone_json, 0, "run without the bridge --json");
    require(contains(alone_json.standard_output, "    \"unplaced\": [],\n") &&
                contains(alone_json.standard_output, "        \"delivered\": false,\n        \"unplaced\": [],\n"),
            "the JSON's traffic without the bridge:\n" + alone_json.standard_output);

    // A gateway without --describe: its control agent has J1939 counters for CAN_A, and it hears
    // CAN_B as raw CAN. The counters place it on CAN_A; they do not say that it is not on CAN_B,
    // so the result does not say that the sender's frames there did not arrive.
    write_file(mixed / "Sender.behaviour", "j1939 CAN_B 30\n");
    write_file(mixed / "Gateway.behaviour", "j1939 CAN_A 31 hear CAN_B\n");
    write_file(mixed / "gateway.yaml", "vehicle_sil_simulation: 1\nstop_at_ms: 200\nnetworks:\n  - { name: CAN_A, kind: can }\n"
                                       "  - { name: CAN_B, kind: can }\n"
                                       "participants:\n  - name: Sender\n    command: [" + ecu(o) + "]\n"
                                       "  - name: Gateway\n    command: [" + ecu(o) + undescribed);
    const Captured gateway = launch(o, {"run", "gateway.yaml"}, mixed);
    expect_code(gateway, 0, "run with a gateway");
    expect_text(traffic_part(gateway.standard_output),
                "traffic: CAN_A: 1 participant sent, 0 received (1 counted); CAN_B: 1 participant sent, 0 received (1 counted, "
                "1 unplaced) (steps before 200 ms)\n"
                "\n"
                "network  participant  sent  received\n"
                "CAN_A    Gateway        19         0\n"
                "CAN_B    Sender         19         0\n"
                "CAN_B    Gateway         -         -  unplaced: no J1939 counters\n",
                "the traffic with a gateway that its counters place on one of its two networks");
    require(fs::exists(mixed / "Gateway.heard"), "the gateway heard no frame on CAN_B");
    const Captured gateway_json = launch(o, {"run", "gateway.yaml", "--json"}, mixed);
    expect_code(gateway_json, 0, "run with a gateway --json");
    require(contains(gateway_json.standard_output, "    \"unplaced\": [],\n    \"networks\": [\n") &&
                contains(gateway_json.standard_output, "        \"delivered\": false,\n        \"unplaced\": [],\n") &&
                contains(gateway_json.standard_output,
                         "        \"delivered\": null,\n        \"unplaced\": [\n          {\n"
                         "            \"participant\": \"Gateway\",\n            \"reason\": \"no_counters\"\n          }\n"
                         "        ],\n"),
            "the JSON's traffic with a gateway:\n" + gateway_json.standard_output);

    // A run that did not start has none.
    const fs::path missing = scenario_dir(o, "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n"
                                             "  - name: Engine\n    command: [" + ecu(o) + "]\n"
                                             "  - name: Quitter\n    command: [" + helper(o) + ", --exit, \"2\", --uri, \"{registry_uri}\"]\n"
                                             "    describe: false\n");
    const Captured unstarted = launch(o, {"run", "sim.yaml", "--json"}, missing);
    require(contains(unstarted.standard_output, "  \"simulation\": \"not_started\",\n") &&
                contains(unstarted.standard_output, "  \"traffic\": null,\n"),
            "a run that did not start has no traffic:\n" + unstarted.standard_output);
    require(!contains(launch(o, {"run", "sim.yaml"}, missing).standard_output, "traffic:"),
            "a run that did not start says nothing of the traffic");
}

void network_names(const Options& o)
{
    // Engine's code names its buses J1939_CAN and CAN_B; the file calls them VehicleCAN and
    // "Body CAN" (a space: the argument must be quoted for a shell). Dashboard's code says
    // VehicleCAN itself. Door is not described (describe: false) and calls the second bus DoorCAN.
    // The variant "apart" takes Engine's first line away: its J1939_CAN is then a bus of its own.
    // The variant "moved" puts Engine's CAN_B on VehicleCAN instead of "Body CAN" (`moved`: how).
    const auto config_with = [&o, port = free_port()](const std::string& moved) {
    return std::string(
        "vehicle_sil_simulation: 1\n"                                                    // 1
        "stop_at_ms: 300\n"                                                              // 2
        "registry_uri: silkit://127.0.0.1:") + std::to_string(port) + "\n"               // 3
        "networks:\n"                                                                    // 4
        "  - { name: VehicleCAN, kind: can }\n"                                          // 5
        "  - { name: \"Body CAN\", kind: can }\n"                                        // 6
        "  - { name: J1939_CAN, kind: can }\n"                                           // 7
        "participants:\n"                                                                // 8
        "  - name: Engine\n"                                                             // 9
        "    command: [" + ecu(o) + "]\n"                                                // 10
        "    network_names:\n"                                                           // 11
        "      VehicleCAN: J1939_CAN\n"                                                  // 12
        "      \"Body CAN\": CAN_B\n"                                                    // 13
        "  - name: Dashboard\n"                                                          // 14
        "    command: [" + ecu(o) + "]\n"                                                // 15
        "  - name: Door\n"                                                               // 16
        "    command: [" + ecu(o) + ", --registry-uri, \"{registry_uri}\", --name, \"{name}\"]\n" // 17
        "    describe: false\n"                                                          // 18
        "    network_names: { \"Body CAN\": DoorCAN, VehicleCAN: VehicleCAN }\n"         // 19
        "variants:\n"
        "  together: {}\n"
        "  apart: { participants: { Engine: { network_names: { VehicleCAN: VehicleCAN } } } }\n"
        "  moved: { participants: { Engine: { network_names: { " + moved + " } } } }\n";            // 23
    };
    // The line whose two names are the same takes the common line of "Body CAN" away.
    const std::string config = config_with("VehicleCAN: CAN_B, \"Body CAN\": \"Body CAN\"");
    const fs::path dir = hand_dir(o, "names", config,
                                  {{"Engine", "talk J1939_CAN talk CAN_B"}, {"Dashboard", "hear VehicleCAN"}, {"Door", "hear DoorCAN"}});
    const auto forget = [&dir] {
        fs::remove(dir / "Dashboard.heard");
        fs::remove(dir / "Door.heard");
    };
    forget();
    {
        // Each executable gets its lines after everything else, in the file's order and direction;
        // a line whose two names are the same is not given. What is started is one bus each.
        const Captured run = launch(o, {"run", "sim.yaml", "--variant", "together", "--events"}, dir);
        expect_code(run, 0, "run with network_names");
        const std::string events = lf(run.standard_output);
        const std::string engine = participant_event(events, "Engine", "started");
        require(contains(engine, ",\"--start-timeout-ms\",") &&
                    contains(engine, "\",\"--map-network\",\"VehicleCAN=J1939_CAN\",\"--map-network\",\"Body CAN=CAN_B\"],\"log\"") &&
                    engine.find("--start-timeout-ms") < engine.find("--map-network"),
                "Engine is started with its two lines, last:\n" + engine);
        const std::string door = participant_event(events, "Door", "started");
        require(contains(door, ",\"--name\",\"Door\",\"--map-network\",\"Body CAN=DoorCAN\"],\"log\""),
                "Door (describe: false) gets its line after its command with the placeholders replaced:\n" + door);
        const std::string dashboard = participant_event(events, "Dashboard", "started");
        require(!dashboard.empty() && !contains(dashboard, "--map-network"), "Dashboard gets no --map-network:\n" + dashboard);
        require(contains(events, "\"network_map\":[{\"name\":\"VehicleCAN\",\"line\":12,\"from\":\"J1939_CAN\"},"
                                 "{\"name\":\"Body CAN\",\"line\":13,\"from\":\"CAN_B\"}]"),
                "the check of the run shows the lines:\n" + events);
        require(fs::exists(dir / "Dashboard.heard") && fs::exists(dir / "Door.heard"),
                "Dashboard and Door hear Engine on the buses the file names:\n" + lf(run.standard_error));
        // For people: the simulation's name first.
        const Captured check = launch(o, {"check", "sim.yaml", "--variant", "together"}, dir);
        expect_code(check, 0, "check with network_names");
        require(contains(lf(check.standard_output), "  VehicleCAN <- J1939_CAN (raw), Body CAN <- CAN_B (raw)\n") &&
                    contains(lf(check.standard_output), "VehicleCAN  can   Engine, Dashboard\n"),
                "the tables show both names of a renamed network:\n" + lf(check.standard_output));
    }
    forget();
    {
        // The variant without the first line: Engine's J1939_CAN is not Dashboard's VehicleCAN.
        const Captured run = launch(o, {"run", "sim.yaml", "--variant", "apart", "--events"}, dir);
        expect_code(run, 0, "run with the variant that takes a line away");
        const std::string engine = participant_event(lf(run.standard_output), "Engine", "started");
        require(contains(engine, "\"--start-timeout-ms\",") && contains(engine, "\",\"--map-network\",\"Body CAN=CAN_B\"],\"log\"") &&
                    !contains(engine, "VehicleCAN="),
                "Engine is started with the one line the variant leaves:\n" + engine);
        require(!fs::exists(dir / "Dashboard.heard") && fs::exists(dir / "Door.heard"),
                "without the line, Dashboard hears nothing; Door still does");
    }
    forget();
    {
        // The variant that moves a network to another bus: CAN_B is on VehicleCAN, where Dashboard
        // hears it, and no longer on "Body CAN", where Door is.
        const Captured run = launch(o, {"run", "sim.yaml", "--variant", "moved", "--events"}, dir);
        expect_code(run, 0, "run with the variant that moves a network");
        const std::string engine = participant_event(lf(run.standard_output), "Engine", "started");
        require(contains(engine, "\"--start-timeout-ms\",") && contains(engine, "\",\"--map-network\",\"VehicleCAN=CAN_B\"],\"log\"") &&
                    !contains(engine, "Body CAN="),
                "Engine is started with the line of the variant alone:\n" + engine);
        require(fs::exists(dir / "Dashboard.heard") && !fs::exists(dir / "Door.heard"),
                "Dashboard hears Engine's CAN_B on VehicleCAN, and Door hears nothing");
        // Without that line, CAN_B has two names. The message has the line to add, and the file
        // fails for every variant (each variant of a file is checked, whichever is chosen).
        write_file(dir / "wrong.yaml", config_with("VehicleCAN: CAN_B"));
        const Captured wrong = launch(o, {"check", "wrong.yaml", "--variant", "together"}, dir);
        expect_code(wrong, 1, "check of a file with a variant that gives a network two names");
        require(contains(lf(wrong.standard_output),
                         "error: wrong.yaml:23: variant 'moved': participant 'Engine': network_names gives the executable's "
                         "network 'CAN_B' two names, 'Body CAN' (line 13, the common part) and 'VehicleCAN' (line 23, this "
                         "variant), but a network of an ECU is on one bus: to have it on 'VehicleCAN' in this variant, add "
                         "the line Body CAN: Body CAN to the variant's network_names of 'Engine' (a line whose two names "
                         "are the same takes the common line away), or remove the variant's line "
                         "[network-map-collision]\n"),
                "the message says how the variant takes the common line away:\n" + lf(wrong.standard_output));
    }
    forget();
    HandCommand hand;
    {
        // Started by hand: the command carries the same arguments, the one with a space quoted for
        // this system's shell, and the executable started with it is on the bus.
        Live live(o, dir, {"run", "sim.yaml", "--variant", "together", "--manual", "Door"});
        const std::string err = live.wait_for_error(
            [](const std::string& text) { return contains(text, "[vehicle_sil_launch] waiting for Door to join (manual): "); });
        hand = hand_command(err, "Door");
#ifdef _WIN32
        const std::string quoted = " --name Door --map-network \"Body CAN=DoorCAN\"";
#else
        const std::string quoted = " --name Door --map-network 'Body CAN=DoorCAN'";
#endif
        require(hand.line.size() > quoted.size() &&
                    hand.line.compare(hand.line.size() - quoted.size(), quoted.size(), quoted) == 0,
                "the command to start Door by hand ends with its line, quoted:\n" + err);
        ByHand byHand(hand.line, hand.directory);
        const auto [status, out] = live.finish_unchecked();
        require(status && status->code == 0, "the run with Door started by hand ends with 0:\n" + out_of(out));
        const auto ended = byHand.wait(30s);
        require(ended && ended->code == 0, "Door ends by itself with 0");
        require(fs::exists(dir / "Door.heard"), "Door, started with the command shown, hears Engine");
    }
    forget();
    {
        // The event of a participant started by hand has the same arguments, and the same line
        // (the registry is where the file says, so the command of the run before starts Door here).
        Live live(o, dir, {"run", "sim.yaml", "--variant", "together", "--manual", "Door", "--events"});
        const std::string manual = live.wait_for({"\"event\":\"participant\"", "\"name\":\"Door\",\"state\":\"manual\""});
#ifdef _WIN32
        const std::string quoted = " --name Door --map-network \\\"Body CAN=DoorCAN\\\"\"";
#else
        const std::string quoted = " --name Door --map-network 'Body CAN=DoorCAN'\"";
#endif
        require(contains(manual, ",\"--name\",\"Door\",\"--map-network\",\"Body CAN=DoorCAN\"],") && contains(manual, quoted),
                "the event of the participant started by hand has the arguments and the line:\n" + manual);
        ByHand byHand(hand.line, hand.directory);
        const auto [status, out] = live.finish_unchecked();
        require(status && status->code == 0, "the run with --events and Door started by hand ends with 0:\n" + out_of(out));
        const auto ended = byHand.wait(30s);
        require(ended && ended->code == 0, "Door ends by itself with 0");
    }
    {
        // An executable of a vehicle-sil before --map-network: an error of the check, and nothing
        // is started (it would end at once with a usage error, after the others were started).
        const fs::path old = hand_dir(o, "names-old",
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 100\n"
                                      "networks:\n"
                                      "  - { name: VehicleCAN, kind: can }\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n"
                                      "    network_names: { VehicleCAN: OLD_CAN }\n"   // 8
                                      "  - name: Dashboard\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Dashboard", "pid hear VehicleCAN"}});
        write_file(old / "test-ecu-old", "Engine\n");
        fs::remove(old / "Engine.started");
        fs::remove(old / "Dashboard.pid");
        const Captured run = launch(o, {"run", "sim.yaml"}, old);
        expect_code(run, 1, "run with network_names for an executable that does not take --map-network");
        const std::string out = lf(run.standard_output);
        require(contains(out, "error: sim.yaml:8: participant 'Engine': network_names needs an executable that takes "
                              "--map-network, and this one does not say that it does (no \"map-network\" in the features of "
                              "its --describe; it was built with vehicle-sil 0.1.0): have its author build it again with "
                              "vehicle-sil ") &&
                    contains(out, " or later, or remove network_names and use the executable's own name in networks "
                                  "('OLD_CAN' instead of 'VehicleCAN'), with network_names for the other participants on it "
                                  "[network-map-unsupported]\n") &&
                    contains(out, "\nnothing was started: the check found errors"),
                "the check says that the executable does not take --map-network:\n" + out);
        // The other findings are about the simulation the file means: Engine is on VehicleCAN.
        require(contains(out, "Engine       10 ms  VehicleCAN <- OLD_CAN (raw)\n") && !contains(out, "[unknown-network]"),
                "the rules read Engine's network under the simulation's name:\n" + out);
        require(!fs::exists(old / "Engine.started") && !fs::exists(old / "Dashboard.pid"), "nothing was started");
    }
}
