# Connect a standard SIL Kit participant

[日本語](../ja/howto/standard-silkit-participant.md)

This guide shows how to build a standalone C++ participant that uses only the standard [Vector SIL Kit](https://github.com/vectorgrp/sil-kit) API (with zero vehicle-sil headers or libraries) and run it together with vehicle-sil ECUs under `vehicle_sil_launch`.

The runnable sample is in [examples/standard_silkit_participant/](../../examples/standard_silkit_participant/).

## What the sample contains

| File | Contents |
|---|---|
| `CMakeLists.txt` | Standalone CMake project (`find_package(SilKit REQUIRED)`) that links only `SilKit::SilKit`. Inside a vehicle-sil repository build, it reuses the existing `SilKit::SilKit` target without downloading an external SDK. |
| `main.cpp` | Minimal C++17 coordinated SIL Kit participant (`#include <silkit/...>` only) that exchanges CAN frames and tears down the participant cleanly before reading final counters. |
| `rx_inbox.hpp` | Pure C++17 CAN receive inbox that buffers frames, drains only `tx_ns < now_ns` (retaining `tx_ns >= now_ns` for the next step), and sorts drained frames canonically. |
| `simulation.yaml` | Launcher configuration combining `Engine` (`examples/ecu_main`, 10 ms step) and `StandardPeer` (`standard_silkit_participant`, 25 ms step) on `J1939_CAN`. |

## Build with the official Vector SIL Kit SDK

Download and extract an official Vector SIL Kit release archive (for example `SilKit-5.0.8-ubuntu-24.04-x86_64-gcc.zip`) whose `SilKit/` directory contains `include/`, `lib/`, and `lib/cmake/SilKit/SilKitConfig.cmake`.

Configure and build the sample outside the vehicle-sil build tree, pointing CMake at the extracted SDK:

```sh
cmake -S examples/standard_silkit_participant -B /tmp/build-std-peer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/SilKit-5.0.8-ubuntu-24.04-x86_64-gcc/SilKit
cmake --build /tmp/build-std-peer
```

You can pass `-DSilKit_DIR=/path/to/SilKit/lib/cmake/SilKit` instead of `-DCMAKE_PREFIX_PATH`. The resulting binary links only `libSilKit.so` (or `SilKit.dll` on Windows) and the system C++ runtime; it does not include any `vehicle_sil/` headers or link any vehicle-sil libraries.

When you build the vehicle-sil repository itself (`VEHICLE_SIL_BUILD_EXAMPLES=ON`, the default), the same sample is also built at `build/<preset>/examples/standard_silkit_participant/standard_silkit_participant` using the repository's `SilKit::SilKit` target.

## Configure `vehicle_sil_launch`

List the standard participant in `simulation.yaml` with `describe: false`, and pass the participant name and registry URI through the `{name}` and `{registry_uri}` placeholders:

```yaml
vehicle_sil_simulation: 2

time_step_ms: 5
networks:
  - name: J1939_CAN
    kind: can
participants:
  - name: Engine
    command: [../ecu_main/engine/vehicle_sil_engine_ecu]
    step_ms: 10
  - name: StandardPeer
    describe: false
    command:
      - ./standard_silkit_participant
      - --name
      - "{name}"
      - --registry-uri
      - "{registry_uri}"
      - --network
      - J1939_CAN
      - --step-ms
      - "25"
      - --tx-id
      - "0x200"
      - --tx-period-ms
      - "25"
```

Key points for the configuration (see [simulation-config.md](../simulation-config.md)):

| Setting | Why it is needed |
|---|---|
| `describe: false` | Standard SIL Kit participants do not implement `vehicle-sil`'s `--describe` flag. `vehicle_sil_launch check` verifies that the executable exists and skips `--describe`, emitting `not-checked` (and `single-participant` if only one `describe: true` ECU is on that network) as notes while exiting with code 0. |
| `"{name}"` and `"{registry_uri}"` | For `describe: false` participants, the launcher does not append `--name` or `--registry-uri` automatically. Placeholders pass the configured participant name and the runtime registry URI; always quote them in YAML so YAML does not parse `{...}` as a mapping. |
| `required: true` (default) | Coordinated participants must remain `required: true` so the SIL Kit system controller waits for them before starting virtual time. |

Run `check` and `run` from the directory containing `simulation.yaml` (for example `build/linux-gcc-release/examples/standard_silkit_participant`):

```sh
vehicle_sil_launch check simulation.yaml
vehicle_sil_launch run simulation.yaml --speed max --stop-at-ms 500
```

To run the standalone official-SDK binary directly against vehicle-sil ECUs, edit a copy of `simulation.yaml` kept in `build/<preset>/examples/standard_silkit_participant/` and replace `./standard_silkit_participant` with `/tmp/build-std-peer/standard_silkit_participant` (if you run `/tmp/build-std-peer/simulation.yaml` instead, update `Engine`'s path to the built `vehicle_sil_engine_ecu` as well).

## Set `time_step_ms` explicitly for asymmetric periods

When every participant has `describe: true`, `vehicle_sil_launch` reads each ECU's step period from `--describe` and resolves `time_step_ms` automatically from the greatest common divisor (GCD) of `10` ms, the active ECU periods, and `--stop-at-ms`.

When any enabled participant has `describe: false`, the launcher cannot read its step period from `--describe`, so automatic resolution falls back to **`time_step_ms: 1`**.

In asymmetric-period simulations (such as a `10` ms vehicle-sil ECU paired with a `25` ms standard SIL Kit participant), set `time_step_ms` explicitly to the GCD of all participant step periods and the stop time:

- For `10` ms and `25` ms with `--stop-at-ms 500`, `gcd(10, 25, 500) = 5`, so write `time_step_ms: 5`.
- This avoids unnecessary 1 ms controller steps during `--speed max` runs and keeps every participant's step boundary aligned with the controller's grid.
- Make sure `--stop-at-ms` is a multiple of `time_step_ms`; otherwise `check` and `run` report `stop-not-on-step`.

## Handle simultaneous timestamps deterministically

When two participants have different periods (for example `10` ms and `25` ms), their simulation steps coincide at every least common multiple (`t = 0, 50, 100, 150, ...` ms). At a simultaneous timestamp `T`, a CAN frame sent by peer A at virtual time `T` may arrive in peer B's `AddFrameHandler` either before or after peer B's `SetSimulationStepHandler(T)` executes, depending on IPC scheduling and wall-clock speed (`--speed 1` vs `--speed max`).

To keep execution bit-for-bit reproducible across runs and speeds:

1. In `AddFrameHandler`, copy incoming frames into a mutex-protected inbox instead of mutating simulation state directly.
2. In `SetSimulationStepHandler(now, duration)`, partition the inbox:
   - **Process** only frames sent strictly before the current step (`frame_timestamp < now`).
   - **Keep queued** any frame with `frame_timestamp >= now` for the next step. Do **not** drop equal-timestamp frames.
3. Sort the drained frames canonically (for example by `(timestamp, can_id, flags, length, payload)`) before processing them.

The sample in [examples/standard_silkit_participant/rx_inbox.hpp](../../examples/standard_silkit_participant/rx_inbox.hpp) (used by [examples/standard_silkit_participant/main.cpp](../../examples/standard_silkit_participant/main.cpp)) implements this pattern:

```cpp
std::vector<RxRecord> ready;
{
    std::lock_guard<std::mutex> lock(rx_mutex);
    std::vector<RxRecord> deferred;
    deferred.reserve(rx_inbox.size());
    for (const auto& rec : rx_inbox)
    {
        if (rec.tx_ns < now_ns)
        {
            ready.push_back(rec);
        }
        else
        {
            deferred.push_back(rec);
        }
    }
    rx_inbox.swap(deferred);
}
```

With this rule, a frame sent by the `10` ms ECU at `t = 50` ms is always deferred at the `25` ms participant's `t = 50` ms step and processed deterministically at `t = 75` ms, producing identical CAN traces at `--speed 1` and `--speed max`.

## Restart the participant for subsequent runs

Each coordinated SIL Kit participant executes one lifecycle (`StartLifecycle()` through `ParticipantState::Shutdown`) per simulation run:

- **Managed by `vehicle_sil_launch` (`manual: false`, default):** Each `vehicle_sil_launch run` invocation — and each run issued to a resident `vehicle_sil_launch serve` process — starts a fresh participant process and stops it cleanly at shutdown, so consecutive runs start at virtual time `0` with no residual state or queued frames.
- **Started manually (`manual: true`):** Start a new instance of the standard participant for every subsequent run after the previous run shuts down (see [Starting a participant yourself](../simulation-config.md#starting-a-participant-yourself-manual---manual)).

Keeping an external participant alive across runs or joining a running simulation with mixed non-zero start timestamps is not part of the supported contract; always start a fresh coordinated participant per run.

## Verification scope (CI vs official Vector SIL Kit SDK)

| Environment | SIL Kit library used | What is verified |
|---|---|---|
| Repository CI (`ctest -R '^standard_silkit_participant\.'`) | Repository's `SilKit::SilKit` target (no external SDK download in CI) | Zero `vehicle_sil/` includes in the sample (`no_vehicle_sil_includes`); deterministic unit tests on `DeterministicRxInbox` for strict-before deferral, equal-timestamp retention, and canonical ordering (`rx_inbox_*`); asymmetric `10` ms vs `25` ms bidirectional CAN and strict virtual-time causality with `--speed max` (`asymmetric_bidirectional_max`, with no wall-clock speed assertions); byte-for-byte trace identity between `--speed max` and `--speed 1` (`simultaneous_timestamp_determinism`); fresh `t = 0` cross-run isolation with distinct run tags (`cross_run_isolation`); `check`, `run`, and `Engine` -> `StandardPeer` CAN RX on the shipped `simulation.yaml` (`example_config_smoke`). |
| Local verification on Fedora 44 | Official Vector SIL Kit v5.0.8 SDK (`SilKit-5.0.8-ubuntu-24.04-x86_64-gcc.zip`) | Standalone `find_package(SilKit REQUIRED)` CMake configure/build of `examples/standard_silkit_participant` against the official SDK headers and `libSilKit.so`, and interoperability runs against vehicle-sil executables. |
