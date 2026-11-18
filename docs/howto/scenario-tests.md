# Write tests with scenarios

[日本語](../ja/howto/scenario-tests.md)

In this guide you write tests as scenarios and run them. A scenario is a YAML file that sets ECU variables and checks the results. `vehicle_sil_launch` starts the simulation, runs the scenario to the end and returns the result as its exit code, so you can use it in CI as is.

A scenario judges results by virtual time only. With the same configuration file and the same scenario, the time and value at which each step passes are the same in every run, however fast the computer is.

Each entry in a scenario's `steps` is a scenario step; in this guide, "step" on its own means a scenario step. "Step 1" to "Step 8" are the numbered parts of this guide. The unit by which an ECU advances virtual time is an "ECU step".

## Prerequisites

| Requirement | Details |
|---|---|
| Build | You have built with the `linux-gcc-control` preset, as described in "[Build from the repository](../../README.md#build-from-the-repository)" in the README. |
| Example ECUs | This guide uses Engine and Dashboard from [examples/ecu_main](../../examples/ecu_main). The build puts them, with their configuration file, in `build/linux-gcc-control/examples/ecu_main/`. |

Run these 2 lines in the root of the repository. Run every later command in the directory they change to, the one that contains `simulation.yaml`.

```sh
export PATH="$PWD/build/linux-gcc-control/tools/launch:$PATH"   # where vehicle_sil_launch is
cd build/linux-gcc-control/examples/ecu_main
```

Some things differ in other environments:

| Environment | What differs |
|---|---|
| You built another preset | Replace `linux-gcc-control` in the 2 lines above with the name of that preset. Scenarios also work in a build without the Control API. |
| Windows | Run the same commands in the MSYS2 UCRT64 shell. For how to build, see [build.md](../build.md). |
| Template project | Run the commands in the directory that contains `simulation.yaml`. Add the tools' `bin` directory to `PATH` as in step 1 of [Create your first ECU](first-ecu.md#step-1-build-and-run-the-template). `scenarios/` contains only `throttle_step.yaml`, so create `engine_disconnect.yaml` from the block in this guide. |

## Overview

| Step | What you do | What you get |
|---|---|---|
| 1 | Run the bundled scenario | A table that shows that all 6 steps passed |
| 2 | List the available variables | The variable names to write in a scenario |
| 3 | Write your own scenario, check it and run it | `my_tests/throttle_up.yaml` |
| 4 | Run a failing scenario, read the finding and fix it | `my_tests/response_time.yaml` |
| 5 | Move an input and check values the whole time | `my_tests/throttle_motion.yaml` |
| 6 | Use parameters and variants | `my_tests/dashboard_speed.yaml`, `simulation.variants.yaml` |
| 7 | Run all scenarios in a directory at once | A table with the result of each run (variant and scenario) |
| 8 | Run in CI | An exit code and JSON |

Do Steps 1–8 in order. [examples/scenario_tests](../../examples/scenario_tests) contains the finished files of Steps 3–6. Read the sections that follow, "Disconnect an ECU from the bus" and "Pitfalls", when you need them.

## Step 1: Run the bundled scenario

```sh
vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml
```

### Verify the bundled scenario

The last line on stdout is `passed:` and the exit code (`echo $?`) is 0. The last line counts the findings of both the configuration file check and the scenario (if the scenario fails, it is `failed:`).

<!-- vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

scenario scenarios/throttle_step.yaml (throttle step): 6 steps

run: the simulation ran until the scenario was over (705 ms) and ended

scenario scenarios/throttle_step.yaml (throttle step): passed, 6 of 6 steps passed

line  step                                                              result  at
8     set Engine.engine.throttle = 50                                   ok      0 ms
10    wait_until Engine.engine.speed ge 1700 (timeout 500 ms)           ok      330 ms
12    expect Engine.engine.speed near 1800 (tolerance 10) (for 100 ms)  ok      490 ms
15    expect Dashboard.dashboard.engine_speed ge 1700 (within 200 ms)   ok      490 ms
17    ramp Engine.engine.throttle to 0 over 200 ms                      ok      700 ms
18    expect Engine.engine.speed le 1000 (within 100 ms)                ok      700 ms

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

traffic: J1939_CAN: 1 participant sent, 1 received (2 counted) (steps before 705 ms)

network    participant  sent  received
J1939_CAN  Engine          8         0
J1939_CAN  Dashboard       0         8

