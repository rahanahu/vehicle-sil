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
| `finding` | A run finding is decided | The same keys as one entry of `findings[]` in the result |
| `scenario_step` | A step of the `--scenario` scenario starts or is decided | The same keys as one entry of `scenario.steps[]` in the result |
| `time` | Virtual time advances, the simulation pauses or resumes, or the speed changes | `sim_ms`, `speed`, `paused`, `actual_speed`, `late_steps`, `max_late_ms`, `slipped_ms`, `late_ms` |
| `command` | A command arrives | `command`, `source`, `accepted`, `message`, `ms`, `at_ms` |
| `heartbeat` | Nothing was written for 1 second | None (only the 3 common keys) |
| `result` | Last line | `result` |

### Keys by event

`hello`:

| Key | Value |
|---|---|
| `vehicle_sil_events` | Version of the event format (currently 1) |
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

The phases advance in this order and never go back. A phase can be skipped: if the check fails, `ended` follows `checking`.

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
| `speed` | Current speed (a number or `"max"`) |
| `paused` | Whether the simulation is paused |
| `actual_speed` | Virtual time advanced ÷ real time, over roughly the last second (3 decimal places) |
| `late_steps` | Number of late time steps so far |
| `max_late_ms` | Largest lateness so far |
| `slipped_ms` | Total slip so far |
| `late_ms` | Current lateness of a time step that is past its deadline and has not started yet (0 if there is none) |

- `actual_speed` is `null` when the simulation has run only for a short time, and while it is paused.
- `late_steps`, `max_late_ms` and `slipped_ms` have the same meaning as in the result's `timing`. They are the values the result would have if the run ended now.
- While the simulation is stalled (virtual time does not advance and `late_ms` grows beyond the tolerance), `time` is still written, at most once every 100 ms.

An event with `paused: true` is normally written after the simulation has paused. The first `time` of `--start-paused` is also written after the simulation has paused at time 0, so the result's `paused_ms` counts from before this event. There are 2 exceptions. In both, `true` is written while the simulation has reached the time at which it is to pause but has not paused yet, and `paused_ms` counts from when it actually pauses:

- The simulation has not paused at time 0 within 2 seconds after the start (the launcher stops waiting and writes the event).
- A `pause` or `run_until` names a time at or before the current time, and the simulation pauses at the next time step.

`command`:

| Key | Value |
|---|---|
| `command` | Name of the command (`null` if it cannot be read) |
| `source` | `"stdin"` (arrived as a line), `"eof"` (end of stdin), `"control_server"` (a client call) |
| `accepted` | Whether the command was accepted |
| `message` | Why the command was not accepted (`null` if it was accepted) |
| `ms` | Only for an accepted `step`: the virtual time the simulation actually advances (rounded up to a multiple of the time step) |
| `at_ms` | Only for an accepted `run_until` and an accepted `pause` with a time: the virtual time at which the simulation pauses (rounded up to a multiple of the time step) |

`command` is written when a command arrives on stdin (`--stdin-commands`) and when a Control Server client calls a time operation or a stop (`--control`). `run_until` and `pause` with a time come only from clients.

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
| `log` of a participant | After that participant's `started` |
| `participant` with `joined` | Only for participants that join through the SIL Kit lifecycle |
| `control_link` | After `joined` of all required participants and before `phase` `running`, in configuration file order |
| `participant` with `failed` | Immediately when the failure is decided |
| `command` | Before what the command causes (such as `phase` `stopping`) |
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
| The simulation pauses or resumes, or the speed changes | Immediately. |
| The time changes while the simulation stays paused (after a step) | Immediately. |
| The simulation ends | The state at that moment, whatever the interval (an ended simulation is not paused). |

The first `time` is at time 0. Its `paused` is `true` only when the simulation is paused (or is to pause) at time 0. If the simulation is already past 0 when the event is written, `paused` is `false`, even if the simulation is paused at a later time. This happens, for example, after a `step` or `run_until` that was accepted before the start. If the simulation is still paused afterwards, the next `time` (with `paused` `true`) carries the time at which it paused. If another command advances the simulation first, or the simulation ends, that `time` is not written.

`sim_ms` never decreases (it can keep the previous value across a pause or a speed change). `vehicle_sil_launch` reads the state every 20 ms, so it may not write changes shorter than that (for example, in the middle of a single-step at maximum speed). The last state is always written.

`finding` is a run finding; the check's findings are inside `check`. When the simulation ends (or does not start), the launcher writes the findings decided at that point, and writes the rest after it has ended the participants. A `finding` once written appears unchanged in the `findings` of `result`. The same finding is never written twice. Reading `result` alone gives the full result.

