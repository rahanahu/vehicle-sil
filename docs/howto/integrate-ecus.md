# Integrate and run several ECUs

[日本語](../ja/howto/integrate-ecus.md)

This guide shows how to list ECU executables built by different people in one configuration file and run them as one simulation. You find mismatches between the configuration file and the ECUs with `check`, ask the ECU authors to fix what the configuration file cannot, run the simulation with `run`, and verify that the ECUs communicate. You do not read the ECU source code.

The example has 2 ECUs. The engine ECU sends the engine speed over J1939, and the dashboard ECU receives it. The dashboard ECU as first delivered calls the shared bus by a different name than the engine ECU does. Steps 1–5 find and fix this mismatch. [examples/integrate_ecus](../../examples/integrate_ecus) contains the example files.

## Prerequisites

| Requirement | Details |
|---|---|
| Build | You have built the repository as described in "[Build from the repository](../../README.md#build-from-the-repository)" in the README. |
| ECU executables | Built with `ecu_main()`. To build one, see [Create your first ECU](first-ecu.md). |

Run every command in the directory below. In the repository root, run these 2 lines. `linux-gcc-control` is the preset from the README; if you built another preset, use its name instead.

```sh
export PATH="$PWD/build/linux-gcc-control/tools/launch:$PATH"   # where vehicle_sil_launch is
cd build/linux-gcc-control/examples/integrate_ecus
```

The build puts these files in that directory:

| File | Contents | Used in step |
|---|---|---|
| `simulation.yaml` | The configuration file you write first | 2, 3 |
| `simulation.two-buses.yaml` | `simulation.yaml` with the network name of the dashboard ECU added to `networks` | 3, 5 |
| `simulation.fixed.yaml` | `simulation.yaml` with the dashboard ECU replaced by the rebuilt one | 4, 5 |
| `scenarios/engine_speed.yaml` | A scenario that verifies that the dashboard ECU receives the engine speed | 5 |
| `../ecu_main/engine/vehicle_sil_engine_ecu` | The engine ECU | 2–5 |
| `dashboard_v1/vehicle_sil_dashboard_v1_ecu` | The dashboard ECU as first delivered | 2, 3 |
| `../ecu_main/dashboard/vehicle_sil_dashboard_ecu` | The rebuilt dashboard ECU | 4, 5 |

With the template and the prebuilt tools, the locations differ as shown below. The steps are the same; follow them with the ECUs you received.

| | Repository build (this guide) | Template and prebuilt tools |
|---|---|---|
| Working directory | `build/<preset>/examples/integrate_ecus` | The directory that contains your `simulation.yaml` |
| `vehicle_sil_launch` | `build/<preset>/tools/launch/` | Add it to `PATH` with the `export PATH=...` line that `get_tools.py` prints. |

## Overview

| Step | What you do | What you get |
|---|---|---|
| 1 | Agree on network names and other names, and share them with the ECU authors | A list of names |
| 2 | Collect the executables and write the configuration file | `simulation.yaml` |
| 3 | Check the configuration with `check` and read the findings | Where to fix each finding |
| 4 | Ask the ECU authors to fix the mismatches that the configuration file cannot fix | Rebuilt executables and a `check` with no findings |
| 5 | Run the simulation with `run` and verify that the ECUs communicate | Exit code 0 and the received values |

Do steps 1–5 in order. "Step 1" to "Step 5" are the parts of this guide; anywhere else, a "step" is a simulation step, unless the text says "scenario step". Read the remaining sections when you need them.