passed: 0 errors, 0 warnings, 0 notes
```

| Part of the output | How to read it |
|---|---|
| The first 2 tables | The result of checking the configuration file against each ECU. |
| The `run:` line | The simulation stopped at 705 ms, when the scenario ended. |
| The `line` column | The line number in the scenario file. |
| The `at` column | The virtual time at which the step's result was decided. |

Stderr shows the output of the ECUs, and a line each time the result of a step is decided (`[vehicle_sil_launch] scenario: line 10: ...: ok at 330 ms`).

### What the scenario contains

Here is `scenarios/throttle_step.yaml` without its comments. It sets the throttle to 50 %, waits for the engine speed to rise, checks that the dashboard ECU has received it, then brings the throttle back. On an `expect`, `within_ms: 200` means that the condition must hold at least once within 200 ms, and `hold_for_ms: 100` that it must hold for the whole 100 ms (step 3 has the details).

<!-- examples/ecu_main/scenarios/throttle_step.yaml -->
```yaml
vehicle_sil_scenario: 1
name: throttle step
steps:
  - at_ms: 0
    set: { Engine.engine.throttle: 50 }
  - wait_until: { Engine.engine.speed: { ge: 1700 } }
    timeout_ms: 500
  - after_ms: 50
    expect: { Engine.engine.speed: { near: 1800, tolerance: 10 } }
    hold_for_ms: 100
  - expect: { Dashboard.dashboard.engine_speed: { ge: 1700 } }
    within_ms: 200
  - ramp: { Engine.engine.throttle: { to: 0, over_ms: 200 } }
  - expect: { Engine.engine.speed: { le: 1000 } }
    within_ms: 100
```

The steps run from top to bottom. Each step has one action (`set`, `wait_until`, `expect`, `ramp` or `wave`) and a time.

| How to give the time | Meaning |
|---|---|
| `at_ms: 0` | Runs at virtual time 0 ms. |
| `after_ms: 50` | Runs 50 ms after the previous step ends. |
| Neither | Runs when the previous step ends. |

### Troubleshoot the bundled scenario

| Symptom | Cause | Fix |
|---|---|---|
| `vehicle_sil_launch` is not found | You are in a new shell and have not added it to `PATH`. | Run the 2 lines in "Prerequisites" in the root of the repository. |
| An error that ends in `[executable]` | The ECU executables are missing. | Build as the README describes. |
| `simulation.yaml` cannot be read (exit code 3) | You are in the wrong directory. | Run the `cd` in "Prerequisites". |

## Step 2: List the available variables

A scenario reads and writes the variables that the ECUs declare. The output of `check --json` lists their names and types in `participants[].variables`.

```sh
vehicle_sil_launch check simulation.yaml --json
```

### Verify the variable list

The `variables` of Engine include this entry:

<!-- scenario guide: check simulation.yaml --json, one variable -->
```json
        {
          "name": "engine.throttle",
          "type": "real",
          "causality": "input",
          "unit": "%",
          "min": 0,
          "max": 100,
          "step": null,
          "description": "accelerator pedal position",
          "items": [],
          "builtin": false
        },
```

In a scenario, write a variable as `<participant name>.<variable name>`. This one is `Engine.engine.throttle`. The variables of the 2 example ECUs:

| Name in a scenario | Type | `causality` | Description |
|---|---|---|---|
| `Engine.engine.throttle` | real (0–100 %) | input | The throttle. The engine speed moves toward 800 + 20 × throttle rpm. |
| `Engine.engine.speed` | integer (rpm) | output | The engine speed. |
| `Dashboard.dashboard.engine_speed` | integer (rpm) | output | The engine speed that the dashboard ECU received over J1939. |
| `Dashboard.dashboard.engine_lost` | bool | output | True if the dashboard ECU has not received the engine speed for 300 ms. |
| `Engine.ecu.connected`, `Dashboard.ecu.connected` | bool | input | Every ECU has this variable. Writing false disconnects the ECU from the bus. |

| `causality` | What a scenario can do |
|---|---|
| input | Write it with `set`, `ramp` and `wave`; read it with `expect` and `wait_until`. |
| output | Read it only. |
| parameter | Write it only with a `set` at time 0. |

## Step 3: Write a scenario

Write a scenario that checks that the engine speed reaches 1800 rpm when the throttle is at 50 %. Create the directory `my_tests`, then create `my_tests/throttle_up.yaml` with the content below.

```sh
mkdir my_tests
```

<!-- examples/scenario_tests/my_tests/throttle_up.yaml -->
```yaml
vehicle_sil_scenario: 1
name: throttle up
steps:
  - set: { Engine.engine.throttle: 50 }
  - wait_until: { Engine.engine.speed: { ge: 1700 } }
    timeout_ms: 500
  - expect: { Engine.engine.speed: { near: 1800, tolerance: 50 } }
    hold_for_ms: 100
