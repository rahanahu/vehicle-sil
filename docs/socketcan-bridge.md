# SocketCAN Bridge

[日本語](ja/socketcan-bridge.md)

`vehicle_sil_socketcan_bridge` forwards classic CAN frames in both directions between a SIL Kit CAN network and a SocketCAN interface of the Linux computer it runs on: a CAN adapter (`can0`) or a virtual one (`vcan0`). It needs no privileges and sets nothing on the interface. This page covers the connection steps, the arguments, the list of interfaces, the lines it prints and what is and is not verified. The Vector XL bridge for Windows ([bridges.md](bridges.md)) has the same arguments where they mean the same, the same statistics, and the same lines for programs.

The launcher starts this bridge from `--connect`: on Linux, `vehicle_sil_launch run --connect CAN=can0 simulation.yaml` connects the CAN network `CAN` to the interface `can0` (the backend `socketcan` is the default on Linux for a target that is not `virtual:<n>` or `physical:<n>`; see "[`--connect`](simulation-config.md#--connect)" in simulation-config.md). You can also start it by hand, or list it as a participant with `describe: true` in the configuration file (see "Starting from a configuration file" in [bridges.md](bridges.md#starting-from-a-configuration-file)).

| Connects to | `--interface` | Requirement | Verified | Not verified |
|---|---|---|---|---|
| Virtual CAN | `vcan0` | Linux with the `vcan` module (`sudo modprobe vcan`) | The whole bridge, in a network namespace of the tests | — |
| CAN adapter (USB, PCIe, SPI) | `can0` | Linux, an adapter with a SocketCAN driver, the interface set up | Error frames, bus-off and the transmit queue, against a fake socket layer | A real adapter on a real bus |

## First connection

```sh
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set vcan0 up

sil-kit-registry --listen-uri silkit://127.0.0.1:8500 &
vehicle_sil_socketcan_bridge --registry-uri silkit://127.0.0.1:8500 --network J1939_CAN1 --interface vcan0
candump vcan0        # from can-utils: the frames of the simulation
cansend vcan0 123#1122   # a frame into the simulation
```

For an adapter, set the bit rate and bring it up first (this needs root, and the bridge never does it):

```sh
sudo ip link set can0 type can bitrate 250000 restart-ms 100
sudo ip link set can0 up
vehicle_sil_socketcan_bridge --registry-uri silkit://127.0.0.1:8500 --interface can0 --bitrate 250000
```

## Arguments

| Argument | Meaning |
|---|---|
| `--registry-uri <uri>` | SIL Kit registry (required, except for `--describe` and `--list-channels`) |
| `--interface <name>` | The SocketCAN interface (required) |
| `--network <name>` | SIL Kit CAN network (`J1939_CAN1`) |
| `--name <name>`, `--participant <name>` | SIL Kit participant name (`SocketCanBridge`) |
| `--bitrate <bit/s>` | The bit rate the interface must run at (15000..1000000). The bridge does not set it |
| `--tx-timeout-ms <ms>` | Drop frames while the bus took none for this long (100..60000) |
| `--stats-every-ms <ms>` | Print the statistics line while running (100..3600000) |
| `--start-timeout-ms <ms>` | How long to wait for the participant to run (15000) |
| `--describe` | Write what the bridge is as JSON and exit, without opening the interface |
| `--list-channels [--json]` | List the CAN interfaces and exit |

Exit codes: 0 stopped normally, 1 failed while running (SIL Kit ended, or the interface is gone), 3 setup failed, 4 bad command line. The statistics, the health lines and the shutdown are those of the Vector XL bridge: see "[Statistics](bridges.md#statistics)", "[Exit codes](bridges.md#exit-codes)" and "[Health lines](bridges.md#health-lines)" in [bridges.md](bridges.md); the counts are in "Statistics" there (`ext_*` for the outside, `own_tx_dropped` for the receipts of the bridge's own frames).

### The interface list

`--list-channels` prints the interfaces of link type CAN, and `--json` the form of the other lists (`"vehicle_sil_targets": 1`, one target per interface, `value` the interface name):

```
CAN interfaces (--interface):
  vcan0  virtual CAN, up
  can0  CAN, up, 250000 bit/s, error-active; in use by a vehicle-sil bridge (process 4321)
```

Each target has `can: {kind, up, bitrate, state, restart_ms}` (`kind` is `vcan` or `can`; `state` is the controller's state, `null` for vcan), and `in_use`, `in_use_by` and `in_use_count` as in "[Device in use by another bridge](bridges.md#device-in-use-by-another-bridge)". An interface that is down is listed and `usable`; its `detail` says down. The list is read from the kernel (netlink) without privileges.

## Starting

The bridge refuses to start (exit code 3, with the way to fix it) when

- there is no such interface (the message lists the ones there are);
- the interface is down: `sudo ip link set can0 up`;
- `--bitrate` differs from the bit rate of the adapter: `sudo ip link set can0 down && sudo ip link set can0 type can bitrate 250000 && sudo ip link set can0 up`. Without `--bitrate` the bridge takes the interface as it is. Setting the bit rate needs `CAP_NET_ADMIN`, which the bridge does not ask for;
- the registry does not answer.

On a vcan interface, which has no bit rate, `--bitrate` is noted in a `warning:` line and ignored. On an adapter whose `restart-ms` is 0, the bridge warns: after a bus-off the controller then stays off the bus until someone restarts it.

## Frames

- The bridge opens one raw CAN socket on the interface and has the kernel give it back the frames it sends itself (`CAN_RAW_RECV_OWN_MSGS`). A frame with the `MSG_CONFIRM` mark is the receipt of the bridge's own frame: it is counted in `own_tx_dropped` and not forwarded.
- Every other frame is forwarded and counted in `ext_from_devices`, **including frames that other programs of this computer send on the interface** (`cansend`, `cangen`, another bridge's socket): they are the devices the bridge is for, on `vcan0` as on `can0`. The kernel marks them `MSG_DONTROUTE`; the bridge does not use the mark to leave them out.
- Classic CAN only. CAN FD frames are not received (the bridge does not enable `CAN_RAW_FD_FRAMES`, so the kernel does not hand them to the socket and nothing counts them), and SIL Kit frames that are not classic CAN are counted in `silkit_rejected`. A DLC of 9..15 is sent as 8 bytes.
- The kernel's receive queue of the socket can overflow on a busy bus without the bridge being told (`overruns` stays 0).
- A full transmit queue of the interface (`ENOBUFS`) drops the frame and counts it in `ext_queue_full`. With `--tx-timeout-ms`, frames are dropped (`ext_tx_dropped`) while the oldest frame without a receipt is older than that, and one frame in the same interval goes out as a probe; a receipt ends the drops. Only that time counts, never a count of frames, as in the Vector XL bridge: a receipt the kernel never gives back (its queue was emptied by a bus-off, or the receipt overflowed) is forgotten once the bridge sent nothing for the timeout, and when the interface comes up again.
- `ext_tx_unsent` stays 0: the bridge does not learn of frames the kernel lost from its queue. The receipt (`MSG_CONFIRM`) comes after the frame reached the bus only where the driver echoes it; on a driver that does not (not verified), the receipt comes when the frame is handed over, so `own_tx_dropped` does not count frames that went out on the bus and `--tx-timeout-ms` does not see a stuck bus.

## Link line

Besides `bus` the bridge prints `link=<up|down>` for programs, then the keys of the device mark:

```
[bridge] link bus=active link=up device=552dd508e2f03efe marked=1 shared=0
```

`bus` is `active`, `passive`, `bus-off` or `unknown`, as in "[Link line](bridges.md#link-line)". `unknown` is the only value of a vcan interface. `link=down` is an interface that is down (`IFF_UP` cleared); the bridge also prints a `warning:` line when it goes down and when it is up again. The bridge looks at the interface four times a second, and at once when a send is refused with `ENETDOWN`. It keeps running while the interface is down and receives and sends again when it is up: no frame is sent meanwhile (`ext_tx_failures` counts the refusals). How long a down interface is tolerated is the launcher's decision.

## Bus state and `restart-ms`

The bridge asks for the error frames (`CAN_RAW_ERR_FILTER`) and reads the controller state from the kernel (netlink) every look. Error passive, bus-off and a restart set `bus`; every error frame counts in `error_frames` and appears in the health line "CAN bus 'can0': ...". The kernel, not the bridge, leaves bus-off: it restarts the controller `restart-ms` milliseconds after a bus-off, if that is not 0. A vcan interface sends no error frames.

## When the interface goes away

`ENODEV` and `ENXIO` (an adapter unplugged, `ip link del`) and an interface that is gone or renamed at a look end the bridge: `FAIL: the interface is gone ...` and exit code 1. When the launcher started the bridge from `--connect`, it reports this as a lost connection (`connect-lost`).

## Device in use by another bridge

The bridge places the mark of "[Device in use by another bridge](bridges.md#device-in-use-by-another-bridge)" on the interface (the identity is the interface name). A second bridge on `vcan0` makes both warn and print `shared=1`, and the list says in use. Two bridges on one interface receive each other's frames and mix their networks: the bridge does not stop.

## Build

Linux only; CMake builds `vehicle_sil_socketcan_bridge` with everything else (no extra library). The kernel needs SocketCAN (`can_raw`; `vcan` for virtual interfaces). `scripts/package.py tools` puts the executable into `bin/` of the Linux archive.

## Test coverage

| Test | Checks |
|---|---|
| `socketcan_bridge.*` | The command line, the netlink answers and the checks of an interface, the lists, the error frames, and `SocketCanPort` on a fake socket layer: receipts, error frames, bus-off, `ENOBUFS`, `ENETDOWN`, `ENODEV`, the transmit timeout |
| `socketcan_bridge.netns.*` | The executable on `vcan0` in a network namespace: the round trip of the frame matrix, receipts, frames of another socket, down and up, removal, the refusals, the list, a second bridge, and `vehicle_sil_launch run --connect J1939_CAN=vcan0` starting the bridge beside the engine ECU of `examples/ecu_main` |

The `netns` tests are skipped (77) where a network namespace or `vcan0` cannot be made (no `vcan` module: `sudo modprobe vcan`, or user namespaces turned off); with `VEHICLE_SIL_REQUIRE_SOCKETCAN=1` they fail instead, as they do in CI, which loads the modules.

**Verified**: forwarding both ways on `vcan0`, the receipt of the bridge's own frames, frames of other sockets, down and up, removal, the second bridge, the list. **Not verified**: a physical CAN adapter on a real bus (the bit rate check, error passive, bus-off and the restart by `restart-ms`, an unplugged adapter) and the error frames against the real kernel (the tests play them through a fake socket layer); the rate at which the kernel's receive queue overflows.
