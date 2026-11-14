# Changelog

Changes of vehicle-sil by version, in the format of [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/): each version has a heading `## [<version>] - <release date>` and its changes under `### Added`, `### Changed`, `### Deprecated`, `### Removed`, `### Fixed` and `### Security`. Version numbers are `MAJOR.MINOR.PATCH`. While the version is 0.x, a version that raises MINOR may break compatibility. A change that breaks compatibility says what users have to do.

The version comes from `project(vehicle_sil VERSION ...)` in the root `CMakeLists.txt` (`vehicle_sil_launch version` and `vehicle_sil` in the `--describe` output of an ECU print it). When the version is raised, write the same number in `pyproject.toml`, `gui/server/pyproject.toml`, `VERSION` in `template/get_tools.py`, `GIT_TAG` in `template/CMakeLists.txt`, the version written in `template/README.md`, and the heading in this file, `## [<version>] - Unreleased` until the release. The commit that is tagged `v<version>` replaces `Unreleased` with the release date (`YYYY-MM-DD`). `scripts/package.py version` finds mismatches; with `--expect v<version>`, which the release workflow runs for the tag, it also refuses a heading without the date. The release notes are this version's section.

## [0.1.0] - Unreleased

The first version.

Breaking changes: none (the first version).

### Added

#### Overview

- A library for writing ECUs (`vehicle_sil::Ecu`, `ecu_main()`, the J1939 stack and its parts, typed messages, LIN, ECU variables), a launcher that runs ECU executables together on SIL Kit (`vehicle_sil_launch check`, `run`, scenarios, the Control API), and a browser GUI.
- Prebuilt tools (one archive per OS: `vehicle_sil_launch`, the GUI server and web UI, the Python client of the Control API, the GUI client library, the starter for your own web UI `gui_starter/`, the JSON Schema of the dashboard) and a template for an ECU project (`template/`; in a release, `vehicle-sil-template-0.1.0.zip`).

#### ECU library, J1939 and Ethernet

- **SIL Kit controllers** start at the point SIL Kit recommends (CommunicationReady). The bitrate and the start of a coordinated participant reach the CAN network simulator regardless of the start order. Code that uses `Peer`, `Ecu` or `Simulation` gets this without any change. Code that uses `SilKitCanPort` or `SilKitEthPort` directly calls `runtime.add_communication_ready_handler([&port] { port.start(); })`, and code that uses `CanBridge` or `EthernetBridge` directly calls `start_controller()` from the same place (without the call, the first step is an error that says what to call). After `ParticipantRuntime::stop()` returns, this handler is not called.
- **SIL Kit participant configuration** can be passed: `--silkit-config <file>` of an ecu_main executable, and `SilKitOptions::participant_configuration`. It is for adding the listen addresses for a run on several PCs, or logging settings. The defaults of vehicle-sil (listen addresses, TCP on Windows, fixed step) are also added to a passed configuration, only for keys it does not set. A configuration that sets another participant name, another registry or `DynamicSimulationStep: true` is an error before connecting, and the error names the key to remove (`vehicle_sil_can_bus_simulator --participant-configuration` behaves the same). A file with `Includes` finds the files next to it. A build without rapidyaml uses the passed configuration as it is ([docs/ecu-executable.md](docs/ecu-executable.md)).
- **Raw Ethernet frames in an ECU**: `Ecu::ethernet()`, `kind: ethernet` in the configuration, and the example `examples/ethernet`. The virtual network (`LocalBench`) and SIL Kit (`Peer`, virtual time only) give the same record, and no frame is dropped as long as the frames that arrive in one step do not exceed `receive_capacity`. `ecu.connected` also works for Ethernet. An ECU declares the MAC address it was given in `networks[].mac` of `--describe` (it also appears in `check --json`); a duplicate on the same network is the error `ethernet-duplicate-mac`, and an ECU that does not declare one gives the note `ethernet-mac-unknown` ([docs/internals/ethernet.md](docs/internals/ethernet.md)).
- **Base libraries for raw Ethernet frames**: `vehicle_sil::eth` (a channel that sends and receives inside a step, and an in-process virtual network; no SIL Kit) and `vehicle_sil::eth_silkit` (a port for SIL Kit Ethernet, virtual time). Frames that arrive at the same time are delivered in the order of the sender address, so the virtual network and SIL Kit give the same record ([docs/internals/ethernet.md](docs/internals/ethernet.md)).
- `parse_dtc_list()` in J1939 reads the DM1 / DM2 list (lamps and trouble codes) that another node sent. It returns a view that does not allocate. The most significant bit of the fourth byte of a trouble code is not interpreted and is passed on raw.
- `vehicle_sil::signal` offers the calculations of the scenario step `wave` to C++ code.
- **Participants created by vehicle-sil** do not follow the dynamic step size of SIL Kit (`Experimental.TimeSynchronization.DynamicSimulationStep`). When a participant that sets it to `true` joins (for example the network simulator of another tool), the step size of the ECUs does not change.
- **Frames received before the simulation starts**: when an ECU in virtual time receives a frame from a participant that does not take part in time synchronization (such as the Vector XL CAN Bridge) before the simulation starts, the time of the frame is 0. A J1939 BAM or RTS/CTS that arrives across the start does not time out immediately.
- **When an ECU ends**, it waits until the last notice, which carries cancelled writes among other things, has reached the launcher (the launcher answers, and the ECU waits 500 ms at most; normally it ends at once). A write that the ECU cancelled at the end of a run is settled as `Cancelled`, not `Unknown`. The end of the ECU is delayed by 500 ms only when the launcher has already ended.
- `SilKitEthPort` pads a frame shorter than 60 bytes with zeros before sending it. `refresh_peer_statuses()` and `wait_for_peer()` can be called from several threads at the same time.

