# Examples

[日本語](README.ja.md)

This page lists the examples by task, and describes what each one contains and how to run it.

## List of examples

The further down the list, the lower the layer an example uses.

| Goal | Example | Executables and files |
|---|---|---|
| Run your first ECUs (one sends, one receives) | [getting_started](getting_started) | `vehicle_sil_getting_started` |
| Use the protocol parts for ECU applications (Request, monitoring, identification) | [j1939/ecu_pair](j1939/ecu_pair) | `vehicle_sil_j1939_ecu_pair` |
| Define messages in YAML and use them | [typed_messages](typed_messages) | `vehicle_sil_typed_messages` |
| Build a custom protocol (answer a query by returning values periodically) | [pid_stream](pid_stream) | `vehicle_sil_pid_stream` |
| Build a custom protocol (sessions, writes, long data) | [j1939/param_service](j1939/param_service) | `vehicle_sil_j1939_param_service` |
| Inject faults, observe the communication and test it | [fault_injection](fault_injection) | `vehicle_sil_fault_injection` |
| Combine several ECUs, several networks and LIN into one simulation | [vehicle](vehicle) | `vehicle_sil_vehicle` |
| Build one executable per ECU and combine them into one simulation with a configuration file | [ecu_main](ecu_main) | `vehicle_sil_engine_ecu`, `vehicle_sil_dashboard_ecu` |
| Find and fix mismatches between ECUs built separately | [integrate_ecus](integrate_ecus) | `vehicle_sil_dashboard_v1_ecu` |
| Add an ECU to the ECU project template | [first_ecu](first_ecu) | `cooler_ecu`, `read_cooler.py` |
| Write scenario tests | [scenario_tests](scenario_tests) | `my_tests/*.yaml`, `simulation.variants.yaml` |
| Send and receive raw Ethernet frames | [ethernet](ethernet) | `vehicle_sil_ethernet_example`, `vehicle_sil_eth_sensor_ecu`, `vehicle_sil_eth_display_ecu` |
| Connect to the launcher's Control Server from Python | [launch_control](launch_control) | `write_before_start.py` |
| Write a value from Python, advance time and read a value (the shortest example) | [launch_control](launch_control) | `write_and_read.py` |
| Pause a running simulation and write at a specified time | [launch_control](launch_control) | `pause_and_step.py` |
| Run sequential simulations in one resident launcher (`Serve`, `RunHandle`) | [launch_control](launch_control) | `serve_runs.py` |
| Call the browser GUI server without the screens | [gui_client](gui_client) | `run.mjs`, `time-and-stop.mjs`, `run-with-library.mjs`, `variables.mjs` (plain JavaScript) |
| Build your own screen for the browser GUI | [gui_starter](gui_starter) | HTML of parts and a short script (no build) |
| Assemble the layers yourself (bus, host, driver, network) | [low_level](low_level) | `vehicle_sil_low_level` |

## How to run the examples

The executables are under `examples/` in the build tree (for example, `build/linux-gcc-release/examples/getting_started/vehicle_sil_getting_started`).

getting_started, typed_messages, pid_stream, fault_injection, vehicle, ethernet (`vehicle_sil_ethernet_example`) and low_level run without arguments. They print the result in a few lines and exit with 0 if it is as expected, or 1 if not. CTest runs them too (label `examples`). ecu_pair does not check its result.

## About each example

### getting_started

Read this first. It writes ECUs as classes that take `Ecu&` and runs them with `Simulation` on a virtual bus or on SIL Kit (`--silkit`).

### j1939/ecu_pair

Read this to build ECU behavior from protocol parts (`PeriodicSender`, `Requester`, `Monitor` and others). It runs on SIL Kit by default, and on a virtual bus with `--local`.

### typed_messages

Read this to handle signals without writing bit positions. It uses scaling, sign, enumerations, special values and messages of 9 bytes or more.

### pid_stream

The first example to follow when you write a protocol part yourself. It shows a protocol part that receives requests and replies periodically to the requester, and how to use it. You can replace the byte format.

For a new request from the same requester, it changes the period (`set_period()`) instead of recreating the sending protocol part. It builds replies of varying length by appending to a vector. It creates the protocol part for a new requester inside the handler, after it checks the limit (`max_streams`) and the free timer slots (`timers_free()`), and destroys it inside the handler or the timer on Stop or expiry. The tool's operations are scheduled at given times with `Ecu::at()`.

### j1939/param_service

Read this to build a protocol larger than pid_stream above (sessions, retries, block transfers).

### fault_injection

Read this to reproduce and check disconnection, address conflicts, frames lost in the middle of a transfer and injected raw frames on a virtual bus.

### vehicle

Read this to build a setup that spans 2 CAN networks and 1 LIN network. The LIN master and slave are also inside `Ecu&` classes. The engine ECU declares variables that are read and written from outside the simulation, and `main()` writes them at specified times. It runs on SIL Kit by default; with `--local` it runs on virtual buses (CAN and LIN) with the same result.

### ecu_main

