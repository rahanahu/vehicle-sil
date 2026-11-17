# Launcher, process, configuration and run

[日本語](ja/code-map-launcher.md)

<!-- Style: write each entry as a short block of the same shape (a subheading, a bullet list with one fact per line, a table whose cells are one sentence each). Do not pack several facts into one cell or one bullet: the line gets long and unrelated changes hit the same line. -->

This is the map for changes in this area. Start at [code-map.md](code-map.md); common test rules are in [code-map-testing.md](code-map-testing.md).

Related specifications and change areas: [simulation-config.md](simulation-config.md) / [simulation-check.md](simulation-check.md) / [simulation-run.md](simulation-run.md) / [simulation-events.md](simulation-events.md) / [Scenarios, timelines and batch runs](code-map-scenario.md) / [Control API, gRPC and launcher Control Server](code-map-control.md) / [Core, step, signal and SIL Kit defaults](code-map-core.md) / [ECU, Peer, local execution and Simulation](code-map-execution.md) / [Vector XL and Ethernet bridges](code-map-bridges.md)

## Test labels

### `launch` / `process`

| Test | Details |
|---|---|
| `launch.*` | Loading of configuration files, the check rules, human-readable and JSON output (starts no processes) |
| `launch_cli.*` | Runs `vehicle_sil_launch check` as an executable |
| `launch_run.*` | Runs `vehicle_sil_launch run` as an executable |
| `process.*` | Tests of child processes |

`launch.*` compares a passing and a failing example of each rule, down to the message text and line numbers.

What `launch_cli.*` checks:

- Byte-for-byte comparison with the examples in [docs/simulation-config.md](simulation-config.md)
- An executable whose `--describe` fails, `--strict`, an unreadable file, argument errors
- `launch_cli.connect`: `check --connect` with the two bridges (their `main()` on the fake pcap library and the fake XL driver) copied under their real names into `connect_tools/` of the build directory: found on `PATH` and next to a copy of `vehicle_sil_launch`, not in the working directory; `networks[].outside`; a bridge that refuses its arguments; `run --disable bridge.<network>` and `test --connect` refused
- `launch_cli.connections`: `connections` with the same bridges: the `--connect` lines, which `check` then takes; `--json` (the bridge's list passed on, LIN without one); `--network`; bridges that cannot list (`library-missing`, `driver-missing`) and no bridge (`tool-missing`, exit code 1); the command line (exit code 4) and a file that cannot be read (exit code 3). `launch.connections_*` (`test_connections.cpp`) covers the rest without processes: the quoting, reading a list, the lists the launcher makes, each bridge asked once, the text and the JSON whole

What `launch_run.*` checks:

- Comparison with the examples in docs
- A failing ECU, participants that do not all join, a participant that does not end, processes left behind, interruption, an abnormal exit of the launcher
- Speed, pause and stepping, lateness behind real time
- `launch_run.ethernet`: the check and scenario of the 2 ECUs of `examples/ethernet`
- `launch_run.test_command`, `launch_run.test_interrupt`: `vehicle_sil_launch test`
- `launch_run.live`: the check of `simulation.live.yaml`, the example configuration file that runs until stopped, and a run that goes past 1000 ms before it is stopped
- `launch_run.connect_*` (`launch_run_connect.cpp`): `run --connect` with the bridges of `connect_tools/` (the device `fake0` of the fake pcap library answers, the fake XL driver echoes on `virtual:1`): the bridges join before any ECU starts, the frames go both ways, a bridge that cannot start (`connect-failed`, seen by its process, not by the time limit), one that ends while the run waits for a participant started by hand, one killed or ended with exit code 0 while the simulation runs (`connect-lost`), the speed warnings with `--strict` and `--start-paused`, and `connect-time-changed` (one warning about the first pause of the run; with `--strict`, a speed command fails the run). Each waits for events or lines (no sleeps), and checks what must not happen in the whole output after the end

What `process.*` checks:

- Capture of stdout and stderr, line-by-line forwarding, timeouts
- Empty stdin, the working directory, handles that are not inherited
- Stop requests, ending each process that was started
- Paths and arguments that contain spaces and non-ASCII characters

## Code and tests by directory

### `libs/process`

| Name | Location |
|---|---|
| `ChildProcess`, `run_captured()`, `current_executable()` (the path of this process's executable) | `process/child_process.hpp` |
| `Process` | `process/process.hpp` |

- Tests: `tests/process`
- `ctest -R`: `^process\.`

### `libs/launch`, `tools/launch`

Public headers:

| Name | Location |
|---|---|
| `read_config()` | `launch/config.hpp` |
| `check_file()` | `launch/check.hpp` |
| `--connect`: `parse_connect()`, `target_problem()`, `bridge_arguments()`, `find_tool()`, where the launcher looks for a bridge, `no_bridge_here()` (LIN; CAN elsewhere than on Windows), and `bridge_working_directory()`, the bridge's own directory, where the check, `connections` and `run` start it | `launch/connect.hpp` |
| `vehicle_sil_launch connections`: `list_connections()`, `connections_of()` (each bridge asked once), `interpret_list()` (the bridge's list, or the one the launcher makes: `tool-*`), `quoted_for_shells()`, the outputs | `launch/connections.hpp` |
| `run_file()` | `launch/run.hpp` |
| `TimeCommands` | `launch/time_commands.hpp` |
| `RunTraffic` (the result's `traffic`) | `launch/traffic.hpp` |
| `EventStream`, `parse_command()` | `launch/events.hpp` |
| `read_scenario()`, which reads scenarios; `Waveform`, the interface for functions of time | `launch/scenario.hpp` |
| `ScenarioEngine` | `launch/scenario_run.hpp` |
| `TimelineRecord`, the record of `run --timeline`; `format_timeline_json()`, which writes the file | `launch/timeline.hpp` |

Implementation:

| What | Location |
|---|---|
| The overall flow of the check | `src/check.cpp` |
| Reading the `--describe` JSON | `src/describe_read.cpp` |
| An ECU as the simulation sees it (`SeenEcu`): what its executable described, with the participant's entry applied. The check, `run` and the scenario read a description only through it (its name, step, networks, variables, control offer, features) | `src/seen_ecu.hpp`, `src/seen_ecu.cpp` (with `seen_variables()`) |
| The reported networks under the names after `network_names` of the configuration file, with the J1939 nodes after `j1939_nodes` (`seen_networks()`) | `src/check_networks.cpp` |
| The check rules (one function per network kind) | `src/check_rules.cpp` |
| The check of `--connect` (`check_connections()`: the requests, the bridges found and their `--describe`); the rules that need the whole simulation (the bridge as a participant of its network, `connect-loop`) are in `check_rules.cpp` | `src/check_connect.cpp` |
| The forms of `--connect`, the bridges' arguments, `find_tool()` (next to the launcher, then `PATH`); the search of `PATH` that `find_tool()` and `find_executable()` share is `detail::find_on_path()` in `src/config.cpp` | `src/connect.cpp` |
| `connections`: the bridges' lists read (`TargetReader`) and passed on with every key (`write_node()`), the lists the launcher makes, the text and the JSON. The bridges run through `run_tool()` of `src/check.cpp`, as their `--describe` | `src/connections.cpp` |
| The check output | `src/check_format.cpp` |
| `run --connect`: the bridges as participants of the run only, after those of the file (`detail::with_bridges()`). Where they are started first and waited for (`RunStart::wait_for_bridges()`), and where their end stops the run (`detail::stops_run()` in `look_at_children()`, `RunWatch::watch()` and `gone_before_start()`): `run.cpp`, `run_participants.cpp`, `run_start.cpp`, `run_watch.cpp`. Their findings `connect-failed` and `connect-lost`: `run_assess.cpp`; `connect-time-changed`: the first command that changed the time, noted by `TimeCommands::apply()` under its lock (the commands' threads), read by `RunWatch::settle()` after `detach()` (the main thread), judged in `run_assess.cpp`; their last line: `RunOutput::on_line()` (the reader threads, under the output's lock) | `src/run_connect.cpp` |
| Code shared by the readers of configuration files and scenarios | `src/yaml_reader.hpp` |
| The overall flow of `run` | `src/run.cpp` |
| The classes that share the work of `run` | `src/run_*.cpp`, `src/time_commands.cpp` |
| This computer's addresses (the URI shown to other computers) | `src/host_addresses.cpp` |
| The watch of the running simulation (`RunWatch::watch()`), what is settled when it is over (`RunWatch::settle()`: the scenario's end, the timing, the traffic, how the simulation ended), and `StandStill`, which reports that virtual time does not advance | `src/run_watch.cpp` |
| The command line, and the stdin and stdout threads | `tools/launch/main.cpp` |
| The Control Server of `run --control` (only when `VEHICLE_SIL_BUILD_CONTROL_SERVER=ON`) | `tools/launch/control_server.cpp` |

The comment at the top of `run.cpp` and the class comments in each `run_*.hpp` describe the role of each class that shares the work of `run`, and which thread handles which state under which lock.

- Tests: `tests/launch`, `tests/multi_host`
- `ctest -R`: `^launch\.` (no processes), `^launch_cli\.` (`check`), `^launch_run\.` (`run`), `^multi_host\.`

`tests/multi_host` simulates 2 computers with Linux network namespaces. Where it cannot create the namespaces, it prints the reason and skips. With `VEHICLE_SIL_REQUIRE_MULTI_HOST=1`, it fails instead of skipping.

### `tools/common`

Command-line handling shared by the executables (target `vehicle_sil_cli`).

| What | Location |
|---|---|
| Argument handling | `vehicle_sil/cli/args.hpp` |
| `write_text()`, which writes UTF-8 text to stdout and stderr (UTF-16 to a Windows console) | `vehicle_sil/cli/console.hpp` |
| Handling arguments and paths as UTF-8 | `vehicle_sil/cli/utf8.hpp` |

- Tests: `tests/unit`
- `ctest -R`: `^unit\.`

### `examples/integrate_ecus` (example for the guide "Integrate and run several ECUs")

| File | Details |
|---|---|
| `dashboard_v1/main.cpp` | The same code as the dashboard ECU of `examples/ecu_main`, except that the network name is `VehicleCAN` |
| `simulation.yaml` | The engine and this ECU (`check` reports `unknown-network`) |
| `simulation.two-buses.yaml` | `VehicleCAN` added to `networks` |
| `simulation.fixed.yaml` | The dashboard ECU replaced with the one of `examples/ecu_main` |
| `simulation.network-names.yaml` | `simulation.yaml` with `network_names` on the dashboard ECU |
| `simulation.second-engine.yaml` | `simulation.fixed.yaml` with a second engine ECU, changed with `j1939_nodes` |
| `scenarios/engine_speed.yaml` | Scenario |

- Build: with `VEHICLE_SIL_BUILD_EXAMPLES`, the executable and the YAML files go to `build/<preset>/examples/integrate_ecus/`. The engine and the rebuilt dashboard ECU are used from the neighboring `examples/ecu_main/` through relative paths.
- `ctest -R`: `^launch_docs\.`

What `launch_docs.integrate_ecus` (`tests/launch/integrate_ecus.cmake`) compares:

- The files with the guide's blocks
- The keys of the `--describe` and `check --json` excerpts
- `check` of the 4 configuration files, `--disable Dashboard`
- The result of `run` and the output of the dashboard ECU
- `run` with a scenario

### `simulation.two-pcs.yaml` in `examples/ecu_main` (example for the guide "Run on several computers")

- Tests: `multi_host.docs` compares this file and its `check` output with the blocks of the guide [howto/multi-pc.md](howto/multi-pc.md).
- `ctest -R`: `^multi_host\.docs`

### `schema/` (JSON Schemas for configuration files and scenarios)

| File | Details |
|---|---|
| `schema/simulation.schema.json` | Configuration file |
| `schema/scenario.schema.json` | Scenario |
| `schema/timeline.schema.json` | The file that `run --timeline` writes. It describes launcher output, for the tools that read it. It is in `schema/` of the tools archive, not in the template's `template/schema/` |
| `gui/protocol/dashboard.schema.json` | Dashboard |

The configuration file and scenario schemas are for completion and checking in editors; the launcher does not read them. The code that the launcher uses to read files is `libs/launch/src/config.cpp` and the scenario's reader (`scenario_reader.hpp` and the files it names).

| Where the copies are | Details |
|---|---|
| `template/schema/` | Copies of the 3 schemas |
| `schema/` in the tools archive | The same 3 and `timeline.schema.json` (`template/get_tools.py` copies all of them to the project's `schema/`) |
| Build directory | Copied at configure time when the examples are built (top-level `CMakeLists.txt`) |

Line 1 of an example YAML file copied to the build directory points to the schema copied there.

- Tests: `tests/schema/test_schemas.py`
- `ctest -R`: `^schema\.` (the part that needs no launcher also runs in `schema` of `scripts/verify-python.py`)

What `test_schemas.py` checks:

- The YAML files of the examples, the template and the documentation pass the schemas
- Passing and failing samples are run through both the schemas and `check`
- The keys of each mapping, the network kinds and the step kinds are the same as those that the launcher's error messages list
- The keys are in the tables of the documentation
- The template's copies are identical
- Line 1 of the example and template YAML files points to a schema
- A schema copy exists where line 1 of an example copied to the build directory points
- The reference timeline files (`tests/launch/timeline/`), the fixtures of the timeline page (`gui/timeline/tests/fixtures/`) and the timeline file of an example run pass the timeline schema, and malformed files fail it

This test needs a Python with PyYAML and jsonschema (`uv sync`; on Windows, `mingw-w64-ucrt-x86_64-python-jsonschema` of MSYS2). Without them, configure prints the reason and does not register the test. CI confirms that `schema.files_and_launcher` is registered in `linux-gcc-debug`.

## What to update together

### `check` and `run` output

Where to change:

- The output of `vehicle_sil_launch check` and `run` (`libs/launch/src`)
- `examples/ecu_main/simulation.yaml`

Documentation to update:

- [simulation-config.md](simulation-config.md), [simulation-check.md](simulation-check.md), [simulation-run.md](simulation-run.md), [simulation-events.md](simulation-events.md): the code blocks directly after a `<!-- ... -->` line (compared by `launch_cli.docs` and `launch_run.docs`)
- The guides in [howto/](howto/): the copies of the blocks with the same markers (next entry)

Code and files to update:

- `examples/ecu_main/simulation.live.yaml` (no `stop_at_ms`, `speed: 1`; the configuration file for working from the GUI): `launch_run.live` confirms that it has the same participants and networks as `simulation.yaml`.
- `template.check_and_run` checks the template's file of the same name.
- `simulation.live.dashboard.yaml` in the build directory is a copy of `simulation.dashboard.yaml` made by CMake.
- The recordings of launcher output that the GUI tests replay (`gui/server/tests/recordings/`): record them again with `gui/server/tests/record_launcher.py --launcher <vehicle_sil_launch>`.
- `scripts/verify-gui.py --launcher <path>` finds mismatches between the recordings and the real launcher.

For how the blocks are compared and where the examples with errors are placed, see "[Documentation examples and tests](code-map-testing.md#documentation-examples-and-tests)".

### Output and code copied into the guides

Where to change:

- The output of `check` and `run` (same as the previous entry)
- The code of `examples/ecu_main` (including `scenarios/`) and `examples/launch_control`

Documentation to update:

- The guides in [howto/](howto/): the output code blocks (`launch_cli.docs` and `launch_run.docs` compare them with the reference blocks that have the same markers)
- The guides in [howto/](howto/): code quoted without a marker (not compared; keep it in sync by hand)

For the markers and tests of each guide, see "[Documentation examples and tests](code-map-testing.md#documentation-examples-and-tests)".

### Check rules and `code`

Where to change:

- `libs/launch/src/check_rules.cpp`
- The code that reads configuration files is `config.cpp`

Documentation to update:

- The table of `code` values in "[Checks](simulation-check.md#checks)" in [simulation-check.md](simulation-check.md) (no test compares this table; keep it in sync by hand)

### Configuration file or scenario keys

Where to change:

- Keys of configuration files or scenarios, value forms, network kinds, step kinds (the code that reads files in `libs/launch/src/config.cpp` and the scenario's reader, `scenario_reader.hpp` and the files it names)

Code and files to update:

- `schema/simulation.schema.json`, `schema/scenario.schema.json`: give every key a `description`. Editors show it as the description of the key.
- The template's copies: `cp schema/*.schema.json template/schema/` (the dashboard schema comes from `gui/protocol/dashboard.schema.json`)
- The lists that the unknown-key error messages use (`k...Keys` at the top of `config.cpp` and `scenario_reader.hpp`)
- The samples in `tests/schema/test_schemas.py` (`CONFIG_SAMPLES`, `SCENARIO_SAMPLES`)

How the key branches relate to the lists:

- Most of the code that reads files accepts keys in branches (`key == "..."`) and prints a separate list in the unknown-key message.
- When you add a branch, add the key to the list too.
- The tests compare the list, not the branches. If you forget the list, the schema passes the tests without the new key.

To add a step kind:

1. Add one `step_<kind>` to `definitions` in `scenario.schema.json` (the keys of that kind and the 2 time keys).
2. Add one line to `oneOf` of `definitions.step`.

Rules:

- The schema must not reject a file that the launcher reads (`check` reports the errors that the schema cannot reject).
- Where in doubt, make the schema loose, and add a passing and a failing form to the samples in `test_schemas.py`.
- When you add an example or template YAML file (`examples/`, `template/`), write `# yaml-language-server: $schema=<relative path to the schema>` on line 1.
- Do not change the number of lines of the comments at the top of existing examples. The line numbers of findings and steps appear in the output in the documentation and in the recordings that the GUI tests replay.

Tests:

- `schema.files_and_launcher`

### `network_names` in the configuration file

`network_names` is a participant's network name map.

Where to change:

- Reading it, and the errors that the configuration file alone shows: `read_network_names()` and `check_network_names()` in `libs/launch/src/config.cpp`
- Merging the lines of a variant: `lay_network_names()`; turning them into arguments: `map_network_arguments()`
- The network names that the check rules and the output look at

Rules:

- The check rules, the tables and `networks` in the JSON read the reported networks only through `seen_networks()` (`libs/launch/src/check_networks.cpp`, the only function that applies the map), which `SeenEcu::seen_networks()` (`libs/launch/src/seen_ecu.hpp`) calls. A rule that reads `description.networks` itself sees a renamed network under the executable's name. `launch.described_networks_are_read_in_one_place` (`cmake/VehicleSilCheckDescribedNetworks.cmake`) reads the lines with `.networks`, `->networks` and `::networks` in `libs/launch` and `tools/launch` to confirm this. Only `config.networks` of the configuration is not counted, so a member named `networks` in another struct makes the test fail: give it another name. When you add a rule or an output, add the case of a renamed network to `tests/launch/test_check.cpp`.
- `detail::participant_arguments()` (`libs/launch/src/run_participants.cpp`) is the one place that builds the arguments of `run`. The start, the command of `manual` and the `participant` event use the same list (`--map-network` comes last; with `describe: false`, after the placeholders are replaced).

Documentation to update:

- "[Matching network names (`network_names`)](simulation-config.md#matching-network-names-network_names)", the key table and the variant table in [simulation-config.md](simulation-config.md)
- `network-map-*`, `unknown-network`, `single-participant` and `network_map` in the JSON in [simulation-check.md](simulation-check.md)
- `network-names` in `features` in [simulation-events.md](simulation-events.md)
- The schema (`schema/simulation.schema.json` and its copy in the template)

Tests:

- `^launch\.` (`test_config.cpp`, `test_check.cpp`, `test_run.cpp`)
- `launch_run.network_names`: ECUs with different names communicate on one bus, a variant changes the map, the command of `manual` and the event, an executable that cannot take `--map-network`
- `schema.files_and_launcher`

### `j1939_nodes` in the configuration file

`j1939_nodes` gives a participant's J1939 nodes other addresses and NAME fields than its executable's code.

Where to change:

- Reading it, and the errors that the configuration file alone shows: `read_j1939_nodes()` and `check_j1939_nodes()` in `libs/launch/src/config.cpp`
- Merging the entries of a variant: `lay_j1939_nodes()`; turning them into arguments: `j1939_node_arguments()` (the text of an entry is `ecu_executable::format_j1939_node_argument()`, which `ecu_main` reads back)
- Applying the entries to the reported nodes: `seen_networks()` in `libs/launch/src/check_networks.cpp`, with `j1939::apply_node_overrides()`
- The findings about the entries, and how `address-conflict`, `address-shared` and `name-conflict` say to fix them: `check_j1939_node_entries()` and `conflict_fix()` in `libs/launch/src/check_rules.cpp`
- The `features` word that each setting needs: `kJ1939NodeSettingFeatures` in `vehicle_sil/launch/config.hpp` (a setting added later gets its own word)

Rules:

- `seen_networks()` is the only place that applies the entries in the launcher. The rules read the outcome (`SeenNetwork::j1939_matches`, `unmatched_j1939_entries()`) and do not match again.
- The check and the executable apply the same rules (`vehicle_sil/j1939/node_override.hpp`). `launch.j1939_nodes_match_like_the_network_does` passes the same scenes through `seen_networks()` and a `j1939::Network`.
- An entry that takes no node keeps that participant's nodes on that network out of the rules of the addresses and NAMEs (`Rules::j1939_held`).

Documentation to update:

- "[Matching J1939 addresses and NAMEs (`j1939_nodes`)](simulation-config.md#matching-j1939-addresses-and-names-j1939_nodes)", the key table and the variant table in [simulation-config.md](simulation-config.md)
- `j1939-node-*`, `address-conflict`, `address-shared`, `name-conflict`, the participant table and `j1939_nodes` in the JSON in [simulation-check.md](simulation-check.md)
- `j1939-nodes` in `features` in [simulation-events.md](simulation-events.md)
- The schema (`schema/simulation.schema.json` and its copy in the template)

Tests:

- `^launch\.` (`test_config.cpp`, `test_check.cpp`, `test_run.cpp`, `test_j1939_nodes_match.cpp`)
- `launch_run.j1939_nodes`: 2 copies of one ECU communicate with an entry, the check says which entry to write without one, a variant changes the entry, the command of `manual` and the event, a `describe: false` participant, an executable that cannot take `--j1939-node`
- `schema.files_and_launcher`

### Events, stdin commands, `version --json`

Where to change:

- The events of `run --events`, the commands of `--stdin-commands`, `version --json` (`libs/launch/src/events.cpp`, `tools/launch/main.cpp`)

Documentation to update:

- [simulation-events.md](simulation-events.md) ("Programmatic use"): the table of events and `state` values, the table of commands, the `features` table
- [simulation-events.md](simulation-events.md) ("Programmatic use"): the event examples (`launch_run.events` compares them byte for byte after substitution)
- "[Slow readers and readers that stop reading](simulation-events.md#slow-readers-and-readers-that-stop-reading)" in [simulation-events.md](simulation-events.md) ("Programmatic use"), when you change the invariants below
- [simulation-config.md](simulation-config.md): the table of arguments, when you change `--events-stall-ms`

Decisions for each change:

- When you add an event or a key, decide whether to add a name to `features`.
- If you change a meaning, raise `kEventsFormat`.
- Keep `kFeatures` in `events.hpp` and the `features` table in the documentation in sync by hand.
- When you add a `state` of `participant`, add it to the `state` table.
- When you add a finding, review `detail::settled_findings()` (whether it may be streamed midway).

Rules:

- The invariants of the thread that writes events and of the queue are described in the comment of `EventStream` in `events.hpp`. Make the documentation match it.
- The invariants are: the thread that produces events does not wait for the reader; only `log` may be dropped; and the time after which the launcher stops waiting for a reader that does not read.
- That time is set by `--events-stall-ms` (`events_stall_timeout()` and `kMinEventsStallMs` in `tools/launch/main.cpp`; the default is `EventStreamOptions::stall_timeout`).

Tests:

- `^launch\.` (`test_events.cpp`)
- `launch_run.events_stalled`, `events_slow`, `events_nonblocking`

### Timing of `run`

Speed, pause, stepping and lateness behind real time.

Where to change:

- `TimeCommands` in `libs/launch/src/time_commands.cpp`
- `detail::plan_time_step()` (the default time step of the controller) and `RunTiming::actual_speed()` in `run_assess.cpp`
- `RunNotes::note_time()` in `run_notes.cpp` (how `paused` of `time` is decided: `detail::noted_paused()` in `run_notes.hpp`)
- `RunWatch::note_timing()` in `run_watch.cpp`
- The time gate: `libs/core/src/simulation_controller.cpp`
- The gate's state and the lateness calculation: `libs/core/src/time_gate.cpp` (it has no clock)
- Waiting until a deadline: `libs/core/src/deadline_waiter.cpp`

Documentation to update:

- "[Running at a set speed (`speed`)](simulation-run.md#running-at-a-set-speed-speed)" in [simulation-run.md](simulation-run.md) (the definition of lateness, what happens when the run is late, time steps and ECU steps)
- [simulation-run.md](simulation-run.md): the table of configuration file keys, the `time` and `command` events, the table of commands, the `timing` table, the `features` table
- [simulation-run.md](simulation-run.md): the table of findings (`behind-real-time`, `coarse-time-step`)
- "[Pause, step and speed (time gate)](internals/virtual-time.md#pause-step-and-speed-time-gate)" in [internals/virtual-time.md](internals/virtual-time.md), when you change the definition of lateness
- simulation-config.md, when you change the definition of lateness

Rules:

- The definition of lateness is described in the comment of `TimeControlOptions` (`simulation_controller.hpp`). Make the documentation match it.
- The thread that reads stdin passes commands to the gate (without waiting). Do not add anything that waits inside the lock of `TimeCommands`.
- Check lateness numbers with the tests `core.time_gate_model_*`, examples with fixed times. Tests that use real time make one-sided checks only.

Tests:

- `core.time_gate_model_*`, `core.virtual_time_gate_*`, `core.*deadline_waiter*`
- `^launch\.`
- `launch_run.realtime`, `on_time`, `late`, `stall`, `speed_change`, `step_width`, `pause_step`, `paused_failures`, `time_usage`, `connect_time_changed`

### Participants you start yourself

`manual` and `--manual`. `run` does not own the process, so it learns of a failure only when the participant leaves the simulation.

Where to change:

- The code that reads them: `libs/launch/src/config.cpp`
- The code that prints a command instead of starting the process: `RunParticipants::show_manual()` and `detail::shell_command_line()` in `libs/launch/src/run_participants.cpp`
- Waiting for the start: `RunStart::wait_for_start()` in `run_start.cpp`
- The end: `RunParticipants::end_manual()`

Documentation to update:

- "[Starting a participant yourself (`manual`, `--manual`)](simulation-config.md#starting-a-participant-yourself-manual---manual)" in [simulation-config.md](simulation-config.md) and its examples (compared by `launch_run.manual`)
- [simulation-events.md](simulation-events.md): `manual` of the `participant` event, `manual-participants` of `features`
- "[Run the ECU from a debugger](howto/first-ecu.md#run-the-ecu-from-a-debugger)" in [howto/first-ecu.md](howto/first-ecu.md) (shows the same stderr lines and result table with the name `Cooler`; not compared, so keep it in sync by hand)
- The start deadline in "[The launcher's Control Server (`vehicle_sil_launch run --control`)](internals/control.md#the-launchers-control-server-vehicle_sil_launch-run---control)" in [internals/control.md](internals/control.md), when you change the values below

If you change the start limit, the `--start-timeout-ms` passed to the ECUs (`detail::ecu_start_timeout_ms()`) or the default registry port (`kManualRegistryUri`), update the same section of simulation-config.md and the start deadline in internals/control.md.

Tests:

- `launch_run.manual` (the example in docs)
- `launch_run.manual_*` (one per situation; `ctest -R '^launch_run\.manual'`)
- `launch.a_participant_started_by_hand_from_the_file_or_the_command_line`
- `launch.the_command_to_start_a_participant_by_hand_is_quoted_for_the_shell`

### Run traffic (`traffic`)

`traffic` in the run result: the J1939 frames sent and received on each network, and `delivered`.

Where to change:

| What | Location |
|---|---|
| The tap and the scenario's queue | `RunSamples` in `libs/launch/src/run_samples.cpp` |
| The latest few counts kept for each participant | `TrafficLatest` in `libs/launch/src/run_traffic.cpp` |
| Building the result | `count_traffic()` |
| `senders`, `receivers` and `delivered` of a network | The functions of `TrafficNetwork` |
| The network names and the positions of the counters, from the schema of the hello | `traffic_schema()` |
| Who is on which network | `traffic_plan()` (from `seen_networks()`) |
| Reading at the end | `RunWatch::settle()` in `run_watch.cpp` (`RunSamples::finish()`), which `Runner::run()` calls after `RunWatch::watch()` |
| The text and the JSON | `traffic_text()` and `write_run_json()` in `run_format.cpp` |

Documentation to update:

- "[Network traffic (`traffic:`)](simulation-run.md#network-traffic-traffic)" in [simulation-run.md](simulation-run.md) (how to read the line, how the counts are taken, what `sent` means, the table of reasons for not counting, the cases that differ from run to run), and the `traffic` table in "JSON"
- The `run` output examples in `simulation-run.md`, `scenario.md`, `simulation-config.md`, `simulation-events.md`, `howto/first-ecu.md` and `howto/scenario-tests.md` (the blocks that `launch_run.docs`, `launch_run.scenario` and others compare)
- `run-traffic` in `features`
- "[Samples the launcher receives (`RunSamples`)](internals/control.md#samples-the-launcher-receives-runsamples)" in [internals/control.md](internals/control.md)
- The GUI's recordings (`gui/server/tests/recordings/`) and `gui/protocol/examples/`

Rules:

- Inside the tap, only replace a participant's sets of counts and push onto the scenario's queue (no waiting, no call to the Hub).
- Keep no queue for `traffic`: no state that overflows while nobody takes anything out.
- The counts are the values of "the last step that started before the end time", so that the output does not depend on whether later samples arrived.
- Find the counters by the names that the Agent reported (`PeerInfo::sources`), and do not count those that a map applies to.
- Do not state the line as ratios (a participant that only receives is normal). `delivered` is `false` only when the counts alone settle it: every participant of the run that may be on the network is counted. A participant without `--describe` may be on every network for which it has no counters (`TrafficNetwork::unplaced`; the counters say where it is, not where it is not), so it makes `delivered` of those networks `null`, and the line then gives numbers, not `nothing sent`. `RunTraffic::unplaced` lists those that are known on no network. Each entry has one of two reasons: no hello by the end of the run (which may differ from run to run), or no counters (which does not).
- Do not change the samples passed to the scenario (everything that the tap received after the scenario started, from the Agents of that moment; no limit; `RunSamples::scenario_begins()`).

Tests:

- `launch.the_numbers_are_those_of_the_last_step_*`, `launch.when_the_samples_come_*`, `launch.only_a_few_samples_are_kept_*`, `launch.a_flood_while_nobody_looks_*`, `launch.a_network_says_whether_*`, `launch.every_network_of_the_file_*`, `launch.a_participant_that_is_not_counted_says_why` (`tests/launch/test_traffic.cpp`)
- `launch.the_text_for_people`, `launch.the_json_report`
- `launch_run.traffic`: the same output in every run with the 2 ECUs of the example, buses split and joined with `network_names`, an ECU that declares no variables, participants that are not counted, a network on which nobody is known to be
- `^launch_run\.scenario`, `control_server.launcher`

### Launcher limits

Where to change:

- `kMaxMillis`, `kMinSpeed`, `kMaxSpeed` (`libs/launch/include/vehicle_sil/launch/config.hpp`)

Code to update:

- Copies in the GUI: `gui/server/src/vehicle_sil_gui/launcher.py`, `variables.py`
- Copies in the GUI: `gui/client/src/client.ts`, `protocol/server.ts`
- Copies in the GUI: `gui/web/src/app/run/speed-picker.ts`

Tests:

- `launch.the_gui_takes_the_launchers_limits` finds mismatches.
- The `static_assert`s in the same test file check the exit codes and limits of the other tools (`tools/common/include/vehicle_sil/cli/args.hpp`).