For a participant that failed during the simulation, `participant-failed` includes how the process ended, so it is written after the process has ended. The `participant` event with `failed` reports the failure as soon as the launcher notices it. The `log` lines of `vehicle_sil_launch` also give the reason.

### Events when the run does not start

`run` with `--events` writes `hello` first and `result` last even on a usage error (`heartbeat` can come in between).

| Case | Events | `result` of `result` | Exit code |
|---|---|---|---|
| Usage error (unknown option, no configuration file given, `--events` together with `--json`, and so on) | `hello`, `result` | `"usage"` | 4 |
| Usage error found by reading the configuration file | `hello`, `phase` (`checking`), `check`, `phase` (`ended`), `result` | `"usage"` | 4 |
| The configuration file cannot be read | The same 5 | `"unreadable"` | 3 |
| The check found errors | The same 5 | `"check_failed"` | 1 |
| No stdout (started with stdout closed) | Nothing can be written (an error is written to stderr). | | 4 |

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

### Slow readers and readers that stop reading

`vehicle_sil_launch` does not wait for the reader of the events. A dedicated thread writes the events. The thread that drives the run, the threads that read the participants' output and the thread that reads stdin only add events to a queue. Whether the reader is slow or has stopped, the following holds:

- The participants do not stop (the launcher keeps reading their output). The `--log-dir` files are written as usual.
- `--timeout-ms`, signals and the `stop` command on stdin work as usual.

When events pile up because they cannot be written:

| Event | Handling |
|---|---|
| `log` | While more than 4096 lines or 1 MiB of `log` are waiting, new ones are counted instead of written. |
| `time` that only reports that virtual time advanced | Not created while other events are waiting to be written. |
| `time` that reports a pause or a speed change | Created. |
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

If `result` cannot be written, the exit code is 3 and stderr gets `error: cannot write the result to the standard output (<reason>)`. A run stopped by a stop request exits with 130 and does not write this line.

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

