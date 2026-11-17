# Code map

[日本語](ja/code-map.md)

<!-- Style: write each entry as a short block of the same shape (a subheading, a bullet list with one fact per line, a table whose cells are one sentence each). Do not pack several facts into one cell or one bullet: the line gets long and unrelated changes hit the same line. -->

Use this map when you develop vehicle-sil itself: it shows where to make a change, which tests cover it, how to run only those tests and what else to update with it. It does not describe what each directory and target contains; see "[Directories and targets](architecture.md#directories-and-targets)" in [architecture.md](architecture.md).

## Find by change area

Start with the page for the area you change. Follow its specifications and read neighboring maps only when the change crosses their boundaries. Common test rules are in the testing page. CMake and CTest are authoritative for actual registrations and links; the diagrams and change checklists record design constraints.

| Change area | Map |
|---|---|
| Test development | [code-map-testing.md](code-map-testing.md) |
| Core, step, signal and SIL Kit defaults | [code-map-core.md](code-map-core.md) |
| CAN, Ethernet and LIN buses | [code-map-buses.md](code-map-buses.md) |
| J1939 engine, parts, codec and spy | [code-map-j1939.md](code-map-j1939.md) |
| ECU, Peer, local execution and Simulation | [code-map-execution.md](code-map-execution.md) |
| Launcher, process, configuration and run | [code-map-launcher.md](code-map-launcher.md) |
| Scenarios, timelines and batch runs | [code-map-scenario.md](code-map-scenario.md) |
| Control API, gRPC and launcher Control Server | [code-map-control.md](code-map-control.md) |
| Vector XL and Ethernet bridges | [code-map-bridges.md](code-map-bridges.md) |
| Browser GUI, protocol, client and screen examples | [code-map-gui.md](code-map-gui.md) |
| Build, packaging, dependencies and adding libraries | [code-map-build.md](code-map-build.md) |

## Previous section references

These headings keep existing section links valid and point to the detailed entries.