Read this to build one executable per ECU and run them in one SIL Kit simulation. `main()` is the single line `ecu_main()`, and every ECU takes the same arguments (`--describe`, `--registry-uri`, `--local --run-for-ms`). [docs/ecu-executable.md](../docs/ecu-executable.md) explains how to run them and the `--describe` JSON. The CTest tests `ecu_main.*` run the 2 ECUs in separate processes and check them.

| File | Contents |
|---|---|
| `simulation.yaml` | The configuration file that combines the 2 ECUs into one simulation. Check it with `vehicle_sil_launch check` and run it with `vehicle_sil_launch run` ([docs/simulation-config.md](../docs/simulation-config.md)). |
| `simulation.live.yaml` | A configuration file that runs the same 2 ECUs at real-time speed until you stop them. Use it to watch and change values in the GUI (from the repository, `scripts/run-gui.py` starts the GUI with it). |
| `simulation.two-pcs.yaml` | A configuration file that runs the same 2 ECUs split across 2 computers ([docs/howto/multi-pc.md](../docs/howto/multi-pc.md)). |
| `simulation.dashboard.yaml` | How the browser GUI lays out the variables (see "Dashboard files" in [docs/gui.md](../docs/gui.md#dashboard-files)). |
| `panels/rpm-bar.js` | A custom panel (plain JavaScript) placed on the dashboard (see "Custom panels" in [docs/gui.md](../docs/gui.md#custom-panels)). |

### integrate_ecus

Read this to run the example of the guide [Integrate and run several ECUs](../docs/howto/integrate-ecus.md). Run it in `examples/integrate_ecus/` of the build tree.

| File | Contents |
|---|---|
| `simulation.yaml` | A configuration file that lists the engine ECU of ecu_main and a dashboard ECU (`vehicle_sil_dashboard_v1_ecu`) that calls the same bus by another network name (`VehicleCAN`). `check` reports the mismatch. |
| `simulation.two-buses.yaml` | A configuration file that only adds the name to `networks`. `check` and `run` both pass, but the dashboard ECU does not receive the engine speed. |
| `simulation.fixed.yaml` | A configuration file with the dashboard ECU replaced by the rebuilt one. |
| `simulation.network-names.yaml` | A configuration file that keeps the dashboard ECU as it is and maps its name onto `J1939_CAN` with `network_names`, for when it cannot be rebuilt. |
| `simulation.second-engine.yaml` | `simulation.fixed.yaml` with a second instance of the engine ECU, which `j1939_nodes` gives another J1939 address and NAME. |
| `scenarios/engine_speed.yaml` | A scenario that checks the reception. |

### first_ecu

Read this to see the finished files of the guide [Create your first ECU](../docs/howto/first-ecu.md). It contains an ECU with variables only (`Cooler`) added to the 2 ECUs of [template/](../template), the 4 lines added to CMake, a `simulation.yaml` with 3 ECUs and a script that reads and writes variables from Python.

It is not built on its own. The test build creates `cooler_ecu` next to the 2 ECUs of the template (`build/<preset>/tests/template/project/bin/`). The CTest test `template.first_ecu` compares the files and the output with the guide (`control_server.example_first_ecu` does this for the Python script).

### scenario_tests

Read this to see the finished files that you write in the guide [Write tests with scenarios](../docs/howto/scenario-tests.md). It contains 4 scenarios for the 2 ECUs of [ecu_main](ecu_main) (setting and checking values, response time, checks during `ramp` and `wave`, parameters and `only`) and a configuration file with 2 variants added.

There is nothing to build. Copy the files to `build/<preset>/examples/ecu_main/` and run them with `vehicle_sil_launch test simulation.variants.yaml --scenarios my_tests/`. The line numbers in the output differ from the guide by the comment lines at the start of each scenario. The CTest test `launch_run.scenario_guide` compares the files, the output and the exit codes with the guide.

### ethernet

Read this to have an ECU build Ethernet frames itself and send and receive them (`Ecu::ethernet()`; vehicle-sil has no TCP/IP stack). The sensor sends a value every 100 ms, and the display receives it and replies with an acknowledgement.

| Executable | Contents |
|---|---|
| `vehicle_sil_ethernet_example` | Runs the 2 ECUs in one process (on a virtual network without arguments, on SIL Kit with `--silkit`; the result is the same, and the example checks it itself). `--ethertype <number>` changes the EtherType of the frames. |
| `vehicle_sil_eth_sensor_ecu`, `vehicle_sil_eth_display_ecu` | The same ECUs turned into executables with `ecu_main()`. Run them with `vehicle_sil_launch run simulation.yaml --scenario scenarios/exchange.yaml` (the network has `kind: ethernet`; `ecu.connected` disconnects an ECU from the bus). |

The default EtherType (0x88B5) and the MAC addresses (locally administered addresses) are values that this example chose. They are not verified against the standard.

### launch_control

Python scripts that control simulations via `vehicle_sil_launch` (`serve --once` via `Launch`, or resident `serve` via `Serve` / `RunHandle`) and its Control Server (`--control`).

| Script | Contents | CTest |
|---|---|---|
| `write_before_start.py` | Starts the 2 ECUs of `ecu_main` with `--start-paused`, writes the engine throttle before the first step, advances 500 ms, and reads back and checks the engine speed (exits with 1 if it differs). | `control_server.example_write_before_start` |
| `write_and_read.py` | The first script of the guide [Read and write signals from outside](../docs/howto/operate-signals.md). Starts the 2 ECUs of `ecu_main` with `--start-paused`, writes 50 % to the throttle, advances 500 ms and prints the engine speed (the arguments are the path of `vehicle_sil_launch` and `simulation.yaml`). | `control_server.guide_operate_signals` (compares the full script and output shown in the guide) |
| `pause_and_step.py` | Runs the 2 ECUs of `ecu_main` at a quarter of real-time speed, watches the engine speed with `watch()` and pauses when it exceeds 1000 rpm. Then it writes the throttle with `at` set to 1 ms after the paused time, advances 100 ms, checks the step in which the write was applied and the time and values read with `snapshot()`, and switches to as fast as possible to run to the end (exits with 1 if anything differs). | `control_server.example_pause_and_step` |
| `serve_runs.py` | Starts one resident `vehicle_sil_launch serve --control` via `Serve`, calls `prepare()`, and runs two isolated simulations sequentially with `RunHandle` and `run.control()`, verifying per-run binding and fresh virtual time 0 state (exits with 1 if anything differs). | `control_server.example_serve_runs` |

All of them need a build with `VEHICLE_SIL_BUILD_CONTROL_SERVER=ON` and a Python with gRPC (see "Python" in [docs/control-api.md](../docs/control-api.md#python)).

To start an ECU yourself (to run it from a debugger, for example), make that participant `manual` and connect it to the same launcher (see "Starting a participant yourself (`manual`, `--manual`)" in [docs/simulation-config.md](../docs/simulation-config.md#starting-a-participant-yourself-manual---manual)).

### gui_client

Read this to build your own tool on the same API as the GUI ([gui/protocol](../gui/protocol/README.md)). It runs the check, starts a run, reads events and waits for the end. Run it with `VEHICLE_SIL_GUI_URL='<URL>' node run.mjs <configuration file>`.

| File | Contents |
|---|---|
| `run.mjs` | Uses only the standard `fetch` and `WebSocket` of Node 22 or later. |
| `time-and-stop.mjs` | Uses only standard `fetch` and `WebSocket` of Node 22 or later: sends `{instance, run_id}` on `POST api/time` and `POST api/stop-session`, correlates numeric `id` between HTTP 200 and WebSocket `command` events (`accepted: true`/`false`), and reconnects with `after_stream_seq`. |
| `run-with-library.mjs` | Uses the client library ([gui/client](../gui/client/README.md)) bundled into one file. |
| `variables.mjs` | Reads and writes ECU variables with the same library: starts paused, writes one variable before the first step, advances, pauses and checks the value. Needs a launcher with a Control Server. |

It is not part of the CMake build (`scripts/verify-gui.py` runs it against a stand-in launcher that replays the output of the real one, and checks it). For how to use it, see "Use from tools without a screen" in [docs/gui.md](../docs/gui.md#use-from-tools-without-a-screen).

### gui_starter

Read this to build your own screen, served by the GUI server (`vehicle-sil-gui --web <dir>`), with or without a framework. It is a screen of parts (`<vsil-run-controls>`, `<vsil-slider>`, `<vsil-value>` and others) that you change to your own ECU's variables, with a short script of your own for what the parts do not have. The page loads the parts and the library from the GUI server, so there is nothing to copy. Its [README](gui_starter/README.md) also explains how to use the library package (`.tgz`) with TypeScript and the framework of your choice.

It is not part of the CMake build (`scripts/verify-gui.py` serves it with `--web` against a stand-in launcher that replays the output of the real one, and checks it in a browser). For how to use it, see "Build your own screen" in [docs/gui.md](../docs/gui.md#build-your-own-screen).

### low_level

Read this to build a new backend or a test harness. Normally, use `Simulation`.

## Executables started by integration tests

The integration tests start the executables below as separate processes. They are not meant as introductions.

| Example | Contents |
|---|---|
| [j1939/sender](j1939/sender), [j1939/receiver](j1939/receiver) | A sender that runs the J1939 stack on a real-time `Peer`, and a receiver that receives directly with SIL Kit CAN and checks the frames. |
| [lin](lin) | LIN master and slave peers (assembled from `ParticipantRuntime` and `SilKitLinPort`, without `Peer`). |
| [virtual_time](virtual_time) | Virtual-time peers (physical model, J1939 ECU, LIN sensor, body controller). |

## Use from an external CMake project

[fetchcontent_consumer](fetchcontent_consumer) is an example of an external project that includes vehicle-sil with FetchContent. It is configured and tested as a separate build (label `fetchcontent`). For the steps, see "Use from another project (FetchContent)" in [docs/build.md](../docs/build.md#use-from-another-project-fetchcontent).