```

| Key | What it does |
|---|---|
| `set` | Sets a variable to a value. |
| `wait_until` and `timeout_ms` | Waits until the condition holds. If it does not hold within `timeout_ms`, the step fails. |
| `expect` alone | Checks that the condition holds at that time. |
| `expect` and `within_ms` | Checks that the condition holds at least once within that time. As soon as it holds, the scenario moves on to the next step. |
| `expect` and `hold_for_ms` | Checks that the condition holds for the whole time. |

A condition is either a plain value (the variable must equal it) or one of `ge`, `le`, `gt`, `lt`, `eq`, `ne`, and `near` with `tolerance`. For all keys and conditions, see "File format" in [scenario.md](../scenario.md#file-format).

`wait_until` and an `expect` with `within_ms` both wait until the condition holds. Use `wait_until` to wait for the preconditions of a test, and `expect` for the response you want to test. They fail with different finding codes, `scenario-timeout` and `scenario-failed`, so the result shows which of the two failed. Keep the waits short enough for the scenario to end before `stop_at_ms` of the configuration file (1000 ms in `simulation.yaml`); otherwise the run fails with `scenario-out-of-time`.

Before you run the scenario, check it with `check --scenario`. It finds errors in variable names, types, value ranges and the order of times without starting the simulation.

```sh
vehicle_sil_launch check simulation.yaml --scenario my_tests/throttle_up.yaml
vehicle_sil_launch run simulation.yaml --scenario my_tests/throttle_up.yaml
```

### Verify your scenario

After the 2 tables, the `check` output ends with these 3 lines:

<!-- scenario guide: check simulation.yaml --scenario my_tests/throttle_up.yaml, the end -->
```text
scenario my_tests/throttle_up.yaml (throttle up): 3 steps

passed: 0 errors, 0 warnings, 0 notes
```

The exit code of `run` is 0, and its output from the `run:` line on is:

<!-- scenario guide: run simulation.yaml --scenario my_tests/throttle_up.yaml, from the line run: -->
```text
run: the simulation ran until the scenario was over (445 ms) and ended

scenario my_tests/throttle_up.yaml (throttle up): passed, 3 of 3 steps passed

line  step                                                              result  at
4     set Engine.engine.throttle = 50                                   ok      0 ms
5     wait_until Engine.engine.speed ge 1700 (timeout 500 ms)           ok      330 ms
7     expect Engine.engine.speed near 1800 (tolerance 50) (for 100 ms)  ok      440 ms

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

traffic: J1939_CAN: 1 participant sent, 1 received (2 counted) (steps before 445 ms)

network    participant  sent  received
J1939_CAN  Engine          6         0
J1939_CAN  Dashboard       0         6

passed: 0 errors, 0 warnings, 0 notes
```

### Troubleshoot your scenario

If you misspell a variable name, `check` reports it with the line number and how to fix it. Try it with a copy that has a misspelled name:

```sh
sed 's/engine.throttle/engine.throttel/' my_tests/throttle_up.yaml > mistake.yaml
vehicle_sil_launch check simulation.yaml --scenario mistake.yaml
```

The exit code is 1 and the output ends like this. `run --scenario` does the same check first, and starts nothing if it finds an error.

<!-- scenario guide: check simulation.yaml --scenario mistake.yaml, the end -->
```text
error: mistake.yaml:4: 'Engine.engine.throttel': participant 'Engine' has no variable 'engine.throttel': did you mean 'Engine.engine.throttle'? (vehicle_sil_launch check --json lists the variables) [scenario-variable]

failed: 1 error, 0 warnings, 0 notes
```

| Code | Cause | Fix |
|---|---|---|
| `scenario-variable` | The participant has no variable with that name. | Use a name from the output of Step 2. |
| `scenario-not-writable` | The step writes to an output variable. | Write to an input variable. |
| `scenario-value` | The value does not fit the variable's type or range (`min`, `max`). | Use a value within the range. |
| `scenario-time` | An `at_ms` comes after a wait step (`wait_until`, or `expect` with `within_ms`). | Use `after_ms` instead. The time at which a wait step ends is known only when the scenario runs. |

For all finding codes, see "Check (`check --scenario`, start of `run --scenario`)" in [scenario.md](../scenario.md#check-check---scenario-start-of-run---scenario).

## Step 4: Read a failure and fix it

Write a scenario that checks that the engine speed reaches 1700 rpm within 200 ms after the throttle is set. Create `my_tests/response_time.yaml` with this content:

<!-- scenario guide: my_tests/response_time.yaml with within_ms: 200 -->
```yaml
vehicle_sil_scenario: 1
name: response time
steps:
  - set: { Engine.engine.throttle: 50 }
  - expect: { Engine.engine.speed: { ge: 1700 } }
    within_ms: 200
  - expect: { Engine.engine.speed: { le: 1800 } }
    hold_for_ms: 100
```

```sh
vehicle_sil_launch run simulation.yaml --scenario my_tests/response_time.yaml
```

### Verify the fix

This scenario fails. The exit code is 1, and from the `run:` line on, the output is:

<!-- scenario guide: run simulation.yaml --scenario my_tests/response_time.yaml with within_ms: 200, from the line run: -->
```text
run: the simulation ran until the scenario was over (205 ms) and ended

