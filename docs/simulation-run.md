# Running a simulation (`vehicle_sil_launch run`)

[日本語](ja/simulation-run.md)

This reference describes what `vehicle_sil_launch run <file>` does and in which order, when it aborts and what it writes. It also covers `vehicle_sil_launch test`, which runs several scenarios in one command. For the list of arguments, see "Arguments and exit codes" in [simulation-config.md](simulation-config.md#arguments-and-exit-codes).

| To find | See |
|---|---|
| What `run` does and in which order | [How a run proceeds](#how-a-run-proceeds) |
| Keeping the choices of a run (variant, scenario, participants, connections, speed) in a file | [Request files](#request-files---request) |
| When a run aborts, and how timeouts are measured | [Abort conditions and timeouts](#abort-conditions-and-timeouts) |
| Whether ECU variables can be read and written (`control_link`) | [Connection to ECU variables](#connection-to-ecu-variables-control_link) |
| `speed`, and how lateness behind real time is measured | [Running at a set speed](#running-at-a-set-speed-speed) |
| Connecting a network to a real bus, and what happens when its bridge ends | [Connecting a bus outside](#connecting-a-bus-outside---connect) |
| Where each network can be connected on this computer | [`connections`](#where-a-network-can-be-connected-vehicle_sil_launch-connections) |
| The order in which participants are ended, Ctrl+C | [Stopping a run](#stopping-a-run), [Interrupting (Ctrl+C)](#interrupting-ctrlc) |
| Which addresses participants listen on, Windows Firewall | [Listen addresses](#listen-addresses) |
| Stderr, log files, the result table, the `code` of findings | [Output](#output) |
| Exit codes | [Exit codes](#exit-codes) |
| Keys of `run --json` | [JSON](#json-run---json) |
| Recording what a scenario wrote and saw in a JSON file | [`--timeline`](#recording-a-scenario-run-on-a-timeline---timeline) |
| Seeing that file as a chart, and why a scenario failed | [Viewing a timeline file](#viewing-a-timeline-file) |
| Running several scenarios on every variant | [`test`](#running-several-scenarios-vehicle_sil_launch-test) |

## How a run proceeds

| Stage | What `run` does |
|---|---|
| 1 | Runs the same check as `check`. |
| 2 | Starts the registry inside this process. |
| 3 | Runs the controller (participant name `SimulationController`) inside this process. |
| 4 | Starts each participant that is not disabled as a process of its own, in configuration file order. |
| 5 | Starts the simulation once all required participants have joined. |
| 6 | When the simulation ends, ends all participants and writes the result. |

Stage 1: If the check finds errors (with `--strict`, also warnings), `run` starts nothing, writes the check result and exits with code 1. For a configuration file with variants (`variants`), `run` checks the variant you select with `--variant <name>` and runs with that variant's executables, variable map and participants (see "Variants (`variants`, `--variant`)" in [simulation-config.md](simulation-config.md#variants-variants---variant)). If you omit `--variant`, `run` reports a usage error and starts nothing.

Stage 2: The registry address is `registry_uri`, or a free port on 127.0.0.1 if `registry_uri` is not set. The first line on stderr gives that URI (`[vehicle_sil_launch] registry at silkit://127.0.0.1:<port>`). With `--control`, the Control Server starts before the registry and `run` writes the line `[vehicle_sil_launch] control server at <address>`.

Stage 3: The participants the simulation waits for (called "required participants" on this page) are those with `required: true` that are not disabled. The controller joins time synchronization with the time step (`time_step_ms`, or its default). It holds each time step at its end and decides there on speed, pauses and single-steps. This applies at maximum speed too, which adds one synchronization per time step.

Stage 4: With `--connect`, `run` first starts the bridge of each connected network and waits until it runs (see [Connecting a bus outside](#connecting-a-bus-outside---connect)). `run` does not start a `manual` participant; it prints the command that starts it. A participant that `run` starts gets the directory of the configuration file as its working directory and an empty stdin.

Stage 5: Before the start, `run` waits for the connection to the ECUs that expose variables (see [Waiting before the start](#waiting-before-the-start); normally there is no wait). With `--control` and `--start-paused`, every participant pauses before it starts the step at time 0, and the run waits for a `resume`, `step` or `run_until` command (see "Write before the start (`--start-paused`)" in [control-api.md](control-api.md#write-before-the-start---start-paused)). If `stop_at_ms` is set, the simulation stops when virtual time reaches it. Otherwise it runs until a participant stops it, or until Ctrl+C.

Stage 6: For the order in which the participants are ended, see [Stopping a run](#stopping-a-run).

### Runs with a scenario (`--scenario`)

With `--scenario <file>`, `run` also checks the scenario. Once the simulation has started, `run` carries out the scenario steps and stops the simulation when the scenario ends (see [scenario.md](scenario.md)).

| Item | With `--scenario` |
|---|---|
| The controller's time step | The same as the default with a set speed (`time_step_ms`, or a time step such that every ECU step starts at a controller step). |
| Values of parameters (`${name}`) | Can be overridden with `params` in the configuration file, `params` of the variant, and `--param <name>=<value>`. |
| Scenario steps that use a variable the variant does not have (`absent: true`) | Skipped and shown in the result. |
| Scenario steps that use variables of a participant the variant disables | Also skipped and shown in the result. |

A skipped step has `at_ms` null. If every checking step is skipped, the check reports the warning `scenario-nothing-checked` (see "Variants and parameters" in [scenario.md](scenario.md#variants-and-parameters)).

## Abort conditions and timeouts

### Before the start

| What happens | What `run` does | Finding |
|---|---|---|
| The process of a required participant ends | Abandons the start without waiting for `start_timeout_ms`. | `ended-before-start` |
| A required participant does not join within `start_timeout_ms` | Lists every participant that did not join and abandons the start. | `not-joined` |
| After all have joined, the process of a required participant ends while the run waits for the start | Abandons the start at once. | `ended-before-start` |
| After all have joined, a connection drops while the run waits for the start, with the process still running | Abandons the start at once. | `simulation-start` |
| After all have joined, the time limit for reaching the first step expires | Abandons the start; the message lists the participants that have not reached the first step and their states. | `simulation-start` |
| The bridge of `--connect` cannot be started, does not run within `start_timeout_ms`, or ends or leaves the simulation | Starts nothing more, or abandons the start at once. | `connect-failed` |

"Waits for the start" covers the wait for the ECUs that expose variables, and the pause before the step at time 0 with `--control` and `--start-paused`.

### During the run

| What happens | What `run` does | Finding |
|---|---|---|
| The process of a required participant ends with a nonzero exit code or by a signal | Aborts the simulation at once. | `participant-failed` |
| A required participant leaves the simulation in the Error state | Aborts at once. | `participant-failed` |
| A required participant leaves the simulation in another state | Aborts if the simulation has not ended within 5 seconds. | `participant-failed` |
| A participant with `required: false` ends | Does not stop the simulation. | A warning for a nonzero exit code |
| The bridge of `--connect` ends, whatever its exit code | Aborts at once. | `connect-lost` |
| The run has not ended within the real time of `--timeout-ms` | Aborts. | `timeout` (error) |
| Virtual time does not advance for 10 seconds although the simulation is not paused | Writes one line to stderr and keeps waiting. | None |
| A participant stops the simulation before `stop_at_ms` | The run ends as it is. | `ended-before-stop-at` (note; not a failure, even with `--strict`) |

Whether a participant has left the simulation is decided from the controller's view. The abort rules are the same as in `vehicle_sil_sim_controller` and `vehicle_sil::Simulation`. When the simulation is aborted, the other ECUs still end on their own (exit code 0). The start does not wait for participants with `required: false` either.

While the simulation is paused, participant failures, stop requests and `--timeout-ms` are handled the same way.

### How `start_timeout_ms` is measured

`start_timeout_ms` limits the time until all required participants have joined.

| Case | How it is measured |
|---|---|
| From all joined to the first step | The remainder of `start_timeout_ms` is the limit (at least the time spent waiting for ECUs that expose variables + 1 second). |
| While paused before the step at time 0 with `--control` and `--start-paused` | Not counted. |
| After the run advances from that pause | `start_timeout_ms` applies again as the limit. |
| Until a required manual participant joins | No limit. |
| After everyone has joined, until the manual participants are ready for the first step | No limit. |
| Until the participants that `run` started have joined (in a run with manual participants) | `start_timeout_ms` is the limit. |
| After the manual participants are ready | `start_timeout_ms` is the limit. |

A manual participant is one that you start yourself (see [Starting a participant yourself](simulation-config.md#starting-a-participant-yourself-manual---manual)).

### How `--timeout-ms` is measured

With `--timeout-ms <ms>`, if the simulation has not ended `<ms>` of real time after it started, `run` aborts it and reports an error (`timeout`). By default there is no limit. An ECU that hangs inside a step stops virtual time, and the run never ends even with `stop_at_ms`, so use this option in CI and similar settings.

| Case | How it is measured |
|---|---|
| A normal run | From the start of the simulation. |
| Time spent paused | Counted. |
| `--control` and `--start-paused` | From when the run pauses before the step at time 0 (the time spent paused counts too). |
| A run with a required manual participant | From when `run` starts waiting for the participants (see [Starting a participant yourself](simulation-config.md#starting-a-participant-yourself-manual---manual)). |

If the limit expires while the run is still paused by `--control` and `--start-paused`, `run` executes the step at time 0 and then aborts; the `timeout` message says that the run was paused before the first step. If the limit expires after the run has advanced, the message says how many ms of that time the run was paused.

### Message when virtual time stops advancing

If virtual time does not advance for 10 seconds during a run that is not paused, `run` writes a line of this form:

```text
[vehicle_sil_launch] virtual time has not advanced for 10 s (at t=<ms> ms, not paused): ...
```

The line lists the possible causes (a participant on another computer lost its network, a debugger holds a process, a process hangs inside a step) and mentions `--timeout-ms`. If virtual time still does not advance, the line is written again at 60 seconds and at 10 minutes. When virtual time advances again, `run` writes `virtual time advances again: it stood still for <s> s at t=<ms> ms`.

`run` does not abort; it keeps waiting. The line is not a finding (with `--events`, it is a `log` event). With a set speed, the time counts from the deadline of the time step; time spent waiting for the deadline at a slow speed does not count.

When a network goes down, SIL Kit takes a long time to notice that the connection has dropped (more than 10 minutes on Linux).

## Connection to ECU variables (`control_link`)

On top of the controller participant, `vehicle_sil_launch run` hosts the side that reads and writes ECU variables (`vehicle_sil::control::Hub` and `vehicle_sil::control_silkit::HubTransport`). An ECU built with `ecu_main` connects to it on its own, through SIL Kit publish/subscribe (see "Exposing variables (`--registry-uri`)" in [ecu-executable.md](ecu-executable.md#exposing-variables---registry-uri)). Every ECU has `ecu.connected`, so an ECU that declares no variables connects too. The configuration file needs nothing for this, and gRPC is not used. For how it works and what it guarantees, see "Transport over SIL Kit (`vehicle_sil::control_silkit`)" in [internals/control.md](internals/control.md#transport-over-sil-kit-vehicle_silcontrol_silkit).

The Hub inside `vehicle_sil_launch` uses this connection. With `run --control <address>` (only in builds with the Control Server), the Hub is exposed to clients (Python, CLI, GUI) as the gRPC Control API. For its use, see "Connect to the launcher" in [control-api.md](control-api.md#connect-to-the-launcher).

Whether an ECU exposes variables shows in the `--describe` output of its executable (`control`: the transport and the version of the message format; see "JSON format" in [ecu-executable.md](ecu-executable.md#json-format)). This lets the check find ECUs that do not expose variables and ECUs with a different version before the run.

If the configuration file has `participants[].variables` (a variable map), that participant's variables are read and written with the names and values after mapping (see [Matching variable names and values](simulation-config.md#matching-variable-names-and-values-variables)). Scenarios and Control API clients see only the mapped names and values.

### Connection state (`state`)

Each participant has one of 5 connection states. The state appears in `participants[].control_link` of the result and in the event `control_link`. It is the state at the moment the simulation started and is not updated when the state changes later. For a participant whose hello `run` does not wait for (see [Waiting before the start](#waiting-before-the-start)), it is the state just after the start, when every participant is at its first step.

| `state` | Meaning | Reading and writing variables | Finding |
|---|---|---|---|
| `connected` | `run` received hello, and a reply saying that the ECU can take commands | Possible | None |
| `no_hello` | The `--describe` output says that the ECU exposes variables, but no hello (or reply) arrived in time | Not possible in this run | Warning `control-link-no-hello` (run finding) |
| `version_mismatch` | The version of the message format differs from that of `vehicle_sil_launch` | Not possible in this run | Warning `control-link-version` |
| `not_offered` | The ECU has variables, but its `--describe` output says that it does not expose them (`--no-control`, or an older vehicle-sil executable without `control`) | Not possible | Note `control-link-not-offered` (check finding) |
| `none` | Nothing to connect to (an older vehicle-sil executable without variables, `describe: false`) | Not possible | None |

- For `version_mismatch`, the version comes from the `--describe` output or from the received hello. If the `--describe` output reveals the mismatch, it is a check finding; if only the hello reveals it, it is a run finding.
- `run` does not wait for a `not_offered` ECU.
- A participant whose `--describe` output does not offer variables but that sends hello is `connected`.
- The simulation runs in every state (with `--strict`, warnings are failures).
- `vehicle_sil_launch` reads and writes the variables only of ECU executables whose message format version is its own. An ECU built with vehicle-sil 0.1.0 under a newer launcher, or a newer ECU under the launcher of 0.1.0, takes part in the simulation, but its variables cannot be read or written; the check reports `control-link-version`.

### Waiting before the start

Once all required participants have joined, they all wait before the first step. During that wait, `run` waits for hello and the reply "can take commands" from every ECU that meets all of these conditions:

- Its `--describe` output says that it has a connection to its variables, with the same version as `vehicle_sil_launch`. This includes an ECU without variables: `run` reads the counts of its networks over the same connection.
- It is required and not disabled.

`run` does not wait for a `describe: false` participant, because it cannot know whether that participant has such a connection. If its hello arrives after the start, `control_link` stays `none`, and the entries of its `variables` map that do not match are still reported (`variable-map-not-applied`).

Because of the order in which SIL Kit connects participants, hello has normally arrived before this wait starts, so there is normally no wait. `run` waits, for up to 5000 ms, only when an ECU that says it has the connection has not sent hello. A Ctrl+C or a stop command during the wait ends it. If all participants joined just before the `start_timeout_ms` deadline, this wait can extend past `start_timeout_ms`.

### Guarantees during the run

When time is paused (`--start-paused`, `pause`, after a `step`), `run` has received the values and write results of every ECU step that started before the paused virtual time P. Those of the steps that start exactly at P arrive while the simulation is paused. If a message arrives later than this guarantee allows, `run` reports the warning `control-link-late-values`. This does not happen with ECUs built with `ecu_main`. The count is in `control_link.late_messages` of the result.

A participant that joins after the start (`required: false`, a real-time `Peer` that you started yourself, a restart under the same name) accepts writes once `run` has received its hello and then its reply "can take commands" (normally an instant later). Writes before that are not sent; they complete as Invalid at once. The same applies to writes to a participant that has started to leave the simulation (its lifecycle has started to stop, or it failed).

### Traffic and authentication

Values are not thinned out. An ECU sends all its variables every `sample_period` of its Agent (default: 10 ms of virtual time). The traffic grows with more ECUs, more variables and a higher speed. If the load is too high, increase the ECU's `sample_period`.

There is no authentication between the ECUs and `vehicle_sil_launch`. Anyone who can join the registry can write ECU variables, in the same way as they can send frames on the bus. By default, the registry listens only on 127.0.0.1.

## Running at a set speed (`speed`)

With `speed` (configuration file) or `run --speed`, the controller paces virtual time to real time at every time step (`time_step_ms`). With `speed: 1`, 1000 ms of virtual time takes at least 1000 ms of real time. Use a set speed to run together with devices that work in real time, or when a person operates the simulation while watching a screen.

Pacing only adds waits, so it never makes the simulation faster than its slowest participant. At maximum speed (`max`), there is no pacing and no lateness is counted.

A run that is connected outside the simulation (`--connect`, or an enabled `role: bridge` participant) goes at speed 1 whatever speed is set here, unless you pass `--time-policy free` (see [Time of a run that is connected outside](#time-of-a-run-that-is-connected-outside---time-policy)).

Real-time devices do not wait for the simulation. When the simulation falls behind real time, the devices see different results. That is why `run` measures lateness with the definitions below and reports it.

### Definition of lateness

| Term | Meaning |
|---|---|
| Deadline | The real time at which a time step (the controller step that starts at virtual time T) may start at the earliest |
| Lateness | The real time at which the time step actually started, minus its deadline (0 if negative) |
| Late time step | A time step whose lateness exceeds `late_tolerance_ms` (default: 10 ms of real time) |
| Slip | The lateness of a late time step (the total is `slipped_ms`) |

The deadline is "base real time + (T − base virtual time) ÷ speed". The base is taken at the first time step and taken again at every resume. If the speed changes from one number to another during the run, the deadline of the next time step does not move. No time step starts before its deadline.

A time step starts when the controller has released the previous time step and knows that every participant step that starts before this time step has finished. Lateness includes any delay in the controller itself waking up, because devices cannot tell the difference.

After a late time step, the simulation does not try to catch up. That time step becomes the new base, and the simulation proceeds at the set speed from there. All later virtual time stays shifted later by that lateness. Lateness within the tolerance does not change the base, so the next time step makes it up naturally.

Counting runs from the time step after the base was taken to the last time step (including the time step of `stop_at_ms`).

| Case | How it is counted |
|---|---|
| A time step that is past its deadline but has not started yet | Counted with its lateness so far. |
| The run ends by timeout, failure or stop while a participant is stuck | That lateness stays in the result. |
| Time spent paused | Not counted. |
| While at maximum speed (`max`) | No time step is late. |
| A time step that was already past its deadline when the speed was set to maximum | It keeps the lateness it had, and is counted once, when it starts (real time spent at maximum speed is not added to its lateness). |
| The speed is set back to a number before that time step starts | The deadline of that time step is "the time of the change − the kept lateness". |

### What real-time devices see when the run falls behind

If a time step is 490 ms late, every later message comes out 490 ms late. The period does not change (a 10 ms message still comes out every 10 ms). The simulation does not catch up, because catching up would release the backlog (about 49 periods of messages in this example) within a few ms, which would upset the receivers' period and timeout checks.

The number of late time steps is the number of times lateness occurred. One slow step, however long, is 1 late time step. A simulation whose ECU steps never keep up with real time keeps slipping. `slipped_ms` and `actual_speed` show how far behind the whole run fell (the real time taken is never shorter than virtual time ÷ speed + `slipped_ms`).

The lateness of a single time step (`max_late_ms`) can be reported larger than it really was. When a participant finishes a step and immediately enters the next one, the controller learns that the step finished only after the next step. The controller learns it late by that amount (at most the real time that participant takes for 1 step). The value is never reported smaller than the real lateness.

So, for an ECU whose step computation repeatedly exceeds the real time available for a step (step size ÷ speed), the effective tolerance shrinks by the computation time of the next step. When the overestimated lateness exceeds the tolerance, the time step becomes late and the base is taken again, so from then on the run really is shifted by that amount. With such ECUs, set `late_tolerance_ms` to roughly "the time the devices can wait + the computation time of 1 ECU step".

### Time steps and ECU steps

Pacing happens only at time step boundaries. An ECU step that starts at virtual time T runs at the deadline of the first time step at or after T. Steps that start between time steps run together at the deadline of the next time step. They run later than the real time that corresponds to T, and never earlier.

This delay is at most "time step − (greatest common divisor of the time step and the ECU step size)". It is not counted as lateness. It is in `step_delay_ms` of `timing` in the result.

| Time step | ECU step size | Maximum delay |
|---|---|---|
| 10 ms | 1 ms | 9 ms |
| 10 ms | 15 ms | 5 ms |
| 10 ms | A multiple of the time step | 0 |

So, with a set speed and no `time_step_ms`, the time step is the greatest common divisor of 10 ms, `stop_at_ms` and the ECU step sizes. The ECU step sizes are those that the check reads from the `--describe` output of the ECUs that are not disabled.

| ECU step sizes | Time step |
|---|---|
| 10 ms and 5 ms | 5 ms |
| 4 ms and 6 ms | 2 ms |
| Only 20 ms | 10 ms |
| A participant whose step size is unknown (`describe: false`) is present | 1 ms |
| The greatest common divisor is not a multiple of 1 ms | 1 ms |

If you set `time_step_ms`, that value is used. With a set speed, if ECU steps do not start at time step boundaries, the warning `coarse-time-step` is reported. The warning says by how many ms at most the steps are delayed and bunched together, and what value to write.

| Case | When `coarse-time-step` is reported |
|---|---|
| `speed` is also in the configuration file | At the check (in `run`, only that finding is reported) |
| `run --speed` changed the speed | At `run`, with the changed speed |
| The run started at maximum speed and received a `speed` command | At that moment (the time step is set at the start and does not change during the run) |

A finer time step means more time synchronizations. That is why the default at maximum speed stays 10 ms.

### Lateness warnings and what to do

If there is even 1 late time step, the warning `behind-real-time` is reported (a failure with `--strict`). The counts are in `timing` of the result, in the `time:` line of the human-readable output and in the `time` events of `--events`.

When the run falls behind, lower the speed or look for the cause.

| Cause | Fix |
|---|---|
| An ECU has a heavy computation in one step | Make that step lighter (a larger step size does not reduce lateness if a single step is heavy). |
| Many ECUs with small steps cause many synchronizations | Increase the ECU step sizes and `time_step_ms`. |
| You do not know which participant is slow | Disable the ECUs one at a time (`--disable`) and compare. |

`vehicle_sil_launch` does not know which participant is slow, because SIL Kit does not tell the controller how long each participant's step took.

The default `late_tolerance_ms` of 10 ms was chosen to be well below the timeouts of real-time devices and well above the jitter of OS timers. It is a length of real time and does not depend on the time step or the speed. Change it to match how long the devices can wait.

### Pausing, stepping and changing speed

Pause, single-step and change the speed during a run with stdin commands and with calls from Control Server (`--control`) clients (see [Stdin commands](simulation-events.md#stdin-commands-run---stdin-commands)).

`run --start-paused` pauses at virtual time 0 once all participants have joined, and waits for `resume` or `step`. Use it with `--stdin-commands` or `--control`.

| Options given with it | Where the run pauses |
|---|---|
| Only `--stdin-commands` | After it has executed the step at time 0. |
| `--control` | Before the step at time 0. |

In a run with a scenario (`--scenario`), these commands and calls are accepted too, and they do not change the result of the scenario: it writes and checks by virtual time alone (see "Watching variables during a run (`--control`)" in [scenario.md](scenario.md#watching-variables-during-a-run---control)). `--start-paused` cannot be combined with `--scenario`.

With `--connect`, the devices on the real bus do not stop or change speed with the simulation: `--start-paused` and a `--speed` other than 1 are refused unless you pass `--time-policy free`, and so are the commands that pause, step or change the speed while the run lasts (see [Time of a run that is connected outside](#time-of-a-run-that-is-connected-outside---time-policy)).

## Connecting a bus outside (`--connect`)

`run --connect <network>=<target>` connects the network to a real bus (an Ethernet adapter or TAP device, a SocketCAN interface, or a CAN channel of the Vector XL Driver Library) through the bridge of this repository. First see where each network can be connected on this computer, and copy a `--connect` line of the output (see [Where a network can be connected](#where-a-network-can-be-connected-vehicle_sil_launch-connections)):

```sh
vehicle_sil_launch connections simulation.yaml
vehicle_sil_launch run --speed 1 --connect "ETH_EXT=vEthernet (A)" simulation.yaml
```

The target is `none` or `[<backend>:]<selector>`: a backend (`packet`, `tap`, `npcap`, `xl`, `socketcan`) chooses how the network is connected, and a target without one takes the default of this OS (the grammar and the table are in "[`--connect`](simulation-config.md#--connect)" in simulation-config.md). On Linux, `--connect J1939_CAN1=can0` connects a CAN network to the SocketCAN interface `can0` and `--connect ETH_EXT=tap:vsil0` an Ethernet network to the TAP device `vsil0`.

The check looks for the bridge and asks it `--describe`, as `check --connect` does (see "[`--connect`](simulation-config.md#--connect)" in simulation-config.md). If the check passes, `run` goes on:

| Order | What `run` does |
|---|---|
| 1 | Starts the bridge of each connected network as the participant `bridge.<network>`, in the order of the networks in the file. The arguments are the command that the check shows (`networks[].outside.command` of `check --json`), then `--name`, `--registry-uri`, `--start-timeout-ms` and `--stats-every-ms 1000`. |
| 2 | Waits until each bridge runs in SIL Kit, up to `start_timeout_ms`. A bridge runs in real time and does not take part in the coordinated start. |
| 3 | Starts the participants of the file, as without `--connect`. |
| 4 | When the simulation ends, asks the bridges to stop together with the other participants with `required: false` (see [Stopping a run](#stopping-a-run)). |

The bridges start first because a frame that an ECU sends before its bridge has joined does not reach the bus outside (for example the address claims at time 0).

A bridge is not a participant of the file. It appears in the result (`participants[]`, after the participants of the file, with `for_network`; `outside[]`) and in the events (`started`, `joined`, `ended`, its lines) like any participant, except for its statistics lines (see below). `--disable`, `--enable` and `--manual` do not take its name: to run the network on SIL Kit only, give `--connect <network>=none` or leave `--connect` out.

| What happens to a bridge | What `run` does | Finding |
|---|---|---|
| It cannot be started, ends, or leaves the simulation before the simulation starts (also while the run waits for a participant you start yourself, or stands still before its first step with `--start-paused` and `--control`) | Starts nothing more and ends what it started. | `connect-failed` |
| It does not run in SIL Kit within `start_timeout_ms` | The same. | `connect-failed` |
| It ends while the simulation runs (also with exit code 0, while the simulation is paused, and at time 0 of `--start-paused` without `--control`) | Aborts the simulation at once. | `connect-lost` |
| It has ended by itself when the simulation ends, before `run` asks it to stop | Ends the run as usual. | `connect-lost` |
| It ends by itself after the launcher aborted the simulation for another reason (SIL Kit tells the bridges of the abort too) | Nothing more: the other finding says why. | None (`participants[]` has how it ended) |

The messages say how the bridge ended and quote the line in which it said why: its last line that starts with `error:` or `FAIL`, after its tag such as `[eth-bridge]`. A bridge that was killed, or ended without such a line, has none quoted (its last line would be about something else, such as `stopped`). `run` watches the bridge's process: a bridge that ends before it connects to SIL Kit fails the start at once, without waiting for `start_timeout_ms`.

A real bus does not wait for the virtual time: see [Time of a run that is connected outside](#time-of-a-run-that-is-connected-outside---time-policy) below.

### Time of a run that is connected outside (`--time-policy`)

A run is connected outside the simulation if at least one of these holds:

- `--connect` connects a network (a target other than `none`).
- The file has an enabled participant with `role: bridge` (also one with `describe: false`). A participant that is disabled (`enabled: false`, `--disable`) does not count.

What is connected there does not wait for the virtual time, so by default such a run keeps real time. `--time-policy` chooses:

| `--time-policy` | Speed of the run | `--speed` other than 1, `--start-paused` |
|---|---|---|
| `realtime` (the default) | 1, whatever `speed` the file has. The note `outside-speed-set` says so when the file's speed is not 1 (also without a speed). | The error `outside-real-time` of the check: nothing starts. |
| `free` | As in any run: `--speed`, or else `speed` in the file, or else as fast as possible. | Accepted. The note `outside-not-real-time` says that the run is not in real time. |

- `--time-policy` has no effect on a run that is not connected outside.
- `check`, `run` and `test` take it. `check` judges the file's `speed`; `run` also its `--speed` and `--start-paused`. `check --json` has the decision in `time_lock` (see [simulation-check.md](simulation-check.md#json---json)).
- `test` runs each scenario as fast as possible, but at speed 1 when the file has an enabled `role: bridge` participant (the note `outside-speed-set`). The table of `test` then has the note `test-speed-set` after it, once for all the runs. To run them as fast as possible all the same, pass `--time-policy free` or `--disable` the bridge.
- `free` changes only what you may do with the time. The bridge still carries frames both ways: while the simulation is paused, frames from outside pile up, and what the ECUs sent before stays sent.
- The notes do not fail a run with `--strict`.
- A real-time participant of your own (an ECU or a `role: test` participant without a step) does not make the run keep real time.

The commands during the run (stdin, a Control Server client, the GUI) follow the same choice:

| Command | `realtime` (the default) | `free` |
|---|---|---|
| `pause`, `step`, `run_until`, `speed` other than 1 (also `max`) | Refused with the reason `outside-real-time`. Nothing changes. | Accepted. The run ends with the note `outside-time-changed`. |
| `resume`, `speed` 1 | Accepted (they change nothing). | Accepted. |
| `stop` | Accepted. | Accepted. |

- The answer is the same from every entry, with a scenario or without: the `command` event has `accepted: false` and `reason: "outside-real-time"`, a Control Server client gets `FAILED_PRECONDITION` with that reason (see "In a run that is connected outside" in [control-api.md](control-api.md#in-a-run-that-is-connected-outside)), and without `--events` a refused stdin line is a line on stderr. The message says what to do: start the run again with `--time-policy free`, or without the connection.
- The `state` event says it before you send anything: with `realtime`, `allowed.pause`, `allowed.step` and `allowed.run_until` have `ok: false` with that reason, and `allowed.speed` has `min` and `max` 1. With `free` they have `ok: true` and a `caution` with the code `outside-not-real-time` (see "The `state` event" in [simulation-events.md](simulation-events.md#the-state-event)).
- A run with a scenario (`--scenario`) follows this table too, for stdin commands and for Control Server clients alike. Only a client's write is rejected for the scenario (reason `scenario-running`; see "While a scenario runs" in [control-api.md](control-api.md#while-a-scenario-runs)).
- A command that arrives before the launcher has read the configuration file waits, and is answered once the file is read: nothing gets past the rule by being early (see "When time commands are accepted" in [simulation-events.md](simulation-events.md#when-time-commands-are-accepted)).
- `outside-time-changed` is written once, when the run ends: it names the first accepted command that paused or stepped the simulation or set a speed other than 1, where it came from and the virtual time it came at (the `command` events show every command). `resume`, a speed of 1 and `--start-paused` itself do not count.

A real-time participant of your own does not stop either while the simulation is paused. In a run that has one (an ECU or a `role: test` participant whose `--describe` output has no step), the first such command gives the run the note `unsynchronized-time-changed` when it ends: what that participant sent and saw from then on does not fit the virtual time, and another run with the same commands can go differently. A participant with `describe: false`, and one that joins without being in the file, are not known to the launcher and get no note.

### Counting the frames to and from the bus

`run` counts, for each connected network, the frames that the bridge sent to the bus and the frames that came from a device there. It reads them from the statistics line of the bridge (`stats ...`, see "[Statistics line](bridge-contract.md#statistics-line)" in bridge-contract.md). `--stats-every-ms 1000` makes the bridge write that line while it runs, once a second when a count has changed, and once more when it stops.

| Network | `to_outside` | `from_outside` |
|---|---|---|
| Ethernet | `silkit_to_ext` | `ext_from_devices`: frames whose source is not this computer, counted before the bridge's filter |
| CAN | `silkit_to_ext` | `ext_from_devices`: frames that came from other nodes on the bus |

- The counts are in the result (`outside[]`; `outside:` lines of the human-readable result, such as `outside: ETH_EXT -> vEthernet (A) (bridge.ETH_EXT): 152 frames out, 3 from devices`) and, with `--events`, in the event `outside` while the run goes (see "Counts of a connected network (`outside`)" in [simulation-events.md](simulation-events.md#counts-of-a-connected-network-outside)). They are the same with and without `--events`.
- The statistics lines that `run` reads while the bridge runs are not shown as lines of the bridge (`[bridge.<network>] ...`) or as `log` events. When the bridge has ended, `run` shows the last one it read, once, as a line of the bridge: the bridge's report with every count, such as the frames that the filter kept from the ECUs (`filtered_from_ext`) or that were dropped at the stop (`ext_tx_unsent`). It is the line the bridge wrote when it stopped, or its last line while it ran if it was ended without one. The `--log-dir` file of the bridge has every line. A line that `run` cannot read (for example, of a bridge of another version without these keys) is shown as usual.
- The counts are those of the last line read. A bridge that was ended without its last line (on Windows, where the launcher could not ask it to stop) has the counts of its last line while it ran. Without any line read, both are `null`.

When the simulation ran and a bridge sent frames to the bus but none came from a device there before the run ended, the run ends with the note `connect-nothing-received`. A device that does not answer is not always a fault, so it is a note: `--strict` does not fail on it. A bridge that failed the run (`connect-failed`, `connect-lost`) and counts of `null` give no such note. On Ethernet, frames that this computer sends on the adapter (for example the IPv6 and ARP frames of the operating system on a Hyper-V `vEthernet` adapter) do not count, so a cable that is not plugged in gives the note. The bridges also warn while they run, 10 s after the first frame they sent with nothing from a device ("Hint when nothing comes from a device" in [ethernet-bridge.md](ethernet-bridge.md#hint-when-nothing-comes-from-a-device)).

```text
network 'ETH_EXT': nothing came from a device on the real bus (vEthernet (A)) before the run ended: bridge.ETH_EXT sent 152 frames there and got none from a device (frames that this computer sent do not count). If a device is meant to answer, check that it is on, its cable, and that --connect names the adapter it is on; a short run can end before a device answers
```

For an automatic test with a device on the bus, run one scenario at speed 1 with `--strict`. `test` does not take `--connect`: it is made for runs that go as fast as possible, one after the other, and a real bus does not wait for the virtual time. A run that is connected outside gets input that differs every time, so its result is not promised to be the same (see the start of [scenarios](scenario.md)).

```sh
vehicle_sil_launch run --scenario <scenario> --speed 1 --strict --connect <network>=<target> <file>
```

### The state of the link and the bus

While the run goes, `run` also reads the other lines of the bridge: the link line (`link link=up|down|unknown` of the Ethernet bridge, `link bus=active|passive|bus-off|unknown` of the CAN bridge; see "[Link line](bridge-contract.md#link-line)" in bridge-contract.md) and every `key=N` of the statistics line, not only the two it counts above. Keys and values it does not know are skipped. The link line is not shown as a line of the bridge either; a change of it is one line of the launcher, with the time from the start of the run, and it is a `log` event with `--events`:

```text
outside: ETH_EXT -> enp3s0: link down at 12.40 s
outside: ETH_EXT -> enp3s0: link up at 15.10 s
```

The first line of a bridge says nothing when all is well (`up`, `active`). `run` tells apart three kinds of trouble:

| Kind | What `run` does | At the end |
|---|---|---|
| The link or the bus is down for a while (`link=down`, `bus=bus-off`) and the bridge goes on | Goes on. When it has lasted 30 s (`RunLimits::outside_lost_after`), `run` aborts the simulation as for a bridge that ended: an unplugged cable that is plugged in again, or a device that restarts, is a matter of seconds, but a run that is meant to be connected must not go on to its end without the bus. | A warning (`outside-link-down`, `outside-bus-off`: how many times, and how long in all), or, for the 30 s, the error `connect-lost` ("link down for 30 s", "bus off for 30 s"). |
| The bridge is connected, but frames were lost | Goes on | A warning for each kind that happened (below). One frame is enough. |
| The bridge cannot go on | Aborts the simulation | The error `connect-lost` (above) |

Npcap does not read the state of the link, so when the link goes down, neither `outside-link-down` nor `connect-lost` follows: the frames that cannot be sent show in `frames-dropped` and, at the end, in `outside-frames-dropped`.

The kinds of loss are told from the counts of the statistics line and the link line; one table in the launcher (`detail::kOutsideDroppedKeys`) holds the keys:

| Finding | From |
|---|---|
| `outside-frames-dropped` | Any of these counts is not 0: `ext_queue_full`, `ext_tx_failures`, `silkit_tx_failures` (both bridges); `ext_overruns` (Ethernet); `overruns`, `ext_tx_dropped`, `ext_tx_unsent`, `silkit_rejected`, `ext_rejected` (CAN). The message names each count that is not 0. |
| `outside-loop-suspected` | `loop_suspected` is not 0: the Ethernet bridge found a source MAC address on both sides and drops its frames from then on. |
| `outside-bus-errors` | The CAN bridge reported `bus=passive` (error passive): the message has the time in all and `error_frames`. |

The other counts (`own_tx_dropped`, `filtered_to_ext`, `filtered_from_ext`, `mac_unguarded`, `oversize`, `malformed`, `ext_checksum_incomplete`) are no losses of the bridge and give no finding; they are in the result, as the bridge wrote them. The messages say what happened, whether to trust the result and what to check, for example `network 'ETH_EXT' -> enp3s0: the link was down once, for 2.70 s in all. The run went on, but frames sent meanwhile did not reach the devices, ...`. These findings are made at the end, from what was read until then.

With `--events`, each `state` event tells the same while the run goes, for each bridge, in `outside[]` (`condition` `ok`, `interrupted` or `degraded`, with `reasons`; see [the `state` event](simulation-events.md#the-state-event)). A link or bus that is down, or a bus that is off, is `interrupted`; error passive, lost frames or stopped loops (for 5 s after the count grew), and a device that a bridge outside the run shares, are `degraded`.

### A device that another run uses too

A bridge puts a mark on the device it opens and reports how many other vehicle-sil bridges are on it (`shared`, and on Linux their process IDs in `peers`; see "[Device in use by another bridge](bridge-contract.md#device-in-use-by-another-bridge)"). Two runs on one device, or a bridge left over from an earlier run, mix their frames: the devices of this run get frames meant for the other, and the other's get this run's. `run` takes the bridges of its own run out of the count, so only a bridge that is not part of this run counts, and tells it once, with the time since the run began and, on Linux, the process ID:

```text
outside: CAN1 -> can0: also used by another vehicle-sil bridge (process 4321) at 3.10 s
```

The bridge's own warning about it (`warning: ... is also used by another vehicle-sil bridge ...`) is not shown in the run's output, as the link line is not; the bridge's log file (`--log-dir`) has it.

| When | Without `--strict` | With `--strict` |
|---|---|---|
| The device is shared when the bridges have started | The run goes on | `run` starts no ECU and ends the run: the error `outside-device-shared`, result `failed`. It waits up to 2 s (`kOutsideFirstLineWait`) for the first link line of each bridge, which comes before the bridge's lifecycle; a bridge that has not written it by then is taken as one that shares nothing |
| It begins while the simulation runs | The run goes on | When it has lasted 3 s (`kOutsideSharedAfter`), `run` aborts the simulation as for a bridge that ended: the error `outside-device-shared`. A bridge that another run started and that is gone again within 3 s does not stop this run, so one run that stops before its ECUs start does not take the other down |
| At the end | The warning `outside-device-shared`: the process IDs, when it was first seen | The same warning, and `--strict` fails on it; the error where `run` stopped the run |

The message says what happens (the frames of both mix, so what the run says of the devices cannot be trusted) and what to do: connect each run to a device of its own, or stop the bridge left over from an earlier run.

If two networks of this run have different `--connect` values (so the check does not see it) but their bridges report one device, `run` warns with `connect-target-shared` in the form of the check's ("the same device under two names"). Two networks with the same target are the check's (see [simulation-check.md](simulation-check.md)); `run` does not repeat it.

### Where a network can be connected (`vehicle_sil_launch connections`)

```text
vehicle_sil_launch connections [--json] [--network <name>] [--request <file>] [--] <file>
```

`connections` lists, for each `can` and `ethernet` network of the file, where it can be connected on this computer. It asks the network's bridge for its list (`vehicle_sil_ethernet_bridge --list-interfaces --backend <backend> --json`, `vehicle_sil_socketcan_bridge --list-channels --json`, `vehicle_sil_vector_xl_bridge --list-channels --json`) and looks for the bridge as `--connect` does: next to `vehicle_sil_launch`, then in the absolute directories of `PATH`. It connects to nothing: it starts neither SIL Kit nor a participant, and the bridges open no adapter and no channel. It lists each network once for each backend of this OS that it can list (on Linux an Ethernet network twice, `packet` and `tap`; a CAN network `socketcan`; on Windows `npcap` and `xl`), and each bridge is asked once for a backend, also for two networks of its kind. A target of the default backend is written without a prefix, a target of another backend with it, so the `--connect` line can be copied as it is.

- `--network <name>` lists that network only. A name that the file does not have is a usage error.
- The file is read without a variant, because every variant has the networks of the file. `--variant` is a usage error.
- `--request <file>` checks a [request file](#request-files---request) of a test bench against this computer, before a run: for each network that its `connect` gives a target (not `none`) it says whether that target is there and can be used. Only those networks are listed, so no other bridge is started (and no Npcap check is made for a network that the request does not use). `--network` lists that network alone; if the request does not connect it, nothing is checked for it. `connections` reads only `connect` of the file; the other keys are read for their form and not used, and `--connect` is not taken (the file is the only request). A network of the file's `connect` that the configuration lacks is left out with the note `connect-network-unused`, as for `check --request`. A request file that cannot be read is a usage error (exit code 4), with the same message as for `check --request`.
- Errors in the file do not stop the list: the networks that could be read are listed, and the file's errors follow, as `check` reports them (exit code 1).
- The launcher starts each bridge in the bridge's own directory, as for `--connect` (see "[`--connect`](simulation-config.md#--connect)" in simulation-config.md).

An example on Windows, where the Vector XL bridge is not installed (on Linux, the line of an Ethernet network is `--connect ETH_EXT=enp3s0` for `packet` and `--connect ETH_EXT=tap:vsil0` for `tap`, each list under its own heading; a CAN network lists `--connect J1939_CAN1=can0` under `vehicle_sil_socketcan_bridge`):

```text
simulation.yaml: where each network can be connected on this computer
ETH_EXT (ethernet; vehicle_sil_ethernet_bridge, npcap: Npcap version 1.80, based on libpcap version 1.10.5):
  --connect "ETH_EXT=vEthernet (A)"  up, connected; Hyper-V Virtual Ethernet Adapter; \Device\NPF_{0A1B2C3D-0000-1111-2222-333344445555}
  --connect "ETH_EXT=Ethernet 2"     up, disconnected; Intel(R) Ethernet; \Device\NPF_{4E5F6A7B-0000-1111-2222-333344445555}
  cannot be used: \Device\NPF_Loopback  (loopback: not an Ethernet adapter)
J1939_CAN1 (can; vehicle_sil_vector_xl_bridge):
  cannot list: vehicle_sil_vector_xl_bridge is neither next to vehicle_sil_launch (C:/vsil/bin) nor on PATH: build it with the Vector XL Driver Library (docs/bridges.md) and put it next to vehicle_sil_launch, or its directory on PATH [tool-missing]
BODY_LIN (lin): SIL Kit only (there is no bridge for LIN)
```

| Line | Meaning |
|---|---|
| `<network> (<kind>; <bridge>, <backend>: <library>):` | The network, its bridge, and the backend and library version that the bridge reports. |
| `--connect ...` | A target that the bridge can use. Copy the argument to `check` or `run`. After it: the target's name if it differs, and what the bridge says about it. |
| `cannot be used: <target>  (<reason>)` | A target that the bridge lists but cannot use (the loopback), or one that `--connect` does not take: a name that starts with `-`, and the other values that `connect-target` refuses. |
| `cannot be connected with --connect: ...` | The network's name has `=` or starts with `--`, so `--connect` cannot name it: rename the network. |
| `note: ...` | What the bridge adds (Npcap that offers no network adapter), or a target quoted for sh only (below). |
| `cannot list: <message> [<code>]` | The list could not be made (see the codes below). |
| `nothing listed: ...` | The bridge lists nothing on this computer. |
| `requested in <file>: --connect <network>=<target>  usable` | With `--request`: the target of the request file is in the list and can be used. It is compared by backend and name, so `packet:enp3s0` and `enp3s0` are one target. |
| `requested in <file>: ...  not usable: <reason> [<code>]` | With `--request`: the target cannot be used here. The reason says what to do: choose a target from the list, or fix the request file. The codes are below. |
| `<network> (<kind>): SIL Kit only (<why>)` | There is no bridge for the network: none for LIN. |
| `<network>: its kind in the file has an error` | `check` says what is wrong. |

The argument of a `--connect` line is written so that sh (bash, zsh), PowerShell and cmd all read it the same: as it is if it has only ASCII letters, digits and `_ - . : / = +`, otherwise in double quotes. A target with `"`, `$`, `` ` ``, `%`, `!`, the quotation marks `“` `”` `„` (U+201C, U+201D, U+201E, which PowerShell reads as `"`) or a control character, or with `\` at its end or before `"`, `$`, `` ` `` or `\`, cannot be written so for all of them: its line is quoted for sh (`'...'`), and a `note:` line says so.

#### `connections --json`

```json
{
  "vehicle_sil_connections": 1,
  "vehicle_sil": "<version>",
  "file": "simulation.yaml",
  "result": "listed",
  "networks": [
    {
      "name": "ETH_EXT",
      "kind": "ethernet",
      "tool": "vehicle_sil_ethernet_bridge",
      "backend": "npcap",
      "tool_path": "C:/vsil/bin/vehicle_sil_ethernet_bridge.exe",
      "list": {
        "vehicle_sil_targets": 1,
        "tool": "vehicle_sil_ethernet_bridge",
        "backend": "npcap",
        "source": "Npcap version 1.80, based on libpcap version 1.10.5",
        "targets": [
          {
            "value": "vEthernet (A)",
            "label": "vEthernet (A)",
            "detail": "up, connected; Hyper-V Virtual Ethernet Adapter; \\Device\\NPF_{0A1B2C3D-0000-1111-2222-333344445555}",
            "usable": true,
            "reason": null,
            "in_use": false,
            "in_use_by": [],
            "in_use_count": 0,
            "ethernet": {
              "pcap_name": "\\Device\\NPF_{0A1B2C3D-0000-1111-2222-333344445555}",
              "connection": "vEthernet (A)",
              "description": "Hyper-V Virtual Ethernet Adapter",
              "up": true,
              "connected": true,
              "loopback": false,
              "wireless": false
            }
          }
        ],
        "notes": [],
        "error": null
      }
    },
    {
      "name": "J1939_CAN1",
      "kind": "can",
      "tool": "vehicle_sil_vector_xl_bridge",
      "backend": "xl",
      "tool_path": null,
      "list": {
        "vehicle_sil_targets": 1,
        "tool": "vehicle_sil_vector_xl_bridge",
        "backend": null,
        "source": null,
        "targets": [],
        "notes": [],
        "error": {
          "code": "tool-missing",
          "message": "vehicle_sil_vector_xl_bridge is neither next to vehicle_sil_launch (C:/vsil/bin) nor on PATH: build it with the Vector XL Driver Library (docs/bridges.md) and put it next to vehicle_sil_launch, or its directory on PATH"
        }
      }
    },
    {
      "name": "BODY_LIN",
      "kind": "lin",
      "tool": null,
      "backend": null,
      "tool_path": null,
      "list": null
    }
  ],
  "findings": []
}
```

| Key | Content |
|---|---|
| `vehicle_sil_connections` | The version of this form (1). |
| `vehicle_sil` | The vehicle-sil version. |
| `file` | The file as given. |
| `result` | `listed`: every bridge that is on this computer made its list (a bridge that is not, `tool-missing`, is part of the answer). `failed`: a bridge that is there could not make its list, the file has errors (also a network's kind), or a target of the request file cannot be used. `unreadable`: the file cannot be read. `usage`: the command line is wrong. |
| `request` | Only with `--request`. `file`: the request file as given. `connect[]`: one entry for each network that the file connects (not `none`) and that is listed: `network`, `target` as written, `usable` (boolean), and for a target that cannot be used its `code` and `reason` (`null` if usable). |
| `networks[]` | The networks in the order of the file; with `--network`, that one. A network has one entry for each backend that is listed on this computer, one after the other (on Linux an Ethernet network has `packet` and `tap`). |
| `networks[].name` | The network's name. |
| `networks[].kind` | `can`, `ethernet` or `lin`; `null` if its kind in the file has an error. |
| `networks[].tool` | The bridge's executable; `null` where there is no bridge for the network (`lin`) and for an unknown kind. |
| `networks[].backend` | The backend of the entry (`npcap`, `packet`, `tap`, `xl`, `socketcan`); `null` where `tool` is `null`. |
| `networks[].tool_path` | Where the launcher found it; `null` if it did not, or there is no bridge. |
| `networks[].list` | The bridge's list as the bridge wrote it, every key passed on: the form of "[Lists of devices](bridge-contract.md#lists-of-devices)". For a bridge that gives no list, a list of the same form that the launcher makes (below). `null` where `tool` is `null`. |
| `findings` | Why the file cannot be read, the usage error, or the errors of the file, as in `check --json`; otherwise empty. |

`value` of a target with `usable: true` is what `--connect <network>=<value>` takes (a program passes it as one argument, without a shell): the name that the bridge lists, with `<backend>:` before it where `--connect` needs it (a backend that is not the default of this OS, a name that is `none` or begins with a backend's word). The launcher writes it in `value` in place of the bridge's own text. the launcher sets `usable: false` and the `reason` of a target that `--connect` does not take (the `cannot be used` and `cannot be connected` lines above), so the text and the JSON say the same. When the launcher cannot get a list from the bridge, it makes one with no targets and the error. The bridge's own codes (`library-missing`, `permission-denied`, `backend-unavailable`, `driver-missing`, `failed`) come in the bridge's list.

| `error.code` | When |
|---|---|
| `tool-missing` | The bridge is neither next to `vehicle_sil_launch` nor on `PATH`. This is an answer, not a failure: the exit code stays 0. |
| `tool-cannot-start` | The bridge could not be started. On Windows also when it ends with exit status `0xC0000135` or `0xC0000139` before it starts (a DLL is missing or of another version; the Vector XL bridge needs `vxlapi64.dll` of the Vector XL Driver Library). |
| `tool-timeout` | The bridge did not end within 30 seconds. |
| `tool-bad-output` | The bridge wrote no list that the launcher reads (a bridge of another vehicle-sil), or nested deeper than 64 levels. The message quotes the line of its stderr that says why. |

The `code` of a target of the request file that cannot be used (`request.connect[].code`):

| `code` | When |
|---|---|
| `target-not-listed` | The list of the target's backend does not have it, or no list of that backend is made on this computer. Choose a target from the list. |
| `target-unusable` | The list has it with `usable: false`. The reason is the list's. |
| `list-failed` | The list of the network could not be made (its error code is in the reason), or the network's kind in the file has an error. |
| `target-bad` | The target is not one that `--connect` takes for this kind of network on this OS (including a backend that the OS lacks), or the network has no bridge (LIN). |

| Exit code | Case |
|---|---|
| 0 | Every bridge that is on this computer made its list. A bridge that is not there (`tool-missing`) and a network without a bridge here (LIN, CAN elsewhere than on Windows) are part of the answer. |
| 1 | A bridge that is there could not make its list (`tool-cannot-start`, `tool-timeout`, `tool-bad-output`, or the bridge's own error), the file has errors, or with `--request` a target of the request file cannot be used. |
| 3 | The file cannot be read. |
| 4 | Usage error, also a request file that cannot be read. |

## Request files (`--request`)

A request file says what to run: the variant, the scenario and its parameters, the participants to leave out or to start by hand, where the networks are connected outside, and the speed. `check` and `run` read it with `--request <file>`; `connections --request <file>` says before a run whether its targets are there and can be used on this computer (see [Where a network can be connected](#where-a-network-can-be-connected-vehicle_sil_launch-connections)). Put into it what stays the same on one computer or for one kind of run (for example, the adapters of a test bench), and keep the configuration file the same for everyone.

For example, a test bench whose configuration file has the networks `ETH_EXT` and `CAN1` (with a configuration file that has neither, such as the example's, `check` notes both as unused and connects nothing):

```json
{
  "vehicle_sil_run_request": 1,
  "connect": { "ETH_EXT": "enp3s0", "CAN1": "physical:1" }
}
```

```sh
vehicle_sil_launch run simulation.yaml                                           # SIL Kit only
vehicle_sil_launch check --request bench.json simulation.yaml                    # what the next line would run
vehicle_sil_launch run --request bench.json simulation.yaml                      # connected as bench.json says
vehicle_sil_launch run --request bench.json --connect CAN1=none simulation.yaml  # this run: CAN1 on SIL Kit only
```

- An argument wins over the file, field by field (see [Arguments together with a request file](#arguments-together-with-a-request-file)).
- The launcher reads only the file that `--request` names. It never looks for one: not next to the configuration file, not in the working directory or the home directory, and not through an environment variable.
- `check --request` judges the same request as `run --request`, the speed and `start_paused` for the [time lock](#time-of-a-run-that-is-connected-outside---time-policy) included. Check first, then run. One rule is left to `run`: that `start_paused` needs `--stdin-commands` or `--control`, which are arguments of `run` only.
- `test` does not take `--request` (usage error).
- Write the file as JSON. The launcher may read a looser form, but that is no promise: the schema and the other tools that read the file take JSON only.
- A mistake in the file (an unknown key, a key written twice, a value of the wrong type, `null`, another version) is a usage error (exit code 4) that names the file and the line; nothing is checked or started. Leave a key out to not set it.
- The JSON Schema is [schema/run-request.schema.json](../schema/run-request.schema.json). An editor completes and checks a file that names it in `"$schema"`.
- What the caller of the launcher decides is not in the file: the configuration file, `--log-dir`, `--timeline`, `--timeout-ms`, `--control`, `--events`, `--stdin-commands` and `--json` are arguments only.
- `examples/ecu_main/requests/throttle_step.json` is a request file of the example: `vehicle_sil_launch run --request requests/throttle_step.json simulation.yaml`.

### Keys of a request file

| Key | Value | The same as |
|---|---|---|
| `vehicle_sil_run_request` | `1` (required) | |
| `$schema` | The path or URL of the schema, for an editor. The launcher skips it. | |
| `variant` | A text | `--variant` |
| `params` | A mapping of parameter names to values (a number, `true` or `false`, or a text). Used only when a scenario runs. | `--param` |
| `scenario` | The path of the scenario file. A relative path is from the directory of the configuration file. | `--scenario` |
| `disable` | A list of participant names | `--disable` |
| `enable` | A list of participant names | `--enable` |
| `manual` | A list of participant names | `--manual` |
| `connect` | A mapping of network names to targets. A target is written as `--connect` takes it; `"none"` is the same as not writing the network. | `--connect` |
| `time_policy` | `"realtime"` or `"free"` | `--time-policy` |
| `speed` | A number from 0.000001 to 1000000, or `"max"` | `--speed` |
| `start_paused` | `true` or `false`. `run` also needs `--stdin-commands` or `--control`, a way to go on. | `--start-paused` |
| `strict` | `true` or `false` | `--strict` |

### Arguments together with a request file

An argument wins over the file, field by field.

| Key | With an argument for the same thing |
|---|---|
| `variant`, `scenario`, `speed`, `time_policy` | The argument replaces the file's value. |
| `start_paused`, `strict` | On if the file or the argument says so. No argument switches them off. |
| `connect` | By network: `--connect <network>=<target>` replaces the file's target of that network, and `--connect <network>=none` takes it away for this run. The other networks of the file stay. |
| `params` | By name: `--param <name>=<value>` replaces the file's value of that parameter. The others stay; their `from` in the check JSON is `request_file`. |
| `disable`, `enable`, `manual` | The names of the file and of the arguments add up. Where they disagree about a name (the file's `disable` and `--enable`, the file's `enable` or `manual` and `--disable`, the file's `disable` and `--manual`), the argument wins. |

Where a request file differs from the arguments:

| Case | Arguments | Request file |
|---|---|---|
| A relative scenario path | From the working directory | From the directory of the configuration file |
| A network to connect that the configuration file does not have | The error `connect-network` | The note `connect-network-unused`; the network is left out (one file serves several configuration files) |
| A parameter that the scenario does not have | Usage error | Usage error if the file names the scenario itself (`params idle_rmp in bench.json: …`). Otherwise (the scenario is from `--scenario`, which replaces the file's) ignored, with the note `request-params-unused` (one file serves several scenarios) |
| Parameters without a scenario | Usage error | Ignored, with the note `request-params-unused` (one file serves runs with and without a scenario) |

A participant in `disable`, `enable` or `manual` that the configuration file does not have is a usage error from the file too, as it is from an argument; the message names the key and the file (`manual Engine in bench.json: …`).

A message about a value from the file names the key and the file, not an option: `speed 5 in bench.json is refused`. To run without a network that the file connects, the message says to take it out of `connect` in the file or to pass `--connect <network>=none`. With `"time_policy": "free"` in the file, the notes `outside-not-real-time` name the file too, because the file lifts the time lock for everyone who uses it (see [Time of a run that is connected outside](#time-of-a-run-that-is-connected-outside---time-policy)).

## Stopping a run

When the simulation ends (also after an abort and on Ctrl+C), `run` ends the participants in this order:

1. Wait up to `stop_timeout_ms` for the required participants to end on their own (ecu_main executables end on their own when the simulation ends).
2. Ask the participants that are still running to stop (Linux: SIGTERM to the participant's process group. Windows: Ctrl+Break to its process group). Participants with `required: false` are stopped here, without a warning. For a required participant, this is an error (`did-not-end`).
3. Wait up to another `stop_timeout_ms` for the participants asked to stop, and the processes they started (process group; on Windows, job object), to end. Kill whatever remains, together with the processes it started (Linux: SIGKILL to the process group. Windows: terminate the job object).
4. If a participant ended on its own but processes it started remain, end them and report a warning (`left-processes`).

| Case | Difference |
|---|---|
| A participant that you start yourself (manual) | None of the items apply (see "How to stop" in [Starting a participant yourself](simulation-config.md#starting-a-participant-yourself-manual---manual)). |
| The simulation did not start | Item 1 is skipped and the order starts at item 2 (this is not an error of that participant). |
| On Windows, `vehicle_sil_launch` has no console | Ctrl+Break cannot be sent, so `run` kills the participant at once instead of item 2 (the message says so). |
| A second stop request (Ctrl+C or the `stop` command again), or one that comes during item 1 | The waits are skipped: `run` asks the participants to stop at once and kills what does not end. A participant killed this way gives no `did-not-end`. In a run that a `stop` command stopped, the note `ended-by-force` and the `message` of the result name it; the exit code stays 0 (you chose not to wait). |

An ecu_main executable that is asked to stop before the start ends at once (see [ecu-executable.md](ecu-executable.md)).

Each participant runs in its own process group (on Windows, a process group and a job object). Ctrl+C in the terminal reaches only `vehicle_sil_launch`, not the participants. On Windows, when `vehicle_sil_launch` has no console (no console to share with the participants), the participants are started without opening a console window.

### Closing the connection of `run` itself

After ending the participants, `run` closes its own SIL Kit connections (the connection to ECU variables, the controller, the registry) and then writes the result. The deadline is 5 seconds plus the longer of `stop_timeout_ms` and 5 seconds. If the connections have not closed by then, `run` stops waiting, adds the warning `connection-not-closed`, writes the result and exits the process without closing them. The rules for the result and the exit code do not change.

Another Ctrl+C or `stop` command also cuts this wait short. `run` then writes one line that says so and exits the same way. The user asked for this, so it is not a finding.

## Interrupting (Ctrl+C)

On Ctrl+C (SIGINT), SIGTERM or SIGHUP (Windows: Ctrl+C, Ctrl+Break, closing the console), `vehicle_sil_launch` stops the simulation in order (the controller's stop), ends the participants as described in [Stopping a run](#stopping-a-run), records the interruption in the result and exits with code 130. The end of stdin with `--stdin-commands` and a closed stdout with `--events` count the same way: the program that ran the launcher has gone. A run that a command stopped (`{"command":"stop"}`, `Stop` of the Control API) is not an interrupted run: see [How a run ended](#how-a-run-ended-end-ok-message).

| When it arrives | What `run` does |
|---|---|
| During the check | Interrupts after the check has finished (nothing is started). |
| Before the start | Stops the participants. |
| After the simulation has ended, while waiting for participants to end on their own | Stops that wait and asks them to stop (item 2 of "Stopping a run"). |
| The second time | Stops waiting and kills the remaining participants at once. |

| Signal or event | Handling |
|---|---|
| A signal that the program which started `vehicle_sil_launch` set to be ignored (SIGHUP with `nohup`, for example) | Stays ignored. |
| Ctrl+Z (SIGTSTP) in the terminal | Stops only `vehicle_sil_launch`. |
| Closing the console on Windows | Windows ends the process within a few seconds. |

On Ctrl+Z, the participants keep running because they are in other process groups; only the registry and the controller stop. When the console is closed on Windows, the job objects end the participants, but the result may not be written.

When a program drives `vehicle_sil_launch`, pass `--stdin-commands`, then write `{"command":"stop"}` to stdin or close stdin. The run then stops in order, as on Ctrl+C (see [Programmatic use](simulation-events.md)). This also stops in order a `vehicle_sil_launch` on Windows that has no console and so can receive neither signals nor Ctrl+Break. When the simulation stops, ecu_main executables end on their own. A participant that does not end on its own is killed at once instead of item 2 of "Stopping a run", because Ctrl+Break cannot be sent.

## Leaving no participant behind

Before it exits, `vehicle_sil_launch` ends every participant it started. Participants inherit no files or handles of `vehicle_sil_launch` other than stdin (empty), stdout and stderr (pipes that `vehicle_sil_launch` reads).

| OS | When `vehicle_sil_launch` is killed (including SIGKILL and crashes) |
|---|---|
| Windows | The participants and the processes they started end. |
| Linux | The participants receive SIGKILL, but processes they started can remain. |
| POSIX systems other than Linux | Not supported (participants remain). |

- Windows: each participant is in a job object, which ends the processes in it when its last handle closes. This holds however `vehicle_sil_launch` ends.
- Linux: participants receive SIGKILL when `vehicle_sil_launch` ends (the parent-death signal). Processes started by a participant remain after a kill because items 3 and 4 of "Stopping a run" do not run.
- Linux: processes that have left the participant's process group (by calling `setsid()` or similar) cannot be ended in any case.
- POSIX systems other than Linux have no parent-death signal.

## Listen addresses

A SIL Kit participant listens for connections from other participants. The addresses it listens on depend on whether the host of the registry URI is a loopback address.

| Host of the registry URI | Addresses participants listen on | Connections from other computers | Windows Firewall |
|---|---|---|---|
| A loopback address (always, in a run without `registry_uri`) | Loopback address only | Not possible. | Does not ask for permission to listen. |
| A non-loopback address (`silkit://0.0.0.0:8500`, for example) | All addresses, as SIL Kit does by default | Possible. | See "Windows Firewall" below. |

This table applies to the participants whose SIL Kit configuration vehicle-sil creates: ecu_main executables, programs that use `ParticipantRuntime` or `Simulation`, `vehicle_sil_can_bus_simulator`, the Vector XL CAN Bridge and the controller of `run`.

When participants listen on loopback only, TCP uses `127.0.0.1` (`::1` if the URI uses `::1`). On systems other than Windows, they also listen on a local-domain socket in a temporary directory.

With a non-loopback address, anyone who can connect to this computer can join the simulation, because SIL Kit has no authentication (note `registry-on-network`).

The registry listens on the address of its own URI, over TCP only (no local-domain socket). If another program or another `run` uses that port, or the host of `registry_uri` is not an address of this computer, `run` starts nothing and reports the `setup` error (exit code 3, `the registry cannot listen at <URI>: ...`). In parentheses, the error says which of these is likely and what to write in `registry_uri` instead.

### What counts as a loopback address

| Form | Examples |
|---|---|
| An IPv4 address in `127.0.0.0/8` (any numeric form that `getaddrinfo()` reads) | `127.0.0.1`, `127.1` |
| `::1` | `[::1]`, `[0:0:0:0:0:0:0:1]` |
| `127.x.x.x` mapped to IPv6 | `[::ffff:127.0.0.1]` |
| `localhost` (in any case) | `localhost`, `localhost.` |

Any other name counts as non-loopback, without name resolution.

### Windows Firewall

When `registry_uri` is not a loopback address, Windows Firewall asks whether the registry (`vehicle_sil_launch`) and the participant programs may listen, because both listen on all addresses.

Only `vehicle_sil_launch`, which hosts the registry, needs permission. Participant executables can join without it, because the participants connect out to the computer that runs the launcher. Participants that cannot connect to each other directly connect through the registry, which relays between them (see "SIL Kit versions that can be mixed" in [dependencies.md](dependencies.md#sil-kit-versions-that-can-be-mixed)).

| Permission | What happens |
|---|---|
| `vehicle_sil_launch` is not allowed | ECUs that join from other computers fail after about 5 seconds. |
| A participant on the computer that runs the launcher is not allowed | Each join from another computer takes about 5 seconds. |
| A participant on the joining computer is not allowed | In the tested setups, it joins normally. |

For the tested setups and the commands that grant permission, see "Firewall rules" and "Tested setups" in [Run on several computers](howto/multi-pc.md).

### The URI printed for joining from another computer

When `registry_uri` is not a loopback address, `run` writes a line with the URI for joining from other computers after the `registry at` line.

```text
[vehicle_sil_launch] from other computers, join at silkit://192.0.2.10:8500 (this computer's address): give it as --registry-uri to a participant started by hand on another computer ...
```

| Host of `registry_uri` | Address shown |
|---|---|
| `0.0.0.0` | The IPv4 addresses of this computer's active networks (except loopback and link-local `169.254.x.x`) |
| `[::]` | The IPv6 addresses (except loopback and link-local) and, on Linux, the IPv4 addresses |
| A specific address | That URI |

- The address of the network with the default route comes first. If there are other addresses, the line adds that the first one is that address. This is because addresses of the virtual networks of WSL, Docker and virtual machines often cannot be reached from other computers.
- On Linux, `[::]` also lists IPv4 addresses, because `[::]` also accepts IPv4 unless `net.ipv6.bindv6only` is `1`.
- On Windows, `[::]` accepts IPv6 only, so the line adds that for IPv4 you write `silkit://0.0.0.0:<port>`.
- If no address is found, the line says to look one up with `ipconfig` (Windows) or `ip addr`.

### The environment variable `VEHICLE_SIL_LISTEN_ON_NETWORK`

| Value | Meaning |
|---|---|
| `1` | Listen on all addresses even if the registry URI is a loopback address. |
| `0`, empty, unset | The rules above apply. |
| Any other value | An error (the participant cannot be created and fails with a message that gives the variable's name and value). |

`run` always passes this variable to the participants it starts: `1` if `registry_uri` is not a loopback address, otherwise `0`. It does not pass on the value from the environment of `vehicle_sil_launch` itself, and the controller does not use it. If that value differs from the one `run` passes, `run` reports the note `listen-variable-ignored`.

Why `1` is passed: when `registry_uri` is `0.0.0.0` (or `[::]`), `run` gives the participants on this computer a URI with `127.0.0.1` (`[::1]`). With only that, they would listen on loopback only, and SIL Kit passes that address unchanged to the participants on other computers too. A participant that receives it tries it on its own loopback address. If another program listens on the same port there, the participant connects to that program, waits for a reply forever and fails to join. Listening on all addresses prevents this.

For a participant that you start yourself (manual), the value to set appears in the command line that `run` prints and in `environment` of the `participant` event (`manual`), only when it is `1`. Also set `1` yourself for any other participant that runs on the registry's computer and connects to it through a loopback address (such as `127.0.0.1`), when participants on other computers join the same registry.

### Participants with their own SIL Kit configuration

| Participant | Addresses it listens on |
|---|---|
| A SIL Kit participant configuration is passed and has `Middleware.AcceptorUris` | As configured. |
| A SIL Kit participant configuration is passed without `Middleware.AcceptorUris` | The rules above (vehicle-sil adds them to the configuration). |
| A SIL Kit program that does not use vehicle-sil (a `describe: false` participant, for example) | As its own configuration says (all addresses if there is no `Middleware.AcceptorUris`). |

Pass a SIL Kit participant configuration with `--silkit-config` of ecu_main, `vehicle_sil_can_bus_simulator --participant-configuration` and similar options (see "Passing a SIL Kit configuration file (`--silkit-config`)" in [ecu-executable.md](ecu-executable.md#passing-a-sil-kit-configuration-file---silkit-config)).

## Output

| Destination | Content |
|---|---|
| Stderr | Participant output and the progress of `vehicle_sil_launch` itself |
| `--log-dir` files | Output per participant |
| Stdout | Only the result (human-readable or `--json`; events with `--events`) |

With `--events`, the lines that would go to stderr (participant lines and the progress of `vehicle_sil_launch`) go to stdout as events instead (see [Programmatic use](simulation-events.md)).

### Stderr

Each line of a participant's stdout and stderr goes to the stderr of `vehicle_sil_launch` with `[<participant name>] ` in front. The progress lines of `vehicle_sil_launch` itself start with `[vehicle_sil_launch] `. They report the registry URI, the URI for joining from other computers, the commands started, the start, virtual time not advancing, the end, and stops.

| Line content | Handling |
|---|---|
| A line longer than 4096 bytes | Split between UTF-8 characters. |
| Control characters (C0 except tab, DEL, C1 from U+0080 to U+009F) | Replaced with `?`. |
| Bytes that are not UTF-8 | Replaced with `?`. |

Participants must write UTF-8. The Windows console also shows the output as UTF-8, so characters in other code pages come out garbled.

If the reader of stderr stops (a pipe that nobody reads), `vehicle_sil_launch` also blocks where it writes to stderr.

### Log files (`--log-dir`)

With `--log-dir <dir>`, each participant's output is also written to `<dir>/<participant name>.log` (without `[name]`). The directory is created if it does not exist. `participants[].log` in the JSON names the file.

Logs from a previous run are overwritten only where the names match. Logs of participants that are not in the current configuration remain.

| Participant name | File name |
|---|---|
| Bytes other than ASCII letters, digits, `.`, `-` and `_` | Replaced with `_`. |
| Windows device names (`NUL`, `CON`, `PRN`, `AUX`, `COM1`–`COM9`, `LPT1`–`LPT9`, also before a `.`) | `_` is appended, on every OS. |
| Names that differ only in case | Treated as a collision; `-2` or similar is appended. |

### Stdout

Stdout gets only the result. The same result on the same computer gives the same output (it contains no times of day, durations, process IDs or registry URI). These values, which show how time progressed, differ from run to run:

- The `time:` line of the human-readable output
- `wall_ms`, `actual_speed` and `paused_ms` in `timing` of the JSON
- The lateness counts in `timing` of the JSON (`late_steps`, `max_late_ms`, `slipped_ms`, `late_ms`)

The network traffic counts (the `traffic:` line and table of the human-readable output, `traffic` in the JSON) are the same in every run when only ECUs that run in virtual time take part, all of them are there before the start, and the run ended at `stop_at_ms` or at the end of a scenario. They can differ from run to run in these cases:

- A run that ended by a stop request, `--timeout-ms`, a participant's failure or a participant that stopped the simulation (the virtual time at which it ended, `sim_ms`, differs, and so do the counts).
- A network on which a participant that does not run in virtual time (a real-time `Peer`, a bridge) sends frames (the receivers' counts differ).
- A participant that joined later (`manual`, `required: false`), or one that was started again (whether it is counted, and why not, differ).
- A short run with a `describe: false` participant (`run` does not wait for the connection to that participant's variables, so if it connects late, the participant is `no_sample` or `no_control_link`).

Line endings are LF on every OS. When writing to the Windows console, both `check` and `run` show the text as UTF-8 characters, whatever the console code page.

### Human-readable result

The human-readable result has these parts, in this order:

| Order | Content |
|---|---|
| 1 | The check tables and the check findings |
| 2 | The simulation line, which starts with `run:` |
| 3 | The `time:` line (when a speed is set, or when there was a paced time step) |
| 4 | With `--connect`, one `outside:` line for each connected network (`outside: ETH_EXT -> vEthernet (A) (bridge.ETH_EXT): 152 frames out, 3 from devices`; without the counts when they are not known) |
| 5 | The participant table (whether each is required, and how it ended; the bridges of `--connect` last) |
| 6 | The traffic on each network (the `traffic:` line and table; see [Network traffic](#network-traffic-traffic) below) |
| 7 | The run findings |
| 8 | The result line (`passed:`, `stopped:`, `incomplete:`, `failed:` or `interrupted:`, with the numbers of errors, warnings and notes from the check and the run together) |

If the check has errors, the `nothing was started:` line follows the check tables and findings.

<!-- vehicle_sil_launch run simulation.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

traffic: J1939_CAN: 1 participant sent, 1 received (2 counted) (steps before 1000 ms)

network    participant  sent  received
J1939_CAN  Engine         11         0
J1939_CAN  Dashboard       0        11

passed: 0 errors, 0 warnings, 0 notes
```

The `time:` line gives the speed, the overall actual speed (excluding pauses), the number of paced time steps and how many of them were later than the tolerance, the largest lateness, the total slip, and the time spent paused (if any). Here is the line from a run with `--speed 4`. Values between `<` and `>` differ from run to run. The time step follows the step size of Dashboard and becomes 5 ms, so there are 200 time steps.

<!-- vehicle_sil_launch run simulation.yaml --speed 4: the line of the time -->
```text
time: speed 4, ran at <actual_speed> times real time; <late_steps> of 200 paced steps more than 10 ms late (the latest by <max_late_ms> ms), <slipped_ms> ms behind in all
```

The `ended` column of the participant table:

| Shown | Meaning |
|---|---|
| `exit code <n>` | Ended on its own (a Windows NTSTATUS value is in hex, for example `0xC0000005`) |
| `signal <name>` | Ended by a signal (Linux; for example `signal SIGSEGV`) |
| `stopped (SIGTERM), exit code <n>` | Ended after being asked to stop (`Ctrl+Break` on Windows) |
| `killed` | Killed |
| `not started` | Could not be started (or the run ended before it was started) |
| `not started (disabled)` | A disabled participant (the `required` column shows `-`) |
| `started by you (manual)` | A participant that you start yourself, which joined the simulation (`run` cannot know how it ended) |
| `not joined (manual)` | A participant that you start yourself, which did not join |

An example with a disabled participant. The check part is the same as the output of `check` (see [Disabling a participant](simulation-config.md#disabling-a-participant-enabled---disable---enable)).

<!-- vehicle_sil_launch run simulation.yaml --disable Dashboard -->
```text
simulation.yaml: 2 participants (1 disabled), 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    -      disabled

network    kind  participants
J1939_CAN  can   Engine

warning: simulation.yaml:8: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it; if it talks only to equipment outside, connect it outside to run it (external says what passes) [single-participant]
note: simulation.yaml:14: participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it [participant-disabled]

run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       exit code 0
Dashboard    -         not started (disabled)

traffic: J1939_CAN: 1 participant sent, 0 received (1 counted) (steps before 1000 ms)

network    participant  sent  received
J1939_CAN  Engine         11         0

passed: 0 errors, 1 warning, 1 note
```

### Network traffic (`traffic:`)

When the simulation started, the result shows after the participant table whether J1939 frames arrived on each network. The counts are the monitoring outputs `j1939.<network name>.frames_sent` and `frames_received`, which each ECU sends through its connection to the ECU variables (see [Connection to ECU variables](#connection-to-ecu-variables-control_link)). An ECU's own variables cannot have these names: the prefix `j1939.` is reserved (see "Reserved variable name prefixes" in [ecu-executable.md](ecu-executable.md#reserved-variable-name-prefixes)).

- The `traffic:` line gives, for each network of the configuration file, how many of the counted participants sent one frame or more, and how many received one or more (`J1939_CAN: 1 participant sent, 1 received (2 counted)`). In the first example above, Engine sends and Dashboard receives. Dashboard only receives, so 1 sent and 1 received is the normal result. The numbers are not ratios and need not equal `counted`. Frames did not arrive on a network with a sender, `0 received`, and only `counted` in the parentheses.
- If a participant was not counted, the parentheses add `1 not counted`. A network on which nobody sent and nobody received, with every participant counted, shows `nothing sent (2 counted)`. Otherwise a network on which nobody sent shows the numbers (`0 participants sent, 1 received (1 counted, 1 not counted)`). A network without a counted participant (a network used for LIN, Ethernet or raw CAN, for example) shows `not counted (3 participants)`. A network on which the launcher does not know who is on it shows `not counted (no participant known to be on it)`, with `, 1 unplaced` inside the parentheses if a participant is unplaced on it (below). Without any network in the configuration file, there is only the line `traffic: no network in use`.
- A `describe: false` participant of the run is unplaced on a network when its connection to ECU variables has no counters for that network. For such a participant, only the counters tell the launcher where it is, and nothing tells where it is not: it may use any other network without counters (as raw CAN, LIN or Ethernet). It may have received the frames of that network, so the parentheses of the network add `1 unplaced`, and the launcher does not say that frames did not arrive there. The reason is at the right of the participant's row (the table of reasons is below). A participant that the launcher knows on no network (a logger, a bridge or another tool) has one row after the networks, with `-` as the network. A participant that its counters place on one network has a row under each other network.
- With a `describe: false` participant in the run, a network on which it is unplaced therefore never shows frames as not arrived (`delivered` in the JSON is `true` or `null`, not `false`). This is the case for every network of a configuration with a logger or a bridge.
- The table has one row for each network and participant. The network name is the simulation's name (after `network_names`). The table lists only participants that the launcher knows to be on the network: those whose `--describe` output names the network, and, for `describe: false`, those whose variables include the counters of the network. A `describe: false` participant without a connection to ECU variables (a bridge or another tool) is not listed with counts under a network, even if it sends frames: it is unplaced (above). A network with no listed participant has one row with `-` as the participant.
- The counts are the values of each participant's last step that started before the virtual time at which the run ended (`steps before <ms> ms` in the line, `before_ms` in the JSON). Values of later steps may or may not arrive before the run ends, so they are not used.
- `sent` is the number of frames that the port accepted for sending before that step, not the number that went out on the bus (Address Claimed messages count too). `received` is the number of J1939 frames of other participants that the port delivered (frames of another node of the same participant, and frames that are not J1939, do not count).
- A participant that was not counted has `-` as its counts and the reason at the right of its row:

| Shown at the right | `not_counted` in the JSON | Meaning |
|---|---|---|
| `not counted: no control link` | `"no_control_link"` | Its connection to ECU variables did not connect (`--no-control`, or an executable without one) |
| `not counted: restarted` | `"restarted"` | A process with the same name was started again (its counts start again from 0) |
| `not counted: real time` | `"real_time"` | It does not run in virtual time (the times of its values are not the simulation's). This includes a participant that reports virtual time but whose values ran more than 64 controller time steps ahead |
| `not counted: no J1939 counters` | `"no_counters"` | It has no counters for that network (it uses the network as raw CAN, LIN or Ethernet) |
| `not counted: counters mapped by variables` | `"mapped"` | The configuration's `variables` maps one of its counters (its values differ from the executable's) |
| `not counted: no sample before the end` | `"no_sample"` | No value of a step before the end arrived |

- An unplaced participant has `-` as its counts and one of two reasons at the right of its row:

| Shown at the right | `reason` in the JSON | Meaning |
|---|---|---|
| `unplaced: no control link` | `"no_control_link"` | No connection to its ECU variables had connected when the run ended. Either it has none (a logger, a bridge, another tool, `--no-control`), which is the same in every run, or its connection came too late in this run (`run` does not wait for the connection of a `describe: false` participant). The launcher cannot tell these apart. If the participant has a connection, a longer run places it |
| `unplaced: no J1939 counters` | `"no_counters"` | Its connection to ECU variables has no counters for the network (in the row with `-` as the network: for any network of the configuration file). This is the same in every run of the configuration |

### Run findings (`code`)

| `code` | Severity | Meaning |
|---|---|---|
| `setup` | Error (exit code 3) | The registry cannot be started (the port of `registry_uri` is in use, for example), the controller cannot be created, `--log-dir` cannot be written, or the `--timeline` file cannot be opened |
| `start` | Error (warning for `required: false`) | A participant cannot be started |
| `ended-before-start` | Error | A required participant ended before the simulation started |
| `not-joined` | Error | The process of a required participant is running, but it did not join within `start_timeout_ms` |
| `simulation-start` | Error | All required participants joined, but the simulation could not be started |
| `participant-failed` | Error | A required participant failed during the run, and the simulation was aborted |
| `connect-failed` | Error | The bridge of `--connect` did not get to run before the simulation started, so the simulation was not started; if it failed before the participants of the file were started, they were not (see [Connecting a bus outside](#connecting-a-bus-outside---connect)) |
| `connect-lost` | Error | The bridge of `--connect` ended while the simulation was running, and the simulation was aborted; or it had ended when the simulation ended; or its link or bus was down for 30 s (see [The state of the link and the bus](#the-state-of-the-link-and-the-bus)) |
| `outside-time-changed` | Note | In a run that is connected outside with `--time-policy free`, a command paused or stepped the simulation, or set a speed other than 1, while what is connected went on (one for the run, about the first such command; see [Time of a run that is connected outside](#time-of-a-run-that-is-connected-outside---time-policy)) |
| `unsynchronized-time-changed` | Note | In a run with a participant that runs in real time (no step), a command paused or stepped the simulation, or set a speed other than 1, while that participant went on (one for the run, about the first such command) |
| `connect-nothing-received` | Note | With `--connect`, the bridge of a network sent frames to the real bus, and none came from a device there (one for each such network; see [Counting the frames to and from the bus](#counting-the-frames-to-and-from-the-bus)) |
| `outside-link-down`, `outside-bus-off` | Warning | The link or the bus of a connected network was down while the simulation ran, and the bridge went on: how many times and how long in all (see [The state of the link and the bus](#the-state-of-the-link-and-the-bus)) |
| `outside-bus-errors`, `outside-frames-dropped`, `outside-loop-suspected` | Warning | The bridge of a connected network was error passive, dropped frames (even one), or stopped a loop of MAC addresses (the same section) |
| `outside-device-shared` | Warning (error where `--strict` stopped or did not start the run) | The device of a connected network was also used by a vehicle-sil bridge that is not part of this run: the process IDs, and when it was first seen (see [A device that another run uses too](#a-device-that-another-run-uses-too)) |
| `connect-target-shared` | Warning | Two networks of the run have different `--connect` values, but their bridges report one device (the same finding as the check's, for the case the check cannot see; same section) |
| `exit-code` | Error (warning for `required: false`) | A participant's exit code is not 0, or it ended by a signal (when none of the above applies) |
| `did-not-end` | Error | A required participant was still running `stop_timeout_ms` after the end of the simulation |
| `left-processes` | Warning | Processes started by a participant were still running after the participant ended |
| `connection-not-closed` | Warning | The SIL Kit connections of `run` itself did not close within the deadline (see [Stopping a run](#stopping-a-run)) after the participants were ended |
| `timeout` | Error | The simulation did not end within the real time of `--timeout-ms`, so it was aborted |
| `simulation` | Error | The simulation did not end normally (and the participant responsible is unknown) |
| `ended-before-stop-at` | Note | A participant stopped the simulation before `stop_at_ms` |
| `control-link-no-hello` | Warning | The hello of an ECU that should expose variables, or its reply that it can take commands, did not arrive before the start |
| `control-link-dropped` | Warning | `vehicle_sil_launch` dropped messages of the connection to ECU variables |
| `control-link-version` | Warning | The version of the message format in a received hello differs from that of `vehicle_sil_launch` |
| `control-link-late-values` | Warning | Values or results of an ECU arrived later than the virtual-time guarantee allows |
| `variable-map-not-applied` | Error | Some entries of the `variables` map in the configuration file do not match the variables that the ECU reported when it connected |
| `before-start` | Error | An action to perform while waiting for the start failed |
| `behind-real-time` | Warning | Some of the paced time steps were later than `late_tolerance_ms` |
| `coarse-time-step` | Warning | A speed is set, and the controller's time step is coarser than the ECU steps |
| `timeline-write` | Error | The `--timeline` file was opened but could not be written after the run |
| `timeline-limit` | Note | The `--timeline` file dropped older points at its limits |
| `interrupted` | Note | The run was interrupted (a signal, the end of stdin, a closed stdout) |
| `stopped` | Note | A command stopped the run (`{"command":"stop"}`, `Stop` of the Control API) |
| `ended-by-force` | Note | In a run that a command stopped: participants that did not end when asked to were killed without the full wait, because the stop was asked for again (or while `run` waited for the participants to end) |

Notes on individual codes:

| `code` | Notes |
|---|---|
| `connection-not-closed` | Gives the time waited. The result is complete. |
| `connect-failed`, `connect-lost` | Give the network, the target, how the bridge ended and the line in which it said why (see [Connecting a bus outside](#connecting-a-bus-outside---connect)). They take the place of `start`, `ended-before-start`, `not-joined`, `participant-failed` and `exit-code` for the bridge. `connect-failed` names `vehicle_sil_launch connections`, which lists where each network can be connected. |
| `timeout` | In a run with a required manual participant, also reported when the simulation did not start (it names what the run was waiting for). |
| `control-link-no-hello` | The message says whether the hello or the reply did not arrive (see [Connection state](#connection-state-state)). |
| `control-link-dropped` | Gives the numbers of rejected hellos, of unreadable messages, and of messages that matched no known ECU or none of its variables. |
| `control-link-version` | When the version in the `--describe` output differs, it is a check finding with the same `code`, and the run does not repeat it. |
| `control-link-late-values` | The ECU sends from outside its step (an error in hand-written communication code). |
| `variable-map-not-applied` | Says which entries do not match, and why. |
| `before-start` | The action is supplied by the code that calls `run` as a function (internal use). |
| `behind-real-time` | Gives the number of late time steps, the largest lateness, the total slip, the overall actual speed and, if the run ended while standing still, the lateness at that moment. In a run that keeps real time because it is connected outside, it also says that what is connected does not wait: the run was that far off from it. |
| `coarse-time-step` | Gives by how many ms at most ECU steps are delayed and run bunched together, and the value to write in `time_step_ms`. |
| `timeline-write` | The file holds only part of the record, so do not use it. The scenario result does not change, but this finding fails the run (exit code 1). Free some disk space or name another file, then run again (see [Recording a scenario run on a timeline](#recording-a-scenario-run-on-a-timeline---timeline)). |
| `timeline-limit` | Gives the number of dropped points. The points at the end are all kept. The series and times are in `limits.dropped` of the file. If you need every point, make the scenario shorter. |

`connection-not-closed` should not happen with any configuration, so it indicates a bug in `vehicle_sil_launch` or SIL Kit. Report it with the result and the output of `run`.

`control-link-dropped` has 4 possible causes. The variables of the participant concerned can be missing or stale.

- More participants expose variables than the limit (256).
- An ECU built with a different vehicle-sil version is present.
- Another program writes to the same topics (`vehicle_sil.control.*`).
- The hello of a participant that joined during the run had not arrived yet.

If `control-link-version` appears, the message says which side is older: rebuild an older ECU with the vehicle-sil of `vehicle_sil_launch`, or use a `vehicle_sil_launch` of a newer ECU's vehicle-sil version.

With `variable-map-not-applied`, the entries that do not match are not applied. Those variables appear with the executable's names and values, and writes to the mapped names of those entries are rejected. The same information is also written once to the run's lines (`log`): at the start for an ECU that is connected by then, and at the end of the run for one that connected later. The check catches mismatches before the run, so this finding appears only for `describe: false` participants, or for executables that report variables different from their `--describe` output (see [Matching variable names and values](simulation-config.md#matching-variable-names-and-values-variables)).

When `before-start` appears, the simulation had started and was aborted at once.

`behind-real-time` and `coarse-time-step` belong to no participant (see [Running at a set speed](#running-at-a-set-speed-speed)).

## Exit codes

These are the exit codes of `run`. For the full table that includes `check` and `test`, and for the list of usage errors, see "Arguments and exit codes" in [simulation-config.md](simulation-config.md#arguments-and-exit-codes). The exit codes of `test` are also in [Running several scenarios](#running-several-scenarios-vehicle_sil_launch-test).

| Exit code | Meaning | `result` of `--json` |
|---|---|---|
| 0 | The simulation ended normally and every required participant exited with 0 (with `--strict`, also no warnings) | `"passed"` |
| 0 | A command stopped the run (`{"command":"stop"}`, `Stop` from a Control Server client), and nothing was wrong. A participant that was killed because the stop was asked for twice is not an error here (the note `ended-by-force` names it) | `"stopped"` |
| 1 | Check errors; nothing was started | `"check_failed"` |
| 1 | Run errors (a participant failed, the participants did not all join, a participant did not end when stopped, and so on) | `"failed"` |
| 3 | The configuration file cannot be read (missing file, syntax error, unknown format version, and so on) | `"unreadable"` |
| 3 | The registry or the controller cannot be set up (finding `setup`) | `"setup_failed"` |
| 3 | The result cannot be written to stdout, or an unexpected failure of `vehicle_sil_launch` itself | |
| 4 | Usage error | `"usage"` |
| 5 | A command stopped the run before its scenario's end (or before the scenario began), and nothing else was wrong: the scenario has no verdict | `"incomplete"` |
| 130 | Interrupted: a signal (Ctrl+C, SIGTERM, closing the console), or the program that ran the launcher went away (end of stdin, closed stdout) | `"interrupted"` |

When several apply, the strongest decides, in this order: 4, 3, 130, 1, 5, 0. A run that a signal interrupted exits with 130 even if it also found errors; a run that a command stopped exits with 1 if it found errors (a scenario step that failed, a participant that failed or did not end, a warning with `--strict`). A script that waited for 130 after its own `stop` command must now accept 0 (and 5 for a scenario).

### How a run ended (`end`, `ok`, `message`)

The result says three things about the end, in the JSON of `run --json` and in the `result` event:

| Key | Value |
|---|---|
| `end` | Why the run ended (the table below). `null` if nothing was started |
| `ok` | `true` exactly when the exit code is 0 |
| `message` | One English line for a person that says what the result is. It names what stopped the run by its kind (`by a stop request`, `by a signal`, `because its input closed`, `because its output closed`; the exact name is `interrupted_by`). For a failure, it ends with the whole message of the first error, which can be long |

| `end` | Why the run ended | `result` |
|---|---|---|
| `"completed"` | It reached an end that was set: `stop_at_ms`, a participant ended the simulation, or the scenario was over | `"passed"`, or `"failed"` if errors were found |
| `"stopped"` | A command stopped it (`{"command":"stop"}`, `Stop` of the Control API) | `"stopped"`; `"incomplete"` if its scenario did not get to its end; `"failed"` if errors were found |
| `"interrupted"` | A signal or a console event, the end of stdin, or a closed stdout | `"interrupted"` |
| `"failed"` | It could not start or go on: a participant or a bridge failed, `--timeout-ms` passed, the participants did not all join | `"failed"` |
| `"setup_failed"` | The registry or the controller could not be set up | `"setup_failed"` |
| `null` | Nothing was started | `"check_failed"`, `"unreadable"`, `"usage"` |

- What asked first decides between `"stopped"` and `"interrupted"`: a Ctrl+C after a `stop` command is still the stop of that command. `interrupted_by` names what asked first.
- A scenario that a stop or an interrupt cut short is `incomplete`, and the finding `scenario-incomplete` is then a note (it says where the scenario got to). When the run ended for another reason (a participant failed, `--timeout-ms`), the finding is an error, and the exit code is 1.
- `message` is for showing, not for deciding: decide with `ok`, `result`, `end` and `scenario.status`.

For a missing file, the exit code depends on the kind of file.

| Missing file | Exit code | Reason |
|---|---|---|
| The configuration file (`run nofile.yaml`) | 3 | The configuration file cannot be read, so the check cannot start. |
| The `--scenario` file | 1 | It counts as a check error (`code` is `file`). |
| A path given to `test --scenarios` | 4 | It is a usage error. |

## JSON (`run --json`)

One JSON object. The rules (key order, `null`, indentation, line breaks, how versions go up and how readers handle them) are the same as for the check JSON.

| Key | Value |
|---|---|
| `vehicle_sil_run` | Format version (currently 2) |
| `vehicle_sil` | vehicle-sil version of `vehicle_sil_launch` |
| `file` | The configuration file (as given in the arguments) |
| `result` | `"passed"`, `"stopped"`, `"incomplete"`, `"failed"`, `"interrupted"`, `"check_failed"` (check errors; nothing was started), `"unreadable"`, `"setup_failed"`, `"usage"` (see [Exit codes](#exit-codes)) |
| `end`, `ok`, `message` | Why the run ended, whether the result is a success, and one sentence for a person (see [How a run ended](#how-a-run-ended-end-ok-message)) |
| `strict` | Whether `--strict` was given |
| `errors`, `warnings`, `notes` | Counts for the check and the run together |
| `simulation` | How the simulation ended (see the table below). `null` if nothing was started |
| `interrupted_by` | What asked first to stop the run: a signal or a key (`"SIGINT"`, `"Ctrl+C"` and so on), or `"stop command"`, `"control server stop"`, `"end of input"`, `"output closed"`. Also set when `end` is `"stopped"`. `null` if nothing asked |
| `registry_uri` | Where the run's registry listened, as the participants got it: with port 0 in the file, the port it took (`registry_uri` of `check` is as written). `null` if no registry was started |
| `timing` | How time progressed (see the table below). `null` if nothing was started |
| `participants[]` | Result per participant (see the table below), in configuration file order, including disabled participants; then the bridges of `--connect` |
| `outside[]` | The networks that `--connect` connected to a real bus (see the table below), in the order of the networks in the file. Empty without `--connect` |
| `traffic` | The traffic on each network (see [Network traffic](#network-traffic-traffic) and the table below). `null` if the simulation did not start |
| `findings[]` | Run findings (same form as `findings[]` of the check) |
| `check` | The check result (the same object as `check --json`) |
| `scenario` | Only with `--scenario`: the scenario result (see "Output" in [scenario.md](scenario.md#output)) |

The selected variant (`variant`) and the parameters (`params`, `scenario.params`) are inside `check`. `check` also contains a `scenario` (`file`, `name`, `steps`). Both `scenario` objects also have `inputs[]` (the dithers declared in `inputs`) and `lines[]` (the list of what the text places on the inputs): empty in a scenario without a `dither`, `null` if the scenario could not be read.

Values of `simulation`:

| Value | Meaning |
|---|---|
| `"reached_stop_at"` | Ran until `stop_at_ms` |
| `"ended"` | A participant stopped it (with `--scenario`, also when the scenario ended and the launcher stopped it) |
| `"aborted"` | Aborted (also by `--timeout-ms`) |
| `"interrupted"` | Stopped by a stop request (Ctrl+C, the `stop` command and similar; `end` says which kind) |
| `"not_started"` | Did not start |

In a run with `--scenario`, look at `scenario.status` to tell which meaning `"ended"` has. If it is `"passed"` or `"failed"`, the run stopped because the scenario ended. If scenario steps were skipped, `scenario.skipped_steps` has their number (`status` stays `"passed"`). A skipped step has `at_ms` null. In a run with `--timeline`, `scenario.timeline` holds the name of the timeline file, as given in the arguments, and `null` without it (see [Recording a scenario run on a timeline](#recording-a-scenario-run-on-a-timeline---timeline)).

Keys of `participants[]`:

| Key | Value |
|---|---|
| `name` | Participant name |
| `required` | `required` in the configuration file |
| `enabled` | `false` for a disabled participant (`ended` is `"not_started"`, `log` is `null`) |
| `manual` | `true` for a participant that you start yourself (`ended` is `"not_started"`, `exit_code` is `null`) |
| `for_network` | For the bridge that `run` started for `--connect`, the network it connects; `null` for a participant of the file |
| `ended` | `"exited"`, `"stopped"`, `"killed"`, `"not_started"` |
| `exit_code` | Exit code (`null` if the participant ended by a signal, was killed or was not started) |
| `signal` | `"SIGSEGV"` and so on (`null` if none) |
| `left_processes` | Whether processes it started were left behind |
| `log` | The `--log-dir` file (`null` if none) |
| `control_link` | State of the connection to ECU variables at the start (see the table below) |

The simulation waits for the participants whose `required` and `enabled` are both `true`. For `manual`, see [Starting a participant yourself](simulation-config.md#starting-a-participant-yourself-manual---manual).

Keys of `outside[]` (see [Connecting a bus outside](#connecting-a-bus-outside---connect)):

| Key | Value |
|---|---|
| `network` | The network |
| `target` | Where it is connected, as `--connect` gave it |
| `participant` | The bridge's name in `participants[]` (`bridge.<network>`) |
| `to_outside` | The frames that the bridge sent to the bus (`silkit_to_ext` in both), or `null` if no statistics line of the bridge could be read |
| `from_outside` | The frames that came from a device on the bus (`ext_from_devices` in both), or `null` as `to_outside` (see [Counting the frames to and from the bus](#counting-the-frames-to-and-from-the-bus)) |
| `counts` | Every `key=N` of the last statistics line of the bridge, with the bridge's own keys (an object; `{}` if no line could be read) |

Keys of `participants[].control_link` (see [Connection to ECU variables](#connection-to-ecu-variables-control_link)):

| Key | Value |
|---|---|
| `state` | `"connected"`, `"no_hello"`, `"version_mismatch"`, `"not_offered"`, `"none"` (`"none"` for a participant that was not started) |
| `variables` | For `connected`, the number of variables the ECU exposes (otherwise `null`) |
| `version` | For `version_mismatch`, the ECU's format version (otherwise `null`) |
| `late_messages` | Number of messages received later than the guarantee allows (normally 0) |

`variables` includes outputs such as `peer.` that are added when the ECU runs, so it can be larger than the count in `--describe`.

Keys of `timing` (for the definitions of the counts, see [Running at a set speed](#running-at-a-set-speed-speed)):

| Key | Value |
|---|---|
| `speed` | Speed at the end (a number or `"max"`) |
| `time_step_ms` | The controller's time step (the value from the configuration file, or the default) |
| `ecu_step_ms` | The smallest ECU step size (milliseconds) |
| `step_delay_ms` | The largest amount by which ECU steps run later than the real time that corresponds to their virtual time, because the time step is coarser than the ECU steps (milliseconds) |
| `late_tolerance_ms` | The lateness tolerance (the value from the configuration file, or the default) |
| `sim_ms` | Virtual time reached (milliseconds; the time of the last time step that started) |
| `wall_ms` | Real time taken (milliseconds, including time spent paused) |
| `actual_speed` | Overall actual speed (`sim_ms` ÷ (`wall_ms` − `paused_ms`), 3 decimal places) |
| `paced_steps` | Number of paced time steps (time steps compared with a deadline) |
| `late_steps` | Of those, the number of time steps whose lateness exceeded `late_tolerance_ms` (the number of slips) |
| `max_late_ms` | Largest lateness of the paced time steps (milliseconds, rounded up; includes lateness within the tolerance) |
| `slipped_ms` | Total slip (the sum of the lateness of the late time steps; milliseconds, rounded up) |
| `late_ms` | Lateness of a time step that was past its deadline and had not started when the run ended (milliseconds, rounded up; 0 if none) |
| `paused_ms` | Real time spent paused (milliseconds) |

The times in `timing` run from the start of the controller's first time step (virtual time 0) to the start of the last time step. Starting the participants, stopping them and the wait until a failure is noticed are not included. When the run ends with the next time step past its deadline, and at maximum speed, the times run up to the end of the run.

| Key | Notes |
|---|---|
| `ecu_step_ms` | `null` when a participant's step size is unknown (`describe: false`), or when no ECU has a step. |
| `step_delay_ms` | Meaningful when the run is paced. |
| `step_delay_ms` | 0 means that every ECU step starts at a time step boundary. |
| `step_delay_ms` | `null` when a participant's step size is unknown. |
| `sim_ms` | In a run that ended by a participant failure or an abort, it varies by one time step from run to run. |
| `actual_speed` | `null` if the run lasted less than 1 ms. |
| `paced_steps` | Includes a last time step that was past its deadline and never started. |
| `slipped_ms` | The simulation as a whole fell this far behind the set speed. |
| `late_ms` | If not 0, the simulation ended while standing still (the lateness is included in `late_steps`, `max_late_ms` and `slipped_ms`). |

`sim_ms` varies because the moment the failure or abort is noticed can fall before or after the start of the controller's next time step.

Keys of `traffic`:

| Key | Value |
|---|---|
| `before_ms` | The virtual time at which the run ended (`timing.sim_ms`). The counts are from steps that started before it |
| `unplaced[]` | The participants of the run that the launcher knows on no network (see "[Network traffic](#network-traffic-traffic)"), in configuration file order: `participant` (its name), `reason` (`"no_control_link"` or `"no_counters"`; the table of reasons is in the same section). `[]` if there are none. Each of them is also in `unplaced` of every network |
| `networks[]` | Every network of the configuration file, in its order: `name` (the simulation's name), `kind`, `counted` (the number of counted participants), `senders` (how many of them sent one frame or more), `receivers` (how many of them received one or more), `delivered` and `unplaced` (see below) |
| `networks[].delivered` | Whether sent frames arrived. `true`: a counted participant sent, and another counted participant received. `false`: a counted participant sent, no counted participant received, every participant known to be on the network was counted, and the network's `unplaced` is empty. `null`: unknown (no counted participant, nobody sent, the only sender is the only receiver (it received the frames of a participant that the launcher does not count), or no counted participant received while a participant that was not counted is on the network or the network's `unplaced` is not empty) |
| `networks[].unplaced[]` | The participants of the run that may be on this network without the launcher knowing (`describe: false`, and no counters for this network), in configuration file order: `participant`, `reason` (as in `unplaced` above). `[]` if there are none. A participant that its counters place on another network is listed here and not in the `unplaced` of `traffic` |
| `networks[].participants[]` | In configuration file order (`[]` for a network on which nobody is known to be): `name`, `counted`, `not_counted` (`null` when counted; otherwise one of the strings of the table in "[Network traffic](#network-traffic-traffic)"), `virtual_time` (whether it runs in virtual time; `null` if its connection to ECU variables did not connect), `at_ms` (the time of the step whose counts were read), `frames_sent`, `frames_received` (these 3 are `null` when not counted) |

For a program that decides whether frames arrived, read `delivered` of each network: `false` means "sent and not delivered", and `null` cannot be decided, so read it as neither. With a `describe: false` participant in the run (a logger, a bridge), `delivered` is never `false` for a network whose `unplaced` lists it, because that participant may have received the frames; the network's `unplaced` says who and why. For each participant, read `not_counted` and `frames_received`. `delivered` follows from the counts alone. A participant does not count its own frames as received, so a received count of 1 or more at a participant other than the sender means that someone's frames arrived. The counts do not tell whose frames they were (they may be those of a participant that was not counted, or of a bridge).

<!-- vehicle_sil_launch run simulation.yaml --json -->
```json
{
  "vehicle_sil_run": 2,
  "vehicle_sil": "<version>",
  "file": "simulation.yaml",
  "result": "passed",
  "end": "completed",
  "ok": true,
  "message": "The run reached its end without errors",
  "strict": false,
  "errors": 0,
  "warnings": 0,
  "notes": 0,
  "simulation": "reached_stop_at",
  "interrupted_by": null,
  "registry_uri": "silkit://127.0.0.1:<port>",
  "timing": {
    "speed": "max",
    "time_step_ms": 10,
    "ecu_step_ms": 5,
    "step_delay_ms": 5,
    "late_tolerance_ms": 10,
    "sim_ms": 1000,
    "wall_ms": <wall_ms>,
    "actual_speed": <actual_speed>,
    "paced_steps": 0,
    "late_steps": 0,
    "max_late_ms": 0,
    "slipped_ms": 0,
    "late_ms": 0,
    "paused_ms": 0
  },
  "participants": [
    {
      "name": "Engine",
      "required": true,
      "enabled": true,
      "manual": false,
      "for_network": null,
      "ended": "exited",
      "exit_code": 0,
      "signal": null,
      "left_processes": false,
      "log": null,
      "control_link": {
        "state": "connected",
        "variables": 22,
        "version": null,
        "late_messages": 0
      }
    },
    {
      "name": "Dashboard",
      "required": true,
      "enabled": true,
      "manual": false,
      "for_network": null,
      "ended": "exited",
      "exit_code": 0,
      "signal": null,
      "left_processes": false,
      "log": null,
      "control_link": {
        "state": "connected",
        "variables": 22,
        "version": null,
        "late_messages": 0
      }
    }
  ],
  "outside": [],
  "traffic": {
    "before_ms": 1000,
    "unplaced": [],
    "networks": [
      {
        "name": "J1939_CAN",
        "kind": "can",
        "counted": 2,
        "senders": 1,
        "receivers": 1,
        "delivered": true,
        "unplaced": [],
        "participants": [
          {
            "name": "Engine",
            "counted": true,
            "not_counted": null,
            "virtual_time": true,
            "at_ms": 990,
            "frames_sent": 11,
            "frames_received": 0
          },
          {
            "name": "Dashboard",
            "counted": true,
            "not_counted": null,
            "virtual_time": true,
            "at_ms": 990,
            "frames_sent": 0,
            "frames_received": 11
          }
        ]
      }
    ]
  },
  "findings": [],
  "check": {
    "vehicle_sil_check": 1,
    "vehicle_sil": "<version>",
    "file": "simulation.yaml",
    "result": "passed",
    "strict": false,
    "errors": 0,
    "warnings": 0,
    "notes": 0,
    "stop_at_ms": 1000,
    "start_timeout_ms": 30000,
    "stop_timeout_ms": 10000,
    "registry_uri": null,
    "participants": [
      {
        "name": "Engine",
        "line": 11,
        "command": [
          "engine/vehicle_sil_engine_ecu"
        ],
        "describe": true,
        "required": true,
        "enabled": true,
        "manual": false,
        "role": "ecu",
        "step_ms": 10,
        "status": "described",
        "reported": {
          "vehicle_sil": "<version>",
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
            }
          ]
        },
        "variables": [
          {
            "name": "ecu.connected",
            "type": "bool",
            "causality": "input",
            "unit": "",
            "min": null,
            "max": null,
            "step": null,
            "description": "on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on",
            "items": [],
            "builtin": true
          },
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
          {
            "name": "engine.speed",
            "type": "integer",
            "causality": "output",
            "unit": "rpm",
            "min": null,
            "max": null,
            "step": null,
            "description": "",
            "items": [],
            "builtin": false
          }
        ],
        "network_map": null,
        "j1939_nodes": null,
        "control": {
          "transport": "silkit",
          "wire_version": 2,
          "wire_revision": 1
        }
      },
      {
        "name": "Dashboard",
        "line": 14,
        "command": [
          "dashboard/vehicle_sil_dashboard_ecu"
        ],
        "describe": true,
        "required": true,
        "enabled": true,
        "manual": false,
        "role": "ecu",
        "step_ms": 5,
        "status": "described",
        "reported": {
          "vehicle_sil": "<version>",
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
            }
          ]
        },
        "variables": [
          {
            "name": "ecu.connected",
            "type": "bool",
            "causality": "input",
            "unit": "",
            "min": null,
            "max": null,
            "step": null,
            "description": "on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on",
            "items": [],
            "builtin": true
          },
          {
            "name": "dashboard.engine_speed",
            "type": "integer",
            "causality": "output",
            "unit": "rpm",
            "min": null,
            "max": null,
            "step": null,
            "description": "the speed shown",
            "items": [],
            "builtin": false
          },
          {
            "name": "dashboard.engine_lost",
            "type": "bool",
            "causality": "output",
            "unit": "",
            "min": null,
            "max": null,
            "step": null,
            "description": "no engine speed for more than 300 ms (it comes every 100 ms)",
            "items": [],
            "builtin": false
          }
        ],
        "network_map": null,
        "j1939_nodes": null,
        "control": {
          "transport": "silkit",
          "wire_version": 2,
          "wire_revision": 1
        }
      }
    ],
    "networks": [
      {
        "name": "J1939_CAN",
        "kind": "can",
        "line": 8,
        "participants": [
          "Engine",
          "Dashboard"
        ],
        "outside": null
      }
    ],
    "time_lock": {
      "locked": false,
      "because": []
    },
    "findings": []
  }
}
```

The CTest test `launch_run.docs` also compares the examples of this section with a real run (byte for byte, except the `vehicle_sil` version and the values shown as `<wall_ms>`, `<actual_speed>` and `<port>`).

## Recording a scenario run on a timeline (`--timeline`)

With `--timeline <file>`, `run --scenario` writes the values that the scenario wrote and saw to one JSON file, in virtual-time order. The file is for tools that draw graphs or compare 2 runs. The launcher supports it if `features` of `vehicle_sil_launch version --json` contains `timeline`.

```sh
vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml --timeline throttle_step.timeline.json
```

- The file is written after the run, and stderr gets the line `[vehicle_sil_launch] timeline: <file>` (a `log` event with `--events`). In the result of `run --json` and `--events`, `scenario.timeline` holds the file name (as given in the arguments), and `null` without `--timeline`. **Without `--timeline`, nothing else in the output of a run changes.**
- The file is opened before anything is started (missing directories are created). If it cannot be opened, the run ends with the error `setup` (exit code 3) and starts nothing. If the file cannot be written after the run (a full disk, for example), the error is `timeline-write`. The scenario result (`scenario.status`) does not change, but the finding is an error, so the run fails even when the scenario passed (exit code 1).
- If points were dropped at the limits, the `timeline:` line adds the number of dropped points, and the run reports the note `timeline-limit` (see "Limits" below). The exit code does not change.
- The file is written whether the scenario passes, fails or ends partway. It is not written when a check error kept the run from starting anything.
- `--timeline` on a `run` without `--scenario` is a usage error. `check` and `test` do not take it.
- The format is in `schema/timeline.schema.json` (JSON Schema). Line ends are LF on every OS.

### Viewing a timeline file

The timeline page draws a timeline file as a chart. After a scenario run started from the browser GUI, open it with the **Timeline** link of the **Scenario** card: the GUI server records the run's timeline and serves the page in the GUI's language (see "Timeline of a scenario run" in [gui.md](gui.md#timeline-of-a-scenario-run)).

For a file you wrote with `--timeline`, the page is `gui/timeline/timeline.html` in the vehicle-sil repository, or `python/vehicle_sil_gui/timeline/timeline.html` in the prebuilt tools archive. Open it in a browser (as a local file is fine; it needs `vehicle-sil-timeline.js` and `page.js` beside it) and drop the timeline file on it, or choose the file with **Open a file** (a button, so the keyboard works too). The page reads the file in the browser and sends nothing anywhere.

- If the scenario failed, the page opens on the failure: the step that failed and when, the launcher's message, each condition that did not hold with the value it saw, and, for a failed write, which writes for that time the ECU applied, which it did not, and for which no result says (not known).
- The chart has a lane for the steps and one for each variable: the inputs (the values sent, and the values seen when a condition reads them), then the other variables. The bands show the values that each condition allows (green: it held; red: it did not; grey: the step was not decided by it). A value sent that the ECU did not apply is red and dashed, one for which that is not known is orange and dotted; neither is drawn as a value the variable took. Drag across the chart to zoom in; double-click shows the whole run.
- Below the chart, tables hold the same steps, input lines and findings.
- The page is in English or Japanese: `?lang=en` or `?lang=ja` in its address, else the browser's language, else English. The names, values and messages of the file and the launcher are shown as they are.
- Served by a web server, the page also takes the file's address in `?src=`, on the same server only (`timeline.html?src=runs/throttle_step.timeline.json`).
- The page reads format 1 with `stage` `"run"`. For another format or stage, it says that it cannot show the file instead of drawing it. It ignores keys it does not know, and names the values it does not know, and values that are not of their variable's type, under their lane instead of drawing them. A file it cannot show leaves the page empty, so that nothing of the file before is taken for it; the name of the file shown is in the header.

### File format

All times are virtual time in ms, and can be fractional. A variable is always referred to by the string `"<participant>.<name>"`.

| Key | Value |
|---|---|
| `vehicle_sil_timeline` | Version of this format (currently 1) |
| `vehicle_sil`, `stage` | The vehicle-sil version; `"run"` (the record of an actual run) |
| `file`, `variant`, `params` | The configuration file (as given in the arguments), the variant (`null` if none), and the scenario parameters with the values in effect (the same form as `scenario.params` of `check --json`) |
| `time_step_ms` | The controller's time step (the same as `timing.time_step_ms` of `run --json`) |
| `scenario` | `file`, `name`, `status` and `over_at_ms` (as in `run --json`), and `recorded_to_ms` (see "End of the record" below) |
| `variables[]` | The variables that the scenario uses, in order of first use: `name`, `participant`, `type`, `causality`, `unit`, `min`, `max`, `items` (enum), `ecu_step_ms` (the step size of that ECU), `written` (the scenario writes it), `watched` (a condition reads it) |
| `steps[]` | The steps: the keys of `scenario.steps[]` of `run --json`, plus `lines`, `watch` and `failed` |
| `series[]` | The value series (see below) |
| `lines[]` | The line of each input: the values and motions that steps placed (see "Input lines" below) |
| `findings[]` | The check and run findings whose `code` starts with `scenario-` (the same form as in `run --json`). The finding `control-link-late-values`, which says that ECU values arrived later than guaranteed, appears with its `severity` and `code` only, the other keys `null` (in a run with this finding, the record can differ from run to run). Other findings, such as the real-time speed finding `behind-real-time`, are left out |
| `limits` | The limits on the number of points, and the dropped points (see "Limits" below) |

A reader ignores keys it does not know. For a value it does not know (`source` and `encoding` of a series, `mode` of `watch`, `op` of a condition, `kind` of a step, `kind` and `base_source` of a line, `type` of `shape`), it leaves that part undrawn and shows the name and `text`. Keys and values can be added without raising the format version. A step's `status` is not one of them: see [Values that carry the result](scenario.md#values-that-carry-the-result).

Every key in this section is in every file. A key that does not apply has the value `null`. The one exception is `expect_skipped` of a step, which, as in `run --json`, is there only when the step has it.

The `lines` of a step is an array of the positions (from 0) in the top-level `lines[]` of the lines that the step placed. It is empty for a step that writes nothing, a step whose time is after `recorded_to_ms`, and a step that sent no write.

The `watch` of a step is the condition that the step checks (`null` for a step without a condition).

| `mode` | Step | What it looks at | `to_ms` |
|---|---|---|---|
| `at` | `expect` at one point in time | The value at `from_ms` | `null` |
| `always` | `expect` with `hold_for_ms` | The value at `from_ms` and every sample up to `to_ms` | The step's time + `hold_for_ms` |
| `once` | `expect` with `within_ms`, `wait_until` | The value at `from_ms` and the samples after it. The time at which the condition held is the step's `at_ms` | The wait limit |
| `during` | `expect` beside `ramp` and `wave` | Only the samples with times from `from_ms` to `to_ms` | The ECU step in which the final value enters the ECU (it can be after the step's `end_ms`; see "Checking values during a ramp or wave" in [scenario.md](scenario.md#checking-values-during-a-ramp-or-wave)) |

- `watch.conditions[]` has one entry per variable: `variable` and `compare[]` (`op` is `eq`, `ne`, `ge`, `le`, `gt`, `lt` or `near`; `value`; `tolerance` for `near`, `null` for the other ops).
- For a step that the scenario never reached (`not_run`), `from_ms` and `to_ms` are `null`.

The `failed` of a failed step holds what failed, as numbers (`null` for other steps; the text for people is in `message`).

```json
"failed": {
  "at_ms": 1340,
  "code": "scenario-failed",
  "conditions": [
    {
      "condition": 0,
      "value": 3050,
      "sample_ms": 1340
    }
  ],
  "writes": [],
  "write": null
}
```

- `conditions[]` lists every condition that did not hold. `condition` is the index (from 0) in `watch.conditions[]`. `value` and `sample_ms` are the value at that moment and the time of the sample it came from. Both are `null` for a condition that had no value yet, and for a condition that got no sample in its `during` interval.
- `writes[]` and `write` describe a failed write (below). In a step that did not fail on a write, `writes[]` is empty and `write` is `null`.

For a step that failed on a write, `writes[]` holds every write that the scenario sent together for that time. In this example, one `set` wrote 2 variables of the participant `Body`, and the ECU's setter rejected the second one:

```json
"failed": {
  "at_ms": 20,
  "code": "scenario-write",
  "conditions": [],
  "writes": [
    {
      "variable": "Body.mode",
      "at_ms": 20,
      "value": 2,
      "point": 0,
      "applied": true
    },
    {
      "variable": "Body.level",
      "at_ms": 20,
      "value": 13,
      "point": 1,
      "applied": false
    }
  ],
  "write": 1
}
```

- `writes[]` lists the writes of the failed request in the order sent: every variable of the `set` (for all its participants), the one write of a `ramp` or `wave`, or the writes that the `dither`s made for that time. It is empty unless a write failed.
- The writes that `dither`s make while no motion runs belong to no step. The ones for the same time and the same participant reach the ECU together, like the variables of one `set`, so `applied` follows the same rules: the inputs before the rejected one are already written.
- `applied` says whether the ECU applied that write:

  | `applied` | Meaning |
  |---|---|
  | `true` | The ECU applied it. The value is in the ECU although the step failed. |
  | `false` | The ECU did not apply it: the ECU or its Agent refused it, it comes after the refused write in the same `set`, or it was never sent. |
  | `null` | Not known: no result came, or the ECU's result does not say which writes took effect. |

  An ECU applies the writes of one `set` one after the other and stops at the write that its setter rejects. So the writes before the rejected one are already applied (`true`), and the rejected one and the ones after it are not (`false`). When the ECU's Agent refuses a write as it receives it (wrong name, type or range), none of that participant's writes in the `set` is applied. The same holds when the `set` writes a parameter after the first step: the ECU refuses that participant's writes as a whole.
- `point` is the index (from 0) in the `points` of that variable's `written` series; that point is this write. It is `null` if nothing was sent. A point whose `applied` is `false` or `null` is at or after the `not_applied_from` of its series.
- `write` is the index (from 0) in `writes[]` of the write that caused the failure: the write that the ECU or its Agent pointed at when it refused (by its position among that participant's writes, not by its name), or the only write for the participant that refused. It is `null` if no single write is known to be the cause (a participant refused several writes together without pointing at one, or no result came), and in a step that did not fail on a write.
- If the step failed before it sent anything (the variable cannot be written, or a `ramp` or `wave` had no start value), `writes[]` holds that one write with `point: null` and `applied: false`. Its `value` is `null` if the value was never computed.

### Value series (`series[]`)

```json
{
  "variable": "Engine.engine.throttle",
  "source": "written",
  "encoding": "all",
  "columns": ["at_ms", "value"],
  "samples": null,
  "first_ms": null,
  "last_ms": null,
  "not_applied_from": null,
  "points": [
    [0, 50],
    [500, 50],
    [510, 47.5]
  ]
}
```

| `source` | Contents | `encoding` |
|---|---|---|
| `written` | Every write that the scenario sent. A point is `[time, value]`, where the time is when the write takes effect. **These are the values sent; they did not necessarily reach the ECU** (see `not_applied_from` below) | `all` |
| `sampled` | The sample values that the scenario saw, only for the variables that conditions read | `changes`: only samples whose value changed become points. `samples` (the number of samples seen), `first_ms` and `last_ms` (the times of the first and the last sample) are kept beside the points. They are `null` in a `written` series |

- `columns` names the columns of a point: `["at_ms", "value"]`. **The `written` series of an input on which the scenario places a `dither` has 3 columns**: `["at_ms", "value", "base"]`. The third column, `base`, is the value before the dither is added and the type rounding is applied: the value of a `set`, or the value of a `ramp` or `wave` at that time (see "Overlaying a dither" in [scenario.md](scenario.md#overlaying-a-dither-dither)). A series has 3 columns when the text of the scenario places a dither on that input (a declaration in `inputs`, or a `dither` step with a waveform that runs in the variant). Then every point has 3 columns, also before the dither is placed and after it is removed, whether or not the run reached that step (the number of columns does not change with the controller's time step). A reader takes the columns by the names in `columns`.

  ```json
  {
    "variable": "Engine.engine.throttle",
    "source": "written",
    "encoding": "all",
    "columns": ["at_ms", "value", "base"],
    "samples": null,
    "first_ms": null,
    "last_ms": null,
    "not_applied_from": null,
    "points": [
      [0, 50, 50],
      [10, 52, 50],
      [20, 50, 50]
    ]
  }
  ```

- Only the variables that the scenario uses are recorded. A variable that the scenario writes has a `written` series, and a variable that a condition reads has a `sampled` series (a variable that is both has 2). The series follow the order of `variables[]`, with `written` first for each variable.
- Each point is on a line of its own. Times never go back. Several points can share a time (the end of a `ramp` and the next `set`); they are in the order sent.
- Values: a bool is `true` or `false`, an enum is the item's value (the names are in `variables[].items`), and a real that is not a number is `null`.
- A sample is the value at the end of an ECU step, with the time of the start of that step (see "Values that conditions see" in [scenario.md](scenario.md#values-that-conditions-see)). Read a `sampled` value as lasting from its point to the next point (the last point lasts until `last_ms`).
- "Changed" in `changes` means that the written text differs (`0` and `-0` differ; consecutive `null`s are the same).
- If `not_applied_from` of a `written` series is not `null`, the points from that index (from 0) on were sent but are not known to have reached the ECU. Do not draw them as if the value changed. These are:
  - The point of a write of the failed step whose `applied` is `false` or `null` (the `point` of that entry in `failed.writes[]`) and the points after it, whether the ECU's Agent refused the write at once or the ECU refused it in its step. A write of the same `set` (or of the same time's `dither` writes) that the ECU did apply (`applied: true`) is not marked.
  - Points whose time is after `scenario.over_at_ms` (no ECU advanced to that time).
- In a failed run and in a run that ended partway, some points before `not_applied_from` can also be writes that the scenario had not confirmed when the run ended (it had not yet seen their results; for a point at exactly `over_at_ms`, whether the ECU ran the step at that time differs from run to run). In a run that passed, every point was applied, with one exception: a `dither` write for the time of the last step may not have its result yet, because the scenario is over as soon as its last step passes.

### Input lines (`lines[]`)

Each variable that the scenario writes has one line. When a step's time comes, the step places what it does on the line: a `set` places a value, and a `ramp` or `wave` places a motion. `lines[]` lists the placed items for which at least one write was sent, in the order placed (time order; at the same time, step order; within one step, the order written).

```json
{
  "variable": "Engine.engine.throttle",
  "kind": "ramp",
  "line": 9,
  "step": 2,
  "from_ms": 500,
  "to_ms": 1500,
  "base": 50,
  "base_source": "line",
  "shape": {
    "type": "ramp",
    "to": 80,
    "over_ms": 1000
  },
  "text": "Engine.engine.throttle to 80 over 1000 ms"
}
```

| Key | Value |
|---|---|
| `variable` | The variable |
| `kind`, `line`, `step` | The kind of step that placed it (`set`, `ramp`, `wave`; for `dither`, see below), the line in the scenario file, and the step's `index` |
| `from_ms`, `to_ms` | The step's time and the end of the motion as written (`from_ms` + the duration). The same value for `set`. If the run ended first, points exist only up to `recorded_to_ms` |
| `base` | For `set`, the value written. For `ramp` and `wave`, the start value or the center of the swing (`from` and `around` in [scenario.md](scenario.md)). Written the same way as the series points (a number that the step wrote stays as written). `null` for shapes that carry their own values (`square` with `low` and `high`, `points` that start at 0 ms, and so on) |
| `base_source` | Where `base` came from (see the table below). `null` if `base` is `null` |
| `shape` | The shape of `ramp` and `wave`: `type` (`ramp` or the waveform name) and the keys and values written in the step (after parameters are applied). `null` for `set` |
| `text` | One line for people (this variable's part of the step's `text`) |

| `base_source` | Where `base` came from |
|---|---|
| `written` | A value written in the step (the value of `set`, `from`, `around`, or the other value of a bool) |
| `line` | The line's value: the value that the scenario had placed for the variable at that time (the value of an earlier `set` or the end value of an earlier motion, rounded for integers) |
| `sample` | The latest sample at that moment, for a variable that the scenario has not written yet. **Which sample it is depends on the controller's time step** (see "End of the record" below). The same holds for `line` bases after a motion that ends by returning to this value |

- `shape` holds only the keys written in the step (for the defaults of omitted keys, see the waveform tables in [scenario.md](scenario.md#waveform-keys)). `from` and `around` are not in `shape`; they become `base`. For `wave`, `shape` also holds `for_ms` (for waveforms that take `for_ms`). `shape` is there to display the line. The values are in the series, so do not compute them again from `shape`.
- Read the meaning of `base` from `shape.type`: the start value for `ramp`, `lag` and `chatter`, the center of the swing for waveforms with a center such as `sine`, and the value to return to at the end for `pulse`.
- **A `dither` line** (`kind` is `dither`) is a dither overlaid on an input. Each `inputs` declaration and each item that a `dither` step placed is one line.

  | Key | Value in a `dither` line |
  |---|---|
  | `line`, `step` | The line and `index` of the `dither` step. For an `inputs` declaration, the line of that input and `null` |
  | `from_ms` | The time of the first write that carried this dither (a dither placed on an input without a base starts at the first `set`, `ramp` or `wave`) |
  | `to_ms` | `null` (the text gives no end). **The `from_ms` of the next `dither` line of the same variable is the end of this dither** |
  | `base`, `base_source` | `null` |
  | `shape` | The waveform of the dither. `null` for `none` (removed) |

  A dither line comes after the lines that steps placed at that time, in the order in which the first writes were sent. The `lines` of a `dither` step are the positions of the dither lines that the step placed. A dither that no write ever carried (on an input that never got a base, or replaced by the next step before any write) does not become a line.
- The `shape` of `points` holds the number of points (`points`) and `interpolate`. For points read from a CSV file, it also holds `file`, `value`, `time` and `time_unit` as written (the column names only; the point values are in the series).
- The file holds only lines whose `from_ms` is at or before `recorded_to_ms` and **for which at least one write was sent** (the file records what happened). If the scenario failed before the first write of a line, the line is left out, and it is not in the step's `lines` either. Examples: a `ramp` after a refused `set` of an earlier step at the same time; when one step moves several variables and the start value of one is not found, that variable and the variables written after it. A refused write was sent, so its line is included (as with the series points).
- In a run where the scenario did not start, `lines` is empty.

### End of the record (`recorded_to_ms`)

`scenario.recorded_to_ms` is the virtual time at the right edge of the record. The series hold only points up to this time, and `lines[]` holds only lines placed up to this time for which a write was sent.

| Scenario | `recorded_to_ms` |
|---|---|
| Failed (`failed`) | The `at_ms` of the failed step (in a run that ended at `stop_at_ms`, the time it stopped) |
| Passed (`passed`) | The latest `at_ms` of the steps |
| Ended partway (`incomplete`) | The time of the last sample seen (0 if none was seen). If a write was refused and then a participant failure became the result of the run, the time of that write (`failed` is `null` for every step, but `not_applied_from` is not `null`) |
| Did not start (`not_run`; if the simulation started but the scenario could not, `incomplete`) | `null` (the series are empty) |

- The controller sees samples only once per time step, so the time at which it knows that the scenario is over (`over_at_ms`) is usually after `recorded_to_ms`. The samples and writes in between are left out of the file. Only in a run that failed on a write that the ECU's Agent refused at once is `over_at_ms` (the time step of the refusal) at or before `recorded_to_ms` (the time of that write).
- **The same run gives the same file.** The file holds only what virtual time decides (no real time, process IDs or lateness counts), so a different speed (`--speed`) gives the same file. This holds for runs that passed and for runs that failed on a sample or a wait. If an ECU itself depends on real time or random numbers, its value series change too.
- For runs that passed and runs that failed on a sample or a wait, a different controller time step (`time_step_ms`) gives the same file, except `over_at_ms` and `time_step_ms`. The following do depend on the time step, in the same places as the scenario result does (see "Timing" in [scenario.md](scenario.md#timing) and "Results" in [scenario.md](scenario.md#results)):
  - The `end_ms` of a wait step that succeeded (`wait_until`, `expect` with `within_ms`), and the times of the steps after it.
  - In a run where a step failed, the `status` of the other steps (the steps after the failed one and the steps in progress), which can be `passed`, `unfinished` or `not_run`, and their `start_ms`, `at_ms` and `end_ms`. The time step decides how far ahead the controller had sent and how many results it had seen before the failure was known. **So a `passed` step can have an `at_ms` after `recorded_to_ms`, and the points of that step's writes are not in the series** (the series and `recorded_to_ms` do not depend on the time step).
  - Everything in a run that ended with a write not applied. When a write that a `dither` makes while no motion runs (a write that belongs to no step) is refused, the step that fails (the first step not yet decided at that moment) also depends on the time step. If that write would be sent after the time step that decides the last step, with some time steps the scenario passes without sending it (see "When a step fails" in [scenario.md](scenario.md#when-a-step-fails)).
  - The start value of a `ramp` or `wave` without `from` or `around` on a variable that the scenario has not written yet (the `base` of a line whose `base_source` is `sample`), and the values of that motion. When that motion ends by returning to its start value (`pulse`, and waveforms that return to the center, such as `sine` without `around`), the `base` of the following motions without `from` or `around` (`base_source` `line`) comes from the same sample and changes the same way. The controller sends a write for time T in its own time step before T, and the start value is the latest sample seen by then. A coarse time step therefore gives an older sample, and before any sample has been seen (T is at most the length of the controller's first time step: with a 30 ms time step, times up to 30 ms), the step fails with `scenario-write` (no start value). For a result that does not depend on the time step, write `from` or `around`, or `set` the variable first.
- An `incomplete` run and a run that ended at `stop_at_ms` do not always give the same file, because real time can decide how far the run saw.

### Limits

When a series has too many points, **the older points are dropped and the newer ones kept**. The end, where a failure is, is always kept in full.

- The limits are 100,000 points per series and 1,000,000 points for all series together (limits on the points kept). They count the points of `sampled` and the `ramp` and `wave` points of `written`. `set` points are not counted and never dropped.
- When the total limit is exceeded, older points are dropped from the series with the most points first, until those series have the same number of points.
- `limits` holds the limits (`points_per_series`, `points_total`) and the series that dropped points (`dropped[]`). If nothing was dropped, `dropped` is empty. `samples` and `first_ms` of a `sampled` series include the dropped points.

  | Key of `dropped[]` | Value |
  |---|---|
  | `variable`, `source` | Which series |
  | `dropped` | The number of dropped points |
  | `dropped_from_ms` | The time of the oldest dropped point |
  | `dropped_before_ms` | The time of the oldest point kept among those that can be dropped (all points of `sampled`; the `ramp` and `wave` points of `written`) |

- **Values are missing from `dropped_from_ms` up to `dropped_before_ms`.** A tool that draws the file must not join points across this interval. In a `written` series, `set` points remain in and before this interval (they are correct as the values sent at that time), but the `ramp` and `wave` values between such a point and `dropped_before_ms` are missing.
- In a run that dropped points, the stderr line is `[vehicle_sil_launch] timeline: <file> (without <N> older points, dropped at its limits: limits.dropped in the file)`, and the result has the note `timeline-limit` (with the number of dropped points). You can tell that points are missing without opening the file.

## Running several scenarios (`vehicle_sil_launch test`)

`vehicle_sil_launch test <file> --scenarios <directory or files>...` runs the listed scenarios one at a time for each variant of the configuration file, and puts all the results in one table. It is meant for CI: one command, and its exit code tells you whether every scenario passes on every variant.

```sh
vehicle_sil_launch test simulation.yaml --scenarios scenarios/                      # every variant × the scenarios in the directory
vehicle_sil_launch test simulation.yaml --variant medium --scenarios a.yaml b.yaml  # one variant × the listed files
```

### Which runs, in which order

The runs are every variant × every scenario.

| Item | Rule |
|---|---|
| Order | Variants in configuration file order; within each variant, scenarios in argument order. |
| A configuration file without variants | Only the scenarios are listed. |
| `--variant <name>` | Runs only that variant (repeatable; the order stays the configuration file order). |
| One run | The same as `run <file> --variant <variant> --scenario <scenario> --speed max`. |
| Speed | Maximum, whatever `speed` the configuration file sets. |
| Between runs | The registry, the controller and the participant processes are created anew (no state carries over from the previous run). |
| Concurrent runs | None. |

### Selecting scenarios (`--scenarios`)

Give `--scenarios` directories or files. You can list any number of them and repeat `--scenarios`. `--scenarios` reads every argument up to the next one that starts with `--` as a scenario, so put the configuration file before `--scenarios`, or after `--` (`test --scenarios scenarios/ -- simulation.yaml`).

| Given | Scenarios run | Order |
|---|---|---|
| A directory | All files directly in it whose names end in `.yaml`, `.yml` or `.json` | File names compared as UTF-8 byte strings |
| A file | That file, whatever its extension | The order given |

- In a directory, the extension is not case-sensitive, and subdirectories are not entered.
- The directory order is the same on every OS (upper case comes before lower case).
- A scenario from a directory is shown in the table as `<directory as given>/<name>`.
- Do not put YAML files that are not scenarios in the directory (if you do, that run fails the check).
- File paths are relative to the current directory, like the configuration file.
- A path that does not exist, or a directory with no scenario file, is a usage error (nothing runs).

### Skipped runs, failed runs and interruption

| Case | What `test` does |
|---|---|
| A run whose variant is not in the scenario's `only` | Does not run it and marks it `skipped`. |
| Every run is `skipped` | After the table, writes one line with the warning `test-nothing-run` (`nothing was run: ...`). |
| A run failed | Still carries out the remaining runs. |
| `--fail-fast` is given and a run failed | Runs nothing after the first failure (the rest are `not run`). |
| Ctrl+C (first) | Stops the current run in the same order as `run`, skips the remaining runs and writes the table up to that point (exit code 130). |
| Ctrl+C (second) | As with `run`: ends the participants without waiting for them to end. |

A run whose variant is not in `only` is the error `scenario-variant` with `run`, but `test` does not count it as a failure. If that run's configuration or scenario has other errors, it is a failure, not `skipped`.

Every run is `skipped`, for example, when the variants selected with `--variant` are in no scenario's `only`. Then nothing has been checked. By default this passes (exit code 0); with `--strict`, it fails (exit code 1).

### Accepted options

| Option | In `test` |
|---|---|
| `--json`, `--strict`, `--describe-timeout-ms`, `--disable`, `--enable` | Accepted (applied the same way to every run). |
| `--timeout-ms` | Accepted (the limit for each run). |
| `--param <name>=<value>` | Applied only to scenarios that declare or use that parameter. |
| `--speed`, `--log-dir`, `--events`, `--stdin-commands`, `--control`, `--manual` and other options only for `run` | Usage error. |
| `--scenario` | Usage error. |

If no scenario has the name given to `--param`, it is a usage error (nothing runs). This keeps a typo from being silently ignored.

### How the configuration file is used

`test` reads the configuration file before the first run. If it cannot be read, nothing runs and the exit code is 3. If the configuration file has check errors, every run fails (each run is checked on its own).

`test` starts every participant itself. If the configuration of a variant to run has an enabled participant with `manual: true` (one that a person starts), every run would wait for a person. So `test` reports a usage error before the first run (exit code 4; the message names the participant).

| Goal | How |
|---|---|
| Run without that participant | Disable it with `--disable <name>`. |
| Run together with that participant | Run the scenarios one at a time with `run --scenario`. |

If you disable the participant with `--disable`, a scenario with steps that use its variables fails the check of that run.

### Output of `test`

Stderr gets the same lines as with `run` (participant output, launcher lines). Before and after each run, these lines appear:

```text
[vehicle_sil_launch] test 2 of 4: variant small, scenario scn/turbo.yaml
[vehicle_sil_launch] test 2 of 4: variant small, scenario scn/turbo.yaml: skipped (the scenario is only for medium (its only))
```

Stdout gets the following, in this order (no times of day, durations or process IDs):

| Order | Content |
|---|---|
| 1 | The table, one line per run |
| 2 | If a run failed, the command to rerun one run alone |
| 3 | Findings for the whole `test` (if any) |
| 4 | The result line |

```text
variant  scenario        result
small    scn/idle.yaml   passed
small    scn/turbo.yaml  skipped  the scenario is only for medium (its only)
medium   scn/idle.yaml   FAILED   line 4: expect Engine.rpm ge 650: not true at 100 ms: Engine.rpm ge 650, but it was 640 (its sample of 100 ms)
medium   scn/turbo.yaml  passed   1 of 3 steps was skipped, 1 warning

to see a failed run in full, run it alone: vehicle_sil_launch run sim.yaml --speed max --variant medium --scenario scn/idle.yaml

FAILED: 4 runs, 2 passed, 1 failed, 1 skipped
```

| `result` in the table | Meaning | Right column |
|---|---|---|
| `passed` | `run` passed for that run | The number of skipped steps, of `expect`s not checked and of warnings (only those present) |
| `FAILED` | The check or the run failed (exit code 1 or 3 of `run`) | The first error (with its line, for a scenario error) and the number of the rest |
| `skipped` | The scenario's `only` does not include this variant | The variants in `only` |
| `interrupted` | The run was interrupted midway | The signal or key that interrupted it |
| `not run` | Not run (interruption, `--fail-fast`, or the launcher's SIL Kit connection did not close after an earlier run) | The reason |

- The "`expect`s not checked" of `passed` are `expect`s beside `ramp` and `wave` (`1 expect was not checked`).
- To see what the warnings of a `passed` run are, run it alone or read `runs[].run` in `--json`.
- For a `FAILED` run that has only warnings under `--strict`, the right column shows the first warning.
- A configuration file without variants gives no `variant` column.

The command to rerun one run alone (`to see a failed run in full, ...`) is for the first failed run. It lists the options given to `test` that apply to that run, so it reproduces the same run. The options listed are `--param` (only names that are in that run's scenario), `--disable`, `--enable`, `--strict`, `--timeout-ms` and `--describe-timeout-ms`. The command has `--speed max`, as `test` runs; it has none for a run that went at speed 1 because a `role: bridge` participant connects it outside (see [Time of a run that is connected outside](#time-of-a-run-that-is-connected-outside---time-policy)), and `--time-policy free` if `test` had it. Paths and values that contain white space or similar characters are enclosed in `'...'` so that you can paste them into a POSIX shell (sh, bash). In the Windows Command Prompt, `'` does not quote, so change them to `"..."`.

The last line is `passed`, `FAILED` or `interrupted`, followed by the counts of runs. The counts of `interrupted` and `not run` appear only when there are such runs.

| Case | End of the last line |
|---|---|
| The remaining runs were not run because a connection did not close | `(the launcher's connection to the simulation did not close)` |
| `--strict` was given | `(--strict)` |

### Findings for the whole `test`

Findings for the whole `test` are written after the table, one line each, as `<severity>: <configuration file>: <text> [<code>]` (in `--json`, in `findings[]`).

| `code` | Severity | Meaning |
|---|---|---|
| `test-nothing-run` | Warning | No run was carried out (every run is `skipped`), so nothing was checked (a failure with `--strict`) |
| `test-runs-left` | Error | Some runs were not carried out although `test` was not interrupted |
| `test-speed-set` | Note | Some runs went at speed 1, not as fast as possible, because a `role: bridge` participant connects them outside the simulation (once for the whole `test`, with the note `outside-speed-set` of the first such run) |

`test-runs-left` appears when the launcher's own SIL Kit connection did not close after a run (that run has the warning `connection-not-closed`; see [Stopping a run](#stopping-a-run)). `test` then ends without starting another simulation in the same process. Select the `not run` runs with `--variant` and `--scenarios` and run them again.

### Exit codes of `test`

| Exit code | Meaning |
|---|---|
| 0 | No run failed and none was left out |
| 1 | 1 or more runs failed |
| 1 | Some runs were not carried out although `test` was not interrupted (`test-runs-left`) |
| 1 | With `--strict`, no run was carried out (`test-nothing-run`) |
| 3 | The configuration file cannot be read (nothing was run) |
| 3 | The result cannot be written, or an unexpected failure of `vehicle_sil_launch` itself |
| 4 | Usage error (nothing was run) |
| 130 | Interrupted (130 even if a run failed before) |

`test` takes no stop command, so it has no exit code 5: a run of `test` ends before its scenario's end only by a signal (130) or by `--timeout-ms` (1).

`skipped` does not count as a failure. Even if every run is `skipped`, the exit code is 0 without `--strict`. The error for a participant with `manual: true` in the configuration file is also 4.

### JSON (`test --json`)

One JSON object. The rules (key order, `null`, indentation, line breaks, how versions go up and how readers handle them) are the same as for the check JSON.

| Key | Value |
|---|---|
| `vehicle_sil_test` | Format version (currently 1) |
| `vehicle_sil` | vehicle-sil version of `vehicle_sil_launch` |
| `file` | The configuration file (as given in the arguments) |
| `result` | `"passed"`, `"failed"` (every case of exit code 1), `"interrupted"`, `"unreadable"`, `"usage"` |
| `strict` | Whether `--strict` was given |
| `variants` | The variants to run (in configuration file order). Empty for a configuration file without variants |
| `scenarios` | The scenarios to run (names as shown in the table, in run order) |
| `passed`, `failed`, `skipped`, `interrupted`, `not_run` | Number of runs with each result |
| `runs[]` | Result per run (see the table below), in run order |
| `findings[]` | Why nothing was run (when `result` is `"usage"` or `"unreadable"`), and the findings for the whole `test` (`test-nothing-run`, `test-runs-left`, `test-speed-set`). Empty if there are none |

`findings[]` has the same form as `findings[]` of the check.

Keys of `runs[]`:

| Key | Value |
|---|---|
| `variant` | The variant (`null` for a configuration file without variants) |
| `scenario` | The scenario |
| `result` | `"passed"`, `"failed"`, `"skipped"`, `"interrupted"`, `"not_run"` |
| `reason` | The right column of the table (`null` if empty) |
| `run` | The same object as `run --json` for that run (`null` for `skipped` and `not_run`) |

`formats` of `version --json` has the version of this JSON as `test`; `features` has no entry for `test`.

## See also

- How to write the configuration file, and the list of arguments → [simulation-config.md](simulation-config.md)
- The check and its findings → [simulation-check.md](simulation-check.md)
- The events of `run --events`, and stdin commands → [simulation-events.md](simulation-events.md)
- How to write scenarios → [scenario.md](scenario.md)
- Reading and writing the variables of a running simulation from a program → [control-api.md](control-api.md)
- How to run on several computers → [howto/multi-pc.md](howto/multi-pc.md)
