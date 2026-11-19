# Building and integrating vehicle-sil

[日本語](ja/build.md)

This page explains how to build vehicle-sil itself and how to include vehicle-sil in your own CMake project.

| Goal | Section |
|---|---|
| Build the repository and run the tests | [Requirements](#requirements), [Build vehicle-sil itself](#build-vehicle-sil-itself) |
| Use the tools without building them | [Prebuilt tools](#prebuilt-tools) |
| Use vehicle-sil from your own CMake project | [Use from another project (FetchContent)](#use-from-another-project-fetchcontent) |
| Ship an ECU as a single executable that needs no other files | [Build a single executable (`VEHICLE_SIL_STATIC`)](#build-a-single-executable-vehicle_sil_static) |

## Requirements

| Requirement | Used for | Linux | Windows |
|---|---|---|---|
| Compiler | Every build | GCC (default) or Clang with C++20 support | MinGW-w64 GCC of MSYS2 UCRT64 |
| CMake 3.21 or later, Ninja | Every build | Distribution packages | MSYS2 packages |
| Git | Cloning, and applying the patches to SIL Kit | Distribution package | MSYS2 package |
| Python 3.11 or later with PyYAML | Building the tests and examples, generating typed messages | Install with [uv](https://docs.astral.sh/uv/) (recommended) | MSYS2 packages |
| Python 3.11 or later with grpcio, grpcio-tools and protobuf | The Python client of the Control API | Install with uv (recommended) | MSYS2 packages |

To build the Control Server and pull in gRPC with FetchContent, you need CMake 3.28 or later (see "gRPC and Protocol Buffers (Control Server, optional)" in [dependencies.md](dependencies.md#grpc-and-protocol-buffers-control-server-optional)). You do not need Visual Studio or MSVC.

After you clone, run `scripts/doctor.py` to see whether everything is in place. The script changes nothing; for anything missing, it prints how to install it.

### Python (uv)

`pyproject.toml`, `.python-version` and `uv.lock` at the repository root pin the Python version and the Python packages. With [uv](https://docs.astral.sh/uv/), the commands below install the development Python and all packages into `.venv`. If the computer does not have the Python version in `.python-version`, uv downloads it.

```sh
uv sync                        # everything (the default dev group)
uv sync --only-group codegen   # typed message generation only (PyYAML)
uv sync --only-group control   # Control API client only (grpcio, grpcio-tools, protobuf)
```

`--only-group` removes the packages of the other groups from `.venv`. For example, after `uv sync --only-group control`, you cannot use `.venv` to generate typed messages. If in doubt, run `uv sync`.

Which Python CMake uses depends on how you build (see "How Python is found" in [dependencies.md](dependencies.md#how-python-is-found)).

| Build | Python that CMake uses |
|---|---|
| On Linux, with vehicle-sil as the top-level project (`.venv` has the required packages) | The Python in `.venv`, first. You do not need to activate the environment or name the Python. |
| The same build, without `.venv` or without the required packages | The one that `find_package(Python3)` finds. |
| A consumer's build that includes vehicle-sil with FetchContent or similar | Not the `.venv` of vehicle-sil. |
| A Windows build | Not the `.venv` of vehicle-sil. |

When you change the Python packages, update `uv.lock` with `uv lock` and commit both `pyproject.toml` and `uv.lock`. `uv sync --locked` fails if `uv.lock` does not match `pyproject.toml`.

To check the Python code with the oldest supported version (3.11) and with the version in `.python-version`, run `scripts/verify-python.py` (no C++ build needed). On 3.11, it also runs with exactly the lower bounds of the packages in `pyproject.toml`, so the lower bounds are versions that the tests have run on.

Without uv, you can install the packages listed in `[dependency-groups]` of `pyproject.toml` with pip. On Windows (MSYS2 UCRT64), you can skip uv and keep the Python from the pacman packages below.

## Build vehicle-sil itself

A fresh clone does not have the SIL Kit patches (`patches/sil-kit/`) applied. `cmake --preset …` applies them during the configure step to a copy inside the build directory; `external/sil-kit` stays as upstream has it. The line `vehicle-sil: sil-kit (…) + … patch(es) … -> …` in the configure output shows the commit used, the patches applied and the location of the copy.

### Linux

```sh
git clone --recursive https://github.com/rahanahu/vehicle-sil.git
cd vehicle-sil
scripts/doctor.py                      # prints one line per requirement: is it in place?
uv sync                                # installs the Python packages that the build uses
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug
```

In the 3 commands that name `linux-gcc-debug`, you can replace it with any preset below. "[Build from the repository](../README.md#build-from-the-repository)" in the README uses `linux-gcc-control`.

| Preset | Contents | Additional requirements |
|---|---|---|
| `linux-gcc-debug`, `linux-gcc-release` | GCC | None |
| `linux-clang-debug` | Clang | None |
| `linux-clang-asan`, `linux-clang-tsan` | Clang with AddressSanitizer + UBSan, or with ThreadSanitizer | None |
| `linux-gcc-asan`, `linux-gcc-tsan` | GCC with sanitizers | The packages `libasan`, `libubsan` and `libtsan` |
| `linux-gcc-static` | Release with `VEHICLE_SIL_STATIC=ON` ("Build a single executable (`VEHICLE_SIL_STATIC`)" below) | A static libstdc++ (`libstdc++-static` on Fedora) |
| `linux-gcc-control` | GCC Debug with `VEHICLE_SIL_BUILD_CONTROL_SERVER=ON` | If the computer has no gRPC: CMake 3.28 or later, and Git and a network connection for the first configure |

`linux-gcc-control` also builds the Control Server and its tests (see "Build" in [control-api.md](control-api.md#build)). If CMake does not find gRPC, it pulls in a pinned version with FetchContent and builds it. The tests of the Python client are registered only if a Python with grpcio and grpcio-tools is available (`uv sync`).

`scripts/verify.py --linux` does not include `linux-gcc-static`, `windows-mingw-static` or `linux-gcc-control`. Run them by name.

The tests of the ThreadSanitizer presets suppress only reports known to be false positives. The suppressions are in `cmake/tsan-suppressions.txt`, each with its reason.

`-DVEHICLE_SIL_WARNINGS_AS_ERRORS=ON` turns warnings in vehicle-sil's own targets into errors.

Before you commit, you can check with `scripts/verify.py --linux` (see "Verification and scripts" in [AGENTS.md](../AGENTS.md#verification-and-scripts)).

### Windows (MSYS2 UCRT64)

Build with the GCC of MSYS2 (MinGW-w64, UCRT variant). Work in the MSYS2 "UCRT64" shell: a bash with this GCC, CMake and the other tools in `PATH`.

First, in PowerShell or the Command Prompt, install MSYS2 and open the UCRT64 shell:

```powershell
winget install MSYS2.MSYS2
C:\msys64\msys2_shell.cmd -ucrt64 -defterm -here -no-start
```

The second line opens the UCRT64 shell in the current terminal and the current directory. The "MSYS2 UCRT64" entry in the Start menu opens the same shell. If `echo $MSYSTEM` prints `UCRT64`, you are in the right shell. Run all later commands in this shell.

```sh
pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-yaml
git clone --recursive https://github.com/rahanahu/vehicle-sil.git
cd vehicle-sil
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
ctest --preset windows-mingw-release
```

There are 3 presets: `windows-mingw-debug`, `windows-mingw-release` and `windows-mingw-static` (Release with `VEHICLE_SIL_STATIC=ON`).

Where the executables find the DLLs they load:

| DLL | Location |
|---|---|
| `SilKit.dll` | Copied next to each executable automatically after the build. |
| MinGW runtime DLLs (`libstdc++-6.dll` and others) | Loaded from the `PATH` of the UCRT64 shell. |

To run the executables outside the UCRT64 shell (in PowerShell, for example), add `C:\msys64\ucrt64\bin` to `PATH`, before the `bin` directory of any other MinGW. If a DLL fails to load, the executable exits without printing anything.

| Exit code | Cause |
|---|---|
| `0xC0000135` | A DLL was not found. |
| `0xC0000139` | A DLL from a different GCC version was loaded first. |

## Prebuilt tools

The releases page provides the prebuilt tools as one archive per OS.

| Archive | Built on | Needed to run |
|---|---|---|
| `vehicle-sil-tools-<version>-linux-x86_64.tar.gz` | Ubuntu 22.04 (x86_64) | Only glibc 2.35 or later (the glibc of Ubuntu 22.04 or newer). No other shared libraries. |
| `vehicle-sil-tools-<version>-windows-x86_64.zip` | MinGW of MSYS2 UCRT64 | Single executables with no bundled DLLs (SIL Kit, gRPC and the C++ runtime are linked statically). |

The archive contains the items below. The `README.md` in the archive describes the contents in detail, and [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) lists the bundled components and their licenses.

| Item | Notes |
|---|---|
| `vehicle_sil_launch` | With the Control Server. |
| `vehicle_sil_ethernet_bridge` | The Ethernet bridge ([ethernet-bridge.md](ethernet-bridge.md)), next to `vehicle_sil_launch`, where `vehicle_sil_launch run --connect` finds it. Its `npcap` (Windows) and `pcap` (Linux) backends need Npcap or libpcap, which the archive does not contain: install them yourself. |
| `vehicle_sil_socketcan_bridge` (Linux archive only) | The SocketCAN bridge ([socketcan-bridge.md](socketcan-bridge.md)): a CAN adapter or `vcan0` of this computer. It needs no privileges; `vehicle_sil_launch run --connect CAN=can0` starts it. |
| GUI server and screens | — |
| Python client of the Control API | With the gRPC stubs. |
| GUI client library | A single ES module, and an npm package with types. |
| Template for your own screen (`gui_starter/`) | Use it with `vehicle-sil-gui --web gui_starter simulation.yaml`. |
| JSON Schema of the dashboard | — |
| License notices | — |

The Vector XL CAN Bridge is not in the archive, because it cannot be built without the Vector XL Driver Library (see "[Build](bridges.md#build)" in bridges.md).

To build your own screen for the browser GUI, install `vehicle-sil-gui-client-<version>.tgz` from the releases page with `npm install <its URL or the file>`. You can then use the client library as `@vehicle-sil/gui-client` (see "Build your own screen" in [gui.md](gui.md#build-your-own-screen)). The same file is also in `client/` of the tools archive.

`scripts/package.py` creates the archives.

| Subcommand | What it does |
|---|---|
| `tools` | Creates the tools archive. |
| `verify` | Checks an archive by following the steps a user takes. |
| `template` | Creates the template archive. |

`scripts/package.py --help` prints the arguments. When you push a tag `v<version>`, GitHub Actions creates and checks the archives on both OSes and attaches them to a draft release.

## Use from another project (FetchContent)

Write this in the `CMakeLists.txt` of your CMake project:

```cmake
cmake_minimum_required(VERSION 3.21)
project(example LANGUAGES C CXX)

include(FetchContent)

FetchContent_Declare(
  vehicle_sil
  GIT_REPOSITORY https://github.com/rahanahu/vehicle-sil.git
  GIT_TAG <commit hash to pin>)

FetchContent_MakeAvailable(vehicle_sil)

add_executable(example main.cpp)
target_link_libraries(example PRIVATE vehicle_sil::simulation vehicle_sil::j1939_periodic_sender)   # vehicle_sil::core if you need only core
vehicle_sil_copy_runtime_dlls(example)   # Windows: copies SilKit.dll (does nothing on other OSes)
```

To start a new project, the quickest way is to copy the template [template/](../template), which already contains this (see "[Quick start](../README.md#quick-start)" in the README). [examples/fetchcontent_consumer](../examples/fetchcontent_consumer) is a complete example.

| Item | Behavior when you include vehicle-sil |
|---|---|
| SIL Kit | Fetched and configured automatically as a submodule of vehicle-sil. You do not set it up separately. |
| Patches to SIL Kit | The build uses a copy of the SIL Kit sources in the build tree, with the patches of `patches/sil-kit/` applied ([dependencies.md](dependencies.md)). |
| Git | Used to apply the patches. |
| Libraries you do not link | Not built. |

A minimal `main.cpp`:

```cpp
#include <vehicle_sil/j1939/periodic_sender.hpp>
#include <vehicle_sil/simulation/simulation.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace j1939 = vehicle_sil::j1939;
using namespace std::chrono_literals;

// An ECU is a class that takes Ecu&. The ECU class also holds the model state (here rpm_)
class Engine {
public:
    explicit Engine(vehicle_sil::Ecu& ecu) {
        auto& node = ecu.j1939("J1939_CAN1").add_node(
            {.address = 0x00, .name = j1939::Name{}.set_identity_number(0x2345).set_manufacturer_code(0x7F0)});   // the stack does the Address Claim
        node.attach<j1939::PeriodicSender>(j1939::Pgn{0xFF10}, 100ms,   // sends every 100 ms once it has an address
                                           [this](std::span<std::uint8_t> out) -> std::size_t {
                                               out[0] = static_cast<std::uint8_t>(rpm_ & 0xFF);
                                               out[1] = static_cast<std::uint8_t>(rpm_ >> 8);
                                               return 2;
                                           });
        ecu.every(10ms, [this](vehicle_sil::SimTime) { rpm_ = static_cast<std::uint16_t>(rpm_ + 10); });
    }
private:
    std::uint16_t rpm_ = 0;
};

int main() {
    vehicle_sil::Simulation sim(vehicle_sil::SilKitOptions{});   // SIL Kit virtual time; the registry runs in this process
    sim.add<Engine>("Engine", 10ms);
    sim.run_for(3s);
}
```

For receiving, Request and response, reception monitoring and writing tests, see [j1939-guide.md](j1939-guide.md).

To define signals in YAML and turn them into typed messages, call `vehicle_sil_add_j1939_codec(<target> <definition.yaml>)` and link `<target>`. This needs Python 3.11 or later with PyYAML ([j1939-codec.md](j1939-codec.md)).

### Build a single executable (`VEHICLE_SIL_STATIC`)

To ship an ECU executable without any other files, configure with `-DVEHICLE_SIL_STATIC=ON` (the default is OFF). SIL Kit is then built as a static library, and the executables that link SIL Kit also link the C++ runtime statically.

| What you build | How to set it |
|---|---|
| vehicle-sil itself | `cmake --preset linux-gcc-static` (`windows-mingw-static` on Windows) |
| Your project that includes vehicle-sil with FetchContent | Pass `-DVEHICLE_SIL_STATIC=ON` to the configure step, or write `set(VEHICLE_SIL_STATIC ON)` before `FetchContent_MakeAvailable`. |
| The template project | `cmake --preset linux -DVEHICLE_SIL_STATIC=ON` |

| OS | Files no longer needed | Additional build requirement |
|---|---|---|
| Linux | `libSilKit.so`, `libstdc++`, `libgcc_s` | A static libstdc++ (`libstdc++-static` on Fedora; on Debian and Ubuntu it is in `libstdc++-<version>-dev`) |
| Windows (MinGW) | `SilKit.dll`, `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll` | None (linked with `-static`) |

On Linux, glibc stays a shared library. A program with glibc linked statically cannot load modules such as those for name resolution, which narrows the environments it runs in instead of widening them.

On Windows there are no DLLs left to copy, so `vehicle_sil_copy_runtime_dlls()` does nothing.

The executable grows by the size of SIL Kit.

#### License notices to ship with the executable

An executable linked with `VEHICLE_SIL_STATIC=ON` contains SIL Kit, the code that SIL Kit compiles in, and the C++ runtime. When you distribute it, ship these license notices with it:

| Contained | What to ship | Notes |
|---|---|---|
| vehicle-sil | `LICENSE` (MIT) | The vehicle-sil libraries are static libraries, so ship this with ECU executables built without `VEHICLE_SIL_STATIC` too. |
| SIL Kit | `external/sil-kit/LICENSE` | — |
| Code that SIL Kit compiles in | `external/sil-kit/ThirdParty/LICENSES.rst`, the license files of fmt, spdlog and asio under `external/sil-kit/ThirdParty/`, and the notice inside `external/sil-kit/ThirdParty/rapidyaml/rapidyaml.hpp` | — |
| libstdc++, libgcc (Linux, Windows) | Nothing | The GCC Runtime Library Exception requires no notice for programs compiled with GCC. |
| winpthreads, mingw-w64 C runtime (MinGW on Windows) | The files that MSYS2 installs in `/ucrt64/share/licenses/winpthreads/` and `/ucrt64/share/licenses/crt/` | The C runtime and the startup code are in every MinGW executable, with or without `VEHICLE_SIL_STATIC`. |

The prebuilt tools archive contains the same files. You can ship them as they are.

| Directory in the archive | Contents |
|---|---|
| `licenses/sil-kit/` | All files of the 2 SIL Kit rows in the table above. The notice inside `rapidyaml.hpp` is extracted into `rapidyaml-notices.txt`. |
| `licenses/msys2/` (Windows archive) | The license files of winpthreads and the C runtime that MSYS2 installs. |

## See also

- Commands to try first: "[Quick start](../README.md#quick-start)" in the README
- Verification scripts: [AGENTS.md](../AGENTS.md)
- Test labels and how to run tests: [code-map.md](code-map.md)
- Writing ECUs: [j1939-guide.md](j1939-guide.md)
- SIL Kit pinning, patches, gRPC, how Python is found: [dependencies.md](dependencies.md)