scenario my_tests/response_time.yaml (response time): FAILED, 1 of 3 steps passed

line  step                                                result   at
4     set Engine.engine.throttle = 50                     ok       0 ms
5     expect Engine.engine.speed ge 1700 (within 200 ms)  FAILED   200 ms
7     expect Engine.engine.speed le 1800 (for 100 ms)     not run  -

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

traffic: J1939_CAN: 1 participant sent, 1 received (2 counted) (steps before 205 ms)

network    participant  sent  received
J1939_CAN  Engine          3         0
J1939_CAN  Dashboard       0         3

error: my_tests/response_time.yaml:5: expect Engine.engine.speed ge 1700 (within 200 ms): not true at any time within 200 ms (from 0 ms to 200 ms): at the end Engine.engine.speed ge 1700, but it was 1050 (its sample of 200 ms) [scenario-failed]

failed: 1 error, 0 warnings, 0 notes
```

The `error:` line tells you everything about the failure.

| Part of the `error:` line | Meaning in this example |
|---|---|
| `my_tests/response_time.yaml:5:` | The failed step is on line 5 of the file. |
| `not true at any time within 200 ms (from 0 ms to 200 ms)` | The condition did not hold even once between 0 ms and 200 ms. |
| `at the end Engine.engine.speed ge 1700` | The condition that still did not hold at the end of that time (200 ms). |
| `but it was 1050 (its sample of 200 ms)` | In the sample at 200 ms, the engine speed was 1050 rpm. A sample is the value of a variable that the ECU sends at a fixed interval, every 10 ms by default. |
| `[scenario-failed]` | The finding code. It means that the condition of an `expect` did not hold. |

When one step fails, the scenario ends there and the simulation stops too (205 ms in the `run:` line). The later steps do not run, and the table shows them as `not run`.

The ECU in this example takes 330 ms to reach 1700 rpm (the `at` in the table of Step 3). If the ECU behaves correctly, fix the test instead. Change `within_ms: 200` on line 6 to `within_ms: 400`, and run the scenario again:

```sh
sed -i 's/within_ms: 200/within_ms: 400/' my_tests/response_time.yaml
vehicle_sil_launch run simulation.yaml --scenario my_tests/response_time.yaml
```

This time it passes, and the exit code is 0.

<!-- scenario guide: run simulation.yaml --scenario my_tests/response_time.yaml, the result -->
```text
scenario my_tests/response_time.yaml (response time): passed, 3 of 3 steps passed

line  step                                                result  at
4     set Engine.engine.throttle = 50                     ok      0 ms
5     expect Engine.engine.speed ge 1700 (within 400 ms)  ok      330 ms
7     expect Engine.engine.speed le 1800 (for 100 ms)     ok      440 ms
```

### Troubleshoot failures

These 4 finding codes appear only when the scenario runs:

| Code | Cause | Fix |
|---|---|---|
| `scenario-failed` | The condition of an `expect` did not hold. | Look at the value and the time on the `error:` line, then fix the ECU or the scenario. |
| `scenario-timeout` | The condition of a `wait_until` did not hold within `timeout_ms`. | Find out why the precondition is not met, or increase `timeout_ms`. |
| `scenario-out-of-time` | The run reached `stop_at_ms` of the configuration file before the scenario ended. | Increase `stop_at_ms`, or remove it. The simulation also stops when the scenario ends. |
| `scenario-incomplete` | The run ended in the middle of the scenario: an ECU ended abnormally, or the run was ended by Ctrl+C or `--timeout-ms`. | Fix the error reported with it first. |

For the rules of the result, see "Results" in [scenario.md](../scenario.md#results).

## Step 5: Check values while an input moves

`ramp` moves a value in a straight line over a set time. `wave` moves a value along a waveform, such as a sine wave or a pulse. With an `expect` beside it, either one checks that the condition holds in every sample while the value moves.

Create `my_tests/throttle_motion.yaml` with this content:

<!-- examples/scenario_tests/my_tests/throttle_motion.yaml -->
```yaml
vehicle_sil_scenario: 1
name: throttle motion
steps:
  - set: { Engine.engine.throttle: 50 }
  - wait_until: { Engine.engine.speed: 1800 }
    timeout_ms: 500
  - ramp: { Engine.engine.throttle: { to: 80, over_ms: 200 } }
    expect: { Engine.engine.speed: { ge: 1800, le: 2400 } }
  - wave: { Engine.engine.throttle: { sine: { amplitude: 10, period_ms: 100 }, for_ms: 200 } }
    expect: { Engine.engine.speed: { le: 0xA28 } }
