# CAN, Ethernet and LIN buses

[日本語](ja/code-map-buses.md)

<!-- Style: write each entry as a short block of the same shape (a subheading, a bullet list with one fact per line, a table whose cells are one sentence each). Do not pack several facts into one cell or one bullet: the line gets long and unrelated changes hit the same line. -->

This is the map for changes in this area. Start at [code-map.md](code-map.md); common test rules are in [code-map-testing.md](code-map-testing.md).

Related specifications and change areas: [internals/can.md](internals/can.md) / [internals/ethernet.md](internals/ethernet.md) / [internals/lin.md](internals/lin.md)

## Test labels

### `can`

The `unit` label is also set.

| Test | Covers |
|---|---|
| `can.*` | Unit tests of `vehicle_sil::can` (built without SIL Kit) |
| `can_silkit.*` | The SIL Kit port |
| `can_netsim.*` | The simple CAN Network Simulator |

What they check:

- Frame validation
- Receive order and overflow on the virtual bus
- The order of the host's phases and its failures, the order of the step driver
- Transmission time and arbitration (11-bit / 29-bit), congestion, the transmit queue
- Independence from the receiver's step size
- Port events (transmit results, receive queue overflow, bus state, error frames)
- Fault injection and disconnection of a single ECU
- `post()`, rejection of mixed time domains
- The real-time driver (deterministic tests on a substituted clock, and arrival and order on the wall clock)

For the simple CAN Network Simulator, they check the following both with in-process participants and with executables:

- The receive times are the same as on the virtual bus
- Repeated runs give the same result
- A sender without transmit times
- Controller modes (frames sent by a controller that has not started are dropped, `Reset()` cancels transmissions, delivery to a stopped controller)
- Loading the configuration file

With an in-process registry, they check:

- The trace of the same scenario is the same on the virtual bus and on SIL Kit (for several combinations of step sizes)
- Participants can be stopped and destroyed in a fixed order while transmissions continue

### `eth`

The `unit` label is also set.

| Test | Covers |
|---|---|
| `eth.*` | Unit tests of `vehicle_sil::eth` (built without SIL Kit) |
| `eth_silkit.*` | The SIL Kit port |
| `eth.steps_do_not_allocate` | No memory allocation inside a step (an executable of its own) |
| `eth.boundary` | `libs/eth` includes no SIL Kit header, directly or indirectly |

What they check:

- The frame and MAC address types
- Virtual network: delivery of frames as a hub, reception in the next step, padding of short frames, no reception of a node's own frames, time order, overflow
- Channel: runs on `step::Host` and `StepDriver`, gateways, frames with the same time ordered by source address
- Channel: `on_frame()` and `receive()`, the transmit phase, limits, filters, port events, option errors
- The port against a fake controller (including frame times from a sender without time synchronization, and the acknowledgement of a transmission that overflowed)
- With an in-process registry, the record of the same scenario is the same on the virtual network and on SIL Kit (for several combinations of step sizes)

The last item includes 3 senders that send at the same time. Their addresses are in a different order from their registration order, and the test checks the address order on the virtual network as well.

## Code and tests by directory

### `libs/can`

| Name | Location |
|---|---|
| `Frame` | `can/frame.hpp` |
| `Port` | `can/port.hpp` |
| `Host`, `Steppable` | `can/host.hpp` |
| `StepDriver`, `RealTimeDriver`, clocks | `can/step_driver.hpp`, `can/real_time_driver.hpp`, `can/real_time_clock.hpp` |
| `RealTimeReceiver` | `can/real_time_receiver.hpp` |
| `VirtualCanBus` | `can/virtual_can_bus.hpp` |
| `CanBusModel` | `can/bus_model.hpp` |
| `TapPort` | `can/tap_port.hpp` |
| `RawChannel` | `can/raw_channel.hpp` |

`Host` is built on `step::Host`. `StepDriver`, `RealTimeDriver` and the clocks are aliases of those in `vehicle_sil::step`.

- Tests: `tests/can`
- `ctest -R`: `^can\.`

### `libs/can_silkit`

| Name | Location |
|---|---|
| `SilKitCanPort`, the alias of `bind_host()` | `can_silkit/silkit_can_port.hpp` |

The body of `bind_host()` is in `core/bind_host.hpp`.

- Tests: `tests/can`
- `ctest -R`: `^can_silkit\.`

### `libs/can_netsim`, `tools/can_bus_simulator`

| Name | Location |
|---|---|
| `CanBusSimulator` | `can_netsim/can_bus_simulator.hpp` |
| `NetworkSimulatorConfig` | `can_netsim/network_simulator_config.hpp` |

- Tests: `tests/can`
- `ctest -R`: `^can_netsim\.`

### `libs/eth`, `libs/eth_silkit`

| Name | Location |
|---|---|
| `Frame`, `MacAddress` | `eth/frame.hpp` |
| `Port`, `PortEvent` | `eth/port.hpp` |
| `Channel` | `eth/channel.hpp` |
| `VirtualEthernetBus` | `eth/virtual_ethernet_bus.hpp` |
| `SilKitEthPort` | `eth_silkit/silkit_eth_port.hpp` |

- Tests: `tests/eth`
- `ctest -R`: `^eth\.`, `^eth_silkit\.`

### `libs/lin`, `libs/lin_silkit`

| Name | Location |
|---|---|
| `LinMaster` | `lin/lin_master.hpp` |
| `LinSlave` | `lin/lin_slave.hpp` |
| `LinPort` | `lin/lin_port.hpp` |
| `VirtualLinBus` | `lin/virtual_lin_bus.hpp` |
| `SilKitLinPort` | `lin_silkit/silkit_lin_port.hpp` |

- Tests: `tests/lin`
- `ctest -R`: `^lin_bus\.` (virtual LIN bus), `^lin_local\.` (on `LocalBench`), `^lin\.` (on SIL Kit), `^lin_cluster\.` (the processes of `examples/lin`)