#### Launcher, configuration and check

- `simulation.live.yaml` in `examples/ecu_main` and in the template is a configuration that runs at real-time speed until it is stopped.
- **JSON Schema for configuration files and scenarios** (`schema/`; also shipped in the template and in the tools). Completion and validation in editors work from the first line of the YAML files of the examples and the template.
- **Variable map**: with `variables` on a participant in the configuration file, scenarios and the Control API read and write variables under the same names and values even when the names, units and value representations differ between executables (renaming, linear expressions, value names, variables that an executable does not have). `participants[].variables` of `check --json` lists the variables after mapping, and `variable_map` shows both names. A variable in the Control API has `source`, its name in the executable ([docs/simulation-config.md](docs/simulation-config.md)).
- **Variants and scenario parameters**: `variants` in the configuration file describes the differences per variant (executables, variable map, whether a participant is present, parameters), and `--variant <name>` selects one for `check` and `run` (omitting it for a configuration with variants prints the list of names and ends with exit code 4). A scenario uses parameters as `${name}`; values are overridden in the order `params` of the scenario, the configuration, the variant, `--param name=value`. `only` in a scenario limits the variants it runs for. A scenario step that uses a variable the variant does not have (`absent: true`), or a variable of a participant the variant does not have, is skipped and reported instead of being an error (`skipped`; time still advances by `over_ms` and `hold_for_ms`, so the times of later steps do not depend on the variant). When every checking step is skipped, the warning is `scenario-nothing-checked` ([docs/simulation-config.md](docs/simulation-config.md), [docs/scenario.md](docs/scenario.md)).
- `role` (`ecu`, `test`, `bridge`) on a participant in the configuration. `check` does not count a test participant for `single-participant`, and warns when a test participant that has a J1939 node is on a network that a bridge carries (or may carry) to real hardware (`test-participant-talks-on-bridged-network`). A participant in `check --json` has `role`.
- **Listen addresses**: when `registry_uri` is a loopback address, the registry and the participants created by vehicle-sil listen only on the loopback address. With any other address, the setup is taken to span several PCs: they listen on all addresses, and `check` and `run` give the note `registry-on-network`. `run` passes `VEHICLE_SIL_LISTEN_ON_NETWORK` to the participants it starts ([docs/simulation-run.md](docs/simulation-run.md#待ち受けるアドレス)).
- **Format version of the ECU self-description** (`--describe`): it is raised only when the meaning of a key changes or a key is removed. When only values were added, ECUs and launchers of different versions can be mixed. A value the launcher does not know is the error `describe-unknown-value`, for that ECU only.
- `run --events`: a run that ends by `--timeout-ms` or by the failure of a participant while paused also emits a last `time` with `paused` false (the same as the other ways of ending).
- `run --events`: when a `step` or `run_until` that was accepted before the start finishes quickly, the first `time` (`sim_ms` 0) does not have `paused: true`.

#### Scenarios

- `vehicle_sil_launch test <file> --scenarios <directory or file>` runs the scenarios one by one for each variant, summarizes them in a table and in JSON (`vehicle_sil_test`), and gives the verdict as the exit code (`--variant`, `--param`, `--fail-fast`, `--strict`). It fails when some combination was not run (except after an interruption), and with `--strict` when no combination was run. When the configuration has a `manual` participant, it refuses at the start. The command it prints for running a failed combination again carries over the arguments given to `test`.
- **Step `wave`** (`sine`, `triangle`, `sawtooth`, `square`, `pulse`, `noise`, `chirp`, `lag`, `chatter`, `points`). `check --scenario` warns about a waveform that is finer than the step of the ECU; `undersampled: ok` silences the warning. `features` of `version --json` has `scenario-wave`.
- `expect:` can be written beside a `ramp` or `wave` step (it must hold all the time from the time of the step to the simulation step in which the final value takes effect; `features` has `scenario-motion-expect`). For a variant without the variable, only the `expect` is skipped, and the output of `check`, `run` and `test` counts it (`unchecked_expects`).
- **A `ramp` without `from`** starts from the last value the scenario wrote to that variable, if the scenario has written to it; the start does not change when another writer, such as the ECU, changes the value. If the scenario has not written to it yet, the ramp starts from the value of the latest sample. `around` of `wave` follows the same rule.
- **Hexadecimal values** (`0x1F`) can be written for integer and enum variables in a scenario (`set`, `ramp`, `wave`, comparisons, parameters). Output stays decimal.

#### Control API

- **A run that plays a scenario** also accepts Control API clients (`run --scenario` with `--control`). A client can only read, wait and stop; calls that write and calls that move time are refused.
- `vehicle_sil_launch run --control unix:<path>` ends with exit code 4 when the path is too long, and gives the length and the limit.
- **The time answer of the Control API** does not have both `ended` and `paused` true at the moment the run ends.

#### Browser GUI

- `scripts/run-gui.py` starts the GUI from the repository with one command (for anything missing, it prints the command that builds it; a `--launcher` or a configuration file that does not exist is exit code 2).
- **Your own web UI** can be served by the browser GUI (`vehicle-sil-gui --web <dir>`). The client library is also distributed as a typed npm package, `vehicle-sil-gui-client-<version>.tgz`. `client.variables.bind` delivers the value of one variable, its freshness, whether it can be written and the state of a write as a single value, from any framework.

#### Bridges and the CAN network simulator

- `vehicle_sil_ethernet_bridge`, the Ethernet bridge executable, carries frames in both directions between a SIL Kit Ethernet network and an external interface. On Linux, `--backend tap` (a TAP device) and `--backend packet` (a network interface) are available and keep VLAN tags. `--backend pcap` on Linux (libpcap) and `--backend npcap` on Windows (Npcap) load the library at run time (vehicle-sil does not ship Npcap; when it is missing or old, the bridge says where to get it and ends with exit code 3 before touching SIL Kit). `npcap` looks for `wpcap.dll` and the DLLs it depends on only in the Npcap and system directories, and opens the adapter in a way that does not receive its own transmissions. `--list-interfaces` prints the interfaces that can be opened (also on Windows). `packet` counts frames with an unfinished checksum sent by the OS of the same PC in `ext_checksum_incomplete` and warns once how to fix it. Use `tap` when the other side is the network stack of the OS of the same PC ([docs/ethernet-bridge.md](docs/ethernet-bridge.md)).
- **Ethernet bridge libraries**: the port for an external interface, which does not use SIL Kit, with its filter (`vehicle_sil::eth_link`; EtherType, what follows the VLAN tag, the MACs on the SIL Kit side, maximum length), and `EthernetBridge`, which carries frames to and from SIL Kit Ethernet (`vehicle_sil::ethernet_bridge_core`). A second bridge between the same networks, and a MAC that is on both sides, are detected where the source MAC makes that possible; frames of that address are then dropped in both directions with a warning (the documentation lists the cases that cannot be detected). The libraries include the statistics line, the monitoring lines and the argument parsing ([docs/ethernet-bridge.md](docs/ethernet-bridge.md)).
- **Vector XL CAN Bridge**: reports bus anomalies, at most one line per second (`--no-bus-monitor` turns this off). It ends with exit code 1 when the device is unplugged. It counts the frames that went out on the bus by transmit receipts (`own_tx_dropped`), and on stopping prints the number it could not send in `xl_tx_unsent`. With `--tx-timeout-ms`, it drops new frames while the bus is congested (`xl_tx_dropped`). When bus-off persists, it activates the channel again (`--bus-off-restart-ms`, default 1000, 0 disables). The last two take effect only with `--bitrate` (init access).
- **Vector XL CAN Bridge, start-up**: the bridge fails only when the participant is stopping or has ended; it does not end with `FAIL: SIL Kit participant left Running (state ReadyToRun)` and exit code 1 right after start under high load. If the participant is still starting and has not reached Running after 15 seconds, the bridge says so and ends with exit code 1 ([docs/bridges.md](docs/bridges.md#接続先とチャネルの選択cli)). In addition, `ParticipantRuntime::start()` returns only after the state is Running, also for a participant without time synchronization (so `state()` right after it returns is not ReadyToRun).
- **Vector XL CAN Bridge**: when the registry does not answer, it says where it looked and how to fix it. For a usage error, it prints only two lines instead of the whole usage text.
- **Messages of the Ethernet bridge**: an error in the arguments prints only two lines. The message for a registry that cannot be reached says what to check when the address is not a loopback address or is `0.0.0.0`. When the Npcap list has only the loopback interface, it gives the possible reasons.
- **Simple CAN Network Simulator** (`vehicle_sil_can_bus_simulator`): drops and counts transmissions of a controller that has not been started (no acknowledgement; one warning per network). `Reset()` cancels the waiting frames with `Canceled`. Reception does not depend on the mode of the receiver.
- **Arguments on Windows**: `vehicle_sil_can_bus_simulator` and the Vector XL CAN Bridge read their arguments as UTF-8 (a file with a non-ASCII name can be passed).
- **Non-ASCII names on Windows**: the Ethernet bridge handles its arguments and connection names as UTF-8. The bridges and `vehicle_sil_can_bus_simulator` print non-ASCII names in a readable form regardless of the console code page (UTF-8 to files and pipes).
- **Warnings before stopping**: the bridges (Vector XL, Ethernet) print the warnings raised just before stopping ahead of the statistics.

#### Build, packaging and tools

- `scripts/doctor.py` checks whether this PC has what building and testing vehicle-sil needs (read-only; for anything missing, it prints how to install it; `--json`). It also has a line "CMake for gRPC", which a build with the Control Server needs (optional).
- **Preset `linux-gcc-control`**: a GCC Debug build with the Control Server (gRPC from the PC if present, otherwise a fixed version through FetchContent).
- **CMake option `VEHICLE_SIL_STATIC`**: links SIL Kit and the C++ runtime library statically into the ECU executables, so that each can be distributed as a single file (default OFF).
- **Configuring** (`cmake --preset …`) prints in one line the SIL Kit commit that is built, the patches applied and the location of the copy.

### Removed

- **The standalone Control Server** that existed during development (`vehicle_sil_control_server`) is not part of this version. Connect to the Control API with `vehicle_sil_launch run --control`, and add an ECU that the launcher does not start to the same run with `manual` on the participant (or `run --manual <name>`).

### Fixed

#### Fixes to SIL Kit carried as patches

- **Patch 0004**: fixes a rare case in which a run with the simple CAN Network Simulator started with every controller still in SIL Kit's trivial mode (the bitrate and the controller mode did not reach the simulator, and there was no error or warning).
- **Patch 0005**: fixes a rare case in which a participant that joined right after another participant had ended waited 5 seconds and then failed with a handshake timeout to the registry (same name).