```

| Line | What it does |
|---|---|
| 7–8 | Raises the throttle to 80 % over 200 ms, and checks that the engine speed stays within 1800–2400 rpm the whole time. |
| 9–10 | Moves the throttle in a sine wave from 10 % below to 10 % above 80 % for 200 ms, and checks that the engine speed stays at or below 2600 rpm the whole time. |
| `0xA28` on line 10 | 2600 in hexadecimal. You can write integer and enum values in hexadecimal. |

```sh
vehicle_sil_launch run simulation.yaml --scenario my_tests/throttle_motion.yaml
```

### Verify the moving input

The exit code is 0, and the output contains this table. In the `ramp` and `wave` rows, the text after `expecting` is the condition that was checked while the value moved.

<!-- scenario guide: run simulation.yaml --scenario my_tests/throttle_motion.yaml, the result -->
```text
scenario my_tests/throttle_motion.yaml (throttle motion): passed, 4 of 4 steps passed

line  step                                                                                                           result  at
4     set Engine.engine.throttle = 50                                                                                ok      0 ms
5     wait_until Engine.engine.speed eq 1800 (timeout 500 ms)                                                        ok      350 ms
7     ramp Engine.engine.throttle to 80 over 200 ms, expecting Engine.engine.speed ge 1800, le 2400                  ok      560 ms
9     wave Engine.engine.throttle sine amplitude 10 period 100 ms for 200 ms, expecting Engine.engine.speed le 2600  ok      760 ms
```

`ramp` and `wave` do not move on to the next step until the movement ends. To check the result after the movement, put an `expect` in the next step. The last 2 steps of `throttle_step.yaml` in Step 1 do this, and so does `scenarios/throttle_wave.yaml` of the repository (the template does not have it).

To play recorded values into an input, save them as a CSV file and read it from `points`. The first row holds the column names and the first column holds the time in ms. Put the file in the scenario's directory or below it, and name the file and the value column (`value`):

```yaml
  - wave: { Engine.engine.throttle: { points: { file: throttle_trace.csv, value: throttle } } }
```

The repository's [examples/ecu_main/scenarios/throttle_trace.yaml](../../examples/ecu_main/scenarios/throttle_trace.yaml) does this (the template does not have it). For the rules, see "Reading points from a CSV file" in [scenario.md](../scenario.md#reading-points-from-a-csv-file).

### Troubleshoot ramps and waves

| Symptom | Cause | Fix |
|---|---|---|
| The step fails with `not true during the ramp (from ... to ...): at 430 ms ...` | The condition did not hold in one of the samples during the movement. | Look at the time and the value shown, then fix the ECU or the condition. |
| `within_ms` or `hold_for_ms` on the `expect` beside `ramp` gives a check error | The interval that an `expect` beside `ramp` checks is fixed: it is the time during which the value moves. | To check over another interval, put the `expect` in the next step. |
| The check reports the warning `scenario-wave-undersampled` for a `wave` | The waveform changes faster than the ECU steps. | Make the period longer. If this is intended, write `undersampled: ok` in that step. |
| `0x1F` in quotes gives a check error | `"0x1F"` is a string. | Remove the quotes. |

For reference:

| To find | See |
|---|---|
| The waveform types (`sine`, `square`, `pulse`, `noise` and others) and how to write them | "Waveforms (`wave`)" in [scenario.md](../scenario.md#waveforms-wave) |
| The samples that an `expect` beside `ramp` or `wave` looks at | "Checking values during a ramp or wave" in [scenario.md](../scenario.md#checking-values-during-a-ramp-or-wave) |
| Where you can write hexadecimal | "File format" in [scenario.md](../scenario.md#file-format) |

## Step 6: Use parameters and variants

Turn values in a scenario into parameters to run the same scenario with different values. With a configuration file that has variants (`variants`), `only` limits the variants that a scenario runs in.

First, create a configuration file with variants. Copy `simulation.yaml` and add 2 variants at the end. `full` uses both ECUs as they are; `engine_only` disables Dashboard.

```sh
cp simulation.yaml simulation.variants.yaml
cat >> simulation.variants.yaml <<'EOF'
variants:
  full: {}
  engine_only:
    participants:
      Dashboard: { enabled: false }
EOF
```

The 5 lines added:

<!-- examples/scenario_tests/simulation.variants.yaml: the lines added -->
```yaml
variants:
  full: {}
  engine_only:
    participants:
      Dashboard: { enabled: false }
```

Create `my_tests/dashboard_speed.yaml` with this content:

<!-- examples/scenario_tests/my_tests/dashboard_speed.yaml -->
```yaml
vehicle_sil_scenario: 1
name: dashboard speed
only: [full]
params: { throttle: 50, speed: 1800 }
steps:
  - set: { Engine.engine.throttle: "${throttle}" }
  - expect: { Dashboard.dashboard.engine_speed: { near: "${speed}", tolerance: 50 } }
    within_ms: 600
