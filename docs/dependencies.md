# Dependencies

[日本語](ja/dependencies.md)

This page covers the external libraries that vehicle-sil uses: how they are pinned and pulled into the build, the APIs used, the upstream behavior the implementation relies on, and the patches to SIL Kit.

| To find | See |
|---|---|
| The libraries used, how to get them, their licenses | [Libraries used](#libraries-used) |
| How SIL Kit is pulled into the build, how the patches are applied | [How dependencies are pulled in](#how-dependencies-are-pulled-in-cmakevehiclesildependenciescmake), [Targets used](#targets-used) |
| The SIL Kit APIs that vehicle-sil calls | [APIs used](#apis-used) |
| What to check when you update upstream | [Upstream behavior the implementation relies on](#upstream-behavior-the-implementation-relies-on), [SIL Kit versions that can be mixed](#sil-kit-versions-that-can-be-mixed), [Updating the upstream version](#updating-the-upstream-version) |
| Upstream bugs and how vehicle-sil handles them | [Known upstream bugs](#known-upstream-bugs), [Patches to SIL Kit](#patches-to-sil-kit) |

## Libraries used

vehicle-sil includes the upstream library SIL Kit as a Git submodule, pinned to a specific commit. SIL Kit is built with patches applied (see "Patches to SIL Kit"). The work tree of the submodule is not modified. The J1939 stack is vehicle-sil's own (`vehicle_sil::j1939`) and uses no external library.

| Library | Path | Pinned commit | Origin | License |
|---|---|---|---|---|
| Vector SIL Kit | `external/sil-kit` | `52ad61d5399483c395585d5feee89d2e89fc74b1` | Tag `v5.0.8` | MIT |

SIL Kit has submodules of its own (`ThirdParty/asio`, `fmt`, `spdlog`, `googletest`), so you need `git submodule update --init --recursive`. FetchContent does this automatically.

SIL Kit itself is built as C++17. Its C++ API is header-only and wraps the C API, so consumers can compile with C++20 without problems. Upstream supports MinGW-w64 (`-Wa,-mbig-obj` and other settings).

### gRPC and Protocol Buffers (Control Server, optional)

Used only with `VEHICLE_SIL_BUILD_CONTROL_SERVER=ON` (default OFF). Only `vehicle_sil::control_grpc`, the code generated from `proto/` and `vehicle_sil_launch` (`run --control`) use them. The other libraries (including `vehicle_sil::launch`) and the ECU executables do not.

| Case | gRPC used |
|---|---|
| `find_package(gRPC CONFIG)` finds gRPC | The one found (on MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-grpc`). |
| Not found | gRPC is pulled in with FetchContent (`grpc/grpc` on GitHub, tag `v1.76.0`, which `VEHICLE_SIL_GRPC_VERSION` overrides; of its submodules, only abseil, protobuf, re2, c-ares, BoringSSL and zlib). |

The pulled-in gRPC gets no install rules (only the export settings of abseil are enabled) and is excluded from warnings (`SYSTEM`). Pulling it in with FetchContent needs CMake 3.28 or later. With an older CMake, install gRPC and let `find_package` find it. To reuse downloaded sources, pass `-DFETCHCONTENT_SOURCE_DIR_GRPC=<path>`.

| Library | License |
|---|---|
| gRPC | Apache-2.0 |
| Protocol Buffers | BSD-3-Clause |
| abseil | Apache-2.0 |
| re2 | BSD-3-Clause |
| c-ares | MIT |
| BoringSSL | Apache-2.0 (some parts keep older OpenSSL and ISC terms) |
| zlib | zlib License |

The vehicle-sil repository does not contain these libraries.

The Python client (`python/vehicle_sil_control`) uses `grpcio` and `protobuf`, and `grpcio-tools` to generate the stubs (the `control` group in `pyproject.toml`; without uv, install them with pip; on MSYS2, `mingw-w64-ucrt-x86_64-python-grpcio` and `-python-grpcio-tools`). Its tests are registered only if a Python 3.11 or later with these packages is available (`VEHICLE_SIL_CONTROL_PYTHON`; if that is not set, in the order of "How Python is found" below).

### How Python is found

These files at the repository root pin the Python version and the Python packages: `pyproject.toml` (`requires-python` and `[dependency-groups]`), `.python-version` (the version used for development) and `uv.lock` (the resolved versions).

| Group | Packages |
|---|---|
| `codegen` | PyYAML |
| `control` | grpcio, grpcio-tools, protobuf |
| `gui` | The GUI server (`gui/server`, a workspace member, with grpcio and protobuf for reading and writing variables) and pytest |
| `schema` | jsonschema and PyYAML (tests of the JSON Schemas in `schema/`) |
| `dev` | Everything (default) |

Each lower bound is the oldest version that the tests actually run on: `scripts/verify-python.py` installs exactly the lower bounds on Python 3.11 with uv's lowest-direct resolution and checks them. There are no upper bounds.

When vehicle-sil is the top-level project, the configure step looks for Python in this order:

1. If the repository's `.venv/bin/python` (created by `uv sync`) is Python 3.11 or later and can import the required modules, it uses that.
2. Otherwise, it searches with `find_package(Python3 3.11)`. An activated virtual environment comes first. You can name a Python with `-DPython3_EXECUTABLE=`.
3. If the Python found lacks the modules, it tries `python3` and `python` from `PATH`, in `PATH` order. It skips this step if you named a Python with `-DPython3_EXECUTABLE=`.

On Windows, `find_package(Python3)` finds Pythons registered in the Windows registry (installed for Windows) before those in `PATH`, so the MSYS2 Python (the one with `mingw-w64-ucrt-x86_64-python-yaml` and the other packages installed) is found through this order. The logic is in `cmake/VehicleSilPython.cmake`.

| Case | Behavior |
|---|---|
| You created `.venv` with `uv sync`, or added packages to `.venv` | The generator's Python is searched for again at every configure, so a normal reconfigure picks it up (no `--fresh` needed). |
| A consumer's build that includes vehicle-sil with FetchContent or `add_subdirectory` | The `.venv` of vehicle-sil is not used. The consumer's Python is used (an activated virtual environment, `Python3_EXECUTABLE`, `PATH`). |
| Windows | `.venv` is not a candidate (using uv's Python from a MinGW build is not verified). The Python from the MSYS2 UCRT64 pacman packages is found and used as before. |

### Python 3 and PyYAML (J1939 typed message generation)

`vehicle_sil_add_j1939_codec()` runs `tools/j1939_codegen/j1939_codegen.py` at build time. It needs Python 3.11 or later with PyYAML (`import yaml`). The configure step searches in the order above and checks that PyYAML can be imported. If it cannot, the configure step fails at the call of this function. The message lists each Python it tried with the reason it was not used (`No module named 'yaml'`, a version older than 3.11 and so on), and how to install PyYAML (uv, pip, OS and MSYS2 packages). When the configure step skips the Control API tests, it also prints the Pythons it tried and the reasons.

The tests and examples of vehicle-sil (`VEHICLE_SIL_BUILD_TESTS`, `VEHICLE_SIL_BUILD_EXAMPLES`; ON by default in a top-level build) use this function, so that build needs Python and PyYAML. A consumer that uses only the libraries (through FetchContent or similar) does not need them unless it calls the function.

PyYAML reads YAML 1.1. How the generator reads it:

| What you write | How the generator treats it |
|---|---|
| `on`, `off`, `yes`, `no` | Read as strings, not as booleans. |
| Integers | Only decimal (no leading 0), hexadecimal and binary are accepted. Octal, and sexagesimal integers and floats, are errors. |
| Duplicate keys | Error. Overriding a key brought in with a merge key is allowed. |

A configure result in which Python or PyYAML was not found is not kept in the cache. After you install them, reconfigure; the configure step then finds them.

### aiohttp and PyYAML (browser GUI server)

The GUI server (`gui/server`, distribution name `vehicle-sil-gui`, [gui.md](gui.md)) depends on aiohttp and PyYAML (for reading dashboard files). aiohttp provides an HTTP server, WebSocket and the client that the tests use in one package. It runs on the default asyncio event loop on Windows (ProactorEventLoop, which can run child processes), and has wheels for Windows (3.11 and later). It needs no C++ build.

The GUI server uses grpcio and protobuf only to read and write ECU variables, as a client of the launcher's Control Server (the optional dependency `vehicle-sil-gui[variables]`, which the `gui` group installs). Without them, the GUI server still runs, but variables are unavailable and it says why. The optional dependency alone is not enough: the stubs are needed too. The GUI server assumes that you run it in the repository after `uv sync`, or use the prebuilt tools, which bundle the stubs; the Python package does not contain them. It generates the gRPC stubs from `proto/` with grpcio-tools (the `control` group) at every start, or uses the stubs that `python -m vehicle_sil_control.generate` created, or those in the directory that `VEHICLE_SIL_CONTROL_STUBS` names.

| Library | License |
|---|---|
| aiohttp | Apache-2.0 (the bundled llhttp is MIT) |
| multidict, yarl, frozenlist, aiosignal and propcache, which aiohttp depends on | Apache-2.0 |
| aiohappyeyeballs | PSF-2.0 |
| attrs | MIT |
| idna | BSD-3-Clause |

The vehicle-sil repository does not contain these libraries; uv installs them.

### Vector XL Driver Library (Vector XL CAN Bridge, Windows only)

vehicle-sil does not include the XL Driver Library, not even as a submodule. Get it from Vector and set its location with `VEHICLE_SIL_VECTOR_XL_DIR`. The files used are `vxlapi.h`, `vxlapi64.lib` (`vxlapi.lib` for 32-bit) and, at run time, `vxlapi64.dll`. If they are not found, the bridge (`vehicle_sil::vector_xl` and `vehicle_sil_vector_xl_bridge`) is not built.

The APIs used are those for classic CAN (`XL_INTERFACE_VERSION`, `XL_BUS_TYPE_CAN`).

| Used for | API |
|---|---|
| Driver | `xlOpenDriver`, `xlCloseDriver`, `xlGetDriverConfig`, `xlGetErrorString` |
| Port | `xlOpenPort`, `xlClosePort`, `xlActivateChannel` (`XL_ACTIVATE_NONE`), `xlDeactivateChannel` |
| Configuration | `xlCanSetChannelBitrate` (needs init access; 15000–1000000 bit/s), `xlCanSetChannelMode(tx=1, txrq=0)` (enables transmit receipts and disables transmit request notifications; needs init access; called only with init access) |
| Transmission and reception | `xlCanTransmit` (`XL_TRANSMIT_MSG`, DLC 0–8), `xlReceive` (`XL_RECEIVE_MSG`, one event per call), `xlSetNotification` and `WaitForSingleObject` |

Only `bridges/vector-xl/src/xl_backend_vector.cpp` calls these APIs. This file implements an internal interface of vehicle-sil (`src/xl_backend.hpp`; its names, types and values are vehicle-sil's own) with calls into the XL Driver Library. It only converts types and values; it makes no decisions and holds no state. It is built only on Windows, and only if the XL Driver Library is found.

The decisions and state of the bridge (classification of events, handling of overruns, the receive thread, the order of opening and closing, cleanup, construction of errors) are in `src/xl_driver.cpp`. It depends only on the internal interface, so it builds on every platform.

The tests link `src/xl_driver.cpp` with a test-only fake that implements the internal interface (`tests/vector_xl/fake_xl/`). They use neither the Vector headers nor the Vector library. The repository contains no Vector headers and no copies of them.

### Linux kernel APIs (Ethernet Bridge `tap` and `packet`, Linux only)

No external library is used. Only kernel and libc headers are used, so nothing needs to be installed to build or to run. Only `libs/eth_link/src/linux` calls these APIs (part of `vehicle_sil::eth_link`, built only on Linux).

| Used for | API |
|---|---|
| `tap` | Opens `/dev/net/tun`, `ioctl(TUNSETIFF)` (`IFF_TAP`, `IFF_NO_PI`, and also `IFF_TUN_EXCL` with `--tap-create`), `read` and `write` of one frame per call |
| `packet` | `socket(AF_PACKET, SOCK_RAW)` bound to the interface with `bind` (`ETH_P_ALL`), `recvmsg` and `send`, `PACKET_AUXDATA` (stripped VLAN tags), `PACKET_MR_PROMISC` with `PACKET_ADD_MEMBERSHIP`, `PACKET_STATISTICS` (number of frames dropped) |
| Interfaces | `if_nameindex`, `if_nametoindex`, `ioctl(SIOCGIFFLAGS)`, `ioctl(SIOCGIFHWADDR)` (link type) |
| Receive thread | `poll`, `eventfd` (stop signal) |

The kernel behavior the implementation relies on (the tests `ethernet_bridge.netns.*` verify it):

- A frame written to a TAP device does not come back to the reads of the writer.
- A packet socket does not receive the frames it sent itself, but does receive frames that other sockets on the same interface sent.
- The outer VLAN tag of a received frame is removed from the frame bytes and passed in `PACKET_AUXDATA`.

One assumption is not tested. When a frame is sent to a group address, the kernel passes a copy back for the receivers on the same computer (`PACKET_LOOPBACK`). That copy duplicates the copy taken at transmission, so `packet` drops it.

### Npcap and libpcap (Ethernet Bridge `npcap` and `pcap`, run time only)

vehicle-sil includes neither library: not in the repository, not as a submodule and not in the release assets. The build does not use them either (no headers or import libraries are needed). At run time, the bridge loads the library that the user installed on the computer. If it is not installed, the bridge prints how to install it and exits (see "`npcap` and `pcap`" in [ethernet-bridge.md](ethernet-bridge.md#npcap-and-pcap)).

| OS | Library | License |
|---|---|---|
| Windows | [Npcap](https://npcap.com) | The Npcap license sets the permitted use and the conditions for redistribution. The vehicle-sil documentation does not interpret it. |
| Linux | [libpcap](https://www.tcpdump.org) of the distribution | BSD license |

The repository contains neither library's headers nor copies of them. vehicle-sil declares the function types, the 3 structures it reads and the result codes itself, following the published function signatures (`libs/eth_link/src/pcap/pcap_api.hpp`; the names are vehicle-sil's own).

Only `libs/eth_link/src/pcap/pcap_loader_win.cpp` (`LoadLibraryExW`, `GetProcAddress`; the DLLs that the library uses are searched for only in the library's directory and in the system directory) and `pcap_loader_posix.cpp` (`dlopen`, `dlsym`) load the library and get the function addresses. These files also contain how to wait (on Windows, the library's event and `WaitForMultipleObjects`; on Linux, the library's descriptor and `poll`) and the connection names (`GetAdaptersAddresses` on Windows; links `iphlpapi`).

The decisions and state of the pcap interface (order of opening, the receive thread, error texts, how an interface is chosen) are in `src/pcap/pcap_link.cpp`. It depends only on the function table, so it builds on every platform.

| Used for | Library functions |
|---|---|
| Listing | `pcap_findalldevs`, `pcap_freealldevs`, `pcap_lib_version` |
| Opening | `pcap_create`, `pcap_set_snaplen`, `pcap_set_promisc`, `pcap_set_timeout`, `pcap_set_immediate_mode`, `pcap_activate`, `pcap_datalink`, `pcap_setnonblock`, `pcap_close`, and with Npcap also `pcap_open` (adapters are opened with it, because only this function has the option not to receive the handle's own transmissions; libpcap is not required to have it) |
| Transmission and reception | `pcap_next_ex`, `pcap_sendpacket`, `pcap_geterr`, `pcap_stats` |
| Waiting | `pcap_getevent` on Windows, `pcap_get_selectable_fd` on Linux |

If any one of them is missing, the bridge stops before opening and prints the library version and the names of the missing functions.

The tests load a test-only fake shared library with functions of the same names (`tests/eth_link/fake_pcap`, built from vehicle-sil's declarations) through the same path as the real one. The Linux test `ethernet_bridge.netns.pcap` verifies the bridge against the real libpcap (it is skipped on computers without libpcap). There is no test against the real Npcap.

The libpcap (Linux) behavior the implementation relies on (`ethernet_bridge.netns.pcap` verifies it):

- A sent frame does not come back to the receive side of the handle that sent it.
- The VLAN tag of a received frame is put back in its original position.
- With non-blocking reads, the library reports neither that the interface went down nor that it disappeared (the bridge asks the kernel).

The Npcap behavior the implementation relies on (verified by hand against the real Npcap; for the scope, see "Verified and not verified" in [ethernet-bridge.md](ethernet-bridge.md#verified-and-not-verified)):

- The structure layouts match.
- The library's event (`pcap_getevent`) is reset by the library once every received frame has been read (vehicle-sil never resets it).
- A sent frame comes back to the receive side of the handle that sent it.
- It does not come back to a handle opened by `pcap_open` with `PCAP_OPENFLAG_NOCAPTURE_LOCAL`, and that handle still receives frames that other handles sent.

The pcap interface opens adapters with this flag and declares that it does not echo its own transmissions. vehicle-sil declares the flag values passed to `pcap_open` (promiscuous, no capture of the handle's own transmissions, immediate delivery) itself.

These 2 assumptions are not verified:

- That the number of dropped frames in `pcap_stats` is cumulative since the handle was opened.
- That the "immediate delivery" flag of Npcap's `pcap_open` delivers frames as fast as `pcap_set_immediate_mode`.

## How dependencies are pulled in (`cmake/VehicleSilDependencies.cmake`)

Dependencies are added with `add_subdirectory(... EXCLUDE_FROM_ALL)`. Only linked targets are built.

### Copying and patching SIL Kit

The submodule `external/sil-kit` is copied into `external/sil-kit-patched/` in the build tree (nested submodules included, `.git` excluded). Then `patches/sil-kit/*.patch` are applied with `git apply` in file-name order, and the copy is added to the build. If there are no patches, the submodule is added directly, without a copy.

The copy and the patching are redone only if the contents of the patches or of the submodule files (`.git` excluded) differ from those of the last configure. The check uses a stamp that lists the SHA-256 of every file (`sil-kit-patched.stamp`). It does not look at the Git state, so it also works when the submodule is not a Git checkout. When the copy is redone, every copied file is touched, so the whole library is rebuilt.

When you add, change or remove a patch, or change submodule files (switching the checkout or editing locally), CMake reruns the configure step automatically.

The Git hash in the SIL Kit version information (`SilKit::Version::GitHash()`) is `UNKNOWN`. SIL Kit looks for its own `.git/HEAD`, but the `.git` of a submodule is a file, so the result would be the same without the copy.

If a patch does not apply, the configure step fails. Patches are applied with `git apply`, so Git is required. The parent Git repository and inherited environment variables such as `GIT_DIR` have no effect. Even if the build tree is inside a repository, `GIT_CEILING_DIRECTORIES` hides the repositories above the copy from Git. The value is passed in the path form that Git itself prints (the `git` of MSYS2 reads only the `/d/a/...` form).

The failure message shows the path and version of the Git used, the value of `GIT_CEILING_DIRECTORIES`, the repository that Git found around the copy (if there is one, Git took the patch paths as relative to that repository and skipped them), and the line endings (LF or CRLF) of the files that the patch changes.

Patches are applied with `--ignore-whitespace`, so they also apply to a submodule checked out with CRLF (`core.autocrlf` on Windows). `.gitattributes` excludes the patch files themselves from line-ending conversion.

### When the consumer already defines `SilKit::SilKit`

If the consumer already defines `SilKit::SilKit`, SIL Kit is not added. Only v5.0.8 is verified. Older SIL Kit versions lack some of the behavior that vehicle-sil relies on (see "[SIL Kit versions that can be mixed](#sil-kit-versions-that-can-be-mixed)").

The consumer's SIL Kit does not have the patches of "Patches to SIL Kit" applied. A patch takes effect only if it is applied to the SIL Kit of the process in the table below. The same applies when another tool (CANoe, the `sil-kit-registry` that ships with SIL Kit and others) plays the role of that process.

| Patch | Process that needs it | Without it |
|---|---|---|
| 0001 | The LIN master that sends the headers | In a setup where several `LinSlave`s take turns responding to the same LIN ID (diagnostic 0x3D, for example), headers for that ID are no longer sent after the second response. |
| 0002 | Participants with CAN controllers | Where a controller is called from outside the SIL Kit threads (closing, transmission on a real-time port, the Vector XL CAN Bridge), a data race occurs when the call overlaps with the network simulator joining or leaving (rare). |
| 0003 | Registry | After a participant refused for its name disconnects, participants that cannot connect directly to the earlier participant with that name can no longer connect to it through the relay. |
| 0004 | The participant that creates the network simulator (`vehicle_sil_can_bus_simulator`) | Rarely, the network is not announced and all controllers stay in trivial simulation. The bit rate and the mode do not reach the simulator, with no error and no warning. |
| 0005 | Registry | When a participant joins right after another one ended, it rarely fails after waiting 5 seconds (`Middleware.ConnectTimeoutSeconds`) with "Timeout during connection handshake with the SIL Kit Registry". Creating the participant again connects. |
| 0006 | Coordinated participants (the ECUs of a run) | A participant that the run does not require (a mistyped `--name`, for example) can leave Error again during the start. `ParticipantRuntime::start()` still fails for it, but SIL Kit may not take its stop, so it can stay for up to `shutdown_timeout` and hold the virtual time of all participants at 0 meanwhile. Rarely, `start()` returns and the participant then runs no step and reports nothing. |

The registry that `vehicle_sil_launch run` starts, and the executables built with vehicle-sil, have all patches applied.

### SIL Kit build settings

The public include directories of upstream are marked SYSTEM, so that warnings in upstream headers do not make vehicle-sil targets fail in builds with `-Werror`.

Upstream options are overridden with normal variables in function scope (CMP0077). The consumer's cache is not changed.

| Library | Setting | Reason |
|---|---|---|
| SIL Kit | `SILKIT_BUILD_DEMOS/TESTS/DOCS/DASHBOARD=OFF` | Skips what is not needed. |
| SIL Kit | `SILKIT_BUILD_UTILITIES=ON` | The integration tests need `sil-kit-registry`. |
| SIL Kit | `SILKIT_BUILD_STATIC` equals `VEHICLE_SIL_STATIC` (default OFF) | Makes SIL Kit a static library with `VEHICLE_SIL_STATIC=ON` (see the next paragraph). |

With `VEHICLE_SIL_STATIC=ON`, SIL Kit becomes a static library, and static linking of the C++ runtime (Linux: `-static-libstdc++ -static-libgcc`; MinGW: `-static`) is added to `INTERFACE_LINK_OPTIONS` of `SilKit::SilKit`. This applies to every executable that links SIL Kit, the consumer's ECUs included. glibc stays a shared library (see "Build a single executable (`VEHICLE_SIL_STATIC`)" in [build.md](build.md#build-a-single-executable-vehicle_sil_static)).

## Targets used

| Target | Description |
|---|---|
| `SilKit::SilKit` | SIL Kit itself. A shared library by default (static with `VEHICLE_SIL_STATIC=ON`). In Debug it is `libSilKitd.so` / `SilKitd.dll`. The output directory is `${CMAKE_BINARY_DIR}/$<CONFIG>`. |
| `sil-kit-registry` | The registry executable. It links SIL Kit statically, so it needs no shared library. |
| `rapidyaml` | The YAML/JSON parser that SIL Kit builds from `ThirdParty/rapidyaml` (MIT; static library). `vehicle_sil::can_netsim` and `vehicle_sil::launch` use it to read configuration files, and `vehicle_sil::core` uses it to add vehicle-sil's defaults to the SIL Kit configuration it is given (`merged_participant_configuration()`). |

When SIL Kit is not added from the submodule (the consumer already defines `SilKit::SilKit`), there is no `rapidyaml` target, so `vehicle_sil::can_netsim`, `vehicle_sil_can_bus_simulator`, `vehicle_sil::launch` and `vehicle_sil_launch` are not created. `vehicle_sil::core` is created, but uses the SIL Kit configuration it is given as it is: it adds no defaults and does not check the name, the registry URI or `DynamicSimulationStep` (see "SIL Kit configuration defaults" in [internals/core.md](internals/core.md#sil-kit-configuration-defaults)).

On Windows, `SilKit.dll` is written to a different directory from the executables. Call `vehicle_sil_copy_runtime_dlls(<target>)` to copy the DLL next to the executable after the build.

## APIs used

| Library | Used for | API |
|---|---|---|
| SIL Kit | Joining | `Config::ParticipantConfigurationFromString`, `CreateParticipant(config, name, registryUri)` |
| SIL Kit | Configuration keys | `Middleware.AcceptorUris`, `Middleware.EnableDomainSockets`, `Experimental.TimeSynchronization.DynamicSimulationStep` (experimental; always `false` in the default configuration) |
| SIL Kit | Lifecycle | `CreateLifecycleService({OperationMode::Autonomous / Coordinated})`, `StartLifecycle`, `Stop`, `State`, `ReportError`, `SetStartingHandler`, `SetCommunicationReadyHandler` (where controllers are started; `ParticipantRuntime::add_communication_ready_handler()`), `SetCommunicationReadyHandlerAsync` and `CompleteCommunicationReadyHandlerAsync` (the start window; `SimulationController::set_start_window()`) |
| SIL Kit | Time synchronization | `ILifecycleService::CreateTimeSyncService`, `ITimeSyncService::SetSimulationStepHandlerAsync` and `CompleteSimulationStep` (completed at the end of the handler) |
| SIL Kit | System controller (experimental) | `Experimental::Participant::CreateSystemController`, `SetWorkflowConfiguration`, `AbortSimulation` |
| SIL Kit | LIN (`vehicle_sil::lin_silkit`) | `CreateLinController`, `ILinController::Init`, `SetFrameResponse`, `SendFrame` (frames the master sends), `SendFrameHeader`, `GoToSleep`, `GoToSleepInternal`, `Wakeup`, `WakeupInternal`, `AddFrameStatusHandler`, `AddGoToSleepHandler`, `AddWakeupHandler` |
| SIL Kit | LIN dynamic response (experimental) | `Experimental::Services::Lin::InitDynamic`, `AddFrameHeaderHandler` (registered after `InitDynamic`), `SendDynamicResponse` |
| SIL Kit | Publish/subscribe (`vehicle_sil::control_silkit`, examples/virtual_time) | `CreateDataPublisher` (history 0; history 1 only for hello), `CreateDataSubscriber`, `IDataPublisher::Publish`, `PubSubSpec` (topic and media type; labels are not used) |
| SIL Kit | Monitoring other participants | `CreateSystemMonitor`, `AddParticipantStatusHandler` (`SetParticipantConnectedHandler` and `SetParticipantDisconnectedHandler` are not used; see "Known upstream bugs") |
| SIL Kit | CAN | `CreateCanController`, `ICanController::Start/Stop/SendFrame/SetBaudRate` (`SetBaudRate` and `Start` from the CommunicationReady handler), `AddFrameHandler`, `AddFrameTransmitHandler`, `AddErrorStateChangeHandler` (notifications come only when a network simulator is present), the transmission time in the userContext of `SendFrame` (an agreement with the simple CAN Network Simulator) |
| SIL Kit | Ethernet (`vehicle_sil::eth_silkit`; `Ecu::ethernet()` of `vehicle_sil::peer` creates the controller) | `CreateEthernetController`, `IEthernetController::Activate/Deactivate/SendFrame` (`Activate` from the CommunicationReady handler), `AddFrameHandler` (RX only), `AddFrameTransmitHandler`, `AddStateChangeHandler` |
| SIL Kit | Ethernet (`vehicle_sil::ethernet_bridge_core`) | `CreateEthernetController`, `IEthernetController::Activate/Deactivate/SendFrame` (`Activate` from the CommunicationReady handler), `AddFrameHandler` (RX direction only), `AddFrameTransmitHandler` (only acknowledgements other than `Transmitted`) |
| SIL Kit | Network Simulator (experimental, `vehicle_sil::can_netsim`) | `Experimental::Participant::CreateNetworkSimulator`, `INetworkSimulator::SimulateNetwork/Start`, `ISimulatedNetwork`, `Can::ISimulatedCanController`, `Can::ICanEventProducer::Produce` (frames, transmit acknowledgements, state changes) |
| SIL Kit | In-process registry (`Simulation`, `vehicle_sil_launch run`) | `Vendor::Vector::CreateSilKitRegistry`, `StartListening` |

## Upstream behavior the implementation relies on

When you update upstream, check that the behavior below has not changed. All of it is SIL Kit behavior.

### Participants and lifecycle

`ParticipantStatus` is published with a history length of 1. A participant that joins later also receives the latest status of the existing participants. `ParticipantRuntime::wait_for_peer` relies on this.

Frames sent before a subscription is established may not be received. Controllers are therefore created before `ParticipantRuntime::start()`.

The SIL Kit documentation recommends making the first controller settings (CAN `SetBaudRate` and `Start`, LIN `Init`, Ethernet `Activate`) in the CommunicationReady handler. The ports and bridges of vehicle-sil make them there (`ParticipantRuntime::add_communication_ready_handler()`). In both coordinated and autonomous mode, this handler is called on the participant's communication thread before Running (the first step); for an autonomous start too, `StartLifecycle()` queues the start to the communication thread. For a coordinated participant, the handler runs after all required participants have created their services, so the networks that a network simulator simulates have been discovered as well. An autonomous participant has no such guarantee. The tests `core.communication_ready_handlers_that_were_added_run_in_order_before_the_one_that_was_set_and_the_first_step` (thread and order) and `can_netsim.simulator_learns_the_bit_rate_and_the_start_whichever_joins_first` (the simulator receives the settings whatever the start order) verify this.

`sil-kit-registry --listen-uri silkit://127.0.0.1:0 --generate-configuration <file>` listens on a free port and atomically writes a configuration file that contains the actual URI. The integration tests use this so that ports do not conflict when tests run in parallel.

When the registry refuses a participant because its name is in use, it returns an error with the text "A participant with the same name ... already exists in the simulation". `create_participant()` waits and retries only if the error text contains "already exists in the simulation" (SIL Kit has no public API to tell error kinds apart). How the registry refuses, and with which text, differs between registry versions, so with registries of other versions (started by other tools, or from the official release) the text may not match. In that case, `create_participant()` does not wait and fails at once with the SIL Kit error (`name_in_use_wait` does not help; to restart right away with the same name, leave a short gap). Retrying after a refusal also relies on the registry refusing the same name every time (4.0.55 and later). Older registries do not refuse the second connection, so two participants with the same name silently join (from the change log; not tested with old versions). When you update upstream, check that this text has not changed (tests `core.participant_runtime_waits_while_the_registry_still_has_the_name` and `core.participant_runtime_says_how_long_the_name_was_refused`).

A participant that went into Error through `ReportError` stays in Error until `Stop` or an abort, and the future of `StartLifecycle` does not complete. `Stop` moves it from Error to Shutdown. The public headers and the documentation allow `Stop` in Running (or Paused); `Stop` from Error is outside the guarantee (about Error, they say only that the participant can only be ended). `ParticipantRuntime::stop()` calls `Stop` in Error too, so if upstream starts to check this precondition, participants in Error will no longer reach Shutdown. Check this when you update upstream (test `core.virtual_time_stop_during_a_failing_step_leaves_through_error`).

A workflow configuration reaches only the participants connected at the moment it is sent. `SimulationController` therefore sends it after the status of the required participants is visible (the same approach as `sil-kit-system-controller`).

### Time synchronization

A time-synchronized participant advances only when its own next step time is less than or equal to the next step time of every other participant (`TimeConfiguration::OtherParticipantHasLowerTimepoint`). "Wait for the slowest peer" and the stop_at guarantee of `SimulationController` rest on this.

The configuration key `Experimental.TimeSynchronization.DynamicSimulationStep` has 3 states. A participant with `true` shrinks its step to the smallest step size of all participants and asks the other participants to do the same. A participant without the key shrinks along if at least one participant has `true`. A participant with `false` does not shrink. The steps of vehicle-sil assume a fixed size (the step grid, `TickScheduler`, the time gate), so the default configuration (`default_participant_configuration()`) always sets `false`. For a participant given its own configuration, vehicle-sil adds `false` if the key is missing, and reports an error before it creates the participant if the key is `true` (`merged_participant_configuration()`). If SIL Kit shrinks the step anyway (in a build without rapidyaml, given a configuration without the key or with `true`), `ParticipantRuntime` detects that the step size it receives differs from its own and fails that step (see "SIL Kit configuration defaults" in [internals/core.md](internals/core.md#sil-kit-configuration-defaults)). The key is experimental, so when you update upstream, check that its name and meaning have not changed. The test `core.virtual_time_keeps_its_step_when_another_participant_asks_for_dynamic_steps` runs next to a participant with `true` and checks that the default configuration is effective (it fails if the key disappears or its meaning changes). `core.virtual_time_keeps_its_step_with_a_configuration_of_its_own` checks the same for a configuration of the participant's own without the key.

The time gate stops virtual time by not completing the asynchronous step ([internals/virtual-time.md](internals/virtual-time.md)). The SIL Kit HealthCheck (the `HealthCheck` configuration) measures only the time spent in calls to the step handler, so it does not time out a pending step. The official documentation guarantees only the behavior during `Pause()`; it says nothing about an asynchronous step left uncompleted. If this changes, a pause turns into a warning or an Error in configurations with HealthCheck.

A message sent by a time-synchronized participant carries the time of the step it was sent in as its timestamp.

A message sent by a participant without time synchronization carries the timestamp `std::chrono::nanoseconds::min()` (the default of `TimeProvider`). The SIL Kit of a time-synchronized receiver replaces this value in received messages with its own current time (`SilKitLink::DistributeRemoteSilKitMessage`). Before the first step, the current time is also this value, so the value stays. `SilKitCanPort` (virtual time) and `SilKitEthPort` recognize negative times and move them to the time of the receiver's latest step (0 before the first step) (see "SIL Kit port (`SilKitCanPort`)" in [internals/can.md](internals/can.md#sil-kit-port-silkitcanport) and "Senders outside time synchronization" in [internals/ethernet.md](internals/ethernet.md#senders-outside-time-synchronization)). The tests `j1939_silkit.j1939_on_virtual_time_receives_a_bam_from_a_participant_without_time_synchronisation*` check both before and after the start.

### Publish/subscribe (between Agent and Hub)

The layer that carries messages between Agent and Hub over publish/subscribe (`vehicle_sil::control_silkit`), and the start window, rely on the behavior below. The tests `control_silkit.*` and `launch_control.*` verify it as a whole: if any of it changes, `HubTransportStats::late_messages` is no longer 0, or writes made in the window miss the step at time 0.

`IDataPublisher::Publish` copies the data on the calling thread and only queues the transmission to the participant's I/O thread. It is safe to call from any thread, and messages reach each receiver in the order they were queued.

Step completion (`CompleteSimulationStep`) goes into the same queue. Messages sent within a step therefore reach the receiver before the notification of that step's next time. Together with the advance condition in "Time synchronization" above: when the receiver's step P starts, all messages from the sender's steps that started before P have arrived (for the sender's step at exactly P, this is not determined). Messages sent outside a step (from another thread or a receive handler) have no such relation.

Receive handlers are called on the participant's I/O thread, one at a time, in arrival order. The step handler runs on the same thread. While the time gate holds a step, the I/O thread is free and messages still arrive.

`CreateDataPublisher` hands the registration of the publisher to the participant's I/O thread and waits for it, so it cannot be called on that thread (from a receive handler or the step handler): it would wait for itself. `HubTransport` therefore makes all its publishers in its constructor.

Labels (documented): a label of kind Optional matches a counterpart without that key, and one with the same value; it does not match another value, and a publisher and a subscriber that do not match do not communicate. The correctness of the Hub's command publishers per group of Agents relies on this: an Agent receives its group's publisher and the one without a label (tests `control_silkit.a_command_reaches_the_agents_of_its_group_only_and_in_the_order_written`, `control_silkit.a_last_word_is_answered_also_for_a_session_that_has_ended_or_was_never_connected`).

Sending (read in the implementation, not documented): each publisher has a link of its own, and a message goes only to the participants whose subscribers matched that publisher; the others receive nothing at all. Only the speed of the groups relies on this: if SIL Kit sent to every participant and filtered on the receiving side, the commands would still arrive, as before the groups.

The last message sent by a publisher with history 1 also reaches receivers that subscribe later (while the publisher's participant is connected). A hello sent before the participant starts arrives too.

The CommunicationReady handler of a coordinated participant is called after all required participants are ServicesCreated, replies from all of them have arrived, and its own publish/subscribe subscriptions are confirmed. Other participants may still be in CommunicationInitializing at that point; they can receive commands. A publisher that is notified of a subscription sends its history (hello) before it confirms, so a hello that a required ECU sent before it started has arrived before the window opens. With the asynchronous handler, no participant advances to ReadyToRun and no step starts until `CompleteCommunicationReadyHandlerAsync` is called. Publish/subscribe messages are delivered during that time. A call of `CompleteCommunicationReadyHandlerAsync` before the handler has been called is ignored (`ParticipantRuntime` passes it on only after the handler has been called).

If a participant with a started lifecycle disconnects without Shutdown, the other participants receive its state as Error (`HubTransport` uses this to end the connection of an Agent that has gone).

The implementation assumes that `EnableMessageAggregation` in the participant configuration stays at its default, Off. A participant with it On buffers data messages before it sends them, which changes the ordering assumptions above.

When a participant is destroyed, transmissions not yet written to the socket are dropped (if an earlier transmission to the same receiver is still being written, the transmissions queued after it are not written). The Agent's last word therefore waits for the Hub's reply before it destroys the participant (destructor of `AgentTransport`; "End of a connection" in internals/control.md). While the participant lives, queued transmissions are written in order.

### Threading and ordering guarantees

This section separates what the official documentation guarantees about threads and ordering from what vehicle-sil relies on after reading the implementation.

**Guaranteed**: Receive handlers and the step handler are called on one communication thread of the participant, and no other handler is called while the step handler runs. Messages sent within a blocking step handler are delivered when the handler ends. Messages are sent in the order the API is called, whichever thread or handler calls it (change log of 4.0.24; the order between calls made concurrently from different threads is not guaranteed).

**Not guaranteed (relied on after reading the implementation)**: That with the asynchronous step (`CompleteSimulationStep`) too, transmissions within the step go out before the completion notification (the completion notification passes through the same queue as the transmissions). That `Publish` and `SendFrame` may be called from any thread (the ordering guarantee mentions calls from different threads, and the official examples send from another thread, but nothing states thread safety itself). The publish/subscribe items above and the item on the frames the LIN master sends, below, rely on this.

"When step T starts, all messages with times before T have arrived" holds only when the sender sent them within its step handler. For frames that other SIL Kit programs send outside a step (from another thread or a receive handler), the receiver step they land in can differ from run to run ([internals/can.md](internals/can.md), [internals/ethernet.md](internals/ethernet.md)).

Controller configuration operations (CAN `SetBaudRate` and `Start`, Ethernet `Activate` and others) cannot be assumed safe to call from any thread (the switch between trivial and detailed simulation happens on the communication thread; see 0002 in "Patches to SIL Kit"). vehicle-sil calls them on the communication thread (in the CommunicationReady handler).

### CAN

`RemoveFrameHandler` returns only after a running handler has finished, because calling and removing handlers are serialized by the same `recursive_mutex`. `SilKitCanPort::close()` relies on this: it removes the handler, then discards the receive buffer.

In trivial simulation, CAN drops a `SendFrame` made before `Start()` with only a warning in the log. No transmit acknowledgement comes (`SimBehaviorTrivial::SendMsg`). `Start()` changes the state immediately. Received frames arrive regardless of `Start()`. `SilKitCanPort` therefore reports an error for a transmission before `start()` instead of passing it to SIL Kit, and the bridge does not pass frames received from outside to SIL Kit before it has started the controller. In trivial simulation, Ethernet returns a `ControllerInactive` acknowledgement for a `SendFrame` made before `Activate()`.

The userContext of a CAN `SendFrame` is carried as a pointer value (64 bits). It reaches the network simulator's `CanFrameRequest` and the transmit acknowledgement, and is `nullptr` in the RX frames of other receivers (`CanController::ReceiveMsg`). `SilKitCanPort` puts the transmission time in it, and the simple CAN Network Simulator reads it. The official documentation says only that the userContext reaches the sender's transmit acknowledgement and the simulator's request; carrying a value across processes is not guaranteed. Frames that the SIL Kit replay sends again have an empty userContext and so no transmission time. The simple CAN Network Simulator places a request without a time at the time of its own last step when it receives the request, and prints a warning once. This placement is not deterministic.

### Network Simulator

Network Simulator API (experimental): when a participant appears that takes over the simulation of a network, the CAN controllers of that network switch to detailed simulation and send transmit requests, baud rate changes and mode changes to that participant. The controller state (`Started` and others) changes only with the state change that the simulator returns. `ControllerDescriptor` is a serial number in order of discovery and is not deterministic.

`SetBaudRate()` and `Start()` called before the controller discovers the simulator are handled by trivial simulation and are not sent to the simulator again after the discovery. A simulator that is already discovered is notified on the same thread when the controller is created. The ports of vehicle-sil call both in the CommunicationReady handler, so the settings of coordinated participants reach the simulator whatever the start order. The settings of autonomous participants (those that join midway, and bridges) and of other programs that call them right after creating the controller do not necessarily arrive. The simple CAN Network Simulator therefore uses `--default-bitrate` if no bit rate arrives, and treats a controller that never reported a mode as started (it stops transmission only for controllers that reported a mode other than `Started`). After the simulation starts, every transmission goes through the simulator. Ethernet works the same way: an `Activate()` before the discovery sets only the local state to LinkUp, and a second `Activate()` does nothing.

### Ethernet

Without a Network Simulator, Ethernet delivers a sent frame to all other controllers on the same network (with no filtering by destination address). The transmit acknowledgement returns inside `SendFrame()`, with the transmission time. `Activate()` sets the state to LinkUp within the call. The receiving controller pads frames shorter than 60 bytes with zeros before it passes them to the handler (`EthController::ReceiveMsgInternal`). `VirtualEthernetBus` matches this behavior, and the tests `eth_silkit.records_match_*` check that it gives the same records.

`EthernetBridge` relies on the behavior below (the tests `ethernet_bridge.silkit_*` check the padding, and that the bridge's own transmissions do not reach its RX handler).

`SendFrame` copies the frame bytes within the call. The bridge passes it, without copying, a buffer that is valid only during the handler of the external interface.

In trivial simulation, a sent frame reaches the other participants as RX and the sender itself as TX. A handler registered for the RX direction only does not receive the sender's own transmissions.

In trivial simulation, `Activate()` makes the controller `LinkUp` immediately, and the transmit acknowledgement is called synchronously inside `SendFrame`. The status is `Transmitted` if the controller is `LinkUp`, `LinkDown` if it is `LinkDown`, and `ControllerInactive` before `Activate()`.

The receiver pads frames shorter than 60 bytes with zeros before it passes them to the handler (`EthController::ReceiveMsgInternal`). The sender has no minimum length check (`SilKitEthPort` pads to 60 bytes itself before it sends; the bridge passes on frames received from outside as they are). No maximum length check is apparent on either the sender or the receiver.

`RemoveFrameHandler`, as with CAN, returns only after a running handler has finished (calling and removing handlers are serialized by the same `recursive_mutex`).

The controller calls the frame handlers while it holds the `recursive_mutex` of the handler collection (`SynchronizedHandlers::InvokeAll`). In trivial simulation, `SendFrame` passes through `InvokeAll` of the same collection to deliver TX to the sender itself, so it takes the same lock. A `SendFrame` called by the receive thread of the external interface therefore waits until the SIL Kit receive thread has finished the handler of `EthernetBridge` (and the `ExternalLink::transmit()` within it). The contract of `ExternalLink` (`transmit()` does not block, and no lock that `transmit()` takes is held during the handler) relies on this.

### LIN

In LIN trivial simulation, a slave's response is generated within the handler that receives the header (`SimBehaviorTrivial::ProcessFrameHeaderRequest`; `LinFrameHeaderEvent` for dynamic response). The timestamp of the header is the master's transmission time. The timestamp of the response is the slave's `Now()` and so is not deterministic (`LinMaster` does not use it).

For a header with no slave configured to respond and no dynamic-response slave, `LIN_RX_NO_RESPONSE` is reported synchronously from within the master's own `SendFrameHeader`. If a dynamic-response slave (`LinSlave`) is present, the header is always sent, and if there is no response, no status is reported. `LinMaster` treats that case as NoResponse at the end of the slot.

SIL Kit records the first slave that responds to an ID as the sender for that ID, and there is no way to undo this. If another slave responds to the same ID, the master keeps reporting Error without sending the header.

In trivial simulation, LIN `GoToSleep()` (master only) sends the go-to-sleep frame (ID 0x3C) without a header and puts the master into Sleep immediately. `Wakeup()` sends a pulse with direction RX to the other participants, calls the caller's own handler with direction TX, and puts the caller into Operational. The state of the receivers does not change, so they call `GoToSleepInternal()` / `WakeupInternal()` themselves. Changing to the state a controller is already in logs a warning.

A LIN controller in Sleep discards received frames (`LinTransmission`), the go-to-sleep frame included. Header events of dynamic response and wakeup pulses arrive in any state. `LinSlave` therefore does not put the SIL Kit controller into Sleep.

A dynamic-response slave receives every frame, with the status it was sent with, regardless of its Rx configuration. `SilKitLinPort` passes on the frames that are Ok (except the go-to-sleep frame), and `LinSlave` passes on only the subscribed IDs.

`SendDynamicResponse` sends the other controllers an update that makes the response of that ID `TxUnconditional`. The v5.0.8 master counted this as a responding node in `SendFrameHeader`, so once 2 dynamic-response slaves had each responded once to the same ID, it stopped sending that header and reported `LIN_RX_ERROR` instead. Patch 0001 makes it not count dynamic-response nodes (see "Patches to SIL Kit").

When 2 nodes respond to the same header, the master receives both responses as `LIN_RX_OK` (`LIN_TX_OK` for a frame the master sends). Trivial simulation does not treat the collision as an error (upstream's tests also list this as a known issue). `LinMaster` reports Error when a second response arrives in one slot.

If the checksum model of the Rx configuration or of the response is `LinChecksumModel::Unknown`, trivial simulation does not compare the checksum models. The `LinChecksumModel` of vehicle-sil has only Classic and Enhanced, so `SilKitLinPort` never passes `Unknown` to SIL Kit.

A frame the master sends (`MasterResponse` in `SendFrame`) is not sent within `SendFrame`. It is created when SIL Kit delivers the header to the master itself (a task on the I/O thread) and is sent from there as a further task. The status of the master's own frame (`LIN_TX_OK`) is also reported within that delivery of the header. Its timestamp is the master's `Now()` at that moment.

The SIL Kit `SendMsg` only queues the transmission as a task on the I/O thread. With a blocking step handler, a task that notifies the next step time is queued as soon as the handler returns, so the second round of sends above comes after that notification. In addition, the I/O thread processes several messages received in one read in a row (`VAsioPeer::DispatchBuffer`) and may run the next step directly among them (`ReceiveNextSimTask`). With the asynchronous version, `CompleteSimulationStep` queues the completion as a task, and the next step does not run until the step completes. `ParticipantRuntime` uses this to make the master's frame go out before the notification of the next step time.

## SIL Kit versions that can be mixed

Only v5.0.8 is verified, for the consumer's `SilKit::SilKit`, for participants on other computers and for registries started outside vehicle-sil alike. Since 4.0.5, SIL Kit guarantees that participants of different versions can connect, but some of the behavior vehicle-sil relies on came in later versions. The table below is taken from the SIL Kit change log; it has not been tested with older versions.

| Version | Behavior introduced | With an older version |
|---|---|---|
| 4.0.24 | Messages are sent in the order the API is called, and the registry relays between participants that cannot connect directly | The ordering assumptions of Control do not hold, and participants behind a firewall cannot connect |
| 4.0.26 | Waits for subscription confirmations before CommunicationReady | No guarantee that the start window and the first controller settings reach the other participants |
| 4.0.31 | An autonomous participant can join midway into virtual time that has already advanced, and a coordinated participant that tries to join midway aborts | Joining midway behaves differently |
| 4.0.34 | A participant goes into Error only after `StartLifecycle`, and a disconnected participant is reported as Error only if it started its lifecycle | The way to detect participants that are gone changes |
| 4.0.39 | If a connection can be made in one direction only, the other side connects back | Multi-computer setups in which only one side can listen cannot connect |
| 4.0.55 | The registry refuses a participant with a name in use every time | With an old registry, a second participant with the same name silently joins (the retry of `create_participant()` relies on the refusal) |
| 5.0.7 | Disconnected participants are removed from the wait for subscription confirmations, and `Experimental.TimeSynchronization.DynamicSimulationStep` is introduced | With a disconnected participant, controller creation may not return, and older versions do not know this key of the default configuration (what happens when they read it is not verified) |
| 5.0.8 | IPv6 addresses in square brackets are supported, and a participant stops even when the stop does not pass through Stopping | IPv6 registry URIs may not work |

## Known upstream bugs

### SIL Kit: data race in `ISystemMonitor::SetParticipantConnectedHandler`

**Description**: `SystemMonitor::SetParticipantConnectedHandler()` assigns `_participantConnectedHandler` outside the lock. So does `SetParticipantDisconnectedHandler()`. Meanwhile, `OnParticipantConnected()` on the I/O thread reads and calls the same handler, also outside the lock (`SilKit/source/services/orchestration/SystemMonitor.cpp`). Calling these functions after the participant is created is undefined behavior if the call coincides with another participant connecting. Participants that connect between the replay of past connections and the assignment may also go unreported.

**Detection**: When these APIs are used, ThreadSanitizer reports a data race.

**Handling**: vehicle-sil does not use these 2 APIs. It tracks the status of other participants with `AddParticipantStatusHandler` only. Inside SIL Kit, these handlers are kept in a synchronized handler list (calling handlers may overlap with registering and removing them). However, the wrapper of the public C++ API keeps the registered handlers in a table without a lock, so registering and removing cannot be called from 2 threads at the same time. `ParticipantRuntime` limits the registering and removing it does to re-read statuses (`refresh_peer_statuses()`, `wait_for_peer()`) to one thread at a time (test `core.peer_statuses_can_be_re_read_from_several_threads_at_once`, checked with ThreadSanitizer). If a participant that started its lifecycle disconnects without ending normally, SIL Kit publishes the status `Error`, so this case is detected too. The upstream code is not changed, so no patch is needed.

**Reported upstream**: Not yet.

### SIL Kit: status updates lost while `AddParticipantStatusHandler` registers

**Description**: `SystemMonitor::AddParticipantStatusHandler()` works in this order (`SystemMonitor.cpp`):

1. Takes the lock and replays the current status of the connected participants to the new handler.
2. Releases the lock.
3. Adds the handler to the list.

A status that arrives between 2 and 3 does not reach the new handler. If it is the last status of the other participant (Running, for example), the handler never observes that state.

**Impact**: Rarely, a participant keeps waiting for another participant that is already Running, because it never observes that state.

**Handling**: While `ParticipantRuntime::wait_for_peer()` waits, it resynchronizes every 200 ms (`resync_peers()`). A resynchronization registers a temporary handler and removes it right away. The replay at registration runs correctly under the lock, so the lost status is recovered.

- `enterTime` is compared so that a replayed status does not overwrite a newer status that has already arrived.
- Normal notifications are always applied. The `Error` on disconnection is stamped with the local clock, so it is not compared with the other participant's clock.
- `ISystemMonitor::ParticipantStatus()` is not used, because it returns a reference to internal data under the lock and the caller reads it outside the lock.

**Reported upstream**: Not yet.

### SIL Kit (Windows): closing a local-domain socket to a process with suspended threads may not return

**Observed**: By default, SIL Kit connects participants on the same machine through local-domain sockets (AF_UNIX on Windows). On Windows, all threads of one participant were stopped with `SuspendThread` while a simulation was running, and the controller's participant was then destroyed. Rarely, the call that closes the socket to the stopped participant (`closesocket()` on the I/O thread) did not return. SIL Kit waits for the I/O thread to end when it destroys a participant (`VAsioConnection::~VAsioConnection()`), so the destruction did not return. The process then remained even after it called `TerminateProcess` on itself. Repeating the same procedure with a configuration without local-domain sockets (`Middleware.EnableDomainSockets: false`, TCP only) did not reproduce it within the repetitions tried. The same test on Linux has not shown it either.

**Hypothesis (not verified)**: The stopped thread is in the middle of a system call on the AF_UNIX socket, and the close on this side waits for that call. That is also why the process cannot end. A debugger's pause also stops threads, so the same can happen there. TCP connections close regardless of the state of the other process.

**Impact**: If the hypothesis is right, stopping the whole simulation while one participant is stopped in a debugger rarely leaves other participants or the launcher unable to end.

**Handling**: On Windows, vehicle-sil participants (`ParticipantRuntime`) are created with `Middleware.EnableDomainSockets: false` when no configuration is passed. The registries that vehicle-sil starts (`vehicle_sil_launch run`, `Simulation`) do this on every OS: if the SIL Kit registry cannot listen on a TCP port, it keeps running with local-domain sockets only and takes over the local-domain path of another registry with the same URI (see "Listen addresses" in [simulation-run.md](simulation-run.md#listen-addresses)). The defaults are created in one place (`default_participant_configuration()`, `default_participant_configuration_with()`, `merged_participant_configuration()`, and `default_registry_configuration()` for the registry; for the other places to change, see "SIL Kit configuration defaults" in [internals/core.md](internals/core.md#sil-kit-configuration-defaults)). The configuration that `vehicle_sil_can_bus_simulator` creates itself (the log settings) is built on top of these defaults too. Even if the other participant has a local-domain endpoint, this side does not use it, so the two connect over TCP. When you pass your own SIL Kit configuration (`participant_configuration`, `--silkit-config` of ecu_main, `vehicle_sil_can_bus_simulator --participant-configuration`), `Middleware.EnableDomainSockets` is also set to `false` on Windows (`true` in the file becomes `false` too). This handling does not apply to connections between SIL Kit participants that do not use vehicle-sil.

**Enforcement**: The test `core.silkit_configuration_defaults` (`cmake/VehicleSilCheckSilKitConfiguration.cmake`) checks that every place in `libs/`, `tools/`, `bridges/` and `tests/` that builds a SIL Kit configuration from a string (`ParticipantConfigurationFromString(`) goes through the functions above.

**Safety net**: `vehicle_sil_launch run` destroys its own SIL Kit objects (the variable endpoints, the controller, the registry) on a dedicated thread and waits until a deadline. If the destruction does not return, it adds the finding `connection-not-closed`, writes the result, and ends the process without destroying static objects (`OutcomeClose::not_closed_ms`; see "Stopping a run" in [simulation-run.md](simulation-run.md#stopping-a-run)). In the situation under "Observed" above, the result was written, but the process remained.

**Reported upstream**: Not yet.

### SIL Kit: the lifecycle goes on after an Error during the start

**Description**: Two steps of the start of a coordinated participant are finished by a callback that runs later: the wait for the replies of all participants (`ServicesCreatedState::ServicesCreated()`) and the wait for the pending subscriptions (`CommunicationInitializingState::CommunicationInitializing()`, both in `SilKit/source/services/orchestration/LifecycleStates.cpp`). The callback sets the next state without looking at the current one. If the participant enters Error while one of these waits is under way, the callback replaces Error with CommunicationInitializing or CommunicationInitialized, and the lifecycle goes on from there.

A coordinated participant that is not required gets into this order. `LifecycleService::SetWorkflowConfiguration()` reports the error with a deferred call (`ReportError()`). If the same message, or one that is read from the socket together with it, makes the system state ServicesCreated, the wait for the replies begins before the deferred call runs.

**Impact**: What the participant does next depends on which message arrives first. Either it goes on to Running and takes part in the simulation, although SIL Kit reported it as invalid. Or it stays in a state before Running for good, because the system state it waits for has already passed. With time synchronization, the other participants then wait for it, and virtual time stays at 0 until it leaves.

**Handling**: Fixed by patch [0006](#0006-the-lifecycle-does-not-leave-error-when-a-wait-of-the-start-ends): the two callbacks do nothing unless the participant is still in the state that began the wait, so a participant in Error stays there until it is stopped. `ParticipantRuntime::start()` throws for it, and an ecu_main executable ends with the exit code of a failed start and leaves the simulation. The test `core.participant_start_fails_for_a_participant_that_is_not_required_and_it_stays_in_error` forces the order and fails without the patch.

The patch does not cover a stop that comes while the start still waits for the replies. SIL Kit takes the participant to Shutdown, but drops the wait for the replies that would then set the final state, because it runs one such wait at a time (`ParticipantReplies::CallAfterAllParticipantsReplied()`). `ParticipantRuntime::stop()` therefore ends its wait when the lifecycle is in Shutdown after such a failed start, instead of waiting for `shutdown_timeout`; the test `core.participant_stop_after_a_failed_start_returns_once_the_lifecycle_is_in_shutdown` holds the replies back to force this order.

`ParticipantRuntime` also has a provision for a SIL Kit without the patch (the consumer's own `SilKit::SilKit`). It remembers an Error that SIL Kit publishes for its own participant before the participant begins to run. `start()` then throws, as it does for a start that ended in Error, whatever the state is by then, and the step handler is not called (see "State when `start()` returns" in [internals/core.md](internals/core.md#state-when-start-returns)). The provision does not put SIL Kit back into Error, so two things remain without the patch:

- SIL Kit may not take the stop of the participant whose start failed. The participant then stays until `shutdown_timeout` has passed, and with time synchronization the virtual time of all participants does not advance meanwhile.
- Rarely, `start()` reads the state after SIL Kit has left Error and before the Error status is delivered. It returns, and the participant runs no step and reports nothing for the rest of the run.

Remove the patch when the submodule is updated to an upstream commit that has the fix.

**Reported upstream**: Not yet.

### SIL Kit: a participant that joins while another one leaves waits for the one that left

**Description**: When a participant ends, the registry learns of it only when its I/O thread handles the end of that connection. A participant that joins before that gets the participant that ended in its list of known participants (`VAsioRegistry::SendKnownParticipants()`). It then tries 3 ways to reach it (`ConnectKnownParticipants.cpp`):

1. A direct connection. It fails at once.
2. A connection that the other participant makes on a request relayed by the registry. The registry has removed the target by then and drops the request without an answer (`VAsioConnection::ReceiveRemoteParticipantConnectRequest_Registry()`, "Ignoring invalid RemoteParticipantConnectRequest"). The requester waits for `Middleware.ConnectTimeoutSeconds`.
3. The registry as a proxy. The registry cannot deliver the message either ("Unable to deliver proxy message") and does not say so. The requester waits for a reply for another `ConnectTimeoutSeconds`.

Nothing tells the joining participant that the other one is gone.

**Impact**: Rarely, when a participant joins right after another one ended (an ECU that is started again at once, runs that follow each other at one registry), `CreateParticipant()` fails after twice `ConnectTimeoutSeconds` (10 seconds by default) with `Timeout while waiting for replies from known participants: <name> (...) is waiting for reply`. A second attempt succeeds.

**Handling**: `create_participant()` (and so `ParticipantRuntime`) creates the participant once more after this timeout and writes one line saying so to standard error. A participant that meets the bug therefore joins about 10 seconds late instead of failing. If the second attempt times out as well, it throws SIL Kit's error with a note that it tried again. It recognizes the error by its text; when you update upstream, the test `core.silkit_still_writes_the_texts_create_participant_matches` checks that SIL Kit's sources still contain it. Patch 0005 covers only the moment in which the registry has already closed the socket of the participant that ended. A longer `ConnectTimeoutSeconds` does not help, because the reply never comes.

**Reported upstream**: Not yet.

## Patches to SIL Kit

The patches in `patches/sil-kit/` were created with `git format-patch` on the upstream tag `v5.0.8` (the pinned commit) and follow the upstream style (clang-format). 0001 includes an upstream integration test, and 0006 includes upstream unit tests.

| Patch | What it fixes |
|---|---|
| [0001](#0001-the-lin-master-does-not-count-dynamic-response-nodes) | The LIN master does not count dynamic-response nodes |
| [0002](#0002-make-the-can-controllers-behavior-switch-atomic) | Makes the CAN controller's behavior switch atomic |
| [0003](#0003-the-registry-removes-only-the-peers-own-name-entry) | The registry removes only the peer's own name entry |
| [0004](#0004-do-not-lose-a-handler-when-an-asynchronous-subscription-completes) | Does not lose a handler when an asynchronous subscription completes |
| [0005](#0005-the-registry-does-not-advertise-a-participant-whose-connection-has-closed) | The registry does not advertise a participant whose connection has closed |
| [0006](#0006-the-lifecycle-does-not-leave-error-when-a-wait-of-the-start-ends) | The lifecycle does not leave Error when a wait of the start ends |

**Reported upstream**: None of them yet. The code that 0001, 0002, 0004 and 0005 change is the same on upstream `main` (for 0003 and 0006, upstream `main` has not been checked).

### 0001: The LIN master does not count dynamic-response nodes

**Description**: When the master in trivial simulation counts the nodes that respond in `SendFrameHeader`, it does not count dynamic-response nodes. A dynamic-response node decides for each header whether it responds, so an earlier `SendDynamicResponse` does not make it a node that always responds.

**Why vehicle-sil needs it**: `LinSlave` uses dynamic response. Without the fix, once 2 `LinSlave`s have each responded once to the same ID (LIN diagnostics 0x3D is shared by all slaves), headers for that ID are no longer sent.

**Upstream reproduction test**: `ITest_LinDynamicResponse.normal_master_dynamic_slaves_take_turns_on_one_id`

**Verification**: Verified with the upstream LIN tests (unit and integration).

### 0002: Make the CAN controller's behavior switch atomic

**Description**: Makes atomic the pointer with which a CAN controller switches between trivial and detailed behavior (`SimBehavior::_currentBehavior`). The switch happens in service discovery on the I/O thread, while the controller's thread may read the same pointer at the same time, in `Start()` and other calls.

**Why vehicle-sil needs it**: Some places still call the controller from outside the SIL Kit threads: `Stop()` in `SilKitCanPort::close()` (on the stopping thread; it can overlap with the switch back to trivial when the simulator leaves first), transmission on real-time ports (driver thread), and transmission in the Vector XL CAN Bridge (XL receive thread). `SetBaudRate()` and `Start()` are called from the CommunicationReady handler (a SIL Kit thread), so they are not affected. The race was found when `Start()` was still called at creation: ThreadSanitizer reported a data race in a test that runs the simple CAN Network Simulator in a separate process.

**Upstream reproduction test**: None (verified with the ThreadSanitizer tests of vehicle-sil). Not fixed: on a second switch, when the simulator leaves and appears again, a call that uses the behavior from before the switch can overlap with the rewrite of the simulator link.

**Verification**: Verified by running the simple CAN Network Simulator tests of vehicle-sil (`can_netsim.*`) with ThreadSanitizer. The race overlaps only rarely, so the tests do not necessarily catch it if the patch is removed. The tests therefore cannot tell whether the patch can be dropped. Decide by checking whether the code still has the places listed under "Why vehicle-sil needs it" above, which call the controller from outside the SIL Kit thread.

### 0003: The registry removes only the peer's own name entry

**Description**: When a peer disconnects, the registry removes the name entry (`_participantNameToPeer`) only if that entry belongs to that peer. The registry gives an incoming peer its name before it checks whether the name is in use, so when a peer that was refused for its name disconnected, the entry of the participant already using the name was removed.

**Why vehicle-sil needs it**: After a participant is refused for its name (a wrong `--name`, or a process restarted right away; `create_participant()` retries while it is refused), participants that cannot connect directly to the participant already using the name (behind a firewall, or listening only on local-domain sockets) cannot connect to it through the registry's relay (`RemoteParticipantConnectRequest`), and time out waiting to join.

**Upstream reproduction test**: None (verified with `core.a_refused_name_leaves_the_registry_able_to_relay_to_the_participant_that_has_it` of vehicle-sil)

**Verification**: Verified with a test in which a TCP-only participant connects through the relay to a participant that listens only on local-domain sockets. Without the patch, the connection times out after a participant has been refused for the same name.

### 0004: Do not lose a handler when an asynchronous subscription completes

**Description**: Protects with one mutex both the registration of handlers that are called when asynchronous subscriptions (such as the registration of the network simulator) complete (`VAsioConnection::AddAsyncSubscriptionsCompletionHandler()`) and the completing side (`AsyncSubscriptionsCompleted()`). The registration checks on the calling thread that the subscriptions have not completed and then adds the handler to the list, while the completing side, on the I/O thread, calls and clears the list and then resets the flag. If the last acknowledgement arrived in between, the added handler was never called.

**Why vehicle-sil needs it**: SIL Kit issues the announcement of a simulated network (the Link service that switches the controllers to detailed simulation) from this handler (the constructor of `SimulatedNetworkRouter`, on the user's thread). If the handler is lost, the network is never announced: the controllers keep running in trivial simulation, `SetBaudRate()` and the mode do not reach the simulator (the bit rate stays at its default), and frames arrive both directly and through the simulator. No error or warning appears. This can happen, rarely, every time the simple CAN Network Simulator starts.

**Upstream reproduction test**: None (the overlap is rare, and tests do not reliably catch it. `j1939_silkit.*_on_the_simulator_*` and `can_netsim.*` of vehicle-sil notice it in the simulator's bit rate statistics)

**Verification**: Verified by temporarily adding a delay between the check and the add in the registration: without the patch, the tests that use the simulator fail with the bit rate left at its default; with the patch, they pass. Without the delay the overlap is rare, so the tests do not necessarily catch it if the patch is removed.

### 0005: The registry does not advertise a participant whose connection has closed

**Description**: The registry leaves participants whose connection has already closed out of the list it sends to a joining participant (known participants). When a connection drops, the socket closes immediately, but the registry removes the participant from the list only later, when the end processing runs on the I/O thread. If the registry handled the join of another participant in between, it tried to read the remote address of the closed socket, got an exception (`remote_endpoint: Bad file descriptor`), and the joining participant received no reply.

**Why vehicle-sil needs it**: Where a participant joins right after another one ends (ECUs restarted one after another, the launcher recreating participants, tests), `CreateParticipant()` on the joining side fails after `Middleware.ConnectTimeoutSeconds` (default 5 seconds) with "Timeout during connection handshake with the SIL Kit Registry". The message suggests a duplicate name, but the name is not a duplicate. The registry log shows "SilKit-IOWorker: Something went wrong". Creating the participant again joins.

**Upstream reproduction test**: None (the timing of the overlap cannot be controlled from outside, and vehicle-sil has no test that always fails either)

**Verification**: Verified by temporarily delaying the end processing in the registry: without the patch, a test that creates participants one after another always fails with this timeout; with the patch, it passes. The temporary delay was not kept.

### 0006: The lifecycle does not leave Error when a wait of the start ends

**Description**: The two callbacks that finish a wait of the start (the replies of all participants in `ServicesCreatedState::ServicesCreated()`, the pending subscriptions in `CommunicationInitializingState::CommunicationInitializing()`) set the next state only if the participant is still in the state that began the wait. Before, they set it whatever the state was. A participant that had entered Error, or had been aborted, while it waited was put back into the start and went on from there (see "[SIL Kit: the lifecycle goes on after an Error during the start](#sil-kit-the-lifecycle-goes-on-after-an-error-during-the-start)").

**Why vehicle-sil needs it**: A participant that is started by hand with a name that the run does not have joins as a coordinated participant that is not required. SIL Kit puts it into Error when the simulation starts. Without the patch it sometimes left Error again: it took part in the run, or it stayed before Running and held the virtual time of all participants at 0, so the run ended with "virtual time has not advanced". Handling this in `ParticipantRuntime` alone leaves the two cases listed under that bug.

**Upstream reproduction test**: `Test_LifecycleService.error_while_waiting_for_replies_is_kept` and `Test_LifecycleService.error_while_waiting_for_subscriptions_is_kept` (in the patch)

**Verification**: Verified with the upstream unit tests of the lifecycle service and with `core.participant_start_fails_for_a_participant_that_is_not_required_and_it_stays_in_error` of vehicle-sil. Both fail without the patch. The abort during a wait is fixed by the same check and has no test of its own.

## Updating the upstream version

1. Run `git -C external/<lib> fetch --tags`, then `git -C external/<lib> checkout <commit>`, and stage the change with `git add external/<lib>`.
2. Run `git submodule update --init --recursive` to update the nested submodules.
3. Review "Upstream behavior the implementation relies on" on this page.
4. If there are patches, check that they apply (the configure step does not fail).
5. If they do not apply, recreate them on the new commit (`patches/README.md`).
6. Delete patches that upstream has merged.
7. Build and test all Linux presets, and run the FetchContent tests too.
8. Update the table of pinned commits and commit.

## See also

- Overview and list of targets: [architecture.md](architecture.md)
- Internals of the part you change: "Internals" in [architecture.md](architecture.md#internals)
- Build steps and requirements: [build.md](build.md)
- How to create patches: [patches/README.md](../patches/README.md)
