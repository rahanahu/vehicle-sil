# ECU executables (`ecu_main`) and the `--describe` output

[日本語](ja/ecu-executable.md)

This is the reference for `ecu_main()`, the function that turns an ECU class (a class that takes an `Ecu&`) into an executable. It covers the executable's arguments and exit codes, how it stops, and the JSON format in which `--describe` prints the ECU's configuration. The API may still change.

Use it when different people build the executables of different ECUs and you run them together in one SIL Kit simulation. Every ECU executable takes the same arguments, so you can run an ECU without knowing what is inside it.

```cpp
#include "vehicle_sil/simulation/ecu_main.hpp"

int main(int argc, char** argv)
{
    return vehicle_sil::ecu_main<Engine>(argc, argv, {.name = "Engine", .step = 10ms});
}
```

For a complete example, see [examples/ecu_main](../examples/ecu_main) (`vehicle_sil_engine_ecu` and `vehicle_sil_dashboard_ecu`).

| To find | See |
|---|---|
| How to call `ecu_main()`, its arguments and exit codes | [`ecu_main`](#ecu_main) |
| What to ask of ECU authors, and reserved variable names | [Requirements for ECU authors](#requirements-for-ecu-authors) |
| What `Ecu::describe()` returns | [`Ecu::describe()`](#ecudescribe) |
| The keys of the `--describe` JSON | [JSON format](#json-format) |
| How to run several ECUs without the launcher | [Running several ECUs in one simulation](#running-several-ecus-in-one-simulation) |

## `ecu_main`

The library is `vehicle_sil::simulation`. Its header does not include the SIL Kit headers.

| Call | What it does |
|---|---|
| `ecu_main<App>(argc, argv, EcuMainOptions, args...)` | Gives the ECU an `App{ecu, args...}` (`Ecu::emplace()`) and runs it as the arguments say. |
| `ecu_main(argc, argv, EcuMainOptions, std::function<void(Ecu&)>)` | Builds the ECU with a function instead of a class, and runs it as the arguments say. |

`args` are passed unchanged to the constructor of `App`. `App` is constructed only once, so move-only values can be passed too. In the form with a function, the function is called once, before anything runs.

| Member of `EcuMainOptions` | Meaning |
|---|---|
| `name` | Name of the ECU (the SIL Kit participant name, and `name` in `--describe`). |
| `step` | Step size (must be greater than 0). |
| `last_word_wait` | The longest time that the ECU, when it ends under `--registry-uri`, waits for the launcher's response to its last word (default 500 ms; 0 means no wait). |

The last word that `last_word_wait` waits on tells the launcher about cancelled writes and similar results. The response usually comes at once, so change this value only on a computer so slow that the response takes 500 ms or more. No command-line argument sets this value.

### Arguments and exit codes

| Argument | What it does |
|---|---|
| `--describe` | Constructs the ECU on in-process virtual buses, runs nothing and connects to nothing, writes `to_json(Ecu::describe())` to stdout and exits. |
| `--registry-uri <uri>` | Joins the SIL Kit simulation of the registry at `<uri>` as a coordinated virtual-time participant, and runs until the simulation ends. |
| `--start-timeout-ms <ms>` | Only with `--registry-uri`. How long to wait from joining until the simulation starts (an integer of 1 or more; default 30000). |
| `--no-control` | With `--registry-uri`, does not expose the ECU's variables to the simulation (see [Exposing variables](#exposing-variables---registry-uri)). |
| `--silkit-config <file>` | With `--registry-uri`, passes a SIL Kit participant configuration file (YAML or JSON) to this participant (see [Passing a SIL Kit configuration file](#passing-a-sil-kit-configuration-file---silkit-config)). |
| `--map-network <simulation name>=<executable name>` | Only with `--registry-uri`. Connects the network that the ECU's code created as `<executable name>` to the SIL Kit network `<simulation name>` (see [Renaming networks](#renaming-networks---map-network)). |
| `--j1939-node <network>=<settings>` | Only with `--registry-uri`. Changes the address and NAME fields of a J1939 node of the ECU for this run (see [Changing J1939 node addresses and NAMEs](#changing-j1939-node-addresses-and-names---j1939-node)). |
| `--local --run-for-ms <ms>` | Runs this ECU alone on in-process virtual buses for `<ms>` milliseconds of virtual time (an integer of 1 or more). |
| `--name <name>` | Participant name used instead of `EcuMainOptions::name` (ASCII or UTF-8). |
| `--help` (`-h`) | Writes the usage to stdout and exits with 0. |

Exactly one of `--describe`, `--registry-uri` and `--local` is required. Notes on individual arguments:

| Argument | Notes |
|---|---|
| `--registry-uri` | A controller in another process (`vehicle_sil_sim_controller`) starts and stops the simulation. |
| `--registry-uri` | If the host of `<uri>` is a loopback address (`127.0.0.1`, `localhost` and so on), the ECU listens for connections from other participants on loopback addresses only. |
| `--registry-uri` | With the environment variable `VEHICLE_SIL_LISTEN_ON_NETWORK=1` set, it listens on all addresses (see "Listen addresses" in [simulation-run.md](simulation-run.md#listen-addresses)). |
| `--start-timeout-ms` | If the simulation does not start in time, the ECU exits with exit code 3. |
| `--start-timeout-ms` | Use it when you start many ECUs, or on a slow computer. |
| `--no-control` | Does not change how the ECU behaves. |
| `--no-control` | With `--describe`, `variables` is unchanged and `control` becomes `null` (`vehicle_sil_launch run` does not wait for this ECU). |
| `--no-control` | With `--local`, it changes nothing. |
| `--silkit-config` | With `--describe`, the file is only read and checked (an error gives exit code 3). |
| `--silkit-config` | Not allowed with `--local` (exit code 4). |
| `--map-network` | Give it once for each network, as many times as needed. |
| `--j1939-node` | Give it once for each node, as many times as needed. |
| `--local` | Use it to try the ECU on its own. |
| `--name` | Also changes `name` in `--describe`. |
| `--name` | Only the name changes. The J1939 NAME, the addresses and the variable names stay the same. |
| `--help` | Writes the usage even if other arguments have errors. |
| `--help` | A `-h` right after an argument that takes a value is read as that value (`--name -h` sets the name to `-h`). |

To run two instances of the same ECU, make sure that the NAME and the addresses, which `--name` leaves unchanged, do not overlap (`--j1939-node` changes them; see [Changing J1939 node addresses and NAMEs](#changing-j1939-node-addresses-and-names---j1939-node)). The variable names can stay the same: each participant has its own. For how, see "Run two instances of the same ECU" in [Integrate and run several ECUs](howto/integrate-ecus.md#run-two-instances-of-the-same-ecu).

The exit codes are the same as those of the other tools.

| Exit code | Meaning |
|---|---|
| 0 | Done (`--describe` wrote the JSON, the `--registry-uri` simulation ended, `--local` ran for the full time) |
| 1 | The ECU failed while running (an exception in a step; `SimulationError`) |
| 3 | Setup failed (an exception in the constructor of `App`, an error in `EcuMainOptions`, the registry cannot be reached, the simulation does not start, stdout cannot be written, the ECU has no network with the executable name of a `--map-network`, a `--j1939-node` that no node takes) |
| 4 | Usage error |
| 130 | Under `--registry-uri`, the ECU was asked to stop (see [When asked to stop](#when-asked-to-stop---registry-uri)) |

The following give exit code 4. The ECU starts nothing and writes the usage to stderr.

| Usage error | Notes |
|---|---|
| None of `--describe`, `--registry-uri` and `--local` | Also with no arguments at all. |
| 2 or more of the 3 | |
| An unknown argument | The form `--name=X` is an unknown argument too (separate the value with a space). |
| `--name`, `--registry-uri`, `--run-for-ms` or `--start-timeout-ms` without a value | The value also counts as missing when the next argument starts with `--`. |
| An empty value | |
| A `--name` that is neither ASCII nor UTF-8 | For example, a name in a Windows code page. The message shows the bytes in hexadecimal. |
| An argument given twice | `--map-network` and `--j1939-node` are exceptions (see [Renaming networks](#renaming-networks---map-network) and [Changing J1939 node addresses and NAMEs](#changing-j1939-node-addresses-and-names---j1939-node)). |
| An error in the value of `--map-network` | See [Renaming networks](#renaming-networks---map-network). |
| An error in the value of `--j1939-node`, or 2 of them that clash | See [Changing J1939 node addresses and NAMEs](#changing-j1939-node-addresses-and-names---j1939-node). |
| `--map-network` or `--j1939-node` without `--registry-uri` | |
| `--local` without `--run-for-ms`, or `--run-for-ms` without `--local` | |
| `--start-timeout-ms` without `--registry-uri` | |
| A `--run-for-ms` or `--start-timeout-ms` that is not an integer, is 0 or exceeds 10^12 | |

On failure, the ECU writes one line `error: <ECU name>: <reason and how to fix it>` to stderr.

| Item | Rule |
|---|---|
| Keeping it to one line | Line breaks in SIL Kit messages become spaces, and an exception that is not a `std::exception` gets a fixed one-line message. |
| ECU name | The name set with `--name` (`EcuMainOptions::name` for a usage error). |
| Usage error | The usage follows the line. |
| The simulation does not start | Different messages for a timeout and for other causes. |

A timeout means that there is no controller or that the required participants did not all join; its message also says how to check the controller and points to `--start-timeout-ms`. An example of another cause is a variable name that collides with a monitoring variable of `Peer`.

The message when the registry cannot be reached (`cannot join the simulation at "<URI>": ...`) lists what to check: that `vehicle_sil_launch run` was started first and is waiting, the address and port of the URI, the firewall of that computer, and a participant with the same name.

When the registry refuses the participant because its name is in use, the ECU retries creating the participant for 5 seconds before it fails (or for `--start-timeout-ms`, if that is shorter). The reason: when you restart a participant with the same name right away, the registry may not yet have noticed that the previous process ended. The message then mentions only the time it waited and the name. Because the registry was reached, the message leaves out the list of things to check.

### ECU-specific arguments

`ecu_main()` exits with exit code 4 on an unknown argument. To give an ECU arguments of its own (a J1939 address, for example), follow these rules:

| Item | Rule |
|---|---|
| Where to take them out | In `main()`, take the ECU's own arguments out of `argv` and pass the rest to `ecu_main()`. |
| Passing them to `App` | Pass the values to the constructor of `App` through the `args...` of `ecu_main<App>()`. |
| Names you cannot use | The names of the `ecu_main()` arguments (table above). |
| `--help` | The `--help` of `ecu_main()` does not show the ECU's own arguments. |

For both `check` and `run`, `vehicle_sil_launch` starts the ECU with the arguments in its `command` in the configuration file. So the `--describe` output shows the configuration that the ECU's own arguments produce (the addresses and NAMEs of its nodes).

### Stdout and stderr

`ecu_main` writes only two things to stdout: the `--describe` JSON and the `--help` usage.

With `--describe`, stdout (fd 1) is redirected to stderr from before the ECU is constructed until the process ends; it is not restored. The JSON is written directly to a duplicate of the original stdout descriptor. So nothing that `App` writes to stdout gets mixed into the JSON.

| Case | What happens |
|---|---|
| The constructor and destructor of `App`, threads of `App`, `atexit` handlers or linked libraries write to stdout (`printf`, `std::cout` and so on) | The output goes to stderr. |
| Stderr is closed | The null device is opened as stderr first, so the output of `App` goes nowhere. |
| Stdout is closed | The JSON cannot be written, so the exit code is 3. |

After `ecu_main` returns, fd 1 is still stderr, so call `ecu_main` at the end of `main()`.

### Exposing variables (`--registry-uri`)

When the ECU joins with `--registry-uri`, it exposes its variables to the simulation with SIL Kit publish/subscribe, with no code of your own (`vehicle_sil::control_silkit::AgentTransport`; see "Transport over SIL Kit (`vehicle_sil::control_silkit`)" in [internals/control.md](internals/control.md#transport-over-sil-kit-vehicle_silcontrol_silkit)). An ECU that declares no variables still has `ecu.connected`, so every ECU exposes variables.

`vehicle_sil_launch run` receives what the ECU publishes and reads and writes the variables (see "Connection to ECU variables (`control_link`)" in [simulation-run.md](simulation-run.md#connection-to-ecu-variables-control_link)). If no process receives it, what the ECU sends reaches no one, and nothing else happens.

| Item | Details |
|---|---|
| Dependencies | Neither gRPC nor Protocol Buffers is needed (ECU executables depend on neither). |
| When it sends | Values and the results of writes are sent within the step, right after the values are read. |
| How often it sends | At the Agent's `sample_period` (default 10 ms; `Ecu::set_agent_options()`), without thinning out. |
| `control` in `--describe` | Reports in `control` that the ECU exposes its variables (`Ecu::describe()` itself does not report it; `ecu_main` adds it). |
| ECUs the launcher waits for | `vehicle_sil_launch run` waits only for ECUs that report in `control` that they expose their variables. |
| `--no-control` | The ECU no longer exposes its variables, and `control` in `--describe` becomes `null`. |
| Authentication | There is no authentication between the ECU and the launcher (any process that can join the registry can write the variables). |

If the application has attached its own transport (something that holds a `control::TransportLease`) to the Agent, `ecu_main` does not attach one. In that case, if the transport is already attached at the time of `--describe`, `ecu_main` does not report in `control` that the ECU exposes its variables. If it is not attached yet, `ecu_main` reports that it does, and `vehicle_sil_launch run` waits for the first message from the ECU (hello). So if the application attaches its own transport to the Agent, pass `--no-control`.

### Passing a SIL Kit configuration file (`--silkit-config`)

Use this option to change SIL Kit's middleware or logging settings, for example to listen on a fixed port, to try to connect more times or to write the SIL Kit log to a file. The ECU runs without it.

```yaml
# engine.silkit.yaml: listen on a fixed port, and also write the SIL Kit log to a file
Middleware:
  AcceptorUris: ["tcp://0.0.0.0:8601"]
  ConnectAttempts: 5
Logging:
  Sinks:
    - Type: File
      Level: Info
      LogName: engine_silkit
```

```sh
engine/vehicle_sil_engine_ecu --registry-uri silkit://192.0.2.10:8500 --silkit-config engine.silkit.yaml
```

The file format and the keys are those of the SIL Kit participant configuration ("Configuration" in the SIL Kit documentation). A relative path is relative to the working directory of the process.

Settings that you write are used as written. For settings that you leave out, vehicle-sil adds its own defaults.

| Key | If not written |
|---|---|
| `Middleware.AcceptorUris` | The same as without a configuration file (if the URI of the registry has a loopback address, the ECU listens on loopback only; see "Listen addresses" in [simulation-run.md](simulation-run.md#listen-addresses)). |
| `Middleware.EnableDomainSockets` | Set to `false` on Windows. |
| `Experimental.TimeSynchronization.DynamicSimulationStep` | Set to `false`. |

On Windows, `Middleware.EnableDomainSockets` becomes `false` even if you write `true`. The SIL Kit default is `true`, so vehicle-sil cannot tell whether you wrote it.

How problems in the file are handled:

| Setting | Handling | Reason and notes |
|---|---|---|
| Unknown keys | SIL Kit prints a warning. | |
| Values of the wrong type | SIL Kit reports an error. | The message is `error: <name>: --silkit-config "<file>": SIL Kit does not accept the configuration: ...`. |
| A file that cannot be read as YAML | SIL Kit reports an error. | The message above ends with "(at line <line>, column <column> of the configuration text)". |
| `ParticipantName` differs from the participant name (`--name`, or `EcuMainOptions::name` without it) | An error before connecting. | SIL Kit joins under the name in the configuration file, so a process that waits for the participant name keeps waiting. |
| `Middleware.RegistryUri` differs from `--registry-uri` | An error before connecting. | SIL Kit connects to the URI in the configuration file. |
| `DynamicSimulationStep: true` | An error before connecting. | vehicle-sil participants run with their own fixed step size. |

An error before connecting gives exit code 3, and the message (`error: <name>: --silkit-config "<file>": ...`) says how to fix it. `ParticipantName` and `Middleware.RegistryUri` pass if they have the same value. `--describe` does not know the URI, so it does not compare `Middleware.RegistryUri`.

Other rules:

- Files split off with `Includes` are also searched for in the directory of the configuration file.
- The checks and defaults above apply to the configuration with the split files included (it does not matter which file a setting is in).
- Do not write controller sections (`Network` in `CanControllers` and so on). The network names would no longer match the `--describe` output, and nothing detects this.
- With `vehicle_sil_launch`, write the option in `command` (see "ecu_main executables (`describe: true`)" in [simulation-config.md](simulation-config.md#ecu_main-executables-describe-true)). The check's `--describe` run gets the option too, so `check` finds errors in the SIL Kit configuration.

From the library, pass the configuration as a string to `SilKitOptions::participant_configuration` or `ParticipantRuntimeOptions::participant_configuration`. `SilKitOptions` belongs to `Simulation` and applies to every participant in the process. To read it from a file, use `participant_configuration_from_file()` (for the rules, see "SIL Kit configuration defaults" in [internals/core.md](internals/core.md#sil-kit-configuration-defaults)).

### When asked to stop (`--registry-uri`)

Under `--registry-uri`, `ecu_main` stops when it receives SIGINT (Ctrl+C) or SIGTERM (on Windows: Ctrl+C, Ctrl+Break, closing the console).

| When it is received | What `ecu_main` does | Message on stderr |
|---|---|---|
| After the simulation started | Stops the participant (`request_stop()`). When `run()` has returned and the destructor of `App` and the rest have run, exits with exit code 130. | `error: <ECU name>: interrupted (SIGTERM): it stopped the simulation and left it` |
| Before the start (while connecting to the registry, or while waiting for the start) | Exits at once with exit code 130, without running destructors. | `error: <ECU name>: interrupted (SIGTERM) before the simulation started: it ends at once` |
| A second time, while stopping | Exits at once with exit code 130, without running destructors. | `error: <ECU name>: interrupted (SIGTERM) again while it stopped: it ends at once` |

When a coordinated participant stops, the whole simulation stops in an orderly way. Skipping the destructors when the signal arrives before the start matches the signal's default action. Even then, the C standard I/O buffers (`printf` and so on) are flushed before the process ends.

| Item | Rule |
|---|---|
| When the signals are handled | Only while the `--registry-uri` simulation runs inside `ecu_main`. |
| Destructor of `App` | By the time it runs, the previous signal settings have been restored. |
| `--describe` and `--local` | Do not handle the signals (the default action ends the process at once). |
| `read()` and similar calls in `App` | Not interrupted, because `SA_RESTART` is used. |
| Signals that the parent process set to be ignored (`nohup`) | Stay ignored. |
| Closing the console on Windows | The ECU waits up to 4 seconds for the orderly stop to finish (then Windows ends the process). |

While `ecu_main` handles these signals, they are its own. An ECU that needs to handle them itself must use `Simulation` directly instead of `ecu_main`.

`vehicle_sil_launch run` uses these signals to ask participants to stop: those that do not end after the simulation ends, and those of a simulation that did not start (see "Stopping a run" in [simulation-run.md](simulation-run.md#stopping-a-run)).

### When another participant leaves during a run (`--registry-uri`)

`ecu_main` exits on its own in only 3 cases:

| Case | Exit code |
|---|---|
| The simulation ended | 0 |
| A step of this ECU failed | 1 |
| The ECU was asked to stop | 130 |

An ECU whose step failed stays in the Error state and waits up to 5 seconds for the controller to abort the simulation before it ends. This lets the controller see the failure as Error. Under a controller that aborts as soon as it sees Error, the ECU waits only that long.

What happens when another process goes away:

| What goes away | What happens |
|---|---|
| The controller process | The simulation does not stop: virtual time keeps advancing, and the ECU does not end by itself. |
| The registry process | Depending on the connections between the participants, this ECU may be in the Error state as other participants see it, and it may keep waiting without virtual time advancing. |
| Another participant that failed (the controller aborts the simulation) | This ECU ends with exit code 0. |

The controller reports which participant failed (in the output of `vehicle_sil_sim_controller`, and with exit code 1).

In every case, the process that started the ECUs must watch for the controller to end and then stop the ECU processes. `vehicle_sil_launch run` does this (see [simulation-run.md](simulation-run.md)). If you start them yourself, follow these 2 rules.

Decide the result of the simulation by the exit code of the controller, not by the exit codes of the ECUs alone. An ECU can end with 0 even when another participant failed. An exit code 1 from an ECU means only that a step of that ECU failed.

If the controller process goes away without a result, stop all ECUs (they do not end by themselves). Also stop ECUs that are still running some time after the controller ended (when the registry has gone away, for example). First ask them to stop with SIGTERM (Ctrl+Break on Windows; see "When asked to stop" above), and kill those that still do not end.

### Renaming networks (`--map-network`)

When a network name in the ECU's code differs from the name that the simulation uses, `--map-network` connects the network to the simulation's name without a rebuild.

```sh
engine_ecu --registry-uri silkit://localhost:8500 --map-network VehicleCAN=J1939_CAN
```

The value is `<simulation name>=<executable name>`, in that direction. In the example, the network that the code created with `j1939("J1939_CAN")` connects to the SIL Kit network `VehicleCAN`. The value is split at the first `=`, so the executable name can contain `=` and the simulation name cannot.

| Item | Rule |
|---|---|
| What is renamed | A name, not a call. `j1939()` and `can()` with the same name are both renamed, and the networks of `lin_master()`, `lin_slave()` and `ethernet()` the same way. Networks that are not listed keep the code's name. A mapping onto the same name (`CAN=CAN`) changes nothing. |
| What changes | Only the name of the SIL Kit network and the names in the monitoring output of `Peer` (the variables that start with `j1939.<simulation name>.`, and the text of the node state events). |
| What keeps the code's name | The names that the code sees (`ecu.network("J1939_CAN")` and the like), the names of the SIL Kit controllers and `networks[].name` in `--describe`. |
| `--describe` and `--local` | Not allowed with either (exit code 4). The `--describe` output does not depend on the arguments: an executable that accepts `--map-network` lists `"map-network"` in `features` (see [Keys](#keys)). Under `--local`, the monitoring output uses the code's names. |

Values that give exit code 4: no `=`, an empty name on either side, the same simulation name twice, the same executable name twice, a value that is neither ASCII nor UTF-8, and on Windows a value with a non-ASCII character.

Cases that give exit code 3, each reported in one line before the simulation starts:

| Case | Notes |
|---|---|
| The ECU has no network with the executable name | The message lists the ECU's networks. |
| After renaming, 2 networks of the ECU have the same name | For example `--map-network B=A` when the code has both `A` and `B`. A swap (`--map-network B=A --map-network A=B`) is allowed. |
| The configuration of `--silkit-config` sets `Network` for a controller (`CanControllers`, `LinControllers`, `EthernetControllers`, `FlexrayControllers`) | SIL Kit prefers the `Network` of the configuration, so it would be unclear which name applies. This holds also for a controller that has nothing to do with the renamed networks. A `--map-network` that renames nothing is not refused. A vehicle-sil build that uses a SIL Kit not built from source cannot read the configuration, so it refuses a renaming `--map-network` together with `--silkit-config`, whatever the configuration contains (the same holds for `PeerOptions::network_names` with a participant configuration). |
| The ECU registers a variable directly on `agent()` with the name of a renamed monitoring output | For example `j1939.VehicleCAN.bus_state`. `input()`, `output()` and the like already refuse a name that starts with `j1939.` (see [Reserved variable name prefixes](#reserved-variable-name-prefixes)). |

On Windows (MinGW), give the networks you rename ASCII names; a `--map-network` with a non-ASCII character gives exit code 4. Windows converts arguments to the code page and replaces the characters that the code page lacks with `?` or a similar character, so the converted value would put the ECU on a network of another name without an error. `ecu_main` checks this argument alone against the command line before the conversion (when `main()` passes its `argc` and `argv` unchanged). On Linux, names can be ASCII or UTF-8 (other byte sequences give exit code 4).

With `vehicle_sil_launch`, do not write this argument in `command`: write `network_names` for the participant in the configuration file instead (`network_names: { VehicleCAN: J1939_CAN }`; see "Matching network names (`network_names`)" in [simulation-config.md](simulation-config.md#matching-network-names-network_names)). `run` starts the executable with `--map-network` after the other arguments, and `check` checks the reported networks under the mapped names. `--map-network` in the `command` of an ecu_main executable (`describe: true`) is the configuration error `launcher-argument`.

A program that does not use `ecu_main` passes the same mapping (simulation name to code name) in `SilKitOptions::network_names` (`Simulation`) or `PeerOptions::network_names` (`Peer`). In a `Simulation` with several ECUs in one process, the mapping applies to all of them, and an ECU without a network of that name is left as it is (only `ecu_main` treats a missing name as an error). The name given to `Peer::j1939(name, port)` (a port the caller created) cannot be mapped (`std::invalid_argument`): the port does not connect to a SIL Kit network, so there is nothing to rename.

### Changing J1939 node addresses and NAMEs (`--j1939-node`)

To put 2 copies of the same ECU executable on one J1939 network, give the second copy another address and NAME without a rebuild. Without it, both copies ask for the same address with the same NAME, and neither of them gets an address.

```sh
engine_ecu --registry-uri silkit://localhost:8500 --name Engine2 --j1939-node J1939_CAN=address:0x01,identity_number:2
```

The value is `<network>=<setting>:<value>[,...]`: the simulation's name of a J1939 network of the ECU (for a network renamed with `--map-network`, the new name), `=`, then settings separated by commas. In the example, the ECU's only node on `J1939_CAN` asks for address 0x01 instead of the address in its code, and its NAME has the identity number 2.

| Setting | Range | Meaning |
|---|---|---|
| `from` | 0..254 | Picks the node by the address that the ECU's code gives it in `add_node()`. Without `from`, the line is for the ECU's only node on that network. |
| `address` | 0..254 | The address that the node asks for. 254 (any free address) only for a node whose NAME is arbitrary address capable. |
| `identity_number` | 0..2097151 | The identity number field of the node's NAME. |
| `function_instance` | 0..31 | The function instance field of the node's NAME. |
| `ecu_instance` | 0..7 | The ECU instance field of the node's NAME. |

Numbers are decimal, or hexadecimal with `0x`. Give each setting at most once, and at least one setting besides `from`. The NAME fields need a node that has a NAME.

| Item | Rule |
|---|---|
| When it applies | In `add_node()`, while the ECU is set up. A node that the ECU adds after it starts keeps the values in its code. |
| What changes | Only that node's address and the NAME fields of the line. The other fields of the NAME stay as in the code. |
| What does not change | What other ECUs have in their code about this node (its address, its whole NAME), and other constants in the ECU's own code (see [Requirements for ECU authors](#requirements-for-ecu-authors)). |
| Several nodes | Give one `--j1939-node` for each node. When the ECU has more than one node on the network, each line needs `from`. |
| `--describe` and `--local` | Not allowed with either (exit code 4). `--describe` gives the addresses and NAMEs in the code: an executable that accepts `--j1939-node` lists `"j1939-node"` in `features` (see [Keys](#keys)). |

Values that give exit code 4: no `=`, an empty network name or one that starts with `-`, an empty setting, a setting without a value, an unknown setting, the same setting twice, a value that is not a number or is out of range, a line with only `from`, a value that is neither ASCII nor UTF-8 (on Windows, like `--map-network`, a value with a non-ASCII character), 2 lines for the same node (the same network and `from`), and a line without `from` together with another line for the same network.

Cases that give exit code 3, each reported in one line before the ECU joins the simulation:

| Case | Notes |
|---|---|
| The ECU has no J1939 network of that name | The message lists the ECU's J1939 networks (a renamed one with the name in its code) and the `--map-network` to give if the simulation uses another name for one of them. |
| The ECU has no node on that network when it is set up | |
| The ECU has no node at the address of `from` | The message lists the addresses of the ECU's nodes there. `from` is the address in the ECU's code, not the new one. |
| The line cannot pick a node | A line without `from` on a network where the ECU adds a second node, or 2 nodes at the same address in the code. |
| The line cannot be applied to its node | A NAME field for a node without a NAME, or 254 for a NAME that is not arbitrary address capable. |

A program that does not use `ecu_main` passes the same lines in `SilKitOptions::j1939_nodes` (`Simulation`, for each ECU name) or `PeerOptions::j1939_nodes` (`Peer`); see "SIL Kit options (`SilKitOptions`)" in [j1939-guide.md](j1939-guide.md#sil-kit-options-silkitoptions).

## Requirements for ECU authors

When you run several ECUs with `vehicle_sil_launch`, each ECU executable must meet these requirements. A requirement that is not met shows up as a finding or in the launcher's output.

| Ask the ECU author to | If they do not |
|---|---|
| Build `main()` with `ecu_main()`. | `check` cannot check the ECU. |
| Use the names agreed with the integrator (the person who writes the configuration file), such as network names, and check them with `--describe` before handing the ECU over. | `check` reports findings ([simulation-check.md](simulation-check.md)). |
| Build with a version of vehicle-sil that is not newer than `vehicle_sil_launch`. | The check reports the note `version-mismatch` whenever the versions differ. `vehicle_sil_launch` reads and writes the variables of ECU executables built with its own version of vehicle-sil or an older one, back to 0.1.0. An ECU built with a newer vehicle-sil than the launcher is not promised to work: if its variables cannot be used, the check reports the warning `control-link-version`; use a launcher at least as new as the ECU. |
| End the processes that the ECU started itself when the ECU ends. | The run reports the warning `left-processes` (see "Stopping a run" in [simulation-run.md](simulation-run.md#stopping-a-run)). |
| Write stdout and stderr in UTF-8. | `vehicle_sil_launch run` shows bytes that are not UTF-8 as `?`. |
| Do not use the prefixes in the next table in variable names. | The ECU exits with exit code 3. |
| Read a node's own address with `node.address()`, not from a constant of your own. Do not keep the addresses or the whole NAMEs of other ECUs as constants: find another ECU by the NAMEs that `Network::name_at()` reports, compared without the identity number and the 2 instance fields, or take its address or NAME as an argument. | When a simulation changes addresses and NAMEs with `--j1939-node`, the ECU sends to or from an address that is no longer the right one. |

### Reserved variable name prefixes

Names that start with these prefixes are for the variables that vehicle-sil adds. An ECU cannot declare them, whether it runs on SIL Kit or on the local buses and whether monitoring is on or off.

| Prefix | Used for | Example |
|---|---|---|
| `ecu.` | Variables that every ECU has | `ecu.connected` |
| `peer.` | Monitoring variables that `Peer` adds | `peer.steps` |
| `j1939.` | Monitoring variables that `Peer` adds for each J1939 network (`j1939.<network name>.`) | `j1939.J1939_CAN.frames_sent` |
| `can.`, `lin.`, `eth.` | Kept for the monitoring variables of raw CAN channels, LIN nodes and Ethernet channels (none yet) | |

Only the start of the name counts: `engine.j1939.speed` is an ordinary name. `spy.` is not reserved: `vehicle_sil::spy::Spy` declares its variables like an application does, under `spy.` by default (`SpyOptions::statistics_as`), so do not use names under it for your own variables in an ECU with a `Spy`.

To fix an ECU that uses one of these prefixes, rename the variable, and the scenarios, `variables` entries and GUI settings that use the old name.

An ECU that joins a SIL Kit simulation (`Peer`) adds the monitoring variables after the variables that the ECU declared (`PeerOptions::monitor`, enabled by default). They are not in `variables` of `--describe`, but during a run they appear next to the ECU's variables. For the list of variables, see "Peer monitoring" in [internals/control.md](internals/control.md#peer-monitoring).

When a colliding name causes an error depends on how it is declared and on the prefix.

| Declaration | When it fails |
|---|---|
| Declaring a name that starts with one of the prefixes above with `input()`, `output()` and the like of `Ecu` | At the declaration, with `std::invalid_argument`, so `--describe` and `--local` find it too. |
| A name registered directly with `agent()` collides with a monitoring variable of `Peer` | Not checked at registration; found when the ECU starts on SIL Kit (exit code 3 with `ecu_main`). |

The `std::invalid_argument` of `input()` and the like is the same for `Peer` and `LocalBench`, and its message gives the variable name and the rule. Variable names belong to each ECU, so they need not be aligned between ECUs.

## `Ecu::describe()`

`Ecu::describe()` returns the configuration of the ECU as an `EcuDescription` (`vehicle_sil/ecu/describe.hpp`).

| Member | Meaning |
|---|---|
| `name` | Name of the ECU (the participant name in SIL Kit). |
| `step` | Step size. Empty for a real-time `Peer`. |
| `networks` | The networks the ECU connects to. |
| `mac` of `networks` | Ethernet only. The ECU's own address passed to `ethernet()` (`eth::ChannelOptions::mac`). |
| `nodes` of `networks` | J1939 only. The values given when each node was added (`NodeOptions`), in the order the nodes were added. |
| `variables` | The `control::VariableInfo` of the variables. |
| `control`, `features` | Empty when `Ecu::describe()` returns. `ecu_main` fills them in for `--describe` (`control`: see [Exposing variables](#exposing-variables---registry-uri); `features`: see [Keys](#keys)). |

**`networks`**: In the order in which `j1939()`, `can()`, `lin_master()`, `lin_slave()` and `ethernet()` were called, across kinds. One call is one element. If the same ECU has `j1939()` and `can()` with the same name, the same CAN network appears twice (because there are 2 controllers). Each element has `name`, `use` (`NetworkUse`: `Raw`, `J1939`, `LinMaster`, `LinSlave`, `Ethernet`), `kind()` (the `NetworkKind` that follows from `use`: `Can`, `Lin`, `Ethernet`), `nodes` and `mac`.

**`mac`**: Empty if no address was passed to `ethernet()` (an ECU whose application decides the address at run time). The channel does not write this value into frames. The check uses it to confirm that the addresses of the ECUs on a network do not overlap.

**`nodes`**: `address` is the address that the node requested. It does not change when address claiming leaves the node at another address (the current address is `Node::address()`, the requested one `Node::requested_address()`). `j1939::kNullAddress` (254) marks a node that takes a free address from the arbitrary address range. `name` is the NAME (empty for a node without a NAME; the JSON key is `j1939_name`). `arbitrary_address_capable()` is the arbitrary address bit of the NAME.

**`variables`**: First comes `ecu.connected`, the input that every ECU has; it is listed even before the Agent is created. Then come the variables the ECU declared (with `input()`, `output()` and the like, and those registered directly with `agent()`), in declaration order. The monitoring variables that `Peer` adds (`PeerOptions::monitor`) are not included.

`ecu.connected` is a bool input. While it is `false`, the ECU is disconnected from all its buses, and its steps keep running (see "Disconnecting from the bus (`ecu.connected`)" in [internals/execution.md](internals/execution.md#disconnecting-from-the-bus-ecuconnected)).

When you can call `describe()`, and what it returns:

| Item | Rule |
|---|---|
| When you can call it | At any time (during construction, before and after the run). |
| During the run | Only from within a step of that ECU (as for the other functions). |
| Effect on declarations | It does not close the declarations (it does not fix the Agent's schema, so you can add variables, networks and nodes afterwards). |
| Differences between implementations | An ECU built the same way from the same class returns the same content on `Peer` (virtual time) and on `LocalBench`, including `name` and `step`. |
| Before and after the run | The same content. |
| Comparison | `EcuDescription` and the types of its members can be compared with `==`. |

## JSON format

`to_json(const EcuDescription&)` (`vehicle_sil/ecu/describe.hpp`) turns an `EcuDescription` into a JSON object. `--describe` writes this string. The same content always gives the same bytes.

| Item | How it is written |
|---|---|
| Key order | As in the table below. |
| Keys without a value | Always written (as `null`, an empty string or an empty array). |
| Indent and lines | An indent of 2 spaces, one key or element per line, and a line break at the end. |
| Line endings | LF (`--describe` never writes `\r\n`, not even on Windows). |
| Integers | Decimal, with no digits lost. |
| Floating-point numbers | The shortest form that reads back as the same `double` (`std::to_chars`; for example `0.1`, `100`, `1e+300`). |
| Non-finite values | `null`. |
| Times | Integer nanoseconds (`step_ns`). |
| NAME | A string of `"0x"` and 16 upper-case hexadecimal digits (key `j1939_name`), because the 64-bit value is an identifier. |
| Strings | Valid UTF-8 is written as is. `"`, `\` and control characters below 0x20 are escaped (`\b`, `\f`, `\n`, `\r`, `\t`, others as `\u00XX`). |
| Bytes that do not start a valid UTF-8 sequence | Each byte becomes U+FFFD (the output is always valid JSON). |
| Enumerator values out of range (a `NetworkUse` made with a cast, for example) | `std::logic_error` is thrown instead of writing a made-up name. |

Enum values are 64-bit integers and can exceed 2^53. Read them as integers.

### Format version

The first key, `vehicle_sil_ecu`, is the format version.

| Change | `vehicle_sil_ecu` |
|---|---|
| The meaning of a key changes, or a key is removed | Goes up. |
| A key is added | Does not go up. |
| A key that takes fixed values (`kind`, `use`, `type`, `causality`) gets a new value | Does not go up. |
| `features` gets a new name | Does not go up. |

A reader rejects an unknown version and ignores unknown keys. An unknown value of a key that takes fixed values is an error for that ECU (the `vehicle_sil_launch check` finding `describe-unknown-value`; checking of the other participants continues). This happens when an old `vehicle_sil_launch` reads an ECU from a newer vehicle-sil. `features` is not a key that takes fixed values but an open list: a reader ignores the names it does not know.

`vehicle_sil` is the version of vehicle-sil that the ECU was built with (the version of the CMake project). Use it to find version differences when you run executables built separately in one simulation.

### Keys

| Key | Value |
|---|---|
| `vehicle_sil_ecu` | Format version (currently 1) |
| `vehicle_sil` | The vehicle-sil version string |
| `name` | Name of the ECU |
| `step_ns` | Step size (ns). `null` for a real-time `Peer` |
| `networks` | Array of networks (in the order of `describe()`) |
| `networks[].name` | Name of the network |
| `networks[].kind` | `"can"`, `"lin"`, `"ethernet"` |
| `networks[].use` | `"raw"`, `"j1939"`, `"lin_master"`, `"lin_slave"`, `"ethernet"` |
| `networks[].nodes` | Array of J1939 nodes (has elements only when `use` is `"j1939"`) |
| `networks[].nodes[].address` | The requested address (0–253; 254 takes one from the arbitrary address range) |
| `networks[].nodes[].j1939_name` | NAME (`"0x"` and 16 digits). `null` for a node without a NAME |
| `networks[].nodes[].arbitrary_address_capable` | The arbitrary address bit of the NAME (`false` without a NAME) |
| `networks[].mac` | The ECU's address on an Ethernet network (in the form `"02:00:00:00:00:01"`, lower case). `null` if not reported, and on networks other than Ethernet |
| `variables` | Array of variables (`ecu.connected`, then in declaration order) |
| `variables[].name` | Name of the variable |
| `variables[].type` | `"bool"`, `"integer"`, `"real"`, `"enum"` |
| `variables[].causality` | `"input"`, `"output"`, `"parameter"` |
| `variables[].unit` | Unit (`""` if none) |
| `variables[].min`, `max` | Range (both ends included). `null` if none |
| `variables[].step` | Suggested increment for editing the value. `null` if none |
| `variables[].description` | Description (`""` if none) |
| `variables[].items` | Array of the values of an `"enum"` (`name` and a 64-bit integer `value`). Empty for other types |
| `variables[].builtin` | `true`: a variable that vehicle-sil gives every ECU (its name starts with `ecu.`; `ecu.connected`). `false`: a variable that the ECU declared |
| `control` | How this executable exposes its variables to the simulation under `--registry-uri`. `null` if it does not (`--no-control`) |
| `control.transport` | The transport. Currently only `"silkit"` (SIL Kit publish/subscribe) |
| `control.wire_version` | The message format version (an integer) |
| `features` | Array of the names (strings) of the arguments and behaviors that this executable accepts from the program that starts it. `"map-network"`: it accepts `--map-network`. `"j1939-node"`: it accepts `--j1939-node`. An `ecu_main` executable writes the same list whatever its arguments |

How to read JSON in which a key is missing:

| Key | If missing |
|---|---|
| `networks[].mac` | The JSON of an ECU from an earlier version. Read it as `null`. |
| `control` | An executable built with a vehicle-sil from before this key existed. Treat it as `null`. |
| `features` | The JSON of an ECU from an earlier version. Read it as an empty array. |

Screens and other tools use `variables[].builtin` to show these variables separately from the application's variables. `vehicle_sil_launch` reads and writes the variables if it speaks `control.wire_version`: its own version of the messages and the older ones (version 1 is what an ECU built with vehicle-sil 0.1.0 reports). If the ECU reports a later version than the launcher speaks, the variables cannot be read or written.

Example: the output of `vehicle_sil_engine_ecu --describe`. The CTest test `ecu_main.describe` compares the output of the executable with this block, byte for byte.

<!-- vehicle_sil_engine_ecu --describe -->
```json
{
  "vehicle_sil_ecu": 1,
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
  ],
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
  "control": {
    "transport": "silkit",
    "wire_version": 2
  },
  "features": [
    "map-network",
    "j1939-node"
  ]
}
```

## Running several ECUs in one simulation

To run 2 ECU executables in one SIL Kit simulation, write a configuration file and pass it to `vehicle_sil_launch run` ([simulation-config.md](simulation-config.md), [simulation-run.md](simulation-run.md)). `run` runs the check, starts the registry, the controller and each ECU, monitors them to the end and shows how each ECU ended.

```sh
cd build/<preset>/examples/ecu_main
vehicle_sil_launch run simulation.yaml
```

Before you run, you can use the configuration file and `vehicle_sil_launch check` to find mismatches in the ECU names, step sizes, networks, and J1939 addresses and NAMEs ([simulation-check.md](simulation-check.md)). For the steps, see the guide [Integrate and run several ECUs](howto/integrate-ecus.md).

### Running without the launcher

The same setup, started with plain commands and without the launcher. The ECUs join with `--registry-uri`, and `vehicle_sil_sim_controller` waits for all of them to join, starts the simulation and stops it at 1 second of virtual time.

```sh
sil-kit-registry --listen-uri silkit://localhost:8500 &
vehicle_sil_engine_ecu --registry-uri silkit://localhost:8500 &
vehicle_sil_dashboard_ecu --registry-uri silkit://localhost:8500 &
vehicle_sil_sim_controller --registry-uri silkit://localhost:8500 --required Engine,Dashboard --stop-at-ms 1000
```

In `--required`, list the name of each ECU (`name` in `--describe`).

## See also

| To find | See |
|---|---|
| How to write an ECU class | [j1939-guide.md](j1939-guide.md) |
| Steps to build and run one ECU | [Create your first ECU](howto/first-ecu.md) |
| Keys of the configuration file and arguments of `vehicle_sil_launch` | [simulation-config.md](simulation-config.md) |
| The check order and the finding codes | [simulation-check.md](simulation-check.md) |
| Running, stopping and results | [simulation-run.md](simulation-run.md) |