```

| Line | Meaning |
|---|---|
| `only: [full]` | Runs this scenario only in the variant `full`. It cannot run in `engine_only`, which has no Dashboard. |
| `params: { ... }` | The parameters and their default values. |
| `"${throttle}"` | The same as writing the value of the parameter `throttle` in its place. Inside `{ }`, put it in quotes. |

With a configuration file that has variants, select the variant with `--variant`. The first command runs with the default values; the second changes them with `--param`.

```sh
vehicle_sil_launch run simulation.variants.yaml --variant full --scenario my_tests/dashboard_speed.yaml
vehicle_sil_launch run simulation.variants.yaml --variant full --scenario my_tests/dashboard_speed.yaml --param throttle=30 --param speed=1400
```

### Verify parameters and variants

Both commands exit with code 0. The output of the second has this line and this table. The `parameters:` line shows where each value came from.

<!-- scenario guide: run --variant full --scenario my_tests/dashboard_speed.yaml --param throttle=30 --param speed=1400, the parameters -->
```text
parameters: throttle = 30 (--param), speed = 1400 (--param)
```

<!-- scenario guide: run --variant full --scenario my_tests/dashboard_speed.yaml --param throttle=30 --param speed=1400, the result -->
```text
line  step                                                                              result  at
6     set Engine.engine.throttle = 30                                                   ok      0 ms
7     expect Dashboard.dashboard.engine_speed near 1400 (tolerance 50) (within 600 ms)  ok      310 ms
```

If you select the variant `engine_only`, the launcher starts nothing, exits with code 1 and prints the error below.

```sh
vehicle_sil_launch run simulation.variants.yaml --variant engine_only --scenario my_tests/dashboard_speed.yaml
```

<!-- scenario guide: run --variant engine_only --scenario my_tests/dashboard_speed.yaml, the error -->
```text
error: my_tests/dashboard_speed.yaml:3: the scenario is only for the variant full (its only), not for engine_only: run it with --variant full, or add engine_only to only [scenario-variant]
```

### Troubleshoot parameters and variants

| Symptom | Cause | Fix |
|---|---|---|
| `the file defines variants ...: give --variant <name>` (exit code 4) | The configuration file has variants, and you did not give `--variant`. | Give `--variant` one of the variant names in the message. |
| `--param x=1: the scenario has no parameter x` (exit code 4) | You passed a parameter whose name is not in the scenario. | Correct the name. |
| A scenario with `only` gives an error when you run it with `simulation.yaml` | A configuration file without variants cannot run a scenario that has `only`. | Run it with a configuration file that has variants. |

You can also set parameter values in the `params` of the configuration file and in the `params` of each variant. These sections cover them, and how to write scenarios for variants whose executables or variable names differ:

| To find | See |
|---|---|
| Where you can write `${name}`, and the order in which values are decided | "Parameters (`params`, `${name}`)" in [scenario.md](../scenario.md#parameters-params-name) |
| What happens to a step that uses a variable the variant does not have | "Skipping steps that use an absent variable" in [scenario.md](../scenario.md#skipping-steps-that-use-an-absent-variable) |
| How to write `variants` in the configuration file | "Variants (`variants`, `--variant`)" in [simulation-config.md](../simulation-config.md#variants-variants---variant) |

## Step 7: Run several scenarios at once

`test` runs the scenarios in a directory one at a time, for each variant of the configuration file, and puts all the results in one table.

```sh
vehicle_sil_launch test simulation.variants.yaml --scenarios my_tests/
```

### Verify the test run

The exit code is 0 and stdout is:

<!-- scenario guide: test simulation.variants.yaml --scenarios my_tests/ -->
```text
variant      scenario                       result
full         my_tests/dashboard_speed.yaml  passed
full         my_tests/response_time.yaml    passed
full         my_tests/throttle_motion.yaml  passed
full         my_tests/throttle_up.yaml      passed
engine_only  my_tests/dashboard_speed.yaml  skipped  the scenario is only for full (its only)
engine_only  my_tests/response_time.yaml    passed   1 warning
engine_only  my_tests/throttle_motion.yaml  passed   1 warning
engine_only  my_tests/throttle_up.yaml      passed   1 warning

