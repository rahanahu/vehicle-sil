# Ethernet Bridge

[日本語](ja/ethernet-bridge.md)

`vehicle_sil_ethernet_bridge` forwards Ethernet frames, unchanged, in both directions between a SIL Kit Ethernet network and an interface outside SIL Kit. It runs in real time and does not take part in time synchronization. This page covers what is particular to this bridge: the connection steps, arguments, the behavior of each backend, filtering and loop prevention, the library API and what is verified. What all bridges share with the launcher (the arguments it passes, `--describe`, the exit codes, the lines a bridge prints and the mark on a device) is in [bridge-contract.md](bridge-contract.md).

| Connects to | `--backend` | OS | What to install | Verified |
|---|---|---|---|---|
| The network stack of the same computer's OS, and programs that open the TAP device | `tap` | Linux | Nothing | Automated tests against the real kernel in a network namespace |
| Devices beyond a network interface (NIC, veth) | `packet` | Linux | Nothing | Automated tests on a veth pair (physical NICs not verified) |
| Same as `packet` | `pcap` | Linux | libpcap | Automated tests on a veth pair with the real libpcap (physical NICs not verified) |
| Devices beyond a network adapter | `npcap` | Windows | Npcap | With the real Npcap, only manual checks: on the virtual NIC of a virtual machine, and on one physical USB adapter on a LAN |

> [!WARNING]
> On Windows, `npcap` is verified against the real Npcap only by hand: on the virtual NIC of a virtual machine, and on one physical USB adapter passed through to that virtual machine, on a LAN with a computer's network stack as the peer. It is not verified with a device or a second computer on a cable, or with high traffic on a physical NIC. The tests run against a fake library that has the same functions. For what is verified, see "[Verified and not verified](#verified-and-not-verified)".

The prebuilt tools archive (see "[Prebuilt tools](build.md#prebuilt-tools)" in build.md) has `vehicle_sil_ethernet_bridge` in `bin/`, next to `vehicle_sil_launch`, so `vehicle_sil_launch run --connect` finds it without a change to `PATH`. Npcap and libpcap are not in the archive. In a build of the repository, the bridge is `build/<preset>/bridges/ethernet/vehicle_sil_ethernet_bridge`.

For the Vector XL CAN Bridge, see [bridges.md](bridges.md). It is not in the prebuilt tools archive: build it yourself and put it next to `vehicle_sil_launch` or in a directory on `PATH`.

## First connection

These steps connect the bridge to a TAP device inside the computer, on Linux. With a TAP device that you created beforehand and own, you need no external library and no privileges at run time. The steps assume that `vehicle_sil_ethernet_bridge` and `sil-kit-registry` are in `PATH`.

| Step | What you do | What you see |
|---|---|---|
| 1 | Create a TAP device owned by your user and bring it up. | `--list-interfaces` shows `vsil0  Ethernet, up`. |
| 2 | Start the SIL Kit registry. | The registry listens on `silkit://127.0.0.1:8500`. |
| 3 | Start the bridge in another terminal. | The line `bridging SIL Kit network ...` appears. |
| 4 | Stop the bridge with Ctrl+C. | `stopping`, the statistics and `stopped` appear in that order, and the bridge exits with code 0. |

The commands for step 1:

```sh
sudo ip tuntap add dev vsil0 mode tap user $USER
sudo ip link set vsil0 up
vehicle_sil_ethernet_bridge --list-interfaces
```

The command for step 2:

```sh
sil-kit-registry --listen-uri silkit://127.0.0.1:8500
```

The command for step 3, and what the bridge prints when it is connected:

```sh
vehicle_sil_ethernet_bridge --registry-uri silkit://127.0.0.1:8500 --network ETH1 --backend tap --interface vsil0
```

```
[eth-bridge] bridging SIL Kit network 'ETH1' (participant 'EthernetBridge', silkit://127.0.0.1:8500) <-> tap 'vsil0' (TAP device in this computer): every frame, both ways; MAC guard on; Ctrl+C to stop
```

Step 4 prints the following. If the OS sent frames to the TAP device, `ext_to_silkit` is not 0. On a TAP device the other end is the OS, so its frames count as from a device (`ext_from_devices`, see "[Statistics](#statistics)").

```
[eth-bridge] stopping
[eth-bridge] stats silkit_to_ext=0 ext_to_silkit=6 ext_from_devices=6 own_tx_dropped=0 loop_suspected=0 mac_unguarded=0 filtered_to_ext=0 filtered_from_ext=0 oversize=0 malformed=0 ext_overruns=0 ext_queue_full=0 ext_tx_failures=0 silkit_tx_failures=0 ext_checksum_incomplete=0
[eth-bridge] stopped
```

If something goes wrong, find the symptom in this table.

| Symptom | Cause | Fix |
|---|---|---|
| Prints `error: ...` and exits with code 4 | An argument is wrong. | Correct it as described in "[Arguments](#arguments)". |
| `FAIL setup: cannot connect to the SIL Kit registry ...` | The bridge cannot connect to the registry. | Do what the message says: start the registry or correct the address. |
| `FAIL setup: no TAP device 'vsil0' ...` and similar | The bridge cannot open the TAP device. | Follow the fix for that message in the list in "[`tap`](#tap)". |
| The warning `interface 'vsil0' is down: ...` | The TAP device is down. | Run `sudo ip link set vsil0 up`. |

### Connect to a network interface (Linux)

To connect to a network interface, the bridge needs `CAP_NET_RAW`, and you must say what may pass.

```sh
sudo setcap cap_net_raw+ep ./vehicle_sil_ethernet_bridge
vehicle_sil_ethernet_bridge --registry-uri silkit://127.0.0.1:8500 --network ETH1 --backend packet --interface enp3s0 \
    --ethertype 0x88b5
```

### Connect to a network adapter (Windows)

Install Npcap first (see "[`npcap` and `pcap`](#npcap-and-pcap)"). Write the `--interface` name exactly as `--list-interfaces` shows it.

```bat
vehicle_sil_ethernet_bridge --list-interfaces
vehicle_sil_ethernet_bridge --registry-uri silkit://127.0.0.1:8500 --network ETH1 --backend npcap --interface "Ethernet 2" ^
    --ethertype 0x88b5
```

## Arguments

```
vehicle_sil_ethernet_bridge --list-interfaces [--backend <backend>] [--json]
vehicle_sil_ethernet_bridge --registry-uri <silkit://host:port> --network <name> [--backend <tap|packet|npcap|pcap>] --interface <name> [options]
vehicle_sil_ethernet_bridge --describe --network <name> [--backend <backend>] --interface <name> [options]
```

| Argument | Meaning |
|---|---|
| `--registry-uri <uri>` | Address of the SIL Kit registry (required). |
| `--network <name>` | Name of the SIL Kit Ethernet network (required; there is no default name). |
| `--name <name>` | Participant name (default `EthernetBridge`). |
| `--participant <name>` | The same as `--name` (give one of them). |
| `--start-timeout-ms <ms>` | How long the bridge waits for its participant to reach Running in SIL Kit (1–10^12, default 15000). If it does not, the bridge exits with code 3. |
| `--stats-every-ms <ms>` | Also prints the statistics line while the bridge runs: at the first interval, then at each interval where a count changed (100–3600000). Without it, the line comes once, when the bridge stops (see "[Statistics](#statistics)"). |
| `--describe` | Writes what the bridge is as JSON to stdout and exits (see "[Starting from a configuration file](#starting-from-a-configuration-file)"). |
| `--backend <tap\|packet\|npcap\|pcap>` | How the external interface is opened (default: `packet` on Linux, `npcap` on Windows; see "[Backends](#backends)"). |
| `--interface <name>` | The interface to connect to (required; the bridge never picks "the first one found"). |
| `--ethertype <hex>,...` | EtherTypes that may pass (hexadecimal starting with `0x`; repeating the option adds more). |
| `--src-mac <mac>,...` | MAC addresses of the SIL Kit side (6 bytes separated by `:` or `-`; repeating the option adds more). |
| `--all-frames` | No filtering (cannot be combined with `--ethertype` or `--src-mac`). |
| `--vlan-tpid <hex>,...` | Values that mark a VLAN tag (default `0x8100,0x88a8`). |
| `--max-frame-bytes <n>` | Maximum length without FCS (60–65535, default 1522). |
| `--promiscuous` | Receives frames to every address, and fails to start if the interface refuses promiscuous mode (not available with `tap`). |
| `--no-promiscuous` | Receives only frames to the interface's own address and to group addresses (not available with `tap`). |
| `--tap-create` | Creates the TAP device for as long as the bridge runs (`tap` only). |
| `--no-mac-guard` | Does not use the MAC rules (see "[Loop prevention](#loop-prevention-macsidetable)"). |
| `--mac-table-size <n>` | Size of the MAC table (16–65536, default 1024). |
| `--list-interfaces` | Prints the interfaces for the backend and exits (needs no other arguments; see "[Listing interfaces](#listing-interfaces---list-interfaces)"). |
| `--json` | With `--list-interfaces` only: writes the list as JSON to stdout, for programs (see "[The list as JSON](#the-list-as-json---json)"). |
| `--help` | Prints the usage. |

Notes on the arguments:

- `--network` has no default name so that the bridge never connects to the wrong network by mistake.
- Without `--promiscuous` and `--no-promiscuous`, the bridge uses promiscuous mode where the interface accepts it, and runs without it, with one warning, where it refuses (see "[Promiscuous mode](#promiscuous-mode)").
- The `tap` backend is treated as a virtual interface inside the computer; the others are treated as able to put frames on a physical adapter. `tap`, `packet` and `pcap` are available on Linux, `npcap` on Windows.
- The minimum of `--max-frame-bytes` is 60 because SIL Kit does not pass on frames shorter than 60 bytes.
- Without `--backend`, the bridge uses the backend of its OS: `packet` on Linux, `npcap` on Windows. The list of `--list-interfaces` and the bridge itself use the same backend.
- `--list-interfaces` with `--backend npcap` or `--backend pcap` prints the list that the pcap library returns.
- On Windows, the bridge reads its arguments as UTF-8 from the wide-character command line. `--name`, `--network` and `--interface` can therefore contain non-ASCII characters.

With a backend that can put frames on a physical adapter (`packet`, `npcap`, `pcap`), given with `--backend` or by default, you must say what may pass: one of `--ethertype`, `--src-mac` or `--all-frames`. Without it, the bridge reports this error, with the name of the backend:

```
--backend npcap sends to a real adapter: say what may pass with --ethertype, --src-mac or --all-frames.
```

`parse_bridge_options()` in `vehicle_sil/ethernet_bridge/bridge_options.hpp` parses the arguments. An error is a `BridgeUsageError` that says what is wrong and how to fix it. `bridge_usage()` returns the full usage.

## Starting from a configuration file

`vehicle_sil_launch` can start the bridge with the other participants of a simulation. Write it in the configuration file like an ECU (`describe: true`, the default), with `role: bridge` and `required: false`:

```yaml
participants:
  - name: EthBridge
    role: bridge
    required: false
    command: [vehicle_sil_ethernet_bridge, --network, ETH_EXT, --backend, npcap,
              --interface, "Ethernet 2", --src-mac, "02:00:00:00:00:01"]
```

On Linux, write `--backend packet` (or `pcap`) and the name of the network interface. The network (`ETH_EXT`) must be in `networks` with `kind: ethernet`.

- What the launcher adds, and what `--describe` writes, are in "[Starting a bridge of your own](bridge-contract.md#starting-a-bridge-of-your-own-role-bridge)" and "[`--describe`](bridge-contract.md#--describe)" in bridge-contract.md. This bridge does not open the interface, load the pcap library or connect to SIL Kit for `--describe`, so `check` works on a computer without Npcap or libpcap.
- `required: false`: the simulation does not wait for the bridge, and when the run ends, the launcher asks the bridge to stop (see "[Startup order](bridges.md#startup-order)" in [bridges.md](bridges.md) for frames sent before the bridge joins).

To have the launcher start the bridge without an entry in the file, give `--connect <network>=<target>` to `check` and `run` (see "[`--connect`](simulation-config.md#--connect)" in simulation-config.md). The adapter or channel differs from PC to PC, so it is given when you start, not written in the shared file. `run` then starts the bridge before the ECUs, waits until it runs, so that the first frames reach the bus too, and fails the run if the bridge ends (see "[Connecting a bus outside](simulation-run.md#connecting-a-bus-outside---connect)" in simulation-run.md).

### Trying `run --connect` on one computer (Linux)

