// SPDX-License-Identifier: MIT
//
// The launch_run scenario about the samples a scenario judges with:
//
//   scenario_samples  an ECU with a step of 1 ms whose code sends its values every 10 ms, and an
//                output that is wrong for one step between two of those: a hold_for_ms over the
//                step, a hold_for_ms between two samples and an expect at the step's time all fail
//                (the launcher starts the ECU with --sample-every-step), and pass with
//                samples: as_sent, which the check notes; an ECU that does not follow the argument
//                is refused before the first step (scenario-not-observed), and runs with as_sent;
//                with samples as far apart as its steps (10 ms and 10 ms) it is not refused
#include "launch_run_support.hpp"

void scenario_samples(const Options& o)
{
    // Body steps every 1 ms; its code asks for a sample every 10 ms (0, 10, ...). "healthy" is
    // false in the step at 1053 ms only: between the samples of 1050 ms and 1060 ms.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                                      "  - name: Body\n    command: [" + ecu(o) + "]\n",
                                      {{"Body", "sample-ms 10 scenario-variables glitch 1053"}});
    write_file(dir / "test-ecu-step-ms", "1\n");
    fs::remove(dir / "test-ecu-own-sample-period");
    struct Form
    {
        std::string what;
        std::string step;     ///< the one step of the scenario
        std::string failure;  ///< its message with every step sampled
        std::string as_sent;  ///< "at_ms" of the step that passes with samples: as_sent
    };
    const std::vector<Form> forms{
        {"a hold over a step that is wrong between two samples",
         "  - at_ms: 1000\n    expect: { Body.healthy: true }\n    hold_for_ms: 100\n",
         "not true for 100 ms (from 1000 ms): at 1053 ms Body.healthy eq true, but it was false (its sample of 1053 ms)", "1100"},
        {"a hold that has no sample of the ECU's own period in it",
         "  - at_ms: 1051\n    expect: { Body.healthy: true }\n    hold_for_ms: 8\n",
         "not true for 8 ms (from 1051 ms): at 1053 ms Body.healthy eq true, but it was false (its sample of 1053 ms)", "1059"},
        {"an expect at a time between two samples", "  - at_ms: 1053\n    expect: { Body.healthy: true }\n",
         "not true at 1053 ms: Body.healthy eq true, but it was false (its sample of 1053 ms)", "1053"},
    };
    const std::vector<std::string> run{"run", "sim.yaml", "--scenario", "s.yaml", "--json"};
    for (const Form& form : forms)
    {
        // As the file is: the launcher asks the ECU for every step, whatever its code says, and
        // the scenario sees the step.
        write_file(dir / "s.yaml", "vehicle_sil_scenario: 1\nsteps:\n" + form.step);
        const Captured seen = launch(o, run, dir);
        expect_code(seen, 1, form.what);
        const std::string failed = scenario_json(lf(seen.standard_output));
        require(contains(failed, "    \"status\": \"failed\",\n") &&
                    contains(failed, "        \"status\": \"failed\",\n        \"start_ms\": ") &&
                    contains(failed, "        \"at_ms\": 1053,\n") &&
                    contains(failed, "        \"code\": \"scenario-failed\",\n        \"message\": \"" + form.failure + "\"\n"),
                form.what + ": the step fails at the step of the ECU that was wrong:\n" + failed);
        require(contains(lf(seen.standard_error), " --start-timeout-ms ") &&
                    contains(lf(seen.standard_error), " --sample-every-step\n"),
                form.what + ": the ECU is started with --sample-every-step:\n" + lf(seen.standard_error));

        // samples: as_sent: the ECU is started as without a scenario, the step passes on the
        // samples of its code, and the check notes what that means.
        write_file(dir / "s.yaml", "vehicle_sil_scenario: 1\nsamples: as_sent\nsteps:\n" + form.step);
        const Captured coarse = launch(o, run, dir);
        expect_code(coarse, 0, form.what + " with samples: as_sent");
        const std::string out = lf(coarse.standard_output);
        const std::string passed = scenario_json(out);
        require(contains(passed, "    \"status\": \"passed\",\n") && contains(passed, "        \"at_ms\": " + form.as_sent + ",\n"),
                form.what + " with samples: as_sent passes:\n" + passed);
        require(contains(out, "\"code\": \"scenario-samples-as-sent\"") &&
                    contains(out, "\"code\": \"coarse-sample-period\"") && contains(out, "\"notes\": 2,") &&
                    !contains(lf(coarse.standard_error), "--sample-every-step"),
                form.what + " with samples: as_sent: the notes, and no argument:\n" + out + lf(coarse.standard_error));
    }

    // An ECU that does not follow the argument (its --describe says that it takes it): the
    // scenario is not begun, before any step, and that is a failure with its own code.
    write_file(dir / "test-ecu-own-sample-period", "\n");
    write_file(dir / "s.yaml", "vehicle_sil_scenario: 1\nsteps:\n  - set: { Body.level: 5 }\n" + forms[0].step);
    const Captured refused = launch(o, run, dir);
    expect_code(refused, 1, "an ECU that sends its values less often than its step");
    const std::string out = lf(refused.standard_output);
    const std::string scenario = scenario_json(out);
    require(contains(scenario, "    \"status\": \"failed\",\n    \"over_at_ms\": null,\n") &&
                !contains(scenario, "\"status\": \"passed\"") && !contains(scenario, "\"status\": \"running\"") &&
                contains(scenario, "        \"kind\": \"set\",\n        \"text\": \"set Body.level = 5\",\n        \"status\": \"not_run\",\n"),
            "the scenario failed and no step was run:\n" + scenario);
    require(contains(out, "\"result\": \"failed\"") && contains(out, "\"ok\": false") &&
                contains(out, "\"code\": \"scenario-not-observed\"") && !contains(out, "\"code\": \"scenario-incomplete\"") &&
                contains(out, "the scenario was not begun: it reads values of participant 'Body' and judges with every step of the "
                              "ECU, and 'Body' sends its values every 10 ms (its sample_period) while its step is 1 ms") &&
                contains(out, "It was started with --sample-every-step and did not follow it") &&
                contains(out, "or write samples: as_sent at the top of the scenario"),
            "the finding of the run:\n" + out);
    require(contains(lf(refused.standard_error),
                     "\n[vehicle_sil_launch] scenario: not begun: the scenario reads values of Body, which sends them less often than "
                     "every step of its own (scenario-not-observed)\n") &&
                !contains(lf(refused.standard_error), "scenario: line ") &&
                contains(lf(refused.standard_error),
                         "\n[vehicle_sil_launch] the scenario was not begun: the simulation was stopped before its first step\n") &&
                !contains(lf(refused.standard_error), "the scenario is over"),
            "the lines on stderr, and no step's line:\n" + lf(refused.standard_error));
    // For people: the run's line says that the scenario was not begun, not that it was over.
    const Captured told = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml"}, dir);
    expect_code(told, 1, "the same run, for people");
    require(contains(lf(told.standard_output), "\nrun: the simulation was stopped before its first step: the scenario was not begun\n") &&
                !contains(lf(told.standard_output), "until the scenario was over") &&
                contains(lf(told.standard_output), "\nscenario s.yaml: FAILED, 0 of 2 steps passed\n"),
            "the run's line:\n" + lf(told.standard_output));
    // The same ECU with samples: as_sent runs, and judges as its samples allow.
    write_file(dir / "s.yaml", "vehicle_sil_scenario: 1\nsamples: as_sent\nsteps:\n  - set: { Body.level: 5 }\n" + forms[0].step);
    const Captured allowed = launch(o, run, dir);
    expect_code(allowed, 0, "the same ECU with samples: as_sent");
    require(contains(scenario_json(lf(allowed.standard_output)), "    \"status\": \"passed\",\n"), lf(allowed.standard_output));

    // The limit: an ECU that does not follow the argument and whose code sends its values every
    // 10 ms, with a step of 10 ms, sends one for every step by itself. It is not refused.
    write_file(dir / "test-ecu-step-ms", "10\n");
    write_file(dir / "s.yaml", "vehicle_sil_scenario: 1\nsteps:\n  - set: { Body.level: 5 }\n"
                               "  - at_ms: 100\n    expect: { Body.healthy: true, Body.level: 5 }\n    hold_for_ms: 50\n");
    const Captured enough = launch(o, run, dir);
    expect_code(enough, 0, "an ECU whose own samples are as far apart as its steps");
    require(contains(scenario_json(lf(enough.standard_output)), "    \"status\": \"passed\",\n") &&
                !contains(lf(enough.standard_output), "scenario-not-observed") &&
                contains(lf(enough.standard_error), " --sample-every-step\n"),
            "it runs and passes:\n" + lf(enough.standard_output) + lf(enough.standard_error));
    fs::remove(dir / "test-ecu-own-sample-period");
    fs::remove(dir / "test-ecu-step-ms");
}