passed: 8 runs, 7 passed, 0 failed, 1 skipped
```

| Part of the table | How to read it |
|---|---|
| Row order | Variants in the order of the configuration file, scenarios in the order of their file names. |
| `skipped` | The variant is not in the scenario's `only`, so the scenario did not run. This does not count as a failure. |
| `1 warning` | A warning about the configuration file: in `engine_only`, Engine is the only ECU on `J1939_CAN` (`single-participant`). |

If any run fails, the exit code is 1. Try it by changing the parameter `speed` to a value that the engine never reaches:

```sh
vehicle_sil_launch test simulation.variants.yaml --scenarios my_tests/ --param speed=3000
```

<!-- scenario guide: test simulation.variants.yaml --scenarios my_tests/ --param speed=3000 -->
```text
variant      scenario                       result
full         my_tests/dashboard_speed.yaml  FAILED   line 7: expect Dashboard.dashboard.engine_speed near 3000 (tolerance 50) (within 600 ms): not true at any time within 600 ms (from 0 ms to 600 ms): at the end Dashboard.dashboard.engine_speed near 3000 (tolerance 50), but it was 1800 (its sample of 600 ms)
full         my_tests/response_time.yaml    passed
full         my_tests/throttle_motion.yaml  passed
full         my_tests/throttle_up.yaml      passed
engine_only  my_tests/dashboard_speed.yaml  skipped  the scenario is only for full (its only)
engine_only  my_tests/response_time.yaml    passed   1 warning
engine_only  my_tests/throttle_motion.yaml  passed   1 warning
engine_only  my_tests/throttle_up.yaml      passed   1 warning

to see a failed run in full, run it alone: vehicle_sil_launch run simulation.variants.yaml --speed max --variant full --scenario my_tests/dashboard_speed.yaml --param speed=3000

FAILED: 8 runs, 6 passed, 1 failed, 1 skipped
```

When a run fails, the remaining runs still run. The command on the `to see a failed run in full` line reruns only the failed run, as `test` ran it (`--speed max`: as fast as possible). If the file has an enabled `role: bridge` participant, `test` runs at speed 1 and the command has no `--speed max` (see [Time of a run that is connected outside](../simulation-run.md#time-of-a-run-that-is-connected-outside---time-policy)).

| Goal | How |
|---|---|
| Run only one variant | Add `--variant full`. |
| Run selected files | List the files after `--scenarios`. |
| Stop at the first failure | Add `--fail-fast`. |
| Run with a configuration file without variants | Write the command the same way: `test simulation.yaml --scenarios scenarios/`. |

For the rules of `test`, see "Running several scenarios (`vehicle_sil_launch test`)" in [simulation-run.md](../simulation-run.md#running-several-scenarios-vehicle_sil_launch-test).

## Step 8: Run in CI

The exit code gives the result, for `check`, `run` and `test` alike.

| Exit code | Meaning |
|---|---|
| 0 | Passed. |
| 1 | A step failed, or the check found an error. With `--strict`, a warning also gives 1. |
| 3 | The configuration file cannot be read. |
| 4 | The arguments are wrong. |
| 5 | `run` only: a `stop` command stopped the scenario before its end, so it has no verdict (see "Exit codes" in [simulation-run.md](../simulation-run.md#exit-codes)). |
| 130 | The run was interrupted, for example with Ctrl+C. |

To read the failures from a program, add `--json`. The result is written to stdout as one JSON document.

```sh
vehicle_sil_launch test simulation.variants.yaml --scenarios my_tests/ --param speed=3000 --json > result.json
```

### Verify the CI job

The exit code is 1. `result.json` starts with the overall result, and `runs` lists the result of each run (variant and scenario). Here are the overall result and the first run, taken from that file:

<!-- scenario guide: test --json, an excerpt -->
```json
{
  "vehicle_sil_test": 1,
  "file": "simulation.variants.yaml",
  "result": "failed",
  "passed": 6,
  "failed": 1,
  "skipped": 1,
  "runs": [
    {
      "variant": "full",
      "scenario": "my_tests/dashboard_speed.yaml",
      "result": "failed",
      "reason": "line 7: expect Dashboard.dashboard.engine_speed near 3000 (tolerance 50) (within 600 ms): not true at any time within 600 ms (from 0 ms to 600 ms): at the end Dashboard.dashboard.engine_speed near 3000 (tolerance 50), but it was 1800 (its sample of 600 ms)"
    }
  ]
}
```

The `run` key of each entry in `runs` holds the JSON that `run --json` prints for that run. Its `scenario.steps[]` has the `line`, `status`, `code` and `message` of each step.

In CI, these options are also useful:

| Option | Used for |
|---|---|
| `--timeout-ms <ms>` | Sets a limit in wall-clock time. If an ECU hangs inside an ECU step, virtual time stops advancing and the run never ends. In `test`, the limit applies to each run. |
| `--strict` | Treats warnings as failures. |
| `--log-dir <dir>` | With `run`, keeps the output of each ECU in files. |
| `--events` | With `run`, writes the progress as one JSON object per line. The `scenario_step` event is emitted when a step starts and when its result is decided. |

| To find | See |
|---|---|
| The keys of `scenario` in `run --json` | "Output" in [scenario.md](../scenario.md#output) |
| All arguments and exit codes | "Arguments and exit codes" in [simulation-config.md](../simulation-config.md#arguments-and-exit-codes) |
| The events of `--events` | [simulation-events.md](../simulation-events.md) |

## Disconnect an ECU from the bus

Every ECU has the input `ecu.connected`. Write `false` to it, and the ECU disconnects from all buses while it keeps running. Write `true` to reconnect it. With this, you can test whether the other ECUs detect that the ECU they communicate with is gone.

Here is `scenarios/engine_disconnect.yaml` without its comments. It disconnects Engine at 300 ms and checks that `dashboard.engine_lost` of Dashboard becomes true within 400 ms. Then it reconnects Engine and checks that the value returns to false within 200 ms.

<!-- examples/ecu_main/scenarios/engine_disconnect.yaml -->
```yaml
vehicle_sil_scenario: 1
name: engine disconnect
steps:
  - at_ms: 300
    set: { Engine.ecu.connected: false }
  - expect: { Dashboard.dashboard.engine_lost: true }
    within_ms: 400
  - set: { Engine.ecu.connected: true }
  - expect: { Dashboard.dashboard.engine_lost: false }
    within_ms: 200