A veth pair stands in for the cable: the bridge opens one end (`vsil0`), and `tcpdump` watches the other end (`vsil1`), where a device would be. The steps use the example `examples/ethernet` of a build of this repository (`build/<preset>`). Its two ECUs talk on the network `ETH` and report their MAC addresses, so the bridge passes their frames without an `external` entry in the file. In a build, the bridge is not next to `vehicle_sil_launch`, so the steps put its directory on `PATH`.

| Step | What you do | What you see |
|---|---|---|
| 1 | Create a veth pair and bring both ends up. | `vehicle_sil_ethernet_bridge --list-interfaces` lists `vsil0` and `vsil1` as `Ethernet, up`. |
| 2 | Give the bridge `CAP_NET_RAW` (see "[`packet`](#packet)"). | Nothing. |
| 3 | In another terminal, watch the other end. | `tcpdump` waits. |
| 4 | Run the example at speed 1 with `--connect ETH=vsil0`. | The line `outside: ETH -> vsil0 (bridge.ETH)`, `bridge.ETH` in the participant table with `stopped (SIGTERM), exit code 0`, and `passed: 0 errors, 0 warnings, 0 notes`. |
| 5 | Look at what `tcpdump` printed. | The frames between Sensor (`02:00:00:00:00:01`) and Display (`02:00:00:00:00:02`), EtherType `0x88b5`, about 20 in the second the example runs. |
| 6 | Remove the veth pair and the capability. | Nothing. |

The commands for steps 1 and 2:

```sh
sudo ip link add vsil0 type veth peer name vsil1
sudo ip link set vsil0 up
sudo ip link set vsil1 up
build/<preset>/bridges/ethernet/vehicle_sil_ethernet_bridge --list-interfaces
sudo setcap cap_net_raw+ep build/<preset>/bridges/ethernet/vehicle_sil_ethernet_bridge
```

The command for step 3:

```sh
sudo tcpdump -e -n -i vsil1 ether proto 0x88b5
```

The commands for step 4:

```sh
cd build/<preset>/examples/ethernet
PATH="$PWD/../../bridges/ethernet:$PATH" ../../tools/launch/vehicle_sil_launch run simulation.yaml --speed 1 --connect ETH=vsil0
```

The first frames that `tcpdump` prints in step 5 look like this:

```
02:00:00:00:00:01 > 02:00:00:00:00:02, ethertype Unknown (0x88b5), length 60:
02:00:00:00:00:02 > 02:00:00:00:00:01, ethertype Unknown (0x88b5), length 60:
```

The commands for step 6:

```sh
sudo ip link del vsil0
sudo setcap -r build/<preset>/bridges/ethernet/vehicle_sil_ethernet_bridge
```

- Without step 2, the run fails with `connect-failed`, and the bridge's line says which `setcap` command to run.
- The launcher gives the bridge `--backend packet` here, the default on Linux. To connect a TAP device that you created (see "[First connection](#first-connection)") instead, write the backend in the target: `--connect ETH=tap:vsil0`; on Windows the default is `npcap` (see "[`--connect`](simulation-config.md#--connect)" in simulation-config.md).
- A real device on a NIC is connected the same way: give `--connect ETH=<the NIC's name>` instead of `vsil0`. For an automatic test with the device, run a scenario with `--speed 1 --strict` (see "[Connecting a bus outside](simulation-run.md#connecting-a-bus-outside---connect)" in simulation-run.md).
- The test `ethernet_bridge.netns.launch` does the same in a network namespace, where it needs neither `sudo` nor `setcap` (see "[`ethernet_bridge.netns.*`](#ethernet_bridgenetns)").

## Exit codes

The exit codes (0, 1, 3, 4) and the judgment of the participant state are those of all bridges (see "[Exit codes](bridge-contract.md#exit-codes)" in bridge-contract.md). For this bridge, exit code 3 is a backend that is not available on this OS, a pcap library that is not installed, an interface that cannot be opened, or SIL Kit during setup (`FAIL setup: <reason>`); exit code 1 includes an external interface that stopped working (it disappeared, or reading failed) (`FAIL: <reason>`).

## Behavior from start to stop

The bridge goes through these steps in order:

| Order | What the bridge does | If it fails |
|---|---|---|
| 1 | Checks the arguments. | Prints only 2 lines to stderr, the error and a pointer to the usage, and exits with code 4. |
| 2 | Checks that `--backend` is available on this OS. | Prints the reason and a backend to use instead, and exits with code 3 before connecting to SIL Kit. |
| 3 | Connects to the SIL Kit registry and creates the Ethernet controller. | Prints the address it tried and what to do next, and exits with code 3. |
| 4 | Opens the external interface and starts forwarding (`EthernetBridge::start()`). | Says what happened and how to fix it, and exits with code 3. |
| 5 | Starts the participant lifecycle. | — |
| 6 | Prints the startup line and runs until a stop request or a failure. | — |

- In step 2, `npcap` and `pcap` load their library. If it is not installed, the bridge says how to install it and exits with code 3.
- For the messages of step 4, see the sections under "[Backends](#backends)".
- Forwarding (step 4) starts before the lifecycle (step 5) because other participants start sending as soon as they see the bridge reach Running.
- An argument error (`error: <reason>` and a pointer to the usage, for example `error: --backend must be tap, packet, npcap or pcap, got: usb`) and a registry that cannot be reached have the same form in all bridges (see "[Lines the bridge prints](bridge-contract.md#lines-the-bridge-prints)" in bridge-contract.md).

### The startup line

The startup line shows what is connected to what, what may pass, and the state of promiscuous mode and of the MAC rules.

```
[eth-bridge] bridging SIL Kit network 'ETH1' (participant 'EthernetBridge', silkit://127.0.0.1:8500) <-> packet 'enp3s0' (network interface): EtherType 0x88b5 only, both ways; promiscuous on; MAC guard on; Ctrl+C to stop
```

| Part | Text |
|---|---|
| What may pass | `every frame, both ways`, `EtherType 0x88b5,0x0800 only, both ways`, `SIL Kit MAC 02:00:00:00:00:01 only, both ways`, or the last 2 joined with `and` |
| Promiscuous mode in effect | `promiscuous on`; with `--no-promiscuous`, `promiscuous off (only frames to the interface's own address and to group addresses arrive)`; when the interface refused it, `promiscuous off (the interface does not accept it: only frames to its own address and to group addresses arrive)` (not shown for `tap`) |

### Output while running

| Output | Form | When |
|---|---|---|
| Warning | `[eth-bridge] warning: <text>` | At most once a second |
| Health line (see "[Health lines](#health-lines)") | `[eth-bridge] interface <name>: ...` | At most once a second |
| Link line (see "[Link line](#link-line)") | `[eth-bridge] link link=<value> device=<hash> marked=<0/1> shared=<n>` | Once before the SIL Kit lifecycle starts, then when a value changes |
| Statistics line, with `--stats-every-ms` (see "[Statistics](#statistics)") | `[eth-bridge] stats silkit_to_ext=<n> ...` | At the first interval, then at each interval where a count changed |

### Stopping

A stop request is Ctrl+C, Ctrl+Break, SIGTERM or, on Windows, closing the console window. The bridge then prints `stopping`, the statistics line (see "[Statistics](#statistics)"), `last error: <text>` if there was an error, and `stopped`, in that order, and ends. Warnings raised just before the stop are printed after `stopping` and before the statistics.

The stop sequence:

| Order | What happens |
|---|---|
| 1 | The SIL Kit handler is removed. |
| 2 | External receiving stops (the interface is closed). |
| 3 | The statistics are printed. |
| 4 | The lifecycle is stopped. |

### Promiscuous mode

Without promiscuous mode, a network adapter passes on only frames to its own MAC address and to group addresses. The devices on the SIL Kit side have addresses of their own, so unicast frames to them arrive only in promiscuous mode. The bridge therefore uses it by default (`packet`, `npcap`, `pcap`; a TAP device gets every frame anyway).

| Arguments | Promiscuous mode |
|---|---|
| Neither (default) | On where the interface accepts it. Where it refuses, the bridge runs without it and prints one warning (below). |
| `--promiscuous` | On. Where the interface refuses it, the bridge fails to start (exit code 3). |
| `--no-promiscuous` | Off. |

The warning when the interface refuses promiscuous mode (the text in parentheses is the OS's or the library's):

```
interface 'Wi-Fi' does not accept promiscuous mode (result 2, not supported), so the bridge runs without it there: only frames to the interface's own address and to group addresses arrive, and frames to the devices of the SIL Kit side (they have addresses of their own) do not. If devices outside must reach them, use an adapter that accepts promiscuous mode (a wired one usually does).
```

The startup line says which mode is in effect (see "[The startup line](#the-startup-line)"). How a refusal shows, by backend:

| Backend | Refusal the bridge can tell | Refusal it cannot tell |
|---|---|---|
| `packet` | The kernel refuses the socket setting with an error. | A driver that accepts the setting and ignores it (some wireless drivers): the bridge takes the interface for promiscuous. |
| `pcap` | libpcap's warning that the interface does not support it (result 2), or its error that this user may not use it (result -11). | As with `packet`. |
| `npcap` | Opening with promiscuous mode fails and opening without it works. | An adapter that opens with promiscuous mode and ignores it, without a word (Npcap's way of opening passes no warning on). |

Where the bridge cannot tell, the default behaves like `--promiscuous`: the startup line says `promiscuous on`, and the hint below does not appear.

### Hint when promiscuous mode is off

With promiscuous mode off (`packet`, `npcap`, `pcap`: `--no-promiscuous`, or an interface that refused it), if no frame addressed to a MAC address of the SIL Kit side has been received 5 seconds after the bridge sent its first frame out, the bridge prints a hint as a warning, once. Here, a MAC address of the SIL Kit side means the source address of a frame that the bridge sent out.

```
no frame to a MAC address of the SIL Kit side has come from packet 'enp3s0' (network interface) in the 5 s since the bridge first sent a frame there, and promiscuous mode is off (--no-promiscuous): without promiscuous mode an interface passes on only frames to its own address and to group addresses, so frames to the SIL Kit devices (they have other addresses) do not arrive. If devices outside are meant to answer, leave out --no-promiscuous.
no frame to a MAC address of the SIL Kit side has come from packet 'enp3s0' (network interface) in the 5 s since the bridge first sent a frame there, and the interface runs without promiscuous mode (it does not accept it, see the warning at the start): without promiscuous mode an interface passes on only frames to its own address and to group addresses, so frames to the SIL Kit devices (they have other addresses) do not arrive. If devices outside are meant to answer, use an adapter that accepts promiscuous mode (a wired one usually does).
```

The first is for `--no-promiscuous`, the second for an interface that refused promiscuous mode. In promiscuous mode there is no hint.

The reason for the warning: without promiscuous mode, an interface passes on only frames to its own address and to group addresses, and the SIL Kit devices have addresses other than the interface's. The warning also appears when you do not expect responses (when you only send frames out), so the text is conditional.

A frame also counts when its destination was not yet known as a MAC address of the SIL Kit side at the time the bridge received it (including frames received before the bridge's first transmission). The bridge remembers the destination of such a frame (not a group address), and if it later sends a frame with that address as the source, the frame to that address counts as received. The bridge remembers up to 32 such destinations and ignores the rest (those do not count as received). The limit exists because the bridge also receives the frames that this computer's OS sends from the same interface: many destination addresses therefore do not prove that the interface passes frames regardless of their destination.

### Hint when nothing comes from a device

With a backend that can put frames on a physical adapter (`packet`, `npcap`, `pcap`), if no frame has come from a device there 10 seconds after the bridge sent its first frame out (`ext_from_devices` is still 0, see "[Statistics](#statistics)"), the bridge prints a hint as a warning, once. A cable that is not plugged in, a device that is switched off, and the wrong adapter all look like this, and sending succeeds all the same: a Hyper-V `vEthernet` adapter, for example, stays connected without a cable on its physical NIC.

```
nothing has come from a device on packet 'enp3s0' (network interface) in the 10 s since the bridge first sent a frame there (frames from the addresses of this computer's adapters do not count): if a device is meant to answer, check that it is on, its cable, and that this is the adapter it is on.
nothing has come from a device on packet 'enp3s0' (network interface) in the 10 s since the bridge first sent a frame there (the bridge does not know the addresses of this computer's adapters, so frames this computer sends there would count): if a device is meant to answer, check that it is on, its cable, and that this is the adapter it is on.
```

- Frames that this computer sends there (ARP, IPv6) arrive whether a device is there or not, so they do not count: frames from the MAC address of any of this computer's adapters (the bridge's adapter, a `vEthernet` adapter whose switch is bound to it, a Linux bridge it is a port of, a macvlan on it), and with `packet` every frame the kernel marks as sent by this computer, whatever its address. The bridge learns the addresses from the backend once, when it starts (an adapter added or changed later is not among them): `packet` from the kernel, `pcap` from libpcap's list of interfaces, `npcap` from Windows (the physical address of every adapter). The first text is for when it knows them, the second for when it does not.
- Frames from another virtual machine on the same virtual switch count: the bridge cannot tell them from a device's, and they may come without a cable.
- The time counts from the first frame sent out. A frame from a device that came earlier keeps the hint back as well, and once a frame from a device has come, there is no hint.
- The hint also appears when no answer is expected (when you only send frames out), so the text is conditional.
- `tap` has no hint: the TAP device stays in this computer.
- The Vector XL CAN bridge gives the same hint for its channel (see [bridges.md](bridges.md#statistics)).

## Backends

| `--backend` | OS | Peer | Mechanism | Privileges needed |
|---|---|---|---|---|
| `tap` | Linux | The network stack of the same computer's OS, and programs that open that TAP device (Wireshark and others) | A TAP device of `/dev/net/tun` | None for a device that you created beforehand and own. `CAP_NET_ADMIN` for `--tap-create` |
| `packet` | Linux | Devices beyond a network interface (NIC, veth), and programs on the same computer that open the same interface with a packet socket or pcap | A packet socket (`AF_PACKET`) bound to the interface | `CAP_NET_RAW` |
| `pcap` | Linux | Same as `packet` | Loads the libpcap installed on this computer at run time | `CAP_NET_RAW` |
| `npcap` | Windows | Devices beyond a network adapter (NIC, Microsoft KM-TEST Loopback Adapter), and programs on the same computer that open the same adapter with Npcap | Loads the Npcap installed on this computer (`wpcap.dll`) at run time | Usually none. Administrator on a computer where Npcap was installed for administrators only |

- `packet` cannot talk to the network stack of the same computer's OS (see "[Talking to the network stack of the same computer](#talking-to-the-network-stack-of-the-same-computer)").
- `tap` and `packet` use only Linux kernel APIs and need no external library.
- `pcap` and `npcap` load the library installed on the computer at run time. vehicle-sil neither ships that library nor uses it in the build ([dependencies.md](dependencies.md)).
- On Linux, use `packet` as a rule: it needs nothing installed and can detect frames with incomplete checksums. `pcap` is for when you want to open the same interface with libpcap.

The bridge neither sets an IP address on the interface nor brings it up. If the interface is down, the bridge warns once at startup and says how to bring it up.

```
interface 'vsil0' is down: no frame passes until it is up. Bring it up with 'sudo ip link set vsil0 up'.
```

### Listing interfaces (`--list-interfaces`)

`--list-interfaces` prints the interfaces for the backend. For `packet` (the default on Linux) and `tap`, these are the interfaces of this computer, with whether each is Ethernet and whether it is up.

```
interfaces of this computer (--backend packet and --backend pcap take the Ethernet ones; --backend tap takes a TAP device):
  --interface lo      not Ethernet (link type 772), up
  --interface enp3s0  Ethernet, up
  --interface vsil0   Ethernet, down
```

An interface that vehicle-sil bridges use now ends its line with `; in use by a vehicle-sil bridge (process 4321)` (see "[Device in use by another bridge](bridge-contract.md#device-in-use-by-another-bridge)" in bridge-contract.md). For `npcap` (the default on Windows) and `pcap`, it prints the interfaces that the pcap library returns.

```
interfaces Npcap offers (Npcap version 1.80, based on libpcap version 1.10.5); --backend npcap takes the Ethernet ones:
  --interface "Ethernet 2"          up, connected  (Microsoft KM-TEST Loopback Adapter; \Device\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318})
  --interface Ethernet              up, disconnected  (Intel(R) Ethernet; \Device\NPF_{0A1B2C3D-0000-1111-2222-333344445555})
  --interface \Device\NPF_Loopback  up, loopback  (Adapter for loopback traffic capture)
```

| Part of the line | Content |
|---|---|
| First | The name to pass to `--interface` (a name with spaces is enclosed in `"`). |
| Next | The state that the library returns (`up` / `down`, `connected` / `disconnected`, `wireless`, `loopback`). |
| In parentheses | The description and the pcap name. |

- The names are padded to one width, counted in console columns: a wide character (Japanese, Chinese, Korean, full-width forms) counts as 2. The next column stays in line as long as the console's font draws those characters 2 columns wide.
- Interfaces that are not Ethernet (`any` on Linux, for example) are listed too. The bridge refuses to open them.
- If the Npcap list contains no network adapter (only `\Device\NPF_Loopback`), the bridge adds a last line about Npcap installed for administrators only and about disabled adapters (`Npcap offers no network adapter here: ...`).

#### The list as JSON (`--json`)

`--list-interfaces --json` writes the same list in the form of "[Lists of devices](bridge-contract.md#lists-of-devices)" in bridge-contract.md; this section gives what differs and an example.

```json
{
  "vehicle_sil_targets": 1,
  "tool": "vehicle_sil_ethernet_bridge",
  "backend": "npcap",
  "source": "Npcap version 1.80, based on libpcap version 1.10.5",
  "targets": [
    {
      "value": "Ethernet 2",
      "label": "Ethernet 2",
      "detail": "up, connected; Intel(R) Ethernet; \\Device\\NPF_{0A1B2C3D-0000-1111-2222-333344445555}",
      "usable": true,
      "reason": null,
      "in_use": false,
      "in_use_by": [],
      "in_use_count": 0,
      "ethernet": {
        "pcap_name": "\\Device\\NPF_{0A1B2C3D-0000-1111-2222-333344445555}",
        "connection": "Ethernet 2",
        "description": "Intel(R) Ethernet",
        "up": true,
        "connected": true,
        "loopback": false,
        "wireless": false
      }
    },
    {
      "value": "\\Device\\NPF_Loopback",
      "label": "\\Device\\NPF_Loopback",
      "detail": "up, loopback; Adapter for loopback traffic capture",
      "usable": false,
      "reason": "loopback: not an Ethernet adapter",
      "in_use": false,
      "in_use_by": [],
      "in_use_count": 0,
      "ethernet": {
        "pcap_name": "\\Device\\NPF_Loopback",
        "connection": null,
        "description": "Adapter for loopback traffic capture",
        "up": true,
        "connected": null,
        "loopback": true,
        "wireless": false
      }
    }
  ],
  "notes": [],
  "error": null
}
```

| Key | Content for this bridge |
|---|---|
| `tool` | `vehicle_sil_ethernet_bridge`. |
| `backend` | The backend the list is for: `--backend`, or the default. |
| `source` | `npcap`, `pcap`: the version the library reports. `packet`, `tap`: `null`. |
| `targets[].detail` | `detail` also ends with `in use by a vehicle-sil bridge (process 4321)` for a device in use. |
| `targets[].ethernet` | What the backend reports. `npcap`, `pcap`: `pcap_name`, `connection` and `description` (`null` where there is none), `up`, `connected` (`null` where the library does not say), `loopback`, `wireless`. `packet`, `tap`: `name`, `ethernet`, `link_type`, `up`. |
| `notes` | The explanations of the text's last line, one sentence each (Npcap that offers no network adapter); otherwise empty. |

`value` is the connection name where the device has one and no other device in the list has the same connection name, compared without the case of ASCII letters as `--interface` does (see "[Names accepted by `--interface`](#names-accepted-by---interface)"). Otherwise it is the pcap name. With `packet` and `tap` it is the interface name.

| Interface | `usable` | `reason` |
|---|---|---|
| Loopback (the library's flag) | `false` | `loopback: not an Ethernet adapter` |
| Not Ethernet (`packet`, `tap`) | `false` | `not Ethernet (link type <n>)` |
| `any` of libpcap | `false` | `not Ethernet` |
| Down or disconnected | `true` | `null`; `detail` says it, and the bridge warns at startup |
| Others | `true` | `null` |

The pcap library does not say whether a device carries Ethernet before it is opened: apart from the loopback and `any`, a device that is not Ethernet is listed as usable, and the bridge refuses it at startup. With `tap`, the list shows the interfaces of this computer; the bridge can open only a TAP device among them.

When the list cannot be made, the JSON has no targets and an error (see the contract for the exit code and where the `FAIL setup:` line goes).

```json
{
  "vehicle_sil_targets": 1,
  "tool": "vehicle_sil_ethernet_bridge",
  "backend": "npcap",
  "source": null,
  "targets": [],
  "notes": [],
  "error": {
    "code": "library-missing",
    "message": "Npcap is not installed: cannot load C:\\Windows\\System32\\Npcap\\wpcap.dll (Windows error 126). ..."
  }
}
```

| `error.code` | When |
|---|---|
| `library-missing` | The pcap library is not installed, or lacks a function the bridge needs. |
| `permission-denied` | The OS refused the list to this user. |
| `backend-unavailable` | The backend is not available on this OS (`--backend npcap` on Linux, for example). |
| `failed` | Anything else. |

### `tap`

A TAP device is a virtual Ethernet interface inside the computer, and the bridge is at the other end of its wire. A frame that the bridge sends becomes a frame that the OS receives on that interface. Frames that the OS or other programs send on that interface are received by the bridge.

- By default, the bridge connects to an existing device. If you created it with yourself as owner (`ip tuntap add ... user $USER`), no privileges are needed.
- With `--tap-create`, the bridge creates the device (needs `CAP_NET_ADMIN`). The device it creates is down, and it disappears when the bridge stops. Bringing it up is up to you (once it is up, the health line shows `link up`).
- No filter is needed (by default every frame passes), because the frames stay inside the computer. If you connect that TAP device to a NIC with an OS bridge, whoever connected it is responsible for the frames that leave the computer.
- Only one program can connect to a TAP device.
- Frames that the OS drops before handing them to the TAP device are invisible to the bridge (`ext_overruns` does not increase). Check them in the OS interface statistics (`ip -s link show <name>`).

The messages when the device cannot be opened (exit code 3):

```
no TAP device 'vsil0'. Make it first: 'sudo ip tuntap add dev vsil0 mode tap user $USER' and 'sudo ip link set vsil0 up'; or let the bridge make it while it runs with --tap-create (needs CAP_NET_ADMIN). Interfaces here: lo, enp3s0.
cannot attach to TAP 'vsil0': Operation not permitted. It belongs to another user. Make it yours first: 'sudo ip tuntap del dev vsil0 mode tap', then 'sudo ip tuntap add dev vsil0 mode tap user $USER' and 'sudo ip link set vsil0 up'.
cannot attach to TAP 'vsil0': Device or resource busy. Another program has it open, and a TAP device serves one program: stop that program, or use another TAP device.
interface 'lo' is not a TAP device. For a network interface use --backend packet; a TAP device is made with 'sudo ip tuntap add dev <name> mode tap user $USER'.
cannot create TAP 'vsil0': an interface with that name exists. Leave out --tap-create to attach to it, or choose another name.
cannot create TAP 'vsil1': Operation not permitted. Creating a TAP device needs CAP_NET_ADMIN: make it first with 'sudo ip tuntap add dev vsil1 mode tap user $USER' and 'sudo ip link set vsil1 up' and leave out --tap-create, or run as root.
cannot open /dev/net/tun: Permission denied. This user may not use TAP devices here: ask the administrator for read and write access to /dev/net/tun, or run as root.
cannot open /dev/net/tun: No such file or directory. The kernel's TUN/TAP driver is not there: load it with 'sudo modprobe tun'.
```

### `packet`

The bridge sends and receives frames unchanged through a packet socket bound to the interface.

- Needs `CAP_NET_RAW`. Grant it to the executable once (`sudo setcap cap_net_raw+ep <executable>`), or run as root. The error message shows the command with the path of this executable filled in.
- Frames can reach the physical network, so you must say what may pass (one of `--ethertype`, `--src-mac` or `--all-frames`).
- The bridge receives the frames that the interface receives, and the frames that the OS or other programs on the same computer send on that interface. It does not receive the frames it sends itself.
- Frames that the bridge sends only go out over the wire. They are received by the devices beyond the wire and by programs that have the same interface open with a packet socket or pcap. The network stack of the same computer's OS (programs that use ordinary sockets) does not receive them.
- The kernel removes the outer VLAN tag from the bytes of a received frame and passes it separately. The bridge puts it back in its original position, so tagged frames pass in both directions with their tags.
- Promiscuous mode (see "[Promiscuous mode](#promiscuous-mode)") is a socket setting for as long as the bridge runs; it is undone when the bridge stops.
- The bridge does not open interfaces that are not Ethernet (`lo`, for example).
- Frames that the OS drops before handing them to the socket are counted in `ext_overruns`.
- With receive offload (GRO, LRO) enabled, frames longer than the maximum length can appear. The bridge drops them as `oversize`. If this is a problem, turn off the offload on the interface (`sudo ethtool -K <name> gro off lro off`).

The messages when the interface cannot be opened (exit code 3):

```
no interface 'eth9' for --backend packet. Interfaces here: lo, enp3s0, vsil0 (run --list-interfaces).
interface 'lo' is not Ethernet (link type 772): --backend packet carries Ethernet frames only. Choose an Ethernet interface (run --list-interfaces).
cannot open a packet socket on 'enp3s0': Operation not permitted. A network interface needs CAP_NET_RAW: run 'sudo setcap cap_net_raw+ep /opt/vehicle-sil/bin/vehicle_sil_ethernet_bridge' once, or run as root.
interface 'wlp2s0' does not accept promiscuous mode (Operation not supported), and --promiscuous asks for it: leave out --promiscuous to let the bridge run without it (it then says so).
```

#### Talking to the network stack of the same computer

If a program on the same computer talks to the SIL Kit devices through ordinary OS sockets (UDP, TCP), use `tap`. With `packet`, 3 things happen:

| What happens | Statistics | Fix |
|---|---|---|
| The OS stack does not receive the responses from SIL Kit. | The counts of both directions increase. | Use `tap`, or use a veth pair. |
| Frames with incomplete checksums enter SIL Kit. | `ext_checksum_incomplete` | Run `sudo ethtool -K <name> tx off` on the interface that sends the frames. |
| Long TCP frames are dropped. | `oversize` | With the same `tx off`, the bridge sees the frames after segmentation. |

**Why the responses are not received**: the bridge receives the frames that the program sends and passes them to SIL Kit, but it only sends the responses from SIL Kit out over the wire, so they never reach the OS stack of the same computer. Only one direction passes. If the bridge opens one end of a veth pair and the program uses the other end with an IP address on it, both directions pass.

**Why the checksums are incomplete**: the OS offloads the checksum calculation of outgoing UDP and TCP to the NIC (transmit checksum offload). A packet socket sees the frame before the NIC calculates it, so the checksum field does not hold the correct value. The same applies to frames that the OS sends from the other end of a veth pair (there is no NIC to calculate it). The bridge neither drops these frames nor calculates the checksum: it passes them on as they are, counts them in `ext_checksum_incomplete` and warns the first time only. A receiver that verifies checksums (an ECU with its own TCP/IP stack) silently drops these frames. Turn off the offload on the interface that sends the frames (for a veth pair, the other end).

**Why long frames appear**: the OS also offloads the segmentation of TCP data to the NIC (transmit segmentation offload, TSO, GSO). The packet socket sees the frame before segmentation, longer than the maximum length.

The checksum warning has 2 texts. The first is for frames that the OS sent from the bridge's own interface; the second is for frames that the interface received (from the other end of a veth pair).

```
interface 'enp3s0': a frame this computer's OS sent on it has no TCP/UDP checksum yet: the OS leaves the checksum to the network adapter (checksum offload), and the bridge sees the frame before that. The bridge passes such frames on as they are, and a receiver in SIL Kit that checks checksums drops them. Turn the offload off with 'sudo ethtool -K enp3s0 tx off'; or, if the peer is this computer's OS, use --backend tap instead. This warning is given once.
interface 'va': a frame received on it has no TCP/UDP checksum: this computer's OS sent it through a virtual interface (the other end of a veth pair) and left the checksum to a network adapter that is not there (checksum offload). The bridge passes such frames on as they are, and a receiver in SIL Kit that checks checksums drops them. Turn the offload off on the interface the frames are sent from (for a veth pair the other end, not 'va'): 'sudo ethtool -K <that interface> tx off'; or, if the peer is this computer's OS, use --backend tap instead. This warning is given once.
```

Neither the checksum problem nor the length problem occurs with `tap`, because frames read from a TAP device carry their checksums and have the length after segmentation.

### `npcap` and `pcap`

The same code loads Npcap's `wpcap.dll` on Windows and libpcap on Linux at run time. Neither is included in vehicle-sil; install them yourself.

| OS | `--backend` | What to install |
|---|---|---|
| Windows | `npcap` | [Npcap](https://npcap.com) |
| Linux | `pcap` | Your distribution's libpcap (`libpcap0.8` on Debian and Ubuntu, `libpcap` on Fedora) |

- The free edition of Npcap has limits on where you can use it. Read the Npcap license before you install it (vehicle-sil does not interpret it).
- On Linux, `pcap` needs `CAP_NET_RAW`, the same as `packet`.

#### Where the library is loaded from

The location of the library is fixed. No argument, environment variable or file changes it.

| OS | What is loaded |
|---|---|
| Windows | `<system directory>\Npcap\wpcap.dll` (usually `C:\Windows\System32\Npcap\wpcap.dll`), by absolute path. |
| Linux | `libpcap.so.1`, or `libpcap.so.0.8` if that is missing, as the system loader finds it (the same as any other shared library). |

How DLLs are found on Windows:

- The DLL that `wpcap.dll` uses (`Packet.dll`) is searched for only in the directory of `wpcap.dll`, then in the system directory.
- Neither `wpcap.dll` nor the DLLs it uses are loaded from the current directory, `PATH` or the directory of the executable. A `wpcap.dll` or `Packet.dll` in a folder you received from someone is therefore never loaded (only administrators can write to the system directory).
- If a DLL is not found in these 2 places, the bridge refuses with the same message as when Npcap is not installed.
- A `wpcap.dll` directly in `System32` (an installation compatible with WinPcap) is not used.

The tests load a fake library, but the means to do so exists only in a separate executable built for the tests (`tests/ethernet_bridge/fake_pcap_library.cpp`). The installed executable links `bridges/ethernet/app/pcap_library.cpp`, which leaves the location empty, meaning the fixed location above. The library location (`eth_link::PcapLinkOptions::library`) is an argument that the calling program decides.

#### Names accepted by `--interface`

| Name | Example | OS |
|---|---|---|
| pcap name | On Linux, the interface name itself; on Windows, `\Device\NPF_{...}` | Linux, Windows |
| Network connection name (the start of a `--list-interfaces` line) | `Ethernet 2` | Windows |
| Adapter identifier | `{4D36E972-...}` | Windows |

- The adapter identifier can be given with or without braces, in upper or lower case.
- If a connection name matches 2 adapters, the bridge reports an error and shows their pcap names.
- Connection names are compared as UTF-8 strings (the arguments are read as UTF-8). As in Windows, the case of ASCII letters does not matter (`ethernet 2` chooses `Ethernet 2`); other letters must be the same. If 2 connection names differ only in case, a name in a third case gives an error with both names, and each of them written exactly chooses its own adapter.
- pcap names are compared exactly, including case.
- Both the list and the error messages are written in a form that is readable in a console whatever the code page, and as UTF-8 to files and pipes (see "Output encoding" in [bridge-contract.md](bridge-contract.md#output-encoding)). You do not need `chcp`.
- Text that Npcap returns (device descriptions, error texts) is read in the Windows ANSI code page and converted to UTF-8.
- A connection name in Japanese is listed and matched (checked on English Windows). Files and pipes receive it as correct UTF-8, and a console receives the correct characters; the console font of English Windows draws them as boxes. How a console on Japanese Windows shows them is not verified.

#### Behavior common to `npcap` and `pcap`

- When opening, the bridge always uses immediate delivery of received frames (immediate mode) and non-blocking reads. If the library is so old that it lacks the functions for these settings, the bridge refuses to open it.
- To receive, the bridge waits for the library's notification that frames are there (an event on Windows, a descriptor on Linux), then reads. It also reads at least once every 100 ms without a notification. With a library that gives no notifications, frames arrive late, but they do arrive.
- If, 3 times, frames are found only by the read every 100 ms and not through a notification, the bridge warns once (see the warning texts below).
- Frames can reach the physical network, so you must say what may pass (one of `--ethertype`, `--src-mac` or `--all-frames`).
- The bridge does not open interfaces that are not Ethernet.
- Frames that the library drops (because they were not read in time) are counted in `ext_overruns`.
- For a wireless adapter (one that the library reports as `wireless`), the bridge warns once at startup.

The bridge asks the library for promiscuous mode as "[Promiscuous mode](#promiscuous-mode)" says. Whether `npcap` fails to open an adapter that does not accept promiscuous mode, or opens it without the mode and says nothing, is **not verified** with the real Npcap.

**Frames with incomplete checksums cannot be told apart.** The pcap library does not pass that flag on. Frames sent by the OS of the same computer (see "[Talking to the network stack of the same computer](#talking-to-the-network-stack-of-the-same-computer)") reach SIL Kit with an incorrect checksum field, `ext_checksum_incomplete` stays 0, and no warning appears. On Linux, use `packet` if you need to know.

#### Behavior of `pcap` on Linux only

- libpcap puts the VLAN tags of received frames back in their original position.
- Frames that the bridge sends do not come back to the bridge.
- The link going up and down appears in the health line, and if the interface disappears, the bridge ends with `FAIL` (the same as `packet`).
- An interface that is down cannot be opened (`packet` opens it with only a warning).

#### Behavior of `npcap` on Windows only

| Item | Behavior |
|---|---|
| Link state | The bridge does not watch the link going up and down (the health line shows no link state). |
| When the adapter disappears | Detected only when a read from the library fails. |
| Frames the bridge sends | Do not come back to the bridge (`ExternalLink::echoes_own_transmissions()` is false). |
| Disabled adapters | Not in the Npcap list, so giving one to `--interface` gives the "no interface" error (`no interface ...`). |
| Npcap installed for administrators only, used by a user who is not an administrator | Npcap shows an elevation prompt, and the bridge waits until the prompt is closed (`--list-interfaces` too). |
| Frames with VLAN tags | Whether they pass depends on the adapter driver and its settings (neither the bridge nor Npcap changes tags). |

Frames the bridge sends: Npcap passes a program that has an adapter open the frames that the program itself sent. The bridge opens the adapter with the option that stops this (`PCAP_OPENFLAG_NOCAPTURE_LOCAL` of `pcap_open()`). It still receives frames sent by other programs that have the same adapter open. A `wpcap.dll` without `pcap_open` is refused when it is loaded (see the messages below).

Disabled adapters: the error is "no interface", not "down". The error message says that disabled adapters are not in the list.

Npcap installed for administrators only: if you close the elevation prompt, the list contains only `\Device\NPF_Loopback`, and the adapter gives the "no interface" error. Npcap does not report missing privileges, so when the list contains no network adapter, the bridge names this case in the last line of the `--list-interfaces` output and in the "no interface" error message (see the messages below). Run the bridge from a console started as administrator, or install Npcap again without that limit.

VLAN tags: with its default settings, a driver may not pass frames with VLAN tags. Disabling the driver's priority and VLAN handling makes them arrive, but then the outer tag may be stripped from frames that the bridge sends. Transmitting still reports success, and neither the statistics nor a warning shows it. Before you use VLAN tags, verify both directions on that adapter.

#### Warning and error messages

The warning texts:

```
interface 'Wi-Fi' is a wireless adapter: wireless adapters and access points often pass only frames from and to the adapter's own MAC address, and the devices of the SIL Kit side have other addresses. If frames do not pass, use a wired adapter.
interface 'va' does not accept promiscuous mode (result 2, not supported), so the bridge runs without it there: only frames to the interface's own address and to group addresses arrive, and frames to the devices of the SIL Kit side (they have addresses of their own) do not. If devices outside must reach them, use an adapter that accepts promiscuous mode (a wired one usually does).
interface 'va' was opened with a warning (result 1).
Npcap does not tell when frames arrive on interface 'Ethernet 2': the bridge finds them by looking every 100 ms, so frames from this interface reach SIL Kit up to 100 ms late. None is lost by this. If the delay matters, install the current Npcap, or try another adapter.
libpcap does not tell when frames arrive on interface 'va': the bridge finds them by looking every 100 ms, so frames from this interface reach SIL Kit up to 100 ms late. None is lost by this. If the delay matters, update libpcap, or use --backend packet.
```

The messages when the library cannot be used (exit code 3; printed before the bridge connects to SIL Kit):

```
Npcap is not installed: cannot load C:\Windows\System32\Npcap\wpcap.dll (Windows error 126). Npcap is not part of vehicle-sil: install it yourself from https://npcap.com (see its licence for the limits of the free edition), then run --list-interfaces.
libpcap is not installed: cannot load libpcap.so.1 or libpcap.so.0.8 (libpcap.so.0.8: cannot open shared object file: No such file or directory). libpcap is not part of vehicle-sil: install your distribution's package (Debian and Ubuntu: 'sudo apt install libpcap0.8'; Fedora: 'sudo dnf install libpcap'), or use --backend packet, which needs no library.
C:\Windows\System32\Npcap\wpcap.dll (WinPcap version 4.1.3) lacks the function pcap_set_immediate_mode, which the bridge needs: it is too old, or it is not Npcap. Install a current one. Npcap is not part of vehicle-sil: install it yourself from https://npcap.com (see its licence for the limits of the free edition), then run --list-interfaces.
```

The third shows the form for one missing function. If `pcap_open` is missing, the text is the same with only the function name changed.

The messages when the interface cannot be opened (exit code 3):

```
no interface 'Ethernet 9' for --backend npcap. Interfaces Npcap offers here: "Ethernet 2", Ethernet (run --list-interfaces --backend npcap). Npcap does not offer an adapter that is disabled: enable it in Windows. It does not offer one it is not bound to either, such as the physical adapter of a Hyper-V external switch: give the switch's adapter, "vEthernet (<switch>)", as --interface instead.
no interface 'Ethernet 2' for --backend npcap. Interfaces Npcap offers here: \Device\NPF_Loopback (run --list-interfaces --backend npcap). Npcap does not offer an adapter that is disabled: enable it in Windows. It does not offer one it is not bound to either, such as the physical adapter of a Hyper-V external switch: give the switch's adapter, "vEthernet (<switch>)", as --interface instead. It offers no network adapter here: if Npcap was installed for administrators only, run the bridge from a console started as administrator, or install Npcap again without that limit.
'Ethernet' is the name of 2 interfaces for --backend npcap: \Device\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}, \Device\NPF_{0A1B2C3D-0000-1111-2222-333344445555}. Give one of these names as --interface instead.
interface 'any' is not Ethernet (pcap link type 113): --backend pcap carries Ethernet frames only. Choose an Ethernet interface (run --list-interfaces --backend pcap).
cannot open 'enp3s0' with libpcap (pcap_activate: result -8, socket: Operation not permitted). A network interface needs CAP_NET_RAW: run 'sudo setcap cap_net_raw+ep /opt/vehicle-sil/bin/vehicle_sil_ethernet_bridge' once, or run as root.
cannot open 'va' with libpcap (pcap_activate: result -9, va: That device is not up). The interface is down: bring it up with 'sudo ip link set va up'.
cannot open 'Ethernet 2' with Npcap (pcap_open: the reason the library gives). Run --list-interfaces --backend npcap to see whether Npcap still offers the adapter.
cannot open 'Ethernet 2' with Npcap (pcap_open: the reason the library gives). Run --list-interfaces --backend npcap to see whether Npcap still offers the adapter. The adapter may not accept promiscuous mode: try without --promiscuous.
interface 'va' does not accept promiscuous mode (result 2, not supported), and --promiscuous asks for it: leave out --promiscuous to let the bridge run without it (it then says so), or use an adapter that accepts promiscuous mode (a wired one usually does).
this user may not open 'va' in promiscuous mode with libpcap (result -11, the reason the library gives), and --promiscuous asks for it: run 'sudo setcap cap_net_raw+ep /opt/vehicle-sil/bin/vehicle_sil_ethernet_bridge' once or run as root, or leave out --promiscuous to let the bridge run without it (it then says so).
```

- In parentheses, `result` is the result of the library function, and the text after it comes from the library.
- The Npcap way of opening (`pcap_open()`) returns no result number, so an `npcap` message contains only the library's text. In the example above, the text after `pcap_open:` is a placeholder for the library's text.
- The `npcap` "no interface" error always adds that disabled adapters are not in the list.
- If the list contains no network adapter (only `\Device\NPF_Loopback`, or nothing), the error also mentions an installation for administrators only (the second example).
- The `npcap` "cannot open" error adds the advice to try without `--promiscuous` only when the adapter was opened with `--promiscuous`. The last 2 examples are the `pcap` errors with `--promiscuous` when the interface refuses promiscuous mode.

#### Connecting within one computer on Windows

Windows has no backend that corresponds to a TAP device. To connect programs within one computer, install the virtual adapter that comes with Windows, "Microsoft KM-TEST Loopback Adapter", and have both the bridge and the other program open that adapter with Npcap. If you open the same adapter in Wireshark, you can see the frames that come out of SIL Kit. Between programs that open the same adapter with Npcap, frames pass in both directions and VLAN tags are kept.

Programs that use ordinary OS sockets (UDP, TCP) on the same computer: frames that the OS sends on the adapter reach the bridge, but **frames that the bridge sends do not reach the OS**. The bridge opens the adapter with the option not to receive its own transmissions (`PCAP_OPENFLAG_NOCAPTURE_LOCAL`), and on this adapter a frame sent through a capture opened with that option did not reach the OS, while the same frame sent through a capture opened without it did.

### When the interface state changes while running

| What happened | Backend | What the bridge does |
|---|---|---|
| The link went down or up | `tap`, `packet`, `pcap` | Shows `link down` or `link up` in the health line and keeps running. |
| The interface disappeared (deleted or renamed), or reading failed | All | Prints a `FAIL:` line and the statistics, and exits with code 1. |

- Frames sent while the link is down and rejected by the OS are counted in `ext_tx_failures`.
- `FAIL:` lines look like `FAIL: interface 'vsil0' is gone (removed or renamed while the bridge ran)` and `FAIL: reading from 'Ethernet 2' failed: <library text>`.

### Backends not available on this OS

```
--backend npcap is for Windows. On Linux use --backend packet or --backend pcap (a network interface), or --backend tap (a TAP device).
--backend tap is for Linux. On Windows use --backend npcap (a network adapter through Npcap).
--backend tap cannot be used here: vehicle_sil_ethernet_bridge has no backend for this operating system (tap, packet and pcap are for Linux; npcap is for Windows). Run the bridge on a Linux or Windows computer that reaches the same SIL Kit registry.
```

The third is the message on an OS that is neither Linux nor Windows. There, `--list-interfaces` also exits with code 3 for the same reason (`there is no interface to list here: ...`).

## Statistics

The form of the line, the keys and their order are in "[Statistics line](bridge-contract.md#statistics-line)" in bridge-contract.md. What the keys count in this bridge, where it is more than the table says there:

- When a key that raises a health line increases, a health line appears (`problems_grew()`). The forwarded counts, the filter counts and `own_tx_dropped` are not problems.
- `ext_to_silkit` counts where `SendFrame()` returned. Frames in `silkit_tx_failures` that SIL Kit confirmed it could not send (`ControllerInactive`, `LinkDown`, `Dropped`, `InvalidFrameFormat`) are also counted in `ext_to_silkit`, at the time they were passed on.
- `ext_checksum_incomplete` counts frames sent by the OS of the same computer, with `packet` (see "[Talking to the network stack of the same computer](#talking-to-the-network-stack-of-the-same-computer)"). They are also counted in `ext_to_silkit`.
- `ext_from_devices` tells whether anything outside answers. It counts frames from the outside that this computer did not send, and whose source is not an address of the SIL Kit side, before the filter. Not counted: frames this computer sent (from the MAC address of one of its adapters, and with `packet` every frame the kernel marks as sent here, as "[Hint when nothing comes from a device](#hint-when-nothing-comes-from-a-device)" says; where the bridge does not know the addresses, they count), frames from an address of the SIL Kit side (the `--src-mac` addresses, and with the MAC rules the sources of the frames the bridge sent out: the frames that `own_tx_dropped` and `loop_suspected` count), frames shorter than a header or longer than the maximum, and frames before the SIL Kit controller is activated. A frame from a device counts even where the filter keeps it out of SIL Kit (it is also in `filtered_from_ext`).
- An answer from a program on this computer that uses the same adapter does not count: with `packet` whatever its source address, with `npcap` and `pcap` when it sends from the address of one of this computer's adapters (as such programs usually do).
- With `tap`, the other end is this computer's network stack, so nothing that comes in is taken for this computer's own: `ext_from_devices` counts every frame that comes in and is not from the SIL Kit side, as `ext_to_silkit` and `filtered_from_ext` together do.
- `own_tx_dropped` counts only with an external link that echoes its own transmissions; none of the external links of vehicle-sil echo. With `--no-mac-guard`, only the `--src-mac` addresses tell the SIL Kit side, so a frame that the bridge sent out and that comes back from such a link counts in `ext_from_devices`.

## Health lines

`format_health_change(before, now, interface, problems_before)` turns the change between 2 snapshots (`EthernetBridge::health()`) that the caller took into one line. If nothing changed, it makes no line.

| Case | Line |
|---|---|
| A problem count increased or the link state changed | `interface enp3s0: link down; since the last report: ext_tx_failures +12` |
| Nothing increased, but the previous snapshot had increases (`problems_before`) | `interface enp3s0: link up; no new problems since the last report` |

- A change from an unknown state to `link up` is the first report of a healthy link, so it makes no line.
- The link state is one of `link up`, `link down` and `link state unknown`.

## Link line

The form of the line is in "[Link line](bridge-contract.md#link-line)" in bridge-contract.md. This bridge prints `link link=<value>`:

```
[eth-bridge] link link=up device=af3d43e7e4b8883a marked=1 shared=0
```

| Value | Meaning |
|---|---|
| `up` | The link is up (`tap`, `packet`, `pcap`: the interface is up and has a carrier) |
| `down` | The link is down |
| `unknown` | The link has not reported yet, or cannot tell. `npcap` does not watch the link, so its value is always `unknown` |

- The bridge places its mark once the interface is open (only then does Npcap's link know the pcap name of a connection name). `packet` and `pcap` on the same interface are the same device.
- `link_line_value()` gives the value.

## Filter (`FrameFilter`)

The same rules apply in both directions (`vehicle_sil/eth_link/frame_filter.hpp`).

| Condition | Setting | Frames that pass |
|---|---|---|
| Length | Maximum length (default 1522 bytes, without FCS) | Frames of at least the Ethernet header (14 bytes) and at most the maximum length |
| EtherType | `FrameFilterOptions::ether_types` | Only frames with a listed EtherType |
| SIL Kit side MAC | `FrameFilterOptions::silkit_macs` | Only frames whose source (from SIL Kit to the outside) or destination (from the outside to SIL Kit) is one of them |

- Length: frames shorter than the header are dropped as `malformed`, frames longer than the maximum length as `oversize`.
- EtherType: if the 2 bytes after the source MAC are a value that marks a VLAN tag (TPID; by default `0x8100` and `0x88A8`), the filter looks behind that 4-byte tag. It looks behind at most 2 tags. A frame that ends inside a tag does not pass.
- SIL Kit side MAC: from the outside to SIL Kit, frames whose destination is a group address (broadcast, multicast) pass too. Group addresses cannot be listed in `silkit_macs`.
- If you list both EtherTypes and MAC addresses, only frames that meet both pass. If you list neither, there is no filtering except by length.

> [!NOTE]
> The default VLAN TPIDs (`0x8100`, `0x88A8`), the default maximum length of 1522 bytes and the position of the EtherType are values from general sources and are not verified against the standards. All of them can be changed with `FrameFilterOptions` (`--vlan-tpid`, `--max-frame-bytes` on the command line). The EtherType `0x88B5` used by the tests and examples is a value known for local experiments, also not verified against the standards. The library contains no protocol EtherTypes; you list them yourself.

SIL Kit pads frames shorter than 60 bytes with zeros on the receiving side (see "Upstream behavior the implementation relies on" in [dependencies.md](dependencies.md#upstream-behavior-the-implementation-relies-on)). Frames from SIL Kit are therefore at least 60 bytes, and short frames from the outside reach their receiver in SIL Kit padded.

## Loop prevention (`MacSideTable`)

Put only one bridge between the same 2 networks (the SIL Kit Ethernet network and the external wire). A second bridge makes a wrong setup in which frames can keep circling through both bridges. When it can, the bridge detects this setup from the source MAC addresses of frames, stops the frames from that address and warns. It does not guarantee to stop every case (see "[When a loop is not stopped](#when-a-loop-is-not-stopped)"). It does not rely on OS settings (`vehicle_sil/eth_link/mac_side_table.hpp`).

### MAC rules

| Received frame | What the bridge does |
|---|---|
| From SIL Kit, passed by the filter | Remembers the source as "SIL Kit side". |
| From the outside, passed by the filter | Remembers the source as "outside". |
| From SIL Kit, with a source on the outside | Marks the address as "on both sides". |
| From the outside, with a source on the SIL Kit side (the external link does not echo its own transmissions) | Marks the address as "on both sides". |
| From the outside, with a source on the SIL Kit side (the external link echoes its own transmissions) | Drops it as an echo of a frame the bridge sent, and counts it in `own_tx_dropped`. |
| With a source address that is on both sides | From then on, drops it in both directions and counts it in `loop_suspected`. |
| With a group source address (broadcast, multicast) | Passes it without remembering or checking it. |

- The addresses given with `--src-mac` (`FrameFilterOptions::silkit_macs`) are registered as SIL Kit side when the bridge is created.
- The source of a frame that the filter did not pass is not remembered.
- "On both sides" means that there are 2 bridges between the same networks, or that the same MAC address is on both sides.
- `echoes_own_transmissions()` tells whether the external link echoes its own transmissions. If it does not (`false`), a frame from the outside cannot be one the bridge sent. If it does (`true`), dropping such frames gives no warning.
- The warning for an address on both sides is printed once per address for the first 8 addresses; beyond that, one warning covers all the rest.
- The record that an address is on both sides lasts until the bridge restarts. Once you fix the setup (down to one bridge), restart the remaining bridge too.

### When a loop is stopped

The tests verify these 3 cases. They use 2 bridges with fake external links and fake controllers. The `npcap` way of opening is verified with 2 bridges connected to the same device of the fake pcap library.

- With an external link that does not echo its own transmissions, a loop is stopped whether a SIL Kit device or an outside device started it, whatever order the copies arrive in. The loop lasts only until the later bridge receives the original frame on its own side. At that point it learns that the address is on both sides.
- With an external link that does not echo its own transmissions, if only one bridge has `--no-mac-guard`, the bridge that uses the rules detects the loop, stops it and warns. The bridge without the rules may also warn once that a frame it had just sent came from the outside (see "[With `--no-mac-guard`](#with---no-mac-guard)").
- With an external link that echoes its own transmissions, a loop started by a SIL Kit device is still stopped: the bridge that saw the address outside first learns from the frame from SIL Kit that the address is on both sides.

### When a loop is not stopped

"An external link that echoes its own transmissions" means an `ExternalLink` that you implement yourself and that declares it echoes them. None of the external links of vehicle-sil echo.

- A loop started by an outside device, with an external link that echoes its own transmissions.
- With an external link that echoes its own transmissions, when one bridge has `--no-mac-guard`. The bridge that uses the rules drops the other bridge's copies as its own echoes.
- When both bridges have `--no-mac-guard`.
- Frames from addresses that did not fit in a full MAC table (counted in `mac_unguarded`) and frames with a group source address.

What happens in the first case: bridge 1 passes an outside frame into SIL Kit. If bridge 2 receives it from SIL Kit before the copy on its own outside side, bridge 2 remembers the address as SIL Kit side and drops the copy that comes later from the outside as its own echo. Neither bridge sees the address on both sides, so the loop goes on with no warning (the tests verify this too). With this kind of external link, the source address alone cannot tell an echo of the bridge's own frame from a frame of another bridge or of another device with the same address.

### Whether an external link echoes its own transmissions

The implementation of an external link declares whether it echoes its own transmissions. Every external link of vehicle-sil declares that it does not. For `tap`, `packet` and `pcap`, this is the behavior verified with the Linux kernel and libpcap (tests `ethernet_bridge.netns.*`).

| Backend | Behavior |
|---|---|
| `tap` | A frame that the bridge writes to the TAP device goes to the OS and does not come back to the bridge's reads. |
| `packet` | A frame that the bridge's socket sends does not come back to that socket. |
| `pcap` | The libpcap socket behaves the same as `packet`. |
| `npcap` | Npcap echoes a program's own transmissions, but the bridge opens the adapter with the option that stops this (see "[`npcap` and `pcap`](#npcap-and-pcap)"). |

- With `packet`, the bridge does receive frames that other sockets on the same interface send.
- With `npcap`, the bridge does receive frames that other programs with the same adapter open send, so it can also detect a second bridge on the same adapter.

So with these external links, all of "When a loop is stopped" applies: if a frame from the outside has a SIL Kit side address as its source, the bridge treats that address as on both sides, stops it and warns. A program on the same computer that sends on the same interface with the MAC address of a SIL Kit device as source causes this warning.

**If an external link declares that it does not echo, but does** (for example, an Npcap or adapter on which the option against echoes does not work; whether one exists is not verified), the bridge sees each echo as a frame from the outside with a SIL Kit side address as its source, the case above.

- With the MAC rules on, as soon as a SIL Kit device sends its first frame, the bridge receives the echo from the outside, and the address of that device is stopped as on both sides (it errs on the side of stopping). The warning for a SIL Kit side address seen outside therefore ends by saying that, if there is no second bridge and no device with the same address, the external link echoes its own transmissions, and what to do next (the second text in "[Warning messages](#warning-messages)"). What to do next, for `npcap`: install a current Npcap, try another adapter, and if that does not help, report it to vehicle-sil.
- With `--no-mac-guard`, the bridge warns once, as described in "[With `--no-mac-guard`](#with---no-mac-guard)".

### When the same MAC address is on both sides

Frames from an address on both sides are dropped in both directions, so if devices with the same MAC address really are on both sides, frames from that address no longer pass. The warning names the address, so you can find the wrong setup. However, with an external link that echoes its own transmissions, an address first remembered on the SIL Kit side and then seen outside cannot be told from an echo, so its frames are dropped silently.

### MAC table size

The MAC table holds 1024 addresses by default (`--mac-table-size`, 16–65536). Memory is allocated only when the bridge is created. When the table is full, a SIL Kit side address (and a record of an address on both sides) evicts the oldest outside address. New outside addresses are not remembered. Frames from addresses that are not remembered are passed without the check and counted in `mac_unguarded`, with a warning the first time only.

### With `--no-mac-guard`

Setting `EthernetBridgeOptions::mac_guard` to `false` (`--no-mac-guard` on the command line) turns off the MAC rules. Preventing loops is then up to you.

**With `--no-mac-guard` and an external link that echoes its own transmissions**, frames from SIL Kit come back into SIL Kit, because only the MAC rules drop the echoes of the bridge's own frames.

- A frame that the bridge sends out comes back from the external link, passes the filter as a frame from the outside and enters SIL Kit (counted in `ext_to_silkit`). The sending device receives its own frame, and every other device receives the frame twice. The bridge sends out only frames it received from SIL Kit, so the frame does not keep circling.
- A network stack that checks whether its own address is used by others may see its own frame and conclude that the address is in use.
- If the external link declares that it echoes and the rules are off, the bridge warns once at every startup (the second-to-last text in "[Warning messages](#warning-messages)").
- Even with the rules on, the same happens to some frames: frames from SIL Kit side addresses that did not fit in a full MAC table (counted in `mac_unguarded`, with the table-full warning) and frames with a group source address. Both are passed without the check when they come back.
- None of the external links of vehicle-sil declare that they echo. This is the behavior when an `ExternalLink` that you implement yourself declares it.

**With `--no-mac-guard` and an external link that declares it does not echo**, the bridge compares each frame from the outside that it passes to SIL Kit with the last frame it sent out (the length and up to the first 64 bytes). If they are the same, it warns once (the last text in "[Warning messages](#warning-messages)"). It does not drop the frame but passes it to SIL Kit, because without the rules nothing is dropped based on where it came from. The statistics do not change (the frame is only counted in `ext_to_silkit`).

- This warning appears when the external link echoes its own transmissions against its declaration, when there is a second bridge between the same networks, or when an outside device sends received frames straight back. The bridge cannot tell these apart, so the text lists them all.
- Only the last frame sent is compared. If the bridge sends the next frame before the echo arrives, the echo is not detected. It is not detected either if the external link changes the length when echoing (padding a short frame to the minimum length). The absence of the warning does not prove that nothing is echoed.
- The warning appears at most once per startup.

### Warning messages

The warning texts follow (the tests check them word for word). They name the side that saw the address first and the side that sees it now (`SIL Kit`, `the external link`). Only when a SIL Kit side address is seen outside (the second text) is a sentence about an external link that echoes its own transmissions added at the end.

```
MAC 02:00:00:00:00:99 is on both sides: frames from it came from the external link and now come from SIL Kit, so frames from it are dropped both ways (loop_suspected). Run one bridge between these networks and give each device its own MAC address, then restart the bridge: it keeps dropping them until it restarts.
MAC 02:00:00:00:00:01 is on both sides: frames from it came from SIL Kit and now come from the external link, so frames from it are dropped both ways (loop_suspected). Run one bridge between these networks and give each device its own MAC address, then restart the bridge: it keeps dropping them until it restarts. If there is no second bridge and no other device with this address, the external link gives the bridge's own transmissions back although it says it does not: with --backend npcap, install the current Npcap or try another adapter, and report it to vehicle-sil if it stays.
more MAC addresses are on both sides than the first 8 named: frames from them are dropped both ways too (loop_suspected).
the MAC table is full (1024 addresses): frames from addresses that do not fit are forwarded without the check against loops (mac_unguarded); addresses of the SIL Kit side take the places of the oldest addresses outside. Raise --mac-table-size if this many devices are expected.
the MAC guard is off (--no-mac-guard) and this external link gives the bridge's own transmissions back to it: every frame the bridge sends out comes back and is passed into SIL Kit again (counted in ext_to_silkit), so its sender receives its own frame and every other participant receives it twice. A network stack that looks for its own address in use by others then finds it. Run without --no-mac-guard unless this is what you want.
a frame like the one the bridge had just sent out (the same length and first bytes) came from the external link, and with the MAC guard off (--no-mac-guard) it was passed into SIL Kit again (counted in ext_to_silkit). A second bridge or a device outside sends the frames back, or the external link gives the bridge's own transmissions back although it says it does not: with --backend npcap, install the current Npcap or try another adapter, and report it to vehicle-sil if it stays. Run without --no-mac-guard to have such frames dropped and their addresses named. This is said once.
```

## Library structure

A frame passes in both directions through SIL Kit's `IEthernetController`, `EthernetBridge` (which uses `FrameFilter` and `MacSideTable`), `ExternalLink` and the external interface, in that order.

| Target | Contents | Dependencies |
|---|---|---|
| `vehicle_sil_ethernet_bridge` (executable) | The command line of the bridge. | `ethernet_bridge_core`, `bridge_runtime` and `core`. Built on every platform, with the external links of that OS; the pcap library is loaded at run time. |
| `vehicle_sil::eth_link` | `ExternalLink`, which represents an external interface, its implementations, the filter and the MAC table. | Does not use SIL Kit. |
| `vehicle_sil::ethernet_bridge_core` | `EthernetBridge`, which forwards frames, plus the output and argument parsing of the executable. | SIL Kit and `eth_link`. Platform-independent. |

`vehicle_sil::eth_link` contains:

- `ExternalLink`, which represents an external interface
- MAC addresses
- The filter `FrameFilter`
- The loop prevention table `MacSideTable`
- External links that use APIs the OS offers without a library (`os_links.hpp`: Linux TAP devices and packet sockets)
- External links that load the pcap library installed on the OS at run time (`pcap_link.hpp`: Npcap on Windows, libpcap on Linux)

`vehicle_sil::ethernet_bridge_core` contains:

- `EthernetBridge`, which forwards frames between SIL Kit's `IEthernetController` and an `ExternalLink`
- Statistics and health lines
- Argument parsing (`parse_bridge_options()`)
- What the executable prints (`bridge_app.hpp`: the startup line, the backends available on each OS, the promiscuous mode hint)

For the source locations, see "Ethernet Bridge" in [code-map-bridges.md](code-map-bridges.md#ethernet-bridge).

## External link (`ExternalLink`)

`vehicle_sil::eth_link::ExternalLink` (`vehicle_sil/eth_link/external_link.hpp`) represents one external Ethernet interface. This page calls its implementations "external links". A frame is a byte sequence from the destination MAC address to the end of the payload, without FCS.

| Function | Contract |
|---|---|
| `start(handler)` | Opens the interface and starts passing `LinkEvent`s to `handler` from the receive thread. Throws `LinkError` if the interface cannot be opened |
| `stop()` | Stops passing events. Before it returns, it may report, as an `Overrun` from the calling thread, the number of frames the OS dropped since the last report. Once it returns, the handler is not running. Can be called even if the link was not started |
| `transmit(frame)` | Sends one frame. The result is `Ok`, `QueueFull` (not sent because the transmit queue was full) or `Failed` (another error; the reason is in `last_error()`). Reads the frame only during the call. Does not block |
| `echoes_own_transmissions()` | Whether frames sent with `transmit()` come back to the handler as received frames. Does not change during the lifetime of the external link. The bridge reads it in `start()` and adapts its loop prevention (see "[Loop prevention](#loop-prevention-macsidetable)") |
| `describe()` | The string for the startup line: kind, name, physical or virtual (for example `tap 'vsil0' (TAP device in this computer)`, `packet 'enp3s0' (network interface)`) |
| `this_computer_addresses()` | The MAC addresses this computer sends from on the external link's side: those of all of its Ethernet adapters, found once by `start()`. Empty: nothing that comes in is this computer's own (`tap`). `nullopt`: the external link cannot tell (the default). The bridge reads it in `start()`: frames from these addresses do not count in `ext_from_devices`. An external link that wraps another passes the wrapped one's answer on |

### Threads and locks

- The handler is called from only one thread at a time (the receive thread or the thread that called `stop()`).
- `transmit()` can be called from any thread, also concurrently with the handler.
- `transmit()` does not block, does not throw and does not wait for the receive thread.
- An external link must not hold a lock that `transmit()` takes while it calls the handler.
- No receive time is passed, because the OS clock is not the simulation clock.

The reason for the lock constraint: the bridge calls `transmit()` in a receive thread that SIL Kit calls while holding the lock of the frame handler. `SendFrame()`, which the bridge calls inside the external link's handler, takes the same lock (see "Upstream behavior the implementation relies on" in [dependencies.md](dependencies.md#upstream-behavior-the-implementation-relies-on)). If the external link held its lock during the handler, the 2 threads would wait for each other and hang.

If `transmit()` throws an exception against its contract, the bridge catches it, counts it in `ext_tx_failures` and continues.

### `LinkEvent` and `LinkError`

| `LinkEvent` kind | Meaning |
|---|---|
| `Frame` | A received frame (`frame`; valid only during the handler call) |
| `Overrun` | The number of frames the OS dropped before they were read (`count`; the increase since the last report) |
| `LinkState` | The link went up or down (`state`: `Up`, `Down`, `Unknown`) |
| `Fault` | The external link stopped working (the interface disappeared, or reads keep failing). The reason is in `detail` |
| `Warning` | Something to tell the user that is not reason enough to stop (`detail`) |

- If `sent_here` of a `Frame` is true, this computer sent the frame on the interface (only `packet` tells it). The bridge does not count it in `ext_from_devices`.
- If `checksum_incomplete` of a `Frame` is true, the frame has an incomplete UDP or TCP checksum (sent by the OS of the same computer, which left the calculation to the NIC). The external link passes it on as it is and emits a `Warning` once, before the first such frame. The bridge passes it on and counts it in `ext_checksum_incomplete`.
- After a `Fault`, no more frames are passed and `transmit()` fails.

The kinds of `LinkError` (`kind()`) are `NoSuchInterface`, `PermissionDenied`, `LibraryMissing`, `NotEthernet` and `Other`. `what()` says what happened and how to fix it.

### Functions that create external links

| External link | Header | Function | OS |
|---|---|---|---|
| TAP device | `vehicle_sil/eth_link/os_links.hpp` | `make_tap_link()` | Linux |
| packet socket | `vehicle_sil/eth_link/os_links.hpp` | `make_packet_link()` | Linux |
| pcap library | `vehicle_sil/eth_link/pcap_link.hpp` | `make_pcap_link()` | Windows, Linux |

The packet socket and the pcap library take `promiscuous` (`eth_link::Promiscuous`: `Off`, `On`, `Auto`) in their options. `Off` is the default of the library, so that a program that set nothing keeps its behavior; the executable gives `Auto` unless `--promiscuous` or `--no-promiscuous` says otherwise. `ExternalLink::promiscuous()` tells the mode in effect once the link has started (`nullopt` for a link without the mode, such as a TAP device), and `Auto` reports a refusal as one `Warning` event (`promiscuous_refused_warning()`).

The external links of `os_links.hpp`:

- On an OS other than Linux, `has_os_links()` is `false` and the create functions throw `LinkError`.
- `list_interfaces()` returns the interfaces of this computer.
- In both external links, the receive thread looks at the interface every 250 ms and reports the link going up and down, the interface disappearing and the number of frames the OS dropped.
- When the fix is an argument, the `LinkError` text names it with the argument name of `vehicle_sil_ethernet_bridge`.

The external link of `pcap_link.hpp`:

- Loads the library when it is created, and throws `LinkError` (`LibraryMissing`) if the library is missing. `start()` opens the interface.
- `pcap_interface_list()` returns the text of `--list-interfaces`.
- If `PcapLinkOptions::library` is empty, the library installed on the computer is loaded; otherwise the given file is. The option exists so that tests can pass a fake library; the executable passes an empty value.

The fake external link for tests is `tests/eth_link/fake_link` (`FakeLink`, and `FakeSegment`, which connects fake external links to each other). It has a receive thread and simulates transmit congestion, lost frames, `Fault`, the link going up and down, and an interface that echoes its own transmissions.

## Forwarding (`EthernetBridge`)

`vehicle_sil::ethernet_bridge::EthernetBridge` (`vehicle_sil/ethernet_bridge/ethernet_bridge.hpp`) forwards frames unchanged in both directions.

| Function | Behavior |
|---|---|
| `start()` | Starts the external link, registers the SIL Kit frame handler for the RX direction only, and registers the transmit acknowledgement handler. |
| `start_controller()` | Calls `Activate()` on the controller. |
| `stop()` | Removes the SIL Kit frame handler, stops the external link, removes the transmit acknowledgement handler and calls `Deactivate()` on the controller. |
| `fault()` | Returns the reason of a `Fault`. |
| `take_warnings()` | Returns the warnings, oldest first. |

- Call `start()` before the participant lifecycle starts. If it fails, it undoes what it started, then throws.
- `stop()` can be called any number of times.
- The bridge has no queue of its own. From SIL Kit to the outside it calls `transmit()` in SIL Kit's receive thread; from the outside to SIL Kit it calls `SendFrame()` in the receive thread of the external link. There is no backpressure; lost frames are counted.
- On a `Fault`, the bridge keeps running. The caller decides whether to stop.
- Warnings are the external link's `Warning`s and the bridge's own warnings (see "[Loop prevention](#loop-prevention-macsidetable)").

### When the controller starts

`start_controller()` is called from the participant's CommunicationReady handler (`runtime.add_communication_ready_handler([&bridge] { bridge.start_controller(); })`). This is the point that SIL Kit recommends (on a SIL Kit thread, before Running).

- Frames received from the external link before then are not passed to SIL Kit (and not counted). From SIL Kit to the outside, forwarding runs from `start()`.
- If `Activate()` fails, it is reported through `fault()`, not by an exception.
- `start()` after `start_controller()` (stopping and starting again) also calls `Activate()`.
- The bridge is an autonomous participant, so there is no guarantee that it has found a network simulator for Ethernet by that point.

### Order of frame checks

| Direction | Order |
|---|---|
| SIL Kit to the outside | Length (`malformed`, `oversize`) → filter (`filtered_to_ext`) → MAC rules (`loop_suspected`) → `transmit()` |
| Outside to SIL Kit | Length (`malformed`, `oversize`) → MAC rules (`own_tx_dropped`, `loop_suspected`) → from a device (`ext_from_devices`) → filter (`filtered_from_ext`) → `SendFrame()` |

For frames from the outside, the MAC rules apply before the filter, so that echoes of the bridge's own frames and frames from addresses on both sides are counted as such even if the filter would not pass them.

## Verified and not verified

### Verified (Linux, automated tests)

The tests `ethernet_bridge.netns.*` verify the following against the real executable and kernel, in a network namespace inside a user namespace.

`tap`:

- A frame sent from a packet socket on the TAP device side is received unchanged by a peer on the SIL Kit side, and the response comes back. Every frame that comes in counts in `ext_from_devices`.
- Frames that the bridge writes do not come back to the bridge.
- A second bridge is refused.
- A device created with `--tap-create` is created down and disappears when the bridge stops.

`packet` (veth pair):

- Untagged frames, frames with 1 VLAN tag and frames with 2 tags pass in both directions with their tags.
- A frame that another socket on the same interface sends is received by a peer on the SIL Kit side. It does not count in `ext_from_devices` (the kernel marks it as sent here), nor do frames from the address of the interface or of another interface of the namespace.
- Frames that the bridge sends do not come back to the bridge.
- Promiscuous mode with `--promiscuous` raises the promiscuity of the interface only while the bridge runs. The default and `--no-promiscuous` are not checked on the real kernel here (the unit tests cover how the bridge chooses the mode).
- Health lines and link lines for the link going up and down (`link link=up`, then `down`, then `up` again, each once).
- UDP sent by the OS stack (from the other end of the veth pair and from the bridge's own interface) is received unchanged by a peer on the SIL Kit side, counted in `ext_checksum_incomplete`, and warned about once only.
- After `ethtool -K <name> tx off` as the warning says, there is neither a count nor a warning.

`pcap` (the same veth pair, with the real libpcap installed on this computer):

- Loading the library, and the shape of the functions and structures that the bridge uses.
- Untagged frames, frames with 1 VLAN tag and frames with 2 tags pass in both directions with their tags.
- A frame that another socket on the same interface sends is received by a peer on the SIL Kit side. It counts in `ext_from_devices` (libpcap does not tell that it was sent here), but frames from the address of the interface or of another interface of the namespace do not.
- Frames that the bridge sends do not come back to the bridge.
- `--promiscuous`, `--list-interfaces`, and health lines for the link going up and down.
- A `packet` bridge on the same interface: the two find each other through their device marks (`shared`, `peers`, the warning), `--list-interfaces` says that 2 bridges use the interface, and the first one's `shared` goes back to 0 once the second stops.
- The messages when the interface cannot be opened (missing, not Ethernet, down, privileges dropped).
- CI runs the same tests with the libpcap of the Ubuntu package `libpcap0.8` and fails if they are skipped (`VEHICLE_SIL_REQUIRE_ETHERNET_NETNS=1`).

`vehicle_sil_launch run --connect` (the same veth pair, `ethernet_bridge.netns.launch`):

- The launcher finds the bridge on `PATH`, starts it with its default backend (`packet`) on one end, with the ECU's MAC address as `--src-mac`, and stops it with exit code 0 at the end.
- The frames of an ECU in SIL Kit reach the other end, and the answers of a device there come back to the ECU (a scenario at speed 1 with `--strict` expects them, and the run has no finding).

Common to all backends:

- The messages and exit code 3 when the interface cannot be opened (missing, not a TAP device, not Ethernet, already exists, busy, privileges dropped).
- `FAIL` and exit code 1 when the interface is deleted.
- The message and exit code 3 when there is no registry.

### Verified (all platforms, fake pcap library)

Verified against the fake pcap library `tests/eth_link/fake_pcap`. The fake is a shared library with functions of the same names as the real one, and the bridge loads it the same way as the real one.

- Loading the library by path and getting the functions by name. The messages and exit code 3 when the library is missing or is an old version that lacks functions.
- What the bridge requests when it opens (immediate mode, non-blocking reads, promiscuous mode; with the Npcap way of opening, also the option not to receive its own transmissions).
- Sending and receiving frames, frames that are too long, stopping, devices that cannot be opened, transmit failures and congestion, read failures (`FAIL` and exit code 1), the dropped count and warnings.
- On a device that never notifies that frames are there, all frames are received in order and one warning appears. While there are no frames, the bridge reads only once per wait.
- Both the Npcap way of opening (`pcap_open()`) and the libpcap way (`pcap_create()`), on every OS.
- With the Npcap way of opening, the bridge's own transmissions do not come back even on a device that echoes sent frames, and frames that other handles on the same device send are received.
- A loop of 2 bridges connected to the same device is stopped, whatever order the copies arrive in. Even with `--no-mac-guard`, the bridge's own frames do not come back into SIL Kit.
- A library without `pcap_open` is refused as Npcap.
- Answers from a device count in `ext_from_devices`; answers from the address of an adapter in the library's list are forwarded but do not count (Linux only: on Windows the addresses come from the OS, which does not know the fake adapters). With `--stats-every-ms`, the statistics line comes while the bridge runs, and again only where it changed.
- On Windows, the DLLs that the fake library uses are only those in the fake library's directory or the system directory (tests `eth_link.fake_pcap_imports.*`). The bridge has DLLs searched for only in these 2 places, so the fake library links the compiler runtime statically.

### Verified (Windows, real Npcap, manual)

Verified by hand against the real Npcap, on the virtual NIC of a virtual machine, on the Microsoft KM-TEST Loopback Adapter and on one physical USB adapter (the last item). These checks are not automated tests.

- `wpcap.dll` loads from `System32\Npcap` (and `Packet.dll` is found). The functions the bridge uses are there, and the structures have the expected shape (the list, frame length and content, the dropped count).
- With the receive event, the bridge receives without busy-waiting and without losing frames.
- Connection names appear in the list and match `--interface`, in ASCII and in Japanese (English Windows), in a console and in a file. The adapter identifier (lower case, without braces) and the pcap name open the adapter too. A name that differs only in upper and lower case does not match.
- Round trips between a peer on the SIL Kit side and a device beyond the adapter. Without promiscuous mode (`--no-promiscuous` today; the default was off then), unicast frames to SIL Kit devices are not received and the hint warning appears.
- Frames pass in both directions with other programs that open the same adapter with Npcap (on the virtual NIC and on the KM-TEST Loopback Adapter).
- Npcap gives a handle back the frames that handle sent. A handle opened with `PCAP_OPENFLAG_NOCAPTURE_LOCAL` does not receive its own transmissions but receives those of other handles.
- On an adapter that the bridge opens the current way (`pcap_open()` with the option not to receive its own transmissions), the bridge's transmissions do not come back to it (also with `--no-mac-guard`). 2 bridges on the same adapter warn and stop forwarding, whichever starts first. Receiving is not delayed, and the bridge uses no CPU when idle.
- The bridge receives frames that the OS sends on the same adapter (ARP).
- Whether frames with VLAN tags pass depends on the adapter driver and its settings (see "[`npcap` and `pcap`](#npcap-and-pcap)"). One NIC kept all tags; on another, one direction lost them with either setting.
- Frames that Npcap drops because the bridge did not read them in time are counted in `ext_overruns` (exactly the frames that reached the adapter but not SIL Kit), and a health line reports them.
- Frames sent from SIL Kit in bursts all left the virtual NIC; no transmit congestion occurred, and `ext_queue_full` stayed 0. While the virtual NIC's link was down, every send failed and was counted in `ext_tx_failures`, with the library's text after `last error:`, and the bridge kept running.
- On the KM-TEST Loopback Adapter, frames that the OS sends reach the bridge, and frames that the bridge sends do not reach programs that use ordinary OS sockets (see "[Connecting within one computer on Windows](#connecting-within-one-computer-on-windows)").
- On a stop request (Ctrl+C, Ctrl+Break), the bridge prints the statistics and exits with code 0. Disabling the adapter while the bridge runs gives `FAIL` and exit code 1. Disabled adapters are not in the list.
- Npcap installed without the limit can be used by users who are not administrators. If Npcap was installed for administrators only and a user who is not an administrator uses it, the bridge stops at the elevation prompt.
- Physical NIC: one USB Ethernet adapter (ASIX AX88772, 100 Mbit/s, the driver that comes with Windows), passed through to the virtual machine, on a LAN with the network stack of the host computer (on its own NIC) as the peer (ARP). The list and `--interface` find the adapter. Without promiscuous mode (`--no-promiscuous` today; the default was off then), the request goes out, the unicast reply is not received, and the hint warning appears. With `--promiscuous`, the adapter accepts promiscuous mode and the reply reaches the SIL Kit side. The bridge's transmissions do not come back to it, and no loop warning appears. Frames that the OS sends on the adapter reach the bridge. Disabling the adapter while the bridge runs gives `FAIL` and exit code 1. Ctrl+C prints the statistics and exits with code 0.

### Not verified

Windows:

- Physical NICs other than the one USB adapter above; a device or a second computer on a cable; pulling the cable while the bridge runs; wireless adapters.
- High traffic on a physical NIC, and VLAN tags on a physical NIC.
- Windows in Japanese (how its console shows connection names in Japanese).
- Computers with Npcap installed in WinPcap compatible mode.
- An adapter that refuses promiscuous mode: whether it fails to open with promiscuous mode (the bridge then opens it again without, and warns) or opens without it and says nothing (the bridge then cannot tell). The fallback is tested with the fake library only.
- Whether there is an Npcap or adapter that accepts the option against echoes but still echoes the bridge's own transmissions.
- The confirmation prompt that Windows with a desktop shows for Npcap installed for administrators only.
- Whether transmit congestion is ever counted in `ext_queue_full` with the real Npcap (it did not occur in the checks).
- The addresses of the adapters from Windows with the real Npcap, and whether `ext_from_devices` stays 0 on a `vEthernet` adapter, or on the physical NIC its switch is bound to, when that NIC has no cable (that the frames of the host's OS there come from the address of an adapter of the host), and how Npcap's option against the bridge's own transmissions bears on it. The hint when nothing comes from a device is verified only in unit tests, with the time given by the test.

Linux:

- Whether libpcap can tell that the transmit queue is congested (`ext_queue_full`). The library reports failures as a single kind, so the bridge tells them apart if the OS error number is still there. Verified with the fake library only.
- Physical NICs, and round trips with real hardware or another computer beyond them.
- How frames are received with and without promiscuous mode, and an interface that refuses it. A veth passes every frame to the socket even without promiscuous mode, so there is no difference in a namespace, and it accepts the mode. The promiscuous mode hint warning is verified only in tests with a replaced clock.
- Whether an executable with `setcap cap_net_raw+ep` can find SIL Kit as a shared library. In an executable granted privileges, the environment variables for finding libraries may have no effect.
- The message when connecting to a TAP device owned by another user. The tests check the wording, but no test could create a situation in which the kernel returns that error.
- Situations in which receive offload makes long frames appear; wireless adapters.
- The warning for frames with incomplete checksums on a physical NIC, and the effect of `ethtool -K <name> tx off` there (the tests use a veth pair only).
- That transmit segmentation offload makes long TCP frames appear, and that `tx off` stops it, was checked on a veth pair but is not in the tests.

Other:

- Starting the bridge from a configuration file with the real Npcap on Windows, and with a physical NIC on Linux. The automated test `ethernet_bridge.integration.pcap-launcher` starts it from a configuration file with the fake pcap library.
- The position of the EtherType, the VLAN TPIDs and the maximum length are not verified against the standards, as the note in "[Filter](#filter-framefilter)" says.

## Tests

Run them with `ctest -R '^(eth_link|ethernet_bridge)\.'`. Except for the namespace tests, they do not rely on real time: they use a fake external link and a fake controller and wait for events.

| Test | Platform | What it verifies |
|---|---|---|
| `eth_link.*` | All | MAC addresses, the filter, the MAC table, the fake external link |
| `ethernet_bridge.*` | All | Forwarding with a fake controller and a fake external link, statistics, health and link lines, argument error texts |
| `eth_link.os_link_*` | All | The messages and kinds when an OS external link cannot be opened, the `--list-interfaces` output |
| `ethernet_bridge.app_*` | All | The backends available on each OS and the message when one is not, the startup line, the promiscuous mode hint |
| `ethernet_bridge.integration.*` | All | The behavior of an executable linked with a fake external link, run as a process |
| `eth_link.pcap_*` | All | Messages of the pcap external link, the way of opening, name matching, the list, and sending, receiving and failures with the fake library |
| `ethernet_bridge.integration.pcap-*` | Linux, Windows | The behavior of an executable in which only the library location is changed to the fake |
| `ethernet_bridge.netns.*` | Linux | What "Verified (Linux, automated tests)" lists |

### `eth_link.*`

- The filter tests check EtherTypes, the position behind VLAN tags, MAC addresses and the maximum length.
- The MAC table tests check echoes, both sides, the limit and concurrency.
- `eth_link.boundary` checks that `libs/eth_link` includes no SIL Kit header, directly or indirectly.
- `eth_link.os_link_*` checks texts only, so it runs on all platforms.

### `ethernet_bridge.*`

- These tests check forwarding, the filter and echoes of the bridge's own frames.
- For a loop through 2 bridges, they check the orders in which it is stopped, the order in which it is not stopped with an external link that echoes its own transmissions, and the case of one bridge with `--no-mac-guard`.
- `ethernet_bridge.*_npcap_adapter_*` opens 2 pcap external links the Npcap way on the same device of the fake pcap library, and checks that the loop is stopped and that, even with `--no-mac-guard`, the bridge's own frames do not come back.
- They check a full MAC table, `stop()` in the middle of forwarding, lost frames, `Fault`, warnings, and starting and stopping.
- `ethernet_bridge.silkit_*` checks the real SIL Kit side with an in-process registry (on a free port) and 2 participants.
- `ethernet_bridge.app_*` checks the promiscuous mode hint with a replaced clock, without waiting 5 seconds.
- They check what counts in `ext_from_devices`: frames from devices, also those the filter keeps out, and not frames from any of this computer's addresses, marked as sent here, from the SIL Kit side, back from an echoing link, or too short; with `--no-mac-guard`, by the `--src-mac` addresses.
- The rule of the hint when nothing comes from a device (`bridge_runtime.nothing_came_hint_*`) and the statistics line while running (`bridge_runtime.run_bridge_*`) are checked with the time given by the test.

### `ethernet_bridge.integration.*`

These tests run, as processes, the executable linked with a fake external link (`vehicle_sil_ethernet_bridge_on_fake`, `tests/ethernet_bridge/fake_link_factory.cpp`), together with a registry and a peer on the SIL Kit side. The peer is `eth_bridge_it_peer`, which uses SIL Kit's `IEthernetController` directly.

- They check round trips and statistics, the startup line, argument errors (4), an interface that cannot be opened (3), `Fault` (1) and a stop request (statistics, `stopped`, 0).
- `device-shared` starts two bridges on `fake0`: with the default scope of the fake devices (one per process, so that tests that run at the same time do not find each other) they do not find each other; with one scope (`VEHICLE_SIL_FAKE_DEVICE_SCOPE`) the second finds the first at its start, the first finds the second while it runs and warns, both run on, `--list-interfaces --json` says `fake0` is in use by 2 bridges, and once the first ends the second's `shared` is 0 again.
- `backend-not-here` uses the real executable and checks the message for a backend not available on that OS, and the pcap list. If the library is on that computer, the output is the list; otherwise it is the message that says how to install the library.

### `eth_link.pcap_*` and `ethernet_bridge.integration.pcap-*`

- `eth_link.pcap_*` checks the messages of both Npcap and libpcap, on every OS.
- The ways of opening, Npcap's `pcap_open()` and libpcap's `pcap_create()`, are both checked on every OS.
- The fake library (`tests/eth_link/fake_pcap`) is a shared library with functions of the same names as the real one, loaded at run time.
- In the fake library, `fake0` is a device that responds. There are also devices that cannot be opened, cannot send, cannot be read, drop frames, are wireless, and echo what they send, and `fake-self`, where the answers come from the adapter's own address. `fake0`, `fake1` and `fake-self` list an address each, as libpcap on Linux does.
- There are 2 versions that lack one function each (an old version, and a version without `pcap_open`).
- `ethernet_bridge.integration.pcap-*` uses an executable with the same content as the installed one, except that the library location is changed to the fake library (`vehicle_sil_ethernet_bridge_on_fake_pcap`).
- `ethernet_bridge.integration.pcap-*` checks round trips and statistics, the list, the messages when the library is missing or old (3, before connecting to SIL Kit), a missing device (3) and a read failure (1).
- `pcap-describe` checks that `--describe` answers without the library.
- `pcap-roundtrip` and `echo-roundtrip` check that every answer of the device counts in `ext_from_devices`. `pcap-own-address` checks that answers from the adapter's own address are forwarded and do not count (Linux only). `pcap-nothing` sends to a device where nothing answers, with `--stats-every-ms 100`: the statistics line comes while the bridge runs, only where it changed, with every key in order, and `ext_from_devices=0`.
- `pcap-promiscuous` checks the default on a device that accepts promiscuous mode and on one that refuses it (the warning and the startup line), `--no-promiscuous`, `--promiscuous` on the one that refuses it (exit code 3), and both options together (exit code 4).
- `pcap-launcher` writes a configuration file with the bridge (`role: bridge`, `required: false`) and an ECU on its network (`eth_bridge_it_ecu.cpp`), with `registry_uri` on port 0. It checks that `check` passes without a finding and lists the bridge on the network, that the ECU's frames go out through the bridge and the device's answers come back to the ECU (a scenario expects them), and that the bridge ends with exit code 0 when the run ends.

### `ethernet_bridge.netns.*`

`tests/ethernet_bridge/bridge_netns.py` runs `ethernet_bridge.netns.tap`, `ethernet_bridge.netns.packet`, `ethernet_bridge.netns.pcap` and `ethernet_bridge.netns.launch` (`vehicle_sil_launch run --connect` with this script as the device at the other end of a veth pair). The conditions for skipping and for leaving parts out:

| Condition | What happens | Environment variable that makes it a failure |
|---|---|---|
| A user namespace cannot be created | Skipped (exit code 77). | `VEHICLE_SIL_REQUIRE_MULTI_HOST=1` |
| The namespace can be created, but a TAP device, veth or packet socket cannot be created in it | Skipped. | `VEHICLE_SIL_REQUIRE_ETHERNET_NETNS=1` |
| libpcap is not installed (`pcap` only) | Skipped. | `VEHICLE_SIL_REQUIRE_ETHERNET_NETNS=1` |
| `setpriv` or `ethtool` is missing | Only the parts that need it are left out, with `NOT CHECKED` in the output. | `VEHICLE_SIL_REQUIRE_ETHERNET_NETNS=1` |

The `skipped:` line of the output shows which condition caused a skip. CI sets both variables, so both a skip and a left-out part are failures.
