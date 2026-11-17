# Vector XL CAN Bridge

[日本語](ja/bridges.md)

`vehicle_sil_vector_xl_bridge` forwards classic CAN frames in both directions between a SIL Kit CAN network and a Vector XL CAN channel. This page covers the connection steps, arguments, exit codes, statistics, forwarding behavior, the build and the test coverage.

| Connects to | `--xl-target` | Requirement | Verified | Not verified |
|---|---|---|---|---|
| Vector Virtual CAN | `virtual` | Windows and the Vector XL Driver Library | Sending and receiving with an MSYS2 UCRT64 build on Windows 11 and the real XL Driver Library | — |
| Physical CAN (CANcase, VN16xx and others) | `physical` | The above, plus a Vector CAN interface | Sending and receiving over a physical loopback that connects the 2 channels of a VN5620 | Other physical CAN setups and devices |

> [!WARNING]
> Sending and receiving on Vector Virtual CAN is verified with an MSYS2 UCRT64 build on Windows 11 and the real XL Driver Library. Sending and receiving is also verified over a physical loopback that connects the 2 channels of a VN5620. Other physical CAN setups and devices are not verified.

For the Ethernet Bridge (`vehicle_sil_ethernet_bridge`), see [ethernet-bridge.md](ethernet-bridge.md).

## First connection

These steps connect the bridge to the first channel of Vector Virtual CAN. The bridge runs on Windows only.

| Step | What you do | What you see |
|---|---|---|
| 1 | Build the bridge, giving the location of the XL Driver Library (see "[Build](#build)"). | `vehicle_sil_vector_xl_bridge` is built. |
| 2 | List the available channels. | A numbered list of channels for each target. |
| 3 | Start the SIL Kit registry, then start the bridge. | The line `bridging SIL Kit network ...` appears. |
| 4 | Stop the bridge with Ctrl+C. | One line of statistics appears and the bridge exits with code 0. |

The command for step 2 prints a list of this form. In step 3, use the number shown after `--xl-channel`.

```sh
vehicle_sil_vector_xl_bridge --list-channels
```

```
virtual CAN channels (--xl-target virtual):
  --xl-channel 1  <channel name>  hwType=<n> hwIndex=<n> hwChannel=<n> serial=<n> <transceiver name>
physical CAN channels (--xl-target physical):
  (none)
```

The command for step 3:

```sh
# connect to the first channel of Vector Virtual CAN
vehicle_sil_vector_xl_bridge --registry-uri silkit://127.0.0.1:8500 --network J1939_CAN1 --xl-target virtual --xl-channel 1
```

When the bridge is connected, it prints 3 lines of this form:

```
[bridge] XL channel: virtual 1 = "<channel name>" (hwType=<n>, serial=<n>)
[bridge] XL port open, init access: <yes|no>, bit rate unchanged
[bridge] bridging SIL Kit network 'J1939_CAN1' (participant 'VectorXlBridge', silkit://127.0.0.1:8500) <-> XL '<channel name>'; Ctrl+C to stop
```

To connect to physical CAN, give `--xl-target physical`. This example connects to the first channel at 250 kbit/s:

```sh
vehicle_sil_vector_xl_bridge --registry-uri silkit://127.0.0.1:8500 --network J1939_CAN1 --xl-target physical --xl-channel 1 --bitrate 250000
```

The bridge stops on Ctrl+C, and on Windows also when you close its console window. In both cases it prints the forwarding statistics.

> [!CAUTION]
> Do not unplug the USB cable of the CAN interface while the bridge runs. The bridge stops (exit code 1), but after being plugged back in, a VN5620 was sometimes not detected until it was power-cycled.

If something goes wrong, find the symptom in this table.

| Symptom | Cause | Fix |
|---|---|---|
| Prints `error: ...` and exits with code 4 | An argument is wrong. | Correct it as described in "[Arguments](#arguments)". |
| `FAIL setup: cannot connect to the SIL Kit registry ...` | The bridge cannot connect to the registry. | Do what the message says (see "[Message when the registry is unreachable](#message-when-the-registry-is-unreachable)"). |
| Prints the list of available channels and exits with code 3 | No channel has the number given to `--xl-channel`. | Give a number from the `--list-channels` output. |
| Startup fails when you add `--bitrate` | Another application holds init access. | Leave out `--bitrate`. The bridge then connects without changing the channel configuration. |

## Arguments

```
vehicle_sil_vector_xl_bridge --list-channels [--json]
vehicle_sil_vector_xl_bridge --registry-uri <silkit://host:port> --xl-target <virtual|physical> --xl-channel <n> [--bitrate <bit/s>] [--network <name>] [--name <name>] [--start-timeout-ms <ms>] [--stats-every-ms <ms>] [--rx-queue-size <n>] [--tx-timeout-ms <ms>] [--bus-off-restart-ms <ms>] [--no-bus-monitor]
vehicle_sil_vector_xl_bridge --describe --xl-target <virtual|physical> --xl-channel <n> [options]
```