```

```sh
vehicle_sil_launch run simulation.yaml --scenario scenarios/engine_disconnect.yaml
```

### Verify the disconnection

The exit code is 0, and the output contains this table:

<!-- scenario guide: run simulation.yaml --scenario scenarios/engine_disconnect.yaml, the result -->
```text
scenario scenarios/engine_disconnect.yaml (engine disconnect): passed, 4 of 4 steps passed

line  step                                                             result  at
9     set Engine.ecu.connected = false                                 ok      300 ms
11    expect Dashboard.dashboard.engine_lost eq true (within 400 ms)   ok      510 ms
13    set Engine.ecu.connected = true                                  ok      520 ms
14    expect Dashboard.dashboard.engine_lost eq false (within 200 ms)  ok      610 ms
```

The `line` column counts lines in the file including its comments.

The code of the Dashboard ECU does the detection; the scenario only reads its output. The Dashboard in this example sets `engine_lost` to true when it has not received the engine speed for 300 ms. Choose `within_ms` to match how long you allow the detection to take.

| To find | See |
|---|---|
| What keeps running inside an ECU while it is disconnected | "Disconnecting from the bus (`ecu.connected`)" in [internals/execution.md](../internals/execution.md#disconnecting-from-the-bus-ecuconnected) |
| How J1939 behaves on disconnect and reconnect | "Disconnect an ECU from the bus (`ecu.connected`)" in [j1939-guide.md](../j1939-guide.md#disconnect-an-ecu-from-the-bus-ecuconnected) |

## Pitfalls

The values that a scenario sees are the samples that each ECU sends every `sample_period` (10 ms of virtual time by default). The first 2 rows of this table follow from that.

| Symptom | Cause | Fix |
|---|---|---|
| An `expect` at the same time as a `set` sees the value from before the `set`, and fails | The ECU step that starts at that time may not take a sample. The check cannot detect this. | Use an `expect` with `within_ms` to check that the value was applied. |
| A short pulse is not seen, with either `within_ms` or `hold_for_ms` | A value that changes and changes back between 2 samples does not appear in the condition. | Make the ECU's `sample_period` smaller (`set_agent_options()` in "ECU variables" in [control-api.md](../control-api.md#ecu-variables)). |
| An `after_ms` after a wait step does not match the time counted from the `at` in the table | A wait step ends a little after the time of the sample in which the condition held (`at`). | Leave a margin in the times. For how the time is decided, see "Timing" in [scenario.md](../scenario.md#timing). |
| `--scenario cannot be used with --start-paused` (exit code 4) | A scenario itself writes the values at time 0 when it runs. | Remove `--start-paused`. |
| A client connected with `--control` cannot write values while a scenario runs | While a scenario runs, a client can only read, wait and stop. | To check values while you change them, use the methods in [Read and write signals from outside](operate-signals.md). |
| The variables of a participant cannot be used in a scenario | The variables of a disabled participant, a participant with `describe: false` and a participant that runs in real time cannot be used. | See "Incompatible options and limitations" in [scenario.md](../scenario.md#incompatible-options-and-limitations). |

For the rules of samples, see "Values that conditions see" in [scenario.md](../scenario.md#values-that-conditions-see).

## Notes

If you use a different version of `vehicle_sil_launch`, the `features` in the output of `vehicle_sil_launch version --json` tell you what it supports.

| `features` value | Feature |
|---|---|
| `scenario` | `--scenario` |
| `scenario-wave` | The `wave` step |
| `scenario-motion-expect` | `expect` beside `ramp` and `wave` |
| `scenario-points-file` | `points` from a CSV file |

## What to read next

- Look up the scenario format, timing, results and finding codes: [scenario.md](../scenario.md)
- Check values interactively while you change them, and pause and step time: [Read and write signals from outside](operate-signals.md)
- Run your ECU with ECUs that other people built: [Integrate and run several ECUs](integrate-ecus.md)
