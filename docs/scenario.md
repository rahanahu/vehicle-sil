# Scenarios (`vehicle_sil_launch run --scenario`)

[日本語](ja/scenario.md)

This is the reference for the scenario file format and for the rules that apply when a scenario runs: timing, the values that conditions see, results, finding codes and output. To try scenarios one step at a time, start with the guide [Write tests with scenarios](howto/scenario-tests.md).

| To find | See |
|---|---|
| Keys, and how to write values and conditions | [File format](#file-format) |
| When a step starts and ends | [Timing](#timing) |
| The values that conditions see | [Values that conditions see](#values-that-conditions-see) |
| The ECU step in which a write is applied | [Writes](#writes) |
| Waveform types and keys | [Waveforms (`wave`)](#waveforms-wave) |
| `expect` beside `ramp` and `wave` | [Checking values during a ramp or wave](#checking-values-during-a-ramp-or-wave) |
| Parameters, variants and skipped steps | [Variants and parameters](#variants-and-parameters) |
| How the result is decided, and the finding codes of a run | [Results](#results) |
| The finding codes of the check | [Check (`check --scenario`, start of `run --scenario`)](#check-check---scenario-start-of-run---scenario) |
| Human-readable output and JSON keys | [Output](#output) |
| Using a scenario with `--control` | [Watching variables during a run (`--control`)](#watching-variables-during-a-run---control) |
| Options you cannot combine, and what scenarios cannot do | [Incompatible options and limitations](#incompatible-options-and-limitations) |
| Which features a launcher has | [Launcher feature names (`features`)](#launcher-feature-names-features) |

A scenario is a file that sets ECU variables in a fixed sequence of steps and checks the results. `vehicle_sil_launch run <config> --scenario <scenario>` starts the simulation, runs the scenario to its end, stops, and reports the result in its exit code. A scenario is an automated test as it stands.

With the same configuration file and the same scenario, the result is always the same: which steps passed or failed, at which virtual time and with which values. It does not depend on the speed of the computer, on whether the run is as fast as possible or at real-time speed (`speed`), or on how much work the ECUs do. Every decision uses virtual time only; there are no real-time waits or timeouts.

A scenario can use the variables that an ECU built with `ecu_main` declares, and `ecu.connected`, which every ECU has (see "Exposing variables (`--registry-uri`)" in [ecu-executable.md](ecu-executable.md#exposing-variables---registry-uri)). `participants[].variables` in the output of `vehicle_sil_launch check --json` lists their names. Scenarios do not use gRPC, so they also work in a build without the Control Server.

On this page, a "step" is one item of the scenario's `steps`. The unit by which an ECU or the controller advances virtual time once is an "ECU step" or a "controller step" (together, simulation steps).

## Example

[examples/ecu_main/scenarios/throttle_step.yaml](../examples/ecu_main/scenarios/throttle_step.yaml) is a scenario for the Engine and Dashboard ECUs of [examples/ecu_main](../examples/ecu_main).

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

```sh
vehicle_sil_launch check simulation.yaml --scenario scenarios/throttle_step.yaml   # check only
vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml
```

The run prints this to stdout. The participants' own lines and one line per step (`[vehicle_sil_launch] scenario: line 10: ...: ok at 330 ms`) go to stderr.

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

passed: 0 errors, 0 warnings, 0 notes
```

`at` is the virtual time at which the step's result was decided (see [Results](#results)). `stop_at_ms` in the configuration file is 1000 ms, but the run stops at 705 ms, when the scenario is over.

### Disconnecting an ECU from the bus

Writing `false` to `<participant>.ecu.connected` disconnects that ECU from every bus while it keeps running. The frames it sends do not reach the bus, and it receives no frames from the bus. Writing `true` reconnects it (see "Disconnecting from the bus (`ecu.connected`)" under "The `Ecu` class" in [internals/execution.md](internals/execution.md#disconnecting-from-the-bus-ecuconnected)). Use this to test that the other ECUs correctly detect that an ECU they talk to is gone.

[examples/ecu_main/scenarios/engine_disconnect.yaml](../examples/ecu_main/scenarios/engine_disconnect.yaml) disconnects Engine at 300 ms and checks that Dashboard detects the loss of the engine speed (`dashboard.engine_lost` is true when no engine speed has arrived for 300 ms). It then reconnects Engine and checks that Dashboard recovers.

```yaml
steps:
  - at_ms: 300
    set: { Engine.ecu.connected: false }
  - expect: { Dashboard.dashboard.engine_lost: true }
    within_ms: 400
  - set: { Engine.ecu.connected: true }
  - expect: { Dashboard.dashboard.engine_lost: false }
    within_ms: 200
```

## File format

A scenario is YAML (JSON if it starts with `{`). Keep it in a file of its own, separate from the configuration file. The format may still change.

### Top-level keys

| Key | Required | Meaning |
|---|---|---|
| `vehicle_sil_scenario` | Yes | The format version: `1`. |
| `name` | No | The name of the scenario. The output shows it. |
| `only` | No | The list of variants on which this scenario runs (`only: [small, medium]`). Without it, the scenario runs on every variant. See [Limiting a scenario to variants](#limiting-a-scenario-to-variants-only). |
| `params` | No | Default values of parameters (name → value). Use a parameter by writing `${name}` where a step takes a value. See [Parameters](#parameters-params-name). |
| `steps` | Yes | The list of steps (at least 1). They run in order from the top. |

### Step keys

A step has one action and a time. Only `ramp` and `wave` can also have an `expect` (see [Checking values during a ramp or wave](#checking-values-during-a-ramp-or-wave)).

| Action | Syntax | What it does |
|---|---|---|
| `set` | `set: { <variable>: <value>, ... }` | Sets the values. |
| `ramp` | `ramp: { <variable>: { to: <value>, over_ms: <ms>, from: <value> }, ... }` | Moves the value along a straight line over `over_ms`. `from` is optional. |
| `wave` | `wave: { <variable>: { <waveform>: { ... }, around: <value>, for_ms: <ms> }, ... }` | Moves the value in a given waveform (`sine`, `square` and others). See [Waveforms](#waveforms-wave). |
| `wait_until` | `wait_until: { <variable>: <condition>, ... }` | Waits until the condition holds. |
| `expect` | `expect: { <variable>: <condition>, ... }` | Checks that the condition holds at the step's time. |

| Key that goes with an action | Actions | Meaning |
|---|---|---|
| `at_ms` | Any | The time of the step (virtual time in milliseconds, 0 or more). |
| `after_ms` | Any | The time of the step, in milliseconds (0 or more) after the end of the previous step. |
| `timeout_ms: <ms>` | `wait_until` (required) | If the condition does not hold within this time, the step fails. |
| `within_ms: <ms>` | `expect` | The step passes if the condition holds at least once within this time. The scenario moves to the next step as soon as it holds. |
| `hold_for_ms: <ms>` | `expect` | Checks that the condition holds for the whole of this time. |
| `expect: { <variable>: <condition>, ... }` | `ramp`, `wave` | Checks that the condition holds all the time the value moves (from the step's time to its end). |
| `undersampled: ok` | `wave` | The check does not report the step's waveform even if it is finer than the ECU step. |

A step with neither `at_ms` nor `after_ms` is the same as `after_ms: 0`.

### Variables and values

Write a variable as `<participant>.<variable name>`. The part up to the first `.` is the participant name; the rest is the name of the variable inside the ECU. A variable name may contain `.`: the variable `engine.speed` of participant `Engine` is `Engine.engine.speed`.

Write each value to match the variable's type.

| Variable type | Value |
|---|---|
| bool | `true` or `false` |
| integer | An integer (hexadecimal is allowed) |
| real | A number (decimal only) |
| enum | The name or the value of an item (the value may be hexadecimal) |

Rules for hexadecimal (`0x1F`, `0X1F`):

| Topic | Rule |
|---|---|
| Where it is allowed | Integer and enum values: the value of `set`, `from` and `to` of `ramp`, the values of `wave`, the value of a condition, and parameters (`params`, `--param`) used in those places. |
| Where it is not allowed | Real values, times, `amplitude` and `tolerance` are decimal only. |
| Quotes | Do not quote it. `"0x1F"` is a string and gives the same error as a quoted number. |
| Sign | Do not add a sign. Write negative values in decimal. |
| Output | Step texts and results show decimal: a step that sets `0x1F` shows `= 31`. |
| Out-of-range errors | Show both hexadecimal and decimal (`is 0x65 (101), outside its range`). |

The kind of declaration decides which variables a scenario can write.

| Kind of variable | Write | Read (conditions) |
|---|---|---|
| Input | Can be written with `set`, `ramp` and `wave`. | Yes. |
| Parameter | Can be written only with a `set` at time 0 (applied before the first simulation step). | Yes. |
| Output | Cannot be written. | Yes. |

Values of `set`, `ramp` and `wave` must be within the variable's range (`min`, `max`). `ramp` works only on integer and real variables (integer values are rounded). Each waveform of `wave` works on certain types only.

### Conditions

A condition is a comparison, nothing more. A bare value means "equal". A mapping (the form `{ ge: 1700 }`) takes these operators:

| Operator | Holds when | Types |
|---|---|---|
| `eq` | Equal | Any |
| `ne` | Not equal | Any |
| `ge` | Greater than or equal | integer, real |
| `le` | Less than or equal | integer, real |
| `gt` | Greater than | integer, real |
| `lt` | Less than | integer, real |
| `near` and `tolerance` | The difference is at most `tolerance` | integer, real |

- With several operators on one variable, all of them must hold (`{ ge: 1700, le: 1900 }`).
- With several variables in one step, the conditions of all the variables must hold.

### Schema

The JSON Schema is [schema/scenario.schema.json](../schema/scenario.schema.json). In an editor that uses the YAML language server, put `# yaml-language-server: $schema=<path to the schema>` at the top of the file to get key completion, descriptions and shape checks. Line 1 of [the example file](../examples/ecu_main/scenarios/throttle_step.yaml) does this. The path is relative to the directory of the file you are writing.

| Topic | Details |
|---|---|
| What the schema checks | Only the shape of keys and values. |
| What the schema does not check | Whether the variables exist, whether values fit their type and range, and the order of times. `check --scenario` checks these (see [Check](#check-check---scenario-start-of-run---scenario) below). |
| Scenarios written in JSON | Bind them to the schema the same way as a configuration file (see "Editor completion" in [simulation-config.md](simulation-config.md#editor-completion)). |
| Template | `schema/` contains a copy. `get_tools.py` overwrites it with the schema of the tools version it downloads. |
| Tools archive | The schema is in `schema/`. |

## Timing

The time T of a step is `at_ms`, or the end of the previous step plus `after_ms`. Before the first step, the "end of the previous step" is 0.

| Step | Ends at | Known before the run |
|---|---|---|
| `set`, `expect` (at one point) | T | Yes. |
| `ramp` | T + the longest `over_ms` | Yes. |
| `wave` | T + the duration of the longest waveform | Yes. |
| `expect` with `hold_for_ms` | T + `hold_for_ms` | Yes. |
| `wait_until`, `expect` with `within_ms` (wait steps) | One time step after P, the first controller step after the time t of the sample on which the condition held (P + time step) | Not until the run. |

The duration of a waveform is `for_ms`. For `pulse` it is `width_ms`, for `points` the time of the last point, and for `chatter` with `bounces` it is 2 × `bounces` × `interval_ms`.

A step whose end is known before the run runs at the time written, whatever the result of the previous step.

A wait step ends at P + time step for this reason: the launcher is sure to have the sample of time t at the next controller step P. The first time at which a write of the next step can still be applied is P + time step, so the wait step ends there. In the example, the sample in which the engine speed reached 1700 is at 330 ms, P is 335 ms and the step ends at 340 ms.

For this reason, a step after a wait step cannot have `at_ms` (write `after_ms`). An `at_ms` that goes back in time is also an error.

The controller's time step is `time_step_ms` in the configuration file. If it is not set, the time step is the greatest common divisor of 10 ms, `stop_at_ms` and the ECU step sizes, so every ECU step starts on a controller step (`timing.time_step_ms` in the result; the same as the default when a speed is set, see "Time steps and ECU steps" in [simulation-run.md](simulation-run.md#time-steps-and-ecu-steps)). In a run with a scenario, this time step applies whatever the speed.

## Values that conditions see

Every `sample_period` of its Agent (10 ms of virtual time by default), an ECU sends one set of the values of all its variables at the end of an ECU step: a sample. The time of a sample is the time at which that ECU step started. Conditions look at these samples.

| Condition | Samples it looks at |
|---|---|
| `expect` at one point (time T) | For each variable, the last sample of its ECU from an ECU step that started at or before T ("the value at time T"). |
| `wait_until`, `within_ms` | The value at time T and every sample received after it. A change that holds for a single sample is not missed. |
| `hold_for_ms` | The same samples. If any one of them does not satisfy the condition, the step fails at that sample's time. |
| `expect` beside `ramp` and `wave` | See [Checking values during a ramp or wave](#checking-values-during-a-ramp-or-wave). |

- Changes finer than `sample_period` are invisible. A value that changes and changes back between 2 samples never reaches the conditions.
- No condition holds for a variable that has no sample yet (the finding says `it had no value yet (no sample of its ECU had arrived)`).
- An `expect` at time 0 is an exception: it looks at the sample of the ECU step that starts at 0 ms (`its sample of 0 ms`).
- A missing sample fails a step at time 0 when a `ramp` without `from` or a `wave` without `around` moves a variable that the scenario has not written yet (`scenario-write`). Writes at time 0 are handed over before the first simulation step, so there is no sample yet to take the start value from (see [Writes](#writes)).

### `expect` at the same time as `set`

Whether the sample of time T contains the value of a `set` at time T depends on whether the ECU step that starts at T takes a sample. The ECU step that takes a sample is the first one at or after each multiple of `sample_period`. By default, that is an ECU step that starts at a multiple of 10 ms.

| ECU step that starts at T | `expect` right after the `set`, at the same time |
|---|---|
| Takes a sample (example: `at_ms: 100`) | Sees the value after the `set`. |
| Does not take a sample (example: the 105 ms step of an ECU with a 5 ms step size) | Sees the previous sample (the value before the `set`) and fails. |

To check that a value was applied, use `expect` with `within_ms`; it holds on the next sample. The launcher does not know the ECU's `sample_period`, so the check cannot detect this failure.

### Participants with a variable map (`variables`)

If a participant in the configuration file has `variables` (a variable map), the scenario uses the names and values after mapping (see [simulation-config.md](simulation-config.md#matching-variable-names-and-values-variables)). The configuration file absorbs the differences in names and units between executables, so one scenario is enough. A variable converted with a linear expression becomes a real, and the value read back can differ from the value written by the rounding. Compare such values with `near` and `tolerance`.

## Writes

A `set` is a write stamped with time T. Each ECU applies it at the start of its first ECU step that starts at or after T, and the application sees the new value in that step. If T falls between 2 ECU steps, the write is applied in the next one.

| Topic | Rule |
|---|---|
| One `set` with several variables of the same participant | They are applied together in the same ECU step. |
| When `ramp` writes | For each variable, at T, T + step, T + 2 steps, … (in steps of that ECU), it writes the value on the line from `from` to `to`. At T + `over_ms`, it writes `to`. |
| Start value of `ramp` (without `from`) | The last value the scenario wrote to that variable. |
| Start value of a variable the scenario has not written yet | The value of the latest sample. |
| Neither a written value nor a sample | The step fails (write `from`). |
| Another writer (such as the ECU itself) changed the value after the scenario wrote it | The start value does not change. |
| When the result of a write is checked | At the controller step after the ECU step that applies it. |
| A write that was not applied (the ECU's setter rejected it, the target is not connected, and so on) | The step fails. |

A write is never too late for the ECU step at its time. The launcher holds each controller step with the time gate, and lets time advance only after it has handed the ECUs every write up to the next time step. It hands over the writes for time 0 while all participants wait before the first simulation step.

## Waveforms (`wave`)

`wave` moves a variable in a given waveform. The example below moves the throttle for 200 ms in a sine wave of ±5 % around its current value, at 10 cycles per second. It is a step of [examples/ecu_main/scenarios/throttle_wave.yaml](../examples/ecu_main/scenarios/throttle_wave.yaml).

```yaml
  - wave: { Engine.engine.throttle: { sine: { amplitude: 5, period_ms: 100 }, for_ms: 200 } }
```

For each variable, write one waveform, and beside it the duration (`for_ms`) and the center (`around`). One step can have several variables (each variable once). The step ends when its longest waveform ends.

### Waveform types

| Waveform | How the value moves | Types |
|---|---|---|
| `sine` | A sine wave of center ± `amplitude`. It starts at the center and reaches its maximum after a quarter period. | integer, real |
| `triangle` | A triangle wave with its peaks and troughs at the same places as `sine`. | integer, real |
| `sawtooth` | A sawtooth wave. It is at its minimum at the start of each period, rises in a straight line to its maximum and drops back. | integer, real |
| `square` | A square wave. It is high for `duty` % of each period, from the start of the period. | integer, real, bool |
| `pulse` | The value is `value` for `width_ms` only. When the pulse ends, a number returns to the value it had when the step started, and a bool becomes the opposite of `value` (whatever its value at the start). | integer, real, bool |
| `noise` | Random values. Each value is held for `hold_ms`. | integer, real |
| `chirp` | A sine wave whose frequency changes from `from_hz` to `to_hz` over `for_ms`. | integer, real |
| `lag` | Approaches `to` as a first-order lag (63 % at `tau_ms`, 99 % at 5 times that). | integer, real |
| `chatter` | Switch bounce: `to`, the original value, `to`, the original value, … changing every `interval_ms` and ending at `to`. | integer, real, bool |
| `points` | Follows a table of times and values. | Any |

### Waveform keys

| Waveform | Key | Required | Meaning |
|---|---|---|---|
| `sine`, `triangle`, `sawtooth` | `amplitude` | Yes | The swing from the center. |
| | `period_ms` or `frequency_hz` | One of them | The period (see "Period" below). |
| | `phase_deg` | No | Where in the waveform to start. 90 starts a quarter period in. |
| `sawtooth` | `falling: true` | No | Reverses the direction. |
| `square` | `low` and `high` | These or `amplitude` | The low and the high value. |
| | `amplitude` | This or `low` and `high` | Uses the 2 values center ± `amplitude`. |
| | `period_ms` or `frequency_hz` | One of them | The period. |
| | `duty` | No | The share of the period that is high (%). The default is 50. |
| | `phase_deg` | No | Where in the waveform to start. |
| `pulse` | `value` | Yes (optional for bool) | The value during the pulse. A bool without it is `true`. |
| | `width_ms` | Yes | The length of the pulse. |
| `noise` | `amplitude` | Yes | The spread of the random values (see the `distribution` row). |
| | `hold_ms` | Yes | How long each value is held. |
| | `seed` | Yes | The random seed (see "How values are computed" below). |
| | `distribution` | No | `uniform` (the default) is a uniform distribution over center ± `amplitude`. `gaussian` is a normal distribution with a standard deviation of `amplitude` (it never goes beyond 6 standard deviations). |
| `chirp` | `amplitude` | Yes | The swing from the center. |
| | `from_hz`, `to_hz` | Yes | The start and end frequencies (0 or more). |
| | `sweep` | No | `linear` (the default) changes by the same number of Hz every second; `log` changes by the same factor every second. |
| `lag` | `to` | Yes | The value it approaches. |
| | `tau_ms` | Yes | The time constant. |
| | `from` | No | The start value. Without it, the waveform starts from the current value. |
| | `settle: true` | No | Makes the value after the waveform `to`. |
| `chatter` | `to` | Yes | The value it settles at. |
| | `interval_ms` | Yes | The interval between changes. |
| | `bounces` | This or `for_ms` beside the waveform | How many times the value returns to the original value. |
| | `seed` | No | With it, each change shifts earlier or later by up to 1/4 of `interval_ms`. |
| | `from` | No | The original value. Not for bool, where it is the opposite of `to`. |
| `points` | `[[<ms>, <value>], ...]` | Yes | The table of times (ms from the start of the step, ascending) and values. |

A bool `square` takes neither `low` and `high` nor `amplitude`: its values are `false` and `true`.

The frequency rules of `chirp` depend on `sweep`:

| `sweep` | `from_hz` and `to_hz` |
|---|---|
| `linear` | Either may be 0 Hz, so the chirp can start or end at 0 Hz (both 0 is an error). |
| `log` | Both must be greater than 0. |

Three keys go beside the waveform, at the same level:

| Key | Meaning | Waveforms |
|---|---|---|
| `for_ms` | The duration. It need not be a multiple of the period. | Not for `pulse` and `points`, which have their own duration. `chatter` takes either `bounces` or `for_ms`. |
| `around` | The center. | Only waveforms with a center (`sine`, `triangle`, `sawtooth`, `square` with `amplitude`, `noise`, `chirp`). |
| `interpolate` | How the points of `points` are joined. | `points` only. |

- **Period**: either `period_ms` (an integer, 1 or more) or `frequency_hz` (a number greater than 0). Write a period shorter than 1 ms with `frequency_hz`.
- **Center** (`around`): without it, the center is the variable's value when the step starts, decided the same way as `from` of `ramp` (see [Writes](#writes)). If there is no value, the step fails, so write `around`.
- **`pulse`** takes no `around`. When it ends, a number returns to the value at the start and a bool becomes the opposite of `value` (to set another value, `set` it in the next step). A numeric `pulse` fails if there is no value at the start, so `set` one in an earlier step.
- **Start value**: write it as `from` for `lag` and `chatter`, and as the first point for `points`.
- `${name}` can also go in a numeric position inside a waveform (`amplitude: "${amp}"`).

`interpolate` for `points`:

| `interpolate` | Between points | Types |
|---|---|---|
| `linear` (the default for numbers) | Joined by straight lines. | integer, real |
| `hold` | The value is held until the next point. | Any (`hold` only for bool and enum) |

- Write `interpolate` beside `points`, not inside it (`{ points: [...], interpolate: hold }`). It stays there even if more ways to give the points are added.
- To make the value jump, use `hold` (with `linear`, 2 points at the same time are an error).
- If the first point is after 0, the value moves in a straight line from the current value to the first point (with `hold`, it stays at the current value until then).

### Value after the waveform ends

| Waveform | Value after it ends |
|---|---|
| Waveforms with a center | Returns to the center (`around`, or the value at the start if `around` is omitted). |
| `square` with `low` and `high`, bool `square` | Ends low. |
| `pulse` | A number returns to the value at the start. A bool becomes the opposite of `value`. |
| `points` | Stays at the value of the last point. |
| `chatter` | Stays at `to`. |
| `lag` | Stays at the value reached at `for_ms` (`to` with `settle: true`). |

A `wave` without `around` or a `ramp` without `from` that follows starts from this value.

### How values are computed

A value depends only on the virtual time elapsed since the step started and on the numbers you wrote. It does not depend on the ECU step size, the controller time step or the speed of the run.

The random values of `noise` and `chatter` also come only from `seed`, the variable name (`<participant>.<variable>`) and the time. The same scenario gives the same values on every OS and every run, so you can reproduce a failed run exactly. Because the variable name is an input, 2 variables with the same `seed` do not get the same sequence; separate inputs never get correlated noise.

The write times are the same as for `ramp`. For each variable, at T, T + step, T + 2 steps, … (in steps of that ECU), the scenario writes the value for that time. At the end time, it writes the value after the waveform.

Integers are rounded to the nearest (exactly .5 rounds away from 0, and a value that differs from .5 only in the last digit of the calculation also rounds as .5). Even so, a value very close to a rounding boundary may differ by 1 between operating systems or compilers: `sine`, `chirp` and `lag` use C library functions whose last digit is not guaranteed to match. To check the result of a waveform on an integer input, allow a margin with `ge` and `le`.

The range (`min`, `max`) is checked at these points:

| Value | When it is checked |
|---|---|
| `low`, `high`, `value`, `to`, `from`, `around`, the values of `points`, and `around` ± `amplitude` when `around` is written | By the check. |
| A waveform without `around`, `noise` with `gaussian` | During the run, because the values are not known until then. The step fails when it tries to write a value outside the range (`scenario-write`). Values are never clamped silently. |

`square` switches the value itself between high and low, for an ECU input that watches the edges of a pin. For a PWM input, where the ECU receives the duty cycle or the frequency as a numeric variable, use `set`, `ramp` or `sine` on that variable.

`vehicle_sil::signal` ([`signal/waveform.hpp`](../libs/signal/include/vehicle_sil/signal/waveform.hpp)) has functions that do the same calculation. You can use them in your own C++ code.

### Waveforms finer than the ECU step

An ECU takes its inputs once per ECU step. A waveform finer than that does not reach the ECU as written. The check (`check --scenario`, and the start of `run --scenario`) compares the waveform with the step size of the target ECU and reports, as a warning, what the ECU will actually see. Because it is a warning, the run can go ahead. With `--strict`, the check fails.

| Code | Reported when |
|---|---|
| `scenario-wave-undersampled` (warning) | The period is shorter than twice the step size (`sine`, `triangle`, `sawtooth`, `square`; for `chirp`, the higher frequency). |
| | A `sine` or `triangle` has the same value at every time at which the ECU takes a value (the period is twice the step size and the waveform starts at the center, so the ECU sees a constant). |
| | `width_ms` of `pulse`, `hold_ms` of `noise`, the interval between the points of `linear` `points`, or `interval_ms` of `chatter` (half of it with `seed`) is shorter than the step size. |
| | A `hold` `points` has a point that never reaches the ECU (the intervals between points are not checked). |
| `scenario-wave-duty` (warning) | The high time of `square` (period × `duty`) is not a multiple of the step size. The finding gives the duty the ECU sees. |
| | The period is not a multiple of the step size. |
| `scenario-wave-coarse` (note) | One period of `sine`, `triangle`, `sawtooth` or `chirp` is shorter than 8 ECU steps, so peaks and troughs may be missed. |
| `scenario-wave-not-checked` (note) | The step size of the target participant is unknown, so the waveform is not compared. |

Here is the warning for a square wave with a 30 ms period and a 25 % duty, written for an ECU with a 10 ms step size:

```text
warning: pwm.yaml:5: the square of Door.lamp is high for 7.5 ms of each period of 30 ms (duty 25 %), but participant 'Door' takes its inputs once per step of 10 ms: the ECU sees it high for 10 ms of each period (33.3 %). Make the high time (period x duty / 100) a multiple of 10 ms; to run it as it is without this message, add undersampled: ok to the step [scenario-wave-duty]
```

If the waveform is what you intend and you want to run it as it is, add `undersampled: ok` to the step. The 4 findings above are then not reported for that step.

```yaml
  - wave: { Door.lamp: { pulse: { width_ms: 3 } } }
    undersampled: ok
```

Notes for each waveform:

| Waveform | Notes |
|---|---|
| Smooth waveforms (`sine`, `triangle`, `sawtooth`) | The warning recommends a period at which the shape is visible (8 times the step size or more). |
| `pulse` shorter than the step size | If the pulse (including its start time, excluding its end time) contains the time of one ECU step, the ECU sees it for 1 ECU step. If not, the ECU does not see it. |
| `hold` `points` | They are meant for writing jumps, so short intervals between points are not reported. |

The sequence of values that the ECU sees does not depend on when the step starts: the writes are at T, T + step, …, and the ECU receives each one in a separate ECU step.

A short `pulse` is invisible because its start and end writes are applied in the same ECU step, and the later one wins. For an ECU with a 10 ms step size, a 5 ms pulse that starts at 107 ms is visible in the 110 ms step; one that starts at 101 ms is not. When the step's time is known before the run (`at_ms`, or after steps of known length), the warning says which case applies. When it is not known (after `wait_until`, for example), the warning says that it is not known.

For `hold` `points`, the check warns when the value of a point never reaches the ECU. That happens in 2 cases:

- None of the writes made every step size from the start of the step falls in that point's interval. This cannot happen if you place the points at multiples of the step size.
- The write of the second-to-last point is applied in the same ECU step as the end write (the same rule as for a short `pulse`).

Anything that cannot run at all is an error, not a warning: a period or width of 0 or less, `noise` without `seed`, `points` whose times are not in ascending order, a waveform that does not fit the type (`sine` on a bool, for example), and values out of range.

## Checking values during a ramp or wave

An `expect` beside `ramp` or `wave` checks that the condition holds all the time the value moves.

```yaml
  - ramp: { Engine.engine.throttle: { to: 80, over_ms: 1000 } }
    expect: { Engine.engine.speed: { le: 3000 } }
```

For each variable in the condition, the check looks at every sample with a time from T to U.

| Symbol | Meaning |
|---|---|
| T | The time of the step. |
| E | The end time of the step. |
| U | The time of the ECU step in which the ECU of a written variable applies its end value (`to`, or the value after the waveform). |

U is E if E falls on an ECU step, and the next ECU step if E falls between 2 ECU steps. If the variables are on several ECUs, U is the latest of these. Samples before T (the values before the movement) and after U are not looked at. This keeps the result from depending on whether T and E fall on or between ECU steps. The variables of other ECUs are looked at in the same samples, from T to U.

| Topic | Rule |
|---|---|
| The step passes when | All writes are applied and the condition holds until U. |
| `at` of the result, and the end from which the next step counts | Stays E. Adding `expect` changes neither the step's time nor its end. |
| Order in which results are decided | While a write or the condition is still open, this step's result is not decided, even if the result of a later step is decided first. |
| A variable with no sample in the interval | Counts as not checked, and the step fails at U (`no sample of <variable> in that time`). |
| `within_ms`, `hold_for_ms` | Not allowed (error). |
| The condition did not hold | The step fails at the time of that sample (`scenario-failed`). |
| A write was not applied | `scenario-write`. |
| Both happened | `code` is whichever the launcher learned of first (see [Results](#results)). |

- A variable without samples is handled differently from an `expect` with `hold_for_ms`: a sample before T is never used (see [Values that conditions see](#values-that-conditions-see)).
- To check over a different interval, put the `expect` in a step of its own after the `ramp` or `wave`.
- The failure message has the same form as for `hold_for_ms`, and shows the interval and the sample it looked at: `not true during the ramp (from 1000 ms to 2000 ms): at 1340 ms Engine.engine.speed le 3000, but it was 3050 (its sample of 1340 ms)`. When U differs from E, the interval gets `, where its end (2000 ms) takes effect`.

### When the `expect` variable is absent

If a variable of the `expect` is absent in the configuration file (`absent: true`, or a participant that the variant disables), only the `expect` is skipped. The `ramp` or `wave` runs as written. The conditions on the variables that are present are not checked either, as when a whole step is skipped. If a variable that the step moves is absent, the whole step is skipped (see [Skipping steps that use an absent variable](#skipping-steps-that-use-an-absent-variable)).

| Where it shows | Details |
|---|---|
| Step text | `ramp ... over 100 ms, expect not checked: <condition>` |
| The check's note `scenario-step-skipped` | Has the form `the ramp runs; its expect is not checked (...)`. |
| Counts | In the result lines of the check and the run (`1 expect not checked`), in `unchecked_expects` in JSON, and in the table of `vehicle_sil_launch test` (`1 expect was not checked`). |
| Reason | In the human-readable output, `line <line> expect not checked: <reason>` below the table. In JSON, `expect_skipped` of the step. |

## Variants and parameters

When the configuration file has variants (`variants`), 3 mechanisms let one scenario serve every variant:

| Mechanism | What it does |
|---|---|
| [Parameters](#parameters-params-name) | Changes the values in steps per variant or per argument. |
| [Skipping steps](#skipping-steps-that-use-an-absent-variable) | Does not run the steps that use a variable the variant does not have. |
| [`only`](#limiting-a-scenario-to-variants-only) | Limits the variants on which the scenario runs. |

Write the differences between variants in the configuration file. Do not write steps that branch on variant names in the scenario. For the configuration side, see "Variants (`variants`, `--variant`)" and "Scenario parameters (`params`, `--param`)" in [simulation-config.md](simulation-config.md).

```yaml
vehicle_sil_scenario: 1
name: idle
params: { idle_rpm: 800, settle_ms: 500 }      # default values
steps:
  - set: { Engine.engine.throttle: 0 }
  - after_ms: ${settle_ms}
    expect: { Engine.engine.speed: { near: "${idle_rpm}", tolerance: 50 } }
  - expect: { Engine.engine.turbo: { le: 0.1 } }    # skipped on variants without a turbo
```

### Parameters (`params`, `${name}`)

Write `${name}` where a step takes a value, as the whole value. It reads as if the parameter's value were written there. The type and range checks are the same, and an error message shows which parameter the value came from and where it was set.

| Topic | Rule |
|---|---|
| Where it is allowed | The value of `set`, the value and `tolerance` of a condition, `to`, `from` and `over_ms` of `ramp`, the numbers and durations of `wave`, `at_ms`, `after_ms`, `timeout_ms`, `within_ms` and `hold_for_ms`. |
| Where it is not allowed | Keys, variable names and part of a value (`"${a} rpm"`). These give the error `scenario-param`. |
| Expressions | None (`${a+1}` gives the error `scenario-param`). If you need a calculated value, make it another parameter. |
| Inside `{ ... }` or `[ ... ]` | Quote it (`{ near: "${idle_rpm}" }`). YAML cannot read an unquoted `${` there. |
| JSON | Write it as a string. |
| Type when quoted | The value is read as written in the parameter (a number stays a number). |
| Values in `params` | A single value (a number, `true` or `false`, or a string). A value cannot use other parameters. |
| Names | Letters, digits and `_`, not starting with a digit. |

Values are applied in this order, and a later one replaces an earlier one. A name that is not in the scenario's `params` can still be used if a value comes from another place.

| Order | Where the value is written |
|---|---|
| 1 | `params` of the scenario |
| 2 | `params` of the configuration file |
| 3 | `params` of the selected variant |
| 4 | The argument `--param <name>=<value>` |

| Error | Result |
|---|---|
| `${name}` with no value from anywhere | A check error, before the run (`scenario-param`). |
| `--param` with a name the scenario neither uses nor declares | A usage error (exit code 4). The message lists the scenario's parameter names. |

While a parameter has an error, the contents of the steps are not checked. Fix the parameters first.

### Skipping steps that use an absent variable

Steps that use the following variables are skipped in the run. The check reports the note `scenario-step-skipped` for each such step; it is not an error.

| Variable | Handling |
|---|---|
| A variable marked `absent: true` in the variable map of the configuration file (declared as missing from that executable) | Steps that use it are skipped. |
| A variable of a participant that the selected variant disables (`enabled: false` in the variant) | Steps that use it are skipped as well. That participant is not checked, so the variable name is not verified. |
| A variable of a participant disabled for another reason (`enabled: false` in the common part, `--disable`) | A check error (`scenario-variable`). |
| A variable of a participant that the variant disables and `--enable` takes back in | Checked like the variables of any other participant. |
| A variable that has neither a mapping nor `absent` and is missing from the executable | A check error (`scenario-variable`). It is never skipped silently. |

For a participant that the variant disables, the reason reads `participant 'Door' is not in the variant medium (enabled: false in that variant)`.

Only the steps that use such a variable are skipped.

| Case | What is skipped |
|---|---|
| One step uses both present and absent variables | The whole step. The present variables are not written either. |
| Only the `expect` beside `ramp` or `wave` uses an absent variable | Only that `expect` (see [When the `expect` variable is absent](#when-the-expect-variable-is-absent)). |
| A later step relies on a skipped `set` | The later step is not skipped; it runs. |

For a step that does not hold on some variants, either make it use an absent variable, or split the scenario and add `only`.

A skipped step still uses up the duration that its contents give, and `after_ms` of the next step counts from its end. This keeps the times of later steps the same on variants that skip the step and on variants that do not.

| Skipped step | Duration it uses up |
|---|---|
| `set`, `expect` without a duration | 0 |
| `ramp` | The longest `over_ms`. For an absent variable, `ramp` still reads `over_ms`, but not `to` and `from`. |
| `wave` | The duration of the longest waveform. For an absent variable, `wave` still reads `for_ms`, `width_ms`, the time of the last point, `bounces` and `interval_ms`, and nothing else. |
| `expect` with `hold_for_ms` | That duration |
| Wait steps (`wait_until`, `expect` with `within_ms`) | 0 (the step counts as ended at its time) |

Wait steps are the one exception: when they end is not known until the run, so they count as 0. A later step with `after_ms` then starts earlier than on variants that do not skip the wait step. If a step needs the time after the wait, either make it use the same variable as the wait step, so that both are skipped together, or split the scenario and add `only`.

| Topic | Rule |
|---|---|
| Reading the file | Does not depend on the variant. The error that `at_ms` is before the end of the previous step is reported the same way on variants that skip steps. |
| `at_ms` after a skipped wait step | Not allowed, the same as without skipping. |
| Duration of the last skipped step | Does not extend the run. The scenario ends when the results of all steps that are not skipped are decided. |
| Result of the step | `status` is `skipped` (also `skipped` in the human-readable output), with a reason. |
| Result of the scenario | Stays `passed` if all other steps pass, and the number of skipped steps is shown (`passed, 3 of 4 steps passed, 1 skipped` in the human-readable output, `skipped_steps` in JSON). |

If every checking step (`expect`, `wait_until`, and `expect` beside `ramp` and `wave`) is skipped, the scenario passes without checking anything. The check then reports 1 warning, `scenario-nothing-checked`. The warning lists the lines of the checking steps and what to do next: add `only` if the scenario is not meant for that variant, or add a variable map if it is. By default the run passes. With `--strict`, the check fails and nothing starts. A scenario that has no checking steps in the first place (only `set`, `ramp` and `wave`) does not get this warning.

### Limiting a scenario to variants (`only`)

A scenario with `only: [small, medium]` is meant for those variants.

| Case | Result |
|---|---|
| Passed to `check --scenario` or `run --scenario` with another variant (`--variant large`) | Error `scenario-variant`. Nothing starts. |
| `vehicle_sil_launch test`, which runs every variant | Marks that run `skipped` without running it (see "Running several scenarios (`vehicle_sil_launch test`)" in [simulation-run.md](simulation-run.md#running-several-scenarios-vehicle_sil_launch-test)). |
| `only` has a name that is not in `variants` of the configuration file | Error. |
| A scenario with `only` is passed with a configuration file that has no variants | Error. |

When the variant does not match, the contents of the steps are not checked, because they may not fit that variant.

## Results

The result of the scenario (`status` in JSON) is decided as follows:

| What happened | Scenario | Run |
|---|---|---|
| All steps passed | `passed` | The launcher stops the simulation at that controller step, and the run passes (exit code 0 if there are no other errors). |
| One step failed | `failed` | The launcher stops the simulation, and the run fails (exit code 1). |
| `stop_at_ms` was reached first | `failed` | The first step whose result is not decided fails (`scenario-out-of-time`). |
| A stop request (Ctrl+C, the `stop` command), a participant failure, `--timeout-ms` | `incomplete` | These take precedence over the scenario. The rules of [simulation-run.md](simulation-run.md) apply unchanged. |

- The launcher stops the run when all steps pass, so the configuration file does not need `stop_at_ms`.
- When `stop_at_ms` is reached first, the results are decided as far as possible. Increase `stop_at_ms` or remove it.
- With `incomplete`, the error `scenario-incomplete` shows how far the scenario got.
- If a write to an ECU that crashed fails first, the result of the run is still a participant failure, and the scenario is `incomplete`.

### When a step fails

When one step fails, the scenario ends there. Steps that had started become `unfinished`, and the steps after them `not_run`. The writes of later steps up to the next time step may already have been handed to the ECUs.

The failure reported is the first one the launcher learned of, which is not always the first in virtual time. At each controller step P, the launcher looks first at the results of earlier writes, then at the samples before P, then at the writes up to the next time step. A write that is rejected at once (out of range, for example) is detected when the launcher sends the writes for one time step ahead. Such a write can be reported before a condition that fails on a sample of an earlier time, and changing the time step can swap the two.

### The `at` of a step

The `at` of each step (`at_ms` in JSON) is the virtual time at which the step's result was decided.

| Step | `at` |
|---|---|
| `set` | The ECU step that applied the value |
| `ramp`, `wave` | The end |
| `expect` at one point | T |
| `wait_until`, `within_ms` | The time of the sample on which the condition held |
| `hold_for_ms` | The end (on failure, the time of the sample on which the condition did not hold) |
| `ramp` and `wave` with an `expect` beside them, when the condition did not hold | The time of the sample on which the condition did not hold |
| Timeout | The time of the timeout |

### A failing example

A failure finding shows the scenario file and line, the step, the time and the value at that time. The example below checks that the engine speed reaches 1700 rpm within 200 ms of setting the throttle, but the speed reaches it only at 330 ms (the `wait_until` of the example above).

<!-- throttle_response.yaml -->
```yaml
vehicle_sil_scenario: 1
name: throttle response
steps:
  - at_ms: 0
    set: { Engine.engine.throttle: 50 }
  - expect: { Engine.engine.speed: { ge: 1700 } }
    within_ms: 200
  - expect: { Dashboard.dashboard.engine_speed: { ge: 1700 } }
    within_ms: 200
```

<!-- vehicle_sil_launch run simulation.yaml --scenario throttle_response.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

scenario throttle_response.yaml (throttle response): 3 steps

run: the simulation ran until the scenario was over (205 ms) and ended

scenario throttle_response.yaml (throttle response): FAILED, 1 of 3 steps passed

line  step                                                             result   at
4     set Engine.engine.throttle = 50                                  ok       0 ms
6     expect Engine.engine.speed ge 1700 (within 200 ms)               FAILED   200 ms
8     expect Dashboard.dashboard.engine_speed ge 1700 (within 200 ms)  not run  -

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

error: throttle_response.yaml:6: expect Engine.engine.speed ge 1700 (within 200 ms): not true at any time within 200 ms (from 0 ms to 200 ms): at the end Engine.engine.speed ge 1700, but it was 1050 (its sample of 200 ms) [scenario-failed]

failed: 1 error, 0 warnings, 0 notes
```

The exit code is 1. The steps after the failed step do not run (`not run`). A `wait_until` timeout looks like this:

```text
error: s.yaml:4: wait_until Body.ticks ge 10; Body.level eq 6 (timeout 100 ms): timed out at 100 ms (100 ms after 0 ms): Body.level eq 6, but it was 5 (its sample of 100 ms) [scenario-timeout]
```

### Run finding codes

All of them are errors.

| `code` | Meaning |
|---|---|
| `scenario-failed` | An `expect` (including one beside `ramp` or `wave`) did not hold, or the participant of a variable it reads is not connected in this run. |
| `scenario-timeout` | A `wait_until` did not hold within `timeout_ms`. |
| `scenario-write` | A write of `set`, `ramp` or `wave` was not applied (a `wave` tried to write a value out of range, for example), or there is no start value for a `ramp` or no center value for a `wave`. |
| `scenario-out-of-time` | The run stopped at `stop_at_ms` before the result was decided. |
| `scenario-incomplete` | The simulation ended for another reason before the scenario was over. |

## Check (`check --scenario`, start of `run --scenario`)

The whole scenario is checked before the run. `vehicle_sil_launch check <config> --scenario <scenario>` only checks. If there are errors, `run` starts nothing. Findings carry the file name and line of the scenario, and appear together with the findings for the configuration file (`file` of the finding in JSON).

An example with errors: it misspells a variable name, writes to an output, sets a value out of range, has `at_ms` after a wait step, and gives `near` no `tolerance`.

<!-- throttle_mistakes.yaml -->
```yaml
vehicle_sil_scenario: 1
name: throttle mistakes
steps:
  - at_ms: 0
    set: { Engine.engine.throttel: 50 }
  - set: { Engine.engine.speed: 1000 }
  - set: { Engine.engine.throttle: 150 }
  - wait_until: { Engine.engine.speed: { ge: 1700 } }
    timeout_ms: 500
  - at_ms: 600
    expect: { Engine.engine.speed: { near: 1800 } }
```

<!-- vehicle_sil_launch check simulation.yaml --scenario throttle_mistakes.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

scenario throttle_mistakes.yaml (throttle mistakes): 5 steps

error: throttle_mistakes.yaml:5: 'Engine.engine.throttel': participant 'Engine' has no variable 'engine.throttel': did you mean 'Engine.engine.throttle'? (vehicle_sil_launch check --json lists the variables) [scenario-variable]
error: throttle_mistakes.yaml:6: Engine.engine.speed is an output of its ECU: it can be read (expect, wait_until), not written; write one of the ECU's inputs instead [scenario-not-writable]
error: throttle_mistakes.yaml:7: the value of Engine.engine.throttle (real) is 150, outside its range (from 0 to 100 %) [scenario-value]
error: throttle_mistakes.yaml:10: at_ms after a step that waits (line 8): when that step ends is not known before the run; give this step after_ms (from the end of the step before it) instead [scenario-time]
error: throttle_mistakes.yaml:11: Engine.engine.speed: near needs tolerance (how far from the value it may be): add tolerance: <number> [value]

failed: 5 errors, 0 warnings, 0 notes
```

The check lists every error; it does not stop at the first one. The exit code is 1. It is also 1 when the scenario file is missing (`file`), unlike a missing configuration file, which gives 3.

| `code` | Severity | Meaning |
|---|---|---|
| `file`, `encoding`, `syntax`, `not-a-scenario`, `format-version`, `unsupported-yaml` | Error | The file cannot be read, is not a scenario, has another format version, or contains an anchor or a second document. |
| `unknown-key`, `duplicate-key`, `missing-key`, `value` | Error | An error in the shape of keys and values (2 actions in one step, `wait_until` without `timeout_ms`, `near` without `tolerance`, and so on). |
| `scenario-variable` | Error | A variable is not of the form `<participant>.<variable name>`, the participant or the variable does not exist, or the participant cannot be used (it is disabled, has `describe: false`, runs in real time, or does not expose variables). |
| `scenario-not-writable` | Error | The step writes to an output, or writes a Parameter other than with a `set` at time 0. |
| `scenario-value` | Error | A value does not fit the type, the range or the enum items. A `ramp` or a magnitude comparison is used on a non-numeric variable. The waveform of a `wave` does not fit the variable's type, or `around` ± `amplitude` is out of range. |
| `scenario-param` | Error | `${name}` has no value, or `${...}` is in a key, in a variable name or in part of a value. (A `--param` with a name that is not in the scenario is the usage error `usage`.) |
| `scenario-variant` | Error | The scenario's `only` does not include the selected variant, or a name in `only` is not a variant of the configuration file. |
| `scenario-time` | Error | `at_ms` is before the end of the previous step, `at_ms` comes after a wait step, or the durations of the steps add up to more than the time limit (1000000000000 ms). |
| `scenario-wave-undersampled`, `scenario-wave-duty` | Warning | A `wave` is finer than the step of the target ECU (see [Waveforms finer than the ECU step](#waveforms-finer-than-the-ecu-step)). |
| `scenario-wave-coarse`, `scenario-wave-not-checked` | Note | See the same section. |
| `scenario-nothing-checked` | Warning | Every checking step (`expect`, `wait_until`, and `expect` beside `ramp` and `wave`) is skipped, so the scenario checks nothing. |
| `scenario-step-skipped` | Note | The step uses a variable with `absent: true` or a variable of a participant that the variant disables, so it is skipped in the run. If only the `expect` beside a `ramp` or `wave` uses it, only that `expect` is skipped. |

## Output

### Human-readable output

| Part | Content |
|---|---|
| Check | The line `scenario <file> (<name>): <n> steps`. |
| Check (with parameters) | The line `parameters: <name> = <value> (<source>), ...`. |
| Run | One result line and the table of steps (line, step, result, and the virtual time at which the result was decided). |
| Run (for each skipped step) | `line <line> skipped: <reason>`. |
| Run (for each step whose `expect` beside it was skipped) | `line <line> expect not checked: <reason>`. |
| During the run (stderr) | One line each time the result of a step is decided. |

- If steps are skipped, the check line gets ` (<n> skipped)`. If `expect`s beside steps are not checked, it gets ` (<n> expect not checked)`. When both apply, they are joined with `, `.
- If `expect`s beside steps are not checked, the result line ends with `, <n> expect not checked`.
- A skipped step has `-` as its time in the table, and its line during the run has no ` at <time> ms`.

### `check --json` and the `check` of `run --json`

`scenario` is present only with `--scenario`.

| `scenario` key | Present | Meaning |
|---|---|---|
| `file`, `name` | Always | The scenario's file and name |
| `steps` | Always | The number of steps |
| `only` | Only if set | An array of variant names |
| `params` | Only if any | For each parameter that the scenario declares or uses: `name`, `value`, `from` |
| `skipped_steps` | Only if any | The number of steps to be skipped |
| `unchecked_expects` | Only if any | The number of `expect`s beside steps that will not be checked |

`value` in `params` is the effective value (`true` or `false`, a number or a string). `from` is one of `scenario`, `configuration`, `variant` and `command_line`.

### The `scenario` of `run --json`

`scenario` is at the top level (only with `--scenario`).

| Key | Meaning |
|---|---|
| `file`, `name` | The scenario file (as given in the argument) and the name (null if it has none). |
| `status` | One of `passed`, `failed`, `incomplete` (ended early for another reason) and `not_run` (the simulation did not start). |
| `over_at_ms` | The controller step at which the scenario ended (for `passed` and `failed`; null otherwise). |
| `skipped_steps` | The number of skipped steps. The key is present only if there is at least 1. |
| `unchecked_expects` | The number of `expect`s beside `ramp` and `wave` that were not checked. The key is present only if there is at least 1. |
| `steps[]` | The result of each step (next table). |

A step counted in `unchecked_expects` did run, so it is not counted in `skipped_steps`.

| `steps[]` key | Meaning |
|---|---|
| `index` | The number of the step, from 1. |
| `line` | The line in the scenario file. |
| `kind` | One of `set`, `ramp`, `wave`, `expect` and `wait_until`. |
| `text` | The text of the step. |
| `status` | The result of the step (next table). |
| `start_ms` | The time of the step. |
| `at_ms` | The time at which the result was decided. |
| `end_ms` | The end time. |
| `code`, `message` | Present when the step failed or was skipped (null otherwise). |
| `expect_skipped` | Present only on a `ramp` or `wave` that ran with the `expect` beside it skipped: the reason it was skipped. |

| Step `status` | Meaning |
|---|---|
| `passed` | The step passed. |
| `failed` | The step failed. |
| `unfinished` | The step had started, but the scenario ended before its result was decided. |
| `not_run` | The step did not start. |
| `skipped` | The step was skipped (see [Skipping steps that use an absent variable](#skipping-steps-that-use-an-absent-variable)). |

The keys of a skipped step:

| Key | Value in a skipped step |
|---|---|
| `code` | `scenario-skipped` |
| `message` | The reason |
| `at_ms` | null (the result was decided before the run, not at a point in virtual time) |
| `start_ms`, `end_ms` | The time and end the step would have had without skipping (`start_ms` plus the duration its contents give) |

`start_ms` and `end_ms` of a skipped step do not mean that the simulation got that far.

- Times are numbers of milliseconds. They can have a fraction when an ECU step is finer than 1 ms. An unknown time is null.
- In a run that the launcher stopped because the scenario was over, `simulation` is `ended`.

Here is the `scenario` that `run --json` gives for the failing example of [Results](#results) above (`throttle_response.yaml`). For the other keys, see "JSON (`run --json`)" in [simulation-run.md](simulation-run.md#json-run---json).

<!-- vehicle_sil_launch run simulation.yaml --scenario throttle_response.yaml --json: the scenario -->
```json
  "scenario": {
    "file": "throttle_response.yaml",
    "name": "throttle response",
    "status": "failed",
    "over_at_ms": 205,
    "steps": [
      {
        "index": 1,
        "line": 4,
        "kind": "set",
        "text": "set Engine.engine.throttle = 50",
        "status": "passed",
        "start_ms": 0,
        "at_ms": 0,
        "end_ms": 0,
        "code": null,
        "message": null
      },
      {
        "index": 2,
        "line": 6,
        "kind": "expect",
        "text": "expect Engine.engine.speed ge 1700 (within 200 ms)",
        "status": "failed",
        "start_ms": 0,
        "at_ms": 200,
        "end_ms": null,
        "code": "scenario-failed",
        "message": "not true at any time within 200 ms (from 0 ms to 200 ms): at the end Engine.engine.speed ge 1700, but it was 1050 (its sample of 200 ms)"
      },
      {
        "index": 3,
        "line": 8,
        "kind": "expect",
        "text": "expect Dashboard.dashboard.engine_speed ge 1700 (within 200 ms)",
        "status": "not_run",
        "start_ms": null,
        "at_ms": null,
        "end_ms": null,
        "code": null,
        "message": null
      }
    ]
  }
```

### `run --events`

The event `scenario_step` (with the same keys as `steps[]`) is emitted when a step starts (`status` is `running`) and when its result is decided. The result event also has `scenario`.

For a step that is skipped, `scenario_step` (`status` `skipped`, `at_ms` null) is emitted once, when the time of the step is decided. Because that is known before the run, the event can come before virtual time reaches the step.

## Watching variables during a run (`--control`)

With both `--scenario` and `--control`, the Control Server also runs during a scenario (see "Connect to the launcher" in [control-api.md](control-api.md#connect-to-the-launcher); only in builds with the Control Server). A client can only read, wait and stop.

| Client call | During a scenario |
|---|---|
| Read and wait: `Ping`, `ListPeers`, `GetValues`, `GetSnapshot`, `Watch`, `GetTime`, `WaitPaused`, `GetOperation`, `WaitOperation` | Work as usual. |
| Write variables, forget peers: `Write`, `ForgetPeer` | Rejected. |
| Move time: `Pause`, `Resume`, `Step`, `RunUntil`, `SetSpeed` | Rejected. |
| Stop: `Stop` | Accepted. It is one of the stop requests, and the scenario becomes `incomplete` (`interrupted_by` is `"control server stop"`). |

A rejected call:

| Topic | Details |
|---|---|
| Status code | `FAILED_PRECONDITION` |
| Reason | `vehicle-sil-reason` in the trailing metadata is `scenario-running` (`ControlError.reason` in Python) |
| Message | Says what was rejected, why, and what the client can do instead. |
| Role (see "Authentication" in [control-api.md](control-api.md#authentication)) | The call is rejected whatever the role. |
| Record | The call reaches neither the Hub nor the time gate and does not become a `command` event. The audit log still gets a line. |

The result of the scenario is the same with or without clients. Read calls and subscriptions do not hold up the simulation. They do contend briefly for the same lock, so many reading clients can slow the run slightly in real time. The `timing` of a run at real-time speed can change by that much, but the result of the scenario does not. For a client that does not read, subscriptions keep a bounded backlog, and drop and count what overflows (for values, the latest one per peer is kept).

What a client sees:

| Situation | What the client sees |
|---|---|
| Writes of the scenario | Operations (operations in `Watch`, with `client` `scenario`). |
| While the scenario holds a controller step | Not a pause (`paused` of `GetTime` is false, and so is `consistent` of `GetSnapshot`). |
| While paused with `pause` from stdin | `paused` is true, and `consistent` behaves as in a run without a scenario. |

The stdin commands (`--stdin-commands`: `stop`, pause, step, speed) work the same with a Control Server.

## Incompatible options and limitations

- `--scenario` cannot be combined with `--start-paused` (a usage error, also with `--control`). A scenario completes its writes for time 0 before the first simulation step, and then advances on its own.
- `--stdin-commands` can be used (`stop`, pause, step, speed). Pausing and stepping do not change the result of the scenario.
- A scenario can use only variables of ECUs that are in the configuration file, are not disabled, are built with `ecu_main` and run in virtual time.
- Scenarios have no conditions that wait for frames directly, no expressions, no per-variant branching and no variable maps.

## Launcher feature names (`features`)

When you use a `vehicle_sil_launch` of another version, check `features` in the output of `vehicle_sil_launch version --json` for the following names to see which scenario features it supports.

| `features` name | Feature | Without it |
|---|---|---|
| `scenario` | `check --scenario` and `run --scenario` | Scenarios cannot be used. |
| `scenario-wave` | The `wave` step | `wave` is an unknown-key error. |
| `scenario-motion-expect` | `expect` beside `ramp` and `wave` | An error that the step has 2 actions. |
| `scenario-control` | `--scenario` together with `--control` | The two cannot be combined. Only builds with the Control Server have this name. |

[simulation-events.md](simulation-events.md) lists every `features` name.

## See also

| To find | See |
|---|---|
| Write a scenario step by step | [Write tests with scenarios](howto/scenario-tests.md) |
| Configuration file, variants, arguments and exit codes | [simulation-config.md](simulation-config.md) |
| The check (`check`) | [simulation-check.md](simulation-check.md) |
| The run (`run`), and `test`, which runs several scenarios | [simulation-run.md](simulation-run.md) |
| Events (`run --events`) and stdin commands | [simulation-events.md](simulation-events.md) |
| Reading values from outside during a run | [control-api.md](control-api.md) |
