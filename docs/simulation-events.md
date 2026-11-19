# Programmatic use (`--events`, `--stdin-commands`, `version`)

[日本語](ja/simulation-events.md)

This reference is for programs, such as the GUI or a script, that start `vehicle_sil_launch`, read its progress and result, and stop it. For how a run proceeds, how it stops and its result, see [simulation-run.md](simulation-run.md).

| Use | What it does | Section |
|---|---|---|
| `run --events` | Writes the progress of the run to stdout as events, and the result last. | [Events](#events-run---events) |
| `run --stdin-commands` | Reads commands from stdin. | [Stdin commands](#stdin-commands-run---stdin-commands) |
| `version --json` | Writes the format versions this `vehicle_sil_launch` writes, and its list of features. | [Version and features](#version-and-features-vehicle_sil_launch-version) |

```sh
vehicle_sil_launch version --json
vehicle_sil_launch check --json simulation.yaml
vehicle_sil_launch run --events --stdin-commands simulation.yaml
```

## Events (`run --events`)

`run --events` writes one JSON object per line to stdout, in the order things happen (NDJSON). The encoding is UTF-8, the line ending is `\n`, and no line contains a line break. Each line is flushed as it is written. Only `run` writes events. Every line starts with these 3 keys:

| Key | Value |
|---|---|
| `event` | The event type |
| `seq` | An integer that starts at 0 and increases by 1 (the same order as the lines; no gaps) |
| `t_ms` | Real time since `vehicle_sil_launch` started, at the moment of the event (milliseconds, integer) |

`seq` lets the reader confirm that no line was lost. `t_ms` never decreases from one line to the next.

### Event types

| `event` | When | Other keys |
|---|---|---|
| `hello` | First line | `vehicle_sil_events`, `vehicle_sil`, `pid`, `file`, `arguments` |
| `phase` | The phase changes | `phase` |
| `check` | The check has finished | `check` |
| `control_server` | The Control Server of `--control` starts listening (before `registry`) | `address`, `tls`, `credentials` |
| `registry` | The registry starts listening | `uri` |
| `participant` | The state of a participant changes | `name`, `state` |
| `control_link` | Right after the simulation starts (before `phase` `running`), once per participant | `name`, `state`, `variables`, `version` |
| `log` | A participant or `vehicle_sil_launch` writes a line | `source`, `line` |
| `log_dropped` | In place of `log` events that could not be written because the reader is slow | `count`, `sources` |
| `outside` | With `--connect`, the counts of a bridge change (at most once a second for each network) | `network`, `to_outside`, `from_outside` |
| `finding` | A run finding is decided | The same keys as one entry of `findings[]` in the result |
| `scenario_step` | A step of the `--scenario` scenario starts or is decided | The same keys as one entry of `scenario.steps[]` in the result |
| `time` | Virtual time advances, the simulation pauses at another time, or the run is late | `sim_ms`, `actual_speed`, `late_steps`, `max_late_ms`, `slipped_ms`, `late_ms` |
| `state` | Where the run stands, or what it accepts, has changed | `rev`, `phase`, `paused`, `paused_at_ms`, `holding`, `speed`, `pacing`, `stop`, `scenario`, `allowed`, `outside` |
| `command` | A command arrives | `command`, `source`, `accepted`, `message`, `reason`, `ms`, `at_ms`, `id` |
| `heartbeat` | Nothing was written for 1 second | None (only the 3 common keys) |
| `result` | Last line | `result` |

### Keys by event

`hello`:

| Key | Value |
|---|---|
| `vehicle_sil_events` | Version of the event format (currently 2) |
| `vehicle_sil` | vehicle-sil version |
| `pid` | Process ID of `vehicle_sil_launch` |
| `file` | The configuration file (as given in the arguments; `null` if unknown) |
| `arguments` | Array of the arguments received |

Values of `phase` in the `phase` event:

| Value | Phase |
|---|---|
| `checking` | The check |
| `starting` | Starting the registry, the controller and the participants |
| `waiting` | Waiting for all required participants to join |
| `running` | The simulation is running |
| `stopping` | Ending the participants |
| `ended` | Everything has ended |

The phases advance in this order and never go back. A phase can be skipped: if the check fails, `ended` follows `checking`; a run that cannot be set up goes from `starting` to `ended` (from `checking`, if it was stopped before it started). See [How a run ends](#how-a-run-ends).

`check`, `registry`, `result`, `control_server`:

| Event | Key | Value |
|---|---|---|
| `check` | `check` | The same object as `check --json` (see [JSON](simulation-check.md#json---json)) |
| `registry` | `uri` | The URI that participants connect to |
| `result` | `result` | The same object as `run --json` (see [JSON](simulation-run.md#json-run---json)) |
| `control_server` | `address` | The address that clients connect to (with port 0, the port that was picked) |
| `control_server` | `tls` | Whether TLS is used |
| `control_server` | `credentials` | Whether calls need a token |

`control_link`:

| Key | Value |
|---|---|
| `name` | Participant name |
| `state` | One of the same 5 values as `participants[].control_link.state` in the result |
| `variables` | Present only when `state` is `connected` (same meaning as the key of the same name in the result). |
| `version` | Present only when `state` is `version_mismatch` (same meaning as the key of the same name in the result). |

`control_link` is written for the participants that the launcher started and for the participants that you start yourself (manual participants).

`log` and `log_dropped`:

| Event | Key | Value |
|---|---|---|
| `log` | `source` | A participant name or `vehicle_sil_launch` |
| `log` | `line` | The line (see [`log` lines](#log-lines)) |
| `log_dropped` | `count` | Number of `log` events not written |
| `log_dropped` | `sources` | Object with the count per `source` (example: `{"Engine": 120, "vehicle_sil_launch": 1}`) |

`log_dropped` appears when the reader is slow (see [Slow readers and readers that stop reading](#slow-readers-and-readers-that-stop-reading)).

The keys of `finding` are `severity`, `code`, `file`, `line`, `participant`, `network`, `message` and `detail`.

For the keys of `scenario_step`, see "Output" in [scenario.md](scenario.md#output). When a step starts, its `status` is `running`. A skipped step (`status` is `skipped`, `at_ms` is null) is written once only.

`time`:

| Key | Value |
|---|---|
| `sim_ms` | Virtual time as the controller sees it (milliseconds; never decreases) |
| `actual_speed` | Virtual time advanced ÷ real time, over roughly the last second (3 decimal places) |
| `late_steps` | Number of late time steps so far |
| `max_late_ms` | Largest lateness so far |
| `slipped_ms` | Total slip so far |
| `late_ms` | Current lateness of a time step that is past its deadline and has not started yet (0 if there is none) |

- `actual_speed` is `null` when the simulation has run only for a short time, and in a `time` that is written while it is paused.
- Whether the simulation is paused, where, and the speed set, are not in `time`: read `paused`, `paused_at_ms` and `speed` of the latest `state`.
- While the simulation is paused, the last `time` can be from before the pause: its `sim_ms` can be earlier than `paused_at_ms` (until the launcher's next look, 20 ms), and its `actual_speed` and `late_ms` can be the values from before the pause (when the simulation pauses at the time of the last `time`, no new `time` is written).
- `late_steps`, `max_late_ms` and `slipped_ms` have the same meaning as in the result's `timing`. They are the values the result would have if the run ended now.
- While the simulation is stalled (virtual time does not advance and `late_ms` grows beyond the tolerance), `time` is still written, at most once every 100 ms.

`command`:

| Key | Value |
|---|---|
| `command` | Name of the command (`null` if it cannot be read) |
| `source` | `"stdin"` (arrived as a line), `"eof"` (end of stdin), `"control_server"` (a client call) |
| `accepted` | Whether the command was accepted |
| `message` | Why the command was not accepted (`null` if it was accepted) |
| `reason` | The code of why the command was not accepted (see [Reason codes](#reason-codes); `null` if it was accepted) |
| `ms` | Only for an accepted `step`: the virtual time the simulation actually advances (rounded up to a multiple of the time step) |
| `at_ms` | Only for an accepted `step`, an accepted `run_until` and an accepted `pause` with a time. For `step`: the virtual time at which the simulation pauses after the step, known when the step is accepted (see "Stdin commands" for where a step counts from). For the other two: the virtual time asked for, rounded up to a multiple of the time step. It is the time at which the simulation pauses, except when that time has already passed (see "Stdin commands") |
| `id` | The `id` of the stdin line, unchanged (`null` if the line had none, and for the other sources) |

`command` is written when a command arrives on stdin (`--stdin-commands`) and when a Control Server client calls a time operation or a stop (`--control`). `pause` with a time comes only from clients.

A program that sends a command and waits for its answer puts a number of its own into `id` and waits for the `command` event with that `id`. Without `id`, a `command` event of the same name can be the answer to a line that another program sent.

### The `state` event

`state` says where the run stands now and which operations it accepts now. Read it instead of working these out from the other events: the launcher decides each command with the same rules that fill `allowed`.

Unlike every other event, `state` is not history. Only the latest one matters: while a `state` waits to be written (the reader is slow), a newer one replaces it. A reader keeps the last `state` it read and needs no earlier one.

| Key | Value |
|---|---|
| `rev` | Counts the states the launcher made: 1, 2, 3, ... (a gap means that states were replaced before they were written; nothing is lost) |
| `phase` | The current phase (the values of the `phase` event) |
| `paused` | Whether the simulation is paused. Before the simulation starts: whether it will pause at time 0 (`--start-paused`, a `pause`). `false` once the simulation has ended |
| `paused_at_ms` | The virtual time (milliseconds) at which the simulation is paused. `null` while `paused` is `false`, and while `paused` is `true` but the simulation has not come to a stand yet (see below) |
| `holding` | Whether the run waits before the step at time 0 (`--control` with `--start-paused`; see "Write before the start (`--start-paused`)" in [control-api.md](control-api.md#write-before-the-start---start-paused)) |
| `speed` | The speed set: a number or `"max"`. `null` while it is not known yet (before the launcher has set up the simulation, unless a `speed` command has arrived) |
| `pacing` | Whether the run keeps the speed set: `"not-paced"`, `"on-time"` or `"late"` (see below) |
| `stop` | The stop requests: `requests` is 0, 1 (stopping in order) or 2 (not waiting for the participants); `by` is who asked first (the values of `interrupted_by` in the result; `null` if nobody) |
| `scenario` | Whether the run plays a scenario (`--scenario`) |
| `allowed` | For each operation, whether the run accepts it now (see below) |
| `outside` | One element for each network connected to a real bus (`--connect`), in the order of the bridges; an empty array without `--connect` (see below) |

`outside[]` (see "The state of the link and the bus" in [simulation-run.md](simulation-run.md#the-state-of-the-link-and-the-bus)):

| Key | Value |
|---|---|
| `network`, `target`, `participant` | The network, what `--connect` gave for it, and the name of its bridge (`bridge.<network>`) |
| `link` | `"up"`, `"down"` or `"unknown"` as the bridge's link line says; `null` before it has said |
| `bus` | `"active"`, `"passive"`, `"bus-off"` or `"unknown"`; `null` for a bridge that has no bus state (Ethernet), or before it has said |
| `nothing_received` | `true` when the bridge sent frames to the bus and none came from a device there for 10 s since the first frame (the condition of the note `connect-nothing-received`) |
| `condition` | `"ok"`, `"interrupted"` (the link is down or the bus is off; the run goes on) or `"degraded"` (the rest below). When both hold: `"interrupted"` |
| `reasons` | Why the condition is not `ok` (empty when it is), in this order: `link-down`, `bus-off`, `bus-errors` (the bus is error passive), `frames-dropped` (a count of lost frames grew within the last 5 s), `loop-suspected` (the count of stopped loops grew within the last 5 s), `device-shared` (a bridge that is not this run's uses the device; for as long as it does) |

The 5 s hold keeps a `state` from flickering. A bridge that has ended is not shown here (see `participant`, and the error `connect-lost`). The counts themselves are in `outside` events.

`state` has only values that change in steps. Virtual time and the numbers that keep growing are in `time` (and, for a connected network, in `outside`).

`paused`:

A `state` with `paused` `true` is normally written after the simulation has paused. The first one of `--start-paused` is also written after the simulation has paused at time 0, so the result's `paused_ms` counts from before this event. There are 2 exceptions. In both, `true` is written while the simulation has reached the time at which it is to pause but has not paused yet, and `paused_ms` counts from when it actually pauses:

- The simulation has not paused at time 0 within 2 seconds after the start (the launcher stops waiting and writes the events).
- A `pause` or `run_until` names a time at or before the current time while the simulation is running, and the simulation pauses at the next time step.

Where the simulation is paused is `paused_at_ms`: it is in the same `state` as `paused`, so you do not need a `time` event to know it. It is `null` while the simulation has not come to a stand: before the simulation starts (a `--start-paused` run that has not reached time 0), and in the 2 exceptions above. While the run holds before its first step (`holding` `true`), it is `0`. The `state` with the time follows when the simulation has paused. A change of `paused_at_ms` alone (a step ends at another time) also writes a `state`.

`pacing`:

| Value | Meaning |
|---|---|
| `not-paced` | There is no speed to keep: the speed is `"max"`, the simulation is paused, or it is not running |
| `on-time` | The simulation runs at the speed set |
| `late` | A time step is later than `late_tolerance_ms` now (`late_ms` of `time`), or one was late within the last 5 seconds. It goes back to `on-time` after 5 seconds without a late time step |

How late the run is, and how fast it actually goes, are `late_ms`, `slipped_ms` and `actual_speed` of `time`.

`allowed` has one object for each of `pause`, `resume`, `step`, `speed`, `run_until`, `stop` and `write`:

| Key | Value |
|---|---|
| `ok` | Whether the run accepts the operation now (with arguments that are in range) |
| `reason` | The code of why not (see [Reason codes](#reason-codes); `null` if `ok` is `true`). A command sent now is refused with the same `reason` |
| `message` | Why not, and what to do instead: one English sentence (`null` if `ok` is `true`) |
| `caution` | `null`, or `{"code", "message"}`: the run accepts the operation, but there is something to know first (see [Caution codes](#caution-codes)). Always `null` while `ok` is `false` |
| `min_ms`, `max_ms` | Only in `step`: the range of its `ms` (also while `ok` is `false`) |
| `min`, `max` | Only in `speed`: the range of its value (also while `ok` is `false`). `"max"` is accepted as well, unless `min` equals `max` |

`ok` answers "would the run accept it", not "would it change anything": `pause` is `ok` while the simulation is paused (the command is accepted and does nothing). `step` can be `ok` while `paused` is `false`: right after a `pause` was accepted and until the simulation has paused, and while a step is being done.

`pause`, `resume`, `step`, `run_until`, `speed` and `stop` are answered as for a stdin command. `write` (writing a variable) exists only for Control Server clients, so it is answered as for a client. In a run without a Control Server (no `--control`) nothing can send a write, so `write` has `ok` `false` with the reason `no-control-server` for the whole run. A client's time call gets the same answer as the stdin command, also during a scenario; only `write` is refused for the scenario (`scenario-running`).

In a run that is connected outside the simulation and keeps real time (`--time-policy realtime`, the default), `pause`, `step` and `run_until` have `ok` `false` with the reason `outside-real-time`, and `speed` has `min` and `max` 1 (only a speed of 1 is accepted). With `--time-policy free` they have `ok` `true` and the `caution` `outside-not-real-time`. What connects the run outside is `time_lock.because` of the `check` event.

When `state` is written:

| Moment | What is written |
|---|---|
| The phase changes | A `state` with the new phase is written; normally after the `phase` event. |
| A command changes the state (a pause, a speed, a stop from stdin or a client) | A `state` with the change is written after the `command` event of that command. A command that changes nothing writes no `state`. A `pause` can take effect at once (a time step is waiting to keep the speed): this `state` then has `paused` `true` and `paused_at_ms` already. |
| The simulation pauses or continues, the pacing changes, a signal asks to stop | When the launcher next looks (every 20 ms while the simulation runs). A look that finds the simulation paused at another time than the last `time` said writes the `time` event with that time first and this `state` (with `paused_at_ms`) right after it. |

Several threads of the launcher write `state`, so do not rely on the exact place of a `state` among the other events. Next to a `phase` event or a `time` event, one `state` that was made just before that event can follow it and does not show it yet, and the `state` with a new phase can come just before its `phase` event. A `state` that shows the change is always written, and the last `state` is correct: use the latest `state`, not the first one after an event.

A `step` or a `run_until` ends with the simulation paused at the `at_ms` of its `command` event. Reaching that time writes nothing by itself: the launcher's next look writes the `time` event with that `sim_ms`, then the `state` with `paused` `true` and that time in `paused_at_ms`. To send a command once the simulation is there, wait until the latest `state` has `paused` `true` and `paused_at_ms` at (or after) the `at_ms`. A `state` can say so before the `time` event of that time is written (the command's own `state`, if the simulation was already there). A command sent as soon as the `time` event comes can be decided before that `state` is written; the next `state` then shows what the command did.

A stop request by a signal (Ctrl+C) that arrives while the simulation does not run (before it runs, or after it has ended) appears in the next `state` (the one of the next phase).

A `state` of a run with `--control` (pretty-printed here; it is one line in the output):

```json
{
  "event": "state", "seq": 31, "t_ms": 1204,
  "rev": 9,
  "phase": "running",
  "paused": false,
  "paused_at_ms": null,
  "holding": false,
  "speed": 1,
  "pacing": "on-time",
  "stop": { "requests": 0, "by": null },
  "scenario": false,
  "allowed": {
    "pause": { "ok": true, "reason": null, "message": null, "caution": null },
    "resume": { "ok": true, "reason": null, "message": null, "caution": null },
    "step": { "ok": false, "reason": "not-paused", "message": "step works while the simulation is paused: pause it first", "caution": null, "min_ms": 1, "max_ms": 1000000000000 },
    "speed": { "ok": true, "reason": null, "message": null, "caution": null, "min": 1e-06, "max": 1e+06 },
    "run_until": { "ok": true, "reason": null, "message": null, "caution": null },
    "stop": { "ok": true, "reason": null, "message": null, "caution": null },
    "write": { "ok": true, "reason": null, "message": null, "caution": null }
  },
  "outside": []
}
```

### Reason codes

The `reason` of a `command` event that was not accepted, and of `allowed` in `state`. Codes can be added; the meaning of a code does not change.

| Code | Meaning |
|---|---|
| `not-set-up` | The launcher has not set up the simulation yet (`step`, `run_until`, `pause` with a time). Also: more than 256 commands arrived before the launcher had read the configuration file (see [When time commands are accepted](#when-time-commands-are-accepted)) |
| `not-paused` | `step` while the simulation is not paused |
| `not-running` | The simulation has ended, or did not start |
| `scenario-running` | The run plays a scenario: a client cannot write variables (it can move time, as stdin can) |
| `no-control-server` | Only in `allowed` of `state`: the run has no Control Server (`--control`), and only a Control Server client can send the operation (`write`) |
| `outside-real-time` | The run is connected outside the simulation (`--connect`, or an enabled `role: bridge` participant) and keeps real time (`--time-policy realtime`, the default): `pause`, `step`, `run_until` and a `speed` other than 1 are refused. `resume`, a `speed` of 1 and `stop` are accepted. To move time, start the run with `--time-policy free` (see "Time of a run that is connected outside (`--time-policy`)" in [simulation-run.md](simulation-run.md#time-of-a-run-that-is-connected-outside---time-policy)) |
| `out-of-range` | An argument is out of range (a client's speed, step or time) |
| `missing-argument` | A client's `run_until` without a time (on stdin it is `invalid-argument`) |
| `not-a-time-command` | The name is not a time command |
| `invalid-command` | A stdin line that is not a command: not a JSON object, no `command`, or longer than 64 KiB |
| `unknown-command` | A stdin line with a command name that stdin does not accept |
| `invalid-argument` | A stdin line whose argument or `id` is missing, of the wrong form or out of range |
| `unknown-source` | Not written by the launcher: the launcher library refuses a command from a source it does not know |

### Caution codes

The `code` of `caution` in `allowed` of `state`. Codes can be added; the meaning of a code does not change.

| Code | Meaning |
|---|---|
| `outside-not-real-time` | The run is connected outside the simulation and was started with `--time-policy free`: `pause`, `step`, `run_until` and `speed` are accepted, but what is connected does not wait for virtual time, and its bridge still carries frames both ways (frames from outside pile up while the simulation is paused). After the first such command, the run ends with the note `outside-time-changed` |

### The `state` of `participant`

| `state` | Meaning | Other keys |
|---|---|---|
| `disabled` | A disabled participant (not started) | |
| `not_started` | The process could not be started | `message` |
| `started` | The process was started | `pid`, `command`, `log` |
| `manual` | A participant that you start yourself, so the launcher did not start it (instead of `started`) | `command`, `working_directory`, `command_line`, `environment` |
| `joined` | The participant joined the simulation (the controller saw its lifecycle state) | |
| `failed` | `vehicle_sil_launch` decided to end the run because this participant failed | `reason`, `message` |
| `stop_requested` | The participant was asked to stop (item 2 in [Stopping a run](simulation-run.md#stopping-a-run)) | |
| `ended` | The process ended | `exit_code`, `signal` |
| `killed` | The participant was killed (with the processes it started) | |

| `state` | Key | Value |
|---|---|---|
| `not_started` | `message` | The reason |
| `started` | `pid` | ID of the process |
| `started` | `command` | Array of the executable that was started and its arguments |
| `started` | `log` | The `--log-dir` file (`null` if none) |
| `manual` | `command` | Array of the executable to start and its arguments |
| `manual` | `working_directory` | The directory to start it in |
| `manual` | `command_line` | The same command as one line that you can paste into this OS's shell (the same line that is written to stderr) |
| `manual` | `environment` | Object of the names and values of the environment variables to set when you start it |
| `failed` | `reason` | `left_simulation`, `process_ended`, `ended_before_start` (see the table below) |
| `failed` | `message` | The same text as the `message` of the `finding` for this failure |
| `ended` | `exit_code`, `signal` | Same meaning as in `participants[]` of the result |

A `manual` participant later gets `joined` if it joins. It never gets `ended`, `stop_requested` or `killed` (see [Starting a participant yourself](simulation-config.md#starting-a-participant-yourself-manual---manual)). `environment` is present only when `registry_uri` is not a loopback address, and is then `{"VEHICLE_SIL_LISTEN_ON_NETWORK": "1"}` (see [Listen addresses](simulation-run.md#listen-addresses)).

`reason` of `failed`:

| `reason` | Meaning |
|---|---|
| `left_simulation` | The process is running but has left the simulation |
| `process_ended` | The process ended as a failure during the simulation |
| `ended_before_start` | The process ended before the simulation started |

When a process ends, its connection closes first, so the launcher can notice `left_simulation` before the end of the process. `failed` has `message` only when the text is already decided at that point (never with `left_simulation`).

### Event order

The first line is `hello` and the last line is `result`. Nothing is written after `result`.

| Event | Where it is written |
|---|---|
| `participant` with `disabled` | Right after `check` and before `starting`, in configuration file order |
| `control_server` | Once only, after `phase` `starting` and before `registry` |
| `participant` with `manual` | Where `started` would be, in configuration file order |
| `participant` of a bridge of `--connect` (`bridge.<network>`) | Its `started` and `joined` come before the `started` of any participant of the file (see "Connecting a bus outside" in [simulation-run.md](simulation-run.md#connecting-a-bus-outside---connect)) |
| `log` of a participant | After that participant's `started` |
| `outside` | After the `started` of that network's bridge, and before `result` (see [Counts of a connected network (`outside`)](#counts-of-a-connected-network-outside)) |
| `participant` with `joined` | Only for participants that join through the SIL Kit lifecycle |
| `control_link` | After `joined` of all required participants and before `phase` `running`, in configuration file order |
| `participant` with `failed` | Immediately when the failure is decided |
| `command` | Before what the command causes (such as `phase` `stopping`) |
| `state` | Normally after the event of what changed it; one older `state` can come in between, and the last `state` is correct (see [The `state` event](#the-state-event)) |
| `heartbeat` | Between other lines, in any phase from `hello` to `result` |

- `control_server` is not written for a run that fails the check, or for a usage error.
- `manual` is not written when the simulation did not start.
- A `manual` participant has no `log` events (its output appears where you started it).
- `joined` is not written for tool participants that do not join through the lifecycle.
- There is no `control_link` for a participant that could not be started or is disabled.
- The number of `heartbeat` events depends on how long the run takes.
- Time commands from stdin and from clients are processed one at a time, in order, so the order of the `command` events is the order in which they were applied.

The tail of a participant's `log` has no guarantee. Another thread reads the participant's output, so the last few lines can come after that participant's `ended`, or after `phase` `ended`. They never come after `result`.

`failed` is written before the other participants are stopped (`phase` `stopping`) and before the `finding` for the failure. Unlike `log`, it is not dropped when the reader is slow. With `reason` `left_simulation`, it comes before that participant's `ended`. With `process_ended` and `ended_before_start`, the launcher notices the failure from the end of the process, so `failed` comes after `ended`. A reader that does not know `failed` can ignore it (the result and the other events do not change).

When `time` is written:

| Moment | What is written |
|---|---|
| The phase becomes `running` | A `time` at time 0. |
| Virtual time advances | At most once every 100 ms of real time. |
| The simulation is paused at another time than the last `time` said (a pause has taken effect, a step has ended) | Immediately. |
| The run is late (`late_ms` beyond the tolerance, or another late time step) | At most once every 100 ms of real time. |
| The simulation ends | If virtual time has advanced since the last `time`, or the run is late: the time at that moment, whatever the interval. |

That the simulation pauses or resumes, and a change of the speed, write no `time` by themselves: they write a `state`. The time at which the simulation is paused is `paused_at_ms` of `state`; the `time` with that time follows at the launcher's next look, if the last `time` was of another time.

The first `time` is at time 0, even if the simulation is already past 0 when the event is written. This happens, for example, after a `step` or `run_until` that was accepted before the start. If the simulation is paused afterwards, the next `time` carries the time at which it paused. If another command advances the simulation first, or the simulation ends, that `time` is not written.

`sim_ms` never decreases. `vehicle_sil_launch` reads the state every 20 ms, so it may not write changes shorter than that (for example, in the middle of a single-step at maximum speed). The last time is always written.

`finding` is a run finding; the check's findings are inside `check`. When the simulation ends (or does not start), the launcher writes the findings decided at that point, and writes the rest after it has ended the participants. A `finding` once written appears unchanged in the `findings` of `result`. The same finding is never written twice. Reading `result` alone gives the full result.

For a participant that failed during the simulation, `participant-failed` includes how the process ended, so it is written after the process has ended. The `participant` event with `failed` reports the failure as soon as the launcher notices it. The `log` lines of `vehicle_sil_launch` also give the reason.

### How a run ends

Every run that has written `phase` `checking` ends the same way, whether the simulation ran, could not be set up, or the run did not get past the check. A reader needs no special handling for each case:

1. A `state` whose `allowed` takes no time command any more (`pause`, `resume`, `step`, `run_until` and `speed` have `ok: false` and the reason `not-running`).
2. For each time command that was still waiting for the configuration (see [When time commands are accepted](#when-time-commands-are-accepted)), a `command` with `accepted: false` and the reason `not-running`, in the order the commands arrived. Commands wait only until the launcher has made a configuration of the file, so these refusals come when it could not: the file cannot be read (`result` `"unreadable"`), cannot be read as a configuration (a syntax error, not UTF-8 text, an empty file and the like: `"check_failed"`), or the arguments do not fit it (a name of `--disable`, `--enable` or `--manual` that it does not have, `--variant` missing or unknown, overrides that conflict: `"usage"`).
3. Only if the registry and the controller were set up: the findings decided at that point, `phase` `stopping` and the end of the participants. A run whose check passed but that ends before that writes the findings that say why and goes on with step 4, without `stopping`: a stop before the start (there is no `starting` either; `"interrupted"`), log or timeline files that cannot be opened, a control server that cannot be started, a registry or a controller that cannot be set up (`"setup_failed"`). So does a run that an internal error of the launcher ends (the finding `internal`).
4. `phase` `ended`, then the last `state` (`phase` `ended`, still taking no time command).
5. `result`.

The `state` of step 1 is not written when the one of step 4 replaces it before the reader has read it (see [The `state` event](#the-state-event)). A command that arrives while the run ends is answered before step 4 or after its `state`, never between `phase` `ended` and the last `state`. A time command after step 1 is refused with `not-running`; a `stop` on stdin is still accepted (see [What `accepted` means](#what-accepted-means-and-the-result-of-a-stopped-run)). A run that is refused before its check (a usage error on the command line, or in the values of `--control`) reads no command and writes no `phase` and no `state`: only `hello` and `result`.

### Events when the run does not start

`run` with `--events` writes `hello` first and `result` last even on a usage error. The table leaves out the `state` and `heartbeat` lines; a run that has begun its check ends as [How a run ends](#how-a-run-ends) says.

| Case | Events | `result` of `result` | Exit code |
|---|---|---|---|
| Usage error (unknown option, no configuration file given, `--events` together with `--json`, a value of `--control` that is wrong, and so on) | `hello`, `result` | `"usage"` | 4 |
| Usage error found by reading the configuration file | `hello`, `phase` (`checking`), `check`, `phase` (`ended`), `result` | `"usage"` | 4 |
| The configuration file cannot be read | The same 5 | `"unreadable"` | 3 |
| The check found errors | The same 5 | `"check_failed"` | 1 |
| No stdout (started with stdout closed) | Nothing can be written (an error is written to stderr). | | 4 |

Time commands sent before the configuration was made are refused before `phase` (`ended`) when the file cannot be read, on a usage error found by reading it, and when the check fails because the file cannot be read as a configuration (step 2 of [How a run ends](#how-a-run-ends)).

These usage errors are found by reading the configuration file:

- A name given to `--disable`, `--enable` or `--manual` is not in the configuration file.
- The configuration file has variants, and `--variant` is not given.
- The name given to `--variant` is not in the configuration file.

`--events` on a command other than `run` (`check`, `version`, no command) writes no events. It is a usage error: the error goes to stderr and the exit code is 4.

### `log` lines

`line` is one line that a participant wrote to stdout or stderr (without the line break at the end), or one progress line of `vehicle_sil_launch`. It is the same as the line written to stderr without `--events`, minus the leading `[<name>] `.

| Line content | Handling |
|---|---|
| Control characters (except tab) and bytes that are not valid UTF-8 | Replaced with `?`. |
| A line longer than 4096 bytes | Split into several `log` events of 4096 bytes or less (never inside a UTF-8 character). |

No key marks a `log` as the continuation of a split line.

The `--log-dir` files get the original bytes, before replacement. They contain every line, even when `log` events could not be written because the reader was slow.

### Counts of a connected network (`outside`)

With `--connect`, `run` reads the counts of each bridge from its statistics lines (see "Counting the frames to and from the bus" in [simulation-run.md](simulation-run.md#counting-the-frames-to-and-from-the-bus)) and writes them as `outside` events.

| Key | Value |
|---|---|
| `network` | The network that `--connect` connected |
| `to_outside` | The frames that the bridge sent to the bus so far (an integer) |
| `from_outside` | The frames that came from a device on the bus so far (an integer) |

- An `outside` is written when the counts of a network differ from those of its last `outside`, at most once a second for each network. While the simulation runs, counts that change within the second are written when the second has passed; before it runs and while the participants end, they are written with the next counts that differ, or at the end.
- The last `outside` of a network has the counts of that network in the result's `outside[]`. It comes before `result`, and it can come after the bridge's `ended` and after `phase` `stopping`.
- No `outside` is written for a network whose bridge wrote no statistics line that `run` could read (its counts in the result are `null`).
- Unlike `log`, `outside` is not dropped when the reader is slow.
- The statistics lines that give the counts are not `log` events while the bridge runs (the `--log-dir` file of the bridge has them). The last one read is written once, as a `log` event of the bridge, after the last `outside` of that network and before `result`.

A reader that does not know `outside` can ignore it (the result has the last counts).

### Slow readers and readers that stop reading

`vehicle_sil_launch` does not wait for the reader of the events. A dedicated thread writes the events. The thread that drives the run, the threads that read the participants' output and the thread that reads stdin only add events to a queue. Whether the reader is slow or has stopped, the following holds:

- The participants do not stop (the launcher keeps reading their output). The `--log-dir` files are written as usual.
- `--timeout-ms`, signals and the `stop` command on stdin work as usual.

When events pile up because they cannot be written:

| Event | Handling |
|---|---|
| `log` | While more than 4096 lines or 1 MiB of `log` are waiting, new ones are counted instead of written. |
| `time` that only reports that virtual time advanced | Not created while other events are waiting to be written. |
| `time` that reports the time at which the simulation is paused | Created. |
| Other events | Never dropped. |

The counted `log` events are reported by one `log_dropped` line in their place (the reader receives it when it reads again). `count` and `sources` cover what was counted up to the moment that line is written.

In either of these cases, the launcher treats the reader as gone (see [When the reader goes away](#when-the-reader-goes-away)):

- Writing makes no progress at all for 30 seconds.
- More than 4096 events other than `log` are waiting.

How "no progress for 30 seconds" is measured:

- The count starts when a write starts to wait (when the pipe is full), not when the reader stops reading. In a run that writes little (only `heartbeat`, for example), the pipe takes a long time to fill. Stopping reading does not always stop the run after 30 seconds.
- `vehicle_sil_launch` writes in units of up to 4096 bytes and counts progress each time one unit fits. A reader that reads about 4096 bytes within 30 seconds counts as reading (with a pipe set not to block, 1 byte is enough).
- Change the 30 seconds with `--events-stall-ms <ms>` (1000 or more). Make it longer for a reader that may stop in a debugger. The same applies when the output goes to a terminal: if you keep the output paused, the run stops.

When the simulation ends, `vehicle_sil_launch` waits until it has written `result`. It waits at most 30 seconds (the value of `--events-stall-ms`), even if the reader is reading. With many waiting events and a slow reader, the reader may not receive `result`. The launcher stops waiting when 2 or more stop requests have arrived while writing has made no progress for 0.5 seconds (or at the first request, if there was no request before).

If `result` cannot be written, the exit code is 3 and stderr gets `error: cannot write the result to the standard output (<reason>)`. This also holds for a run that a `stop` command stopped (its exit code would have been 0 or 5, or 1 with errors): whoever waits for the result did not get it. Only an interrupted run (a signal, the end of stdin, a closed stdout) exits with 130 and does not write this line.

### When the reader goes away

The launcher notices that the reader of stdout has gone (the pipe has closed) the next time it writes a line. Because of `heartbeat`, this takes at most 1 second in any phase. The same handling applies when the "stop reading" conditions above are met.

| Action | Details |
|---|---|
| Close the event output | Nothing more is written, including `result` (a line being written can be cut off). |
| Treat it as a stop request | The same as the first Ctrl+C. |
| Write one line with the reason to stderr | `vehicle_sil_launch: cannot write the events (<reason>): stopping the run` |
| Result and exit code | `interrupted_by` in the result is `"output closed"` and the exit code is 130 (the launcher does not end on SIGPIPE). |

- If a stop request has already arrived, this does nothing as a stop request (it does not count as a second one). The stderr line then ends with `no more events are written`.
- If the output closes while the launcher waits to write `result`, stderr gets the `error: cannot write the result ...` line above instead of this line.
- If nobody reads stderr either, nobody sees this line (`vehicle_sil_launch` still ends).

The behavior is the same when stdout is a pipe set not to block on writes (`O_NONBLOCK` and similar). A full pipe is not treated as closed; the launcher waits until it has room.

### Reader requirements

- Keep reading stdout. The `result` line ends the output.
- Drop a last line that does not end with a line break (`vehicle_sil_launch` was killed, or the output closed).
- Ignore unknown `event` values and unknown keys.
- Also read stderr (you can discard it).
- A program that starts `vehicle_sil_launch` and reads it to the end (the GUI, a CI script) also passes `--stdin-commands`.

| What you see | Meaning |
|---|---|
| Stdout ends without a `result` | `vehicle_sil_launch` ended without writing the result (killed, crashed, or the "goes away" handling above). |
| No line at all, not even `heartbeat`, for several seconds | `vehicle_sil_launch` is stuck (it writes at least 1 line per second). |

If no `result` arrives, use the exit code of the process.

`event` values, keys, reason codes, and the values of `phase` and of a participant's `state` can be added without a version change. When the meaning of a key changes or a key is removed, `vehicle_sil_events` goes up. Format 2 took `paused` and `speed` out of `time`; they are in `state`.

With `--events`, stderr gets only the notices of `vehicle_sil_launch` itself (usage errors, events can no longer be written, the result cannot be written, unexpected failures). Participant lines and progress lines do not go there. Anything a library inside `vehicle_sil_launch` writes to stdout goes to stderr (it never mixes with the event lines).

With `--stdin-commands`, stdin closes when the program that started the launcher stops or ends, so `vehicle_sil_launch` stops in order right away, whatever the state of stdout.

### Other rules

- String escaping is the same as with `--json` (`"`, `\`, control characters; bytes that are not UTF-8 become U+FFFD). The `line` of `log` has already been through the `?` replacement described above.
- Lines have no length limit (`check` and `result` get long).
- `--json` together with `--events` is a usage error (the last event is the result).
- The exit codes are the same as without `--events`.

### Example

All lines of a short run. `--disable Dashboard` leaves only Engine, so the output order is fixed. Times, process IDs, the registry port, the example directory, the version and the pace of time differ from run to run, so they appear as `<t_ms>`, `<pid>`, `<port>`, `<dir>`, `<version>`, `<wall_ms>` and `<actual_speed>`. On Windows, the executable names end in `.exe`.

<!-- vehicle_sil_launch run simulation.yaml --events --disable Dashboard -->
```text
{"event":"hello","seq":0,"t_ms":<t_ms>,"vehicle_sil_events":2,"vehicle_sil":"<version>","pid":<pid>,"file":"simulation.yaml","arguments":["run","simulation.yaml","--events","--disable","Dashboard"]}
{"event":"phase","seq":1,"t_ms":<t_ms>,"phase":"checking"}
{"event":"check","seq":2,"t_ms":<t_ms>,"check":{"vehicle_sil_check":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"stop_at_ms":1000,"start_timeout_ms":30000,"stop_timeout_ms":10000,"registry_uri":null,"commands":null,"participants":[{"name":"Engine","line":11,"command":["engine/vehicle_sil_engine_ecu"],"describe":true,"required":true,"enabled":true,"manual":false,"role":"ecu","step_ms":10,"status":"described","reported":{"vehicle_sil":"<version>","name":"Engine","step_ns":10000000,"networks":[{"name":"J1939_CAN","kind":"can","use":"j1939","nodes":[{"address":0,"j1939_name":"0x00000000FE000001","arbitrary_address_capable":false}],"mac":null}]},"variables":[{"name":"ecu.connected","type":"bool","causality":"input","unit":"","min":null,"max":null,"step":null,"description":"on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on","items":[],"builtin":true},{"name":"engine.throttle","type":"real","causality":"input","unit":"%","min":0,"max":100,"step":null,"description":"accelerator pedal position","items":[],"builtin":false},{"name":"engine.speed","type":"integer","causality":"output","unit":"rpm","min":null,"max":null,"step":null,"description":"","items":[],"builtin":false}],"network_map":null,"j1939_nodes":null,"control":{"transport":"silkit","wire_version":2,"wire_revision":1}},{"name":"Dashboard","line":14,"command":["dashboard/vehicle_sil_dashboard_ecu"],"describe":true,"required":true,"enabled":false,"manual":false,"role":"ecu","step_ms":5,"status":"disabled","reported":null,"variables":null,"network_map":null,"j1939_nodes":null,"control":null}],"networks":[{"name":"J1939_CAN","kind":"can","line":8,"participants":["Engine"],"outside":null}],"time_lock":{"locked":false,"because":[]},"findings":[{"severity":"warning","code":"single-participant","file":"simulation.yaml","line":8,"participant":null,"network":"J1939_CAN","message":"network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it; if it talks only to equipment outside, connect it outside to run it (external says what passes)","detail":null},{"severity":"note","code":"participant-disabled","file":"simulation.yaml","line":14,"participant":"Dashboard","network":null,"message":"participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it","detail":null}]}}
{"event":"participant","seq":3,"t_ms":<t_ms>,"name":"Dashboard","state":"disabled"}
{"event":"phase","seq":4,"t_ms":<t_ms>,"phase":"starting"}
{"event":"registry","seq":5,"t_ms":<t_ms>,"uri":"silkit://127.0.0.1:<port>"}
{"event":"log","seq":6,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"registry at silkit://127.0.0.1:<port>"}
{"event":"log","seq":7,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"starting Engine: <dir>/engine/vehicle_sil_engine_ecu --name Engine --registry-uri silkit://127.0.0.1:<port> --start-timeout-ms 50000"}
{"event":"participant","seq":8,"t_ms":<t_ms>,"name":"Engine","state":"started","pid":<pid>,"command":["<dir>/engine/vehicle_sil_engine_ecu","--name","Engine","--registry-uri","silkit://127.0.0.1:<port>","--start-timeout-ms","50000"],"log":null}
{"event":"log","seq":9,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"not starting Dashboard: disabled"}
{"event":"phase","seq":10,"t_ms":<t_ms>,"phase":"waiting"}
{"event":"participant","seq":11,"t_ms":<t_ms>,"name":"Engine","state":"joined"}
{"event":"control_link","seq":12,"t_ms":<t_ms>,"name":"Engine","state":"connected","variables":22}
{"event":"phase","seq":13,"t_ms":<t_ms>,"phase":"running"}
{"event":"log","seq":14,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"simulation running"}
{"event":"time","seq":15,"t_ms":<t_ms>,"sim_ms":0,"actual_speed":<actual_speed>,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0}
{"event":"time","seq":16,"t_ms":<t_ms>,"sim_ms":1000,"actual_speed":<actual_speed>,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0}
{"event":"log","seq":17,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"the simulation ended"}
{"event":"phase","seq":18,"t_ms":<t_ms>,"phase":"stopping"}
{"event":"log","seq":19,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"Engine exited with code 0"}
{"event":"participant","seq":20,"t_ms":<t_ms>,"name":"Engine","state":"ended","exit_code":0,"signal":null}
{"event":"phase","seq":21,"t_ms":<t_ms>,"phase":"ended"}
{"event":"result","seq":22,"t_ms":<t_ms>,"result":{"vehicle_sil_run":2,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","end":"completed","ok":true,"message":"The run reached its end without errors","strict":false,"errors":0,"warnings":1,"notes":1,"simulation":"reached_stop_at","interrupted_by":null,"registry_uri":"silkit://127.0.0.1:<port>","timing":{"speed":"max","time_step_ms":10,"ecu_step_ms":10,"step_delay_ms":0,"late_tolerance_ms":10,"sim_ms":1000,"wall_ms":<wall_ms>,"actual_speed":<actual_speed>,"paced_steps":0,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0,"paused_ms":0},"participants":[{"name":"Engine","required":true,"enabled":true,"manual":false,"for_network":null,"ended":"exited","exit_code":0,"signal":null,"left_processes":false,"log":null,"control_link":{"state":"connected","variables":22,"version":null,"late_messages":0}},{"name":"Dashboard","required":true,"enabled":false,"manual":false,"for_network":null,"ended":"not_started","exit_code":null,"signal":null,"left_processes":false,"log":null,"control_link":{"state":"none","variables":null,"version":null,"late_messages":0}}],"outside":[],"traffic":{"before_ms":1000,"unplaced":[],"networks":[{"name":"J1939_CAN","kind":"can","counted":1,"senders":1,"receivers":0,"delivered":false,"unplaced":[],"participants":[{"name":"Engine","counted":true,"not_counted":null,"virtual_time":true,"at_ms":990,"frames_sent":11,"frames_received":0}]}]},"findings":[],"check":{"vehicle_sil_check":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"stop_at_ms":1000,"start_timeout_ms":30000,"stop_timeout_ms":10000,"registry_uri":null,"commands":null,"participants":[{"name":"Engine","line":11,"command":["engine/vehicle_sil_engine_ecu"],"describe":true,"required":true,"enabled":true,"manual":false,"role":"ecu","step_ms":10,"status":"described","reported":{"vehicle_sil":"<version>","name":"Engine","step_ns":10000000,"networks":[{"name":"J1939_CAN","kind":"can","use":"j1939","nodes":[{"address":0,"j1939_name":"0x00000000FE000001","arbitrary_address_capable":false}],"mac":null}]},"variables":[{"name":"ecu.connected","type":"bool","causality":"input","unit":"","min":null,"max":null,"step":null,"description":"on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on","items":[],"builtin":true},{"name":"engine.throttle","type":"real","causality":"input","unit":"%","min":0,"max":100,"step":null,"description":"accelerator pedal position","items":[],"builtin":false},{"name":"engine.speed","type":"integer","causality":"output","unit":"rpm","min":null,"max":null,"step":null,"description":"","items":[],"builtin":false}],"network_map":null,"j1939_nodes":null,"control":{"transport":"silkit","wire_version":2,"wire_revision":1}},{"name":"Dashboard","line":14,"command":["dashboard/vehicle_sil_dashboard_ecu"],"describe":true,"required":true,"enabled":false,"manual":false,"role":"ecu","step_ms":5,"status":"disabled","reported":null,"variables":null,"network_map":null,"j1939_nodes":null,"control":null}],"networks":[{"name":"J1939_CAN","kind":"can","line":8,"participants":["Engine"],"outside":null}],"time_lock":{"locked":false,"because":[]},"findings":[{"severity":"warning","code":"single-participant","file":"simulation.yaml","line":8,"participant":null,"network":"J1939_CAN","message":"network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it; if it talks only to equipment outside, connect it outside to run it (external says what passes)","detail":null},{"severity":"note","code":"participant-disabled","file":"simulation.yaml","line":14,"participant":"Dashboard","network":null,"message":"participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it","detail":null}]}}}
```

The CTest test `launch_run.events` compares this example with the real output byte for byte. Before the comparison, it removes the `heartbeat` lines, the `state` lines and every `time` line other than the first and the last from the real output (renumbering `seq`), and makes the replacements above. It removes those `time` lines because on a slow computer the launcher also writes `time` events for intermediate times, and the `state` lines because how many are written depends on how fast they are read (see [The `state` event](#the-state-event)).

## Stdin commands (`run --stdin-commands`)

The launcher reads stdin only with `--stdin-commands`. Without it, stdin is not read (a closed or empty stdin does not stop the run). Each line is one JSON object. Use it together with `--events` (see [Reader requirements](#reader-requirements)); it also works on its own.

| Line | Meaning |
|---|---|
| `{"command":"stop"}` | Stops in order (the same as the first Ctrl+C). |
| `{"command":"pause"}` | Pauses. |
| `{"command":"resume"}` | Ends the pause and continues. |
| `{"command":"step","ms":<ms>}` | Only while paused: advances virtual time by `<ms>` (an integer from 1 to 10^12) and pauses again. |
| `{"command":"run_until","at_ms":<ms>}` | Runs on and pauses at virtual time `<ms>` (an integer from 0 to 10^12; rounded up to a multiple of the time step). The same as `RunUntil` of the [Control API](control-api.md), and it does not need `--control`. |
| `{"command":"speed","value":<speed>}` | Changes the speed (`<speed>` is a number or `"max"`, in the same range as `speed` in the configuration file). |
| Any of the above with `"id":<id>` | The same; `<id>` (an integer from 0 to 2^53) comes back in the `id` of the line's `command` event. |
| End of stdin (EOF) | The same as one `stop` (the program that started the launcher has gone, or closed the pipe). |
| A line of only white space | Skipped. |
| Anything else | Does nothing. |

| Command | Notes |
|---|---|
| `stop` | A second one is the same as a second Ctrl+C (the launcher stops waiting and kills the remaining participants). |
| `stop` | One received during the check stops the run after the check has finished (the same as Ctrl+C). |
| `pause` | The simulation pauses at a boundary of the controller's time steps (at a time step that is waiting to keep the speed, if there is one; otherwise at the next time step). |
| `resume` | Speed keeping starts over from here. |
| `step` | Rounded up to a multiple of the time step (the `ms` of the `command` event is the actual amount). |
| `step` | The `at_ms` of the `command` event is the virtual time at which the simulation pauses after the step. To know that the step has arrived, wait until the latest `state` has `paused` `true` and that time (or a later one) in `paused_at_ms`, instead of adding `ms` to a time you read earlier. A later `pause`, `resume`, `run_until` or `step`, or the end of the run, changes where the simulation pauses. |
| `step` | Sent while a previous `step` or a `run_until` is in progress, it counts from the time at which that one pauses. |
| `step` | Sent after a `pause` was accepted and before the simulation has paused, it counts from the time step in progress: the `pause` has ended the `run_until` or `step` that was in progress, whether or not a time step was waiting to keep the speed at that moment. |
| `step` | Sent after a client's `pause` with a time ([Control API](control-api.md), `at_ms`) that the simulation has not reached yet, it counts from that time (the `at_ms` of that `pause`'s `command` event). |
| `run_until` | Works while running and while paused. If the simulation is paused at or after the time, it stays paused and nothing happens. If the time has passed while it runs, it pauses at the next time step. The `at_ms` of the `command` event is the time asked for, rounded up to a multiple of the time step; for a time that has passed, it is not the time at which the simulation pauses (wait until the latest `state` has `paused` `true` and a `paused_at_ms`, and read that, instead of waiting for `at_ms`). |
| `speed` | A pause stays in effect. |
| EOF | If a stop request (`stop`, a signal, a closed stdout) has already arrived, does nothing (it does not count as a second one and no `command` event is written). |
| Anything else | This covers a line that is not a JSON object, has no `command`, has an unknown command, has an `id` that is not an integer from 0 to 2^53, or is longer than 64 KiB. |
| Anything else | With `--events`, a `command` event (`accepted: false`, the `message` and the `reason`) is written; otherwise one line goes to stderr. |

Other keys of the object are ignored; a `pause` with an `at_ms` is a plain `pause` (to pause at a time, use `run_until`). The `state` event answers a `run_until` as for stdin even in a run without `--stdin-commands`. Commands are accepted and carried out even when nobody reads the events.

### When time commands are accepted

For a time command (`pause`, `resume`, `step`, `run_until`, `speed`), the thread that reads stdin decides whether to accept it, writes the `command` event, and then hands the command to the controller. None of this blocks: neither the thread that drives the run nor the reader of the events waits. The controller applies the command at the next time step boundary.

| Case | Handling |
|---|---|
| `step` sent while not paused (after a `resume`, for example) | Not accepted. |
| `step` or `run_until` sent before the controller exists (during the check, for example) | Not accepted. |
| A time command after the simulation has ended (or did not start) | Not accepted. |
| `pause` while paused, `resume` while running | Accepted; does nothing. |
| `pause`, `resume` or `speed` received before the controller exists (during the check, for example) | Kept, and handed to the controller when it exists. |
| Any time command received before the launcher has read the configuration file | Waits, undecided. Answered once the file is read (see below). |
| `pause`, `step` or a `speed` other than 1 in a run that is connected outside and keeps real time | Not accepted (`outside-real-time`). |

The reason for not accepting goes into `message` (a sentence) and `reason` (a code; see [Reason codes](#reason-codes)). `allowed` of the `state` event says the same before a command is sent. A `pause` received before the controller exists pauses at 0, as `--start-paused` does.

A pause leaves `phase` at `running`. The `paused` of `state` reports the pause.

Whether a run keeps real time (`--connect`, a `role: bridge` participant, `--time-policy`) is known only once the launcher has read the configuration file, so a time command that arrives earlier is not decided when it arrives:

- It waits. Its `command` event is written when the file has been read, with the answer of that moment. The commands that waited are answered in the order in which they arrived, before any command that arrives later, so the order of the `command` events is still the order in which the commands took effect.
- `stop` does not wait: it is accepted at once, whatever waits.
- At most 256 commands wait. One more is refused at once with the reason `not-set-up`; send it again after the `check` event (the file has been read then; the first `state` event comes before that).
- If the launcher cannot make a configuration of the file (it cannot be read, it cannot be read as a configuration, or the arguments do not fit it), nothing starts, and the commands that waited are refused with the reason `not-running`, before `phase` `ended` (see [How a run ends](#how-a-run-ends)).
- The `state` event written before the file is read does not know the rule yet: its `allowed` is the answer for a run that is not connected outside. The next `state` has the rule.

In a run with `--control` and `--start-paused` that pauses before the step at time 0, a `resume`, `step`, `run_until` or `pause` accepted before the pause also decides how the run pauses afterwards (see "Write before the start (`--start-paused`)" in [control-api.md](control-api.md#write-before-the-start---start-paused)).

### What `accepted` means, and the result of a stopped run

`accepted: true` in a `command` event means that the line is a valid command and was handed on for processing (for a time command, also that it can be carried out now). It does not mean that the command changed the run. For example, a `stop` received after the run has ended (after `phase` `ended`) has `accepted: true` but does not change the result.

A `command` event is written before what the command causes (such as `phase` `stopping`).

A run stopped by `{"command":"stop"}` or by `Stop` from a Control Server client is not an interrupted run: `result` is `"stopped"` and the exit code is 0 (the note `stopped`), or `"incomplete"` and 5 if a scenario was stopped before its end. The end of stdin counts as an interrupt (`"interrupted"`, 130): the program that wrote the commands has gone. See "How a run ended" in [simulation-run.md](simulation-run.md#how-a-run-ended-end-ok-message).

| Stopped by | `interrupted_by` | `end` |
|---|---|---|
| `{"command":"stop"}` | `"stop command"` | `"stopped"` |
| End of stdin | `"end of input"` | `"interrupted"` |
| `Stop` from a Control Server client | `"control server stop"` | `"stopped"` |

### Commands from Control Server clients

The time operations (`Pause`, `Resume`, `Step`, `RunUntil`, `SetSpeed`) and `Stop` of Control Server clients (`--control`) are processed as the same commands (`source` is `"control_server"`). For the rules and the order, see "Stdin commands and client commands" in [control-api.md](control-api.md#stdin-commands-and-client-commands).

| Item | Stdin | Client |
|---|---|---|
| Without `--stdin-commands` | Not read. | Accepted. |
| `pause` with a time | Cannot be sent. | Can be sent. |
| A stop after the simulation has ended | `stop` is accepted and does nothing. | `Stop` is rejected. |

A run with a scenario (`--scenario`) accepts the time commands from both. They do not change the result of the scenario (see "Watching variables during a run (`--control`)" in [scenario.md](scenario.md#watching-variables-during-a-run---control)).

## Version and features (`vehicle_sil_launch version`)

`vehicle_sil_launch version` (or `--version`) writes one line, `vehicle_sil_launch <version>`. `version --json` writes the versions of the formats and the list of features. Neither reads a file, and the exit code is 0.

<!-- vehicle_sil_launch version --json -->
```json
{
  "vehicle_sil_launch": 1,
  "vehicle_sil": "<version>",
  "formats": {
    "check": 1,
    "run": 2,
    "events": 2,
    "describe": 1,
    "test": 1,
    "timeline": 1,
    "connections": 1
  },
  "features": [
    "events",
    "stdin-commands",
    "check-variables",
    "speed",
    "start-paused",
    "time-commands",
    "control-links",
    "scenario",
    "manual-participants",
    "scenario-wave",
    "scenario-motion-expect",
    "scenario-points-file",
    "timeline",
    "timeline-lines",
    "scenario-inputs",
    "network-names",
    "run-traffic",
    "j1939-nodes",
    "connect",
    "connections",
    "connect-counts",
    "state",
    "time-policy",
    "stdin-run-until",
    "request",
    "scenario-samples"
  ]
}
```

| Key | Value |
|---|---|
| `vehicle_sil_launch` | Version of this JSON format (currently 1) |
| `vehicle_sil` | vehicle-sil version |
| `formats` | Versions of the formats the launcher writes and reads (see the table below) |
| `features` | Array of feature names (see the table below) |

A reader decides by whether a name is in `features`, not by comparing versions. Names are only ever added.

| Key of `formats` | Version of |
|---|---|
| `check` | `vehicle_sil_check` of `check --json` |
| `run` | `vehicle_sil_run` of `run --json` |
| `events` | `vehicle_sil_events` of `hello` |
| `describe` | `vehicle_sil_ecu` of the `--describe` JSON that the launcher reads |
| `test` | `vehicle_sil_test` of `test --json` |
| `timeline` | `vehicle_sil_timeline` of the file that `run --timeline` writes |
| `connections` | `vehicle_sil_connections` of `connections --json` |

### Keys and values of the JSON forms

These rules hold for every JSON form above (`check --json`, `run --json`, `test --json`, `connections --json`, the events, `version --json` and the `--timeline` file):

- Keys are in snake_case: `time_step_ms`, `not_applied_from`.
- A key added after vehicle-sil 0.1.0 is always there. When it does not apply, its value is `null`. Some keys that 0.1.0 already had are written only when they apply, and their descriptions say when: for example `params`, `skipped_steps` and `unchecked_expects` of a scenario, `expect_skipped` of a step, and `variable_map` of a participant.
- Values that are words of the form itself (states, statuses, reasons, sources) use underscores: `not_run`, `after_wait`.
- Names that people also type or search for keep their hyphens: the names in `features` (`run-traffic`) and the codes of findings (`single-participant`).

### `features` names

| Name in `features` | Feature |
|---|---|
| `events` | `run --events` |
| `stdin-commands` | `run --stdin-commands` and `{"command":"stop"}` |
| `check-variables` | `participants[].variables` in the check JSON |
| `speed` | Setting the speed (see [Running at a set speed](simulation-run.md#running-at-a-set-speed-speed)) |
| `start-paused` | `run --start-paused` |
| `time-commands` | `pause`, `resume`, `step` and `speed` on stdin |
| `control-links` | Connection to ECU variables (see [Connection to ECU variables](simulation-run.md#connection-to-ecu-variables-control_link)) |
| `manual-participants` | Participants that you start yourself |
| `scenario` | `check --scenario` and `run --scenario` (see [scenario.md](scenario.md)) |
| `scenario-wave` | The scenario step `wave` (see "Waveforms (`wave`)" in [scenario.md](scenario.md#waveforms-wave)) |
| `scenario-motion-expect` | An `expect` beside a scenario's `ramp` or `wave` (see "Checking values during a ramp or wave" in [scenario.md](scenario.md#checking-values-during-a-ramp-or-wave)) |
| `scenario-points-file` | The `points` of a scenario's `wave` read from a CSV file (`points: { file, value, time, time_unit }`, see "Reading points from a CSV file" in [scenario.md](scenario.md#reading-points-from-a-csv-file)) |
| `timeline` | `run --scenario --timeline <file>`, a file that records the scenario run on a timeline (see "Recording a scenario run on a timeline (`--timeline`)" in [simulation-run.md](simulation-run.md#recording-a-scenario-run-on-a-timeline---timeline)) |
| `timeline-lines` | `lines[]` in the `--timeline` file (the line of each input: the values and motions that steps placed, and the start value of each motion with where it came from), and `writes[]` of the steps (see the same section of [simulation-run.md](simulation-run.md#input-lines-lines)) |
| `scenario-inputs` | The top-level `inputs` of a scenario (a `dither` overlaid on an input) and the `dither` step (see "Overlaying a dither (`dither`)" in [scenario.md](scenario.md#overlaying-a-dither-dither)) |
| `network-names` | `network_names` in the configuration file (see "Matching network names (`network_names`)" in [simulation-config.md](simulation-config.md#matching-network-names-network_names)) |
| `run-traffic` | `traffic` in the run result: the J1939 traffic on each network and `delivered` (see "Network traffic (`traffic:`)" in [simulation-run.md](simulation-run.md#network-traffic-traffic)) |
| `j1939-nodes` | `j1939_nodes` in the configuration file (see "Matching J1939 addresses and NAMEs (`j1939_nodes`)" in [simulation-config.md](simulation-config.md#matching-j1939-addresses-and-names-j1939_nodes)) |
| `connect` | `--connect` of `check` and `run`: connecting a network to a real bus through its bridge (see "Connecting a bus outside (`--connect`)" in [simulation-run.md](simulation-run.md#connecting-a-bus-outside---connect)) |
| `connections` | `vehicle_sil_launch connections`: where each network can be connected on this computer (see "Where a network can be connected (`vehicle_sil_launch connections`)" in [simulation-run.md](simulation-run.md#where-a-network-can-be-connected-vehicle_sil_launch-connections)) |
| `connect-counts` | The frames that a connected network sent to the real bus and got from a device there (see "Counting the frames to and from the bus" in [simulation-run.md](simulation-run.md#counting-the-frames-to-and-from-the-bus)) |
| `state` | The event `state`: where the run stands and what it accepts now (see [The `state` event](#the-state-event)) |
| `time-policy` | `--time-policy` of `check`, `run` and `test`: a run that is connected outside the simulation keeps real time unless `free` is chosen (see "Time of a run that is connected outside (`--time-policy`)" in [simulation-run.md](simulation-run.md#time-of-a-run-that-is-connected-outside---time-policy)) |
| `stdin-run-until` | `run_until` on stdin (see [Stdin commands](#stdin-commands-run---stdin-commands)) |
| `request` | `--request <file>` of `check` and `run`: what to run, from a request file (see "Request files (`--request`)" in [simulation-run.md](simulation-run.md#request-files---request)) |
| `scenario-samples` | The top-level key `samples` of a scenario, and a sample of every ECU step for the ECUs whose values a scenario reads (see "How often the ECUs send their values" in [scenario.md](scenario.md#how-often-the-ecus-send-their-values-samples)) |
| `control-server` | `run --control` (only in builds with the Control Server) |
| `scenario-control` | `run --scenario` and `--control` can be used together (only in builds with the Control Server) |
| `scenario-time` | Control Server clients can move time while a scenario runs (only in builds with the Control Server) |

Each name indicates that the following are present:

| Name in `features` | Includes |
|---|---|
| `speed` | `speed`, `time_step_ms` and `late_tolerance_ms` in the configuration file, `run --speed`, `timing` in the result, the findings `behind-real-time` and `coarse-time-step`, and `actual_speed`, `late_steps`, `max_late_ms`, `slipped_ms` and `late_ms` in the `time` event |
| `control-links` | The event `control_link`, `participants[].control_link` in the result, `participants[].control` in the check JSON, the findings `control-link-no-hello`, `control-link-version`, `control-link-revision`, `control-link-late-values`, `control-link-dropped` and `control-link-not-offered` |
| `manual-participants` | `manual` in the configuration file, `--manual`, `participants[].manual` in the check and result JSON, the finding `participant-manual`, and the `state` `manual` of the event `participant` |
| `scenario` | `scenario` in the result and check JSON, the event `scenario_step`, the findings `scenario-*`, and a finding's `file` being the scenario file |
| `scenario-wave` | `undersampled` of that step, the findings `scenario-wave-*`, and `"wave"` as the `kind` of a step in the result and in events |
| `scenario-motion-expect` | `expect_skipped` of a step in the result and in events |
| `scenario-points-file` | The finding `scenario-points-file` |
| `timeline` | `scenario.timeline` in the result, the findings `timeline-write` and `timeline-limit` |
| `scenario-inputs` | The findings `scenario-dither-*` and `scenario-input-skipped`, `"dither"` as the `kind` of a step in the result and in events, `scenario.inputs` and `scenario.lines` in the check and result JSON, `"dither"` as the `kind` of `lines[]` in the `--timeline` file, and the third column (`columns`) of `series` |
| `network-names` | `--map-network` passed to executables, `participants[].network_map` in the check JSON, and the findings `network-map-*` |
| `run-traffic` | The `traffic:` line and table of the human-readable result, and `traffic` in `run --json` and in the event `result` |
| `j1939-nodes` | `--j1939-node` passed to executables, `participants[].j1939_nodes` in the check JSON, the findings `j1939-node-*`, the end of `address-conflict`, `address-shared` and `name-conflict` that says how to fix them in the configuration file, and `0x01<-0x00` in the participant table of the check |
| `connect` | `external` of a network in the configuration file, `check --connect` and `run --connect`, `networks[].outside` in the check JSON, `outside` and `participants[].for_network` in the result, the participant `bridge.<network>` in the result and the events, the `outside:` lines of the human-readable result, and the findings `connect-*` |
| `connections` | `vehicle_sil_launch connections` with `--json` and `--network`, `connections` in `formats`, and `vehicle_sil_launch connections` named by the findings `connect-target` and `connect-failed` |
| `connect-counts` | The event `outside`, the counts in `outside[].to_outside` and `from_outside` of the result (`null` before this feature), the counts at the end of the `outside:` lines of the human-readable result, the finding `connect-nothing-received`, and `--stats-every-ms 1000` in the command of a bridge that `run` starts |
| `state` | The event `state`, `reason` and `id` in the event `command`, and `id` in the stdin commands |
| `time-policy` | `--time-policy`, `time_lock` in the check JSON, the findings `outside-real-time`, `outside-speed-set`, `outside-not-real-time`, `outside-time-changed` and `unsynchronized-time-changed`, the reason `outside-real-time` of a refused command and of `allowed`, and the caution `outside-not-real-time` (before this feature: the warnings `connect-not-real-time`, `connect-start-paused` and `connect-time-changed`, and a run with `--connect` went at the file's speed and accepted every time command) |
| `request` | `--request`, the findings `connect-network-unused` and `request-params-unused`, and `request_file` as the `from` of a parameter in the check and result JSON |
| `scenario-samples` | `samples` of a scenario, `--sample-every-step` passed to executables, and the findings `scenario-samples-*` and `scenario-not-observed` |
| `control-server` | The event `control_server`, `"control_server"` as the `source` of `command` and its `at_ms`, `--control-credentials`, `--control-tls-cert`, `--control-tls-key`, `--control-audit-log`, `"control server stop"` as `interrupted_by`, and pausing before the step at time 0 with `--start-paused` |

In a build with `scenario-control`, clients cannot write while a scenario runs: calls that write are rejected with `FAILED_PRECONDITION` (reason `scenario-running`). In a build with `scenario-time`, calls that move time (`Pause`, `Resume`, `Step`, `RunUntil`, `SetSpeed`) are accepted during a scenario, as the same commands from stdin are, and do not change the result of the scenario; a build with `scenario-control` alone rejects them with the same reason (see "Watching variables during a run (`--control`)" in [scenario.md](scenario.md#watching-variables-during-a-run---control)).

The example above is the output of a build without the Control Server. A build with it adds `"control-server"`, `"scenario-control"` and `"scenario-time"` at the end of `features`. The CTest test `launch_cli.docs` also checks this example (byte for byte, except the `vehicle_sil` version and these 3 names).

## See also

- How a run proceeds, how to stop it, and the result JSON → [simulation-run.md](simulation-run.md)
- The check JSON → [simulation-check.md](simulation-check.md)
- Reading and writing variables and controlling time over gRPC → [control-api.md](control-api.md)
- The browser GUI and its development → [gui.md](gui.md)

The tests of the browser GUI replay recordings of this output (`gui/server/tests/recordings/`). If you change the launcher's output, compare with `python3 gui/server/tests/record_launcher.py --launcher <vehicle_sil_launch> --compare` and record the scenes that differ again (see "Development" in [gui.md](gui.md#development)).
