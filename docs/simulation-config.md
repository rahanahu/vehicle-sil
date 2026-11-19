# Simulation configuration file (`vehicle_sil_launch`)

[日本語](ja/simulation-config.md)

This is the reference for the configuration file that you pass to `vehicle_sil_launch`. It covers the keys and syntax rules of the file, and the arguments and exit codes of `check`, `run`, `test` and `version`. The format and the rules may still change.

The configuration file lists the executables that become participants, the networks they connect to and the time at which the simulation stops. Use it to run ECU executables that were built separately ([ecu-executable.md](ecu-executable.md)) in one simulation.

```sh
vehicle_sil_launch check simulation.yaml
vehicle_sil_launch run simulation.yaml
```

| To find | See |
|---|---|
| What `check` and `run` start from the configuration file | [What `check` and `run` start](#what-check-and-run-start) |
| The list of keys and their defaults | [Keys](#keys) |
| How to write `command` | [Executable paths](#executable-paths), [Braces and placeholders](#braces-and-placeholders) |
| Running without one of the ECUs | [Disabling a participant](#disabling-a-participant-enabled---disable---enable) |
| Running an ECU from a debugger | [Starting a participant yourself](#starting-a-participant-yourself-manual---manual) |
| Aligning variable names that differ between executables | [Matching variable names and values](#matching-variable-names-and-values-variables) |
| Aligning network names that differ between executables | [Matching network names](#matching-network-names-network_names) |
| Putting 2 copies of one ECU on a network (other J1939 addresses and NAMEs) | [Matching J1939 addresses and NAMEs](#matching-j1939-addresses-and-names-j1939_nodes) |
| Changing executables or values per variant | [Variants](#variants-variants---variant), [Scenario parameters](#scenario-parameters-params---param) |
| Arguments and exit codes | [Arguments and exit codes](#arguments-and-exit-codes) |

## What `check` and `run` start

| Command | What it does |
|---|---|
| `vehicle_sil_launch check` | Checks the configuration file against the `--describe` output of each executable and reports every mismatch at once ([simulation-check.md](simulation-check.md)). |
| `vehicle_sil_launch run` | Runs the same check, then starts the whole simulation, monitors it to the end and shows how each participant ended ([simulation-run.md](simulation-run.md)). |

`check` does not start the simulation and does not connect to the registry. It does start each `describe: true` executable, one at a time, with `--describe`. `--describe` constructs the ECU, so the code of that executable (the ECU's constructor and so on) runs. `check` does not start `describe: false` executables; it only confirms that they exist.

`run` starts the participants only if the check passes.

> [!WARNING]
> A configuration file is a list of commands to execute. Do not pass an untrusted configuration file to `check` or to `run`.

`vehicle_sil_launch` is built only when SIL Kit is built from the submodule, because it reads YAML with rapidyaml, which comes with SIL Kit.

## The configuration file

### File format

| Item | Rule |
|---|---|
| Format | YAML. |
| JSON | If the first character that is not white space is `{`, the file is read as JSON (with the same content). |
| Encoding | UTF-8 (a leading BOM is skipped). |
| Unreadable encodings | UTF-16 files and files that contain NUL cannot be read. |
| Configurations per file | One (a second document after `---` is an error). |
| Anchors and aliases (`&name`, `*name`, `<<`) | Not supported; write the values out in full (the file is reported as unreadable). |
| Size | Files larger than 16 MiB are not read. |
| File type | Anything that is not a regular file (a device or a pipe) is not read. The exception is `check --text-from-stdin`, which reads the text from stdin under the other rules of this table (see [Arguments](#arguments)). |

Example: [examples/ecu_main/simulation.yaml](../examples/ecu_main/simulation.yaml). CMake copies it next to the executables, to `build/<preset>/examples/ecu_main/`.

<!-- examples/ecu_main/simulation.yaml -->
```yaml
# yaml-language-server: $schema=../../schema/simulation.schema.json
# The two ECUs of this directory in one simulation. CMake copies this file next to the executables
# (build/<preset>/examples/ecu_main/); there, vehicle_sil_launch check simulation.yaml checks it and
# vehicle_sil_launch run simulation.yaml runs it.
vehicle_sil_simulation: 1
stop_at_ms: 1000
networks:
  - name: J1939_CAN
    kind: can
participants:
  - name: Engine
    command: [engine/vehicle_sil_engine_ecu]
    step_ms: 10
  - name: Dashboard
    command: [dashboard/vehicle_sil_dashboard_ecu]
    step_ms: 5
```

### Keys

Top-level keys:

| Key | Required | Value | If omitted |
|---|---|---|---|
| `vehicle_sil_simulation` | Yes | Format version (currently `1`) | |
| `stop_at_ms` | | Virtual time at which the simulation stops (ms, an integer from 1 to 10^12) | The simulation runs until a participant stops it or you stop `run` with Ctrl+C |
| `start_timeout_ms` | | How long to wait for the required participants to join (ms, an integer from 1 to 10^12) | 30000 |
| `stop_timeout_ms` | | How long `run` waits for participants to end (ms, an integer from 1 to 10^12) | 10000 |
| `registry_uri` | | Where `run` starts the registry (`silkit://<host>:<port>`, port from 0 to 65535; 0: a free port) | A free port on 127.0.0.1 |
| `speed` | | Speed at which virtual time advances (a number from 0.000001 to 1000000, or `max`) | As fast as possible (same as `max`) |
| `time_step_ms` | | The controller's time step (ms of virtual time, an integer from 1 to 10^12) | 10 when running as fast as possible |
| `late_tolerance_ms` | | Tolerance for lateness (ms of real time, an integer from 1 to 10^12) | 10 (independent of the time step and the speed) |
| `networks` | Yes | Array of networks (may be empty) | |
| `participants` | Yes | Array of participants (at least one) | |
| `variants` | | Variants (variant name → its differences from the common part; see [Variants](#variants-variants---variant)) | The configuration file has no variants |
| `params` | | Values of scenario parameters (name → value; see [Scenario parameters](#scenario-parameters-params---param)) | |

Keys of each element of `networks` (list every network that the ECUs use):

| Key | Required | Value |
|---|---|---|
| `networks[].name` | Yes | Name of the network (the name passed to the ECU's `j1939()`, `can()`, `lin_master()`, `lin_slave()` or `ethernet()`; if an executable calls it by another name, write that in the participant's `network_names`) |
| `networks[].kind` | Yes | One of `can`, `lin` and `ethernet` |
| `networks[].external` | | How the network behaves when it is connected outside to a real bus (`can`: `bitrate`; `ethernet`: `ethertypes`, `all_frames`). See [Connecting a network outside](#connecting-a-network-outside-external---connect) |

Keys of each element of `participants`:

| Key | Required | Value | Default |
|---|---|---|---|
| `participants[].name` | Yes | Participant name | |
| `participants[].command` | Yes | Array of the executable and its arguments | |
| `participants[].step_ms` | | Step size that the executable reports in its `--describe` output (ms, an integer from 1 to 10^12) | Not checked |
| `participants[].describe` | | `true`: an ecu_main executable (checked); `false`: any other executable (not checked) | `true` |
| `participants[].required` | | Whether the simulation waits for this participant (`true` or `false`) | `true` |
| `participants[].enabled` | | `false` disables the participant: it is neither checked nor started (see [Disabling a participant](#disabling-a-participant-enabled---disable---enable)) | `true` |
| `participants[].manual` | | `true`: `run` does not start the participant, and you start it yourself (see [Starting a participant yourself](#starting-a-participant-yourself-manual---manual)) | `false` |
| `participants[].role` | | What the participant is for (`ecu`, `test` or `bridge`; see [Test participants and bridges](#test-participants-and-bridges-role)) | `ecu` |
| `participants[].variables` | | Variable map (see [Matching variable names and values](#matching-variable-names-and-values-variables)) | Variables appear under the executable's names |
| `participants[].network_names` | | Network name map, from the simulation's names (the names in `networks`) to the names this executable uses in its code (see [Matching network names](#matching-network-names-network_names)) | Networks keep the executable's names |
| `participants[].j1939_nodes` | | Other addresses and NAME fields for the J1939 nodes of this executable, one entry for each node (see [Matching J1939 addresses and NAMEs](#matching-j1939-addresses-and-names-j1939_nodes)) | Nodes keep the values in the executable's code |

Notes on individual keys:

**`start_timeout_ms`**: `run` also passes a value to ecu_main executables with `--start-timeout-ms`. For the value it passes, see [ecu_main executables](#ecu_main-executables-describe-true).

**`stop_timeout_ms`**: `run` waits this long at each stage of stopping (see "Stopping a run" in [simulation-run.md](simulation-run.md#stopping-a-run)).

**`registry_uri`**

| Case | Where the registry is, and what happens |
|---|---|
| Omitted | A free port on 127.0.0.1. |
| Omitted, and a required participant is one that you start yourself | `silkit://127.0.0.1:8500` (see [Starting a participant yourself](#starting-a-participant-yourself-manual---manual)). |
| Port 0 (`silkit://127.0.0.1:0`, `silkit://0.0.0.0:0`) | A free port, chosen when the run starts. The `registry at` and `join at` lines, the commands of the participants (also the one printed for a participant that you start yourself) and the `registry` event of `--events` have that port, and so has `registry_uri` of the `run --json` result; `registry_uri` in the JSON of the check is as written. The port changes from run to run, and so does the command for a participant that you start yourself (manual) or that runs on another computer: for a debugger configuration you reuse, or for another computer, write a fixed port. |
| Participants on other computers join too | Write an address such as `silkit://0.0.0.0:8500`. Participants on this computer get a URI with `127.0.0.1`. |
| Not a loopback address (`127.x.x.x`, `[::1]`, `localhost`) | The check reports the note `registry-on-network`, and `run` passes the environment variable `VEHICLE_SIL_LISTEN_ON_NETWORK=1` to the participants it starts (see "Listen addresses" in [simulation-run.md](simulation-run.md#listen-addresses)). |

For the steps to let participants join from other computers, see the guide [Run on several computers](howto/multi-pc.md).

**`speed`**: Seconds of virtual time per second of real time. `1` is real-time speed, `2` is twice as fast and `0.5` is half speed. `run --speed` overrides it (see "Running at a set speed (`speed`)" in [simulation-run.md](simulation-run.md#running-at-a-set-speed-speed)).

**`time_step_ms`**: The granularity of pausing and stepping, and of keeping to the set speed.

| Case | Value of `time_step_ms` |
|---|---|
| Omitted, running as fast as possible | 10. |
| Omitted, running as fast as possible, and `stop_at_ms` is not a multiple of 10 | The greatest common divisor of `stop_at_ms` and 10. |
| Omitted, with a set speed | As above, further reduced to the greatest common divisor with the step sizes of the ECUs (see "Time steps and ECU steps" in [simulation-run.md](simulation-run.md#time-steps-and-ecu-steps)). |
| Written | Must divide `stop_at_ms` (otherwise the error `value`). |

**`late_tolerance_ms`**: A time step that is late by more than this value counts as a late time step.

**`participants[].name`**: The name must not be empty, must fit on one line (no control characters) and must not start with `-`. `SimulationController` (the participant name of vehicle-sil's controller) is not allowed. For the check, the name is passed to ecu_main executables with `--name`.

**`participants[].command`**: If there are no arguments, a single string is enough. The string is not split at spaces.

**`participants[].step_ms`**: Checked only if written.

**`participants[].required`**: `false` is allowed only for `describe: false` participants. At least one enabled participant must be `true` (otherwise the error `no-required-participant`).

**`participants[].role`**: Only the check reads it. It does not change how the run behaves.

**`variants`**: A configuration file with variants needs `--variant <name>` for `check` and `run`.

**`params`**: The values are substituted for `${name}` in the scenario.

### Syntax rules

| Item | Rule |
|---|---|
| Unknown key | An error. If a key with a close name exists, the message suggests it (typos are never silently ignored). |
| A key written twice | An error. |
| Integers | Write them without quotes (`"5000"` is a string, so it is an error). |
| Booleans | `true` or `false` without quotes (`True`, `TRUE`, `False` and `FALSE` are also read). |
| `yes`, `on` and similar | An error where a boolean is expected. |

`stop_at_ms`, `start_timeout_ms`, `stop_timeout_ms`, `registry_uri`, `speed`, `time_step_ms` and `late_tolerance_ms` are values for running the simulation. `check` checks only their form (and the combination of `stop_at_ms` and `time_step_ms`).

### Editor completion

The JSON Schema is [schema/simulation.schema.json](../schema/simulation.schema.json). In an editor that uses the YAML language server, write `# yaml-language-server: $schema=<path to this file>` at the top of the file to get key completion, descriptions and shape checks. Line 1 of the example above is this line. The path is relative to the directory of the file you are editing.

| Item | Details |
|---|---|
| What the schema checks | Only the keys and the shape of the values. |
| What `check` checks | Whether the file matches the executables, keys written twice and combinations of keys. |
| A file written in JSON (first character `{`) | JSON has no comments, so associate the file with the schema in the editor's settings. |
| VS Code settings | In `json.schemas`, write a file name pattern and the path of the schema. |
| The `"$schema"` key | Cannot be written in the file (the launcher reports it as an unknown key). |
| Schema in the template | `schema/` holds a copy. `get_tools.py` overwrites it with the schema of the version of the tools it downloads. |
| Schema in the tools archive | In `schema/`. |

### Executable paths

The first element of `command` is the executable.

| Form | Handling |
|---|---|
| Relative path | Relative to the directory of the configuration file, not to the directory where you ran the command. |
| Path that contains `..` | Not normalized (a `..` across a symbolic link is resolved as the OS resolves it). |
| A name without a separator (`/`, `\`) that is not in the directory of the configuration file (such as `python3`) | Searched for on `PATH` (relative directories in `PATH` are not used). |
| A file in the directory of the configuration file that cannot be executed (no execute permission) | Not used; `PATH` is searched instead. |
| Separators | `/` works on Windows too. |
| Paths with `\` | Write them in YAML single quotes (`'C:\ecu\engine.exe'`). |
| Paths with spaces or non-ASCII characters | Allowed. |
| On Windows, there is no file at the path and the path does not end in `.exe` | If a file with `.exe` added exists, it is used (so the same configuration file works on Linux and Windows). |
| Batch files (`.bat`, `.cmd`) | An error. Write the executable that the batch file starts. |

When the executable is found on `PATH`, the check reports its location in the note `found-on-path`. The location is a path on that computer, so this note differs from computer to computer. Batch files are an error because they run through cmd.exe, which has different rules for quoting arguments.

### Braces and placeholders

In `command`, only `{word}` (letters, digits and `_` between `{` and `}`) is special. Other braces are passed as they are: the JSON argument `'{"a":{"b":1}}'` does not change by a single byte.

| What you write | Handling |
|---|---|
| `{registry_uri}` in an argument after the executable, in a `describe: false` `command` | Replaced with the URI of the simulation's registry. |
| `{name}` in an argument after the executable, in a `describe: false` `command` | Replaced with the participant name. |
| `{registry_uri}` and `{name}` in a `describe: true` `command` | An error. |
| An unknown `{word}` | An error in either kind of `command`. |
| Any `{word}` in the executable path | An error. |
| `{{word}}` | Passes the literal string `{word}` (`{{name}}` becomes `{name}`). |
| A `describe: false` `command` without `{registry_uri}` | The check reports a note, because the executable may not receive the location of the registry. |

- A placeholder can be part of a longer argument (for example `--uri={registry_uri}`).
- An unknown `{word}` is an error so that typos are caught.
- `{{word}}` works the same way in `describe: true` arguments and in the executable path.
- In YAML, put `{...}` in quotes (`"{registry_uri}"`).
- Without quotes, YAML reads it as a mapping, which is an error (the finding message says how to fix it).
- Also quote a value that starts with `*` or `&`. Without quotes, it is an alias or an anchor.

### ecu_main executables (`describe: true`)

`vehicle_sil_launch` starts the executable with the arguments of `command`, followed by the arguments below. The arguments of `command` are passed with each `{{word}}` turned back into `{word}`.

| Start | Arguments added |
|---|---|
| The check | `--describe --name <participant name>` |
| `run` | `--name <participant name> --registry-uri <URI> --start-timeout-ms <ms>` |

With `network_names`, `run` then adds `--map-network <simulation name>=<executable name>` for each line, in the order written, and with `j1939_nodes`, `--j1939-node <network>=<settings>` for each entry that changes something (the check's `--describe` gets none of them).

`<ms>` is `start_timeout_ms + stop_timeout_ms + 10000`. It is the launcher that waits for the start and names the participants that did not join; the limit on the ECU side covers the case where the launcher has gone away. In the following 2 kinds of run, the ECU's wait for the start has no limit, so `<ms>` is 10^12 (`1000000000000`):

- A run with a required participant that you start yourself.
- A run with `--control` and `--start-paused`, which pauses before the step at time 0.

In these 2 kinds of run too, the launcher uses `start_timeout_ms` to detect a participant that does not start. In a run with a manual participant, it counts per phase, as the table in "[How `run` waits](#how-run-waits)" below shows. In a run that pauses before time 0, it counts from when the launcher starts waiting for participants until the pause, stops counting during the pause, and counts a fresh `start_timeout_ms` from when the run continues.

What you can and cannot write in `command`:

| What you write | Handling |
|---|---|
| Arguments of `ecu_main` (`--describe`, `--name`, `--registry-uri`, `--local`, `--run-for-ms`, `--start-timeout-ms`, `--map-network`, `--j1939-node`, `--help`, `-h`, also in the form `--name=X`) | Not allowed (the error `launcher-argument`). Write `--map-network` in `network_names` (see [Matching network names](#matching-network-names-network_names)) and `--j1939-node` in `j1939_nodes` (see [Matching J1939 addresses and NAMEs](#matching-j1939-addresses-and-names-j1939_nodes)). |
| `--no-control` | Allowed. The ECU does not expose its variables. |
| `--silkit-config <file>` | Allowed. Passes a SIL Kit configuration file to the ECU. |
| `required: false` | Not allowed (the error `not-required-ecu`), except with `role: bridge`. |

**`--no-control`**: The `--describe` of the check gets the same argument, so `control` in the `--describe` output is `null`, and `run` does not wait for the ECU's hello (see "Connection to ECU variables (`control_link`)" in [simulation-run.md](simulation-run.md#connection-to-ecu-variables-control_link)).

**`--silkit-config <file>`**: Write it as `command: [engine/vehicle_sil_engine_ecu, --silkit-config, engine.silkit.yaml]`. A relative path is relative to the directory of the configuration file, which is the participant's working directory. The `--describe` of the check gets the argument too, so the check reports errors like these as `describe-failed` (see "Passing a SIL Kit configuration file (`--silkit-config`)" in [ecu-executable.md](ecu-executable.md#passing-a-sil-kit-configuration-file---silkit-config)):

- The file does not exist.
- SIL Kit does not accept it.
- It has a `ParticipantName` that differs from the participant name.

**`required: false`**: Not allowed, because an ecu_main executable joins as a coordinated participant, and SIL Kit does not start a coordinated participant that is not required. To run without an ECU, write `enabled: false` or pass `--disable <participant name>` (see [Disabling a participant](#disabling-a-participant-enabled---disable---enable)). A `role: bridge` participant may have it: a bridge joins on its own, in real time (see [Test participants and bridges](#test-participants-and-bridges-role)).

### Other executables (`describe: false`)

| Item | Rule |
|---|---|
| Check | Not checked. A note says that the name, step size and networks cannot be checked. |
| `step_ms` | Not checked even if written; a note says so. |
| Start | `run` starts the executable with the arguments as written (with the placeholders replaced). |
| Participant with `required: true` (the default) | Must join the registry at `{registry_uri}` under its participant name, with the coordinated lifecycle. The simulation waits for it. |
| A tool that does not take part in the simulation (one that only records logs, for example) | Set `required: false`. |

### Disabling a participant (`enabled`, `--disable`, `--enable`)

To run without one of the ECUs, write `enabled: false` for its participant. To change the set of participants without editing the file, pass these options to `check` and `run`:

| Option | What it does |
|---|---|
| `--disable <participant name>` | Disables the participant. |
| `--enable <participant name>` | Enables a participant that has `enabled: false`. |

Both can be repeated. The command line takes precedence over `enabled` in the file.

A disabled participant is handled as follows:

| Item | Handling |
|---|---|
| Check and start | Neither checked nor started (`--describe` does not run either). |
| Executable | Not searched for (it need not exist yet). |
| Simulation | Does not wait for the participant. |
| The entry | Checked like the entry of an enabled participant (keys, value types, duplicate names, the arguments of `command` and so on). |
| Errors in the entry | Do not stop the rest of the check. |
| Entries with the same name | An error even when disabled, because otherwise it is unclear which one `--disable <name>` means. |
| Note | `participant-disabled` for each participant. It does not fail the check, even with `--strict`. |

- Because the entry is checked, enabling the participant again never brings up errors for the first time.
- A participant whose `enabled` is not a boolean is neither checked nor started, because it is unclear whether it is disabled. It is an error, so the check fails.
- To replace an ECU with another one, give the other one a different name, or use a separate configuration file.
- The note keeps a result with some participants disabled from being mistaken for a result with all of them.

The remaining participants are checked as follows:

- The check covers the remaining participants as a set.
- Findings that result from disabling (such as a network left with only one participant) are reported as usual.
- Warnings caused by disabling (such as `single-participant` in the example below) remain warnings, so `--strict` makes the check fail (`run --strict` starts nothing).
- At least one enabled participant must be required (`required: true`) (otherwise the error `no-required-participant`).

`--disable` and `--enable` take participant names from the configuration file.

| Case | Handling |
|---|---|
| The same name in both `--disable` and `--enable` | Always a usage error (exit code 4). |
| A name not in the configuration file, and the file is readable and has no errors | A usage error (exit code 4). The message lists the valid names and every name that does not match. |
| A name not in the configuration file, and the file is unreadable (exit code 3) or has errors (exit code 1) | The errors of the file are reported first, and the names that do not match are not mentioned. |

The names that do not match are not mentioned because an entry whose name could not be read may be the reason they do not match. The names that are in the configuration file still take effect.

<!-- vehicle_sil_launch check simulation.yaml --disable Dashboard -->
```text
simulation.yaml: 2 participants (1 disabled), 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    -      disabled

network    kind  participants
J1939_CAN  can   Engine

warning: simulation.yaml:8: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it; if it talks only to equipment outside, connect it outside to run it (external says what passes) [single-participant]
note: simulation.yaml:14: participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it [participant-disabled]

passed: 0 errors, 1 warning, 1 note
```

### Starting a participant yourself (`manual`, `--manual`)

To start a participant yourself instead of through `run`, for example to run an ECU from a debugger, write `manual: true` for that participant. To start it yourself for one run only, without editing the file, pass `--manual <participant name>` to `check` and `run` (it can be repeated). `run` does not start that participant. It prints the command to start it and waits for it to join. Below, such a participant is called a manual participant.

The check is the same as for the other participants (with `describe: true`, it runs `--describe`). For each manual participant, the check reports the note `participant-manual`.

Rules for combining the options:

| Case | Handling |
|---|---|
| The same name in `--disable` and `--manual` | A usage error (exit code 4). |
| A name not in the configuration file | Handled as for `--disable`. |
| `--manual` for a disabled participant | Does not change `enabled`. The participant stays disabled. |
| Undoing `manual: true` of the file from the command line | No option does this. |

#### The command `run` prints

When `run` reaches the participant in its start order, it prints the working directory and a one-line command that starts the participant (example below). The arguments are the ones `run` uses when it starts the participant itself (`--name`, `--registry-uri` and `--start-timeout-ms` for an ecu_main executable, `--map-network` if it has `network_names`, and `--j1939-node` if it has `j1939_nodes`). Run that command in the directory shown. In a debugger, set the executable, the arguments and the working directory separately. With `--events`, the `participant` event whose `state` is `manual` carries the same information.

The arguments are quoted for the shell you paste them into:

- On Linux, for POSIX sh (bash, zsh).
- On Windows, for the Command Prompt (cmd.exe), with `\` as the separator in the executable path.
- In the cmd.exe form, the arguments are quoted by the rules the C runtime uses to read a command line, and arguments that contain `&` `|` `<` `>` `^` `,` `;` are also enclosed in `"` (PowerShell reads an unquoted `a,b` as 2 arguments and `;` as the end of a statement).
- cmd.exe expands `%...%` even inside `"`, so edit arguments that contain `%` before you paste them.
- To paste into PowerShell, add `&` before the quoted executable, and edit arguments that contain `$` or `` ` ``.

If `registry_uri` is not written, a run with a required manual participant starts the registry at `silkit://127.0.0.1:8500`. The command is the same in every run, so you can set up the debugger configuration once and reuse it. If another program uses that port, `run` fails with a `setup` error; write another port in `registry_uri`, or port 0 for a free one (the command then changes from run to run).

#### With a non-loopback `registry_uri`

If `registry_uri` is not a loopback address, `run` writes "with VEHICLE_SIL_LISTEN_ON_NETWORK=1 in its environment" on the line before the command. The note `participant-manual` also mentions this environment variable.

| Computer you start it on | `--registry-uri` | `VEHICLE_SIL_LISTEN_ON_NETWORK=1` |
|---|---|---|
| The same computer as the launcher | As in the printed command. | Set it when you start the participant (see "Listen addresses" in [simulation-run.md](simulation-run.md#listen-addresses)). |
| Another computer | Change it to the URI on the line after `registry at` (`from other computers, join at ...`). | Not needed. |

The text of the note `participant-manual` tells you to set this environment variable, whichever computer you start the participant on. The `from other computers, join at ...` line of `run` says that other computers do not need it. When you start the participant on another computer, follow the line of `run` and do not set the variable (for the steps, see the guide [Run on several computers](howto/multi-pc.md)).

#### How `run` waits

In a run with a required manual participant, the time limit for the start is split into 3 phases.

| Phase | Limit |
|---|---|
| 1. Until the participants join | `start_timeout_ms` for the participants that `run` starts. No limit for manual participants. |
| 2. After all have joined, until every manual participant has prepared its first step (ReadyToRun) | No limit (a manual participant may be stopped in a debugger). |
| 3. After that, until all participants reach the first step | `start_timeout_ms`. |

- In phase 1, if a participant that `run` starts does not join within `start_timeout_ms`, the result is `not-joined`, as in a run without manual participants.
- When all required participants that are not manual have joined, `run` prints the line `waiting for <name> to join (manual): ...` once.
- In phase 3, if a participant that `run` started hangs during initialization, the result is a `simulation-start` error, as in a run without manual participants.
- In every phase, Ctrl+C, the `stop` command and a `Stop` call through the Control Server stop the run (exit code 130 for Ctrl+C; 0 for the command and the call).
- `run` does not wait for a manual participant with `required: false`. It only prints the command.

In a run with a required manual participant, `--timeout-ms` counts from the moment `run` starts to wait for the participants. It covers the wait for them to join, the wait for the start and the run itself. If it runs out before the start, the result is a `timeout` error, and the message names what `run` was waiting for: participants that have not joined, or participants that have not reached the first step. To run a configuration file with `manual: true` in CI, pass `--timeout-ms`, so that the run ends even if nobody starts the participant.

If a manual participant that has joined ends before the start (for example, you stop it in a debugger and start it again), the start fails (`simulation-start`). That participant cannot join this run again, so start the launcher again too.

While `run` waits, if a participant whose name is not in the configuration file is waiting for the simulation to start, `run` prints the following line once. A mistyped `--name` is a typical cause. The line is printed for a participant that has stayed in ServicesCreated for 2 seconds, or that is in the Error state.

`participant '<name>' joined the simulation, but this run has no participant of that name (did you mean '<close name>'?): it waits for the simulation's start, which only the participants of the file get; ...`

That participant does not start. `run` keeps waiting for the participants named in the configuration file. Participants that are not in the file but run on their own (autonomous participants: real-time `Peer`s, bridges) count as participants of the simulation, and `run` prints nothing about them.

#### During the run

`run` monitors a manual participant as it monitors the other required participants, with these differences:

| Item | Manual participant |
|---|---|
| Detecting a failure | `run` does not watch the process; it detects that the participant has left the simulation (the connection was lost, or the participant is in the Error state) and reports `participant-failed`. |
| Output | Goes where you started the participant (a terminal, a debugger). |
| While it is stopped in a debugger | Virtual time does not advance. |
| With a set speed | The time it is stopped counts as lateness (`behind-real-time`). |
| `--timeout-ms` | Also counts the time it is stopped. |

#### How to stop

`run` sends neither a stop request nor a kill to the process of a manual participant.

| Case | What happens |
|---|---|
| The simulation ended | An ecu_main executable ends by itself. |
| The simulation did not start (an abort, a failure of another participant) | `run` sends a SIL Kit abort to the manual participants that had joined. An ecu_main executable ends with a start failure. |
| In both cases | `run` waits up to `stop_timeout_ms` for the joined manual participants to leave the simulation, then closes the registry. |
| A participant does not leave (it is stopped in a debugger, for example) | `run` says so and ends. This is not an error. |
| `run` was killed (SIGKILL, a crash, the console closed on Windows) | Participants that you started yourself may remain. End them yourself. |

- While `run` waits for the participants to leave, a second Ctrl+C or `stop` command cuts the wait short.
- If `run` was killed before the start, the remaining participants keep waiting for the start.
- You can stop `run` (Ctrl+C, the `stop` command) while a participant is stopped in a debugger.
- `run` then waits as described above, prints the result and ends.
- The stopped participant remains. Continue it yourself, or end it.

#### Result

| Output | Manual participant |
|---|---|
| `ended` in the participant table | `started by you (manual)` (joined) or `not joined (manual)`. |
| `participants[].manual` in JSON | `true`. |
| `ended` in JSON | `"not_started"`. |
| `exit_code` in JSON | `null`. |

#### Example

Start Engine of [examples/ecu_main/simulation.yaml](../examples/ecu_main/simulation.yaml) yourself. `run` writes these 2 lines to stderr. `<dir>` is the directory of the example; on Windows, the paths use `\` and the executable name ends in `.exe`. The command to run is the part of line 2 after `[vehicle_sil_launch]`.

<!-- vehicle_sil_launch run simulation.yaml --manual Engine: the lines about Engine -->
```text
[vehicle_sil_launch] not starting Engine: start it yourself (manual: true, or --manual); the simulation waits for it to join. In the directory <dir>, run:
[vehicle_sil_launch]   <dir>/engine/vehicle_sil_engine_ecu --name Engine --registry-uri silkit://127.0.0.1:8500 --start-timeout-ms 1000000000000
```

When you start Engine with that command, the simulation starts. The result:

<!-- vehicle_sil_launch run simulation.yaml --manual Engine -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

note: simulation.yaml:11: participant 'Engine': you start it yourself (manual: true, or --manual): run does not start it, but prints the command to start it with, and waits for it to join without a time limit (with --timeout-ms, at most that long) [participant-manual]

run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       started by you (manual)
Dashboard    yes       exit code 0

traffic: J1939_CAN: 1 participant sent, 1 received (2 counted) (steps before 1000 ms)

network    participant  sent  received
J1939_CAN  Engine         11         0
J1939_CAN  Dashboard       0        11

passed: 0 errors, 0 warnings, 1 note
```

### Test participants and bridges (`role`)

`role` tells the check what a participant is for. Only the rules and the output of the check use it. `run` starts every participant and lets it join in the same way, whatever the value. The role appears neither in the `--describe` JSON nor in the simulation's communication.

| Value | Meaning | What changes in the check |
|---|---|---|
| `ecu` (default) | An ECU of the vehicle | Nothing |
| `test` | A test participant (a spy that only receives, a tool that sends to the ECUs and so on) | It does not count as an ECU on the network (adding a spy does not make `single-participant` go away). |
| `bridge` | A participant that relays a network to a real bus ([bridges.md](bridges.md)) | It is treated as a bridge in the warning below. |

If a `role: test` participant reports a J1939 node in its `--describe` output and is on a network that may have a bridge, the check reports the warning `test-participant-talks-on-bridged-network`.

The check treats these participants as bridges:

| Configuration file | Treated as a bridge |
|---|---|
| Has a `role: bridge` participant | Only that participant. |
| `role: bridge` with `describe: true` | The check assumes that it relays only the networks in its `--describe` output. |
| `role: bridge` with `describe: false` | The check cannot tell which networks it relays, so it assumes that it may relay any network. |
| Has no `role: bridge` participant | The check treats each `describe: false` participant without `role` as "may be a bridge" (the warning text makes the same distinction). |

If you write `role: ecu` or `role: test` for a tool that is not a bridge, the check no longer treats it as a possible bridge.

The bridges of this repository (`vehicle_sil_ethernet_bridge`, `vehicle_sil_vector_xl_bridge`) answer `--describe` and take the arguments that `run` adds, so you write them with `describe: true` (the default) and `required: false`. Their `--describe` reports the network of `--network`, so the check knows what they relay:

```yaml
  - name: EthBridge
    role: bridge
    required: false
    command: [vehicle_sil_ethernet_bridge, --network, ETH_EXT, --backend, packet, --interface, eth1, --all-frames]
```

For what the launcher adds to the arguments and what `--describe` must write, see "[Starting a bridge of your own](bridge-contract.md#starting-a-bridge-of-your-own-role-bridge)" in bridge-contract.md; for the arguments of each bridge, see "Starting from a configuration file" in [ethernet-bridge.md](ethernet-bridge.md#starting-from-a-configuration-file) and [bridges.md](bridges.md#starting-from-a-configuration-file).

The reason for the warning: a bridge sends what is sent on the SIL Kit side to the real bus. So the transmissions of a test participant with a J1939 node, in particular its address claim (Address Claimed), also go out on the real bus, and the participant competes for its address with a real ECU that has the same address. To fix this, either make the participant only receive on that network (create no J1939 node), or disable the bridge in the runs that use the participant (`--disable <bridge>`, or `enabled: false` in a variant). The `--describe` output does not show that a participant sends raw CAN, so the check does not warn about such participants.

Other rules:

- In the participant table of the human-readable output, the name of a participant whose role is not `ecu` is followed by `(role: test)` or similar.
- `check --json` writes the role to `participants[].role`.
- A run with an enabled `role: bridge` participant keeps real time, as one with `--connect` does: it goes at speed 1 whatever `speed` the file has, and `test` runs its scenarios at speed 1 too (see "Time of a run that is connected outside (`--time-policy`)" in [simulation-run.md](simulation-run.md#time-of-a-run-that-is-connected-outside---time-policy)).
- A variant (`variants`) cannot change it, as with `describe` and `required`.

### Connecting a network outside (`external`, `--connect`)

A network can be connected outside, to a real bus. `external` says how the network behaves when it is connected, and is used only then: a run that does not connect the network ignores it. It holds what is the same on every PC that runs the simulation. Where the network is connected (an adapter, a channel) differs from PC to PC, so it is not written in the file: you give it when you start, with `--connect <network>=<target>` (below), and `external.connect` is an error.

```yaml
networks:
  - name: J1939_CAN1
    kind: can
    external:
      bitrate: 250000
  - name: ETH_EXT
    kind: ethernet
    external:
      ethertypes: [0x88b5, 0x0800]
```

| Key | Kind | Value | If omitted |
|---|---|---|---|
| `bitrate` | `can` | The bit rate to set on the channel (bit/s, an integer from 1 to 1000000). The tool that connects the network may take a narrower range (Vector XL: from 15000). | The channel's setting is kept |
| `ethertypes` | `ethernet` | The EtherTypes of the frames that pass between the network and the outside: a list of one or more, each once (`0x88b5`, `34997`, or `"0x88b5"` in quotes; JSON has no hexadecimal numbers) | Without `ethertypes` and `all_frames`, the frames of the MAC addresses that the ECUs on the network report pass |
| `all_frames` | `ethernet` | `true`: every frame passes. Not together with `ethertypes` | `false` |

Other rules:

- A `lin` network cannot have `external`: there is no bridge for LIN, so it runs on SIL Kit only.
- By default (no `ethertypes`, no `all_frames`) only the frames of the ECUs (participants without `role`, or with `role: ecu`) go out on the real bus, and only frames to them come in. To pass the frames of the participants with `role: test` too, write `ethertypes` or `all_frames`.
- A key of the other kind (`ethertypes` on a `can` network, `bitrate` on an `ethernet` network) is an `unknown-key` error.
- A variant (`variants`) cannot change it, as with every key of `networks`.

#### `--connect`

`--connect <network>=<target>` connects the network to a real bus through a bridge of this repository. `check --connect` checks the file as it runs connected; `run --connect` starts the bridge before the other participants and fails the run if the bridge ends (see "[Connecting a bus outside (`--connect`)](simulation-run.md#connecting-a-bus-outside---connect)" in simulation-run.md). `test` ends with a usage error (exit code 4) for `--connect`: it runs each scenario as fast as possible.

```sh
vehicle_sil_launch check --connect "ETH_EXT=vEthernet (A)" --connect J1939_CAN1=physical:1 simulation.yaml
vehicle_sil_launch run --speed 1 --connect "ETH_EXT=vEthernet (A)" --connect J1939_CAN1=physical:1 simulation.yaml
vehicle_sil_launch run --speed 1 --connect ETH_EXT=tap:vsil0 --connect J1939_CAN1=can0 simulation.yaml   # Linux
```

A target is `none` or `[<backend>:]<selector>`. The backend says which way the network is connected; without it, the default of the OS takes the selector:

| Kind | `<backend>:` | OS | Bridge | `<selector>` |
|---|---|---|---|---|
| `ethernet` | `npcap:` (default on Windows) | Windows | `vehicle_sil_ethernet_bridge` ([ethernet-bridge.md](ethernet-bridge.md)), `--backend npcap` | The adapter as the bridge's `--interface` takes it: the connection name (`vEthernet (A)`), the pcap name (`\Device\NPF_{...}`) or `{GUID}`. |
| `ethernet` | `packet:` (default on Linux) | Linux | The same, `--backend packet` | The interface name (`enp3s0`). |
| `ethernet` | `tap:` | Linux | The same, `--backend tap` | The name of a TAP device you created (`vsil0`). |
| `can` | `xl:` (default on Windows, and on Linux when the selector is `virtual:<n>` or `physical:<n>`) | Named on any OS, runs on Windows | `vehicle_sil_vector_xl_bridge` ([bridges.md](bridges.md)) | `virtual:<n>` or `physical:<n>`: the channel number from 1. |
| `can` | `socketcan:` (default on Linux, when the selector is not an XL channel) | Linux | `vehicle_sil_socketcan_bridge` ([socketcan-bridge.md](socketcan-bridge.md)) | The interface name (`can0`, `vcan0`). |
| `lin` | None | | None | Cannot be connected (`connect-kind`). |

- `<backend>:` is read only when the word before the first `:` is a backend of the kind in the table above, in lower case. Any other word before a `:` belongs to the selector (`virtual:1`, `eth0:1`, `TAP:x`, `pcap:x`). The selector after a prefix may have `:` too (`npcap:tap:x` is the adapter `tap:x`). A selector that starts with a backend's word, or is `none`, is given with its backend: `packet:none` is an interface named `none`.
- A backend that the OS does not have (`tap:x` or `socketcan:can0` on Windows, `npcap:x` on Linux) is the error `connect-backend`: it lists the backends that this OS has and says to give the target without a prefix to take the default.
- The launcher always passes the backend of an Ethernet network to the bridge (`--backend`), so the bridge does not choose by itself.

`vehicle_sil_launch connections <file>` lists the targets of each network on this computer, as `--connect` lines to copy (see "[Where a network can be connected](simulation-run.md#where-a-network-can-be-connected-vehicle_sil_launch-connections)" in simulation-run.md).

- `none` as the target: SIL Kit only, the same as no `--connect` for that network (for a script that says so outright). Give an interface named `none` with its backend: `packet:none`.
- To keep the targets of one computer in a file instead of typing them, write them in a request file (`"connect"`) and pass `--request <file>`; `--connect` then replaces or removes the target of one network (see "Request files (`--request`)" in [simulation-run.md](simulation-run.md#request-files---request)).
- Give each network once. The network's name ends at the first `=`, so a network whose name has `=` cannot be connected (rename it).
- Quote the whole argument when the target has spaces: `--connect "ETH_EXT=vEthernet (A)"`.
- The launcher looks for the bridge next to `vehicle_sil_launch`, then in the absolute directories of `PATH`. It never looks in the working directory or in the directory of the configuration file. It looks only for the bridges of the networks given with `--connect`, so a check without `--connect` writes the same on every computer. It starts a bridge (its `--describe`, its list for `connections`, its run) in the bridge's own directory, never in the directory of the configuration file, so that a DLL put into a folder received from someone is not loaded. The participants of the file start in the directory of the configuration file, as before.
- The prebuilt tools archive has `vehicle_sil_ethernet_bridge` next to `vehicle_sil_launch` in `bin/`, and on Linux `vehicle_sil_socketcan_bridge`; the Vector XL CAN Bridge is not in it (build it, see "[Build](bridges.md#build)" in bridges.md).
- The Vector XL bridge runs on Windows only. `xl:virtual:1` on Linux gives `connect-tool-missing`, which says that the network runs on SIL Kit only, unless a bridge is found anyway (a build for tests).
- The check runs the bridge's `--describe` with the arguments below and `--name bridge.<network>`, the bridge's participant name. `run` starts it with the same arguments and `--name bridge.<network> --registry-uri <URI> --start-timeout-ms <start_timeout_ms>`. `bridge.<network>` is not a participant of the file: `--disable`, `--enable` and `--manual` do not take it. With `--describe`, the bridge opens neither the adapter nor the channel. A bridge that refuses the arguments is the error `connect-tool-failed`, with the bridge's own line.
- On a connected network, the bridge counts as a participant (one ECU there is not `single-participant`), and a `role: bridge` participant on the same network is a loop (`connect-loop`).
- A real bus does not wait for the virtual time: a run with `--connect` goes at speed 1 whatever `speed` the file has (the note `outside-speed-set`), and another `--speed` or `--start-paused` is the error `outside-real-time`. `--time-policy free` lifts this (see "Time of a run that is connected outside (`--time-policy`)" in [simulation-run.md](simulation-run.md#time-of-a-run-that-is-connected-outside---time-policy)).

The arguments that the launcher gives the bridge:

| Kind | Arguments |
|---|---|
| `ethernet` | `--network <network> --backend <backend> --interface <selector>`, and the frames that pass: `--all-frames` (`all_frames: true`), `--ethertype` (`ethertypes`), or else `--src-mac` with the MAC addresses that the ECUs on the network report in their `--describe` output, not those of participants with `role: test` or `role: bridge` (if an ECU reports none, one has `describe: false`, or no ECU reports one, the error is `connect-filter`) |
| `can` with `xl` | `--network <network> --xl-target <virtual or physical> --xl-channel <n>`, and `--bitrate <bitrate>` with `bitrate` |
| `can` with `socketcan` | `--network <network> --interface <selector>`, and `--bitrate <bitrate>` with `bitrate` (the bridge only checks that the interface runs at this rate; it never sets it) |

The findings are the `connect-*` codes in [simulation-check.md](simulation-check.md#finding-codes). `check --json` writes the bridge, where it was found and its command line to `networks[].outside`, and the human-readable output adds the column `outside` to the network table.

### Matching variable names and values (`variables`)

ECUs that do the same job can still differ from executable to executable in variable names, units and the way values are represented: someone else's ECU, an older version, differences between variants. If you write the mapping in `variables`, every executable shows the same names and values to the simulation side (scenarios, `check --json`, the Control API and the GUI that uses it). The executables do not change. If you can align the names in the ECUs, that is simpler; the variable map is for what you cannot align.

```yaml
participants:
  - name: Engine
    command: [engine/engine_small]
    variables:
      engine.throttle: throttle_pct                        # rename
      engine.speed: { from: rpm_x10, scale: 0.1, unit: rpm }   # convert the value linearly
      engine.gear: { from: gear_raw, map: { P: 0, R: 1, N: 2, D: 3 } }   # name the values
      engine.turbo: { absent: true }                       # this executable does not have it
```

The key is the name used in the simulation. The value takes one of the 4 forms below. Each variable takes one form, and any key that the table does not list is an error.

| Form | Meaning | Variable types |
|---|---|---|
| `<variable name in the executable>` (a string) | Renames the variable and nothing else (same as `{ from: <name> }`). | |
| `{ from, scale, offset }` | Visible value = value in the executable × `scale` + `offset` | Integer or real |
| `{ from, map: { <name>: <value>, ... } }` | Gives names to the (integer) values of the executable. | Integer or enum |
| `{ absent: true }` | This executable does not have the variable. | |

- `scale` is a number other than 0 (1 if omitted). `offset` is a number (0 if omitted).
- In `map`, one value can have only one name.
- `absent: true` cannot be combined with other keys.
- The forms with `from` can also have `unit` (the unit of the visible value).

#### How variables appear

| Item | How it appears |
|---|---|
| Name | A mapped variable is visible only under the name of its key. It cannot be read or written under the executable's name. |
| Order | The order in which the executable declared the variables. |
| Input, output or parameter | Unchanged. |
| Type with `scale`, `offset` | Real. |
| Range (`min`, `max`) and increment (`step`) with `scale`, `offset` | Converted to the visible value (a negative `scale` swaps the ends of the range). |
| Unit with `scale`, `offset` | The one written in `unit`, or empty if omitted (the executable's unit does not apply to the converted value). |
| Type with `map` | Enum, with only the items written in `map`. |
| Numbers with `map` | The executable's values, unchanged. |
| `absent: true` | Not visible anywhere. |

With `map`, a scenario that writes names (`engine.gear: D`) works for every executable, even when the executables use different numbers.

Scenario steps that use an `absent: true` variable are skipped (see "Variants and parameters" in [scenario.md](scenario.md#variants-and-parameters)). If an executable lacks a variable that has neither a mapping nor `absent`, the check of the scenario reports an error.

#### Writes

A write to a variable with `scale`, `offset` is converted back: value in the executable = (written value − `offset`) ÷ `scale`.

| Item | Rule |
|---|---|
| The executable's variable is an integer | Rounded to the nearest integer (exact halves away from 0: 2.5 becomes 3, −2.5 becomes −3). |
| Values outside the range | Rejected before they reach the executable. |
| Range check | Decided on the converted value in the executable (the check of a scenario decides the same way). |
| Ends of the range shown in lists | Rounded to 15 significant digits. |
| Values not in `map` | Cannot be written. |

Because of the rounding, the value you read back can differ from the value you wrote by the rounding error. To compare such values in a scenario, use `near` and `tolerance`.

#### Checking the map

The check (`check`, and the start of `run`) compares the whole map with the `--describe` output of the executable. It checks that the `from` variable exists, that the form fits the type, that the variable can take the values in `map`, and that the name does not collide with other variables (findings `variable-map-*` in [simulation-check.md](simulation-check.md)). If any mapping does not match, `run` starts nothing.

In `check --json`, `participants[].variables` holds the variables after the mapping, and `participants[].variable_map` gives both names.

A `describe: false` participant can also have `variables`, but they cannot be compared before the run (note `variable-map-not-checked`). The map is applied when the Control Agent of that participant connects. Mappings that do not match are handled as follows:

| Item | Handling |
|---|---|
| Applying | Mappings that do not match are not applied. |
| Notification | A line of the run reports it, at the start of the simulation or, if the Control Agent connected after the start, at the end of the run (`[vehicle_sil_launch] participant '<name>': variables: <n> of its mappings do not fit ...`; with events, a `log` event). |
| Run finding | At the end, `variable-map-not-applied` (an error). The run is not stopped for it: it runs to its end and then fails, whenever the Control Agent connected. |
| Reads | For the rest of that run, the variables of mappings that do not match can be read under the executable's names, with the executable's values. |
| Writes | Writes to the names of that mapping (the key and `from`) are rejected. |

Writes are rejected so that a value meant for the common name is not applied to another variable, or without the conversion.

### Matching network names (`network_names`)

An ECU executable has its network names in its code (`j1939("J1939_CAN")` and the like). ECUs written by different people may call the same bus by different names. With a mapping in `network_names`, such a network connects to the bus of the simulation's name without a rebuild of the executable.

```yaml
networks:
  - name: VehicleCAN
    kind: can
participants:
  - name: Engine
    command: [engine/engine_ecu]          # j1939("J1939_CAN") in the code
    network_names:
      VehicleCAN: J1939_CAN               # this executable's J1939_CAN is VehicleCAN in the simulation
  - name: Dashboard
    command: [dashboard/dashboard_ecu]    # VehicleCAN in the code too: no mapping needed
```

The direction is always "simulation name: executable name", as in `variables`.

- A key is a name in `networks` (otherwise the error `network-map-undeclared`). A value is the name that the executable passes to `j1939()`, `can()`, `lin_master()`, `lin_slave()` or `ethernet()`.
- A mapping applies to a name: `j1939()` and `can()` with the same name are both renamed. The kind does not change. Networks that are not listed keep the executable's names.
- A network of the executable can have only one name. One value under 2 keys is an error, and so is a mapping after which 2 networks of the executable have the same name (`network-map-collision`). Swapping 2 names (`A: B` and `B: A`) is allowed.
- A line whose key and value are the same (`VehicleCAN: VehicleCAN`) counts as no line. A variant uses it to remove a common line (see "Moving a network to another bus in a variant" below).
- A key must not be empty, must not contain `=` and must not start with `-` (the error `value`), because it is passed in the argument `--map-network <key>=<value>`.
- `run` adds `--map-network VehicleCAN=J1939_CAN` at the end of the arguments of a participant with a mapping, one for each line in the order written. The executable then connects the code's `J1939_CAN` to `VehicleCAN` in SIL Kit (see "Renaming networks" in [ecu-executable.md](ecu-executable.md#renaming-networks---map-network)). The names that the ECU's code sees do not change. The command of a participant that you start yourself (`manual`) and the `command` of the `participant` event carry the same arguments.
- The check (`check`, and the start of `run`) renames the reported networks with the mapping before it applies the rules. The table, `networks[].participants` in `check --json` and `network` of the findings use the simulation's names. In the table, the row of the participant shows both names: `VehicleCAN <- J1939_CAN (j1939 0x00)`. In `check --json`, `participants[].network_map` lists the lines you wrote, and `participants[].reported.networks[].name` keeps the executable's names.
- The check also compares the mapping with what the executable reports. Two errors can come of it: the executable does not use a network with the name of the value (`network-map-unknown`), or the executable cannot take `--map-network` (`network-map-unsupported`: `features` in its `--describe` output has no `map-network`, because it was built with an earlier vehicle-sil; rebuild it, or give the network in `networks` that executable's name and write the mappings for the other participants). With either error, `run` starts nothing.
- A `describe: false` participant can have `network_names` too (an ECU that you start yourself on another computer with another OS, for example). It cannot be checked, so the check reports the note `network-map-not-checked`, which shows the arguments that `run` adds in that run (with the selected variant). A disabled participant, and a participant whose lines a variant removed entirely, get no note. Only ecu_main executables accept `--map-network`: for bridges and other tools, do not write `network_names`, and give the simulation's name in the arguments or settings of that executable. You can write `--map-network` yourself in the `command` of a `describe: false` participant, but not together with `network_names` (the error `launcher-argument`).

**Moving a network to another bus in a variant**: the lines of a variant are merged into the common lines key by key. A line with another key is added, so writing the same network of the executable under another key gives it 2 names (the error `network-map-collision`; the message shows the line to add). Remove the common line with a line whose key and value are the same:

```yaml
participants:
  - name: Engine
    command: [engine/engine_ecu]
    network_names:
      VehicleCAN: J1939_CAN             # common: J1939_CAN is VehicleCAN
variants:
  standard: {}
  body_bus:
    participants:
      Engine:
        network_names:
          VehicleCAN: VehicleCAN        # removes the common line
          BodyCAN: J1939_CAN            # in this variant, J1939_CAN is BodyCAN
```

Errors in `network_names` are reported for the variants that are not selected too, as errors in a variant's `command` are (`check` reads every variant in the file).

Limits:

- On a Windows launcher, the names in a mapping (keys and values) must be ASCII (the error `network-map-not-ascii`). An ECU executable on Windows refuses a non-ASCII `--map-network`, because its arguments arrive in the code page. A Linux launcher does not report this error, so keep mapped names ASCII also when you start ECUs on a Windows computer from Linux.
- Under `run`, the monitoring variables (`j1939.<network name>.bus_state` and the like) carry the simulation's name (`j1939.VehicleCAN.*`). If the `variables` of a `describe: false` participant name such a variable in `from`, use that name.
- A `Network` in a controller of the SIL Kit configuration passed with `--silkit-config` cannot be used together with a mapping (the executable refuses it at start with exit code 3). An ECU built without rapidyaml refuses any mapping together with `--silkit-config`, also at start. The check finds neither.
- The check does not find a mapping for the name of a J1939 network on a port passed in from outside (`j1939(name, port)`) either (exit code 3 at start).
- `network_names` does not tell the check which networks a `describe: false` participant is on (the check still does not know the networks of that participant).

### Matching J1939 addresses and NAMEs (`j1939_nodes`)

A J1939 node has its address and NAME in the ECU's code (`add_node()`). 2 copies of one ECU executable on one network ask for the same address with the same NAME, and neither gets an address (`name-conflict`). With an entry in `j1939_nodes`, a copy gets another address and NAME fields without a rebuild of the executable.

```yaml
participants:
  - name: Engine
    command: [engine/engine_ecu]          # one node at 0x00 in the code
  - name: Engine2
    command: [engine/engine_ecu]          # the same executable
    j1939_nodes:
      - { network: J1939_CAN, address: 0x01, identity_number: 2 }
```

Keys of an entry:

| Key | Required | Value |
|---|---|---|
| `network` | Yes | The simulation's name of the node's CAN network (a name in `networks`; with `network_names`, the key, not the executable's name) |
| `from` | | The node's address in the executable's code, 0..254 (254: a node whose code asks for any free address). Can be left out when the executable has one node on the network |
| `address` | | The address that the node asks for, 0..254 (254: any free address, only for a NAME that is arbitrary address capable) |
| `identity_number` | | The identity number field of the NAME, 0..2097151 |
| `function_instance` | | The function instance field of the NAME, 0..31 |
| `ecu_instance` | | The ECU instance field of the NAME, 0..7 |

- Write numbers without quotes, as YAML 1.2 writes an integer: in decimal (`010` is ten), or in hexadecimal with a lower-case `0x` (`0x01`; YAML reads `0X01` as text, and also `1_0` and `0b11`). A value out of its range is the error `value`; nothing is cut to the width of the field.
- `from` is the address in the executable's code: the address that the table of `check` shows for the node without `j1939_nodes`. It is not the new address. With 2 or more nodes of the executable on the network, every entry needs `from` (otherwise the error `j1939-node-ambiguous`).
- An entry without `from` follows the executable's only node there, also when the ECU's author changes that node's address in the code. To point at a fixed address, write `from`.
- Write one entry for each node (the error `j1939-node-duplicate` for 2 entries with the same `network` and `from`, and for an entry without `from` beside another entry for the same network).
- An entry with only `network` and `from` changes nothing. `run` does not pass it on, and the check does not match it. A variant uses it to remove a common entry.
- The NAME fields need a node that has a NAME (otherwise `j1939-node-no-name`). The other fields of the NAME (manufacturer code, function, arbitrary address capable and the rest) cannot be changed.
- `run` adds `--j1939-node <network>=<settings>` for each entry that changes something, after `--map-network` (`--j1939-node J1939_CAN=address:0x01,identity_number:2`). The command of a participant that you start yourself (`manual`) and the `command` of the `participant` event carry the same arguments. See "Changing J1939 node addresses and NAMEs" in [ecu-executable.md](ecu-executable.md#changing-j1939-node-addresses-and-names---j1939-node).
- The check applies the entries to the nodes that the executable reports, with the same rules as the executable, before the rules of the addresses and NAMEs (`address-conflict`, `address-shared`, `name-conflict`). In the table, a node whose address an entry changes shows both addresses, the simulation's first: `J1939_CAN (j1939 0x01<-0x00)` (254 is `any`). The note `j1939-node-changed` lists what changes. In `check --json`, `participants[].j1939_nodes` lists the entries with the node each one takes, and `participants[].reported` keeps the values in the code.
- An entry that takes no node is an error (`j1939-node-unknown`, `j1939-node-ambiguous`, `j1939-node-indistinct`, `j1939-node-no-name`, `j1939-node-any-address`; see [simulation-check.md](simulation-check.md#finding-codes)). An executable built with an earlier vehicle-sil does not take `--j1939-node` (`j1939-node-unsupported`: no `j1939-node` in `features`). With any of these errors, `run` starts nothing.
- A `describe: false` participant can have `j1939_nodes` too. It cannot be checked, so the check reports the note `j1939-node-not-checked`, which shows the arguments that `run` adds. Only ecu_main executables accept `--j1939-node`. You can write `--j1939-node` yourself in the `command` of a `describe: false` participant, but not together with `j1939_nodes` that change something (the error `launcher-argument`).

**In a variant**, an entry for the same node (the same `network` and `from`, or both without `from`) replaces the common entry whole: the settings are not merged. Write the entry of a variant in the same form as the common one; a common entry without `from` and a variant's entry with `from` are 2 entries (`j1939-node-duplicate`). To remove a common entry, write it with `network` and `from` only:

```yaml
participants:
  - name: Engine2
    command: [engine/engine_ecu]
    j1939_nodes:
      - { network: J1939_CAN, from: 0x00, address: 0x01, identity_number: 2 }
variants:
  standard: {}
  alone:
    participants:
      Engine2:
        j1939_nodes:
          - { network: J1939_CAN, from: 0x00 }   # removes the common entry
```

Limits:

- What other ECUs have in their code about this ECU does not change: its address (as a destination, or to tell senders apart) and its whole NAME. An ECU that looks this one up with `address_of()` by the whole NAME does not find a copy whose NAME fields changed. Other constants in the ECU's own code (a filter, a value in the data it sends) do not change either. The note `j1939-node-changed` says so for each participant.
- A node that the ECU adds after it starts is not in `--describe` and keeps the values in its code.
- Moving a node to another address can change how long it waits to send after its address claim (the wait depends on the address range). Whether this matches the standard has not been checked.
- On a Windows launcher, the `network` of an entry that changes something must be ASCII (the error `j1939-node-not-ascii`), as for `network_names`.

### Variants (`variants`, `--variant`)

When the executables or variable maps differ by variant, write the common part in `participants` and the differences of each variant in `variants`. `check` and `run` apply the differences of the variant selected with `--variant <name>` on top of the common part, then check and run the result. `test` runs all variants in turn (see "Running several scenarios (`vehicle_sil_launch test`)" in [simulation-run.md](simulation-run.md#running-several-scenarios-vehicle_sil_launch-test)). Scenarios use only the common names, so the same scenario works for every variant.

```yaml
participants:
  - name: Engine
    command: [./engine_small]
    variables:
      engine.turbo: { absent: true }
  - name: Dashboard
    command: [./dashboard]
params: { idle_rpm: 800 }
variants:
  small: {}                           # the common part as is
  medium:
    participants:
      Engine:
        command: [./engine_medium]                   # replace the executable
        variables: { engine.turbo: turbo_bar }       # lay a mapping over the common ones
      Dashboard: { enabled: false }                  # not in this variant
    params: { idle_rpm: 650 }                        # change a parameter
```

| Key | Value |
|---|---|
| `variants.<variant>` | The differences of that variant |
| `variants.<variant>.participants.<participant>` | How a participant of the common part differs in this variant |
| `…<participant>.command` | Executable and arguments (replaces the whole common `command`) |
| `…<participant>.variables` | Variable map (merged into the common `variables`, one mapping at a time) |
| `…<participant>.network_names` | Network name map (merged into the common `network_names`, line by line) |
| `…<participant>.j1939_nodes` | J1939 node entries (merged into the common `j1939_nodes`, node by node) |
| `…<participant>.enabled` | `true` or `false` (replaces the common `enabled`) |
| `variants.<variant>.params` | Parameters (merged into the common `params`, name by name) |

- If `variants.<variant>` is `{}` (or has no value), the variant is the common part as is.
- A variant name must not be empty, must fit on one line and must not start with `-`.
- The keys of `variants.<variant>.participants` are participant names. Only names in `participants` are allowed (the error `variant-participant`).
- `…<participant>.command` follows the same syntax and rules as `participants[].command`.
- In `…<participant>.variables`, a mapping with the same name (key) replaces the common one, and a new name is added.
- In `…<participant>.network_names`, a line with the same key replaces the common one (a line whose key and value are the same removes the common line), and a new key is added.
- In `…<participant>.j1939_nodes`, an entry with the same `network` and `from` replaces the common one whole (an entry with only `network` and `from` removes it), and a new one is added (see [Matching J1939 addresses and NAMEs](#matching-j1939-addresses-and-names-j1939_nodes)).

A variant can change only these 6 keys: `command`, `variables`, `network_names`, `j1939_nodes`, `enabled` and `params`. Any other key is an error (`unknown-key`). Networks, `step_ms`, `describe`, `required`, `role` and the rest are the same in every variant.

| Goal | How to write it |
|---|---|
| Have a participant in one variant only | Write it in `participants` with `enabled: false`, and set `enabled: true` in that variant. |
| Stop using a common mapping in one variant | Write a mapping of that variant for the same name (`engine.speed: engine.speed` keeps the name and the value unchanged). |

The second row is needed because a variant cannot have fewer mappings than the common part.

Rules for `--variant`:

| Case | Handling |
|---|---|
| `--variant` omitted for a configuration file with variants | A usage error (exit code 4; the message lists the variants). |
| `--variant` with a name that is not a variant | A usage error. |
| `--variant` for a configuration file without variants | A usage error. |

- There is no default variant.
- The first 2 errors above are reported first, even if the text of the configuration file has other errors.
- In that case, the check does not check the common part alone in place of a variant (the message adds that the file has errors too).
- The error for `--variant` with a configuration file without variants is, as with `--disable`, not reported while the text has errors (the reason may be a `variants` that could not be read).

Other rules:

- When the file is read, the form of every variant is checked, including the variants that are not selected.
- Only the selected variant is checked against the executables.
- `--disable`, `--enable` and `--manual` apply after the variant is applied.
- `check --json` includes `variant` (the selected variant) and `variants` (all names).
- `participants` in `check --json` shows the participants after the variant is applied.

### Scenario parameters (`params`, `--param`)

To change numbers in a scenario (thresholds, wait times, values to set) by variant, write `${name}` in the scenario and the value in `params`.

| Item | Rule |
|---|---|
| Value | A number, `true`/`false` or a string (a single value; no arrays or mappings). |
| Name | Letters, digits and `_`, not starting with a digit. |

A later value replaces an earlier one, in this order:

| Order | Where the value is written |
|---|---|
| 1 | `params` of the scenario (the defaults) |
| 2 | `params` of the configuration file |
| 3 | `params` of the variant |
| 4 | The option `--param <name>=<value>` |

- `--param` is allowed only with `--scenario`.
- `--param` with a name that the scenario does not use is a usage error (exit code 4; typos are never silently ignored).
- In `test`, which runs several scenarios, only a name that no scenario uses is an error.
- `params` of the configuration file and of the variants may contain names that the scenario does not use, because one configuration file runs several scenarios.

For the syntax and the errors, see "Variants and parameters" in [scenario.md](scenario.md#variants-and-parameters).

## Arguments and exit codes

```text
vehicle_sil_launch check [--json] [--strict] [--variant <name>] [--scenario <file> [--param <name>=<value>]...]
                         [--describe-timeout-ms <ms>] [--connect <network>=<target>]...
                         [--time-policy realtime|free] [--request <file>]
                         [--text-from-stdin] [--descriptions <file>] [--write-descriptions <file>]
                         [--disable <name>]... [--enable <name>]... [--manual <name>]... [--] <file>
vehicle_sil_launch run [--json | --events [--events-stall-ms <ms>]] [--stdin-commands] [--start-paused]
                       [--control <address> [--control-credentials <file>] [--control-tls-cert <file> --control-tls-key <file>]
                       [--control-audit-log <file>]]
                       [--variant <name>] [--scenario <file> [--param <name>=<value>]... [--timeline <file>]]
                       [--speed <speed>] [--strict] [--log-dir <dir>] [--timeout-ms <ms>]
                       [--describe-timeout-ms <ms>] [--connect <network>=<target>]...
                       [--time-policy realtime|free] [--request <file>] [--disable <name>]...
                       [--enable <name>]... [--manual <name>]... [--] <file>
vehicle_sil_launch test [--json] [--strict] [--fail-fast] [--variant <name>]... [--param <name>=<value>]...
                        [--timeout-ms <ms>] [--describe-timeout-ms <ms>] [--time-policy realtime|free]
                        [--disable <name>]... [--enable <name>]...
                        <file> --scenarios <directory or files>...
vehicle_sil_launch connections [--json] [--network <name>] [--request <file>] [--] <file>
vehicle_sil_launch version [--json]
```

`connections` lists where each network of the file can be connected on this computer (see "Where a network can be connected (`vehicle_sil_launch connections`)" in [simulation-run.md](simulation-run.md#where-a-network-can-be-connected-vehicle_sil_launch-connections)).

`test` runs each scenario once for each variant and shows all results in one table (see "Running several scenarios (`vehicle_sil_launch test`)" in [simulation-run.md](simulation-run.md#running-several-scenarios-vehicle_sil_launch-test)).

### Arguments

The usage above shows which commands accept each option. The Condition column of the table gives any other conditions.

| Option | Condition | Meaning |
|---|---|---|
| `--json` | | Writes the result as JSON. |
| `--events` | `run` only | Writes the progress and the result as events ([Programmatic use](simulation-events.md); not with `--json`). |
| `--events-stall-ms <ms>` | `run` with `--events` only | If writing events makes no progress at all for this long, the reader is treated as gone (an integer from 1000 to 86400000; default 30000). |
| `--stdin-commands` | `run` only | Reads commands from stdin (the end of stdin is also a stop request). |
| `--start-paused` | `run` with `--stdin-commands` or `--control` only | When all participants have joined, pauses at virtual time 0 and waits for the `resume` or `step` command. |
| `--control <address>` | `run` only, and only in builds with the Control Server | Starts a Control API (gRPC) server (see "Connect to the launcher" in [control-api.md](control-api.md#connect-to-the-launcher)). |
| `--control-credentials <file>` | `--control` only | A file of tokens and roles (one `<role> <name> <token>` per line). With it, every call needs a token. |
| `--control-tls-cert <file>`, `--control-tls-key <file>` | `--control` only | The server's certificate chain and private key (PEM). Passing both turns on TLS. |
| `--control-audit-log <file>` | `--control` only | Appends a record of who changed what. |
| `--scenario <file>` | | The scenario file ([scenario.md](scenario.md)). |
| `--variant <name>` | | Selects a variant of the configuration file (see [Variants](#variants-variants---variant)). |
| `--param <name>=<value>` | Only with `--scenario` | The value of the scenario parameter `${name}` (see [Scenario parameters](#scenario-parameters-params---param)). |
| `--timeline <file>` | `run` only, and only with `--scenario` | Writes the values that the scenario wrote and saw to a JSON file, in virtual-time order (see "Recording a scenario run on a timeline (`--timeline`)" in [simulation-run.md](simulation-run.md#recording-a-scenario-run-on-a-timeline---timeline)). |
| `--scenarios <dir or files>` | `test` only | The scenarios to run. |
| `--fail-fast` | `test` only | Runs nothing after the first run that fails. |
| `--speed <speed>` | `run` only | Overrides `speed` in the configuration file (a number or `max`; see "Running at a set speed (`speed`)" in [simulation-run.md](simulation-run.md#running-at-a-set-speed-speed)). |
| `--strict` | | Fails on warnings too (not on notes). |
| `--log-dir <dir>` | `run` only | Directory for the output files, one per participant. |
| `--timeout-ms <ms>` | `run`, and `test` (per run) | Limit on the wall-clock time from the start to the end of the simulation (an integer of 1 or more; default no limit). |
| `--describe-timeout-ms <ms>` | | How long to wait for the `--describe` of one executable (an integer of 1 or more; default 10000). |
| `--connect <network>=<target>` | `check` and `run` | Connects the network to a real bus at `<target>` through its bridge; `none`: SIL Kit only (can be repeated, each network once; see [Connecting a network outside](#connecting-a-network-outside-external---connect)). |
| `--time-policy realtime\|free` | | The time of a run that is connected outside the simulation: `realtime` (the default) keeps it at speed 1, `free` leaves the speed and the time to you (see "Time of a run that is connected outside (`--time-policy`)" in [simulation-run.md](simulation-run.md#time-of-a-run-that-is-connected-outside---time-policy)). |
| `--request <file>` | `check`, `run` and `connections` | Reads what to run from a request file (JSON); an argument wins over the file, field by field. `connections` reads only its `connect` and says whether each target can be used on this computer (see "Request files (`--request`)" in [simulation-run.md](simulation-run.md#request-files---request)). |
| `--network <name>` | `connections` only | Lists the network `<name>` only. |
| `--text-from-stdin` | `check` only | Reads the text of the configuration from stdin instead of from `<file>`. `<file>` still names it: its relative paths and the `scenario` of a request file are from its directory, and the findings and the JSON name it. The file need not exist. |
| `--write-descriptions <file>` | `check` only | Writes the `--describe` output of each participant whose output was read to `<file>`, for `--descriptions` (see "Saved descriptions (`--write-descriptions`, `--descriptions`)" in [simulation-check.md](simulation-check.md#saved-descriptions---write-descriptions---descriptions)). |
| `--descriptions <file>` | `check` only | Starts no process: takes each participant's `--describe` output from `<file>`, if its command and its executable are as they were when the output was saved; any other participant is not described (see the same section of [simulation-check.md](simulation-check.md#saved-descriptions---write-descriptions---descriptions)). |
| `--disable <name>` | | Disables participant `<name>` (same as `enabled: false`; can be repeated). |
| `--enable <name>` | | Enables participant `<name>` again (same as `enabled: true`; can be repeated). |
| `--manual <name>` | | You start participant `<name>` yourself (same as `manual: true`; can be repeated). |
| `--` | | Everything after it is the file name (for names that start with `-`). |
| `--help`, `-h` | | Writes the usage to stdout and exits with 0 (anywhere before `--`). |
| `--version` | | Same as `version` (anywhere before `--`). |

Notes on individual options:

| Option | Notes |
|---|---|
| `--events-stall-ms` | Also limits the wait for the `result` event to be written (see "Slow readers and readers that stop reading" in [simulation-events.md](simulation-events.md#slow-readers-and-readers-that-stop-reading)). |
| `--start-paused` | With `--control`, pauses before the step at time 0. |
| `--control` | `<address>` is a port number (listens on 127.0.0.1; 0 picks a free port), `<host>:<port>` or `unix:<path>`. |
| `--control` | A non-loopback address needs TLS and credentials. |
| `--scenario` | `check` only checks; `run` checks, then runs. |
| `--scenario` | When the scenario ends, the simulation stops. If a step failed, the run fails. |
| `--scenario` | Cannot be used with `--start-paused`. |
| `--scenario` | With `--control`, clients can read and move time, and cannot write (see "Watching variables during a run (`--control`)" in [scenario.md](scenario.md#watching-variables-during-a-run---control)). |
| `--variant` | Always required for a configuration file with variants. |
| `--variant` | Not allowed for a configuration file without variants. |
| `--variant` | In `test`, limits the variants to run (can be repeated; without it, all variants run). |
| `--param` | Can be repeated (each name once). |
| `--param` | In `test`, applies only to the scenarios that have a parameter of that name (if no scenario has it, a usage error). |
| `--timeline` | If the file cannot be opened, nothing is started and the exit code is 3. |
| `--scenarios` | A directory (the `.yaml`, `.yml` and `.json` files directly in it, in name order) or files (in the order written). |
| `--scenarios` | Takes any number of paths after it, and can be repeated. |
| `--scenarios` | Write the configuration file before `--scenarios` or after `--`. |
| `--timeout-ms` | Also counts the time the run is paused. |
| `--timeout-ms` | With `--control` and `--start-paused`, counts from the pause before the step at time 0. |
| `--timeout-ms` | In a run with a required manual participant, counts from the moment `run` starts to wait for the participants. |
| `--timeout-ms` | When it runs out, the run is aborted with an error. |
| `--manual` | `run` does not start the participant; it prints the command and waits. |
| `--connect` | `run` starts the bridge `bridge.<network>` before the other participants (see "[Connecting a bus outside](simulation-run.md#connecting-a-bus-outside---connect)" in simulation-run.md). `test` ends with a usage error for it: it runs each scenario as fast as possible. |
| `--request` | Only the file that is named is read; the launcher looks for none. |
| `--request` | `scenario` in the file is from the directory of the configuration file, not from the working directory. |
| `--time-policy` | `test` runs each scenario at speed 1 when the file has an enabled `role: bridge` participant; `--time-policy free` runs them as fast as possible all the same. |
| `--text-from-stdin` | The rules of a configuration file apply to the text (see [File format](#file-format)): UTF-8 with an optional BOM, no UTF-16 or NUL, at most 16 MiB. It is read as bytes, on Windows too, so CRLF line ends and the line numbers are as in a file. |
| `--descriptions` | `--connect` and `connect` of a request file are checked for their form and network names only: no bridge is looked for or run. |
| `--descriptions`, `--write-descriptions` | Can be given together (the outputs taken from the file are written again), and with `--text-from-stdin`. |

On Windows, the arguments are read as UTF-8 from the wide-character command line, so paths with non-ASCII characters work.

### Exit codes

| Exit code | Meaning |
|---|---|
| 0 | No errors |
| 1 | Errors |
| 3 | The configuration file cannot be read |
| 4 | Usage error |
| 5 | `run`: a command stopped a scenario before its end |
| 130 | `run` or `test` was interrupted |

When each exit code occurs:

| Exit code | Case |
|---|---|
| 0 | With only warnings and notes, the exit code is 0 (with `--strict`, there must be no warnings either). |
| 0 | For `run`, when the simulation ended normally and every required participant exited with 0. |
| 0 | For `run`, also when `{"command":"stop"}` or `Stop` from a Control Server client stopped it and nothing was wrong. |
| 1 | With `--strict`, warnings also give 1. |
| 1 | For `run`, an error of the check (nothing is started) or an error of the run (a participant failed, the participants did not all join, a participant did not end when stopped, and so on). |
| 1 | For `test`: at least one run failed, a run did not take place although `test` was not interrupted, or, with `--strict`, no run took place at all. |
| 1 | If the scenario file of `--scenario` does not exist, the exit code is 1 for both `check` and `run` (unlike a missing configuration file, which gives 3). |
| 1 | For `connections`, a bridge that is on this computer could not make its list, the file has errors (a bridge that is not there is not an error), or with `--request` a target of the request file cannot be used. |
| 1 | For `check --write-descriptions`, also when the file cannot be written (the error `descriptions-write`). |
| 3 | The configuration file does not exist, a syntax error, an unknown format version and so on. |
| 3 | Also when `run` cannot set up the registry or the controller. |
| 3 | Also when the result cannot be written to stdout, and on an unexpected failure of `vehicle_sil_launch` itself. |
| 4 | The error and the usage go to stderr (the table below lists the cases). |
| 5 | For `run`, when `{"command":"stop"}` or `Stop` from a Control Server client stopped a scenario before its end and nothing else was wrong. |
| 130 | Ctrl+C and other signals, the end of stdin, a closed stdout. |

On an unexpected failure (exit code 3), the reason goes to stderr. With `--json`, the command's usual JSON holds an error whose `code` is `internal`.

The following give exit code 4:

| Applies to | Usage error |
|---|---|
| Every command | An unknown command or option |
| Every command | No configuration file name, or 2 or more files |
| Every command | An option given twice (`--disable` and `--enable` can be repeated) |
| Every command | `--describe-timeout-ms` without a value |
| `check` | `--events`, `--stdin-commands` |
| `check` | `--log-dir`, `--timeout-ms`, `--speed`, `--start-paused`, `--control` and `--control-*` |
| `run` | `--json` and `--events` together |
| `run` | `--text-from-stdin`, `--descriptions`, `--write-descriptions` (they are for `check`) |
| `run` | `--events-stall-ms` without `--events`, or with a value out of range |
| `run` | `--start-paused` with neither `--stdin-commands` nor `--control` |
| `run` | `--speed` that is neither a number nor `max` |
| `run` | `--scenario` together with `--start-paused` |
| `--control` | `--control-credentials` and the like without `--control` |
| `--control` | `--control` in a build without the Control Server |
| `--control` | An invalid value of `--control` |
| `--control` | A `--control-*` file that cannot be read |
| `--control` | A `--control-credentials` file with no credentials in it |
| `--control` | Only one of `--control-tls-cert` and `--control-tls-key` |
| `--control` | A non-loopback `--control` without TLS and credentials |
| `version` | Any option other than `--json` |
| Participant options | `--disable`, `--enable` or `--manual` with a participant name that is not in the configuration file |
| Participant options | `--disable` and `--enable` with the same name, or `--disable` and `--manual` with the same name |
| Variants and parameters | No `--variant` for a configuration file with variants, or `--variant` with a name that is not a variant |
| Variants and parameters | A malformed `--param`, `--param` without `--scenario`, or `--param` with a name that is not in the scenario |
| `test` | No `--scenarios`, a `--scenarios` path that does not exist, or a directory with no scenario files |
| `test` | Options for `run` only, and `--scenario`, given to `test` |
| `test` | `--text-from-stdin`, `--descriptions`, `--write-descriptions` (they are for `check`) |
| `test` | An enabled participant with `manual: true` in the configuration file given to `test` |

## See also

| To find | See |
|---|---|
| The arguments of ECU executables and the `--describe` JSON | [ecu-executable.md](ecu-executable.md) |
| The check order and the finding codes | [simulation-check.md](simulation-check.md) |
| Running, stopping and results | [simulation-run.md](simulation-run.md) |
| Using `run` from a program (events, stdin commands) | [simulation-events.md](simulation-events.md) |
| Steps to write a configuration file and run ECUs | [Integrate and run several ECUs](howto/integrate-ecus.md) |