`event` values, keys, and the values of `state` and `phase` can be added without a version change. When the meaning of a key changes or a key is removed, `vehicle_sil_events` goes up.

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
{"event":"hello","seq":0,"t_ms":<t_ms>,"vehicle_sil_events":1,"vehicle_sil":"<version>","pid":<pid>,"file":"simulation.yaml","arguments":["run","simulation.yaml","--events","--disable","Dashboard"]}
{"event":"phase","seq":1,"t_ms":<t_ms>,"phase":"checking"}
{"event":"check","seq":2,"t_ms":<t_ms>,"check":{"vehicle_sil_check":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"stop_at_ms":1000,"start_timeout_ms":30000,"stop_timeout_ms":10000,"registry_uri":null,"participants":[{"name":"Engine","line":11,"command":["engine/vehicle_sil_engine_ecu"],"describe":true,"required":true,"enabled":true,"manual":false,"role":"ecu","step_ms":10,"status":"described","reported":{"vehicle_sil":"<version>","name":"Engine","step_ns":10000000,"networks":[{"name":"J1939_CAN","kind":"can","use":"j1939","nodes":[{"address":0,"j1939_name":"0x00000000FE000001","arbitrary_address_capable":false}],"mac":null}]},"variables":[{"name":"ecu.connected","type":"bool","causality":"input","unit":"","min":null,"max":null,"step":null,"description":"on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on","items":[],"builtin":true},{"name":"engine.throttle","type":"real","causality":"input","unit":"%","min":0,"max":100,"step":null,"description":"accelerator pedal position","items":[],"builtin":false},{"name":"engine.speed","type":"integer","causality":"output","unit":"rpm","min":null,"max":null,"step":null,"description":"","items":[],"builtin":false}],"control":{"transport":"silkit","wire_version":1}},{"name":"Dashboard","line":14,"command":["dashboard/vehicle_sil_dashboard_ecu"],"describe":true,"required":true,"enabled":false,"manual":false,"role":"ecu","step_ms":5,"status":"disabled","reported":null,"variables":null,"control":null}],"networks":[{"name":"J1939_CAN","kind":"can","line":8,"participants":["Engine"]}],"findings":[{"severity":"warning","code":"single-participant","file":"simulation.yaml","line":8,"participant":null,"network":"J1939_CAN","message":"network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it","detail":null},{"severity":"note","code":"participant-disabled","file":"simulation.yaml","line":14,"participant":"Dashboard","network":null,"message":"participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it","detail":null}]}}
{"event":"participant","seq":3,"t_ms":<t_ms>,"name":"Dashboard","state":"disabled"}
{"event":"phase","seq":4,"t_ms":<t_ms>,"phase":"starting"}
{"event":"registry","seq":5,"t_ms":<t_ms>,"uri":"silkit://127.0.0.1:<port>"}
{"event":"log","seq":6,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"registry at silkit://127.0.0.1:<port>"}
{"event":"log","seq":7,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"starting Engine: <dir>/engine/vehicle_sil_engine_ecu --name Engine --registry-uri silkit://127.0.0.1:<port> --start-timeout-ms 50000"}
{"event":"participant","seq":8,"t_ms":<t_ms>,"name":"Engine","state":"started","pid":<pid>,"command":["<dir>/engine/vehicle_sil_engine_ecu","--name","Engine","--registry-uri","silkit://127.0.0.1:<port>","--start-timeout-ms","50000"],"log":null}
{"event":"log","seq":9,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"not starting Dashboard: disabled"}
{"event":"phase","seq":10,"t_ms":<t_ms>,"phase":"waiting"}
{"event":"participant","seq":11,"t_ms":<t_ms>,"name":"Engine","state":"joined"}
{"event":"control_link","seq":12,"t_ms":<t_ms>,"name":"Engine","state":"connected","variables":18}
{"event":"phase","seq":13,"t_ms":<t_ms>,"phase":"running"}
{"event":"log","seq":14,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"simulation running"}
{"event":"time","seq":15,"t_ms":<t_ms>,"sim_ms":0,"speed":"max","paused":false,"actual_speed":<actual_speed>,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0}
{"event":"time","seq":16,"t_ms":<t_ms>,"sim_ms":1000,"speed":"max","paused":false,"actual_speed":<actual_speed>,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0}
{"event":"log","seq":17,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"the simulation ended"}
{"event":"phase","seq":18,"t_ms":<t_ms>,"phase":"stopping"}
{"event":"log","seq":19,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"Engine exited with code 0"}
{"event":"participant","seq":20,"t_ms":<t_ms>,"name":"Engine","state":"ended","exit_code":0,"signal":null}
{"event":"phase","seq":21,"t_ms":<t_ms>,"phase":"ended"}
{"event":"result","seq":22,"t_ms":<t_ms>,"result":{"vehicle_sil_run":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"simulation":"reached_stop_at","interrupted_by":null,"timing":{"speed":"max","time_step_ms":10,"ecu_step_ms":10,"step_delay_ms":0,"late_tolerance_ms":10,"sim_ms":1000,"wall_ms":<wall_ms>,"actual_speed":<actual_speed>,"paced_steps":0,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0,"paused_ms":0},"participants":[{"name":"Engine","required":true,"enabled":true,"manual":false,"ended":"exited","exit_code":0,"signal":null,"left_processes":false,"log":null,"control_link":{"state":"connected","variables":18,"version":null,"late_messages":0}},{"name":"Dashboard","required":true,"enabled":false,"manual":false,"ended":"not_started","exit_code":null,"signal":null,"left_processes":false,"log":null,"control_link":{"state":"none","variables":null,"version":null,"late_messages":0}}],"traffic":{"before_ms":1000,"unplaced":[],"networks":[{"name":"J1939_CAN","kind":"can","counted":1,"senders":1,"receivers":0,"delivered":false,"participants":[{"name":"Engine","counted":true,"not_counted":null,"virtual_time":true,"at_ms":990,"frames_sent":11,"frames_received":0}]}]},"findings":[],"check":{"vehicle_sil_check":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"stop_at_ms":1000,"start_timeout_ms":30000,"stop_timeout_ms":10000,"registry_uri":null,"participants":[{"name":"Engine","line":11,"command":["engine/vehicle_sil_engine_ecu"],"describe":true,"required":true,"enabled":true,"manual":false,"role":"ecu","step_ms":10,"status":"described","reported":{"vehicle_sil":"<version>","name":"Engine","step_ns":10000000,"networks":[{"name":"J1939_CAN","kind":"can","use":"j1939","nodes":[{"address":0,"j1939_name":"0x00000000FE000001","arbitrary_address_capable":false}],"mac":null}]},"variables":[{"name":"ecu.connected","type":"bool","causality":"input","unit":"","min":null,"max":null,"step":null,"description":"on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on","items":[],"builtin":true},{"name":"engine.throttle","type":"real","causality":"input","unit":"%","min":0,"max":100,"step":null,"description":"accelerator pedal position","items":[],"builtin":false},{"name":"engine.speed","type":"integer","causality":"output","unit":"rpm","min":null,"max":null,"step":null,"description":"","items":[],"builtin":false}],"control":{"transport":"silkit","wire_version":1}},{"name":"Dashboard","line":14,"command":["dashboard/vehicle_sil_dashboard_ecu"],"describe":true,"required":true,"enabled":false,"manual":false,"role":"ecu","step_ms":5,"status":"disabled","reported":null,"variables":null,"control":null}],"networks":[{"name":"J1939_CAN","kind":"can","line":8,"participants":["Engine"]}],"findings":[{"severity":"warning","code":"single-participant","file":"simulation.yaml","line":8,"participant":null,"network":"J1939_CAN","message":"network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it","detail":null},{"severity":"note","code":"participant-disabled","file":"simulation.yaml","line":14,"participant":"Dashboard","network":null,"message":"participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it","detail":null}]}}}
```

The CTest test `launch_run.events` compares this example with the real output byte for byte. Before the comparison, it removes the `heartbeat` lines and every `time` line other than the first and the last from the real output (renumbering `seq`), and makes the replacements above. It removes those `time` lines because on a slow computer the launcher also writes `time` events for intermediate times.

## Stdin commands (`run --stdin-commands`)

The launcher reads stdin only with `--stdin-commands`. Without it, stdin is not read (a closed or empty stdin does not stop the run). Each line is one JSON object. Use it together with `--events` (see [Reader requirements](#reader-requirements)); it also works on its own.

| Line | Meaning |
|---|---|
| `{"command":"stop"}` | Stops in order (the same as the first Ctrl+C). |
| `{"command":"pause"}` | Pauses. |
| `{"command":"resume"}` | Ends the pause and continues. |
| `{"command":"step","ms":<ms>}` | Only while paused: advances virtual time by `<ms>` (an integer from 1 to 10^12) and pauses again. |
| `{"command":"speed","value":<speed>}` | Changes the speed (`<speed>` is a number or `"max"`, in the same range as `speed` in the configuration file). |
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
| `step` | Sent while a previous `step` is in progress, it counts from the time at which that step pauses. |
| `speed` | A pause stays in effect. |
| EOF | If a stop request (`stop`, a signal, a closed stdout) has already arrived, does nothing (it does not count as a second one and no `command` event is written). |
| Anything else | This covers a line that is not a JSON object, has no `command`, has an unknown command, or is longer than 64 KiB. |
| Anything else | With `--events`, a `command` event (`accepted: false` and the reason) is written; otherwise one line goes to stderr. |

Other keys of the object are ignored. Commands are accepted and carried out even when nobody reads the events.

### When time commands are accepted

For a time command (`pause`, `resume`, `step`, `speed`), the thread that reads stdin decides whether to accept it, writes the `command` event, and then hands the command to the controller. None of this blocks: neither the thread that drives the run nor the reader of the events waits. The controller applies the command at the next time step boundary.

| Case | Handling |
|---|---|
| `step` sent while not paused (after a `resume`, for example) | Not accepted. |
| `step` sent before the controller exists (during the check, for example) | Not accepted. |
| A time command after the simulation has ended (or did not start) | Not accepted. |
| `pause` while paused, `resume` while running | Accepted; does nothing. |
| `pause`, `resume` or `speed` received before the controller exists (during the check, for example) | Kept, and handed to the controller when it exists. |

The reason for not accepting goes into `message`. A `pause` received before the controller exists pauses at 0, as `--start-paused` does.

A pause leaves `phase` at `running`. The `paused` of `time` reports the pause.

In a run with `--control` and `--start-paused` that pauses before the step at time 0, a `resume`, `step` or `pause` accepted before the pause also decides how the run pauses afterwards (see "Write before the start (`--start-paused`)" in [control-api.md](control-api.md#write-before-the-start---start-paused)).

### What `accepted` means, and the result of a stopped run

`accepted: true` in a `command` event means that the line is a valid command and was handed on for processing (for a time command, also that it can be carried out now). It does not mean that the command changed the run. For example, a `stop` received after the run has ended (after `phase` `ended`) has `accepted: true` but does not change the result.

A `command` event is written before what the command causes (such as `phase` `stopping`).

A run stopped by a command has the same result as one stopped by Ctrl+C (`result` is `"interrupted"`, exit code 130, the note `interrupted`).

| Stopped by | `interrupted_by` |
|---|---|
| `{"command":"stop"}` | `"stop command"` |
| End of stdin | `"end of input"` |
| `Stop` from a Control Server client | `"control server stop"` |

### Commands from Control Server clients

The time operations (`Pause`, `Resume`, `Step`, `RunUntil`, `SetSpeed`) and `Stop` of Control Server clients (`--control`) are processed as the same commands (`source` is `"control_server"`). For the rules and the order, see "Stdin commands and client commands" in [control-api.md](control-api.md#stdin-commands-and-client-commands).

| Item | Stdin | Client |
|---|---|---|
| Without `--stdin-commands` | Not read. | Accepted. |
| `run_until` and `pause` with a time | Cannot be sent. | Can be sent. |
| A stop after the simulation has ended | `stop` is accepted and does nothing. | `Stop` is rejected. |

## Version and features (`vehicle_sil_launch version`)

`vehicle_sil_launch version` (or `--version`) writes one line, `vehicle_sil_launch <version>`. `version --json` writes the versions of the formats and the list of features. Neither reads a file, and the exit code is 0.

<!-- vehicle_sil_launch version --json -->
```json
{
  "vehicle_sil_launch": 1,
  "vehicle_sil": "<version>",
  "formats": {
    "check": 1,
    "run": 1,
    "events": 1,
    "describe": 1
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
    "network-names",
    "run-traffic"
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
| `network-names` | `network_names` in the configuration file (see "Matching network names (`network_names`)" in [simulation-config.md](simulation-config.md#matching-network-names-network_names)) |
| `run-traffic` | `traffic` in the run result: the J1939 traffic on each network and `delivered` (see "Network traffic (`traffic:`)" in [simulation-run.md](simulation-run.md#network-traffic-traffic)) |
| `control-server` | `run --control` (only in builds with the Control Server) |
| `scenario-control` | `run --scenario` and `--control` can be used together (only in builds with the Control Server) |

Each name indicates that the following are present:

| Name in `features` | Includes |
|---|---|
| `speed` | `speed`, `time_step_ms` and `late_tolerance_ms` in the configuration file, `run --speed`, `timing` in the result, the findings `behind-real-time` and `coarse-time-step`, and `speed`, `paused`, `actual_speed`, `late_steps`, `max_late_ms`, `slipped_ms` and `late_ms` in the `time` event |
| `control-links` | The event `control_link`, `participants[].control_link` in the result, `participants[].control` in the check JSON, the findings `control-link-no-hello`, `control-link-version`, `control-link-late-values`, `control-link-dropped` and `control-link-not-offered` |
| `manual-participants` | `manual` in the configuration file, `--manual`, `participants[].manual` in the check and result JSON, the finding `participant-manual`, and the `state` `manual` of the event `participant` |
| `scenario` | `scenario` in the result and check JSON, the event `scenario_step`, the findings `scenario-*`, and a finding's `file` being the scenario file |
| `scenario-wave` | `undersampled` of that step, the findings `scenario-wave-*`, and `"wave"` as the `kind` of a step in the result and in events |
| `scenario-motion-expect` | `expect_skipped` of a step in the result and in events |
| `network-names` | `--map-network` passed to executables, `participants[].network_map` in the check JSON, and the findings `network-map-*` |
| `run-traffic` | The `traffic:` line and table of the human-readable result, and `traffic` in `run --json` and in the event `result` |
| `control-server` | The event `control_server`, `"control_server"` as the `source` of `command` and its `at_ms`, `--control-credentials`, `--control-tls-cert`, `--control-tls-key`, `--control-audit-log`, `"control server stop"` as `interrupted_by`, and pausing before the step at time 0 with `--start-paused` |

In a build with `scenario-control`, clients can only read, wait and stop while a scenario runs. Calls that write and calls that move time are rejected with `FAILED_PRECONDITION` (reason `scenario-running`) (see "Watching variables during a run (`--control`)" in [scenario.md](scenario.md#watching-variables-during-a-run---control)).

The example above is the output of a build without the Control Server. A build with it adds `"control-server"` and `"scenario-control"` at the end of `features`. The CTest test `launch_cli.docs` also checks this example (byte for byte, except the `vehicle_sil` version and these 2 names).

## See also

- How a run proceeds, how to stop it, and the result JSON → [simulation-run.md](simulation-run.md)
- The check JSON → [simulation-check.md](simulation-check.md)
- Reading and writing variables and controlling time over gRPC → [control-api.md](control-api.md)
- The browser GUI and its development → [gui.md](gui.md)

The tests of the browser GUI replay recordings of this output (`gui/server/tests/recordings/`). If you change the launcher's output, compare with `python3 gui/server/tests/record_launcher.py --launcher <vehicle_sil_launch> --compare` and record the scenes that differ again (see "Development" in [gui.md](gui.md#development)).
