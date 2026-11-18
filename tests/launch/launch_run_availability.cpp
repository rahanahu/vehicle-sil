// SPDX-License-Identifier: MIT
//
// The launch_run scenario about the availability of variables (VariableOptions::availability):
//
//   availability  an ECU that decides in its first steps which inputs and outputs it uses, and
//                 reports it (the test ECU's "availability"): a scenario writes an input that it
//                 reported Inactive and expects the output of it, and passes. The same ECU without
//                 the report ("availability-silent") gives the same scenario result and the same
//                 timeline, and the same --describe: the availability is information only.
#include "launch_run_support.hpp"

#include <array>

void availability(const Options& o)
{
    // Profile 1: io.in0 and io.out0 are used, io.in1 and io.out1 are not (Inactive from 20 ms on).
    const std::string file = "vehicle_sil_scenario: 1\n"
                             "name: unused inputs\n"
                             "steps:\n"
                             "  - set: { Nvm.nvm.profile: 1, Nvm.io.in0: 2 }\n"
                             "  - expect: { Nvm.io.out0: 4, Nvm.nvm.decided: false }\n"
                             "  - wait_until: { Nvm.nvm.decided: true }\n"
                             "    timeout_ms: 500\n"
                             "  - set: { Nvm.io.in1: 7 }\n"
                             "  - expect: { Nvm.io.out1: 14, Nvm.io.out0: 4 }\n"
                             "  - after_ms: 50\n"
                             "    ramp: { Nvm.io.in1: { to: 0, over_ms: 50 } }\n"
                             "  - expect: { Nvm.io.out1: 0 }\n";
    const auto run = [&](const std::string& behaviour) {
        const fs::path dir = scenario_dir(o,
                                          "vehicle_sil_simulation: 1\n"
                                          "stop_at_ms: 1000\n"
                                          "networks: []\n"
                                          "participants:\n"
                                          "  - name: Nvm\n"
                                          "    command: [" + ecu(o) + "]\n",
                                          {{"Nvm", behaviour}});
        write_file(dir / "s.yaml", file);
        const Captured r = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--json", "--timeline", "timeline.json"}, dir);
        expect_code(r, 0, behaviour);
        const std::string out = lf(r.standard_output);
        require(contains(out, "\"status\": \"passed\""), behaviour + ": the scenario passes:\n" + out);
        vehicle_sil::process::CaptureOptions options;
        options.timeout = scaled_timeout(60s);
        options.working_directory = dir;
        const Captured describe = vehicle_sil::process::run_captured(o.test_ecu, {"--describe", "--name", "Nvm"}, options);
        require(describe.exit_code == 0, behaviour + ": --describe ends with 0:\n" + describe.standard_error);
        return std::array<std::string, 3>{scenario_json(out), read_text(dir / "timeline.json"), lf(describe.standard_output)};
    };
    const auto reported = run("availability");
    require(contains(reported[1], "\"variable\": \"Nvm.io.in1\""), "the timeline has the input:\n" + reported[1]);
    require(contains(reported[2], "\"name\": \"io.in1\""), "--describe has the input:\n" + reported[2]);
    log("CHECK a scenario that writes an Inactive input and expects its output passes -> ok");
    const auto silent = run("availability-silent");
    expect_text(silent[0], reported[0], "the scenario of the ECU that does not report its availability");
    expect_text(silent[1], reported[1], "the timeline of the ECU that does not report its availability");
    expect_text(silent[2], reported[2], "--describe of the ECU that does not report its availability");
    log("CHECK the same scenario result, timeline and --describe without the availability -> ok");
}
