# Launcher, process, configuration and run

[日本語](ja/code-map-launcher.md)

This is the map for changes in this area. Start at [code-map.md](code-map.md); common test rules are in [code-map-testing.md](code-map-testing.md).

Related specifications and change areas: [simulation-config.md](simulation-config.md) / [simulation-check.md](simulation-check.md) / [simulation-run.md](simulation-run.md) / [simulation-events.md](simulation-events.md) / [Scenarios, timelines and batch runs](code-map-scenario.md) / [Control API, gRPC and launcher Control Server](code-map-control.md) / [Core, step, signal and SIL Kit defaults](code-map-core.md) / [ECU, Peer, local execution and Simulation](code-map-execution.md) / [Vector XL and Ethernet bridges](code-map-bridges.md)

**On this page:** [Test labels](#test-labels) / [Code and tests by directory](#code-and-tests-by-directory) / [What to update together](#what-to-update-together)

**Change topics:** [`check` and `run` output](#check-and-run-output) / [Output and code copied into the guides](#output-and-code-copied-into-the-guides) / [Check rules and `code`](#check-rules-and-code) / [Configuration file or scenario keys](#configuration-file-or-scenario-keys) / [`network_names` in the configuration file](#network_names-in-the-configuration-file) / [`j1939_nodes` in the configuration file](#j1939_nodes-in-the-configuration-file) / [Events, stdin commands, `version --json`](#events-stdin-commands-version---json) / [Timing of `run`](#timing-of-run) / [Participants you start yourself](#participants-you-start-yourself) / [Run traffic (`traffic`)](#run-traffic-traffic) / [Launcher limits](#launcher-limits)

## Test labels

### `launch` / `process`

For scenario, timeline or batch changes, see the [scenario change checklists](code-map-scenario.md#what-to-update-together).

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
- `launch_cli.request`: `check --request` and `run --request` with a request file: the same report as with the arguments, an argument wins, the file's scenario is read beside the configuration file, a file that is not named is not read, and a file with a mistake is a usage error.
- `launch_cli.connections`: `connections` with the same bridges: the `--connect` lines, which `check` then takes; `--json` (the bridge's list passed on, LIN without one); `--network`; bridges that cannot list (`library-missing`, `driver-missing`) and no bridge (`tool-missing`, exit code 1); the command line (exit code 4) and a file that cannot be read (exit code 3). `launch.connections_*` (`test_connections.cpp`) covers the rest without processes: the quoting, reading a list, the lists the launcher makes, each bridge asked once, the text and the JSON whole

What `launch_run.*` checks:

- Comparison with the examples in docs
- A failing ECU, participants that do not all join, a participant that does not end, processes left behind, interruption, an abnormal exit of the launcher
- Speed, pause and stepping, lateness behind real time
- `launch_run.ethernet`: the check and scenario of the 2 ECUs of `examples/ethernet`
- `launch_run.test_command`, `launch_run.test_interrupt`: `vehicle_sil_launch test`
- `launch_run.live`: the check of `simulation.live.yaml`, the example configuration file that runs until stopped, and a run that goes past 1000 ms before it is stopped
- `launch_run.connect_*` (`launch_run_connect.cpp`): `run --connect` with the bridges of `connect_tools/` (the device `fake0` of the fake pcap library answers, the fake XL driver echoes on `virtual:1`): the bridges join before any ECU starts, the frames go both ways, a bridge that cannot start (`connect-failed`, seen by its process, not by the time limit), one that ends while the run waits for a participant started by hand, one killed or ended with exit code 0 while the simulation runs (`connect-lost`), the speed warnings with `--strict` and `--start-paused`, the time commands of a run that keeps real time (`connect_time_locked`: refused with `outside-real-time` in their order, also those written before the configuration is read, and the state says the same), `outside-time-changed` with `--time-policy free` (`connect_time_changed`: one note about the first pause of the run; `--strict` passes), and the counts (`connect_roundtrip`: the result, the events `outside` and the bridges' last statistics lines in their logs agree, and of the bridges' statistics lines only the last is a `log` event, once; `connect_nothing_received`: `fake1` and a channel without an echo give `connect-nothing-received` for both networks, a note that `--strict` passes, with `--json` too). Each waits for events or lines (no sleeps), and checks what must not happen in the whole output after the end

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
| What the launcher asks the computer it runs on: `find_executable()` and `check_executable()` (a participant's executable), `find_tool()` and `bridge_tools()` (a bridge), `run_tool()` (one run of `--describe` or of a bridge's list), `read_config_file()`, `read_scenario_file()`, `files_beside()`, `environment_value()` | `launch/probe.hpp` |
| `--connect`: `parse_connect()`, the table of backends (`connect_backends()`) and the grammar of a target (`read_connect_target()`, `backend_problem()`, `target_problem()`), `bridge_tool()` of a backend, `bridge_arguments()`, `connect_device_key()` (the device without the backend), `no_bridge_here()` (LIN; the Vector XL bridge elsewhere than on Windows), and `bridge_working_directory()`, the bridge's own directory, where the check, `connections` and `run` start it | `launch/connect.hpp` |
| `vehicle_sil_launch connections`: `list_connections()`, `connections_of()` (each bridge asked once for each backend; `prefix_targets()` gives a value that `--connect` takes), `interpret_list()` (the bridge's list, or the one the launcher makes: `tool-*`), `check_requested()` (`--request`: whether the request file's targets are in the lists, by backend and name; the file is read and cut to the configuration by the functions of the request-file row), `quoted_for_shells()`, the outputs | `launch/connections.hpp` |
| `run_file()` | `launch/run.hpp` |
| What a run saw, as a value in parts: `RunOutcome` with `setup`, `start`, `end` (how the simulation ended and why), `control` (what the control agents dropped), `scenario` (the scenario and its timeline file) and `close` (how the launcher's connection closed), then `timing`, `participants`, `outside`, `time_changed`, `time_lock`, `real_time_peers`, `traffic`. `assess_run()` judges it; `format_run_text()` and `format_run_json()` write it | `launch/run.hpp` |
| What is settled about a run before it begins, as a value: `RunPlan` (the check that passed with the bridges of `--connect` at the end, `members`: one `RunMember` for each participant, which says whether the run added it as a bridge of `--connect` and what it connects, the directory of the file, the controller's step and the speed the run starts at, whether it holds before the first step, the limits, where the logs and the timeline go), `plan_run()` (pure: from the check, the request and `RunLimits`) and `planned_outcome()` (the `RunOutcome` a run begins with). `run_file()` is given the request (`RunRequest`), the observers (`RunObservers` in `launch/run.hpp`: `write_line`, `events`, `interrupt_name`, `before_start`), what the run borrows (`RunLent`: the controls or the stop requests, a control server) and the waits that only tests shorten (`RunLimits`); it makes this plan of the request and the limits and does not keep the request, so no thread of the run reads it. The parts of a run read the plan, the observers and the controls through `detail::RunContext`. Tested without a process in `tests/launch/test_run_plan.cpp` | `launch/run_plan.hpp`, `src/run_plan.cpp` |
| What a check, a run or a test is asked to do, as a value: `RunRequest` in two parts, `RunIntent` (the user's: variant, parameters, scenario as named, `--disable`, `--enable`, `--manual`, `--connect`, `--time-policy`, speed, `--start-paused`, `--strict`) and `RunHost` (the caller's: the file as named, where the tools and the logs are, the `--describe` and run limits, the timeline's file as named, the control server that is asked for (its address and the names of its files), whether standard-input commands are asked for). `validate_request()` (pure) holds the rules about fields that do not go together (`--param` and `--timeline` need a scenario, `--start-paused` with a scenario or without a way to go on), as findings of code `usage`; `check_file()`, `run_file()` and `test_file()` (in `TestOptions::request`) take it and make the paths of the names; `CheckOptions` has only what is not asked for (`file_only`, `params_for_many`, `fastest_by_default`). `read_request()` and `run()` in `tools/launch/main.cpp` read the arguments in the order their mistakes are reported in and call `validate_request()` at 3 points, refusing the first finding (with `--request`, once after every argument is read). Tested without a process in `tests/launch/test_request.cpp` (the rules, and what the check and the plan read of each field) | `launch/request.hpp`, `src/request.cpp` |
| The request file of `--request` (the intent as JSON): `read_request_file()` reads its text (pure; the one reader of the format, built on `YamlReader` so that an unknown key and a key written twice read as in the other files), `with_request_file()` lays the arguments over it field by field and `merge_connect()` does so for `connect`. `RunRequest::origin` (`RequestOrigin` in `launch/config.hpp`: the file's name, the keys and the networks whose value is the file's) is what the messages of `validate_request()` and `time_lock_findings()` name the file from. `connect_for_config()` (`launch/check.hpp`, in `check.cpp`) leaves out the file's networks that the configuration lacks (`connect-network-unused`); `check_file()` reads the file's scenario from the directory of the configuration file. `probe.cpp` reads the file (`read_request_file_text()`); `tools/launch/main.cpp` only names it (`read_request_argument()`). Tested without a process in `tests/launch/test_request_file.cpp`, with the launcher in `launch_cli.request`, `launch_cli.connect` and `launch_run.connect_request` | `launch/request.hpp`, `src/request_read.cpp`, `src/request.cpp` |
| `StopRequests`, the requests to stop that the caller of `run_file()` or `test_file()` owns and lends (the count the run reads, and the source of the first request); `StopKind` (operator, signal, supervisor), the four sources the library knows (`stop_source::kStopCommand` and the others) with their names, and `stop_kind()`. The signal and console-event names stay in the CLI | `launch/stop_requests.hpp` |
| `RunControls`, the controls of one run, which the run borrows from its caller or makes for itself: the stop requests (lent), the `TimeCommands`, and the phase it keeps and tells its observer; `Phase`, `phase_name()`. `RunState`, the run's state as the event `state` writes it, which `RunControls::publish()` puts together from those three and tells its observer when it has changed | `launch/run_controls.hpp` |
| `TimeCommands` | `launch/time_commands.hpp` |
| The rules of the controls: `decide()` (whether the run takes an operation now, by the entry it came in and where the run stands; the table of the rows is in `src/control_rules.cpp`), `allowed()` (the same answer without arguments, with the range of the argument), `always_refused()` (whether a control server only shows the run), the reason codes `reason::*`. Pure | `launch/control_rules.hpp` |
| `RunTraffic` (the result's `traffic`) | `launch/traffic.hpp` |
| `EventStream`, `parse_command()` | `launch/events.hpp` |
| `Command`, a command as it was read and as `TimeCommands` decided it | `launch/command.hpp` |
| `read_scenario()`, which reads scenarios; `Waveform`, the interface for functions of time | `launch/scenario.hpp` |
| `ScenarioEngine` | `launch/scenario_run.hpp` |
| `TimelineRecord`, the record of `run --timeline` | `launch/timeline.hpp` |
| `format_timeline_json()`, which writes the file of `run --timeline` from the record and the scenario's result | `launch/timeline_format.hpp` |

Implementation:

| What | Location |
|---|---|
| The overall flow of the check | `src/check.cpp` |
| Looking at the computer: the file system (where an executable or a bridge is, the configuration, scenario and points files), the environment (`PATH`, `VEHICLE_SIL_LISTEN_ON_NETWORK`) and the processes run for their output. The rules get what was found as values (`executable_finding()` in `config.cpp` gets the file's status) or as functions (`ConnectTools`, `ScenarioContext::read_file`); one exception left: `bridge_working_directory()` in `connect.cpp` makes a relative directory absolute | `src/probe.cpp` |
| Reading the `--describe` JSON | `src/describe_read.cpp` |
| An ECU as the simulation sees it (`SeenEcu`): what its executable described, with the participant's entry applied. The check, `run` and the scenario read a description only through it (its name, step, networks, variables, control offer, features) | `src/seen_ecu.hpp`, `src/seen_ecu.cpp` (with `seen_variables()`) |
| The reported networks under the names after `network_names` of the configuration file, with the J1939 nodes after `j1939_nodes` (`seen_networks()`) | `src/check_networks.cpp` |
| The check rules (one function per network kind) | `src/check_rules.cpp` |
| The check of `--connect` (`check_connections()`: the requests, the bridges found and their `--describe`); the rules that need the whole simulation (the bridge as a participant of its network, `connect-loop`) are in `check_rules.cpp` | `src/check_connect.cpp` |
| The forms of `--connect` and the bridges' arguments. `find_tool()` (next to the launcher, then `PATH`) and the search of `PATH` that it shares with `find_executable()` (`detail::find_on_path()`) are in `src/probe.cpp` | `src/connect.cpp` |
| `connections`: the bridges' lists read (`TargetReader`) and passed on with every key (`write_node()`), the lists the launcher makes, the text and the JSON. The bridges run through `run_tool()` of `src/probe.cpp` (`bridge_tools()`), as their `--describe` | `src/connections.cpp` |
| The check output | `src/check_format.cpp` |
| `run --connect`: the bridges as participants of the run only, after those of the file (`detail::with_bridges()`). The mark of such a bridge is `RunMember::outside` in the plan (`RunPlan::members`, made by `plan_run()`), not a field of the configuration: a participant whose role in the file is `bridge` has none. Where they are started first and waited for (`RunStart::wait_for_bridges()`), and where their end stops the run (`detail::stops_run()` in `look_at_children()` and `RunWatch::watch()`; `gone_before_start()` reads the plan's mark): `run.cpp`, `run_participants.cpp`, `run_start.cpp`, `run_watch.cpp`. Their findings `connect-failed` and `connect-lost`: `run_assess.cpp`; `connect-time-changed`: the first command that changed the time, noted by `TimeCommands::apply()` under its lock (the commands' threads), read by `RunWatch::settle()` after `detach()` (the main thread), judged in `run_assess.cpp`; their last line: `RunOutput::on_line()` (the reader threads, under the output's lock) | `src/run_connect.cpp` |
| `run --connect`: the counts of the bridges. `detail::read_outside_counts()` reads a bridge's statistics line (the keys of `ethernet_bridge::format_statistics()` and `can_bridge::format_statistics()`, which `launch.read_outside_counts_reads_the_lines_the_bridges_print` holds on both sides); `detail::OutsideNote` decides when the event `outside` goes out (time as an argument). `RunOutput::on_line()` (a reader thread) reads the line, keeps the counts and writes the event under the output's lock, and does not show the line (only the last one read, once, from `take_counts()`); the main thread writes the held counts at each look of `RunWatch::watch()` (`outside_due()`), and `RunParticipants::note_last_lines()` takes the last ones into the result after the output has been read to the end (`take_counts()`), before the result's event. `detail::OutsideState` adds up what a bridge's link line (`read_outside_link_line()`) and all the counts of its statistics line (`read_outside_stats()`) say (spans of the link or the bus down, time error passive; time as an argument); `RunOutput` holds one per bridge under the same lock, says a change of the link, and `outside_lost()` tells `RunWatch` when a link or a bus has been down for `RunLimits::outside_lost_after` (the run is then stopped as for a bridge that ended: `connect-lost`); `take_link()` puts the sums and the counts into `RunOutside::link`. The sharing keys of the link line (`device`, `marked`, `shared`, `peers`) are in the same `OutsideState`; `detail::foreign_sharing()` (pure) takes the run's own bridges out of a bridge's `shared` (`RunParticipants::own_bridges()` gives their process IDs and whether they run), `RunOutput::outside_shared()` keeps what it finds under the lock and, with `--strict`, names the bridge to stop; `RunStart::wait_for_bridges()` asks it once before anything else starts, `RunWatch` at each look (`kOutsideSharedAfter`); the finding `outside-device-shared` is made in `run_assess.cpp`. The one table of the count keys that mean lost frames: `kOutsideDroppedKeys` in `run.hpp`. `OutsideState::facts()` (pure) makes a bridge's element of `RunState::outside` (link, bus, `nothing_received`, condition and reasons; `sent_without_answer()` is also the condition of `connect-nothing-received`); `RunOutput::tell_outside()` collects them under the output's lock at each link line, statistics line and look, and gives them to `RunControls::set_outside()` (lock order: the output's, then TimeCommands'; `EventStream::state()` writes them). The note `connect-nothing-received` and the warnings `outside-*` of the bus: `run_assess.cpp` | `src/run_outside.cpp` |
| Code shared by the readers of configuration files and scenarios | `src/yaml_reader.hpp` |
| The overall flow of `run` | `src/run.cpp` |
| The classes that share the work of `run` | `src/run_*.cpp`, `src/time_commands.cpp` |
| The Hub of a run with the registry, the controller and the Hub's transport, as one thing (`detail::ConnectionOf`): the order in which they close, and who holds them. `Runner::run()` makes it, on the Hub that `RunControl` made, and holds it alone; at the end the closing thread takes it over (`detail::close_connection_within()`, on `close_within()` in `src/run_close.hpp`) and keeps it if it is left at its limit. Such a thread reaches nothing else of the run, so `run_file()` destroys the `Runner` as after any other run. The list of what a closing thread that was left can still reach is the comment at the top of `src/run.cpp` | `src/run_connection.hpp` |
| This computer's addresses (the URI shown to other computers) | `src/host_addresses.cpp` |
| The watch of the running simulation (`RunWatch::watch()`), what is settled when it is over (`RunWatch::settle()`: the scenario's end, the timing, the traffic, how the simulation ended), and `StandStill`, which reports that virtual time does not advance | `src/run_watch.cpp` |
| The command line, and the stdin and stdout threads | `tools/launch/main.cpp` |
| The Control Server of `run --control` (only when `VEHICLE_SIL_BUILD_CONTROL_SERVER=ON`). The run sees it as a `ControlEndpoint` (`launch/run.hpp`); `run_file()` makes it from the request (`RunHost::control`) before the check, starts it between the two stages of a run (`Runner::prepare()`, `Runner::run()` in `src/run.cpp`), and ends it when it returns; the caller destroys it after the result (`RunReport::ended_control_server`) | `libs/launch/src/control_server.cpp` (the server, and reading and checking what the request names for it), `libs/launch/src/run_control_server.hpp` (what `run_file()` calls) |

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
| `schema/run-request.schema.json` | Request file of `check --request` and `run --request` (the code that reads it: `libs/launch/src/request_read.cpp`) |
| `schema/timeline.schema.json` | The file that `run --timeline` writes. It describes launcher output, for the tools that read it. It is in `schema/` of the tools archive, not in the template's `template/schema/` |
| `gui/protocol/dashboard.schema.json` | Dashboard |

The configuration file and scenario schemas are for completion and checking in editors; the launcher does not read them. The code that the launcher uses to read files is `libs/launch/src/config.cpp` and the scenario's reader (`scenario_reader.hpp` and the files it names).

| Where the copies are | Details |
|---|---|
| `template/schema/` | Copies of the configuration, scenario, request file and dashboard schemas |
| `schema/` in the tools archive | The same schemas and `timeline.schema.json` (`template/get_tools.py` copies all of them to the project's `schema/`) |
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
- The result of a run: why it ended (`end_reason()`), the result (`RunReport::result()`) and the sentence for people (`RunReport::message()`) in `libs/launch/src/run_report.cpp`; the exit code and `ok` of each result in the one table of `libs/launch/src/exit_codes.hpp`
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

For scenario-specific changes, see [variants and parameters](code-map-scenario.md#variants-and-scenario-parameters) and [scenario changes](code-map-scenario.md#scenarios).

Where to change:

- Keys of configuration files or scenarios, value forms, network kinds, step kinds (the code that reads files in `libs/launch/src/config.cpp` and the scenario's reader, `scenario_reader.hpp` and the files it names)

Code and files to update:

- `schema/simulation.schema.json`, `schema/scenario.schema.json`: give every key a `description`. Editors show it as the description of the key.
- The template's copies: `cp schema/*.schema.json template/schema/` (the dashboard schema comes from `gui/protocol/dashboard.schema.json`)
- The lists that the unknown-key error messages use (`k...Keys` at the top of `config.cpp` and `scenario_reader.hpp`)
- The samples in `tests/schema/test_schemas.py` (`CONFIG_SAMPLES`, `SCENARIO_SAMPLES`)
- For a key of the request file: `schema/run-request.schema.json` and its copy, `kKeys` in `request_read.cpp`, `REQUEST_SAMPLES`, the table of the keys in `docs/simulation-run.md`, and how `with_request_file()` lays an argument over it

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

For the engine's types and implementation, see [J1939 engine](code-map-j1939.md#libsj1939-engine).

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

When changing events or commands consumed by the GUI, also check [GUI implementation and tests](code-map-gui.md#guiserver-guiprotocol-guiclient-guiweb) and the recordings and protocol examples listed under [Run traffic](#run-traffic-traffic).

Where to change:

- The events of `run --events`, the commands of `--stdin-commands`, `version --json` (`libs/launch/src/events.cpp`, `tools/launch/main.cpp`)
- The members of the event `result`: `format_result_event()` in `libs/launch/src/run_format.cpp` (`EventStream::result()` takes them as written). The event `command` of a time command: `TimeCommands::apply()` calls the function it was given (`TimeCommands::OnCommand`; `main.cpp` passes `EventStream::command()`). The event `phase`: `RunControls::set_phase()` calls the function the controls were given (`RunControls::Observers`; `main.cpp` passes `EventStream::phase()`, and a run that makes its own controls writes to `RunObservers::events`)

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
- The time lock in the controls: `check_file()` calls its `on_time_lock` right after it has read the configuration (before any `--describe`), and `run_file()` gives the lock to `RunControls::set_time_lock()` there. Until then `TimeCommands::apply()` decides nothing but a stop: the commands wait in their order and are decided by `set_time_lock()` (or refused by `detach()` if the configuration could not be read). The lock's row and the caution of `--time-policy free` are in `control_rules.cpp` (`ControlSituation::time_locked`, `time_free`)
- The notes `outside-time-changed` and `unsynchronized-time-changed`: `time_changed_findings()` in `run_assess.cpp`, from `RunOutcome::time_changed`, `time_lock` and `real_time_peers` (`detail::real_time_peers()` in `time_plan.cpp`)
- Whether a time command, a stop or a write is taken, and the message of the refusal: the rows in `libs/launch/src/control_rules.cpp` (one place for `TimeCommands::apply()`, `RunControls::allowed()` and the control server's `observe_only`; the rows are tested in `tests/launch/test_control_rules.cpp`)
- `detail::plan_time_step()` (the default time step of the controller), `detail::coarse_time_step()` and `RunTiming::actual_speed()` in `time_plan.cpp` (declared in `time_plan.hpp`; `check.cpp` uses it without `run.hpp`)
- `time_lock()`, `planned_speed()` and `time_lock_findings()` in `time_plan.cpp`: whether a run that is connected outside the simulation keeps real time (`--time-policy`), the speed it starts at, and the findings `outside-*` of the check. `check_file()` puts the result in `CheckReport::time_lock`, and the run reads the speed from there
- `RunNotes::note_time()` in `run_notes.cpp` (when a `time` is written; whether a look counts as standing still: `detail::noted_standing()` in `run_notes.hpp`. `paused` of `state` is `RunControls`' in `run_controls.cpp`)
- The event `state`: what is in it and when it is told, `RunControls::compose()` and `publish()` in `run_controls.cpp` (`paused` and `speed` come from `facts_of()` in `time_commands.cpp`, `pacing` from `detail::PacingWatch` in `run_pacing.hpp`); that only the latest one waits to be written, `EventStream::emit()` in `events.cpp`
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
- `launch_run.realtime`, `on_time`, `late`, `stall`, `speed_change`, `step_width`, `pause_step`, `run_until_stdin`, `state_on_arrival`, `paused_failures`, `time_usage`, `connect_time_changed`

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
