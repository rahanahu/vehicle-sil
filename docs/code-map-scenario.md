# Scenarios, timelines and batch runs

[日本語](ja/code-map-scenario.md)

This is the map for changes in this area. Start at [code-map.md](code-map.md); common test rules are in [code-map-testing.md](code-map-testing.md).

Related specifications and change areas: [scenario.md](scenario.md) / [simulation-run.md](simulation-run.md) / [Launcher, process, configuration and run](code-map-launcher.md) / [Control API, gRPC and launcher Control Server](code-map-control.md)

**On this page:** [Code and tests by directory](#code-and-tests-by-directory) / [What to update together](#what-to-update-together)

**Change topics:** [Variants and scenario parameters](#variants-and-scenario-parameters) / [Scenarios](#scenarios) / [Timeline files](#timeline-files) / [Running several scenarios](#running-several-scenarios)

## Code and tests by directory

### `examples/scenario_tests` (example for the guide "Write tests with scenarios")

The files that the guide [howto/scenario-tests.md](howto/scenario-tests.md) has users write. The files have the same content as the guide's blocks; users write them themselves, following the guide, in the build directory of `examples/ecu_main`.

| File | Details |
|---|---|
| `my_tests/throttle_up.yaml`, `my_tests/response_time.yaml`, `my_tests/throttle_motion.yaml`, `my_tests/dashboard_speed.yaml` | 4 scenarios |
| `simulation.variants.yaml` | A configuration file with 2 added variants |

The comments at the top of the files are not in the guide's blocks. The line numbers in the guide's output are those of the files without the comments.

- Build: none (the executables are the 2 of `examples/ecu_main`).
- `ctest -R`: `^launch_run\.scenario_guide`

`launch_run.scenario_guide` (`tests/launch/scenario_guide.cmake`) does the following in a working directory, in the order of the guide:

- Compares each file with the guide's block, then writes it
- Compares the output and exit codes of `check`, `run` and `test`
- Compares the keys of the `test --json` excerpt
- Also compares the blocks and output of `throttle_step.yaml` and `engine_disconnect.yaml` in `examples/ecu_main/scenarios`

## What to update together

### Variants and scenario parameters

For configuration or schema changes, see [configuration file or scenario keys](code-map-launcher.md#configuration-file-or-scenario-keys).

`variants` and `params` of the configuration file, `--variant`, `--param`, and `params`, `${name}` and `only` of scenarios. This also covers skipping steps that use a variable with `absent: true` or a variable of a participant that the variant disabled.

Where to change (configuration file side, `libs/launch/src/config.cpp`):

- `read_variants()`
- `kVariantKeys`, `kVariantParticipantKeys` (the lists of keys that a variant can change)
- `apply_variant()`, `choose_variant()`

Where to change (scenario side and around it):

- How `params` is read: `read_params()` in `yaml_reader.hpp`
- `take_params()`, `resolve_params()`, `read_only()` and `skip()` in `scenario.cpp`
- The code that passes them on: `ScenarioContext` in `check.cpp`
- The code that skips: `issue_step()` in `scenario_run.cpp` (`StepStatus::Skipped`)
- Arguments: `common()` in `tools/launch/main.cpp`

Documentation to update:

- "[Variants (`variants`, `--variant`)](simulation-config.md#variants-variants---variant)" and "[Scenario parameters (`params`, `--param`)](simulation-config.md#scenario-parameters-params---param)" in [simulation-config.md](simulation-config.md)
- "[Variants and parameters](scenario.md#variants-and-parameters)" and the `code` table in [scenario.md](scenario.md)
- [simulation-check.md](simulation-check.md): the `code` table and the JSON keys (`variant`, `variants`, `params`)
- [simulation-run.md](simulation-run.md)
- "[Events when the run does not start](simulation-events.md#events-when-the-run-does-not-start)" in [simulation-events.md](simulation-events.md)

When you add an item that a variant can change, update the key lists, `VariantParticipant`, `read_variant_participants()`, `apply_variant()` and the table in simulation-config.md together.

Tests:

- `launch.a_variant_*`, `launch.every_variant_*`
- `launch.scenario_parameter*`, `launch.scenario_only_*`
- `launch.scenario_steps_that_use_*`, `launch.scenario_times_are_the_same_*`, `launch.scenario_that_checks_nothing_*`
- `launch_run.variants`

### Scenarios

`run --scenario` and `check --scenario`.

Where to change:

| What | Location |
|---|---|
| Reading and checking | `libs/launch/src/scenario.cpp` (the file, parameters, variants, steps), `scenario_values.cpp` (variables and values), `scenario_waves.cpp` (waves and points), `scenario_inputs.cpp` (`inputs` and dithers). The class `Reader` is in `scenario_reader.hpp` |
| Advancing the steps | `libs/launch/src/scenario_run.cpp` |
| Why a step failed: the conditions not met and the writes not applied, in the message and in the record (`failed.conditions[]`, `failed.writes[]`) | `libs/launch/src/scenario_failure.cpp` (`unmet()`, `unmet_values()`, `failed_write()`, `applied_text()`, `during()`) |
| The functions that compute `ramp` and `wave` values (implementations of `Waveform`) | `libs/launch/src/scenario_shapes.cpp` |
| Reading `points` from a CSV file | `read_csv()` in `libs/launch/src/scenario_csv.cpp` splits the file into cells. `files_beside()` reads it from the scenario file's directory. `path_problem()`, `file_points()` and `cell_value()` in `scenario_waves.cpp` check the path and convert the cells |
| Waveform calculation | `signal/waveform.hpp` in `libs/signal` |
| Integration into the launcher | `RunScenario` in `libs/launch/src/run_scenario.cpp` (`begin()`, `loop()`, `finish()`) |
| The time gate's guard | `SimulationController::guard_from()` and `wait_guarded()` |
| The tap that receives every sample | `control::Hub::set_values_tap()` |
| The run's tap, from which the scenario takes its samples | `RunSamples` in `libs/launch/src/run_samples.cpp` |
| Reading `dither` (top-level `inputs`, the step) | `read_inputs()`, `read_dither()` and `read_overlay()` in `scenario_inputs.cpp`. The waveform is read by `read_shape()`, shared with `read_wave_of()` |
| The findings `scenario-dither-*` and the list of what is placed on the inputs (`Scenario::lines`) | `note_lines()` and `check_dithers()` in `scenario_inputs.cpp` |
| The writes of a dither in the engine | In `scenario_run.cpp`: `submit()` (one write per ECU step while no motion runs; it belongs to no step, and writes for the same time are combined into one request), `place()` (places the dither on the line), `fail_grid()` (when such a write is refused, fails the first step not yet decided). The contract is "Dithers" in the comment of `ScenarioEngine` |

Documentation to update:

- [scenario.md](scenario.md): the format, times, how values are seen, writes, results, the table of finding codes, the output
- [scenario.md](scenario.md): the block after `<!-- vehicle_sil_launch run simulation.yaml --scenario ... -->` (`launch_run.scenario` compares it byte for byte)
- [scenario.md](scenario.md): the blocks of `<!-- throttle_mistakes.yaml -->` and `<!-- throttle_response.yaml -->`
- [scenario.md](scenario.md): the block after `<!-- vehicle_sil_launch check simulation.yaml --scenario scenarios/throttle_dither.yaml: the lines -->` (`launch_run.scenario_dither` compares it)
- [howto/scenario-tests.md](howto/scenario-tests.md): the files, output and exit codes that the guide shows (`launch_run.scenario_guide` runs them in the guide's order and compares)
- [simulation-config.md](simulation-config.md): the table of arguments
- [simulation-events.md](simulation-events.md): the `scenario_step` event, `scenario` of `features`
- "[Scenarios (`vehicle_sil_launch run --scenario`)](internals/control.md#scenarios-vehicle_sil_launch-run---scenario)" in [internals/control.md](internals/control.md)

Example files:

- The example file that is compared is `examples/ecu_main/scenarios/throttle_step.yaml`.
- The `wave` example is `examples/ecu_main/scenarios/throttle_wave.yaml`, and `launch_run.scenario_wave` runs it.
- The `dither` example is `examples/ecu_main/scenarios/throttle_dither.yaml`, and `launch_run.scenario_dither` runs it.
- The example of `points` from a CSV file is `examples/ecu_main/scenarios/throttle_trace.yaml` with `throttle_trace.csv`, and `launch_run.scenario_wave` runs it.
- The guide howto/scenario-tests.md shows `throttle_step.yaml`, `engine_disconnect.yaml` and the files of `examples/scenario_tests` (see "[`examples/scenario_tests`](#examplesscenario_tests-example-for-the-guide-write-tests-with-scenarios)").
- For the examples with errors and the failing examples, the files too are blocks in scenario.md. `launch_run.scenario` writes them out and compares the output of `check --scenario`, the output of `run --scenario` and `scenario` in `--json`.

Rules (the specification is in the comment of `ScenarioEngine` in `scenario_run.hpp`):

- Only virtual time decides results. Real-time waits and timeouts do not decide a result.
- The values of `ramp` and `wave` are functions only of the virtual time since the start of the step (`Waveform::value()`). They must not depend on how many times or in what order they are called.
- Random values are built from the seed, the variable name and the time (`seed_of_variable()` in `scenario_shapes.cpp`). The calculation uses integers only and is the same on every OS.
- The line of each input (the value that the scenario placed, and the `ramp` or `wave` moving it now) is `Line` in `libs/launch/src/scenario_line.hpp`. The start value of a motion without `from` or `around` is read from the line in step order, before the writes at the step's time are sent (`place()` in `scenario_run.cpp`).
- One function, `motion_number()`, gives the values of a motion before rounding. The value to write is made in one place, `Line::written()` of the line, which adds the dither and then rounds to the type. The engine's `submit_one()` calls it. Code that you add to compute values calls the same function.
- Whether the ECU sees a value (a short `pulse`, the `points` of `hold`) follows one rule, `seen_between()` in `scenario_waves.cpp`.
- `read_scenario()` reads no file except through `ScenarioContext::read_file` (`check.cpp` and `test.cpp` pass `files_beside()`).
- Inline points and points from a CSV file both go through `read_points()`, so they get the same checks.
- In controller step P, only the samples and results of steps that started before P are looked at. Those from P on are handled in the next step, even if they have already been received.
- A write at time T is queued while the controller step before T is held (time 0 is the start window).
- In a run with a scenario, the controller's time step does not depend on the speed (`fine` of `detail::plan_time_step()`).
- The Hub's tap is called on the controller's SIL Kit thread, inside the Hub's lock. Do not add anything that waits or that calls the Hub.

What to update together when you add a waveform:

- The function in `signal/waveform.hpp`
- The implementation in `scenario_shapes.cpp`
- `read_wave_of()` in `scenario_waves.cpp` (keys, types, ranges, comparison with the step size)
- `definitions.wave` in `schema/scenario.schema.json` (and the copy in `template/schema`)
- `WAVE_SHAPES_WITH_KEYS` and the samples in `tests/schema/test_schemas.py`
- The waveform table in [scenario.md](scenario.md)

Tests:

- `^launch\.scenario_`: with a fake ECU model, the result is the same when the receive order changes
- `launch.scenario_wave_*`: waveform values, values after the waveform ends, findings
- `launch.scenario_line_*` (`tests/launch/test_scenario_line.cpp`): the input lines alone
- `launch.scenario_dither_*`, `launch.timeline_has_the_dithers_*` (the reference is `tests/launch/timeline/dither.yaml`)
- `launch.scenario_expect_beside_*`: the interval of an `expect` beside `ramp` and `wave`, the order in which steps are decided, skipping only the `expect` by variant
- `^signal\.`
- `launch_run.scenario`
- `launch_run.scenario_wave`: the time at which the ECU saw the waveform, passing with a different time step, the warning and `undersampled: ok`
- `launch_run.scenario_expect_beside`: the sample that failed an `expect` beside `ramp` or `wave`, `expect_skipped`
- `launch_run.scenario_dither`: a real ECU, out of range, the example `examples/ecu_main/scenarios/throttle_dither.yaml`
- `launch_run.scenario_same`: the same result as fast as possible, at real-time speed, with a slow ECU and with pauses. The ECU reports the step in which a write was applied.
- `launch_run.scenario_failures`
- `core.time_gate_model_the_guard_*`, `core.virtual_time_gate_guard_*`
- `control.control_hub_tap_*`

### Timeline files

`run --scenario --timeline` (see "Recording a scenario run on a timeline (`--timeline`)" in [simulation-run.md](simulation-run.md#recording-a-scenario-run-on-a-timeline---timeline)).

Where to change:

| What | Location |
|---|---|
| The record, its limits and the rule that drops the older points | `TimelineRecord` in `libs/launch/src/timeline.cpp` |
| Where the engine records | In `scenario_run.cpp`: `submit_one()` (writes), `observe()` (samples), `place()` (the input lines `lines[]`; the contents of `shape` are `ScenarioMotion::described`, which the scenario's reader fills when it reads the scenario), `issue_step()` (watches), `fail()` (what failed; what it says comes from `scenario_failure.cpp`), `close_record()` (the end of the record, `recorded_to_ms`) |
| The file format | `format_timeline_json()` in `timeline.cpp` |
| One point per line | `JsonWriter::begin_array_on_one_line()` in `libs/json/include/vehicle_sil/json/json_writer.hpp` |
| Opening and writing the file | `Runner::open_timeline()` and `write_timeline()` in `run.cpp` |
| The findings `timeline-write` and `timeline-limit` | `run_assess.cpp` |
| The argument | `tools/launch/main.cpp` |
| The element `<vehicle-sil-timeline>` that draws the file | `gui/timeline/viewer.js` (the part above "The element" needs no DOM and is what the unit tests call; below it, the drawing functions work on the element that `enter()` makes current, and the class `VehicleSilTimelineElement` with its attributes, properties and events). Its CSS is `gui/timeline/timeline.css` (in its shadow root; `hide`, `compact` and `theme` are read there) |
| The one file that users load | `gui/timeline/vehicle-sil-timeline.js`, made by `gui/timeline/bundle.js` from `messages.js`, `timeline.css` and `viewer.js` (`npm run bundle:timeline` in `gui/web`; `npm run test:timeline` fails when it is out of date). Its types: `gui/timeline/vehicle-sil-timeline.d.ts` (by hand: keep it in step with the class) |
| The timeline page | `gui/timeline/timeline.html` and `page.js`: one element, and what the page adds (`?src=`, `?lang=`, `#token=`, a file dropped or chosen, a file embedded). The smallest page of a user: `gui/timeline/example.html` |
| The words (English and Japanese) | `gui/timeline/messages.js`: every text the element and the page show, by key, in both languages (the unit tests check the keys; the lint refuses a string of words in `viewer.js` and `page.js`). The page says the element's `load.*` texts in its own words (`pageLoad.*`) through the element's `messages` property |
| The GUI server: `--timeline` for a scenario run, the files it keeps, the page and the file it serves | `gui/server/src/vehicle_sil_gui/timelines.py`, and `_start()`, `_keep_timeline()`, `get_timeline()` and `get_timeline_page()` in `server.py` |
| The **Timeline** link of the sample screen | `ScenarioPanel` and `timelinePageUrl()` in `gui/web/src/app/run/scenario-panel.ts` |
| The timeline inside the sample screen (the model for Angular) | `TimelineView` in `gui/web/src/app/run/timeline-view.ts` |
| The element in the npm package and the tools archive | `gui/client/pack.mjs` (`@vehicle-sil/gui-client/timeline`), `TIMELINE_ELEMENT` in `scripts/package.py` (`client/`) |
| A new kind of lane, or a new state of a sent value | `laneKind()` and the branch in `variableLane()`, the state in `pointState()` and `holdSegments()`, a CSS class in `timeline.css` (then `npm run bundle:timeline`), and `KNOWN` for a value the page should draw instead of naming |

What to update together:

- `schema/timeline.schema.json`
- "Recording a scenario run on a timeline (`--timeline`)" in [simulation-run.md](simulation-run.md#recording-a-scenario-run-on-a-timeline---timeline)
- The reference files `tests/launch/timeline/*.timeline.json`. Do not edit them by hand; the `README.md` in that directory says how to write them again and which keys are not compared.
- `gui/timeline/viewer.js`, when the page should draw a new key or value. Until then it ignores the key, or names the value without drawing it; a new format version or `stage` makes it refuse the file.

Rules:

- Recording happens only on the scenario thread (and in `begin()` on the thread of the start window). Nothing is recorded inside the Hub's tap.
- Without a recorder (no `--timeline`), the engine and the output of `run` do not change at all.
- The file holds only what virtual time decides (no real time, process IDs or lateness counts).
- Keys and values are only ever added, and readers must be able to ignore what they do not know (the schema does not close values with `enum`).
- The reference scenarios use no `sine`, `chirp` or `lag`. Their last bit can differ between C libraries, so they cannot be compared byte for byte.

Tests:

- `launch.timeline_*`: independence from the time step, steps that change with the time step, the reference files, the limits, how samples are counted, which writes of a failed `set` were applied and which point each is, a run that ended partway
- `launch.a_timeline_that_dropped_points_or_was_not_written_is_said`
- `launch_run.timeline` (the real launcher): the same file from 2 runs and at another speed, a file that cannot be opened, a file that cannot be written, a failing run, runs that end at `stop_at_ms` and by `--timeout-ms`, a run that does not start, the arguments
- `schema.files_and_launcher`
- The page, on the reference files and on its fixtures `gui/timeline/tests/fixtures/` (what the references do not have; `tests/schema/test_schemas.py` holds them to the schema): `gui/timeline/tests/viewer.test.js` (reading, the model and the layout, on Node: `npm run test:timeline` in `gui/web`) and `gui/web/e2e/timeline.spec.ts` (the page in a browser), `gui/web/e2e/timeline-element.spec.ts` (the element on pages of a user: the plain example, two on a page, every property and event, CSS in and out, a policy with no inline style). Not in CTest: `scripts/verify-web.py` runs them (`--e2e-grep 'timeline page'`, `--e2e-grep 'timeline element'`).
- The GUI server's part: `gui/server/tests/test_timelines.py` (the argument with and without the feature, the file and the page with and without the token, the files kept), on the recordings `run-scenario-*-timeline` (made with `--timeline`; `record_launcher.py --compare` compares them with the real launcher). The link and the page from the GUI, and the element inside the sample screen: `gui/web/e2e/timeline-link.spec.ts` (`--e2e-grep 'timeline'`); in the screen template: `gui/web/e2e/starter.spec.ts`.

### Running several scenarios

`vehicle_sil_launch test`. Each run is a plain `run`, so a change to `run --json` also changes `runs[].run`.

Where to change (`libs/launch/include/vehicle_sil/launch/test.hpp` and `src/test.cpp`):

| Function or member | Details |
|---|---|
| `list_scenarios()` | How scenarios are collected from a directory, and their order |
| `test_file()` | Calls `run_file()` for each run |
| `format_test_text()` | The command that reruns a single run takes its arguments from `TestArguments` |
| `format_test_json()` | `kTestFormat` |
| `TestReport::remarks()` | The findings for `test` as a whole: `test-nothing-run` and `test-runs-left` |
| `TestReport::result()` | Result and exit code |
| `ScenarioCheck::not_for_variant` | Marks a run that does not match `only` as `skipped` |
| `CheckOptions::params_for_many` | Relaxes the check of `--param` |
| `CheckOptions::file_only` | Reads only the configuration file |
| `test()` and `spread_scenarios()` in `tools/launch/main.cpp` | Arguments |

Before `run_file()`, `test_file()` reads the configuration file, checks the variants, and rejects both a `--param` that no scenario uses and any enabled manual participant.

Documentation to update:

- "[Running several scenarios (`vehicle_sil_launch test`)](simulation-run.md#running-several-scenarios-vehicle_sil_launch-test)" in [simulation-run.md](simulation-run.md) (the table, exit codes, JSON keys)
- [simulation-config.md](simulation-config.md): the table of arguments and the table of exit codes
- The usage text of `vehicle_sil_launch`: `kUsage`

Tests:

- `launch.the_scenarios_of_a_directory_*`, `launch.a_test_*`, `launch.the_report_of_a_test_*`, `launch.the_command_of_one_run_alone_*` (`tests/launch/test_batch.cpp`; they start no processes)
- `launch_run.test_command`, `launch_run.test_interrupt`
