# Read and write signals from outside

[日本語](../ja/howto/operate-signals.md)

In this guide you read and write the variables of the ECUs in a running simulation from Python or from the command line, and you pause virtual time and advance it in small steps. You connect over gRPC to the Control Server that `vehicle_sil_launch run` starts when you add `--control`.

## Choose a method

There are 4 ways to read and write variables from outside the simulation. This guide covers the second and the third.

| Goal | Use | How to start | See |
|---|---|---|---|
| Set values in a fixed sequence, check them and get the result as an exit code | Scenario (YAML) | `vehicle_sil_launch run simulation.yaml --scenario <scenario>` | [Write tests with scenarios](scenario-tests.md) |
| Look at values and decide in a program what to write next | Python client (`vehicle_sil_control`) | `Launch(...)` in the script starts `vehicle_sil_launch run --control` | This guide, from "[Run the first script](#run-the-first-script)" on |
| Read, write and advance one command at a time, without writing Python | CLI (`python -m vehicle_sil_control`) | You start `vehicle_sil_launch run --control 8510 simulation.yaml` yourself | "[Run commands one at a time](#run-commands-one-at-a-time)" in this guide |
| Watch values in a browser and write them with sliders and switches | GUI (`vehicle-sil-gui`) | `scripts/run-gui.py` (with the prebuilt tools, `vehicle-sil-gui simulation.live.yaml`) | "ECU variables" in [gui.md](../gui.md#ecu-variables) |

A scenario always gives the same result with the same configuration file and the same scenario. With Python and the CLI, the result can depend on the order in which the writes arrive and on the times you give. To keep what you did as a test, write the same steps in a scenario.

If you only need to pause and resume virtual time, you can also use the stdin of `vehicle_sil_launch run` (see "[Control time from stdin](#control-time-from-stdin)").

## Prerequisites

| Requirement | Details |
|---|---|
| vehicle-sil build | You have built with the `linux-gcc-control` preset, as described in "[Build from the repository](../../README.md#build-from-the-repository)" in the README. |
| [uv](https://docs.astral.sh/uv/) | Runs the Python client and the CLI. |

Run every command in the root of the repository (the directory that contains `README.md`). The guide uses these 3 paths:

| Item | Path |
|---|---|
| `vehicle_sil_launch` | `build/linux-gcc-control/tools/launch/vehicle_sil_launch` |
| Configuration file | `build/linux-gcc-control/examples/ecu_main/simulation.yaml` |
| Python client | `python/` (package `vehicle_sil_control`) |

With the template project and the prebuilt tools, use the right-hand column of the table below instead. `$T` in the table is the tools directory, `tools/vehicle-sil-tools-<version>-linux-x86_64`. The 2 ECUs of the template have the same variables as the example in this guide.

| Item | Built from the repository | Template project |
|---|---|---|
| Directory to run in | The root of the repository | The project directory (the one that contains `simulation.yaml`) |
| `vehicle_sil_launch` | `build/linux-gcc-control/tools/launch/vehicle_sil_launch` | `$T/bin/vehicle_sil_launch` |
| Configuration file | `build/linux-gcc-control/examples/ecu_main/simulation.yaml` | `simulation.yaml` |
| Setup | The 3 lines in "Setup" below | Not needed. |
| Running Python | `uv run python <arguments>` | `PYTHONPATH=$T/python uv run --no-project --no-build --python '>=3.11,<3.15' --with-requirements $T/python/requirements.txt python <arguments>` |
| Example scripts | `examples/launch_control/` | Not included. Save the full text in "Run the first script" to a file. |

## Setup

Install the Python packages and generate the gRPC stubs.

```sh
uv sync                                          # installs grpcio and others into .venv (changes nothing if you already ran it as the README says)
export PYTHONPATH="$PWD/python"                  # where the client package is; run this in every new shell
uv run python -m vehicle_sil_control.generate    # generates the stubs in python/vehicle_sil_control/_generated (the first time only)
```

### Verify the setup

`generate` prints one line with the directory it wrote the stubs to:

```text
generated into <repository root>/python/vehicle_sil_control/_generated
```

To confirm that `vehicle_sil_launch` has the Control Server, run the command below. If it prints `control-server`, the Control Server is built in.

```sh
build/linux-gcc-control/tools/launch/vehicle_sil_launch version --json | grep control-server
```

```text
    "control-server",
```

### Troubleshoot the setup

| Symptom | Cause | Fix |
|---|---|---|
| `grep` prints nothing | This `vehicle_sil_launch` was built without the Control Server. | Build with the `linux-gcc-control` preset (with another preset, add `-DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON` to the configure step). |
| `No module named vehicle_sil_control` | `PYTHONPATH` is not set. | Run `export PYTHONPATH="$PWD/python"` in the root of the repository. |
| `No module named 'grpc_tools'` | The packages are not in `.venv`. | Run `uv sync`. |

## The example variables

The example configuration file runs the engine ECU (participant name `Engine`) and the dashboard ECU (`Dashboard`). The guide reads and writes these 3 variables:

| Participant | Variable | Input or output | Type and unit | Description |
|---|---|---|---|---|
| `Engine` | `engine.throttle` | Input (writable from outside) | real, %, 0–100 | The throttle. If you do not write it, it is 20. |
| `Engine` | `engine.speed` | Output (read only) | integer, rpm | The engine speed. It starts at 0 and, in each 10 ms ECU step, moves 50 rpm toward 800 + 20 × throttle (rpm). |
| `Dashboard` | `dashboard.engine_speed` | Output (read only) | integer, rpm | The engine speed that `Engine` sends over J1939 every 100 ms, as the dashboard ECU received it. |

With the throttle at 20 %, the engine speed settles at 1200 rpm; at 50 %, at 1800 rpm; at 0 %, at 800 rpm.

To find the names, types and ranges of your own ECU's variables, use `show` (see "Run commands one at a time") or the `--describe` option of the ECU executable (the `variables` array). You do not add anything to the ECU code: any variable that an ECU built with `ecu_main()` declares with `ecu.input()` or `ecu.output()` can be read and written from outside as it is.

## Run the first script

This script starts the simulation paused at time 0, writes 50 % to the throttle, advances 500 ms, then reads the engine speed. Here is the full text of [examples/launch_control/write_and_read.py](../../examples/launch_control/write_and_read.py), except line 1 (`# SPDX-License-Identifier: MIT`).

<!-- examples/launch_control/write_and_read.py -->
```python
import sys

from vehicle_sil_control import Control, Launch, ms

launcher, config = sys.argv[1], sys.argv[2]                   # vehicle_sil_launch and simulation.yaml

# Starts "vehicle_sil_launch run" with its control server, paused before the first step.
with Launch(launcher, config, start_paused=True) as launch:
    with Control(launch.address) as c:
        c.sim.wait_paused()                                   # every ECU stands still at 0 ms
        seq = c.write({"Engine": {"engine.throttle": 50.0}})  # participant name, then variable name
        c.wait(seq, until="queued")                           # the ECU has received the write

        c.sim.step(ms(500))                                   # run 500 ms, then pause again
        now = c.sim.wait_paused().now
        values = c.values("Engine").values                    # the values the ECU sent last
        print(f"at {now / 1e6:g} ms: engine.throttle = {values['engine.throttle']}, "
              f"engine.speed = {values['engine.speed']}")
```

```sh
uv run python examples/launch_control/write_and_read.py \
    build/linux-gcc-control/tools/launch/vehicle_sil_launch \
    build/linux-gcc-control/examples/ecu_main/simulation.yaml
```

### Verify the first script

<!-- examples/launch_control/write_and_read.py: the output -->
```text
at 500 ms: engine.throttle = 50.0, engine.speed = 1800
```

The engine speed rises from 0 by 50 rpm every 10 ms and settles at 1800 rpm, the value for 50 % throttle. If you change `50.0` to `0.0`, the script prints `engine.speed = 800`. If you delete the 2 lines with `c.write()` and `c.wait()`, it prints `engine.throttle = 20.0, engine.speed = 1200`.

What each line of the script does:

| Line | What it does |
|---|---|
| `Launch(..., start_paused=True)` | Starts `vehicle_sil_launch run --events --stdin-commands --control 0 --start-paused` as a child process (`--control 0` picks a free port). |
| `launch.address` | The address of the Control Server, as `vehicle_sil_launch` reported it. |
| `Control(launch.address)` | Connects to the Control Server over gRPC, and closes the connection when the `with` block ends. |
| `c.sim.wait_paused()` | Waits until the simulation is paused, and returns the time state. |
| `c.write({"Engine": {"engine.throttle": 50.0}})` | Writes a value, naming the participant and the variable, and returns the operation's sequence number. |
| `c.wait(seq, until="queued")` | Waits until the ECU has received the write. |
| `c.sim.step(ms(500))` | Advances virtual time by 500 ms, then pauses again. |
| `c.values("Engine").values` | Returns the values that the ECU sent last, as a dict keyed by variable name. |

When the script leaves the outer `with` block, it stops the simulation and ends.

### Troubleshoot the first script

| Symptom | Cause | Fix |
|---|---|---|
| `ModuleNotFoundError: No module named 'vehicle_sil_control'` | `PYTHONPATH` is not set. | Run the `export` line in "Setup". |
| `ImportError: the Control API stubs are missing in ...` | The stubs have not been generated. | Run the `generate` line in "Setup". |
| `LaunchError: ... --control needs a vehicle_sil_launch built with the control server` | You passed a `vehicle_sil_launch` that was built without the Control Server. | Pass the one under `build/linux-gcc-control/`. |
| `LaunchError: ... the executable engine/vehicle_sil_engine_ecu does not exist` | You passed `examples/ecu_main/simulation.yaml` (the one in the source tree). | Pass `build/linux-gcc-control/examples/ecu_main/simulation.yaml`. |
| `AttributeError: 'NoneType' object has no attribute 'values'` | There are no values until the simulation has advanced at least one step, so `c.values()` returns `None`. | Advance with `c.sim.step()` before you read. |

## Run commands one at a time

Control the simulation from a terminal one command at a time, without writing Python. Use 2 terminals, both in the root of the repository.

In terminal 1, start the simulation paused at time 0, with the Control Server on port 8510:

```sh
build/linux-gcc-control/tools/launch/vehicle_sil_launch run --start-paused --control 8510 \
    build/linux-gcc-control/examples/ecu_main/simulation.yaml
```

When these 2 lines appear, you can connect. Lines about starting the registry and the ECUs appear between them.

```text
[vehicle_sil_launch] control server at 127.0.0.1:8510
[vehicle_sil_launch] standing still before the first step until a resume, step or run_until command (--start-paused with a control server)
```

With only one terminal (in a script or in CI), add `> launch.log 2>&1 &` to the end of the terminal 1 command to start it in the background. When the 2 lines above appear in `launch.log`, run the terminal 2 commands in the same terminal. To stop partway, use the CLI command `stop` or `kill <PID>` (SIGTERM). The simulation and the ECUs stop (result `interrupted`, exit code 130).

In terminal 2, run `export PYTHONPATH="$PWD/python"`, then run the commands below in order. Unless you give another address, the CLI connects to `127.0.0.1:8510`.

Take your time: terminal 1 stays paused at time 0 until you run `step` or `resume`, and after each `step` it pauses again until the next command.

| What you do | Command |
|---|---|
| List the participants | `uv run python -m vehicle_sil_control peers` |
| Show the time | `uv run python -m vehicle_sil_control time` |
| Write a value | `uv run python -m vehicle_sil_control set Engine engine.throttle=50` |
| Advance 500 ms and pause | `uv run python -m vehicle_sil_control step 500` |
| Show the variables and their values | `uv run python -m vehicle_sil_control show Engine` |
| Write a value out of range and see the result | `uv run python -m vehicle_sil_control set Engine engine.throttle=150 --wait done` |
| Resume | `uv run python -m vehicle_sil_control resume` |

### Verify the commands

The output of each command follows. The number after `seq` changes with every run.

<!-- vehicle_sil_control peers -->
```text
Dashboard  connected  22 variables  SIL Kit: ReadyToRun
Engine     connected  22 variables  SIL Kit: ReadyToRun
```

<!-- vehicle_sil_control time -->
```text
paused at 0 ms, speed max
```

<!-- vehicle_sil_control set Engine engine.throttle=50 -->
```text
seq 1794666759502163
seq 1794666759502163 Engine: queued
```

<!-- vehicle_sil_control step 500 -->
```text
paused at 500 ms, speed max
```

Here are the first 3 lines of `show Engine`. The columns are name, value, unit, and then input or output with type and range. The lines that follow are the variables that vehicle-sil adds automatically (names that start with `peer.` and `j1939.`). If you run `show` before the first `step`, the value column is empty, because the ECUs have not run a simulation step yet.

<!-- vehicle_sil_control show Engine: the first lines -->
```text
ecu.connected    True         input bool
engine.throttle  50.0  %      input real [0..100]
engine.speed     1800  rpm    output integer
```

A value out of range does not cause an error. The result is `invalid`.

<!-- vehicle_sil_control set Engine engine.throttle=150 --wait done -->
```text
seq 1794666759502164
seq 1794666759502164 Engine: invalid (engine.throttle: 150 is out of range)
```

<!-- vehicle_sil_control resume -->
```text
running at 500 ms, speed max
```

After `resume`, the simulation in terminal 1 runs to `stop_at_ms` (1000 ms) of the configuration file, prints the result table and exits with code 0.

Other CLI commands:

| What you do | Command (after `uv run python -m vehicle_sil_control`) |
|---|---|
| Write at a given time | `set Engine engine.throttle=50 --at-ms 600` |
| Pause | `pause` (with `--wait`, waits until the simulation is paused) |
| Advance to a given time and pause | `run-until 700` |
| Change the speed | `speed 1` (real-time speed), `speed max` (as fast as possible) |
| Print values each time they change | `watch --peer Engine --count 10` |
| Stop the simulation | `stop` |
| Connect to a Control Server at another address | Put `--server <address>` before the command name, or set the address in the environment variable `VEHICLE_SIL_CONTROL_SERVER` |

With `--control 0`, the launcher picks a free port. In that case, pass the address on the `control server at` line of terminal 1 to `--server`.

### Troubleshoot the commands

| Symptom | Cause | Fix |
|---|---|---|
| Terminal 1 prints `--control needs a vehicle_sil_launch built with the control server` and exits with code 4 | This `vehicle_sil_launch` was built without the Control Server. | Use the one under `build/linux-gcc-control/`. |
| Terminal 1 starts nothing and exits with code 3 | Another process uses port 8510. | Start with `--control 0`, and pass the printed address to the `--server` option of the CLI. |
| Terminal 2 shows `error: UNAVAILABLE: ... Connection refused` | The simulation in terminal 1 has ended, or the port is wrong. | Run the command in terminal 1 again, and check the address on the `control server at` line. |
| `step` fails with `FAILED_PRECONDITION` | The simulation is not paused. | Run `pause --wait` first. |
| `watch --count` does not end | While the simulation is paused, no new values are sent. | Advance with `resume` or `step`, or stop it with Ctrl+C. |

## Run the Python examples

The sections from here on explain the calls of the Python client, one task per section. The code is taken from the 2 examples in [examples/launch_control](../../examples/launch_control). Both examples check their own results and exit with code 0 if everything is as expected, 1 if not.

| Example | What it does |
|---|---|
| [write_before_start.py](../../examples/launch_control/write_before_start.py) | Writes before the first step, advances 500 ms, checks the result of the write and the values, then runs to the end. |
| [pause_and_step.py](../../examples/launch_control/pause_and_step.py) | Runs at a quarter of real-time speed, pauses when the engine speed reaches 1000 rpm, writes at a given time, advances 100 ms and checks the result. |

Unlike the first script, these examples take the launcher with `--launch`:

```sh
uv run python examples/launch_control/write_before_start.py \
    --launch build/linux-gcc-control/tools/launch/vehicle_sil_launch \
    build/linux-gcc-control/examples/ecu_main/simulation.yaml
uv run python examples/launch_control/pause_and_step.py \
    --launch build/linux-gcc-control/tools/launch/vehicle_sil_launch \
    build/linux-gcc-control/examples/ecu_main/simulation.yaml
```

### Verify the Python examples

`write_before_start.py` prints the following. The port number changes with every run.

<!-- examples/launch_control/write_before_start.py: the output -->
```text
control server at 127.0.0.1:45287
ok   it stands still at 0 before the first step
ok   Engine queued the throttle (queued)
ok   it stands still at 500 ms (500 ms)
ok   the throttle took effect in the step at 0 (applied at 0)
ok   engine.throttle reads 50.0
ok   engine.speed reads 1800 rpm (expected 1800)
ok   the launcher wrote the step and the resume as command events from the control server
ok   the run passed (passed, reached_stop_at)
all as expected
```

`pause_and_step.py` prints the following. It pauses a run that follows real time, so the times and the engine speeds can differ from run to run.

<!-- examples/launch_control/pause_and_step.py: the output -->
```text
engine.speed 1000 rpm at 190 ms
ok   it stands still at 190 ms
ok   it stands still at 290 ms after the step
ok   the throttle took effect in the engine's step at 200 ms (applied at 200000000)
ok   the snapshot is of 290 ms (consistent: True)
ok   engine.throttle reads 50.0 in the sample of 290 ms; engine.speed 1500 rpm
ok   the speed is max, and it still stands still
ok   the launcher wrote the commands of the control server as events (resume, pause, step, speed, resume)
ok   the run passed (passed, reached_stop_at)
all as expected
```

In the excerpts below, `expect(condition, text)` is a function that the examples define. If the condition is true, it prints `ok` and the text; if not, `FAIL` and the text. `c` is the `Control` object and `launch` the `Launch` object. Every call takes times in nanoseconds of virtual time; make them with `ms(500)` or `s(1)`.

## Read values

| Call | Returns | Use it to |
|---|---|---|
| `c.values("Engine")` | The values that the ECU sent last (`.values` is a dict, `.time` the time of those values) | Read the current values of one ECU |
| `c.snapshot(["Engine", "Dashboard"])` | The time and the values of several ECUs, read by the Control Server in one call | Read the time and the values as of the same instant |
| `c.watch(peers=["Engine"])` | An iterator that returns updates, such as values, in the order they are received | Wait for a value to change (see "[Wait for a value to change](#wait-for-a-value-to-change)") |
| `c.peers()` | The participants, with the names, types and ranges of their variables | Look up variable names |

`pause_and_step.py` pauses, then reads the time and the values with `snapshot()`.

<!-- examples/launch_control/pause_and_step.py: an excerpt -->
```python
snap = c.snapshot(["Engine"])
engine = snap.values["Engine"]
expect(snap.consistent and snap.time.now == stands.now,
       f"the snapshot is of {snap.time.now / 1e6:g} ms (consistent: {snap.consistent})")
```

The output line `ok   the snapshot is of 290 ms (consistent: True)` is the result of this check.

4 rules apply when you read:

- An ECU sends its values every 10 ms (the default `sample_period`), so you cannot see changes faster than that.
- If you call `c.sim.time()` and `c.values()` separately, the simulation can advance between the calls. To read a time and values that belong together, use `snapshot()`.
- If you call `snapshot()` after the simulation has paused and `consistent` is true, the values are those at the time at which the simulation is paused.
- Besides the variables you declared, the dict contains the variables that vehicle-sil adds automatically (`ecu.connected`, and names that start with `peer.` and `j1939.`).

For what `snapshot()` guarantees and its limits, see "Read time and values together (`snapshot()`)" in [control-api.md](../control-api.md#read-time-and-values-together-snapshot).

## Write values

Pass `c.write()` the variable names and values for each participant. One call can write to several participants. It returns the operation's sequence number, which you use to wait for the result (see "[Wait for the result of a write](#wait-for-the-result-of-a-write)").

The time at which you write decides the ECU step in which the value is applied:

| When you write | How | Step in which the value is applied |
|---|---|---|
| Before the first step (started with `start_paused=True`, while paused) | `c.write({...})` | The step at time 0. |
| While the simulation runs | Pause first, then call `c.write({...}, at=...)` with an `at` later than the time at which the simulation is paused | The first step of that ECU that starts at or after `at`. |

The code that writes before the first step is in "Run the first script". The code that writes while the simulation runs is this part of `pause_and_step.py`. `paused_at` is the time at which the simulation is paused.

<!-- examples/launch_control/pause_and_step.py: an excerpt -->
```python
at = paused_at + ms(1)
seq = c.write({"Engine": {"engine.throttle": THROTTLE}}, at=at)
c.wait(seq, until="queued")
c.sim.step(ms(100))
stands = c.sim.wait_paused()
```

The simulation paused at 190 ms, so `at` is 191 ms. The engine ECU steps every 10 ms, so the value is applied in the step at 200 ms. The output line `ok   the throttle took effect in the engine's step at 200 ms` is the result of that check.

5 rules apply when you write:

- In both cases, wait with `c.wait(seq, until="queued")` until the ECU has received the write, then advance time.
- While the simulation runs, if you give no `at`, or give the time at which the simulation is paused, the result can be `late`, or the step in which the value is applied can differ from run to run.
- A value out of range and a write to an output variable do not raise an exception. The result is `invalid`.
- A parameter (a variable that `show` lists as `parameter` instead of `input`) can be written only before the first simulation step.
- If several clients write the same variable with the same time, the value that remains depends on the order in which the Control Server received the writes.

Disconnecting an ECU from the bus is also a write to a variable: write `False` to `ecu.connected`, which every ECU has. For how this looks on J1939, see "Disconnect an ECU from the bus (`ecu.connected`)" in [j1939-guide.md](../j1939-guide.md#disconnect-an-ecu-from-the-bus-ecuconnected).

For all the rules on when a write is applied, see "Write before the start (`--start-paused`)" and "Python" in [control-api.md](../control-api.md).

## Wait for the result of a write

| Call | Waits until |
|---|---|
| `c.wait(seq, until="queued")` | Every destination ECU has received the write. |
| `c.wait(seq)` | The result is decided in every destination ECU. |

Both return the result of the operation. Read the result for each destination with `.target("Engine")`. `write_before_start.py` checks the result after it has advanced 500 ms.

<!-- examples/launch_control/write_before_start.py: an excerpt -->
```python
done = c.wait(seq)
expect(done.target("Engine").status == "applied" and done.target("Engine").applied_at == 0,
       f"the throttle took effect in the step at 0 ({done.target('Engine').status} at "
       f"{done.target('Engine').applied_at})")
```

The output line `ok   the throttle took effect in the step at 0 (applied at 0)` is the result of this check.

The 6 main values of `status`:

| `status` | Meaning |
|---|---|
| `queued` | The ECU has received the write. |
| `applied` | The write was applied (`applied_at` is the time of the step in which it was applied). |
| `late` | The write missed the step at the time you gave. |
| `invalid` | The checks of name, type, range and writability rejected the write. A write to a destination that is not connected also gets this value. |
| `rejected` | The ECU's setter rejected the write. The reason starts with the name of that variable. In a write of several variables, the variables before it are already applied (see "When a write is applied" in [control-api.md](../control-api.md#when-a-write-is-applied)). |
| `unknown` | It is not known whether the write was applied. |

3 rules apply when you wait:

- A rejected write does not raise an exception, so check `status`. An exception (`ControlError`) is raised only when the call itself fails.
- For a write that the ECU has received, `c.wait(seq)` does not return until time advances to when the value is applied. If you call it while the simulation is paused, it raises `ControlError` when it times out (after 30 seconds by default).
- `write()` does not retry on its own. A write whose connection broke before the response arrived may still have been accepted.

For the definition of each `status` value, see "Control Agent (`vehicle_sil::control`)" and "Control Server core (`vehicle_sil::control::Hub`)" in [internals/control.md](../internals/control.md).

## Wait for a value to change

`c.watch()` returns updates, such as values, in the order they are received. `pause_and_step.py` watches the engine speed of the running simulation and pauses when it reaches 1000 rpm.

<!-- examples/launch_control/pause_and_step.py: an excerpt -->
```python
for update in c.watch(peers=["Engine"], events=False, operations=False):
    if isinstance(update, Values) and update.values.get("engine.speed", 0) >= 1000:
        print(f"engine.speed {update.values['engine.speed']} rpm at {update.time / 1e6:g} ms")
        break
else:
    print("FAIL the simulation ended before the engine passed 1000 rpm")
    return 1
c.sim.pause()
stands = c.sim.wait_paused()
paused_at = stands.now
```

This code prints the first 2 lines of the output (`engine.speed 1000 rpm at 190 ms` and `ok   it stands still at 190 ms`). Import `Values` with `from vehicle_sil_control import Values`.

- `watch()` returns participant information, values, events and operation results.
- Values come as `Values` objects: `.values` is a dict, `.time` the time of those values.
- You receive the latest values, not a record of every value.
- When the simulation ends, the `for` loop ends too.
- At the fastest speed (the default), the simulation can end before you pause it. To pause partway, lower the speed (in the example, `Launch(..., arguments=["--speed", "0.25"])`), or start paused at time 0 and advance with `c.sim.run_until()`.

For the arguments of `watch()` and a list of what it returns, see "Python" in [control-api.md](../control-api.md#python).

## Pause and step time

The time operations are in `c.sim`.

| What you do | Python | CLI |
|---|---|---|
| Read the current time state | `c.sim.time()` | `time` |
| Pause | `c.sim.pause()` | `pause` |
| Resume | `c.sim.resume()` | `resume` |
| Advance by a given amount and pause | `c.sim.step(ms(100))` | `step 100` |
| Advance to a given time and pause | `c.sim.run_until(ms(700))` | `run-until 700` |
| Wait until the simulation is paused | `c.sim.wait_paused()` | `pause --wait` |
| Change the speed | `c.sim.set_speed(1.0)`; as fast as possible, `c.sim.set_speed(None)` | `speed 1`, `speed max` |
| Stop the simulation | `c.sim.stop()` | `stop` |

Every call returns the time state: `now` is the time, `paused` whether the simulation is paused and `speed` the speed. After its checks, `pause_and_step.py` sets the speed to as fast as possible, resumes and runs to the end.

<!-- examples/launch_control/pause_and_step.py: an excerpt -->
```python
speed = c.sim.set_speed(None)
expect(speed.speed is None and speed.paused, "the speed is max, and it still stands still")
c.sim.resume()
```

6 rules apply when you control time:

- After `step()` and `run_until()`, wait with `c.sim.wait_paused()` until the simulation is paused, then read values.
- `step()` works only while the simulation is paused.
- The time at which `pause()` pauses and the length that `step()` advances are rounded up to a multiple of the time step of `vehicle_sil_launch` (10 ms by default). Because of this, read the time at which the simulation paused from the `now` that `wait_paused()` returns.
- If you change the speed while the simulation is paused, it stays paused.
- An operation that is not possible at the moment raises `ControlError` (with `code` `FAILED_PRECONDITION`); an argument error gives `INVALID_ARGUMENT`.
- Only ECUs that run in virtual time pause.

For the speed and how to read the lateness when you give `--speed`, see "Run at real-time speed (`--speed`) and read the lateness" in [Integrate and run several ECUs](integrate-ecus.md#run-at-real-time-speed---speed-and-read-the-lateness).

## Wait for the run to end

`launch.wait()` waits until `vehicle_sil_launch` ends, and returns the same result as `run --json`, as a dict. `write_before_start.py` resumes, then checks the result.

<!-- examples/launch_control/write_before_start.py: an excerpt -->
```python
expect(result["result"] == "passed" and result["simulation"] == "reached_stop_at",
       f"the run passed ({result['result']}, {result['simulation']})")
```

- To stop partway, call `launch.stop()`.
- Leaving `with Launch(...)` also stops the run if it is still running.
- When the run ends, `vehicle_sil_launch` closes the Control Server, and calls that are waiting (`watch()`, `wait()`, `wait_paused()`) end there.
- Pass other options of `run` (`--speed`, `--disable` and others) with `Launch(..., arguments=[...])`.

For how to read the result dict, see "JSON (`run --json`)" in [simulation-run.md](../simulation-run.md#json-run---json).

## Control time from stdin

To control only virtual time, you can use the stdin of `vehicle_sil_launch run`, with neither the Control Server nor Python. You cannot read or write variables this way. Start the launcher with `--stdin-commands` and type one JSON object per line.

```sh
build/linux-gcc-control/tools/launch/vehicle_sil_launch run --start-paused --stdin-commands \
    build/linux-gcc-control/examples/ecu_main/simulation.yaml
```

After the line `[vehicle_sil_launch] simulation running` appears, the simulation pauses at time 0. Type the 2 lines below, one at a time.

<!-- standard input of run --start-paused --stdin-commands: the input -->
```json
{"command":"step","ms":500}
{"command":"resume"}
```

### Verify the time commands

The dashboard ECU prints one line every 500 ms. After the first input line, the `t=500 ms` line appears. After the second, the `t=1000 ms` line and the result table appear, and the launcher exits with code 0.

<!-- standard input of run --start-paused --stdin-commands: the lines of the Dashboard -->
```text
[Dashboard] t=0 ms engine speed 0 rpm (0 messages)
[Dashboard] t=500 ms engine speed 1200 rpm (5 messages)
[Dashboard] t=1000 ms engine speed 1200 rpm (10 messages)
```

You can type these 5 lines:

| Line | What it does |
|---|---|
| `{"command":"pause"}` | Pauses. |
| `{"command":"resume"}` | Resumes. |
| `{"command":"step","ms":500}` | Advances 500 ms and pauses again. Works only while the simulation is paused. |
| `{"command":"speed","value":1}` | Changes the speed (`"max"` for as fast as possible). |
| `{"command":"stop"}` | Stops the simulation (result `interrupted`, exit code 130). |

- Closing stdin (Ctrl+D in a terminal) is the same as `stop`.
- A line that is not valid JSON is ignored, and the reason is printed on one line of stderr.
- With `--control` as well, you can control time from both stdin and the Control Server.

For the full line format and the cases where a command is not accepted, see "Stdin commands (`run --stdin-commands`)" in [simulation-events.md](../simulation-events.md#stdin-commands-run---stdin-commands). For the order of commands that come from both stdin and the Control Server, see "Stdin commands and client commands" in [control-api.md](../control-api.md#stdin-commands-and-client-commands).

## Connect from another computer

The steps so far connect from the computer that runs `vehicle_sil_launch`. To connect from another computer, you need a TLS certificate and a token. For the steps, see "Use the Control API from another computer" in [Run on several computers](multi-pc.md#use-the-control-api-from-another-computer).

## Read J1939 signals as variables

The Control API handles only time and the variables that ECUs declare. It does not handle J1939 frames. To make a received signal readable from outside, the ECU copies the received value into an output variable. A value that is set from outside goes into an input variable, and the ECU uses it in what it sends.

In the example, the dashboard ECU puts the engine speed it receives in the output variable `dashboard.engine_speed`. Here is part of the constructor of `Dashboard` in [examples/ecu_main/dashboard/main.cpp](../../examples/ecu_main/dashboard/main.cpp):

<!-- examples/ecu_main/dashboard/main.cpp: an excerpt -->
```cpp
node.on_message(ecu_main_example::kEngineSpeed, [this](const j1939::Message& m) {
    rpm_ = ecu_main_example::decode_speed(m.data);
    ++received_;
    last_ = m.time;
});
ecu.output("dashboard.engine_speed", &rpm_, {.unit = "rpm", .description = "the speed shown"});
```

Adding variables is the ECU author's work. If you need a signal, ask the author of that ECU. For how to declare variables, see "Read and write signals from outside (UI, tests)" in [j1939-guide.md](../j1939-guide.md#read-and-write-signals-from-outside-ui-tests) and "ECU variables" in [control-api.md](../control-api.md#ecu-variables).

## Notes

- You can also connect with `--control` to a scenario run (`--scenario`), but only to read, wait and stop. Writes from outside would stop the scenario from giving the same result every time. For details, see "Watching variables during a run (`--control`)" in [scenario.md](../scenario.md#watching-variables-during-a-run---control).
- The GUI also uses the Control Server of `vehicle_sil_launch` internally, so the rules for when a write is applied and for the result of a write are the same as in Python.
- `--timeout-ms` also counts the time during which the simulation is paused.
- If you receive the time state with `c.watch(time=True)`, the updates are thinned out, so a short pause may not appear. To wait for a pause, use `c.sim.wait_paused()`.
- To read from a program the address of a `vehicle_sil_launch` that you started yourself, use the `address` of the `control_server` event of `run --events`.

## What to read next

- Look up the arguments of the calls, when writes are applied and the rules of `snapshot()`: [control-api.md](../control-api.md)
- Turn the same steps into a test whose exit code gives the result: [Write tests with scenarios](scenario-tests.md)
- Read the events of `run --events` and the result of `run --json` from a program: [simulation-events.md](../simulation-events.md)