## Test names and how to run tests
[Details](code-map-testing.md#test-names-and-how-to-run-tests)

### Naming and registration
[Details](code-map-testing.md#naming-and-registration)

### Run a subset of tests
[Details](code-map-testing.md#run-a-subset-of-tests)

### Ports and locks
[Details](code-map-testing.md#ports-and-locks)

### Timeouts and the scale factor
[Details](code-map-testing.md#timeouts-and-the-scale-factor)

## Test labels
[Details](code-map-testing.md#test-labels)

### `unit`
[Details](code-map-testing.md#unit)

### `integration`
[Details](code-map-testing.md#integration)

### `vector_xl`
[Details](code-map-bridges.md#vector_xl)

### `step`
[Details](code-map-core.md#step)

### `signal`
[Details](code-map-core.md#signal)

### `can`
[Details](code-map-buses.md#can)

### `eth`
[Details](code-map-buses.md#eth)

### `j1939`
[Details](code-map-j1939.md#j1939)

### `peer` / `testkit`
[Details](code-map-execution.md#peer--testkit)

### `ecu`
[Details](code-map-execution.md#ecu)

### `simulation` / `examples`
[Details](code-map-execution.md#simulation--examples)

### `core` / `lin`
[Details](code-map-core.md#core--lin)

### `virtual_time`
[Details](code-map-execution.md#virtual_time)

### `lin` (`lin_cluster.*`)
[Details](code-map-core.md#lin-lin_cluster)

### `launch` / `process`
[Details](code-map-launcher.md#launch--process)

### `control`
[Details](code-map-control.md#control)

### `control_server`
[Details](code-map-control.md#control_server)

### `eth_link` / `ethernet_bridge`
[Details](code-map-bridges.md#eth_link--ethernet_bridge)

### `bridge_runtime`
[Details](code-map-bridges.md#bridge_runtime)

### `fetchcontent`
[Details](code-map-build.md#fetchcontent)

## Code and tests by directory

Each entry gives, in this order, a table of the main classes and functions (headers as paths under `include/`), the test directory and the regular expression to pass to `ctest -R`.

### `libs/core`
[Details](code-map-core.md#libscore)

### `libs/step`
[Details](code-map-core.md#libsstep)

### `libs/signal`
[Details](code-map-core.md#libssignal)

### `libs/can`
[Details](code-map-buses.md#libscan)

### `libs/can_silkit`
[Details](code-map-buses.md#libscan_silkit)

### `libs/can_netsim`, `tools/can_bus_simulator`
[Details](code-map-buses.md#libscan_netsim-toolscan_bus_simulator)

### `libs/eth`, `libs/eth_silkit`
[Details](code-map-buses.md#libseth-libseth_silkit)

### `libs/lin`, `libs/lin_silkit`
[Details](code-map-buses.md#libslin-libslin_silkit)

### `libs/j1939` (engine)
[Details](code-map-j1939.md#libsj1939-engine)

### `libs/j1939/parts/<part>`
[Details](code-map-j1939.md#libsj1939partspart)

### `libs/j1939/codec`, `tools/j1939_codegen`
[Details](code-map-j1939.md#libsj1939codec-toolsj1939_codegen)

### `libs/ecu`
[Details](code-map-execution.md#libsecu)

### `libs/json`
[Details](code-map-core.md#libsjson)

### `libs/spy`
[Details](code-map-j1939.md#libsspy)

### `libs/local`
[Details](code-map-execution.md#libslocal)

### `libs/testkit`
[Details](code-map-execution.md#libstestkit)

### `libs/peer`
[Details](code-map-execution.md#libspeer)

### `libs/simulation`
[Details](code-map-execution.md#libssimulation)

### `libs/control`
[Details](code-map-control.md#libscontrol)

### `libs/control_silkit`
[Details](code-map-control.md#libscontrol_silkit)

### `libs/control_grpc`, `proto/`, `python/`
[Details](code-map-control.md#libscontrol_grpc-proto-python)

### `libs/process`
[Details](code-map-launcher.md#libsprocess)

### `libs/launch`, `tools/launch`
[Details](code-map-launcher.md#libslaunch-toolslaunch)

### `tools/common`
[Details](code-map-launcher.md#toolscommon)

### `tools/sim_controller`
[Details](code-map-execution.md#toolssim_controller)

### `bridges/vector-xl`
[Details](code-map-bridges.md#bridgesvector-xl)

### `bridges/common`
[Details](code-map-bridges.md#bridgescommon)

### `libs/eth_link`, `bridges/ethernet`
[Details](code-map-bridges.md#libseth_link-bridgesethernet)

### `examples/j1939` (sender and receiver)
[Details](code-map-j1939.md#examplesj1939-sender-and-receiver)

### `examples/virtual_time`
[Details](code-map-execution.md#examplesvirtual_time)

### `examples/fetchcontent_consumer` (use through FetchContent)
[Details](code-map-build.md#examplesfetchcontent_consumer-use-through-fetchcontent)

### `template` (ECU project template), `scripts/package.py` (archives of the prebuilt tools and the template)
[Details](code-map-build.md#template-ecu-project-template-scriptspackagepy-archives-of-the-prebuilt-tools-and-the-template)

### `examples/first_ecu` (example for the guide "Create your first ECU")
[Details](code-map-execution.md#examplesfirst_ecu-example-for-the-guide-create-your-first-ecu)

### `examples/scenario_tests` (example for the guide "Write tests with scenarios")
[Details](code-map-scenario.md#examplesscenario_tests-example-for-the-guide-write-tests-with-scenarios)

### `examples/integrate_ecus` (example for the guide "Integrate and run several ECUs")
[Details](code-map-launcher.md#examplesintegrate_ecus-example-for-the-guide-integrate-and-run-several-ecus)

### `simulation.two-pcs.yaml` in `examples/ecu_main` (example for the guide "Run on several computers")
[Details](code-map-launcher.md#simulationtwo-pcsyaml-in-examplesecu_main-example-for-the-guide-run-on-several-computers)

### `examples/launch_control` (example for the guide "Read and write signals from outside")
[Details](code-map-control.md#exampleslaunch_control-example-for-the-guide-read-and-write-signals-from-outside)

### `schema/` (JSON Schemas for configuration files and scenarios)
[Details](code-map-launcher.md#schema-json-schemas-for-configuration-files-and-scenarios)

### `gui/server`, `gui/protocol`, `gui/client`, `gui/web`
[Details](code-map-gui.md#guiserver-guiprotocol-guiclient-guiweb)

### `examples/gui_client`, `examples/gui_starter`
[Details](code-map-gui.md#examplesgui_client-examplesgui_starter)

### `scripts/`
[Details](code-map-build.md#scripts)

### `patches/sil-kit`
[Details](code-map-build.md#patchessil-kit)

### `cmake/`
[Details](code-map-build.md#cmake)

## Documentation examples and tests
[Details](code-map-testing.md#documentation-examples-and-tests)

### Reference examples
[Details](code-map-testing.md#reference-examples)

### Guide examples
[Details](code-map-testing.md#guide-examples)

### What the tests do not compare
[Details](code-map-testing.md#what-the-tests-do-not-compare)

## Dependency direction

In every diagram, an arrow means "links" (`target_link_libraries`). Do not create dependencies in the opposite direction.

| In the diagrams | Meaning |
|---|---|
| Solid arrow | `PUBLIC` or `INTERFACE` link |
| Dotted arrow | Private (`PRIVATE`) link |
| Node name | Target name (without `vehicle_sil::`) |

### Layers
[Details](code-map-core.md#layers)

### Foundation and SIL Kit implementations
[Details](code-map-core.md#foundation-and-sil-kit-implementations)

### J1939 engine and protocol parts
[Details](code-map-j1939.md#j1939-engine-and-protocol-parts)

### ECU and local execution (without SIL Kit)
[Details](code-map-execution.md#ecu-and-local-execution-without-sil-kit)

### Peer and Simulation
[Details](code-map-execution.md#peer-and-simulation)

### launch and the Control API
[Details](code-map-control.md#launch-and-the-control-api)

### Vector XL CAN Bridge
[Details](code-map-bridges.md#vector-xl-can-bridge)

### Ethernet Bridge
[Details](code-map-bridges.md#ethernet-bridge)

### Other executables
[Details](code-map-build.md#other-executables)

## Boundary tests

These tests read the sources' `#include` lines and similar, and check the boundaries between layers. They fail when you add an internal header or a SIL Kit header.

| Test | What it checks |
|---|---|
| `j1939_parts.boundary` | `libs/j1939/parts` uses only the engine's public API and the toolbox, and links only `vehicle_sil::j1939` and other protocol parts |
| `local.boundary`, `testkit.boundary`, `peer.boundary`, `ecu.boundary`, `simulation.boundary`, `j1939_codec.boundary`, `param_service.boundary`, `j1939_spy.boundary` | The sources of that directory do not use the engine's internals (`cmake/VehicleSilCheckJ1939Parts.cmake`) |
| `ecu.no_silkit_boundary`, `ecu.simulation_headers_boundary`, `lin.boundary`, `step.boundary`, `eth.boundary`, `signal.boundary` | `libs/ecu`, `libs/simulation/include`, `libs/lin`, `libs/step`, `libs/eth` and `libs/signal` include no SIL Kit header, directly or indirectly (`cmake/VehicleSilCheckNoSilKit.cmake`) |
| `peer.controllers_are_made_on_the_simulation_name` | In `libs/`, only `libs/peer/src/peer.cpp` creates SIL Kit bus controllers, and every call creates one on a network whose name went through `simulation_name()` (`cmake/VehicleSilCheckControllerNetworks.cmake`) |
| `core.silkit_configuration_defaults` | Every SIL Kit configuration that `libs/`, `tools/`, `bridges/` and `tests/` (except `tests/core/configuration_fixtures`) build from a string goes through the vehicle-sil defaults (`default_participant_configuration()` and others) (`cmake/VehicleSilCheckSilKitConfiguration.cmake`) |
| `launch.described_networks_are_read_in_one_place` | In `libs/launch` and `tools/launch`, only `src/check_networks.cpp` (`seen_networks()`, which applies the map, and the code that writes `reported` in the JSON) and `src/describe_read.cpp` read the reported networks (`description.networks`, also through a pointer to member `::networks`) (`cmake/VehicleSilCheckDescribedNetworks.cmake`; `config.networks` of the configuration is not counted) |
| `j1939_codec.independent_of_the_engine` | The only vehicle-sil header that `codec.hpp` includes is `j1939/types.hpp` (the value types); it does not include `Network` or `Node` |
| `eth_link.boundary` | `libs/eth_link` includes no SIL Kit header, directly or indirectly (`cmake/VehicleSilCheckNoSilKit.cmake`) |

## What to update together

Each entry lists, in this order, where to change, the documentation to update with it, the code to update with it, the rules to keep and the tests (headings that do not apply are left out). Items marked "keep in sync by hand" are not compared by any test.

| Area | What you change | Entry |
|---|---|---|
| Launcher | SIL Kit configuration defaults, the listen address | [SIL Kit configuration defaults](code-map-core.md#sil-kit-configuration-defaults) |
| Launcher | The output of `check` and `run`, `examples/ecu_main/simulation.yaml` | [`check` and `run` output](code-map-launcher.md#check-and-run-output) |
| Launcher | The example code and its copies in the guides | [Output and code copied into the guides](code-map-launcher.md#output-and-code-copied-into-the-guides) |
| Launcher | Check rules and `code` | [Check rules and `code`](code-map-launcher.md#check-rules-and-code) |
| Launcher | Configuration file or scenario keys, the schemas | [Configuration file or scenario keys](code-map-launcher.md#configuration-file-or-scenario-keys) |
| Launcher | `network_names` | [`network_names` in the configuration file](code-map-launcher.md#network_names-in-the-configuration-file) |
| Launcher | `j1939_nodes` | [`j1939_nodes` in the configuration file](code-map-launcher.md#j1939_nodes-in-the-configuration-file) |
| Launcher | `run --events`, `--stdin-commands`, `version --json` | [Events, stdin commands, `version --json`](code-map-launcher.md#events-stdin-commands-version---json) |
| Launcher | Speed, pause, stepping, lateness | [Timing of `run`](code-map-launcher.md#timing-of-run) |
| Launcher | `manual`, `--manual` | [Participants you start yourself](code-map-launcher.md#participants-you-start-yourself) |
| Launcher | `variants`, `params`, `--variant`, `--param`, `only` | [Variants and scenario parameters](code-map-scenario.md#variants-and-scenario-parameters) |
| Launcher | `run --scenario`, `check --scenario`, waveforms | [Scenarios](code-map-scenario.md#scenarios) |
| Launcher | `run --scenario --timeline` | [Timeline files](code-map-scenario.md#timeline-files) |
| Launcher | `vehicle_sil_launch test` | [Running several scenarios](code-map-scenario.md#running-several-scenarios) |
| Launcher | `traffic` in the run result | [Run traffic](code-map-launcher.md#run-traffic-traffic) |
| Launcher | `kMaxMillis`, `kMinSpeed`, `kMaxSpeed` | [Launcher limits](code-map-launcher.md#launcher-limits) |
| ECU | The `--describe` JSON | [The `--describe` JSON](code-map-execution.md#the---describe-json) |
| ECU | Where SIL Kit controllers are created, `--map-network` | [Network name mapping](code-map-execution.md#network-name-mapping) |
| ECU | `ecu_main` arguments and exit codes | [`ecu_main` arguments and exit codes](code-map-execution.md#ecu_main-arguments-and-exit-codes) |
| ECU | `ecu.connected` | [Disconnecting an ECU from the bus](code-map-execution.md#disconnecting-an-ecu-from-the-bus) |
| ECU | `NetworkKind`, `NetworkUse` | [Add a network kind or use](code-map-execution.md#add-a-network-kind-or-use) |
| ECU | `template/`, the ECU API | [The template and the ECU API](code-map-execution.md#the-template-and-the-ecu-api) |
| Control API | `participants[].variables` | [Variable maps](code-map-control.md#variable-maps) |
| Control API | `control.proto` | [`control.proto`](code-map-control.md#controlproto) |
| Control API | `run --control` | [The launcher's Control Server](code-map-control.md#the-launchers-control-server) |
| Control API | Read-only access while a scenario runs | [The Control Server during a scenario](code-map-control.md#the-control-server-during-a-scenario) |
| Control API | `GetSnapshot` | [Reading time and values together](code-map-control.md#reading-time-and-values-together) |
| Control API | `wire.cpp`, `kVersion`, `kRevision` | [Messages between Agent and Hub](code-map-control.md#messages-between-agent-and-hub) |
| Control API | `control_silkit`, the start window | [The SIL Kit transport and the start window](code-map-control.md#the-sil-kit-transport-and-the-start-window) |
| Bridges | Shutdown, exit codes, health lines | [Bridge shutdown and exit codes](code-map-bridges.md#bridge-shutdown-and-exit-codes) |
| Bridges | Output and error messages of the Ethernet Bridge | [Ethernet Bridge output](code-map-bridges.md#ethernet-bridge-output) |
| Build and release | `project(... VERSION)` | [Version numbers](code-map-build.md#version-numbers) |
| Build and release | `VEHICLE_SIL_STATIC` | [`VEHICLE_SIL_STATIC`](code-map-build.md#vehicle_sil_static) |
| Build and release | `scripts/package.py tools` | [Contents of the tools archive](code-map-build.md#contents-of-the-tools-archive) |
| Build and release | The SIL Kit APIs in use | [Assumptions about SIL Kit APIs and upstream behavior](code-map-build.md#assumptions-about-sil-kit-apis-and-upstream-behavior) |
| Additions | A J1939 protocol part | [Add a J1939 protocol part](code-map-j1939.md#add-a-j1939-protocol-part) |
| Additions | A library in `libs/` | [Add a library to `libs/`](code-map-build.md#add-a-library-to-libs) |

### SIL Kit configuration defaults
[Details](code-map-core.md#sil-kit-configuration-defaults)

### `check` and `run` output
[Details](code-map-launcher.md#check-and-run-output)

### Output and code copied into the guides
[Details](code-map-launcher.md#output-and-code-copied-into-the-guides)

### Check rules and `code`
[Details](code-map-launcher.md#check-rules-and-code)

### Configuration file or scenario keys
[Details](code-map-launcher.md#configuration-file-or-scenario-keys)

### `network_names` in the configuration file
[Details](code-map-launcher.md#network_names-in-the-configuration-file)

### `j1939_nodes` in the configuration file
[Details](code-map-launcher.md#j1939_nodes-in-the-configuration-file)

### Events, stdin commands, `version --json`
[Details](code-map-launcher.md#events-stdin-commands-version---json)

### Timing of `run`
[Details](code-map-launcher.md#timing-of-run)

### Participants you start yourself
[Details](code-map-launcher.md#participants-you-start-yourself)

### Variants and scenario parameters
[Details](code-map-scenario.md#variants-and-scenario-parameters)

### Scenarios
[Details](code-map-scenario.md#scenarios)

### Timeline files
[Details](code-map-scenario.md#timeline-files)

### Running several scenarios
[Details](code-map-scenario.md#running-several-scenarios)

### Run traffic (`traffic`)
[Details](code-map-launcher.md#run-traffic-traffic)

### Launcher limits
[Details](code-map-launcher.md#launcher-limits)

### The `--describe` JSON
[Details](code-map-execution.md#the---describe-json)

### Network name mapping
[Details](code-map-execution.md#network-name-mapping)

### `ecu_main` arguments and exit codes
[Details](code-map-execution.md#ecu_main-arguments-and-exit-codes)

### Disconnecting an ECU from the bus
[Details](code-map-execution.md#disconnecting-an-ecu-from-the-bus)

### Add a network kind or use
[Details](code-map-execution.md#add-a-network-kind-or-use)

### The template and the ECU API
[Details](code-map-execution.md#the-template-and-the-ecu-api)

### Variable maps
[Details](code-map-control.md#variable-maps)

### `control.proto`
[Details](code-map-control.md#controlproto)

### The launcher's Control Server
[Details](code-map-control.md#the-launchers-control-server)

### The Control Server during a scenario
[Details](code-map-control.md#the-control-server-during-a-scenario)

### Reading time and values together
[Details](code-map-control.md#reading-time-and-values-together)

### Messages between Agent and Hub
[Details](code-map-control.md#messages-between-agent-and-hub)

### The SIL Kit transport and the start window
[Details](code-map-control.md#the-sil-kit-transport-and-the-start-window)

### Bridge shutdown and exit codes
[Details](code-map-bridges.md#bridge-shutdown-and-exit-codes)

### Ethernet Bridge output
[Details](code-map-bridges.md#ethernet-bridge-output)

### Version numbers
[Details](code-map-build.md#version-numbers)

### `VEHICLE_SIL_STATIC`
[Details](code-map-build.md#vehicle_sil_static)

### Contents of the tools archive
[Details](code-map-build.md#contents-of-the-tools-archive)

### Assumptions about SIL Kit APIs and upstream behavior
[Details](code-map-build.md#assumptions-about-sil-kit-apis-and-upstream-behavior)

### Add a J1939 protocol part
[Details](code-map-j1939.md#add-a-j1939-protocol-part)

### Add a library to `libs/`
[Details](code-map-build.md#add-a-library-to-libs)

## See also

| Document | Details |
|---|---|
| [AGENTS.md](../AGENTS.md) | Documentation rules, commits, verification scripts (read it first) |
| [architecture.md](architecture.md) | The overall structure, and what each directory and each target contains |
| The documents in [internals/](architecture.md#internals) | The internals of the places you change |