| Argument | Meaning |
|---|---|
| `--list-channels` | Prints the CAN channels that the XL driver offers, numbered per target, and exits. |
| `--json` | With `--list-channels` only: writes the list as JSON to stdout, for programs (see "[The channel list as JSON](#the-channel-list-as-json---json)"). |
| `--registry-uri <uri>` | Address of the SIL Kit registry (required). |
| `--xl-target <virtual\|physical>` | `virtual` selects Vector Virtual CAN (`XL_HWTYPE_VIRTUAL`); `physical` selects the CAN channels of all other hardware, such as CANcase and VN16xx (required). |
| `--xl-channel <n>` | 1-based channel number among the CAN channels of that target (required). |
| `--bitrate <bit/s>` | Sets the bit rate of the XL channel, 15000–1000000 bit/s (the range of `xlCanSetChannelBitrate`). |
| `--network <name>` | Name of the SIL Kit CAN network (default `J1939_CAN1`). |
| `--name <name>` | Participant name (default `VectorXlBridge`). |
| `--participant <name>` | The same as `--name` (give one of them). |
| `--start-timeout-ms <ms>` | How long the bridge waits for its participant to reach Running in SIL Kit (1–10^12, default 15000). If it does not, the bridge exits with code 3. |
| `--stats-every-ms <ms>` | Also prints the statistics line while the bridge runs: at the first interval, then at each interval where a count changed (100–3600000). Without it, the line comes once, when the bridge ends (see "[Statistics](#statistics)"). |
| `--describe` | Writes what the bridge is as JSON to stdout and exits (see "[Starting from a configuration file](#starting-from-a-configuration-file)"). |
| `--rx-queue-size <n>` | Size of the XL receive queue (a power of two, 16–32768, default 16384). |
| `--tx-timeout-ms <ms>` | Drops new frames while the bus is congested (100–60000; see "[Counting frames on the bus (transmit receipts)](#counting-frames-on-the-bus-transmit-receipts)"). |
| `--bus-off-restart-ms <ms>` | Reactivates the channel when bus-off lasts this long (100–60000, or 0 to disable; default 1000; see "[Bus monitoring while running](#bus-monitoring-while-running)"). |
| `--no-bus-monitor` | Does not ask the XL driver for the error state of the CAN controller. |
| `--help` | Prints the usage. |

Notes on the arguments:

- The channels for `--xl-channel` are numbered in the order of the driver configuration, the same order as the `--list-channels` output. Channels without CAN support (LIN only, for example) are not counted.
- `--bitrate` needs init access. If another application holds init access, startup fails; the bridge does not silently run at a different bit rate. Without `--bitrate`, the bridge leaves the channel configuration as it is.
- With `--rx-queue-size`, the driver actually holds 1 event fewer than the value you give.
- `--tx-timeout-ms` and `--bus-off-restart-ms` take effect only with init access (`--bitrate`). If you give them without init access, the bridge prints a `warning:` at startup.
- On Windows, the bridge reads its arguments as UTF-8 from the wide-character command line. `--name` and `--network` can therefore contain non-ASCII characters.

You choose the channel and the device on this command line when the bridge starts. The Control API and the configuration file (`vehicle_sil_launch`) have no setting to choose them. If you add a way to choose them to the Control API, the configuration file or the GUI, use the same model as the command line (`vehicle_sil::vector_xl::XlTarget` and the channel number).

### The channel list as JSON (`--json`)

`--list-channels --json` writes the channels as one JSON object to stdout, in UTF-8, followed by a line break. The text is for people and its layout may change; programs read `--json`. The form is that of the Ethernet bridge's `--list-interfaces --json` (see "[The list as JSON](ethernet-bridge.md#the-list-as-json---json)"); this section gives what differs.

```json
{
  "vehicle_sil_targets": 1,
  "tool": "vehicle_sil_vector_xl_bridge",
  "backend": null,
  "source": null,
  "targets": [
    {
      "value": "virtual:1",
      "label": "Virtual Channel 1",
      "detail": "virtual 1; hwType=1 hwIndex=0 hwChannel=0 serial=0 Virtual CAN",
      "usable": true,
      "reason": null,
      "can": {
        "target": "virtual",
        "number": 1,
        "name": "Virtual Channel 1",
        "hw_type": 1,
        "hw_index": 0,
        "hw_channel": 0,
        "serial": 0,
        "transceiver": "Virtual CAN"
      }
    }
  ],
  "notes": [],
  "error": null
}
```

| Key | Content |
|---|---|
| `tool` | `vehicle_sil_vector_xl_bridge`. |
| `backend`, `source` | `null`. |
| `targets` | The CAN channels, virtual first, then physical, numbered as for `--xl-channel`. Channels without CAN are left out. |
| `targets[].value` | `<target>:<number>`: give the part before `:` to `--xl-target` and the number to `--xl-channel`. |
| `targets[].label` | The channel name from the driver. |
| `targets[].detail` | The target and number, then what the line of the text shows. |
| `targets[].usable` | Always `true`. |
| `targets[].can` | `target`, `number`, `name`, `hw_type`, `hw_index`, `hw_channel`, `serial` and `transceiver` (`null` where the driver gives none). |
| `notes` | Empty. |

When the list cannot be made, the bridge writes the JSON with no targets and the error, and exits with code 3. The `FAIL setup: ...` line, which is on stdout without `--json`, goes to stderr, so that stdout holds the JSON alone. `error.code` is `driver-missing` when the XL driver cannot be opened, and `failed` otherwise. `error.message` is the message of the `FAIL setup:` line.

## Starting from a configuration file

`vehicle_sil_launch` can start the bridge with the other participants of a simulation. Write it in the configuration file like an ECU (`describe: true`, the default), with `role: bridge` and `required: false`:

```yaml
participants:
  - name: XlBridge
    role: bridge
    required: false
    command: [vehicle_sil_vector_xl_bridge, --network, VehicleCAN, --xl-target, physical, --xl-channel, "1"]
```

