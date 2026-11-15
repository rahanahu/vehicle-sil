# Create your first ECU

[日本語](../ja/howto/first-ecu.md)

In this guide you add an ECU of your own to the template project and run it in a simulation with the other ECUs. At the end, you read and write its variables from Python. The ECU is a small one called `Cooler`: its coolant temperature moves toward a target temperature, and that is all it does. CAN communication comes after the steps, in "Add J1939 communication".

[examples/first_ecu](../../examples/first_ecu) contains the finished files, written as the steps describe.

## Prerequisites

| Requirement | Details |
|---|---|
| OS | Work on Linux, or in the MSYS2 UCRT64 shell on Windows. |
| Compiler, CMake, Ninja, Git | For the versions, see the table in "Prerequisites" in the template's [README](../../template/README.md#prerequisites). |
| Python | 3.9 or later, to run `get_tools.py`. |
| [uv](https://docs.astral.sh/uv/) | Runs the Python client in step 7. |

The commands are for Linux. The table in step 1 lists the commands that differ on Windows.

## Overview

| Step | What you do | What you get |
|---|---|---|
| 1 | Build the template and run it as is | 2 ECU executables, and the output of `check` and `run` |
| 2 | Write the ECU class and `main()` | `cooler/main.cpp` |
| 3 | Add the ECU to CMake and build it | `bin/cooler_ecu` |
| 4 | Try the ECU on its own | JSON that describes the ECU |
| 5 | Add the ECU to the configuration file and check it | A `simulation.yaml` with 3 ECUs |
| 6 | Run the simulation | A table that shows all 3 ECUs ended with exit code 0 |
| 7 | Read and write variables from Python | The target temperature you wrote and the coolant temperature you read |

Do steps 1–7 in order. In the text, a "simulation step" is something else: the unit by which the simulation advances virtual time. Read "Add J1939 communication" and "Run the ECU from a debugger" when you need them.

## Step 1: Build and run the template

First run the 2 ECUs that come with the template (engine and dashboard) as they are. This confirms that everything you need to build and run is in place.

Download the template archive (`vehicle-sil-template-<version>.zip`) from the [releases page](https://github.com/rahanahu/vehicle-sil/releases) and extract it. Rename the directory `vehicle-sil-template-<version>` from the zip to the name of your project. Run the commands below in that directory, and every later command too: it is the directory that contains `simulation.yaml`.

If `get_tools.py` stops without the tools (it prints `may not be published yet`, or the download fails), go to "[Troubleshoot the template run](#troubleshoot-the-template-run)": you can pass a tools archive that you already have with `--archive`, or use a build of the repository.

```sh
python3 get_tools.py             # downloads the prebuilt tools to tools/, then prints an export PATH=... line
export PATH="$PWD/tools/vehicle-sil-tools-<version>-linux-x86_64/bin:$PATH"   # the printed line; run it in every new shell
cmake --preset linux             # downloads vehicle-sil and SIL Kit
cmake --build --preset linux     # the first time only, compiling SIL Kit takes a few minutes
vehicle_sil_launch check simulation.yaml
vehicle_sil_launch run simulation.yaml
```

On Windows, 4 things differ:

| What | Linux | Windows (MSYS2 UCRT64 shell) |
|---|---|---|
| Download the tools | `python3 get_tools.py` | `python get_tools.py` |
| Configure | `cmake --preset linux` | `cmake --preset windows` |
| Build | `cmake --build --preset linux` | `cmake --build --preset windows` |
| Tools directory | `tools/vehicle-sil-tools-<version>-linux-x86_64` | `tools/vehicle-sil-tools-<version>-windows-x86_64` |

### Verify the template run

The last line of the `check` output is `passed:` and the exit code is 0. The `check` command only checks the configuration file against each ECU; it does not run the simulation.

<!-- vehicle_sil_launch check simulation.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

passed: 0 errors, 0 warnings, 0 notes
```

The `run` command does the same check, then starts the SIL Kit registry and the 2 ECUs and runs until virtual time 1000 ms. It prints this result to stdout. Both entries in the `ended` column must be `exit code 0`.

<!-- vehicle_sil_launch run simulation.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

traffic: J1939_CAN: 1 participant sent, 1 received (2 counted) (steps before 1000 ms)

network    participant  sent  received
J1939_CAN  Engine         11         0
J1939_CAN  Dashboard       0        11

passed: 0 errors, 0 warnings, 0 notes
```

Stderr shows the progress of the run. The lines that start with `[Dashboard]` show that the dashboard ECU receives the engine speed:

```text
[Dashboard] t=0 ms engine speed 0 rpm (0 messages)
[vehicle_sil_launch] simulation running
[Dashboard] t=500 ms engine speed 1200 rpm (5 messages)
[Dashboard] t=1000 ms engine speed 1200 rpm (10 messages)
```

### The configuration file you ran

The `simulation.yaml` that you passed to `check` and `run` is the configuration file. It lists the networks and the ECU executables that take part in the simulation. In step 5, you add your ECU to it.

<!-- template/simulation.yaml -->
```yaml
# yaml-language-server: $schema=schema/simulation.schema.json
# The two ECUs of this project in one simulation. Build them first (README.md), then, here:
#   vehicle_sil_launch check simulation.yaml
#   vehicle_sil_launch run simulation.yaml [--scenario scenarios/throttle_step.yaml]
# The paths in command are relative to this file's directory.
vehicle_sil_simulation: 1
stop_at_ms: 1000
networks:
  - name: J1939_CAN
    kind: can
participants:
  - name: Engine
    command: [bin/engine_ecu]
    step_ms: 10
  - name: Dashboard
    command: [bin/dashboard_ecu]
    step_ms: 5
```

### Troubleshoot the template run

| Symptom | Cause | Fix |
|---|---|---|
| `get_tools.py` prints `may not be published yet` and stops | That version is not released yet. | If you have the tools archive of that version as a file (for example from a colleague), run `python3 get_tools.py --archive <file>`. Otherwise use a `vehicle_sil_launch` that you built as described in "[Build from the repository](../../README.md#build-from-the-repository)" in the README. |
| The download fails | Your computer cannot reach the releases page. | Download `vehicle-sil-tools-<version>-linux-x86_64.tar.gz` (on Windows, `-windows-x86_64.zip`) in a browser, then run `python3 get_tools.py --archive <downloaded file>`. The SHA-256 check is the same as for a download. |
| `vehicle_sil_launch` is not found | You are in a new shell and have not added the tools to `PATH`. | Run `python3 get_tools.py` again, then run the `export` line it prints. |
| `check` prints an error that ends in `[executable]` | The ECU executables are not in `bin/`. | Run `cmake --build --preset linux`. |

For other ways to use `get_tools.py`, see the template's [README](../../template/README.md).

## Step 2: Write the ECU class and `main()`

In the project directory (the one that contains `simulation.yaml`), create a directory named `cooler`. Write the following into `cooler/main.cpp`. This is the complete file.

<!-- examples/first_ecu/cooler/main.cpp -->
```cpp
// The cooler ECU: the coolant temperature moves towards a target that is written from outside.
#include "vehicle_sil/simulation/ecu_main.hpp"

#include <chrono>

using namespace std::chrono_literals;

namespace {

class Cooler
{
public:
    explicit Cooler(vehicle_sil::Ecu& ecu)
    {
        ecu.input("cooler.target", &target_,
                  {.unit = "degC", .min = 0.0, .max = 120.0, .description = "target coolant temperature"});
        ecu.output("cooler.temperature", &temperature_, {.unit = "degC"});
        // The model: every 100 ms the temperature moves 0.5 degC towards the target.
        ecu.every(100ms, [this](vehicle_sil::SimTime) {
            temperature_ += temperature_ < target_ ? 0.5 : temperature_ > target_ ? -0.5 : 0.0;
        });
    }

private:
    double target_ = 90.0;
    double temperature_ = 20.0;
};

} // namespace

int main(int argc, char** argv)
{
    return vehicle_sil::ecu_main<Cooler>(argc, argv, {.name = "Cooler", .step = 100ms});
}
```

An ECU is an ordinary class that takes a `vehicle_sil::Ecu&`. Its constructor declares the variables and the periodic work.

| Code | Meaning |
|---|---|
| `ecu.input("cooler.target", &target_, {...})` | Declares a variable that scenarios, Python and the GUI can write. Each written value goes into `target_`. |
| `ecu.output("cooler.temperature", &temperature_, {...})` | Declares a variable that scenarios, Python and the GUI can read. Its value is the value of `temperature_`. |
| `{.unit = ..., .min = ..., .max = ..., .description = ...}` | Unit, range and description. All are optional. |
| `ecu.every(100ms, ...)` | Registers work that runs every 100 ms of virtual time. |
| `ecu_main<Cooler>(argc, argv, {...})` | Turns the class into an executable that takes the same arguments as every other ECU (`--describe`, `--local` and others). |
| `.name = "Cooler"` | The name of the ECU. It becomes `name` in the `--describe` output. |
| `.step = 100ms` | The step size: the virtual time that the ECU advances in one simulation step. |

Follow these rules:

- Variable names cannot start with `ecu.` or `peer.`. These prefixes are reserved for the variables that vehicle-sil adds automatically.
- Reads and writes of variables and the work in `every()` run on the same thread. The model needs no locks.
- Call `ecu_main()` at the end of `main()` and return its result, as above. With `--describe`, everything the program writes to stdout goes to stderr until the process ends, even after `ecu_main()` returns; only the JSON goes to stdout.
- An ECU can have variables and no network, like this one.

For the variable types and the kinds of declaration, see "ECU variables" in [control-api.md](../control-api.md#ecu-variables). You verify the code by building it in the next step.

## Step 3: Add the ECU to CMake and build it

Add these 4 lines to the end of `CMakeLists.txt`. They are the 4 lines of the engine ECU (lines 32–36 of [template/CMakeLists.txt](../../template/CMakeLists.txt) without the comment) with the name and the source file changed.

<!-- examples/first_ecu/CMakeLists.txt -->
```cmake
add_executable(cooler_ecu cooler/main.cpp)
target_include_directories(cooler_ecu PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(cooler_ecu PRIVATE vehicle_sil::simulation)
vehicle_sil_copy_runtime_dlls(cooler_ecu)
```

| Line | Meaning |
|---|---|
| `add_executable` | Names the executable (`cooler_ecu`) and its source file. |
| `target_include_directories` | Lets the code `#include` headers in the project root, such as `engine_speed.hpp`. |
| `target_link_libraries` | Links `ecu_main()` and the libraries that an ECU uses (variables and the CAN, LIN and J1939 stacks). |
| `vehicle_sil_copy_runtime_dlls` | On Windows, copies `SilKit.dll` next to the executable. |

Build the project. Because `CMakeLists.txt` changed, CMake reruns the configure step on its own.

```sh
cmake --build --preset linux
ls bin
```

### Verify the build

The `bin/` directory now contains `cooler_ecu` as well:

```text
cooler_ecu  dashboard_ecu  engine_ecu
```

## Step 4: Try the ECU on its own

Before you add the ECU to the simulation, run the executable by itself. This shows what the ECU declares, and that it runs without throwing an exception.

```sh
bin/cooler_ecu --describe
bin/cooler_ecu --local --run-for-ms 1000; echo "exit code $?"
```

### Verify the standalone ECU

With `--describe`, the ECU prints its configuration as JSON to stdout. It runs nothing and connects to nothing. Here are the keys this step looks at, taken from that output (other keys are left out, and each variable is put on one line):

<!-- cooler_ecu --describe: an excerpt -->
```json
{
  "name": "Cooler",
  "step_ns": 100000000,
  "networks": [],
  "variables": [
    { "name": "ecu.connected", "type": "bool", "causality": "input", "builtin": true },
    { "name": "cooler.target", "type": "real", "causality": "input", "unit": "degC", "min": 0, "max": 120, "builtin": false },
    { "name": "cooler.temperature", "type": "real", "causality": "output", "unit": "degC", "min": null, "max": null, "builtin": false }
  ]
}
```

| Key | Value for this ECU | Should match |
|---|---|---|
| `name` | `"Cooler"` | The `.name` passed to `ecu_main()`. |
| `step_ns` | `100000000` | The 100 ms of `.step`, in nanoseconds. |
| `networks` | `[]` | The declared networks. This ECU has none. |
| `variables` | `ecu.connected`, `cooler.target`, `cooler.temperature` | The 2 declared variables, plus `ecu.connected`, which every ECU gets automatically (`builtin` is `true`). |

Python and scenarios refer to a variable by its `name` in `variables`. Use `--describe` to look up the variable names of ECUs that other people built, too. For all keys and the full output format, see "JSON format" in [ecu-executable.md](../ecu-executable.md#json-format).

The options `--local --run-for-ms 1000` run this ECU alone for 1000 ms of virtual time. On success the ECU prints nothing, so look at the exit code:

```text
exit code 0
```

### Troubleshoot the standalone ECU

| Symptom | Cause | Fix |
|---|---|---|
| Prints the usage and exits with code 4 | The arguments are missing or wrong. | Add either `--describe` or `--local --run-for-ms <ms>`. |
| Prints `error: Cooler: ...` and exits with code 3 | The constructor threw an exception, for example because of a duplicate variable name or a name that starts with `peer.`. | Fix the code as the reason on the `error:` line says. |
| `--local` exits with code 1 | Work in `every()` threw an exception. | Fix the code as the reason on the `error:` line says. |

For the arguments and exit codes, see "Arguments and exit codes" in [ecu-executable.md](../ecu-executable.md#arguments-and-exit-codes).

## Step 5: Add the ECU to the configuration file and check it

Add these 3 lines at the end of `participants` in `simulation.yaml`:

<!-- examples/first_ecu/simulation.yaml: the lines added -->
```yaml
  - name: Cooler
    command: [bin/cooler_ecu]
    step_ms: 100
```

| Key | What to write |
|---|---|
| `name` | The name of the ECU in the simulation (the participant name). Python and scenarios refer to the ECU by this name. |
| `command` | The path of the executable. A relative path is relative to the directory of the configuration file. |
| `step_ms` | The step size. If you write it, the check compares it with the ECU's `.step`. You can leave it out. |

Do not add arguments such as `--name` to `command`: `vehicle_sil_launch` passes the value of `name` to the ECU as `--name`. `Cooler` uses no network, so add nothing to `networks`. For the list of keys, see [simulation-config.md](../simulation-config.md).

```sh
vehicle_sil_launch check simulation.yaml
```

### Verify the check

The table has a new row for `Cooler`, and the last line is `passed:`. The `-` in the `networks` column means that the ECU uses no network.

<!-- first ECU: vehicle_sil_launch check simulation.yaml -->
```text
simulation.yaml: 3 participants, 1 network

participant  step    networks
Engine       10 ms   J1939_CAN (j1939 0x00)
Dashboard    5 ms    J1939_CAN (j1939 0x28)
Cooler       100 ms  -

network    kind  participants
J1939_CAN  can   Engine, Dashboard

passed: 0 errors, 0 warnings, 0 notes
```

### Troubleshoot the check

The check reports each mismatch between the configuration file and an ECU on a line that starts with `error:`, `warning:` or `note:`. Each line gives the line number in the file, the mismatch, how to fix it and the finding code. With `step_ms` mistyped as `10`, the output ends like this (the line number depends on your file):

<!-- first ECU: vehicle_sil_launch check simulation.yaml with step_ms: 10, the end -->
```text
error: simulation.yaml:20: participant 'Cooler': step_ms is 10, but the executable's step is 100 ms: write step_ms: 100, or remove step_ms [step-mismatch]

failed: 1 error, 0 warnings, 0 notes
```

| Code at the end of the line | Cause | Fix |
|---|---|---|
| `[step-mismatch]` | `step_ms` differs from the ECU's `.step`. | Make `step_ms` match the ECU, or remove it. |
| `[executable]` | There is no executable at the path in `command`. | Build the ECU, or correct the path. |
| `[single-participant]` | The network has only one ECU, so nobody receives the messages it sends. | If you left the other ECUs out on purpose with `--disable` (below), ignore this warning. |

To leave some of the other ECUs out, keep the configuration file as it is and pass their participant names to `--disable`. For example, `vehicle_sil_launch check simulation.yaml --disable Dashboard` leaves only `Engine` on `J1939_CAN`, so the check reports the warning below. With only warnings, the last line is still `passed:`.

<!-- first ECU: vehicle_sil_launch check simulation.yaml --disable Dashboard, the warning -->
```text
warning: simulation.yaml:9: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it [single-participant]
```

[simulation-check.md](../simulation-check.md) lists all finding codes.

## Step 6: Run the simulation

Run the 3 ECUs until 1000 ms, the time set in `stop_at_ms`:

```sh
vehicle_sil_launch run simulation.yaml
```

### Verify the run

In the last table on stdout, `ended` for `Cooler` is `exit code 0`.

<!-- first ECU: vehicle_sil_launch run simulation.yaml, after the tables of the check -->
```text
run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0
Cooler       yes       exit code 0

passed: 0 errors, 0 warnings, 0 notes
```

The `run` output does not show variable values. The `[Dashboard]` lines on stderr are there because the dashboard ECU calls `printf` itself; `Cooler` prints nothing, so it has no lines. To see variable values, use one of these:

| Method | Best for | Where |
|---|---|---|
| Python client | Reading and writing values from a script | Step 7 |
| Scenario | Turning value settings and checks into automated tests | [Write tests with scenarios](scenario-tests.md) |
| Browser GUI | Watching values in charts while you change them by hand | [gui.md](../gui.md) |

To interrupt the run, press Ctrl+C. For the output format and the exit codes, see "Output" in [simulation-run.md](../simulation-run.md#output).

## Step 7: Read and write variables from Python

Write the target temperature of `Cooler`, advance the simulation by 500 ms, then read the coolant temperature. The ECU code needs no changes: you can read and write the variables of any ECU built with `ecu_main()` from outside the simulation.

Write the following into `read_cooler.py` in the project directory (next to `simulation.yaml`). This is the complete file.

<!-- examples/first_ecu/read_cooler.py -->
```python
from vehicle_sil_control import Control, Launch, ms

# Starts "vehicle_sil_launch run simulation.yaml" with its control server, paused before the first step.
with Launch("vehicle_sil_launch", "simulation.yaml", start_paused=True) as launch:
    with Control(launch.address) as c:
        c.sim.wait_paused()                                   # every ECU stands still at 0 ms
        seq = c.write({"Cooler": {"cooler.target": 22.0}})    # participant name, then variable name
        c.wait(seq, until="queued")                           # the ECU has received the write

        c.sim.step(ms(500))                                   # run 500 ms, then pause again
        now = c.sim.wait_paused().now
        values = c.values("Cooler").values                    # the values of the last step
        print(f"at {now / 1e6:g} ms: cooler.target = {values['cooler.target']}, "
              f"cooler.temperature = {values['cooler.temperature']}")
```

The Python client (`vehicle_sil_control`) is in the `python/` directory of the prebuilt tools. Run the script with uv. Replace `<version>` with the version that `vehicle_sil_launch version` prints; it is also part of the directory name under `tools/`.

```sh
T=tools/vehicle-sil-tools-<version>-linux-x86_64
export PATH="$PWD/$T/bin:$PATH"   # as in step 1: the script starts vehicle_sil_launch
PYTHONPATH=$T/python uv run --no-project --no-build --python '>=3.11,<3.15' \
    --with-requirements $T/python/requirements.txt python read_cooler.py
```

| Argument | Meaning |
|---|---|
| `PYTHONPATH=$T/python` | Where the client package is. |
| `--with-requirements` | Installs the packages at the versions pinned in `requirements.txt` (grpcio, protobuf and others). |
| `--python '>=3.11,<3.15'` | Uses a Python in this range. If the computer has none, uv downloads one. |
| `--no-project` | Ignores a `pyproject.toml` in a parent directory. |
| `--no-build` | Uses prebuilt packages (wheels) only. |

### Verify the Python output

The script prints one line:

<!-- examples/first_ecu/read_cooler.py: the output -->
```text
at 500 ms: cooler.target = 22.0, cooler.temperature = 22.0
```

The coolant temperature rises from 20 degC by 0.5 degC every 100 ms and stops at the target you wrote, 22 degC. Without the write, the target stays at 90 degC.

What each line of the script does:

| Line | What it does |
|---|---|
| `Launch(..., start_paused=True)` | Starts `vehicle_sil_launch run` as a child process and holds every ECU before its first simulation step. |
| `Control(launch.address)` | Connects over gRPC to the Control Server of the `vehicle_sil_launch` it started. |
| `c.sim.wait_paused()` | Waits until the simulation is paused. |
| `c.write({"Cooler": {"cooler.target": 22.0}})` | Writes a value. It names the participant as in the configuration file and the variable as in the `--describe` output. |
| `c.wait(seq, until="queued")` | Waits until the ECU has received the write. The write is applied in the simulation step at 0 ms. |
| `c.sim.step(ms(500))` | Advances virtual time by 500 ms, then pauses again. |
| `c.values("Cooler").values` | Returns the values that the ECU sent last, as a dict keyed by variable name. |

Leaving the `with` block stops the simulation, and the script ends.

### Troubleshoot the Python script

| Symptom | Cause | Fix |
|---|---|---|
| `ModuleNotFoundError: No module named 'vehicle_sil_control'` | `PYTHONPATH` is not set. | Put `PYTHONPATH=$T/python` at the start of the command. |
| `FileNotFoundError: ... 'vehicle_sil_launch'` | `vehicle_sil_launch` is not in `PATH`. | Run the `export` line from step 1. |
| `AttributeError: 'NoneType' object has no attribute 'values'` | Until the simulation has advanced at least one simulation step, there are no values, and `c.values()` returns `None`. | Advance with `c.sim.step()` before you read. |
| A written value is not applied | Writing a value out of range (`cooler.target` takes 0–120) or writing to an `output()` variable raises no exception; the result of the write is `invalid`. | Look at the result with `c.wait(seq).target("Cooler").status`. |
| The dict has names you did not declare, such as `peer.steps` | These are monitoring variables that vehicle-sil adds automatically. | Your own variables are the ones with `builtin` set to `false` in the `--describe` output. |

To pause a running simulation and write, or to read the values of several ECUs at the same virtual time, see [Read and write signals from outside](operate-signals.md).

## Add J1939 communication

`Cooler` uses no network. For ECUs that send and receive J1939 over CAN, the template's engine ECU (sender) and dashboard ECU (receiver) serve as examples. Compared with an ECU that has only variables, they add the following:

| What to add | Example |
|---|---|
| A header, shared by sender and receiver, with the message definitions (PGN and data format) | [template/engine_speed.hpp](../../template/engine_speed.hpp) |
| The `#include` of the header, and the namespace alias `j1939` | Lines 11 and 18 of [template/engine/main.cpp](../../template/engine/main.cpp) |
| The connection to the network, and the J1939 node | Lines 27–28 of the same file |
| Periodic sending (`node_.send()`) | Lines 38–40 of the same file |
| Receiving (`node.on_message()`) | Lines 22–27 of [template/dashboard/main.cpp](../../template/dashboard/main.cpp) |
| The entry in `networks` of the configuration file | `J1939_CAN` in the `simulation.yaml` of step 1 |

The class of the engine ECU (lines 22–47 of `template/engine/main.cpp`):

```cpp
class Engine
{
public:
    explicit Engine(vehicle_sil::Ecu& ecu) :
      // The network's name is the one in simulation.yaml. 0x7F0 is a placeholder Manufacturer Code.
      node_(ecu.j1939("J1939_CAN").add_node(
          {.address = 0x00, .name = j1939::Name{}.set_identity_number(1).set_manufacturer_code(0x7F0)}))
    {
        ecu.input("engine.throttle", &throttle_,
                  {.unit = "%", .min = 0.0, .max = 100.0, .description = "accelerator pedal position"});
        ecu.output("engine.speed", &rpm_, {.unit = "rpm"});
        // The model: the speed moves towards 800 rpm plus 20 rpm per percent of throttle.
        ecu.every(10ms, [this](vehicle_sil::SimTime) {
            const int target = 800 + static_cast<int>(throttle_ * 20.0);
            rpm_ += rpm_ < target ? 50 : rpm_ > target ? -50 : 0;
        });
        ecu.every(100ms, [this](vehicle_sil::SimTime) {
            static_cast<void>(node_.send(my_ecus::kEngineSpeed, my_ecus::encode_speed(rpm_)));
        });
    }

private:
    j1939::Node& node_;
    double throttle_ = 20.0;
    int rpm_ = 0;
};
```

To run on the same network as other ECUs, decide 3 things:

| What to decide | Rule | If you break the rule |
|---|---|---|
| The network name (`"J1939_CAN"`) | Use the same name in the code and in `networks` of the configuration file. | `check` reports the error `[unknown-network]`. |
| The J1939 address and NAME | Pick values that no other node on the same network uses. | `check` reports `[address-conflict]`, `[address-shared]` or `[name-conflict]`. |
| Manufacturer Code | `0x7F0` in the example is a placeholder. Replace it with your own value. | — |

The `networks` key in the `--describe` output shows the network name, the address and the NAME. For how to send and receive, see [j1939-guide.md](../j1939-guide.md). For defining signals in YAML and generating types from them, see [j1939-codec.md](../j1939-codec.md).

## Run the ECU from a debugger

To run the simulation while you stop your ECU in a debugger, start only that ECU yourself.

1. Leave the configuration file as it is, and run with the participant name in `--manual`:

   ```sh
   vehicle_sil_launch run simulation.yaml --manual Cooler
   ```

2. `vehicle_sil_launch` starts the other ECUs, prints the command that starts `Cooler` to stderr, and waits. `<dir>` is the project directory.

   ```text
   [vehicle_sil_launch] not starting Cooler: start it yourself (manual: true, or --manual); the simulation waits for it to join. In the directory <dir>, run:
   [vehicle_sil_launch]   <dir>/bin/cooler_ecu --name Cooler --registry-uri silkit://127.0.0.1:8500 --start-timeout-ms 1000000000000
   [vehicle_sil_launch] waiting for Cooler to join (manual): start it with the command above; Ctrl+C stops the run
   ```

3. In the debugger, set the executable from the second line, the arguments that follow it, and the working directory that follows `In the directory` on the first line. Then start the ECU. To try it in a terminal instead, run the command from the second line in that directory.

4. When `Cooler` joins, the simulation starts. When the simulation ends, `Cooler` exits on its own.

### Verify the debugger run

In the result table, `ended` for `Cooler` is `started by you (manual)`.

```text
participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0
Cooler       yes       started by you (manual)

passed: 0 errors, 0 warnings, 1 note
```

### What to expect while debugging

| Situation | What happens |
|---|---|
| Until you start `Cooler` | `vehicle_sil_launch` waits with no time limit. |
| While the ECU is stopped at a breakpoint | Virtual time stops for the whole simulation. |
| `Cooler` exits after it joins but before the simulation starts | The start fails and the run ends. Start the run again with `vehicle_sil_launch`. |
| The printed command | The registry port is fixed at 8500, so the command is the same every time and you can reuse the debugger configuration. |
| Another program uses port 8500 | Write a different port in `registry_uri` in the configuration file. |
| The output of `Cooler` | Appears where you started the ECU (the debugger or the terminal), not on the stderr of `vehicle_sil_launch`. |
| Pasting the command into PowerShell | Put `&` before the quoted executable. |

For how the launcher waits and how to stop it, see "Starting a participant yourself (`manual`, `--manual`)" in [simulation-config.md](../simulation-config.md#starting-a-participant-yourself-manual---manual).

## What to read next

- Run your ECU with ECUs that other people built: [Integrate and run several ECUs](integrate-ecus.md)
- Turn value settings and checks into automated tests: [Write tests with scenarios](scenario-tests.md)
- Pause a running simulation to write values, and advance time: [Read and write signals from outside](operate-signals.md)
