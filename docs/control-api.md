# Using the Control API

[日本語](ja/control-api.md)

The Control API is a gRPC API for controlling and monitoring a running simulation from outside (`proto/vehicle_sil/control/v1/control.proto`). This page is the reference for starting it, authentication, declaring ECU variables, the list of calls, the Python client and the CLI, and the rules for controlling time. To try it one step at a time, start with the guide [Read and write signals from outside](howto/operate-signals.md).

| To find | See |
|---|---|
| How clients, the launcher and ECUs connect | [Overview](#overview) |
| Building a launcher with the Control Server | [Build](#build) |
| The address for `--control`, and when the server starts and ends | [Connect to the launcher](#connect-to-the-launcher) |
| Keeping a client to one run (`vehicle-sil-run`, `PingReply.run`) | [The run a call is for](#the-run-a-call-is-for) |
| Writing values before the first simulation step | [Write before the start (`--start-paused`)](#write-before-the-start---start-paused) |
| The order and rules of time commands | [Stdin commands and client commands](#stdin-commands-and-client-commands) |
| TLS, tokens and roles | [Authentication](#authentication) |
| Declaring variables in ECU code | [ECU variables](#ecu-variables) |
| gRPC calls and the matching Python methods, CLI commands and roles | [List of calls](#list-of-calls) |
| The Python client | [Python](#python) |
| Running several simulations in one launcher (`Serve`, `RunHandle`) | [Run sequentially in one launcher (`Serve`, `RunHandle`)](#run-sequentially-in-one-launcher-serve-runhandle) |
| Reading the time and values as of the same instant | [Read time and values together (`snapshot()`)](#read-time-and-values-together-snapshot) |
| Using it from the command line | [CLI](#cli) |
| Status codes and exceptions | [Errors](#errors) |
| What it cannot do, and limits | [Limitations](#limitations) |

## Overview

An ECU declares the variables that can be read and written from outside. Clients (Python, the CLI, the GUI) use the API of the Control Server to work with those variables and with virtual time. The Control Server runs inside the launcher when you add `--control` to `vehicle_sil_launch run`, the command that runs the simulation.

| Between | Protocol |
|---|---|
| Client and launcher (Control Server) | gRPC |
| Launcher and ECUs | SIL Kit |

- If an ECU built with `ecu_main` declares variables, the launcher can read and write them with no further code. ECU executables do not contain gRPC.
- `variables` in the `--describe` output lists the names and types of the variables of an ECU executable (see "JSON format" in [ecu-executable.md](ecu-executable.md#json-format)).
- An ECU that you start yourself instead of having the launcher start it (from a debugger, or in another terminal) connects to the same launcher. Add `manual: true` to the participant in the configuration file, or use `run --manual <name>` (see "Starting a participant yourself (`manual`, `--manual`)" in [simulation-config.md](simulation-config.md#starting-a-participant-yourself-manual---manual)).
- [internals/control.md](internals/control.md) details the guarantees: when writes are applied, ordering, limits and threads.

## Build

The prebuilt release tools archive (`vehicle-sil-tools-*`) already includes `vehicle_sil_launch` with `--control` (the Control Server) and the Python client with pre-generated gRPC stubs; you only need to build when compiling from source.

```sh
cmake --preset linux-gcc-release -DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON
cmake --build --preset linux-gcc-release
```

In this build, `vehicle_sil_launch` has `--control`, and `features` in `vehicle_sil_launch version --json` includes `control-server`. In a build without the Control Server, `--control` is a usage error (exit code 4), and stderr explains how to rebuild.

| Topic | Details |
|---|---|
| If gRPC is not found | FetchContent fetches it. The first download and build take a few minutes. |
| gRPC on Windows (MSYS2 UCRT64) | Install it beforehand with `pacman -S mingw-w64-ucrt-x86_64-grpc`. |
| Running the Control Server tests as well | Use `linux-gcc-control` on Linux or `windows-mingw-control` on Windows (GCC Debug with this setting) with `scripts/verify.py <preset>` (see "Build vehicle-sil itself" in [build.md](build.md#build-vehicle-sil-itself)). |
| Dependency details | See [dependencies.md](dependencies.md). |

## Connect to the launcher

```sh
vehicle_sil_launch run --stdin-commands --start-paused --control 0 simulation.yaml
```

With `--control <address>`, the launcher starts the Control Server. `<address>` is one of these:

| `<address>` | Listens on |
|---|---|
| A port number only | That port on 127.0.0.1 (0 picks a free port). |
| `<host>:<port>` | That address. |
| `unix:<path>` | A socket at that path. |

The OS limits the length of a `unix:` path (107 bytes on Linux; a `%XX` such as `%20` counts as the 1 byte it stands for). If the path is too long, the launcher prints the length and the limit before it starts listening, and exits with a usage error (exit code 4): `the path of the socket is ... bytes long and this system takes ... at most: give a shorter path, for example unix:/tmp/vehicle-sil-control.sock`. Use a short path, for example under `/tmp`.

| Topic | Details |
|---|---|
| The actual address (the chosen port if you gave port 0) | Reported in `address` of the `control_server` event, and in the line `[vehicle_sil_launch] control server at <address>` of the human-readable output. |
| When the server starts | After the check passes, before the registry. A run whose check fails does not start it. |
| If it cannot start (the port is in use, for example) | Nothing starts, and the run ends with a setup error (`setup`, exit code 3). |
| What the server covers | The run of that launcher only. |
| What it handles (ECU variables) | The list of peers, values, writes, results of operations, subscriptions. |
| What it handles (virtual time) | Pause, resume, step, run until a time, speed, stop. |
| List of peers | Required participants from the configuration file are in the list even without an Agent. |

[examples/launch_control](../examples/launch_control) contains examples.

| Example | What it does |
|---|---|
| `write_and_read.py` | Minimal `Launch` script: starts paused, writes throttle before time 0, steps 500 ms and prints engine speed. |
| `write_before_start.py` | Starts the launcher from Python, connects, writes before the start, and reads the value back to verify it. |
| `pause_and_step.py` | Pauses a running simulation, writes at a given time, reads the time and values together with `snapshot()` to verify them, then changes the speed and runs to the end. |
| `serve_runs.py` | Keeps one resident launcher (`Serve`), prepares once, and runs two isolated simulations sequentially with `RunHandle` and `run.control()`. |

### When the run ends

When the run ends, whether normally, by a stop request, by `--timeout-ms` or by a participant failure, the launcher ends the participants and then closes the server before it prints the result.

| Topic | Details |
|---|---|
| Calls that are waiting (`Watch`, `WaitOperation`, `WaitPaused`) | End when the server closes (`Watch` with the end of its stream, `WaitPaused` with `FAILED_PRECONDITION` or `CANCELLED`). |
| Clients that stay connected | The launcher does not wait for clients, so they do not delay its exit. |
| Between the end of the simulation and the closing of the server | Clients can read the values as of the end. |

### The run a call is for

Each run has an ID that the launcher gives it: an opaque string. Compare it for equality only; do not take it apart. Each run has its own Control Server, which knows the ID of its run. gRPC connects a channel again by itself to whatever server listens at the address, so at a fixed address a client of an earlier run would reach the next run's server. A client names the run it means, and the server refuses calls for another run.

| Topic | Details |
|---|---|
| How a client learns the ID | `run` of `PingReply` (`Ping`). With `serve`, the launcher's events tell it too (see [simulation-events.md](simulation-events.md)). |
| How a call names the run | The metadata `vehicle-sil-run: <ID>`. In Python, `Control(address, run=<ID>)`, `Control.for_current_run(address)`, or `run.control()` from `Serve` (see [Python](#python)). |
| A call that names another run | Rejected with `FAILED_PRECONDITION`; `vehicle-sil-reason` in the trailing metadata is `run-changed`. Nothing of the call reaches the ECUs or the time. |
| A call that names no run, with `vehicle_sil_launch run` | Served, as before runs had IDs. |
| A call that names no run, with `vehicle_sil_launch serve` (one launcher, one run after another; see [simulation-run.md](simulation-run.md)) | Rejected with `FAILED_PRECONDITION`; `vehicle-sil-reason` is `run-required`. In Python, `ControlError` appends a hint pointing to `Control.for_current_run()` or `Control(..., run=...)`. |
| `Ping` | Never rejected for the run, so that a client can always learn the current one. |
| Order of the checks | The token (`UNAUTHENTICATED`), then the role (`PERMISSION_DENIED`), then the run. A client without a valid token learns nothing about the run, and a client with one learns no more than `Ping` tells it. |
| The audit log | Each line begins with `[run <ID>] `, so one file (`--control-audit-log`) can hold the lines of several runs. A rejected call is logged with the run it named. |

The run ID is not a credential: anyone who may call `Ping` can read it. It only keeps a client to its run. What a client may do is decided by its token and role (see [Authentication](#authentication)). To keep the clients of one run out of the next one completely, give each run its own credentials file.

### While a scenario runs

In a scenario run (`--scenario`), a client can read, wait, move time and stop. While the scenario is replaying (including up to `end_ms`), it cannot write variables. With `"after_scenario": "pause"` in the run request (launcher feature `scenario-handover`), a scenario that passes hands over to manual control on the same Run while paused, unlocking writes.

| Call | During scenario replay | After `after_scenario: "pause"` handover |
|---|---|---|
| Read and wait calls, `Stop` | Work as usual. | Work as usual. |
| Write calls (`Write`, `ForgetPeer`) | Rejected with `FAILED_PRECONDITION` (`vehicle-sil-reason`: `scenario-running`). | Unlocked on the same Run (`handed_over: true` in `state`), and work as in a run without a scenario. |
| Calls that move time (`Pause`, `Resume`, `Step`, `RunUntil`, `SetSpeed`) | Work as in a run without a scenario (launcher feature `scenario-time`). They do not change the result of the scenario. | Work as usual from the paused handover step (`Step`, `RunUntil`, `Resume` advance from that virtual time). |

`vehicle-sil-reason` in the trailing metadata (value `scenario-running`) identifies the rejection of a write before handover. For the table of calls and the guarantees, see "Watching variables during a run (`--control`)" in [scenario.md](scenario.md#watching-variables-during-a-run---control).

Handover rules:

- When `"after_scenario": "pause"` is requested and the scenario passes without a concurrent stop or abort, the launcher atomically pauses the simulation at the scenario's end controller step (`over_at_ms`), keeps the same Run (`PingReply.run` / `RunHandle`) and ECU state, emits `state` with `handed_over: true` and `scenario_over_at_ms`, and unlocks `Write` and `ForgetPeer` while the run stays open. Even at `speed: "max"`, time remains paused at that step until `Step`, `RunUntil`, or `Resume`.
- When `"after_scenario": "stop"` (the default) is used, or when the scenario fails (`scenario-failed`, `scenario-timeout`, `scenario-write`, `scenario-not-observed`), ends `incomplete` (`stop_at_ms`, `Stop`, `stop`, interrupt, `--timeout-ms`, participant failure), or races with a stop request, handover does **not** occur: `handed_over` remains `false`, `Write` and `ForgetPeer` stay refused with `scenario-running`, and the run ends.

The scenario writes as the client `scenario`: its operations have `client` `scenario` (`Operation.client` in `Watch`, `GetOperation` and `WaitOperation`). This name is fixed. A watch that does not need the scenario's own operations (there can be one in every step) names it in `WatchRequest.operation_clients_excluded`: the operations of the clients in that list are not sent at all, and `Update.dropped` does not count them. An empty list sends every operation. The initial updates of a watch have no operations. A name matches `Operation.client` exactly: a gRPC client's `client` is `<name>@<the address of its connection>` and changes with each connection, so only a fixed name is useful in the list, and `scenario` is the only fixed name now. The GUI server's watch leaves out `scenario`. The Python client's `Control.watch()` has no argument for the list, so a Python watch receives the scenario's operations too.

### In a run that is connected outside

A run that is connected outside the simulation (`--connect`, or an enabled `role: bridge` participant) keeps real time unless it was started with `--time-policy free` (see "Time of a run that is connected outside (`--time-policy`)" in [simulation-run.md](simulation-run.md#time-of-a-run-that-is-connected-outside---time-policy)).

| Call | In a run that keeps real time |
|---|---|
| `Pause`, `Step`, `RunUntil`, `SetSpeed` with a speed other than 1 | Rejected with `FAILED_PRECONDITION`; `vehicle-sil-reason` in the trailing metadata is `outside-real-time`. A wrong argument (a step of 0, a speed out of range) gets this answer too, not `INVALID_ARGUMENT`. |
| `Resume`, `SetSpeed` with the speed 1, `Stop` | Work as usual. |
| Read, wait and write calls | Work as usual (the rule is about time only). |

With `--time-policy free` every call works as usual, and the run ends with the note `outside-time-changed` after the first call that pauses, steps or changes the speed.

### Write before the start (`--start-paused`)

With both `--control` and `--start-paused`, once all participants have joined, every participant stops before its step at time 0 and waits for `resume`, `step` or `run_until`, from stdin or from a client. Values written during this wait are applied in each ECU's step at time 0. Parameters can be written too.

```python
from vehicle_sil_control import Control, Launch, ms

with Launch("vehicle_sil_launch", "simulation.yaml", start_paused=True) as launch, Control(launch.address) as c:
    c.sim.wait_paused()                               # stopped at 0 (time().running is false: not started yet)
    seq = c.write({"Engine": {"engine.throttle": 50.0}})
    c.wait(seq, until="queued")                       # advance only once every participant has it
    c.sim.step(ms(500)); c.sim.wait_paused()
    print(c.wait(seq).target("Engine").applied_at)    # 0
```

After writing, call `wait(seq, until="queued")` to confirm that every participant has received the write, then advance. If you advance before that, the write may miss time 0.

The state while stopped:

| Where to look | Value |
|---|---|
| `GetTime` | `now` is 0, `paused` is true, `running` is false (not started yet) and `ended` is false. |
| `WaitPaused` | Returns 0 at once. |
| `phase` of the events | Stays `waiting`. |
| Launcher output | The line `standing still before the first step until a resume, step or run_until command ...`. |

What the commands accepted while stopped do:

| Command | Effect |
|---|---|
| `resume` | The simulation starts. |
| `step`, `run_until` | The simulation advances from 0 and stops at that time. |
| `run_until` 0 | The simulation runs the step at time 0 and stops at 0 (the same stop as `--start-paused` without `--control`). |
| `pause` with a time (after 0) | The simulation advances, as with `run_until`. |
| `pause` (without a time), `speed` | The simulation does not advance. `speed` sets the speed for when it does. |
| A stop request (Ctrl+C, the `stop` command, the end of stdin, `Stop` from a client) | The launcher stops waiting, runs one step at time 0, then stops. |

Note that `pause` with a time advances only from this pre-start hold before step 0; once the simulation has started and is paused, `pause(at=T)` sets `pause_target` without resuming (use `run_until(T)` to advance to `T` and pause from either state).

Commands accepted before the stop (during the check, or while waiting for the participants to join) also take effect. The simulation stops only if, at the moment it would stop, it is still due to stop at 0.

| Command accepted before the stop | Effect |
|---|---|
| `resume` | The simulation starts without stopping. |
| `step`, `run_until` | The simulation advances from 0 without stopping and stops at that time (the same result as accepting the command after the stop). |
| A later `pause` | The simulation stops again. |

Commands from stdin behave the same way.

How the time spent stopped relates to timeouts:

| Setting | Time spent stopped |
|---|---|
| None | The stop itself has no time limit. The launcher waits until a stop request arrives. |
| `start_timeout_ms` | Not counted while stopped. |
| `--timeout-ms` | Counted, the same way as time spent paused. |

- `start_timeout_ms` applies separately before the stop (until all participants have joined and the ECUs that expose variables have connected) and after the simulation advances (until all participants reach their first step).
- If a participant stops responding before the stop or after the advance, the error is `simulation-start`. The message lists the participants that have not reached that point and their states.
- If `--timeout-ms` passes and no client has come, the launcher runs the step at time 0, then aborts with the error `timeout`. The message says that the simulation was stopped before the first step.
- If `--timeout-ms` passes after the advance, the message also gives how long the simulation was stopped before the first step.
- If a required participant goes away during the wait (its process ends, for example), the launcher stops waiting at once and ends the run as one that did not start (`ended-before-start`).

`--start-paused` without `--control` (with `--stdin-commands` only) runs the step at time 0 and then stops at 0. In that case, and when you write after a pause, set `at` later than the time at which the simulation is stopped (see [Rules for writes](#rules-for-writes) below).

### Stdin commands and client commands

The launcher accepts time commands from stdin (`--stdin-commands`) and from clients. Both go through the same path and follow the same rules (`step` only while paused, for example).

| Client call | Command name | Argument |
|---|---|---|
| `Pause` | `pause` | `at_ms`, for a pause at a time (updates `pause_target`; does not resume if already paused after start) |
| `Resume` | `resume` | None (clears any planned `pause_target`) |
| `Step` | `step` | `ms`, and `at_ms` (the time at which the simulation stops after the step; only while paused) |
| `RunUntil` | `run_until` | `at_ms` (advances to `at_ms` and pauses, whether currently paused or running) |
| `SetSpeed` | `speed` | The speed |
| `Stop` | `stop` | None |

| Topic | Rule |
|---|---|
| Processing order | One command at a time, in order. |
| Processing one command | Deciding whether to accept it, emitting the `command` event and handing it to the time gate happen with no other command in between. |
| Order in which commands take effect | The order of the events. |
| Where a command came from | `source` of the event (`stdin`, `eof`, `control_server`). |
| Stdin and a client send commands at the same time | The order of receipt decides which comes first. |
| Rounding of times | Up to whole milliseconds, then up to the controller's time step. The value in the event is the time at which the simulation actually stops. |

For example, if a `step` from a client and a `resume` from stdin overlap, the order of the events decides whether the simulation stops or keeps running. Time commands follow a "last call wins" rule: `resume` clears any planned `pause_target`, `pause` with `at_ms` while already paused after start updates `pause_target` without resuming, and `run_until` advances to `at_ms` and pauses whether the simulation is currently paused or running.

If the launcher does not accept a command, the client gets a gRPC error whose message is the same as `message` of the event.

| Reason for not accepting | Status code | `vehicle-sil-reason` in the trailing metadata |
|---|---|---|
| An argument error (a speed out of range, a step of 0 or less) | `INVALID_ARGUMENT` | None. |
| A command that is not possible now | `FAILED_PRECONDITION` | The `reason` of the event (`not-paused`, `not-running`, `outside-real-time` and the others in "Reason codes" in [simulation-events.md](simulation-events.md#reason-codes)). |

`Stop` is the same as `{"command":"stop"}` on stdin.

| Topic | Behavior of `Stop` |
|---|---|
| First and second call | The first stops the participants in order. The second ends them without waiting. |
| The result | `interrupted_by` is `"control server stop"`, `end` is `"stopped"`. The exit code is 0, or 5 if a scenario was stopped before its end (1 if the run found errors). |
| The reason text | Kept only in the audit log line. |
| After the simulation has ended (also when it never started) | Rejected like time commands (`accepted: false`, `FAILED_PRECONDITION`) once the launcher has taken the end. The result does not change. A `Stop` that arrives in the moment just before is accepted and changes nothing. |
| Order of events | In the same order as time commands. |

### Authentication

The listen address decides whether authentication is required.

| Address | Authentication |
|---|---|
| Loopback (a port number only, `localhost:<port>`, `[::1]:<port>`, a numeric `127.x.y.z:<port>`, `unix:<path>`) | None, unless you add `--control-credentials`. |
| An address that other computers can reach (`0.0.0.0:<port>`, for example) | TLS and credentials are required (`--control-tls-cert`, `--control-tls-key`, `--control-credentials`). |

- Anyone on the same computer can connect to a loopback address without authentication. Depending on the file permissions, other users on the same computer can also connect to a `unix:` socket.
- On an address that other computers can reach, missing TLS or missing credentials is a usage error (exit code 4).

A credentials file has one `<role> <name> <token>` per line.

```text
# role     name     token
viewer     guest    5e07d2... (a long random string)
operator   tester   0f3c9a...
admin      lead     7b21e4...
```

| Topic | Rule |
|---|---|
| `role` | One of `viewer`, `operator` and `admin`. |
| `token` | A secret string of at least 16 characters. |
| Comments | From a word that starts with `#` to the end of the line. |
| ECUs | Do not use tokens. |
| An `agent` line from an earlier version | A read error. Remove the line, as the message says. |
| A file with no entries (empty, or comments only) | A usage error, so that the option never leaves the server open to anyone. |
| Error messages | Give only the line number and what is wrong. They do not show the words of the line, because one of them may be a token. |
| Tokens as arguments | Not possible, because the launcher's arguments appear in `arguments` of the `hello` event and in the process list. |

The roles that calls require (for each call, see [List of calls](#list-of-calls)):

| Role | Allows |
|---|---|
| `viewer` | Reading only. |
| `operator` | Controlling time and writing. |
| `admin` | `Stop`. Only `admin` can call `Stop`. |

With `--control-audit-log <file>`, the launcher appends who changed what to the file, one line per call, beginning with `[run <ID>] ` (see [The run a call is for](#the-run-a-call-is-for)). If the file can no longer be written, the run goes on and those lines are lost.

There is no authentication between the ECUs and the launcher (SIL Kit). Anyone who can join the registry can write to the ECUs. Run the simulation on a closed network.

## ECU variables

An ECU class (a class that takes `Ecu&`) declares its variables in its constructor. The ECU's Control Agent (`Ecu::agent()`, one per ECU) holds the declarations. The same code works the same way with a SIL Kit `Peer` and with a local `Simulation()` or `LocalBench`.

```cpp
class Engine {
public:
    explicit Engine(vehicle_sil::Ecu& ecu) {
        ecu.input("engine.throttle", &throttle_, {.unit = "%", .min = 0.0, .max = 100.0});
        ecu.output("engine.speed", &rpm_, {.unit = "rpm"});
        ecu.every(10ms, [this](vehicle_sil::SimTime) { rpm_ = 800.0 + 50.0 * throttle_; });
    }
private:
    double throttle_ = 0.0;
    double rpm_ = 800.0;
};
```

### Declaration functions

| Function | Declares |
|---|---|
| `input(name, &value, options)` | A variable that can be written from outside. |
| `input(name, get, set, options)` | A variable that can be written from outside, for when the setter does more than store the value. |
| `input_enum(name, &value, items, options)` | An enumerated variable that can be written from outside. |
| `output(name, &value, options)` | A variable that can only be read from outside. |
| `output(name, get, options)` | A variable that can only be read from outside, whose value a getter returns. |
| `output_enum(name, get, items, options)` | An enumerated variable that can only be read from outside. |

The types are bool, integer, real and enum (`vehicle_sil::control::Value` is `std::variant<bool, std::int64_t, double>`). Values are read and written on the ECU's step thread, so the model needs no locks.

| C++ type | Variable type (`ValueType`) | `input()` / `output()` | `input_enum()` / `output_enum()` | Notes |
|---|---|---|---|---|
| `bool` | `bool` (`Bool`) | Supported | Compile error (`static_assert`) | — |
| Signed integers (`std::int8_t` .. `std::int64_t`, `int`, etc.) | `integer` (`Integer`) / `enum` (`Enum`) | Supported (`Integer`) | Supported (`Enum`) | Stored as `std::int64_t`; writes are checked against the C++ type range and `min`/`max`. |
| Unsigned integers up to 32 bits (`std::uint8_t`, `std::uint16_t`, `std::uint32_t`) | `integer` (`Integer`) / `enum` (`Enum`) | Supported (`Integer`) | Supported (`Enum`) | Range `0`..type max; stored as `std::int64_t`. |
| 64-bit unsigned integer (`std::uint64_t`) | `integer` (`Integer`) | `input()`: **Compile error (`static_assert`)**<br>`output()` without `min`/`max`: Supported (clamped) | `input_enum()`: **Compile error (`static_assert`)**<br>`output_enum()`: **Throws `std::invalid_argument` at runtime** | Control values are carried as `std::int64_t`, which cannot hold the full `uint64_t` range; use `std::int64_t` for writable inputs or enum values. Plain `output()` (with no `min`/`max`) saturates reads above `INT64_MAX` to `INT64_MAX`; `output()` with explicit `min`/`max` or `output_enum()` fails range checks (`std::invalid_argument`). |
| Floating-point (`float`, `double`, `long double`) | `real` (`Real`) | Supported | Compile error (`static_assert`) | Stored as `double` via `static_cast<double>(v)` on read (not clamped); writes are checked (not clamped) against finite `float` or `double` range (`real_max`) and `min`/`max`. |
| `enum` types | `enum` (`Enum`) | Compile error (`static_assert`) | `input_enum()`: Supported (except 64-bit unsigned underlying type)<br>`output_enum()`: Supported (except 64-bit unsigned underlying type, throws at runtime) | Values are carried as `std::int64_t`. Plain `input()` / `output()` accept only `bool`, integral, and floating-point types. |
| Other types (`std::string`, structs, pointers) | — | Compile error (`static_assert`) | Compile error (`static_assert`) | Only `bool`, integral, floating-point, and `enum` types are supported. |

| `VariableOptions` member | Meaning |
|---|---|
| `unit` | The unit. |
| `min`, `max` | The range. |
| `step` | The value increment. |
| `description` | A description. |
| `parameter` | If true, the variable is a Parameter: it can be written only before the first simulation step. |
| `availability` | If set, the variable reports whether it is in use, and this is its first state (usually `Availability::Unknown`). See "Availability of a variable" below. |

| Declaration rule | Details |
|---|---|
| When you can declare | Only before the run, while the ECU is being set up. |
| Declaring after the start | Throws `std::logic_error`. The message names the ECU. |
| The same name twice in one ECU | Throws `std::invalid_argument`. The message names the ECU. |
| What pointers and functions refer to | Objects that the ECU owns (the class that `Simulation::add()` creates, objects added with `emplace()`). |
| Destruction order | The ECU destroys the Agent after the networks and before the objects added with `emplace()`. |

### Agent options (`set_agent_options()`)

`set_agent_options(vehicle_sil::control::AgentOptions)` configures the sampling period and the bounded queue capacities of the ECU's Control Agent. Call it only before the first `input()`, `output()` or `agent()`. Calling it afterwards throws `std::logic_error`, and an invalid value throws `std::invalid_argument` at once; the message names the ECU.

| `AgentOptions` member | Default | Constraint | Meaning |
|---|---|---|---|
| `max_pending` | `256` | `> 0` | Maximum commands in flight (submitted and not yet consumed by `drain_results()`), and capacity of the command and result queues. |
| `max_events` | `1024` | `> 0` | Capacity of the `Agent::log()` event queue. Events beyond this limit are dropped and counted in `AgentStats::events_dropped`. |
| `sample_period` | `10ms` | `>= 0` | Variables are sampled in the first step at or after `0`, `sample_period`, `2 * sample_period`, ...; `0ms` (`SimDuration::zero()`) samples every step. |

```cpp
class Engine {
public:
    explicit Engine(vehicle_sil::Ecu& ecu) {
        ecu.set_agent_options({
            .max_pending = 512,
            .max_events = 2048,
            .sample_period = 20ms,   // or 0ms to sample every step
        });
        ecu.input("engine.throttle", &throttle_, {.unit = "%", .min = 0.0, .max = 100.0});
        ecu.output("engine.speed", &rpm_, {.unit = "rpm"});
    }
private:
    double throttle_ = 0.0;
    double rpm_ = 800.0;
};
```

When `vehicle_sil_launch` runs a scenario on `ecu_main()` ECUs whose variables the scenario reads (unless `samples: as_sent`), it passes `--sample-every-step`, which calls `ecu.sample_every_step()` to override `sample_period` to `0` while keeping the other `AgentOptions` fields.

Every ECU has an Agent, because every ECU has the input `ecu.connected`, even if it declares no variables. So even in an ECU without declared variables, the Agent's work (applying writes, and reading the values every `sample_period`) runs in each step. An `ecu_main` ECU sends one values message every `sample_period` (10 ms by default). With `Peer`, the monitoring outputs (`PeerOptions::monitor`) are added as well.

### When a write is applied

A write from outside is applied in the first ECU step at or after the given virtual time, before the application's work. In that step, J1939 reception and the timers of protocol parts run before the write is applied, so they use the new value from the next step on. Values are read after the application and the LIN processing. The timing is the same with SIL Kit and locally.

Writes are not compared with the current value. Each write is applied, even with the same value, and calls the setter each time: 2 writes in the same step call it twice, in the order of (time, `seq`). So you can implement an input such as "once per press" with a getter that always reads false and a setter that counts its calls.

A command with several writes for one ECU (one `set` of a scenario with several variables of a participant, or one `Write` call) is not all or nothing:

| When the write fails | What is applied |
|---|---|
| A check fails when the Agent receives the command (name, type, range, writability): `invalid` | None of the command's writes. |
| A setter throws in the ECU step: `rejected` | The writes before it in the command are already applied and stay applied. That write and the ones after it are not applied. |

Each ECU's result says which writes took effect. The ECU's writes are those of the request for it, in the order you gave them, counted from 0:

| Field (gRPC `TargetResult`, Python `Target`) | Meaning |
|---|---|
| `applied_writes` | How many took effect, from the first. `applied`: all of them. `rejected`: the position of the write whose setter threw. `invalid`, `late`, `cancelled`, `busy`: 0. |
| `failed_write` | The position of the write that `reason` is about: the one that failed the check (`invalid`) or whose setter threw (`rejected`). |

Either is empty (gRPC: not set, Python: `None`) when it is not known: while the write is open, for `unknown`, and in the cases that the comments of `TargetResult` in `control.proto` list. Use these fields, not `reason`, to tell which writes took effect: `reason` is text for people. A setter cannot be undone, so the Agent does not roll the earlier writes back. If a group of values must be applied together or not at all, check them in one setter, or validate them in the ECU before using them.

| How you write | Writes the ECU receives |
|---|---|
| A scenario's `set`, the Control API's `Write`, `agent().submit()` | As many as you write. |
| A GUI widget (`client.variables.bind`) | Fewer, when writes come in quick succession (see [gui.md](gui.md)). |

If values arrive before the server has received a widget's previous write, the widget merges them into the last one.

### Availability of a variable

An ECU whose application decides at run time which of its inputs and outputs it uses (for example from a configuration it reads in its first steps) can say so for each variable. A screen can then show an unused variable greyed out. The availability is information only: writes, reads, samples and scenarios do not look at it. A write to an `Inactive` variable is checked and applied as any other.

| State (`vehicle_sil::control::Availability`) | Meaning |
|---|---|
| `Unknown` | The application has not decided yet. |
| `Active` | In use. Every variable that does not report its availability counts as `Active`. |
| `Inactive` | Not in use in the present set-up of the ECU. |

| Rule | Details |
|---|---|
| Which variables report | Those declared with `VariableOptions::availability`, which gives the first state. Which variables report is fixed with the declarations; it does not change while the simulation runs. |
| Changing the state | `ecu.set_availability(name, state)` from a step of the ECU (`on_step()`, `every()`, `at()`, a handler). The same state again does nothing. |
| Errors | A name the ECU did not declare, a variable declared without `availability`, and the variables vehicle-sil gives every ECU (`ecu.connected`, the monitoring outputs): `std::invalid_argument`. During the set-up (before the run): `std::logic_error`; give the first state in the declaration instead. |
| Where the state goes | To the Control Server, when it changes, and again once a second of simulation time (so a server that connects later gets it too). A subscription to the Hub receives it only if it asks (`SubscriptionOptions::availability`). The gRPC service passes it on as below, and the GUI server to the screens ("ECU variables" in [gui/protocol](../gui/protocol/README.md#ecu-variables)); the standard browser GUI dims a variable that is not in use and marks it so (see "Behavior common to all widgets" in [gui.md](gui.md#behavior-common-to-all-widgets)). |
| An ECU built with an older vehicle-sil | Reports nothing: all its variables count as `Active`. The check notes it (`control-link-revision`). |

```cpp
ecu.input("io.relay", &relay_, {.availability = vehicle_sil::control::Availability::Unknown});
ecu.every(10ms, [this, &ecu](vehicle_sil::SimTime) {
    if (config_read_) {
        ecu.set_availability("io.relay", relay_fitted_ ? vehicle_sil::control::Availability::Active
                                                       : vehicle_sil::control::Availability::Inactive);
    }
});
```

Over gRPC, `VariableInfo.availability` in `ListPeers` and in the `PeerInfo` of `Watch` declares it: `AVAILABILITY_UNSPECIFIED` for a variable that does not report, else the state it starts with. `Watch` with `availability = true` sends a `PeerAvailability` (the `availability` kind of `Update`) for each peer that has a variable that reports. It holds the state of every variable in the order of `schema`, and `AVAILABILITY_ACTIVE` for a variable that does not report. It comes after the `PeerInfo`, with the initial updates (before `initial_done`) and then when a state changes. A new incarnation starts again from the declared states; a `PeerInfo` sent again for the same incarnation (a reconnect, a change of its SIL Kit state) does not reset them, so keep the states until another incarnation comes, and drop a `PeerAvailability` of an incarnation that is not the peer's current one. A watch keeps only the latest state of each peer, so it can skip a change in between. `peers` narrows it as for the other kinds, and a watch that does not ask never receives it. The availability is information only: `Write`, `GetValues`, `GetSnapshot` and the values of `Watch` do not change with it. The Python client does not ask for it.

### How the connection is made

| How the ECU is built | Connection to the Control Server |
|---|---|
| An `ecu_main` ECU | Connects automatically. |
| Your own `Peer` | Create an `AgentTransport` to connect (see below). |
| A local ECU (`Simulation()`, `LocalBench`) | Does not connect. Read and write it with `agent()` from the same process. |

Only ECUs that are SIL Kit participants connect to the launcher's Control Server. For your own `Peer`, create `vehicle_sil::control_silkit::AgentTransport transport(peer.agent(), peer.runtime());` after the ECU has declared its variables and before `start()`. Destroy it after `peer.stop()` and before the `Peer`.

From the same process, write with `ecu.agent().submit()` and read with `latest()`. For an ECU added with `Simulation::add<App>()`, get its Agent with `sim.ecu(name).agent()` (example: `examples/vehicle`).

## List of calls

The gRPC calls, the matching methods of the Python client (methods of `Control`; `sim.` marks methods of `Control.sim`) and CLI commands, and the role each call requires:

| gRPC | What it does | Python | CLI | Role |
|---|---|---|---|---|
| `Ping` | Returns the server version (`server_version`), the vehicle-sil release the server was built from (`vehicle_sil`) and the ID of its run (`run`). | `ping()` (returns `server_version`), `current_run()` (returns `run`) | None | `viewer` |
| `ListPeers` | Reads the list of peers and their variables. | `peers()`, `peer()` | `peers`, `show` | `viewer` |
| `GetValues` | Reads the latest values of one peer. | `values()` | `show` | `viewer` |
| `GetSnapshot` | Reads the time and values together. | `snapshot()` | None | `viewer` |
| `Watch` | Subscribes to values, events, operations and more. | `watch()` | `watch` | `viewer` |
| `GetTime` | Reads the time state. | `sim.time()` | `time` | `viewer` |
| `WaitPaused` | Waits until the simulation stops. | `sim.wait_paused()` | `pause --wait`, `step`, `run-until` | `viewer` |
| `GetOperation` | Reads the result of a write. | `operation()` | None | `viewer` |
| `WaitOperation` | Waits for the result of a write. | `wait()` | `set` (`--wait`) | `viewer` |
| `Write` | Writes variables. | `write()` | `set` | `operator` |
| `ForgetPeer` | Removes a disconnected peer. | `forget_peer()` | `forget` | `operator` |
| `Pause` | Pauses (or schedules a pause at `at`; does not resume if already paused after start). | `sim.pause()` | `pause` | `operator` |
| `Resume` | Resumes (and clears any planned `pause_target`). | `sim.resume()` | `resume` | `operator` |
| `Step` | Advances by the given time and stops (only while paused). | `sim.step()` | `step` | `operator` |
| `RunUntil` | Advances to the given time and stops (whether currently paused or running). | `sim.run_until()` | `run-until` | `operator` |
| `SetSpeed` | Changes the speed. | `sim.set_speed()` | `speed` | `operator` |
| `Stop` | Stops the simulation. | `sim.stop()` | `stop` | `admin` |

## Python

```sh
uv sync                                         # installs vehicle-sil-control (editable), grpcio, grpcio-tools, protobuf into .venv
source .venv/bin/activate                       # or put uv run in front of each command
python -m vehicle_sil_control.generate          # generates the stubs in python/vehicle_sil_control/_generated
```

| Topic | Details |
|---|---|
| Python version | 3.11 or later. |
| Versions and dependencies | `python/pyproject.toml` (`dependencies = []`, `python -S` safe; shares pure Events v4 protocol constants and NDJSON command builders `protocol.py` and lazy gRPC helpers `grpc_helpers.py` with `gui/server`), `pyproject.toml` at the repository root (`uv sync`, or `uv sync --only-group control` for Control API packages only) and `uv.lock` (see "Python (uv)" in [build.md](build.md#python-uv)). |
| Standard library only vs gRPC | `Launch`, `Serve`, `RunHandle`, `LaunchError`, `ServeError` and `control_address` use only the standard library (`python -S` safe) and import no gRPC modules. `Control`, `Control.for_current_run()`, `run.control()` and the CLI require `grpcio`, `protobuf` and generated stubs (`generate --out DIR` writes elsewhere; set `VEHICLE_SIL_CONTROL_STUBS=DIR` to load them). |
| Prebuilt tools archive (`Control` / gRPC) | Pre-generated stubs are already included under `<tools>/python`. Run with `PYTHONPATH=<tools>/python uv run --no-project --no-build --python ">=3.11,<3.15" --with-requirements <tools>/python/requirements-control.txt python <script.py>`. |
| Prebuilt tools archive (`Launch` / `Serve` only) | Zero extra dependencies: run with `PYTHONPATH=<tools>/python python -S <script.py>` (or any Python 3.11+). |
| Outside `.venv` or without `uv sync` | Set `export PYTHONPATH=$PWD/python` before running `python -m vehicle_sil_control.generate` or your scripts, and install `pip install grpcio grpcio-tools protobuf`. |
| Without uv (MSYS2 UCRT64) | Set `export PYTHONPATH=$PWD/python` and install with `pacman -S mingw-w64-ucrt-x86_64-python-grpcio mingw-w64-ucrt-x86_64-python-grpcio-tools`. |

Choose between `Launch` and `Serve`:

| Helper | Launcher mode | Choose when |
|---|---|---|
| `Launch` | `vehicle_sil_launch serve --once` | You need one simulation run, or you use `VEHICLE_SIL_TRACE`, a fixed `registry_uri`, or `manual` participants (only allowed with `--once`). |
| `Serve` / `RunHandle` | `vehicle_sil_launch serve` (or `once=True`) | You run several simulations sequentially on one configuration file without restarting the launcher, want `prepare()` / `status()` ahead of runs, or need per-run `RunHandle` (`run.id`, `run.control()`, settled `status`, `launcher_ends`). |

### Start the launcher (`Launch`)

`Launch` starts the launcher as a 1-run child process. It runs `vehicle_sil_launch serve --once --control <address>`, sends the initial `{"command":"run", ...}` on standard input, and reads the Events v4 (`vehicle_sil_events: 4`) stream on a separate thread. It uses only the standard library and needs no gRPC.

```python
from vehicle_sil_control import Control, Launch, ms

with Launch("build/linux-gcc-release/tools/launch/vehicle_sil_launch", "simulation.yaml", start_paused=True) as launch:
    with Control(launch.address) as c:   # address waits for the control_server event, then returns
        ...
    launch.stop()                        # {"command":"stop"} (leaving with also stops it if it is running)
    print(launch.wait()["result"])       # the same result as run --json
```

In `Launch(launcher, config, *, address="0", start_paused=False, case=None, params=None, after_scenario=None, arguments=(), cwd=None, stderr=None, keep_logs=True)`, all options after `config` are keyword-only. `case`, `params` (typed JSON parameter mapping), and `after_scenario` populate the initial stdin `run` `intent` directly. `arguments` accepts per-run flags (`--speed`, `--stop-at-ms`, `--scenario`, `--case`, `--variant`, `--disable`, `--enable`, `--manual`, `--param`, `--connect`, `--time-policy`, `--strict`, `--late-tolerance-ms`, `--request`, `--timeline`, `--start-paused`, plus the `Launch`-only `--after-scenario` shorthand for `after_scenario=`, which is not a `vehicle_sil_launch run` CLI flag) translated into the initial stdin `run` command, and host flags (`--control-credentials`, `--control-tls-cert`, `--control-tls-key`, `--control-audit-log`, `--log-dir`, `--timeout-ms`, `--describe-timeout-ms`, `--participant-start-timeout-ms`, `--participant-stop-timeout-ms`, `--events-stall-ms`) passed to `serve --once`. `--param <k>=<v>` is forwarded verbatim via `intent.cli_params` without client-side type inference (taking command-line precedence over typed `params`; duplicate `--param` keys raise `ValueError`). Combining `--request` with inline run options (either keyword arguments or per-run flags in `arguments`), or specifying `case` or `after_scenario` more than once, raises `ValueError`.

| Member | Details |
|---|---|
| `address` | The address of the Control Server. It waits for the `control_server` event, then returns. |
| `pid`, `participant_pids` | The launcher child PID (`int`), and a `dict[str, int]` mapping each started participant name to its PID from Events v4 `participant`/`started` events seen so far. |
| `events` | The list of the events read so far, as dicts (`bad_lines` holds malformed stdout lines; `stderr_text` holds stderr when `stderr=subprocess.PIPE`). |
| `result` | The result. |
| `wait_for_event(event, timeout=60.0, after=None, **keys)` | Waits for the first event whose keys have the given values. |
| `send(command)` | Sends one command line to stdin. |
| `stop()` | Sends `{"command":"stop"}`. Leaving `with` also stops the launcher if it is running. |
| `wait(timeout=120.0)` | Waits for the launcher to end and returns the result. |
| `close()` | Stops the launcher if running and closes pipes (`__exit__` calls this). |
| `returncode` | The exit code of the launcher. |

If the launcher ends without printing a result, or the run ends before the awaited event arrives, `LaunchError` is raised. Its message includes the result, error findings with their `detail` (such as `--describe` stderr, capped to 500 characters), and—on abnormal exit without a result—the command, exit code, and captured stderr tail.

If you start the launcher yourself, `control_address(lines)` returns `address` of the first `control_server` event in the event lines (strings or dicts).

### Run sequentially in one launcher (`Serve`, `RunHandle`)

`Serve` runs `vehicle_sil_launch serve <file>` (or `serve --once <file>` when `once=True`) as a child process speaking Events v4 (`vehicle_sil_events: 4`) for sequential runs on one configuration file (see "[Running sequentially in one launcher (`vehicle_sil_launch serve`)](simulation-run.md#running-sequentially-in-one-launcher-vehicle_sil_launch-serve)" and "[Resident launcher (`serve`)](simulation-events.md#resident-launcher-serve)"). Like `Launch`, `Serve` and `RunHandle` use only the standard library; gRPC is imported only when you call `run.control()`. See [examples/launch_control/serve_runs.py](../examples/launch_control/serve_runs.py) for a complete runnable script.

Unlike `Launch`, each `runner.start(...)` returns a `RunHandle` with its own run ID (`run.id`) and starts a fresh run at virtual time 0 with no state carried over from earlier runs. `run.control()` connects a `Control` already bound to `run.id`. Restrictions of multi-run `serve` apply (`VEHICLE_SIL_TRACE`, fixed `registry_uri`, and `manual` participants are refused unless `once=True`; see "[Restrictions of `serve`](simulation-run.md#restrictions-of-serve)").

```python
from vehicle_sil_control import Serve

with Serve("build/linux-gcc-release/tools/launch/vehicle_sil_launch", "simulation.yaml", control=True) as runner:
    prep = runner.prepare()
    with runner.start(start_paused=True, stop_at_ms=500) as run:
        with run.control() as c:   # bound to run.id; an earlier Control raises ControlError ("run-changed")
            c.sim.wait_paused()
            c.sim.resume()
        print(run.wait()["result"])
    with runner.start(scenario="scenarios/smoke.yaml") as run2:
        print(run2.wait()["result"])
```

In `Serve(launcher, config, *, once=False, control=False, address="0", credentials=None, tls_cert=None, tls_key=None, audit_log=None, log_dir=None, timeout_ms=None, describe_timeout_ms=None, participant_start_timeout_ms=None, participant_stop_timeout_ms=None, events_stall_ms=None, arguments=(), cwd=None, env=None, stdout=subprocess.PIPE, stderr=None, keep_logs=True, on_event=None, start_new_session=False, wait_ready=True, startup_timeout=30.0)`, `address`, `credentials`, `tls_cert`, `tls_key` and `audit_log` require `control=True`.

| `Serve` member | Details |
|---|---|
| `hello`, `instance`, `latest_status` | The initial `hello` event, its `launcher` instance ID, and the latest `ExecutionStatus` dict (updated from `status`, `prepare`, and `result` events; `None` before the first status). |
| `pid`, `returncode`, `stderr_text` | The child PID, exit code (`None` while running), and captured stderr tail (capped at 64 KiB) when `stderr=subprocess.PIPE`. |
| `prepare(*, variant=None, disable=(), enable=(), manual=(), id=None, timeout=30.0)` | Sends `prepare` and returns the `prepare` reply (`accepted`, `status`, `metadata`, `findings`). |
| `status(timeout=30.0, *, id=None)` | Sends `status` and returns the current `status` event. |
| `start(*, request=None, intent=None, ..., timeline=None, id=None, timeout=60.0)` | Starts one run and returns a `RunHandle`. Pass at most one of `request`, `intent`, or per-run keyword arguments (`variant`, `params`, `scenario`, `case`, `disable`, `enable`, `manual`, `connect`, `time_policy`, `speed`, `start_paused`, `strict`, `stop_at_ms`, `late_tolerance_ms`, `after_scenario`). An oversized inline intent automatically falls back to a temporary request file and cleans it up. |
| `wait(timeout=60.0)` | Waits for the launcher child process to exit and returns its exit code (unlike `RunHandle.wait()`). |
| `close_input()`, `close(timeout=60.0)` | `close_input()` closes stdin (EOF shutdown) without waiting; `close()` closes stdin, waits for the child process, joins reader threads, and removes any remaining temporary request files. |

| `RunHandle` member | Details |
|---|---|
| `id`, `run_id` | The opaque run ID assigned by the launcher. |
| `address` | Where this run's Control Server listens (waits for this run's `control_server` event). |
| `start_event`, `result_event`, `events`, `participant_pids`, `result`, `status`, `launcher_ends` | The `run_start` event, the `result` event once finished, events for this run, a `dict[str, int]` of participant name to PID from `participant`/`started` events of this run, the `result` dict and embedded settled `status` (`ExecutionStatus`), and whether the launcher ends with this run. |
| `wait_for_event(event, timeout=60.0, after=None, **keys)` | Waits for the first matching event of this run. |
| `control(**options)` | Returns `Control(run.address, run=run.id, **options)`. Passing `run=...` raises `TypeError`. |
| `send(command)` | Sends a stdin command targeted at this run (`"run": run.id`). |
| `stop(force=None, *, id=None)` | Sends `stop` for this run (`False` graceful, `True` forced, `None` default graceful). |
| `wait(timeout=120.0)` | Waits for this run's Events v4 `result` event and returns its inner `result` dict (while `result_event` holds the full event, which embeds the settled `status` with no separate `idle` event). |

`Serve` keeps no history of completed runs and does not accumulate a stream-wide `events` list. Once a run's `result` event arrives, `Serve` drops its reference to that `RunHandle`, so the run's events and result are freed as soon as you release the handle (`keep_logs=False` omits `"log"` events from `RunHandle.events`). To observe every event across the whole stream (including `"log"` events even when `keep_logs=False`), pass `on_event=callback`. `on_event` runs sequentially on the reader thread outside `Serve`'s lock and before `Serve` updates its state and wakes waiting callers (so `Serve()`, `prepare()`, `start()`, `run.wait()`, and `close()` return only after the corresponding event has been delivered). Each call receives a shallow copy (`dict`) of the event; treat nested values as read-only and do not call `close()`, `wait()`, or blocking methods/properties that wait for events (`prepare()`, `status()`, `start()`, `run.wait()`, `run.wait_for_event()`, or reading `hello`, `instance`, `run.address`, or `run.control()` before those events are processed) inside the callback—doing so raises `ServeError`. Exceptions raised by `on_event` are passed to `sys.excepthook` without stopping the reader thread.

Leaving `with runner.start(...)` stops only that run (`force=False`) and waits for its `result` (which carries the settled `status`), keeping `Serve` alive for the next run (unless `once=True` or `launcher_ends` is true). If a run ends with `launcher_ends: true` (for example exit code 6 when contaminated, or a single run with `once=True`), `Serve` cannot start further runs; leaving `with Serve(...)` closes the resident launcher. When a command is refused, times out, or the launcher fails to start or exits unexpectedly (including `VEHICLE_SIL_TRACE` without `once=True`, reported with `reason="trace-enabled"`), `ServeError` (a subclass of `LaunchError`) is raised with `reason` when available; its message includes finding `detail` (such as `--describe` stderr) and, on abnormal process exit, the command, exit code, and captured stderr tail.

### Connect, read and write (`Control`)

To connect from another computer to a launcher that uses TLS and tokens, pass `token` and `root_certificates` (or `tls=True` to use the system's root certificates without `root_certificates`; see [Authentication](#authentication) above). For how to make the certificates and what the messages mean when a connection fails, see [howto/multi-pc.md](howto/multi-pc.md).

```python
from vehicle_sil_control import Control, ms

with Control("ctrl-pc:8510", token=token, root_certificates=open("server.crt", "rb").read()) as c:
    print([p.participant for p in c.peers()])
    c.sim.wait_paused()
    seq = c.write({"Engine": {"engine.throttle": 50.0}, "Body": {"cabin.lamp": 2}}, at=ms(100))
    c.wait(seq, until="queued")         # advance only once every participant has it (at is after the paused time, so no write is too late)
    c.sim.run_until(ms(500)); c.sim.wait_paused()
    print(c.wait(seq).target("Engine").applied_at)   # 100 ms (in ns)
    print(c.values("Engine").values["engine.speed"])
    for update in c.watch(peers=["Body"]):           # Peer, Values, EventInfo, Operation
        print(update)
        break
```

Times are virtual time in nanoseconds. Make them with `ms()` and `s()`. Pass `late="apply_next"` to `c.write(..., at=..., late="apply_next")` to apply in the next step if `at` has already passed (default `"reject"`). Pass `timeout=<seconds>` to `c.watch(..., timeout=5.0)` to bound a watch stream in wall-clock seconds (`None` waits indefinitely; expiry raises `ControlError` with code `DEADLINE_EXCEEDED`).

Reading values (`c.values()`, `c.snapshot()`, `c.watch()`) returns `Values` objects whose `.values` dict maps variable names to their Python values (`bool`, `int`, `float`). Call `vals.text(name)` to resolve an enum variable's integer value to its declared item name (returns `None` if `name` is absent, not an enum, or its integer has no matching item). Pass `builtin=False` to `c.values(peer, builtin=False)` or `c.snapshot(peers=(), builtin=False)` to filter to application-declared variables only (`not v.builtin`). Note that `builtin=False` is an application-declared variables filter (it also drops deterministic built-in variables such as `ecu.connected`, `peer.steps`, and `j1939.*`); when comparing runs for determinism, keep `builtin=True` and exclude only the two wall-clock monitoring variables (`peer.speed` and `peer.app_time_max_us`).

| What happened | How it shows in Python |
|---|---|
| A call fails | The exception `ControlError` (see [Errors](#errors)). |
| The Agent rejects a write | Not an exception: the result of the operation (`invalid` and others). |

To keep a `Control` to one run (see [The run a call is for](#the-run-a-call-is-for)), bind it to the run. It then names the run in every call, and once the server at the address belongs to another run, every call but `ping()` and `current_run()` raises `ControlError` with the `reason` `run-changed`. Make a new `Control` for the new run. Against a multi-run `vehicle_sil_launch serve` launcher, an unbound `Control(address)` is refused with `reason` `run-required`, and the `ControlError` message includes a hint pointing to `Control.for_current_run()` / `Control(..., run=...)` (or use `run.control()` with `Serve`).

```python
from vehicle_sil_control import Control

with Control.for_current_run("127.0.0.1:8510", token=token) as c:   # the run the server belongs to now
    print(c.run)                                                     # its ID
    c.sim.stop()                                                     # stops that run, never a later one
```

| Member | Details |
|---|---|
| `Control(address, ..., run=<ID>)` | Names the run `<ID>` in every call (the metadata `vehicle-sil-run`). `run=None` (the default) names none. |
| `Control.for_current_run(address, ...)` | A `Control` bound to the run the server belongs to now (one `Ping`). It takes the arguments of `Control()` except `run`. A server that belongs to no run raises `ControlError` (`FAILED_PRECONDITION`). |
| `run` | The ID the `Control` is bound to, or `None`. |
| `current_run()` | The ID of the run the server belongs to now (`PingReply.run`; empty if none). Never rejected for the run: compare it with `run` to see whether the `Control` still reaches its run. |

Use the `vehicle_sil_control` of the same vehicle-sil release as the launcher. Making a `Control` does not connect. At its first call or watch, `Control` asks the server's release with one `Ping`. Another release is refused with `ControlError` (`code` `FAILED_PRECONDITION`): `the control server is vehicle-sil <X>; this vehicle_sil_control is <Y>: use the vehicle_sil_control of the same release as the launcher`. A `Ping` that fails (the server is not there yet) is tried again at the next call.

### Rules for writes

| Topic | Rule |
|---|---|
| Writing after a pause | Set `at` later than the time at which the simulation is stopped (`c.sim.time().now`), for example 1 ns later. |
| Resending | `write()` does not resend automatically. |
| Several clients write the same variable for the same time | The order in which the server receives the writes decides which value remains. |

- At the time at which the simulation is stopped, some peers have already run their step. A write for that time, or without `at`, can therefore be `late`, or the step that applies it can vary from run to run.
- The exception is when the launcher's `--start-paused` holds the simulation before the first step (see [Write before the start](#write-before-the-start---start-paused) above).
- If the connection drops before the response arrives, the write may still have been accepted. Sending the same write again makes a separate operation.

### Read the time state

| Goal | Use |
|---|---|
| Wait until the simulation stops | `c.sim.wait_paused()`. Its response gives the state in which the simulation stopped. |
| Read the time and the values of several peers as of the same instant | `c.snapshot(peers=())` (see [Read time and values together](#read-time-and-values-together-snapshot) below). |
| Subscribe to the time state | `watch(time=True)`. It gives the latest state about every 100 ms, so a short pause may not appear. |

- If you call `c.sim.time()` and `c.values()` separately, the simulation may advance between them. Nothing is guaranteed about how the two relate.
- The response (`TimeState`) of `step()`, `run_until()` and `pause()` with a time, and `time()` right after it, give the state at that moment, after the command was accepted.
- While the simulation is advancing, `paused` is false and `now` is the time before or during the advance. If the simulation is fast, it may already have reached the time and stopped.
- Where a `step()` ends is in its response: `pause_target` while the simulation is advancing, and `now` once it has stopped there (`paused` is true). If stdin or another client sends a time command at the same moment, the response may already show that command's effect; the launcher's `command` event of each step has the time of that step (`at_ms`).
- Calling `sim.pause(at=T)` while already paused after start updates `pause_target` without resuming, and `sim.resume()` clears any planned `pause_target` ("last call wins"). Use `sim.run_until(T)` to advance to `T` and pause whether currently paused or running.
- When `TimeState.running` is false, `ended` is true if the simulation has ended, and false if it has not started yet.

### Peers and variable information (`peers()`, `watch()`)

`peers()` and `watch()` return the same peers with the same contents.

| Peer field | Meaning |
|---|---|
| `has_agent` | Whether an Agent has ever connected (false by default). |
| `simulation_state` | The SIL Kit state. |
| `virtual_time` | Whether the Agent runs on the simulation's time, as the Agent's hello says. False means a real-time peer, or no Agent. |

| Variable field (each element of `schema`) | Meaning |
|---|---|
| `builtin` | Whether vehicle-sil added the variable (true) or the application declared it (false). |
| `source` | For a variable to which the variable map of the configuration file (`participants[].variables`) applies, its name in the executable. Empty if no mapping applies. |

- The server decides `builtin` by the name: true if the name starts with `ecu.`, `peer.`, `j1939.`, `can.`, `lin.` or `eth.`. If the application declares a variable whose name starts with `j1939.`, that variable is true as well.

With `PeerOptions::monitor` (enabled by default), each SIL Kit `Peer` adds the read-only monitoring variables below at runtime (`builtin: true`). They are available via the Control API and the browser GUI, not in `--describe` or scenarios:

| Variable | Type / Unit | Meaning |
|---|---|---|
| `peer.steps` | int | Steps run by the peer. |
| `peer.speed` | float (`s/s`) | Simulation seconds per wall-clock second since the previous sample (`0` at the first sample; includes paused time). |
| `peer.app_time_max_us` | float (`us`) | Longest application and LIN stage of a step since the previous sample (excludes J1939 handling and applying commands). |
| `j1939.<net>.bus_state` | enum | CAN controller error state (`unknown`, `error_active`, `error_passive`, `bus_off`). |
| `j1939.<net>.<counter>` | int | Network statistics counters: `frames_received`, `frames_sent`, `send_failures`, `send_queue_full`, `receive_overflows`, `held_dropped`, `address_violations`, `transport_aborts_sent`, `transport_aborts_received`, `transport_timeouts`, `transport_rejected`, `transport_bam_dropped`, `transport_bam_replaced`. |
| `j1939.<net>.node<i>.address` | int | Current address of J1939 node `i`. |
| `j1939.<net>.node<i>.address_state` | enum | Address claim state (`fixed`, `not_started`, `claiming`, `claimed`, `cannot_claim`). |

`<net>` uses the mapped simulation network name if `--map-network` is set (see "Peer monitoring" in [internals/control.md](internals/control.md#peer-monitoring) for details).

For a peer with a variable map, these are after mapping:

| Topic | Peer with a variable map |
|---|---|
| Names, types, ranges, units, read values and written values in `schema` | All after mapping. |
| `builtin` | Decided by the mapped name. |
| Reason text when a write is rejected | Uses the mapped name. |
| Text when the ECU side rejects a write | Has the form `engine.speed (the executable's rpm_x10): ...`, with the value as the executable has it. |
| A mapping that did not match a variable of the executable | Writes to the names of that mapping (the common name and the name in the executable) are rejected as `invalid`, and the reason says why the mapping did not match. The value can still be read under the executable's name. |

### Simulation state (`watch(simulation=True)`)

A subscription to the simulation state returns `SimulationState`.

| `SimulationState` field | Meaning |
|---|---|
| `participants` | The SIL Kit state of each required participant. |
| `failed_participant`, `failure_reason` | The participant that failed, and the reason. |
| `ended` | Whether the simulation has ended. |
| `error` | Why the server could not read the state. The state keeps its previous value. |

- If a client is slow and the server cannot keep all intermediate states, the number of dropped states goes into `Update.dropped` of gRPC. Python's `watch()` does not return this number. Failures and the end remain in later states, so they are never lost.
- While the server cannot read the state, `peers()` also reports it with a `ControlWarning` on every call, including the second and later calls from the same place. Your warning filters decide whether the warning is shown, raised or ignored; the library does not change the filters.

### Read time and values together (`snapshot()`)

The time of a peer's values (`Snapshot.time_ns`, `Values.time` of the Python client) is the start of the ECU step at whose end they were read, as in [the scenario's "Values that conditions see"](scenario.md#values-that-conditions-see).

```python
c.sim.step(ms(100)); c.sim.wait_paused()
snap = c.snapshot()                       # all peers; c.snapshot(["Engine", "Body"]) to narrow it down
if snap.consistent:                       # the values are those of the paused time snap.time.now
    print(snap.time.now, snap.values["Engine"].values["engine.speed"])
```

`snapshot(peers=(), builtin=True)` (gRPC `GetSnapshot`) returns the time state (the same `TimeState` as `sim.time()`) and the latest values of each peer (the same `Values` as `values()`, including `Values.text()` for enum names), which the server reads in one go. Pass `builtin=False` to include only application-declared variables (`not v.builtin`); for determinism comparisons across runs, keep `builtin=True` and exclude only the two wall-clock variables (`peer.speed` and `peer.app_time_max_us`). The server reads the time state first, then the values of all peers at the same instant.

The guarantees apply to peers that run on the simulation's time, while the server can control time (`time.available`).

| `consistent` | Guarantee |
|---|---|
| True or false (also while running) | The values are not older than the time: every value of each peer's steps that started before `time.now` is included. |
| True | Time is stopped at `time.now` (`time.paused`), and no value comes from a step after `time.now`. |
| False | The simulation is running, has ended, or advanced during the call so that a value from a later step got in. Values and time are present even when it is false. |

- The values can be newer than the time, by as much as the simulation advanced during the read.
- When `consistent` is true, the values are those of the time at which the simulation is stopped: from every step before `time.now`, and from none after it.
- If you call `snapshot()` after waiting for the stop (`sim.wait_paused()`), and nobody advances the simulation in between, `consistent` is true.
- Whether the values of a step that starts exactly at `time.now` are included is not defined, because they are received while the simulation is stopped. `Values.time` tells you which.
- So, right after a stop, the same time can give 2 different responses: before and after the values of the step at exactly that time arrive. `consistent` is true for both.

Assumptions and limits:

| Topic | Details |
|---|---|
| Assumption of the guarantees | The ECU sends its values from inside its step. ECUs built with `ecu_main` do. |
| If the assumption does not hold | The launcher prints the warning `control-link-late-values`, and values may be missing even when `consistent` is true. |
| "The values of a step" | The last sample read before that time. An Agent reads every `sample_period`, which can be longer than a step. |
| Values of real-time peers (`virtual_time` false in `peers()`) | No guarantee. `consistent` is decided without looking at them. |
| Disconnected peers | Their last values are included as they are. Check with `c.peers()` whether a peer is connected. |

Which peers `values` contains:

| Topic | Details |
|---|---|
| `values` in Python | Only peers that have values. Peers without values yet, participants without an Agent and unknown names are left out. |
| The gRPC response (`GetSnapshotReply.values`) when names are given | One entry per name, in the order given (a repeated name once). |
| The gRPC response when no names are given | The peers that have an Agent, in name order. |
| A peer without values | Has no `snapshot`. |
| A name without an Agent | Has no `incarnation` either. |
| Order of the values | The order of the schema of that `incarnation` (`ListPeers`). |

| Topic | Details |
|---|---|
| Role | `viewer` can call it. |
| Names per call | Up to 1024. More gives `INVALID_ARGUMENT`. |
| The response is larger than the limit of one message (4 MiB) | `RESOURCE_EXHAUSTED`. As the message says, split the peers over several calls. Nothing is guaranteed about how separate calls relate. |

## CLI

Pass `address` of the launcher's `control_server` event (in the human-readable output, the `control server at` line) to `--server` (default: `VEHICLE_SIL_CONTROL_SERVER`, fallback `127.0.0.1:8510`). `--token` (default: `VEHICLE_SIL_CONTROL_TOKEN`), `--root-cert` (default: `VEHICLE_SIL_CONTROL_ROOT_CERT`), and `--run <ID>` (default: `VEHICLE_SIL_CONTROL_RUN`) configure authentication, TLS, and the target run (see [The run a call is for](#the-run-a-call-is-for)); a launcher with `serve` requires `--run`. [List of calls](#list-of-calls) maps the commands to gRPC calls (`set` also accepts `--apply-late` and `--wait none|queued|done`; `pause` accepts `--at-ms` and `--wait`; `watch` accepts `--no-values`; `stop` accepts `--reason`).

```sh
export VEHICLE_SIL_CONTROL_SERVER=ctrl-pc:8510 VEHICLE_SIL_CONTROL_TOKEN=... VEHICLE_SIL_CONTROL_ROOT_CERT=server.crt
python -m vehicle_sil_control peers
python -m vehicle_sil_control show Engine
python -m vehicle_sil_control set Engine engine.throttle=50 --at-ms 100   # by default waits until every participant has queued it (--wait done: until applied)
python -m vehicle_sil_control run-until 500
python -m vehicle_sil_control step 10
python -m vehicle_sil_control speed 1        # real time; max for as fast as possible
python -m vehicle_sil_control watch --peer Body --count 10
python -m vehicle_sil_control stop
```

## Errors

A failed call returns a gRPC status code. In Python, it raises the exception `ControlError`.

| `ControlError` member | Details |
|---|---|
| `code` | The gRPC status code. |
| `reason` | The word the server gave when it rejected the call with a reason (`vehicle-sil-reason` in the trailing metadata): `run-changed`, `run-required`, `scenario-running`, `outside-real-time`, and the other reasons of a time command that is not possible now. Empty otherwise. |

| Status code | Returned when |
|---|---|
| `INVALID_ARGUMENT` | An argument is wrong (a speed out of range, a step of 0 or less, more than 1024 names for `snapshot()`). |
| `FAILED_PRECONDITION` | The call is not possible now (`step` while not paused, `Stop` after the end, writes during a scenario before handover, a pause or another speed in a run that is connected outside and keeps real time), or it is for another run than the server's (`run-changed`) or names no run where the server requires one (`run-required`). The Python client also raises it for a server of another vehicle-sil release. |
| `UNAUTHENTICATED` / `PERMISSION_DENIED` | Missing/invalid bearer token (`UNAUTHENTICATED`) or insufficient role (`PERMISSION_DENIED`) when `--control-credentials` is enabled. |
| `CANCELLED` | The server closed while the call was waiting (`WaitPaused`; it can also get `FAILED_PRECONDITION`). |
| `DEADLINE_EXCEEDED` | The call's wall-clock deadline expired (for example `Control.watch(..., timeout=...)` or a slow RPC). |
| `RESOURCE_EXHAUSTED` | The response of `snapshot()` was larger than the limit of one message (4 MiB). |
| `UNIMPLEMENTED` | The address answers gRPC but is not a vehicle-sil Control Server (it has no such call). |
| `UNAVAILABLE` / `NOT_FOUND` | The server is unreachable or a peer keeps restarting during `values()` / `snapshot()` (`UNAVAILABLE`); `peer(name)` found no peer of that name (`NOT_FOUND`). |

Writes during a scenario (before an `after_scenario: "pause"` handover) have the `reason` `scenario-running`.

| Python exception or warning | Raised when |
|---|---|
| `ControlError` | A call failed (`code` and `reason` describe the failure). |
| `LaunchError` | The launcher ended without printing a result, or the run ended before the awaited event arrived. |
| `ServeError` | Subclass of `LaunchError`; a resident `serve` command (`prepare`, `start`, `status`) was refused or timed out, or the resident launcher failed at startup or ended unexpectedly (`reason` holds the refusal/failure code when available, such as `"trace-enabled"` or `"busy"`). |
| `ControlWarning` | `peers()` was called while the server cannot read the simulation state. |

## Limitations

| Topic | Limitation |
|---|---|
| Peers that time control (pause, step, speed) stops | Only peers synchronized in virtual time. Real-time peers keep running. |
| Lateness at a set speed | If the simulation falls behind real time, it does not try to catch up later. |
| Operations that do not exist | Reset, starting and restarting participants, and selecting CAN or LIN channels. |
| Runs the server covers | One run only. The server closes when the run ends. With `serve`, each run has a server of its own. |
| Value subscriptions | Latest value only. A newer value that arrives before the client reads replaces the older one. |

- When a run at a set speed falls behind, it keeps going at the set speed, shifted back by the lateness (see "How lateness is measured" in [internals/virtual-time.md](internals/virtual-time.md#how-lateness-is-measured)).
- Read participant failures from the simulation state (`failed_participant` and `failure_reason` of `watch(simulation=True)`).
- To record every raw CAN/J1939 frame without missing intermediate frames, use `spy::Scope::on_frame` or `ecu.can().on_frame()` in an `ecu_main` ECU (or `Peer::tap()` / `TapPort` when driving a `Peer` or `LocalBench` directly in-process; `TapPort` is not exposed on `Ecu`). Do not rely on SIL Kit `TraceSinks` for CAN: the bundled `PcapFile`/`PcapPipe` sinks are Ethernet-only and throw on CAN frames, and `Mdf4File` requires Vector's external `SilKitExtension_Mdf` extension, which is not bundled with vehicle-sil.

| Limit (server default) | Value |
|---|---|
| Writes in one `write()` | Up to 4096 |
| Peer names in one `snapshot()` | Up to 1024 |
| Waiting calls and subscriptions at the same time | Up to 256 in total |

## See also

| To find | See |
|---|---|
| Reading and writing values, and pausing and stepping time, step by step | [Read and write signals from outside](howto/operate-signals.md) |
| Connecting from another computer, step by step (including making certificates) | [Run on several computers](howto/multi-pc.md) |
| Details of the guarantees (when writes are applied, ordering, limits, threads) | [internals/control.md](internals/control.md) |
| Events and stdin commands | [simulation-events.md](simulation-events.md) |
| What clients can do during a scenario | "Watching variables during a run (`--control`)" in [scenario.md](scenario.md#watching-variables-during-a-run---control) |
| Viewing and changing values in a browser | [gui.md](gui.md) |