The network (`VehicleCAN`) must be in `networks` with `kind: can`. `check` adds `--describe --name XlBridge`, and `run` adds `--name XlBridge --registry-uri <URI> --start-timeout-ms <ms>` (so `registry_uri` may have port 0).

- With `--describe`, the bridge checks its arguments as for a run (an error is exit code 4, which `check` reports as `describe-failed` with the bridge's message; do not write `--participant` or `--name` in `command`, because the launcher adds `--name`), writes the JSON that an ecu_main executable writes for `--describe` (see "[JSON format](ecu-executable.md#json-format)" in [ecu-executable.md](ecu-executable.md)) and exits with code 0, without opening the XL driver or connecting to SIL Kit. `--registry-uri` is not needed; if it is given, it is not used.
- The JSON has the participant name, `"step_ns": null` (the bridge runs in real time), the network of `--network` with `"kind": "can"` and `"use": "raw"`, no variables, `"control": null` and an empty `features` list. `check` therefore knows which network the bridge carries (see "Test participants and bridges (`role`)" in [simulation-config.md](simulation-config.md#test-participants-and-bridges-role)).
- The executable needs `vxlapi64.dll` to start at all, also for `--describe`: Windows loads it with the executable (the build copies it next to the executable when it finds it). On a computer without it, `check` reports `describe-failed`: the bridge could not start because a DLL it needs was not found. Leave the bridge out there with `--disable XlBridge`.
- `required: false`: the simulation does not wait for the bridge (see "[Startup order](#startup-order)"), and when the run ends, the launcher asks the bridge to stop.

To have the launcher start the bridge without an entry in the file, give `--connect <network>=<target>` to `check` and `run` (see "[`--connect`](simulation-config.md#--connect)" in simulation-config.md). The adapter or channel differs from PC to PC, so it is given when you start, not written in the shared file. `run` then starts the bridge before the ECUs, waits until it runs, so that the first frames reach the bus too, and fails the run if the bridge ends (see "[Connecting a bus outside](simulation-run.md#connecting-a-bus-outside---connect)" in simulation-run.md).

## Exit codes

| Exit code | Meaning |
|---|---|
| 0 | Normal end on a stop request (Ctrl+C, Ctrl+Break, SIGTERM, closing the console window). Also 0 when `--list-channels`, `--describe` or `--help` finishes. |
| 1 | Failure while running. |
| 3 | Failure during setup (XL driver, channel selection, SIL Kit). |
| 4 | Argument error. |

The exit code is 1 in any of these cases:

- The SIL Kit participant went into a stopping or ended state (the registry stopped or the connection was lost).
- The participant stayed in a starting state and never reached Running.
- The XL channel became unusable (the device was removed).

### Participant state while running

| State | Handling | Output |
|---|---|---|
| A stopping or ended state (Stopping, Stopped, Error, ShuttingDown, Shutdown, Aborting) | Failure (exit code 1). | `FAIL: SIL Kit participant left Running (state <state>)` |
| The same states, reached before Running | Failure (exit code 1). | `FAIL: SIL Kit participant stopped before it reached Running (state <state>)` |
| A starting state (ReadyToRun and others), 1 second after monitoring began | Not a failure. | `waiting for the SIL Kit participant to reach Running (state <state>; at most 15000 ms)`, once |
| Still in a starting state after 15 seconds | Failure (exit code 1). | `FAIL: SIL Kit participant did not reach Running within 15000 ms (state <state>); its SIL Kit log may say why` |
| Paused | Not a failure. | None |

The bridge never pauses on its own.

### Closing the console window (Windows)

When you close the console window on Windows (and also on logoff or shutdown), the `SetConsoleCtrlHandler` handler requests a stop, waits up to 4 seconds for the bridge to finish stopping (which includes printing the statistics), then ends the process with the bridge's exit code. The handler waits because Windows ends the process as soon as the handler returns, and gives the handler only about 5 seconds. Those 5 seconds cannot be changed, so the bridge prints the statistics before it stops the SIL Kit participant, and waits at most 2 seconds for the participant to stop. If it still takes longer than 4 seconds, Windows ends the process and the exit code is not 0 (the statistics are already printed by then).

### Code shared by the bridge executables

The bridge executables share the following through `run_bridge()` (`vehicle_sil::bridge_runtime`) in `bridges/common`:

- How stop requests are received
- The monitoring loop
- The stop sequence (warnings raised just before the stop, then the statistics, are printed before the SIL Kit participant is stopped)
- The exit codes
- The 4-second wait when the console window is closed
- The statistics line while running (`--stats-every-ms`; `BridgeHooks::statistics_line()`, `RunBridgeOptions::statistics_interval`)
- When to say that nothing came from a device on the bus the bridge sends to (`NothingCameHint`, `nothing_came_hint.hpp`): 10 seconds after the first frame sent out, once, unless a frame from a device has come. The Ethernet bridge counts those frames in `ext_from_devices`, this bridge in `xl_to_silkit` (see "[Statistics](#statistics)")

## Error messages

### Argument errors

On an argument error, the bridge prints only 2 lines to stderr (the error and a pointer to the usage) and exits with code 4. `--help` prints the full usage.

```
error: --xl-target must be 'virtual' or 'physical', got: cancase
Run 'vehicle_sil_vector_xl_bridge --help' for the options.
```

### No channel found

If you give `--xl-channel` a number that does not exist, the bridge prints the list of available channels and exits with code 3.

### Message when the registry is unreachable

If the bridge cannot connect to the registry, it prints one line with the address it tried and what to do next, and exits with code 3. `registry_failure_message()` in `bridges/common` builds the message.

```
[bridge] FAIL setup: cannot connect to the SIL Kit registry at <uri> (<SIL Kit text>). Start the registry first ('sil-kit-registry --listen-uri <uri>'), or give the address it listens at with --registry-uri.
```

The part after `(<SIL Kit text>).` depends on the address. The host is compared case-insensitively.

| Host in `--registry-uri` | What the message tells you |
|---|---|
| A loopback address of this computer (`localhost`, `127.x.y.z`, `::1`), or an address that cannot be parsed | As in the line above: start the registry first, or give the address it listens at. |
| Any other host | On the computer with the registry, check that the registry runs and listens on an address reachable from here, and check its firewall. |
| `0.0.0.0` and `::` (`[::]`) | These are addresses to listen on and cannot be connected to. Give the address of the computer the registry runs on. |

The text for "any other host" is below. The bridge does not determine whether the address belongs to another computer or is a LAN address of this one, so the text claims neither.

```
... <host> is not the loopback address of this computer: check that the registry runs on the computer with that address (which may be this one), that it listens on an address reached from here ('sil-kit-registry --listen-uri silkit://0.0.0.0:<port>' there listens on all of its addresses, not only on its loopback), and that its firewall lets port <port> through.
```

The text for `0.0.0.0` and `::` (`[::]`):

```
... 0.0.0.0 stands for every address of a computer: a registry listens on it, a participant cannot connect to it. Give --registry-uri the address of the computer the registry runs on (silkit://127.0.0.1:<port> if it runs on this one).
```

If the address has no port, `<port>` in these texts is 8500 (the port of a registry started without one).

### Output encoding

The bridges (`vehicle_sil_vector_xl_bridge`, `vehicle_sil_ethernet_bridge`) and `vehicle_sil_can_bus_simulator` write their stdout and stderr text in UTF-8. On Windows, how they write depends on where the output goes (`cli::write_text()` in `tools/common`; `vehicle_sil_launch` writes the same way).

| Output goes to | How it is written |
|---|---|
| A console (Command Prompt, PowerShell) | As UTF-16 (`WriteConsoleW`). |
| A file or a pipe | As UTF-8 bytes. |

- In a console, non-ASCII characters (a `--participant` name, the name of a network connection) are readable whatever the code page (932 on Japanese Windows). The bridges do not change the code page, and you do not need `chcp`.
- An MSYS2 terminal (mintty) counts as a pipe, so it receives UTF-8 bytes. Have it read them as UTF-8.
- In Windows PowerShell 5.1, `>` and `|` re-decode the bytes they receive with `[Console]::OutputEncoding` (by default the console code page), which garbles non-ASCII characters. Run `[Console]::OutputEncoding = [Text.Encoding]::UTF8` first, or use `>` in Command Prompt.
- SIL Kit's own log is not written this way.

## Statistics

When the bridge ends, it prints the statistics on one line: `stats`, then one ` <key>=<count>` for each key of the table below, in its order. With `--stats-every-ms <ms>` it also prints the line while it runs: at the first interval, then at each interval where a count changed. The counts are totals since the start, each read on its own (one line need not show one instant). The keys and their order are fixed by this page for programs that read the line, and the tests hold them.

```
stats silkit_to_xl=0 xl_to_silkit=0 own_tx_dropped=0 error_frames=0 overruns=0 silkit_rejected=0 xl_rejected=0 xl_queue_full=0 xl_tx_failures=0 silkit_tx_failures=0 xl_tx_dropped=0 xl_tx_unsent=0
```


| Key | Counts |
|---|---|
| `silkit_to_xl` | Frames handed to the XL transmit queue |
| `xl_to_silkit` | Frames received from XL and forwarded to SIL Kit |
| `own_tx_dropped` | Receipts of the bridge's own transmissions (with receipts enabled, the same as the number of frames that went out on the bus) |
| `error_frames` | XL error frames |
| `overruns` | Overflows of the XL receive queue |
| `silkit_rejected` | Frames from SIL Kit that are not forwarded (CAN FD, CAN XL, invalid ID or DLC) |
| `xl_rejected` | Frames from XL that are not forwarded (in practice, frames with a DLC above 15) |
| `xl_queue_full` | Frames dropped because the XL transmit queue was full |
| `xl_tx_failures` | Other failures to transmit to XL |
| `silkit_tx_failures` | Frames for which `ICanController::SendFrame` threw an exception |
| `xl_tx_dropped` | Frames dropped by `--tx-timeout-ms` while the bus was congested |
| `xl_tx_unsent` | Frames handed to XL that were lost without going out on the bus (on stop, or when the channel was reactivated) |

`xl_to_silkit` counts only frames from other nodes on the bus: the receipts of the bridge's own transmissions are in `own_tx_dropped`. If the bridge has sent frames to the channel and not one frame has come from the bus 10 seconds after the first (`xl_to_silkit` is still 0), it prints a hint as a warning, once:

```
nothing has come from a device on XL 'Virtual Channel 1' in the 10 s since the bridge first sent a frame there: if a device is meant to answer, check that it is on, its cable and termination, the bit rate, and that this is the channel it is on.
```

A frame from the bus that came earlier keeps the hint back as well. The hint also appears when no answer is expected (when the bridge only sends), so the text is conditional. The Ethernet bridge gives the same hint (see "Hint when nothing comes from a device" in [ethernet-bridge.md](ethernet-bridge.md#hint-when-nothing-comes-from-a-device)).

## Physical CAN and virtual time

- When you connect to physical CAN, run the simulation in real time as a rule.
- Treat Vector Virtual CAN, too, as not synchronized automatically with SIL Kit's virtual time.
- The Vector XL CAN Bridge uses the autonomous lifecycle and runs in real time. Do not make it a participant of a virtual-time simulation.
- A protocol stack placed directly on an XL channel (`ExternalPortAdapter`) also runs in real time only. Registering it with a host that runs with time synchronization is a configuration error.

If you put the bridge on the same network as ECUs that run in virtual time, each ECU receives a forwarded frame with the time of the ECU step in which the frame arrives. A frame received before the simulation starts gets time 0. The wall clock decides which ECU step receives a frame, so the result is not deterministic (see "SIL Kit port (`SilKitCanPort`)" in [internals/can.md](internals/can.md#sil-kit-port-silkitcanport)).

Across a bridge, the order of frames breaks as well as their times. All frames that arrive during one ECU step get the same time, and a J1939 `Network` takes frames with the same time one sender at a time, smallest CAN ID first, so it can handle them in another order than they had on the physical bus. When you listen to a transfer between physical ECUs with a `j1939::Listener` or a `TransferSniffer`, the EndOfMsgACK can come before the last packet, or the next announcement before the previous EndOfMsgACK. The first case is caught, because the sniffer waits until time advances before it decides; in the second case, both transfers are missed. Give the listening participant a short step size, and write the statistics of missed transfers (`incomplete`, `replaced`) to variables to check them (see "Hearing what other participants say (`Listener`)" in [internals/j1939.md](internals/j1939.md#hearing-what-other-participants-say-listener)).

## Forwarding behavior

| Case | What the bridge does |
|---|---|
| Classic CAN frames | Forwards the identifier (11-bit or 29-bit), IDE, RTR, DLC and data unchanged. |
| DLC 9–15 from SIL Kit to XL | Sends them as DLC 8. |
| CAN FD, CAN XL, and frames with an invalid ID or DLC | Does not forward them; counts them in `silkit_rejected` or `xl_rejected`. |
| XL error frames | Does not forward them; counts them in `error_frames`. |
| Overflows of the XL receive queue | Counts them in `overruns`. |

- DLC 9–15 becomes DLC 8 because `xlCanTransmit` accepts DLC 0–8 only (XL Driver Library manual). In classic CAN, DLC 9–15 all mean 8 data bytes. From XL to SIL Kit, the bridge forwards the DLC as received.
- On the XL side, the ID is taken from the receive event as 11 or 29 bits, so in practice `xl_rejected` counts only frames with a DLC above 15.

Forwarding from XL to SIL Kit starts when the participant reaches CommunicationReady in SIL Kit's lifecycle. At that point the bridge starts the SIL Kit controller (the point that SIL Kit recommends). Frames received from XL before then, during a short time right after startup, are neither forwarded nor counted. Forwarding from SIL Kit to XL already runs before that point.

### Counting frames on the bus (transmit receipts)

`silkit_to_xl` is the number of frames handed to the XL transmit queue. A frame that cannot go out on the bus (no node to acknowledge it, a different bit rate, an unplugged cable) stays in the XL transmit queue. All such frames go out together once the bus can take them.

`XlCanPort` enables transmit receipts and counts the frames it handed over that have no receipt yet. `own_tx_dropped` (the number of receipts of the bridge's own transmissions) is the number of frames that went out on the bus.

The port relies on receipts if it could enable them itself (with init access), or once at least one receipt has arrived. On a channel without init access where another application has disabled receipts, the wait on stop does not work and `xl_tx_unsent` stays 0.

On stop (`XlCanPort::stop()`), before it deactivates the channel, the port waits up to 1 second for the handed-over frames to go out on the bus (`XlCanPortOptions::drain_timeout`). Frames that still have not gone out are counted in `xl_tx_unsent`, because deactivating the channel discards what is left in the XL transmit queue. If the bridge on the other side stops at the same time, no node acknowledges the frames, and they may never go out, however long the port waits.

There is no backpressure when the simulation runs faster than the bus. If you run faster than the CAN bandwidth allows, frames pile up in the XL transmit queue and, without `--tx-timeout-ms`, end up in `xl_tx_unsent` on stop.

The bridge does not use `xlCanFlushTransmitQueue`, which removes frames from the XL transmit queue. On a VN5620 it returned success, but the frames in the queue still went out on the bus afterwards.

#### `--tx-timeout-ms` (drop frames while the bus is congested)

With `--tx-timeout-ms <ms>` (100–60000, `XlCanPortOptions::transmit_timeout`), while the bus is congested the bridge drops new frames instead of handing them to XL, and counts them in `xl_tx_dropped` (`TransmitResult::Dropped`). The bus is congested when none of the frames handed over since the last transmit receipt has received a receipt for the given time. This limits the stale frames that flood the bus when it recovers to that time's worth plus the probe frames described below. Without this option, frames wait as long as it takes, then go out.

Congestion is decided only by when receipts arrive, not by the number of waiting frames. That count drifts when receipts are lost in an overflow of the receive queue (on an overflow, the count is reset to 0 and congestion is judged afresh). A receipt for the next frame ends the congestion.

While the bus is congested, the bridge still hands 1 frame to XL now and then as a probe. Without probes, receipts would never arrive again if one was lost or if the driver discarded the waiting frames (after bus-off, for example). The probe intervals:

| Probe | Interval |
|---|---|
| First | The given time after congestion is detected (at least 1 second if the controller reports error warning, error passive or bus-off) |
| From the second on | Doubles with each probe, up to 5 seconds (or the given time if that is longer) |

The interval grows because the probe frames, too, wait in the driver queue until the bus recovers, and should not pile up during a long period of congestion. After D seconds of congestion, roughly D / 5 probe frames plus a few have piled up.

This option relies on receipts, so it takes effect only with init access (`--bitrate`): with init access the port can enable receipts itself. Without init access, another application could disable receipts at any time and the congestion would never end, so the bridge does not use the option and prints a `warning:` at startup.

## Bus monitoring while running

### Error state queries

Every 0.5 seconds, `XlCanPort` asks for the error state of the CAN controller with `xlCanRequestChipState` in its receive thread, and reports the response as `ExternalEventKind::BusState`. The error state is one of error active, error warning, error passive and bus-off, plus the transmit and receive error counters. The interval is `XlCanPortOptions::bus_state_interval`; 0 turns the queries off (`--no-bus-monitor` in the executable). With the queries off, states that the driver sends on its own are still reported. On a VN5620 on a bus without acknowledgement, error states arrived without queries.

Failed queries are handled as follows, except when the transmit queue is full:

| How the query fails | Taken to mean | Action |
|---|---|---|
| It fails before any query was ever accepted | Queries are not available in this setup | Reports `ExternalEventKind::Warning` once, stops querying and keeps forwarding. |
| After one was accepted, it fails 3 times in a row (`XlCanPortOptions::bus_state_failures`) | The channel became unusable (the device was removed) | Reports `ExternalEventKind::Fault` once with the reason and stops receiving. |

In the first case, the executable prints `warning: ... the bus is not monitored, ...`. A single failure does not stop the bridge.

### Health lines

The bridge executable looks at the state every second and prints a line (`format_health_change()`) when either of these happens:

- The error state of the controller changed (except when the first response after startup is error active).
- A counter that indicates a problem (`error_frames`, `overruns`, `silkit_rejected`, `xl_rejected`, `xl_queue_full`, `xl_tx_failures`, `silkit_tx_failures`, `xl_tx_dropped`) increased.

```
[bridge] XL bus: error passive (tx errors 128, rx errors 0); since the last report: error_frames +3650
```

If no problem counter increases in the next check after one did, the bridge prints one line ending in `; no new problems since the last report`. A controller that only transmits does not lower its receive error counter until it receives successfully, so it may stay error passive after the bus recovers. This line is therefore the sign that the bus has recovered.

On a `Fault`, the bridge prints `FAIL: <reason>`, prints the statistics and exits with code 1.

### Bus-off recovery

With the channel left active, the XL driver of a VN5620 did not leave bus-off even after the bus recovered. So when the controller has reported bus-off for 1 second, `XlCanPort` deactivates and reactivates the channel (`xlDeactivateChannel`, `xlActivateChannel`) to bring the controller back. The time is `XlCanPortOptions::bus_off_restart` (`--bus-off-restart-ms` in the executable); 0 disables it.

- The channel is reactivated only with init access (`--bitrate`), because it is not known what happens when the bridge deactivates its port on a channel that another application configures. If you give the option without init access, the bridge prints a `warning:` at startup.
- Frames waiting in the XL transmit queue disappear with the channel, so they are reported as `ExternalEventKind::TransmitUnsent` and counted in `xl_tx_unsent`.
- The reactivation is reported as `ExternalEventKind::Warning`, and the executable prints `warning: XL channel '...' was bus off for <ms> ms; restarted the channel ...`. The line is printed only for the 1st, 2nd, 4th, 8th ... reactivation since the bus last worked (since a frame was received or a receipt for a transmitted frame arrived).
- If the bus is still broken, the controller goes bus-off again while frames are being sent, and the channel is reactivated again. Each time, the wait doubles, up to 10 seconds (or the given time if that is longer). Once the bus works, the wait returns to the original time.
- If the channel cannot be reactivated (deactivating or activating fails), the port reports `TransmitUnsent`, then `Fault`, and stops receiving.
- This relies on the controller state, so with `--no-bus-monitor` the channel is not reactivated unless the driver reports the state on its own.

`ExternalPortAdapter` does not use `BusState`, `Fault`, `Warning` or `TransmitUnsent`. It does not tell the user that the channel was reactivated either.

## Preventing forwarding loops

1. On the SIL Kit side, the receive handler is registered for the RX direction only. The bridge does not receive the frames it sends to SIL Kit.
2. On the XL side, with init access (when `--bitrate` is given), `xlCanSetChannelMode(tx=1, txrq=0)` enables transmit receipts and disables transmit request notifications (see "[Counting frames on the bus (transmit receipts)](#counting-frames-on-the-bus-transmit-receipts)"). This function needs init access (`vxlapi.h`). Without init access, the driver default (transmit receipts on) stays.
3. A receive event flagged `XL_CAN_MSG_FLAG_TX_COMPLETED` or `XL_CAN_MSG_FLAG_TX_REQUEST` is dropped as the bridge's own transmission and counted in `own_tx_dropped`. With receipts enabled, this equals the number of frames that went out on the bus.

Loops cannot be prevented if 2 bridges connect the same SIL Kit network to the same XL bus. This setup is not supported.

## Queues and threads

The bridge has no forwarding queue of its own.

| Direction | Calling thread | Function called |
|---|---|---|
| SIL Kit to XL | SIL Kit receive thread | `xlCanTransmit` |
| XL to SIL Kit | `XlCanPort` receive thread | `ICanController::SendFrame` |

- If the XL transmit queue is full, the frame is dropped and counted in `xl_queue_full`.
- `--rx-queue-size` sets the size of the XL receive queue (a power of two, 16–32768, default 16384). The driver actually holds 1 event fewer than that value.
- `xlReceive` is called for one event at a time, because Vector's samples also receive one at a time and receiving several events in one call is not verified.
- The channel is activated with `XL_ACTIVATE_NONE`, because the bridge does not use timestamps and `XL_ACTIVATE_RESET_CLOCK` cannot be combined with Vector's newer time synchronization protocol.
- XL calls on one port are serialized with a mutex.

### Startup order

Call `CanBridge::start()` before the participant lifecycle starts (`ParticipantRuntime::start()`). Other participants start sending as soon as they see the bridge reach Running. If the handlers and the controller are not running by then, the first frames are lost.

The launcher does not wait for a bridge with `required: false` in the configuration file to join the simulation. Frames that ECUs send before the bridge joins (an address claim right after startup, for example) do not reach the physical bus. In a test that talks to a node on the physical bus from the very start, start sending a little later, or let the launcher start the bridge with `--connect`: it waits until the bridge runs before it starts the ECUs (see "[Connecting a bus outside](simulation-run.md#connecting-a-bus-outside---connect)" in simulation-run.md).

### Shutdown order

| Order | What happens |
|---|---|
| 1 | The SIL Kit handler is removed (no more frames are handed to XL). |
| 2 | The bridge waits for XL transmissions and stops receiving (joins the receive thread and deactivates the channel). |
| 3 | The SIL Kit controller is stopped. |
| 4 | The lifecycle is stopped. |
| 5 | The port is closed. |
| 6 | The driver is closed. |

## Use from a protocol stack (`ExternalPortAdapter`)

`ExternalPortAdapter(external, clock)` starts the external port (joins the bus) and stops it on destruction or `close()`. Pass it the same clock as the `RealTimeDriver` and run it with that driver. It cannot be registered with a time-synchronized host (configuration error).

| What comes from the external port | Handling |
|---|---|
| Received frames | Put in the receive queue with their arrival time, in the receive thread of the external port. |
| Remote frames | Dropped and counted. |
| Receipts of its own transmissions | Dropped. |
| Error frames | Turned into `ErrorFrame` events. |
| Overflows of the external receive queue | Turned into `ReceiveOverflow` events with an unknown count. |
| Bus state (`ExternalEventKind::BusState`) and `Fault` | Not used; no `BusStateChanged` either. |

- Hardware timestamps come from a different clock, so they are not used as the arrival time.
- Receipts of its own transmissions are dropped because the stack itself delivers to the nodes of the same stack.
- Transmitting returns the synchronous result of `transmit()` (`Ok` becomes `Accepted`; `QueueFull` and `Dropped` become `QueueFull`). Whether the frame later went out on the bus is not reported.
- Not verified on real hardware (see "[Test coverage](#test-coverage)").

## Build

- The XL backend (`vehicle_sil::vector_xl`) and the executable are Windows only. They need the Vector XL Driver Library (`vxlapi.h`, `vxlapi64.lib`).
- vehicle-sil does not include the XL Driver Library. Give its location with `VEHICLE_SIL_VECTOR_XL_DIR`.
- If it is not found, the build continues without the bridge. With `VEHICLE_SIL_REQUIRE_VECTOR_XL=ON`, the configure step fails instead.
- The Linux build does not need the XL Driver Library.

The [XL Driver Library](https://www.vector.com/) (`vxlapi.h` and `vxlapi64.lib`) is software of Vector Informatik GmbH and is not included in vehicle-sil. Get it from Vector and use it under Vector's license terms. In the build, give its location:

```sh
cmake --preset windows-mingw-release -DVEHICLE_SIL_VECTOR_XL_DIR="C:/path/to/XL Driver Library/bin"
cmake --build --preset windows-mingw-release
```

If the XL Driver Library is found, the build copies `vxlapi64.dll` next to the executable.

## Class structure

A frame passes from the SIL Kit CAN network to the XL Driver through `ICanController`, `CanBridge` and `ExternalCanPort` (`XlCanPort` in production), in that order. `ParticipantRuntime` owns the participant. Behind the XL Driver is Virtual CAN or a device such as a CANcase.

| Class | Responsibility |
|---|---|
| `vehicle_sil::vector_xl::CanBridge` | Forwards classic CAN in both directions between SIL Kit's `ICanController` and an `ExternalCanPort`. Platform-independent |
| `vehicle_sil::vector_xl::ExternalCanPort` | Interface to external CAN. `XlCanPort` in production, a fake in tests |
| `vehicle_sil::vector_xl::XlDriver` | Manages `xlOpenDriver` / `xlCloseDriver` with RAII and lists the channels from `xlGetDriverConfig` |
| `vehicle_sil::vector_xl::XlCanPort` | Opens one XL channel (`xlOpenPort`) and handles transmitting (`xlCanTransmit`) and the receive thread (`xlReceive` and the notification event) |
| `vehicle_sil::vector_xl::ExternalPortAdapter` | Turns an `ExternalCanPort` into a port of the common CAN foundation (`RealTime`). Used to place a protocol stack directly on an XL channel. Platform-independent |

For the source locations and tests, see "Vector XL CAN Bridge" in [code-map-bridges.md](code-map-bridges.md#vector-xl-can-bridge).

## Test coverage

### Automated tests

The XL backend has 2 parts.

| Part | What it does | Built on | How it is verified |
|---|---|---|---|
| Logic (`xl_driver.cpp`) | Holds the decisions and the state, and depends only on an internal interface of vehicle-sil (`src/xl_backend.hpp`). | Every platform | Automated tests |
| XL Driver Library calls (`xl_backend_vector.cpp`) | Only converts types and values. | Windows, only when the XL Driver Library is present | Build and link against the official XL Driver Library |

- The automated tests run the production `xl_driver.cpp` and `main.cpp` against a test-only fake that implements the internal interface (`tests/vector_xl/fake_xl`).
- The fake simulates a bus per channel, transmit receipts, init access, receive queue overflows, error frames, error state queries of the controller and loss of the device.
- The fake also simulates these XL constraints: transmit receipt settings and the bit rate need init access, the transmit DLC is 0–8, the receive queue holds 1 fewer than the given size, and receiving returns one event per call.
- The conversions in `xl_backend_vector.cpp` (the mapping of functions, constants and struct members) are not covered by automated tests.
- The `no-answers` scenario of `tests/vector_xl/xl_bridge_it_launcher` sends to a channel with no node behind it, with `--stats-every-ms 100`: the statistics line comes while the bridge runs, only where it changed, with every key in order, and `xl_to_silkit=0`. The hint when nothing came is checked in unit tests only (`bridge_runtime.nothing_came_hint_*`), with the time given by the test.

### Verified (Windows 11, Vector Virtual CAN and VN5620)

On Windows 11 in MSYS2 UCRT64, the build with the XL Driver Library and the channel listing are verified. Channels 1 and 2 of Vector Virtual CAN are connected to the same virtual bus; on them, sending and receiving between separate SIL Kit networks, init access with `--bitrate` and the handling of transmit receipts are verified. Sending and receiving between separate SIL Kit networks is also verified with the 2 physical channels of a VN5620 connected in a loopback.

The VN5620 loopback also verified the following:

- J1939 messages longer than 8 bytes (BAM and RTS/CTS, RTS/CTS up to the maximum of 1785 bytes) pass through 2 bridges and the physical bus and are received in full, with no abort and no retransmission request. Because each CTS round trip passes through the bridges and the physical bus, one message takes longer than with SIL Kit alone.
- On physical channels, too, transmit receipts come back with or without init access, and `own_tx_dropped` equals the number of frames sent. When the bridge runs faster than the CAN bandwidth allows, the frames that could not be sent by the time of the stop are counted in `xl_tx_unsent`.
- With no node to acknowledge, `--tx-timeout-ms` drops new frames (`xl_tx_dropped`), and all frames already handed to XL go out on the bus once a node appears (`silkit_to_xl` and `own_tx_dropped` match).
- If another application (another bridge with init access) uses the same channel, the bridge receives the frames that application sends as normal received frames and forwards them to SIL Kit. Whichever side opens the channel later with `--bitrate` fails to start, because it has no init access.
- Bus monitoring (see "[Bus monitoring while running](#bus-monitoring-while-running)"): with no node to acknowledge, with a different bit rate, and with the cable unplugged, the bridge shows error passive and the increase in error frames every second, and shows "no new problems" once the bus recovers. A controller that only transmits does not lower its receive error counter until it receives successfully, so it may stay error passive after the bus recovers.
- If you unplug the USB cable while the bridge runs, `xlCanRequestChipState` keeps failing with `XL_ERR_INVALID_ACCESS` and the bridge exits with code 1. However, the VN5620 sometimes did not reappear as a USB device after it was plugged back in, and had to be power-cycled. Do not unplug the USB cable while the bridge runs.
- Error state queries are accepted on Vector Virtual CAN and on a physical channel without init access (no `warning:` is printed).
- Removing the termination and shorting CAN_H and CAN_L puts the controller into bus-off, and the XL driver stays bus-off after the fault is fixed. When the bridge reactivates the channel, forwarding resumes on the repaired bus (with init access). Removing the termination alone leads to error passive through receive errors, not to bus-off.
- On a receive queue overflow (caused by stopping the receiving process), received frames are lost and transmitting continues afterwards. No transmit receipt was lost in this test, but it is not verified that receipts are never lost on an overflow.
- Closing the console window prints the statistics and gives exit code 0, and Ctrl+C stops the bridge.
- The `windows-mingw-static` bridge and launcher run without MinGW in `PATH` (the only DLLs they import are Windows, UCRT and `vxlapi64.dll`).

### Not verified

The following are not yet verified with Vector's driver and hardware:

- Sending, receiving and transmit receipts with other physical CAN setups and devices
- Whether activating the channel alone, without reactivating it, brings it out of bus-off. Whether deactivating the channel discards the events left in the receive queue (receipts that arrive late)
- The behavior when a channel running in CAN FD mode is opened with a classic CAN port (`XL_INTERFACE_VERSION`)
- The round-trip end-to-end test in `tests/vector_xl/xl_bridge_it_launcher` kills the bridge on Windows, so it does not check the statistics and the exit code. The `stop-request` scenario checks the statistics line and exit code 0 after a stop request (SIGTERM on POSIX, Ctrl+Break to the process group on Windows). On Windows, Ctrl+Break can only be sent when the test executable and the bridge share a console, so this scenario is skipped there (the `device-lost` scenario is checked everywhere, because the bridge ends on its own).
- A protocol stack placed on an XL channel with `ExternalPortAdapter` (the automated tests run only against a fake `ExternalCanPort`)
- Starting the bridge from a configuration file of `vehicle_sil_launch`. The automated tests check `--describe` with the fake driver (`describe` in `tests/vector_xl/xl_bridge_it_launcher`); a run from a configuration file is checked with the Ethernet Bridge only (`ethernet_bridge.integration.pcap-launcher`).
