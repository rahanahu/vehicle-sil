# ECU project (vehicle-sil template)

[日本語](README.ja.md)

This is a project template for building ECUs with [vehicle-sil](https://github.com/rahanahu/vehicle-sil). Follow the steps in this README to build 2 ECUs, run them as one simulation, and read and write their values in the browser GUI. Then rewrite the ECUs into your own.

## Prerequisites

| Requirement | Linux | Windows |
|---|---|---|
| Compiler | GCC 12 or later, or Clang (with C++20 support) | MinGW-w64 GCC (MSYS2 UCRT64, or standalone UCRT64 MinGW-w64 on `PATH`) |
| CMake 3.21 or later, Ninja, Git | Distribution packages | MSYS2 packages or standalone installers on `PATH` |
| Python 3.9 or later (to run `get_tools.py`) | Distribution package or `uv` | MSYS2 Python, standalone Python, or `uv run --no-project` |
| Python 3.11 or later (for the GUI) | If you have [uv](https://docs.astral.sh/uv/), uv installs any missing Python and packages. | uv is required (`mingw-w64-ucrt-x86_64-uv` or Windows `uv`). |

On Windows, the CI-verified environment is the MSYS2 "UCRT64" shell, though standalone MinGW-w64 (UCRT64) GCC with CMake, Ninja, and Git on `PATH` also works without MSYS2 (avoid placing another toolchain's MinGW/GCC DLLs ahead of it on `PATH`). MSVC (Visual Studio) is officially unsupported for building template ECUs. In MSYS2 UCRT64, install the requirements with:

```sh
pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-uv
```

Keep the prebuilt tools version (`VERSION` in `get_tools.py`, `0.6.0`) and `GIT_TAG` in `CMakeLists.txt` (`v0.6.0`) on the same release, or `check` will fail with `version-mismatch`. On Windows, the GUI runs on a Python that uv downloads (with prebuilt wheels). The GUI packages cannot be installed into the Python of MSYS2. External executables or tools built with other compilers (including MSVC) can still connect across process boundaries via SIL Kit or gRPC.

On WSL, place the project on the WSL Linux filesystem (such as under `~/`) rather than `/mnt/<drive>` (9p builds are much slower), and do not share one working tree or `node_modules` between Windows and WSL.

Run every command in the directory where you extracted the template (the one that contains `simulation.yaml`). Use the `vehicle_sil_launch` and `vehicle-sil-gui` that you add to `PATH` in step 2.

## Overview

| Step | What you do | What you get |
|---|---|---|
| 1 | Extract the template | Your project directory |
| 2 | Download the prebuilt tools | `vehicle_sil_launch` and `vehicle-sil-gui` under `tools/` |
| 3 | Build and run | 2 ECUs in `bin/`, and the results of the check, the run and the scenario |
| 4 | Use the ECUs in the browser GUI | A screen where you watch and write variables |

## Step 1: Extract the template

Download `vehicle-sil-template-0.6.0.zip` from the [releases page](https://github.com/rahanahu/vehicle-sil/releases) of vehicle-sil, extract it, and rename the directory to the name of your project. To manage it with Git, run `git init` there (`.gitignore` excludes `build/`, `bin/` and `tools/`).

The template and the tools exist only for released versions. A copy of `template/` from the vehicle-sil repository is the same, but its `get_tools.py` does not contain the SHA-256 of the tools (pass it with `--sha256` in step 2).

## Step 2: Download the prebuilt tools

```sh
python3 get_tools.py     # Windows (UCRT64 shell): python get_tools.py
# Or with uv without MSYS2 Python: uv run --no-project python get_tools.py
# Git Bash on Windows (adjust path printed by get_tools.py):
#   export PATH="$(cygpath -u '...\tools\vehicle-sil-tools-0.6.0-windows-x86_64\bin'):$PATH"
# then run the PATH line that it prints (from Command Prompt or PowerShell, the line named after your shell)
```

`get_tools.py` does the following, in order:

| # | What it does |
|---|---|
| 1 | Downloads the tools archive for your OS from the releases page. |
| 2 | Checks the SHA-256 of the archive. |
| 3 | Extracts it to `tools/vehicle-sil-tools-0.6.0-<OS>/`. |
| 4 | Copies the JSON Schemas in the tools to `schema/`. |
| 5 | Prints the command that adds `bin` to `PATH`. |

`VERSION` and `URL` in `get_tools.py` set the version and the download URL. It never downloads the latest version on its own.

Run the line that it prints after `put their bin directory on PATH`, and run it again in every new shell. On Linux and in an MSYS2 shell, it prints one `export PATH=...` line. Run from Command Prompt or PowerShell on Windows, it prints a line for each of the two: run the one named after your shell.

### Verify the tools

If `vehicle_sil_launch version` prints the version, the tools are in `PATH`. The `README.md` in the extracted directory describes the contents of the tools.

### Troubleshoot the download

| Symptom | Cause | Fix |
|---|---|---|
| Prints `may not be published yet` and stops | The version of this README is not released yet. | If you have the tools archive of that version as a file (for example from a colleague), run `python3 get_tools.py --archive <file>`. Otherwise clone and build the vehicle-sil repository ("Build from the repository" in the vehicle-sil README) and use the `vehicle_sil_launch` of that build. |
| The download fails | Your computer cannot reach the releases page. | Download `vehicle-sil-tools-0.6.0-linux-x86_64.tar.gz` (on Windows, `-windows-x86_64.zip`) in a browser, then run `python3 get_tools.py --archive <downloaded file>`. It still checks the SHA-256. |
| Prints `this copy of the template has no SHA-256` and stops | You copied `template/` from the repository, so it has no SHA-256. | Pass the value from `SHA256SUMS` on the releases page with `--sha256`. |

To extract the tools by hand instead of with `get_tools.py`, check the archive against `SHA256SUMS` on the releases page before you extract it, then add its `bin` to `PATH`.

## Step 3: Build and run

```sh
cmake --preset linux             # Release: linux-release | Windows: windows / windows-release
cmake --build --preset linux     # Release: linux-release | Windows: windows / windows-release
vehicle_sil_launch check simulation.yaml
vehicle_sil_launch run simulation.yaml --stop-at-ms 1000
vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml
```

| Command | What it does |
|---|---|
| `cmake --preset` | Downloads vehicle-sil and SIL Kit. |
| `cmake --build` | Builds the 2 ECUs into `bin/`. The first build also compiles SIL Kit, which takes a few minutes. |
| `check` | Starts each ECU with `--describe` and checks the configuration file against that output. |
| `run` | Starts the registry and each ECU, and runs until the requested stop time (1000 ms). |
| `run --scenario` | Sets and checks values as the steps of the scenario say. |

### Verify the build and run

| Command | What you see on success |
|---|---|
| `check` | The last line is `passed: 0 errors, 0 warnings, 0 notes`. |
| `run` | The `ended` column is `exit code 0` for both ECUs. |
| `run --scenario` | Every entry in the `result` column is `ok`, and the exit code is 0. |

Step 1 of the guide "[Create your first ECU](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/howto/first-ecu.md)" shows the full output of `check` and `run`.

### Troubleshoot the build and run

| Symptom | Cause | Fix |
|---|---|---|
| `vehicle_sil_launch` is not found | You are in a new shell and have not run the command that adds the tools to `PATH`. | Run `python3 get_tools.py` again, then run the PATH line that it prints for your shell. |
| On Windows, an ECU exits without printing anything (`0xC0000135` / `0xC0000139`) | A required DLL is missing (`0xC0000135`) or an incompatible MinGW runtime DLL was loaded (`0xC0000139`, e.g., stale DLLs next to the executable or mismatched DLLs on `PATH`). | Non-static builds copy `SilKit.dll` and the toolchain's MinGW runtime DLLs next to each ECU via `vehicle_sil_copy_runtime_dlls(<target>)` in `CMakeLists.txt`. Verify that call is present (or build with `-DVEHICLE_SIL_STATIC=ON`). Because Windows normally loads DLLs next to the executable before searching `PATH`, for `0xC0000139` first inspect the DLLs next to `bin/<ecu>.exe` to confirm they match the build toolchain, and perform a clean rebuild with that same toolchain if needed so `vehicle_sil_copy_runtime_dlls()` copies matching DLLs. In MSYS2 UCRT64, `ldd bin/<ecu>.exe` lists imported DLLs; `where.exe <dll>` in PowerShell/cmd only lists candidate DLLs on `PATH` (it does not tell you which DLL Windows actually loaded). |
| `check` prints `version-mismatch` | The ECUs and the tools use different versions of vehicle-sil. | Make the versions match, as described in "Upgrade vehicle-sil" below. |

The `vehicle_sil_launch` of the tools is a single executable that needs no DLLs (the `bin` of the tools contains none).

## Step 4: Use the ECUs in the browser GUI

```sh
vehicle-sil-gui simulation.live.yaml
```

The browser opens a screen where you check the configuration file, run the simulation, and watch and write variables. To quit, press Ctrl+C.

`simulation.live.yaml` configures the same 2 ECUs and their screen layout. Choose real-time speed in the GUI to watch and change values until you stop the run.

## What the template contains

| File | Contents |
|---|---|
| `engine/main.cpp` | The engine ECU (input variable `engine.throttle`, output variable `engine.speed`, sends the engine speed over J1939 every 100 ms). |
| `dashboard/main.cpp` | The dashboard ECU (receives the engine speed over J1939 and puts it in the output variable `dashboard.engine_speed`). |
| `engine_speed.hpp` | The message definition that the 2 ECUs share (PGN and data format). |
| `CMakeLists.txt`, `CMakePresets.json` | Include vehicle-sil with FetchContent and build the executables into `bin/` (`GIT_TAG` is the vehicle-sil version). |
| `simulation.yaml` | The configuration file that combines the 2 ECUs into one simulation. |
| `scenarios/throttle_step.yaml` | A scenario that presses the throttle and checks the engine speed and the dashboard. |
| `simulation.dashboard.yaml` | The layout of the **Variables** screen of the GUI. |
| `simulation.live.yaml`, `simulation.live.dashboard.yaml` | A configuration file for watching and operating the ECUs, and its screen layout. |
| `schema/` | JSON Schemas for the 3 kinds of YAML above. |
| `get_tools.py` | Downloads the tools (`vehicle_sil_launch` and the GUI) to `tools/`, checking the version and the SHA-256. |

Because `schema/` is there, editors that use a YAML language server offer completion and descriptions of keys, and check the format. The first line of each YAML file, `# yaml-language-server: $schema=...`, points to its schema. Write the same line in YAML files that you add.

Do not edit the files in `schema/` by hand: `get_tools.py` overwrites them with the schemas in the tools. It overwrites only files with the same names; files you added stay as they are.

## Make it your own ECU

| Goal | What to change |
|---|---|
| Change what the ECU does | Rewrite the body of the class in `engine/main.cpp` (networks, J1939 nodes, variables, per-step work). |
| Rename the ECU | Change `ecu_main<...>(..., {.name = "Engine", ...})` in `main()`. |
| Add an ECU | Add the 4 lines from `add_executable` to `vehicle_sil_copy_runtime_dlls` to `CMakeLists.txt`, and add the ECU to `participants` in `simulation.yaml`. |
| Rename the network (`J1939_CAN`) | Use the same name in the ECU code and in `networks` of `simulation.yaml`. |

The Manufacturer Code 0x7F0 is a placeholder. You can link optional helper targets alongside `vehicle_sil::simulation` in `CMakeLists.txt` (such as `vehicle_sil::j1939_periodic_sender`, `vehicle_sil::j1939_monitor`, or `vehicle_sil::spy`; see [docs/build.md](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/build.md#use-from-another-project-fetchcontent)).

For the complete procedure to add an ECU, see the guide "[Create your first ECU](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/howto/first-ecu.md)".

## Distribute a single executable

To give an ECU executable to someone without any other files (`libSilKit.so`, `SilKit.dll`, MinGW DLLs), reconfigure with `VEHICLE_SIL_STATIC`:

```sh
cmake --preset linux -DVEHICLE_SIL_STATIC=ON      # Windows: cmake --preset windows -DVEHICLE_SIL_STATIC=ON
cmake --build --preset linux                      # Windows: cmake --build --preset windows
```

SIL Kit and the C++ runtime are then linked statically into the executable (glibc on Linux stays a shared library). On Linux, this needs a static libstdc++ (`libstdc++-static` on Fedora). To switch back, reconfigure with `-DVEHICLE_SIL_STATIC=OFF`.

When you give the executable to someone, ship the license notices of what it contains with it. For the list, see "License notices to ship with the executable" in [docs/build.md](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/build.md) of vehicle-sil. The tools contain files ready to ship in `licenses/sil-kit/` (and, on Windows, `licenses/msys2/`).

## Upgrade vehicle-sil

Keep the ECUs and the tools on the same version of vehicle-sil. If the versions differ, `check` prints `version-mismatch`.

| # | What you do |
|---|---|
| 1 | From the template of the new version, copy `get_tools.py` and the `GIT_TAG` line of `CMakeLists.txt`. |
| 2 | Run `python3 get_tools.py` again. |
| 3 | Rebuild with `cmake --build --preset linux` (`windows` on Windows). |
| 4 | Open format 1 configurations in a local editor and set `vehicle_sil_simulation: 2`. Move `stop_at_ms`, `speed` and `late_tolerance_ms` to run options or a run request. Move `start_timeout_ms` and `stop_timeout_ms` to `--participant-start-timeout-ms` and `--participant-stop-timeout-ms`. Keep `time_step_ms` in the configuration if you need a fixed step. |

`get_tools.py` copies the JSON Schemas in the tools to `schema/` again and prints the copied files on one line. Editor completion and checks then know the keys of the new version. The `CHANGELOG.md` of vehicle-sil lists the changes and what to do about them.

## What to read next

- Add an ECU and read and write its variables from Python: [Create your first ECU](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/howto/first-ecu.md) (also covers J1939 communication and starting an ECU from a debugger)
- Write J1939 nodes: [Writing J1939 nodes](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/j1939-guide.md), [J1939 typed messages](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/j1939-codec.md)
- Look up the configuration file and the commands: [Configuration file](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/simulation-config.md), [Checks](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/simulation-check.md), [Running](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/simulation-run.md), [Scenarios](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/scenario.md), [Browser GUI](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/gui.md)

## License

The files of this template are under MIT-0 (MIT No Attribution, `LICENSE`). A project that you create from a copy of the template does not need to keep the copyright or license notice of this template. vehicle-sil itself and its dependencies, which the template pulls in, keep their own licenses (vehicle-sil is MIT; for the dependencies, see `THIRD_PARTY_NOTICES.md` of vehicle-sil).