| Goal | Use | Section |
|---|---|---|
| Run without an ECU you have not received yet | `--disable` | [Run with an ECU disabled](#run-with-an-ecu-disabled-enabled---disable) |
| Stop a run partway, or limit a run that does not end | Ctrl+C, `--timeout-ms` | [Stop the simulation](#stop-the-simulation) |
| Read the result from a script or CI | `--json`, `--events` | [Use from scripts](#use-from-scripts---json---events) |
| Run in step with real time | `--speed` | [Run at real-time speed](#run-at-real-time-speed---speed-and-read-the-lateness) |
| Run 2 instances of the same ECU | Arguments in `command` | [Run two instances of the same ECU](#run-two-instances-of-the-same-ecu) |
| Start tools not built with `ecu_main()` together with the ECUs | `describe: false` | [Add tools not built with `ecu_main()`](#add-tools-not-built-with-ecu_main) |

## Step 1: Agree on names and share them

Before anyone starts building ECUs, decide the items in the table below and share them with every ECU author. For how to choose when in doubt, see [Decisions to make before you start](design-choices.md). All of them are values in the ECU code: if they do not match after you receive the executables, the ECU must be rebuilt.

| Decision | Rule | Finding code if they do not match |
|---|---|---|
| Network name and kind | All ECUs on the same bus use the same name and kind (`can`, `lin`, `ethernet`). | `unknown-network`, `kind-mismatch`, `single-participant` |
| J1939 address | Each address is unique within a network. | `address-conflict`, `address-shared` |
| J1939 NAME | Each ECU has a different value. | `name-conflict` |
| Step size | The ECU author decides it and tells the integrator (the person who writes the configuration file). | `step-mismatch` |

The configuration file cannot change a network name. An ECU connects to a bus by naming it in its code, as in `ecu.j1939("J1939_CAN")`. In this version, the configuration file has no way to map the name an ECU uses to a different name. If 2 ECUs use different names, one of them must be rebuilt.

The example uses these decisions:

| ECU | Network | J1939 address | Step size |
|---|---|---|---|
| Engine | `J1939_CAN` (CAN) | 0x00 | 10 ms |
| Dashboard | `J1939_CAN` (CAN) | 0x28 | 5 ms |

Besides the names, ask the ECU authors for the following:

| Ask the ECU author to | If they do not |
|---|---|
| Build `main()` with `ecu_main()`. | `check` cannot check that ECU. |
| Use the agreed names, and verify them with `--describe` before delivering the executable (step 2). | The check reports the findings of step 3. |
| Build with the same vehicle-sil version as the integrator. | The check reports the note `version-mismatch`. If the format version for reading and writing variables differs, it reports the warning `control-link-version`, and you cannot read or write that ECU's variables. |
| End the processes that the ECU started itself when the ECU exits. | `run` reports the warning `left-processes`. |
| Write stdout and stderr in UTF-8. | Bytes that are not UTF-8 appear as `?`. |

The participant name (the ECU's name in the simulation) appears only in the configuration file, so you do not need to agree on it with the ECU authors. `vehicle_sil_launch` passes the name from the configuration file to the ECU as `--name`.

## Step 2: Write the configuration file

Put the executables you received under one directory and write `simulation.yaml` there. The example `simulation.yaml`:

<!-- examples/integrate_ecus/simulation.yaml -->
```yaml
# yaml-language-server: $schema=../../schema/simulation.schema.json
# Two ECUs received from their authors, in one simulation (the guide docs/howto/integrate-ecus.md).
vehicle_sil_simulation: 1
stop_at_ms: 1000
networks:
  - name: J1939_CAN
    kind: can
participants:
  - name: Engine
    command: [../ecu_main/engine/vehicle_sil_engine_ecu]
    step_ms: 10
  - name: Dashboard
    command: [dashboard_v1/vehicle_sil_dashboard_v1_ecu]
    step_ms: 5
```

| Line | What to write |
|---|---|
| The comment on line 1 | The location of the JSON Schema that editors use for completion and checking. The file works without it. |
| `vehicle_sil_simulation` | The format version of the configuration file. Write `1`. |
| `stop_at_ms` | The virtual time at which the simulation stops. Without it, the simulation runs until Ctrl+C. |
| `networks` | The network names and kinds agreed in step 1, one entry each. |
| `participants[].name` | The participant name. It must be unique within the simulation. |
| `participants[].command` | The path of the executable. A relative path is relative to the directory of the configuration file. |
| `participants[].step_ms` | The step size the ECU author gave you in step 1. If you write it, the check compares it with the ECU's value. You can leave it out. |

Do not write `--name` or `--registry-uri` in `command`; `vehicle_sil_launch` adds them. On Windows, the executable is found without `.exe`. For the keys and their defaults, see "The configuration file" in [simulation-config.md](../simulation-config.md#the-configuration-file).

### Find the names an ECU uses

You do not need the source code to find which network name an ECU uses: run the executable with `--describe`. The ECU prints its configuration as JSON to stdout and exits.

```sh
dashboard_v1/vehicle_sil_dashboard_v1_ecu --describe
```

Here is the part of the output that corresponds to the decisions of step 1:

<!-- integrate ECUs: vehicle_sil_dashboard_v1_ecu --describe, an excerpt -->
```json
{
  "name": "Dashboard",
  "step_ns": 5000000,
  "networks": [
    {
      "name": "VehicleCAN",
      "kind": "can",
      "use": "j1939",
      "nodes": [
        {
          "address": 40
        }
      ]
    }
  ]
}
```

| Key | Meaning | Value in this example |
|---|---|---|
| `networks[].name`, `kind` | The network name and kind that the ECU uses | `VehicleCAN`, CAN |
| `networks[].nodes[].address` | The J1939 address (decimal) | 40 (0x28) |
| `step_ns` | The step size (nanoseconds) | 5 ms |

The network name is `VehicleCAN`, not `J1939_CAN` as agreed in step 1. The ECU author has to fix this mismatch; step 4 shows how to write the request. First, leave the configuration file as it is and go to step 3 to see how `check` reports the mismatch. For all JSON keys, see "JSON format" in [ecu-executable.md](../ecu-executable.md#json-format).

## Step 3: Check the configuration and read the findings

```sh
vehicle_sil_launch check simulation.yaml
```

`check` starts each ECU with `--describe`, one at a time, and checks the configuration file against it. It does not run the simulation. The ECU constructors do run, so do not pass executables or configuration files that you do not trust.

### Verify the check output

In the example, the check reports 2 findings and the last line is `failed:`. The exit code is 1.

<!-- integrate ECUs: vehicle_sil_launch check simulation.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   VehicleCAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine

error: simulation.yaml:12: participant 'Dashboard': the executable uses the network 'VehicleCAN' (j1939), which is not in networks. Another network of kind can in this simulation is 'J1939_CAN' (line 6, used by Engine): if 'VehicleCAN' is meant to be that bus, the names must be the same, and the configuration cannot rename a network, so the author of one of the ECUs changes the name in its code (the name passed to can() or j1939()) and builds it again; if 'VehicleCAN' is a bus of its own, add it to networks with kind: can [unknown-network]
warning: simulation.yaml:6: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it [single-participant]

failed: 1 error, 1 warning, 0 notes
```

The output has 4 parts, from top to bottom:

| Part | Contents |
|---|---|
| Participant table | The step size, network names and J1939 addresses that each ECU printed with `--describe` (not the values in the configuration file) |
| Network table | The `networks` of the configuration file and the participants on each |
| Findings | One per line, in the form `<severity>: <file>:<line>: <description and fix> [<code>]` |
| Last line | The result (`passed` or `failed`) and the count for each severity |

A finding has one of 3 severities:

| Severity | Result of `check` | `run` |
|---|---|---|
| `error` | Fails (exit code 1) | Starts nothing |
| `warning` | Still passes (fails with `--strict`) | Runs |
| `note` | Still passes | Runs |

The `error` line in the example reports that `VehicleCAN`, which the dashboard ECU uses, is not in `networks`. It gives 2 ways to fix this:

| If `VehicleCAN` is meant to be | Fix | Where to fix |
|---|---|---|
| The same bus as `J1939_CAN` | Make the 2 ECUs use the same name. | The ECU code (the configuration file cannot do this) |
| A separate bus | Add `VehicleCAN` to `networks`. | The configuration file |

Adding it to `networks` makes `check` pass, but the 2 ECUs end up on separate buses and do not communicate (see "[If you add the name to `networks`](#if-you-add-the-name-to-networks)" below). In the example, the dashboard ECU receives the engine speed, so both names are meant to be the same bus. The fix belongs in the ECU code: go to step 4.

### If you add the name to `networks`

Adding `VehicleCAN` to `networks` is enough to clear the `error`. `simulation.two-buses.yaml` is `simulation.yaml` with `networks` changed as follows:

<!-- examples/integrate_ecus/simulation.two-buses.yaml: networks -->
```yaml
networks:
  - name: J1939_CAN
    kind: can
  - name: VehicleCAN
    kind: can
```

```sh
vehicle_sil_launch check simulation.two-buses.yaml
```

The result is `passed:` and the exit code is 0. But the network table shows the 2 ECUs on separate buses, and the check reports 2 warnings.

<!-- integrate ECUs: vehicle_sil_launch check simulation.two-buses.yaml -->
```text
simulation.two-buses.yaml: 2 participants, 2 networks

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   VehicleCAN (j1939 0x28)

network     kind  participants
J1939_CAN   can   Engine
VehicleCAN  can   Dashboard

warning: simulation.two-buses.yaml:7: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it; the network 'VehicleCAN' (line 9, only Dashboard) of kind can also has one ECU: if it is meant to be the same bus, the ECUs name it differently, and the configuration cannot rename a network, so the author of one of them changes the name in its code [single-participant]
warning: simulation.two-buses.yaml:9: network 'VehicleCAN': only Dashboard is on it, so what it sends there reaches no one: add the other participants of this network, or remove it; the network 'J1939_CAN' (line 7, only Engine) of kind can also has one ECU: if it is meant to be the same bus, the ECUs name it differently, and the configuration cannot rename a network, so the author of one of them changes the name in its code [single-participant]

passed: 0 errors, 2 warnings, 0 notes
```

Read the warnings even when the result is `passed:`. With this configuration file, `run` also ends with exit code 0, but the dashboard ECU does not receive the engine speed (step 5 shows this). To make sure no warning goes unnoticed, add `--strict`. If there are warnings, the result becomes `failed: 0 errors, 2 warnings, 0 notes (--strict)` and the exit code 1.

### Troubleshoot findings

Look up each finding by the code at the end of its line. The table lists the common codes and where to fix them. For all codes, see the table in "Checks" in [simulation-check.md](../simulation-check.md#checks).

| Code at the end of the line | Cause | Where to fix |
|---|---|---|
| `[unknown-key]`, `[missing-key]`, `[value]` | A key or value in the configuration file is wrong. | Fix the configuration file as the finding says. |
| `[executable]` | There is no executable at the path in `command`. | Correct the path in the configuration file, or put the executable there. |
| `[launcher-argument]` | `command` contains an argument that `vehicle_sil_launch` adds, such as `--name`. | Remove that argument from `command` in the configuration file. |
| `[unknown-network]`, `[kind-mismatch]` | The name or kind of a network that the ECU uses differs from `networks`. | If the configuration file differs from the names agreed in step 1, fix the configuration file. If the ECU differs, go to step 4. |
| `[single-participant]`, `[unused-network]` | A network has only one ECU, or none. | If this is because you disabled an ECU, leave it. If different names cause it, go to step 4. |
| `[step-mismatch]` | `step_ms` differs from the ECU's step size. | If the configuration file differs from the value you were given, fix the configuration file. If the ECU differs, go to step 4. |
| `[address-conflict]`, `[address-shared]`, `[name-conflict]` | J1939 addresses or NAMEs overlap on the same network. | Go to step 4. |
| `[describe-failed]`, `[describe-output]`, `[describe-timeout]` | The ECU does not respond correctly to `--describe`. | Go to step 4. For a tool not built with `ecu_main()`, write `describe: false`. |
| `[describe-version]`, `[describe-unknown-value]` | The ECU and `vehicle_sil_launch` use different vehicle-sil versions. | Update whichever one the finding names as older. |
| `[version-mismatch]`, `[control-link-version]` | The ECU was built with a different vehicle-sil version from `vehicle_sil_launch`. | Ask the ECU author to rebuild the ECU with the same vehicle-sil version. |

## Step 4: Ask ECU authors to fix what the configuration cannot

The values below are built into the ECU, and the configuration file cannot change them. If they do not match, ask the ECU author to change the code and rebuild.

| Mismatch | Whom to ask | What to ask for |
|---|---|---|
| The ECUs use different network names | The author of the ECU whose name differs from the one agreed in step 1 | Change the name passed to `j1939()`, `can()`, `lin_master()`, `lin_slave()` or `ethernet()` to the agreed name. |
| A network kind differs | The author of the ECU whose kind differs from the agreed one | Connect to a bus of the agreed kind. |
| J1939 addresses or NAMEs overlap | The author of the ECU that differs from the list of step 1 | Change the node's address or NAME to the value in the list. |
| The step size differs from the value you were given | The author of that ECU | Confirm which value is correct, and fix the ECU if it is wrong. |
| `--describe` fails | The author of that ECU | Investigate the cause, using the end of stderr that the finding includes. |

If you did not agree on the names beforehand, `check` cannot tell which ECU is right. Agree on the names now and share them with everyone, then ask the author of the ECU that differs.

A request contains these 4 items:

| Item | In the example |
|---|---|
| The finding line from `check` | Paste the `error:` line from step 3 as it is. |
| The current value and the new value | Change the network name from `VehicleCAN` to `J1939_CAN`. |
| How to verify before delivering | Check that `networks[].name` in the `--describe` output is `J1939_CAN`. |
| The vehicle-sil version to build with | The version that `vehicle_sil_launch version` prints |

The example request:

> The dashboard ECU connects to the J1939 network under the name `VehicleCAN`. The agreed name is `J1939_CAN`, and the engine ECU uses it. With different names, the 2 ECUs end up on separate buses and the dashboard ECU cannot receive the engine speed. The configuration file cannot change the name. Could you change `ecu.j1939("VehicleCAN")` in the code to `ecu.j1939("J1939_CAN")` and rebuild with vehicle-sil 0.1.0? Before you deliver it, verify that `networks[].name` in the `--describe` output is `J1939_CAN`. Here is the `check` finding: (the `error:` line from step 3)

When you receive the rebuilt executable, replace the old one and run `check` again. In the example, the rebuilt dashboard ECU is in `../ecu_main/dashboard/`, so `simulation.fixed.yaml` changes the `command` of `Dashboard` to this line:

<!-- examples/integrate_ecus/simulation.fixed.yaml: the command of the Dashboard -->
```yaml
    command: [../ecu_main/dashboard/vehicle_sil_dashboard_ecu]
```

```sh
vehicle_sil_launch check simulation.fixed.yaml
```

### Verify the rebuilt ECU

Both ECUs are on `J1939_CAN` and there are no findings. The exit code is 0.

<!-- integrate ECUs: vehicle_sil_launch check simulation.fixed.yaml -->
```text
simulation.fixed.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

passed: 0 errors, 0 warnings, 0 notes
```

While you wait for a rebuild, disable that ECU and continue with the others (see [Run with an ECU disabled](#run-with-an-ecu-disabled-enabled---disable)).

## Step 5: Run the simulation and verify communication

```sh
vehicle_sil_launch run simulation.fixed.yaml
```

`run` first does the same check as `check`. If there is no `error`, it starts the SIL Kit registry and all ECUs, runs until `stop_at_ms` and ends the ECUs.

### Verify the run result

At the end of stdout, `run` prints the result below. If every entry in the `ended` column is `exit code 0` and the last line is `passed:`, the exit code is 0.

<!-- integrate ECUs: vehicle_sil_launch run simulation.fixed.yaml, from the line "run:" -->
```text
run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

passed: 0 errors, 0 warnings, 0 notes
```

Exit code 0 tells you only that every ECU started, ran until `stop_at_ms` and exited with exit code 0. It does not tell you whether the ECUs received any messages. Run `simulation.two-buses.yaml` from step 3: the 2 ECUs are on separate buses, yet `run` prints the same table and ends with exit code 0.

```sh
vehicle_sil_launch run simulation.two-buses.yaml
```

<!-- integrate ECUs: vehicle_sil_launch run simulation.two-buses.yaml, from the line "run:" -->
```text
run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

passed: 0 errors, 2 warnings, 0 notes
```

Verify communication with one of these methods:

| Method | What to look at | Requires |
|---|---|---|
| The ECU's own output | Lines on stderr that start with `[<participant name>]` | The ECU prints the values it receives. |
| A scenario with `expect` | The result and exit code of `run --scenario` | The ECU declares the received values as variables. |
| The GUI or the Control API | Variable values during the run | The ECU declares the received values as variables. |

If an ECU neither prints nor declares the values it receives, you cannot verify reception from outside. If you need to, ask the ECU author to declare the received values as variables (see "Add J1939 communication" in [Create your first ECU](first-ecu.md#add-j1939-communication)).

### Verify with the ECU output

`run` prints the stdout and stderr of each ECU to its own stderr, with `[<participant name>] ` at the start of each line. The example dashboard ECU prints the received engine speed and the message count every 500 ms. With `simulation.fixed.yaml`, these 3 lines appear:

<!-- integrate ECUs: vehicle_sil_launch run simulation.fixed.yaml, the lines of the Dashboard -->
```text
[Dashboard] t=0 ms engine speed 0 rpm (0 messages)
[Dashboard] t=500 ms engine speed 1200 rpm (5 messages)
[Dashboard] t=1000 ms engine speed 1200 rpm (10 messages)
```

With `simulation.two-buses.yaml`, the count stays at 0 to the end:

<!-- integrate ECUs: vehicle_sil_launch run simulation.two-buses.yaml, the lines of the Dashboard -->
```text
[Dashboard] t=0 ms engine speed 0 rpm (0 messages)
[Dashboard] t=500 ms engine speed 0 rpm (0 messages)
[Dashboard] t=1000 ms engine speed 0 rpm (0 messages)
```

With `--log-dir logs`, `run` also saves the output of each ECU to `logs/<participant name>.log`, without the `[<participant name>] ` prefix.

### Verify with a scenario

A scenario is a YAML file of steps that set ECU variables and check their values. If an `expect` step does not hold, `run` ends with exit code 1, so you can verify communication in CI too. The example `scenarios/engine_speed.yaml` sets the engine throttle to 50 % and checks that the dashboard ECU's engine speed reaches 1700 rpm or more within 600 ms. The 1200 rpm in the `run` output above is the value at the initial throttle of 20 %; at 50 %, the speed rises to 1800 rpm.

<!-- examples/integrate_ecus/scenarios/engine_speed.yaml -->
```yaml
# yaml-language-server: $schema=../../../schema/scenario.schema.json
# The dashboard shows the speed the engine sends (the guide docs/howto/integrate-ecus.md).
vehicle_sil_scenario: 1
name: the dashboard receives the engine speed
steps:
  - at_ms: 0
    set: { Engine.engine.throttle: 50 }
  - expect: { Dashboard.dashboard.engine_speed: { ge: 1700 } }
    within_ms: 600
```

```sh
vehicle_sil_launch run simulation.fixed.yaml --scenario scenarios/engine_speed.yaml
```

Both scenario steps are `ok`, and the run ends with exit code 0.

<!-- integrate ECUs: vehicle_sil_launch run simulation.fixed.yaml --scenario scenarios/engine_speed.yaml, the result of the scenario -->
```text
scenario scenarios/engine_speed.yaml (the dashboard receives the engine speed): passed, 2 of 2 steps passed

line  step                                                             result  at
6     set Engine.engine.throttle = 50                                  ok      0 ms
8     expect Dashboard.dashboard.engine_speed ge 1700 (within 600 ms)  ok      410 ms
```

With `simulation.two-buses.yaml`, the same scenario fails at the `expect` step and the run ends with exit code 1.

```sh
vehicle_sil_launch run simulation.two-buses.yaml --scenario scenarios/engine_speed.yaml
```

<!-- integrate ECUs: vehicle_sil_launch run simulation.two-buses.yaml --scenario scenarios/engine_speed.yaml, from the result of the scenario -->
```text
scenario scenarios/engine_speed.yaml (the dashboard receives the engine speed): FAILED, 1 of 2 steps passed

line  step                                                             result  at
6     set Engine.engine.throttle = 50                                  ok      0 ms
8     expect Dashboard.dashboard.engine_speed ge 1700 (within 600 ms)  FAILED  600 ms

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

error: scenarios/engine_speed.yaml:8: expect Dashboard.dashboard.engine_speed ge 1700 (within 600 ms): not true at any time within 600 ms (from 0 ms to 600 ms): at the end Dashboard.dashboard.engine_speed ge 1700, but it was 0 (its sample of 600 ms) [scenario-failed]

failed: 1 error, 2 warnings, 0 notes
```

For how to write scenarios, see [Write tests with scenarios](scenario-tests.md).

### Verify with the GUI or the Control API

You can read variable values during a run in the browser GUI ([gui.md](../gui.md)) or with the Python client of the Control API (see "Read values" in [Read and write signals from outside](operate-signals.md#read-values)). In the example, if `dashboard.engine_speed` of `Dashboard` follows `engine.speed` of `Engine`, the ECUs communicate.

### Troubleshoot the run

| Symptom | Cause | Fix |
|---|---|---|
| `nothing was started: the check found errors` | The check found an `error`. | Go back to step 3 and fix the `error`. |
| Exit code 0, but the received value stays at 0 | The ECUs are on separate buses. | Read the network table and the warnings of `check` (step 3). |
| The run never ends | An ECU is stuck in the middle of a step, so virtual time does not advance. | Stop the run with Ctrl+C. In CI, set a wall-clock time limit with `--timeout-ms`. |
| Findings such as `[participant-failed]`, `[not-joined]` or `[did-not-end]` | During the run, an ECU failed, did not join or did not end. | Read the output of the participant that the finding names. |

For the finding codes of a run, see "Output" in [simulation-run.md](../simulation-run.md#output). For the conditions that abort a run, see "[Abort conditions and timeouts](../simulation-run.md#abort-conditions-and-timeouts)" in the same page.

## Run with an ECU disabled (`enabled`, `--disable`)

To leave out an ECU that you have not received yet, or one you want to rule out while you look for a cause, check and run with the remaining ECUs. Pass the participant name to `--disable`.

```sh
vehicle_sil_launch check simulation.fixed.yaml --disable Dashboard
vehicle_sil_launch run simulation.fixed.yaml --disable Dashboard
```

### Verify the disabled ECU

The table shows the disabled ECU as `disabled`, and the check reports the note `participant-disabled`.

<!-- integrate ECUs: vehicle_sil_launch check simulation.fixed.yaml --disable Dashboard -->
```text
simulation.fixed.yaml: 2 participants (1 disabled), 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    -      disabled

network    kind  participants
J1939_CAN  can   Engine

warning: simulation.fixed.yaml:7: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it [single-participant]
note: simulation.fixed.yaml:13: participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it [participant-disabled]

passed: 0 errors, 1 warning, 1 note
```

| Topic | Details |
|---|---|
| A disabled ECU | It is neither checked nor started, so its executable need not exist yet. |
| In the configuration file | Writing `enabled: false` in the participant has the same effect. |
| Enabling it again | Add `--enable <participant name>`. The command line takes precedence over the configuration file. |
| The note `participant-disabled` | It keeps a result with some participants disabled from being mistaken for a full result. |
| `--strict` | A warning caused by the disabled ECU (`single-participant` above) also makes the result fail. |
| `required: false` | Not allowed for an ECU built with `ecu_main()`. To leave such an ECU out, use `enabled: false` or `--disable`. |

For the rules, see "Disabling a participant (`enabled`, `--disable`, `--enable`)" in [simulation-config.md](../simulation-config.md#disabling-a-participant-enabled---disable---enable).

## Stop the simulation

| Method | Behavior | Exit code |
|---|---|---|
| Write `stop_at_ms` | The simulation stops at that virtual time. | 0 if there is no `error` finding |
| Ctrl+C | `vehicle_sil_launch` stops the simulation, ends the ECUs, then prints the result. | 130 |
| Ctrl+C a second time | `vehicle_sil_launch` kills the ECUs without waiting. | 130 |
| `--timeout-ms <ms>` | If the run has not ended `<ms>` of wall-clock time after the start, `vehicle_sil_launch` aborts it and reports an `error`. | 1 |

To stop the run from a program, start `run` with `--stdin-commands` and write `{"command":"stop"}` to stdin, or close stdin (see "Stdin commands (`run --stdin-commands`)" in [simulation-events.md](../simulation-events.md#stdin-commands-run---stdin-commands)).

Whichever way the run stops, `vehicle_sil_launch` ends the ECUs it started, and the processes that the ECUs started, before it exits. If an ECU does not exit after the simulation ends, `vehicle_sil_launch` kills it and reports `did-not-end` as an `error`. For the order and the wait times, see "[Stopping a run](../simulation-run.md#stopping-a-run)", "[Interrupting (Ctrl+C)](../simulation-run.md#interrupting-ctrlc)" and "[Leaving no participant behind](../simulation-run.md#leaving-no-participant-behind)" in simulation-run.md.

## Use from scripts (`--json`, `--events`)

From CI scripts or your own tools, use the exit code and the machine-readable output.

| Goal | Command | Output |
|---|---|---|
| Know only whether the run passed or failed | The exit code of `check` or `run` | 0 is pass, 1 is fail and 130 is interrupted. |
| Read the findings in a program | `vehicle_sil_launch check --json simulation.yaml` | Writes the result to stdout as one JSON document. |
| Follow the progress of a run | `vehicle_sil_launch run --events --stdin-commands simulation.yaml` | Writes what happens, in order, as one JSON object per line. |
| Find out which features are available | `vehicle_sil_launch version --json` | `features` lists the feature names. |

With `check --json`, the findings are in `findings`. Here is part of the output for the `simulation.yaml` of step 3:

<!-- integrate ECUs: vehicle_sil_launch check --json simulation.yaml, an excerpt -->
```json
{
  "result": "failed",
  "errors": 1,
  "warnings": 1,
  "findings": [
    {
      "severity": "error",
      "code": "unknown-network",
      "file": "simulation.yaml",
      "line": 12,
      "participant": "Dashboard",
      "network": "VehicleCAN"
    },
    {
      "severity": "warning",
      "code": "single-participant",
      "line": 6,
      "network": "J1939_CAN"
    }
  ]
}
```

| Topic | Details |
|---|---|
| `participants[].variables` | Part of the `check --json` output. It shows which ECU has which variables before you run. |
| First and last lines of `--events` | The first line is `hello` and the last is `result`. The content of `result` is the same as the output of `run --json`. |
| `--json` and `--events` | You cannot use them together. |
| `--stdin-commands` | When the calling program exits, stdin closes and `vehicle_sil_launch` stops too. |

The references:

| To find | See |
|---|---|
| Exit codes | "Arguments and exit codes" in [simulation-config.md](../simulation-config.md#arguments-and-exit-codes) |
| The `check --json` format | "Output" in [simulation-check.md](../simulation-check.md#output) |
| Event types and their order | [simulation-events.md](../simulation-events.md) |

From Python, the `Launch` class starts `vehicle_sil_launch` this way (see "Python" in [control-api.md](../control-api.md#python)).

## Run at real-time speed (`--speed`) and read the lateness

By default, the simulation runs as fast as the computer can calculate. To run together with devices that work in real time, or to operate the simulation while you watch a screen, set the speed with `--speed`. `1` is real-time speed.

```sh
vehicle_sil_launch run simulation.fixed.yaml --speed 1
```

### Verify the timing line

With a speed set, the result has an extra `time:` line. Here is the line for `--speed 4`; the parts between `<` and `>` differ from run to run:

<!-- vehicle_sil_launch run simulation.yaml --speed 4: the line of the time -->
```text
time: speed 4, ran at <actual_speed> times real time; <late_steps> of 200 paced steps more than 10 ms late (the latest by <max_late_ms> ms), <slipped_ms> ms behind in all
```

| Part of the line | Meaning |
|---|---|
| `ran at <actual_speed> times real time` | The actual speed over the whole run, excluding time spent paused |
| `<late_steps> of 200 paced steps more than 10 ms late` | The number of paced time steps that were later than the tolerance (`late_tolerance_ms`, default 10 ms) |
| `the latest by <max_late_ms> ms` | The lateness of the latest time step |
| `<slipped_ms> ms behind in all` | The total time the run slipped behind because of lateness (the simulation does not catch up) |

### Troubleshoot late steps

| Symptom | Cause | Fix |
|---|---|---|
| Warning `behind-real-time` | At least one time step was later than the tolerance. | Lower the speed, or disable the ECUs one at a time with `--disable` to find the slow one. |
| Warning `coarse-time-step` | `time_step_ms` is set, and an ECU's steps do not start on time-step boundaries. | Write the value from the finding in `time_step_ms`. |

For the definition of lateness, how to choose the tolerance, and how time steps relate to ECU steps, see "Running at a set speed (`speed`)" in [simulation-run.md](../simulation-run.md#running-at-a-set-speed-speed).

## Run two instances of the same ECU

If you list the same executable as 2 participants, `--name` gives them different participant names, but the J1939 NAME, the address and the variable names stay the same. If both are on the same network, `check` reports `name-conflict`. The configuration file alone cannot fix this either, so ask the ECU author to make the NAME and address configurable through arguments.

| Task | Who | Details |
|---|---|---|
| Add ECU-specific arguments to the ECU | The ECU author | In `main()`, take the ECU-specific arguments out of `argv` and pass the rest to `ecu_main()` (see "`ecu_main`" in [ecu-executable.md](../ecu-executable.md#ecu_main)). |
| Write the arguments in the configuration file | The integrator | Write them in `command`, after the executable. |

For example, the configuration file looks like this. `--address` and `--identity-number` are examples of arguments that the ECU author defines; the example engine ECU does not have them.

```yaml
  - name: Engine2
    command: [engine/vehicle_sil_engine_ecu, --address, "0x01", --identity-number, "2"]
```

| Topic | Details |
|---|---|
| How `check` handles them | Both `check` and `run` start the ECU with the arguments in `command`, so the check covers the address and NAME of the node that those arguments create. |
| Names you cannot use | The names of the `ecu_main()` arguments (see "Arguments and exit codes" in [ecu-executable.md](../ecu-executable.md#arguments-and-exit-codes)) |
| Unknown arguments | `ecu_main()` exits with exit code 4 when it sees an argument it does not know, so always take the ECU-specific arguments out in `main()`. |
| `--help` | The `--help` of `ecu_main()` does not list ECU-specific arguments, so ask the ECU author to describe them in the ECU's documentation. |

## Add tools not built with `ecu_main()`

To list executables not built with `ecu_main()`, such as logging tools or your own test programs, write `describe: false` for them. `check` does not check such an executable and reports the note `not-checked`.

| What to write | Meaning |
|---|---|
| `describe: false` | `check` and `run` do not start it with `--describe`. |
| `{registry_uri}`, `{name}` in `command` | `run` replaces them with the registry URI and the participant name. |
| `required: false` | For a tool that does not join the simulation. `run` does not wait for it to join. |
| `manual: true` | `run` does not start the tool but prints its command. You start it yourself. |

> [!WARNING]
> If a tool that joins virtual time with the SIL Kit autonomous lifecycle (such as `Peer` with its default settings) advances virtual time before the ECUs do, SIL Kit aborts the whole simulation when the ECUs start. Make such a tool coordinated and set `required: true`, or set `manual: true` and start it after the simulation has started.

For the rules, see "[Other executables (`describe: false`)](../simulation-config.md#other-executables-describe-false)" and "[Starting a participant yourself (`manual`, `--manual`)](../simulation-config.md#starting-a-participant-yourself-manual---manual)" in simulation-config.md.

## Notes

- Variable names are independent for each ECU, so the ECUs need not agree on them. Names that start with `ecu.` or `peer.` are not allowed. Avoid names that start with `j1939.<network name>.`: they can collide with the monitoring output, and on a collision the ECU exits with exit code 3 at startup. The example puts the ECU's name first (`engine.throttle`, `dashboard.engine_speed`).
- J1939 address 254 tells the ECU to pick a free address itself; `check` does not treat it as a conflict. For when an address conflict is an `error` and when it is a `warning`, see "Checks" in [simulation-check.md](../simulation-check.md#checks).
- The Manufacturer Code 0x7F0 in the examples is a placeholder.

## What to read next

- Turn value settings and checks into automated tests: [Write tests with scenarios](scenario-tests.md)
- Read and write variables during a run, and pause and step time: [Read and write signals from outside](operate-signals.md)
- Add ECUs that run on other computers: [Run on several computers](multi-pc.md)
