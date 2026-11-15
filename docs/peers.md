# Run peers as separate processes (`Peer`, `vehicle_sil_sim_controller`)

[日本語](ja/peers.md)

This page shows how to set up `Peer` objects or SIL Kit participants yourself, without `Simulation` or `ecu_main`, and run them as separate processes. It gives the commands that start the example executables and the `PeerOptions` settings for writing your own peer.

| Goal | Use | Section |
|---|---|---|
| Start 2 peers and confirm that J1939 messages are sent and received | `vehicle_sil_j1939_sender`, `vehicle_sil_j1939_receiver` | [Run peers by hand](#run-peers-by-hand) |
| Synchronize several peers in one virtual time | `vehicle_sil_sim_controller`, `PeerOptions` | [Run in virtual time](#run-in-virtual-time) |
| Reproduce CAN bus arbitration and transmission time | `vehicle_sil_can_bus_simulator` | [Simulate CAN bus arbitration and transmission time](#simulate-can-bus-arbitration-and-transmission-time) |

The integration tests also use these executables.

## Prerequisites

Build the repository first, then run the commands in its root directory. `B` in the commands is the build directory (`build/<preset>`).

| Executable | Location |
|---|---|
| `sil-kit-registry` | `$B/<CMAKE_BUILD_TYPE>/` (`Release/` for a Release preset, `Debug/` for a Debug preset) |
| Example peers | Under `$B/examples/` |
| `vehicle_sil_sim_controller`, `vehicle_sil_can_bus_simulator` | Under `$B/tools/` |

SIL Kit's output rules decide where `sil-kit-registry` goes. In a build with tests disabled, build `sil-kit-registry` explicitly:

```sh
cmake --build build/linux-gcc-release --target sil-kit-registry
```

## Run peers by hand

```sh
B=build/linux-gcc-release
$B/Release/sil-kit-registry --listen-uri silkit://127.0.0.1:8500 &
$B/examples/j1939/vehicle_sil_j1939_receiver --registry-uri silkit://127.0.0.1:8500 --expect-name 0x20008200FE001234 &
$B/examples/j1939/vehicle_sil_j1939_sender   --registry-uri silkit://127.0.0.1:8500 --wait-for J1939Receiver --linger-for J1939Receiver
```

| Peer | What it does |
|---|---|
| Sender | Claims address 0x80 with a node of the J1939 stack. Once it holds the address, it sends PGN 0xFF10 5 times. |
| Receiver | Verifies the CAN ID and the payload, and exits with code 0 if both are correct. |

### Verify the manual run

If the run succeeds, the sender and the receiver both exit with code 0.

| Exit code | Meaning |
|---|---|
| 0 | Success |
| 1 | Verification failed |
| 2 | Timeout |
| 3 | Connection or startup failed |
| 4 | Usage error |

Started without arguments, each executable prints an error line followed by the list of options.

## Run in virtual time

Give each peer its own step size and synchronize all peers in one virtual time. The whole simulation advances at the pace of the slowest peer. Unless you set a speed, it runs as fast as it can (faster than real time).

| Peer | Participant name | Step size | Role |
|---|---|---|---|
| `vehicle_sil_vt_physics` | `Physics` | 1 ms | Physics model |
| `vehicle_sil_vt_ecu` | `Ecu` | 10 ms | J1939 ECU |
| `vehicle_sil_vt_lin_sensor` | `LinSensor` | 2 ms | LIN slave |
| `vehicle_sil_vt_body_controller` | `BodyController` | 5 ms | LIN master |

`vehicle_sil_sim_controller` starts the simulation at T=0 once the 4 required peers have joined, and stops it at 5 seconds of virtual time.

```sh
B=build/linux-gcc-release
URI=silkit://127.0.0.1:8500
$B/Release/sil-kit-registry --listen-uri $URI &
# start at T=0 once the 4 required peers are there, and stop at 5 s of virtual time
$B/tools/sim_controller/vehicle_sil_sim_controller --registry-uri $URI \
    --required Physics,Ecu,LinSensor,BodyController --stop-at-ms 5000 &
E=$B/examples/virtual_time
$E/vehicle_sil_vt_physics         --registry-uri $URI --end-ms 5000 --trace physics.trace &   # 1 ms
$E/vehicle_sil_vt_ecu             --registry-uri $URI --end-ms 5000 --trace ecu.trace &       # 10 ms, J1939
$E/vehicle_sil_vt_lin_sensor      --registry-uri $URI --end-ms 5000 --trace sensor.trace &    # 2 ms, LIN slave
$E/vehicle_sil_vt_body_controller --registry-uri $URI --end-ms 5000 --trace body.trace        # 5 ms, LIN master
```

### Verify the virtual-time run

If the run succeeds, the 4 peers and the controller exit with code 0, and 4 trace files (`physics.trace` and the others) are written. When the controller exits, it prints the wall-clock time that the run took from virtual time 0 to the stop time:

```text
[sim-controller] result sim_ms=5000 wall_us=... speed=...
```

### Arguments

| Argument | Applies to | Behavior |
|---|---|---|
| `--real-time-factor 1` | Peers | Advances in step with real time (the other peers wait for it as well). |
| `--real-time-factor 4`, `0.5` | Peers | Advances at 4 times or half the real-time speed. |
| `--tick-cost-us 25000` | `vehicle_sil_vt_ecu` | Simulates a slow ECU whose `tick()` takes 25 ms of wall-clock time. |
| `--stop-at-ms` | Controller | Without it, the controller keeps running until Ctrl+C. |
| `--end-ms` | Peers | A peer does its processing and tracing only in steps before this time. |
| `--timeout-ms` | Peers | Limit on the wait for start-up. |
| `--run-timeout-ms` | Peers | Limit on the whole run (no limit if omitted). |

With `--tick-cost-us`, the whole simulation runs slower than real time, but the content of the traces does not change.

### Write your own peer

The code below is condensed from `peer_options()` and `run_until_finished()` in [examples/virtual_time/common/vt_peer.hpp](../examples/virtual_time/common/vt_peer.hpp) and from [examples/virtual_time/ecu/main.cpp](../examples/virtual_time/ecu/main.cpp). No file in the repository contains it in this form; `name`, `pgn`, `ecu_tick()` and `ecu_output()` stand for your own code.

```cpp
vehicle_sil::PeerOptions options;
options.participant = "MyEcu";
options.registry_uri = "silkit://127.0.0.1:8500";
options.lifecycle = vehicle_sil::LifecycleMode::Coordinated;
options.virtual_time = vehicle_sil::VirtualTimeOptions{std::chrono::milliseconds(10)};
vehicle_sil::Peer peer(options);
auto& ecu = peer.j1939("J1939_CAN1").add_node({0x80, name});

// Order within one step: J1939 receive and processing -> application (on_step, every, at) -> LIN -> J1939 transmit
peer.every(std::chrono::milliseconds(10), [&](vehicle_sil::SimTime) {
    ecu_tick();                                       // tick() of the existing ECU
    // Sent at the end of the step, with the time of the step. In the 250 ms after Address Claim it is held, and sent in the step where the wait ends
    static_cast<void>(ecu.send(pgn, ecu_output()));
});
peer.start(std::chrono::seconds(30));
peer.wait(std::chrono::minutes(10));
```

> [!NOTE]
> By default, `PeerOptions` selects the autonomous lifecycle with virtual time. If such a peer starts first and virtual time is already advancing, SIL Kit aborts the whole simulation as soon as a coordinated participant (such as an ECU that `vehicle_sil_launch run` starts) starts later. The peer that started first ends too.

A peer that runs together with coordinated participants must do one of these:

- Use `Coordinated`, as in the example above, and be one of the required participants.
- Start after all coordinated participants have started.

For how received messages are handled, the determinism of J1939 and LIN, and the constraints on LIN step sizes, see [internals/virtual-time.md](internals/virtual-time.md) and [internals/lin.md](internals/lin.md). You can also do without `Peer` and run your code from a step handler of `ParticipantRuntime` (`bind_host()`; see the LIN example in `examples/lin`).

When a network has another name in the simulation than in the code, set `options.network_names = {{"VehicleCAN", "J1939_CAN1"}};` (simulation name to code name) to match them without changing the code. Only the name of the SIL Kit network and the names of the monitoring output change. For the rules, see "Renaming networks" in [ecu-executable.md](ecu-executable.md#renaming-networks---map-network) (an ECU executable takes `--map-network`).

## Simulate CAN bus arbitration and transmission time

SIL Kit's standard CAN delivers each frame to every participant with the time at which it was sent. To reproduce arbitration and transmission time in virtual time, add the simple CAN Network Simulator as a participant. The peers' code stays the same.

`B` and `URI` are the same as in "Run in virtual time" above. Replace `Ecu1,Ecu2` with the participant names of your peers.

```sh
cat > netsim.yaml <<'YAML'
SchemaVersion: 1
SimulationStepSizeNanoseconds: 1000000
SimulatedNetworks:
  - Name: CAN1
    Type: CAN
YAML
$B/tools/can_bus_simulator/vehicle_sil_can_bus_simulator --connect-uri $URI --default-bitrate 250000 netsim.yaml &
# add NetworkSimulator to the required participants of the system controller
$B/tools/sim_controller/vehicle_sil_sim_controller --registry-uri $URI --required NetworkSimulator,Ecu1,Ecu2 --stop-at-ms 5000
```

| Item | Behavior |
|---|---|
| Time of a received frame | The end of its transmission. |
| Frames sent at the same time | Delivered in CAN ID order (frames from the same controller in the order they were sent). |
| What decides the result | Only the transmit times and the step size of the simulator. |
| What is simulated | CAN networks only. Stuff bits, errors and CAN FD are not modeled. |
| Configuration file and command line | Same format as for the Vector SIL Kit Network Simulator. |

- Running Vector's executable instead (it needs a CANoe license) has not been verified yet.
- In peers that use `SilKitCanPort`, set `SilKitCanPortOptions::transmit_queue_capacity` to no more than the simulator's `--transmit-queue` (default 64).
- If the simulator's transmit queue overflows, frames are not sent.
- For the rules, the assumptions for determinism and the limitations, see "Bus timing (`CanBusModel`)" and "Simple CAN Network Simulator (`vehicle_sil::can_netsim`, `vehicle_sil_can_bus_simulator`)" in [internals/can.md](internals/can.md).

## See also

| Read | For |
|---|---|
| "[Writing an ECU](../README.md#writing-an-ecu)" in the README, [j1939-guide.md](j1939-guide.md) | How to write ECUs and nodes; read these before this page |
| [internals/virtual-time.md](internals/virtual-time.md) | The rules of virtual time |
| [internals/can.md](internals/can.md) | The CAN foundation |
