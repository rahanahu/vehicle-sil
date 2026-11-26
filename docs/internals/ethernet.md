# Ethernet foundation

[日本語](../ja/internals/ethernet.md)

This page specifies the Ethernet frame types, ports, channels, the virtual network and the SIL Kit port, with the guarantees and limitations the implementation keeps. It is also the specification to follow when you write a protocol or a test that works with Ethernet frames directly. The API is not stable yet.

| Section | Contents |
|---|---|
| [Library structure](#library-structure) | Dependencies, the function an ECU uses, frame contents are not interpreted |
| [Value types](#value-types) | `Frame`, `MacAddress`, `TimedFrame` |
| [The port contract (`Port`)](#the-port-contract-port) | What a port implementation must do, events (`PortEvent`) |
| [Channel (`Channel`)](#channel-channel) | One step, order at the same time, step and time of reception, limits, receive queue capacity, statistics |
| [Virtual network (`VirtualEthernetBus`)](#virtual-network-virtualethernetbus) | In-process Ethernet behavior |
| [SIL Kit port (`SilKitEthPort`)](#sil-kit-port-silkitethport) | Start and close, receiving, transmitting, senders outside time synchronization, shutdown order |
| [Use from an ECU (`Ecu::ethernet()`)](#use-from-an-ecu-ecuethernet) | What `Peer` and `LocalBench` create, `ecu.connected`, `--describe` output |
| [Unverified values](#unverified-values) | Values not verified against the standard, and how to change them |
| [Source compatibility and ABI](#source-compatibility-and-abi) | Additive source compatibility and binary layout / vtable changes |
| [Test coverage](#test-coverage) | What the tests check |

## Library structure

- `vehicle_sil::eth` does not depend on SIL Kit. It links only `vehicle_sil::step` and `sim_base`, and `eth.boundary` checks that it does not include SIL Kit headers.
- `vehicle_sil::eth_silkit` builds `vehicle_sil::eth` ports on top of SIL Kit Ethernet controllers (virtual time only).
- An ECU uses Ethernet through `Ecu::ethernet(network, options)` (`options` is a `ChannelOptions`). `Peer` and `LocalBench` create the channel and the port (see "Use from an ECU" below).
- vehicle-sil does not interpret frame contents. It reads only the destination and source addresses; it passes the EtherType, VLAN tags and payload through as bytes.
- There is no TCP/IP or other protocol stack.

The machinery that runs steps (host, driver, time domain) is described in [can.md](can.md). Ethernet uses the same machinery.

## Value types

| Type | Contents |
|---|---|
| `Frame` | The bytes from the destination address to the end of the payload (without the FCS) |
| `MacAddress` | A 6-byte address |
| `TimedFrame` | A frame with a time |

`Frame`:

- The bytes are stored inside the object (up to `Frame::kCapacity`; no memory allocation).
- A copy copies only as many bytes as the length.
- Comparison uses the length and the bytes.
- `destination()` is bytes 0–5 and `source()` is bytes 6–11.

`MacAddress`:

- `parse()` reads the form `02:00:00:00:00:01` (6 groups of 2 hex digits separated by `:` or `-`).
- `to_string()` writes lower case with `:`.
- Ordering (`<=>`) is lexicographic over the bytes.

Time of a `TimedFrame`:

| Direction | Time |
|---|---|
| Receive | The time of the sender's step (for a sender in time synchronization; for other senders, see "[Senders outside time synchronization](#senders-outside-time-synchronization)") |
| Transmit | The time of the step that transmits |

## The port contract (`Port`)

The rules are the same as for `vehicle_sil::can::Port`.

- `release_before(now, sink)` hands over the received frames whose time is before `now`, in time order, and in arrival order for the same time.
- A frame whose time is exactly `now` is handed over on the next call.
- Reordering frames with the same time is the channel's job.
- A port does not receive the frames it transmitted itself.
- `send()` returns a synchronous result (`Accepted`, `QueueFull`, `Failed`). A frame shorter than `kHeaderBytes` (14) is `Failed`. It accepts 1-arg `send(const TimedFrame&)` and 2-arg `send(const TimedFrame&, std::uint64_t token)` (default delegates to 1-arg `send()`); `token` is opaque and scoped per port (`0` is a normal valid token, never a sentinel). `Accepted` never triggers completion in the sending step `T`.
- `transmit_capabilities()` returns `TransmitCapabilities::None` (default) or `TransmitCapabilities::Correlated`. Backends reporting `None` never fabricate `TransmitCompleted` from `Accepted`.
- `release_events_before(now, sink)` hands over only events with `time < now`, in the same order. Events stamped `T` are released at a later step `T' > T`. The host calls it before `release_before()` in the same step.

### Port events (`PortEvent`)

The kinds of event (`PortEvent`):

| Kind | Meaning | Time |
|---|---|---|
| `TransmitCompleted` | The backend transmitted a frame given to the port | The time the backend confirmed it |
| `TransmitFailed` | The backend did not transmit a frame that `send()` accepted (the link is down, the controller is not running, or the frame was dropped) | The time the backend reported it |
| `ReceiveOverflow` | The receive queue was full and received frames were dropped. `count` is the number dropped | The earliest time among the dropped frames |
| `EventOverflow` | The event queue was full and transmit acknowledgement events were dropped. `count` is the number dropped | The earliest time among the dropped events |

`PortEvent` also carries `send_time`, `token`, and `correlated` (`bool`, default `false`) for correlated transmit acknowledgements (so `token == 0` is unambiguously distinguished from uncorrelated events). `ReceiveOverflow` and `EventOverflow` do not take up event queue capacity (`OverflowCollector`). They do not push out transmit acknowledgements, are handed over even when the queue is full, and `OverflowCollector::release_merged` emits each overflow immediately before the first queued event whose `event.time > overflow.time`.

## Channel (`Channel`)

`Channel` is the API through which an application sends and receives raw frames. A channel is a `step::Member`: added to a `step::Host`, it runs in the same step as the other members, in the order they were added. The member functions (`begin()`, `receive(now)`, `process()`, `flush()` and so on) are for the host and are private in `Channel`.

### What happens in one step

A step at time `now` proceeds in this order:

| Phase | What happens |
|---|---|
| 1. Receive | Drains port events before `now` (counting overflows and queuing correlated transmit events merged with `flush()` rejections from earlier steps), then sorts received frames that pass the filter and length check by time, source address and arrival order |
| 2. Process | Calls `on_transmit()` handlers first (after updating `transmit_completed` / `transmit_failed` and decrementing `outstanding`), then calls `on_frame()` handlers for each frame, in registration order on the driver's thread |
| 3. Application | `receive()` returns the same frames in the same order, one at a time |
| 4. Transmit | The frames from `send()` in this step are given to the port at time `now`, in the order of the calls |

In the receive phase, `Channel` still drains and counts transmit events even when no `on_transmit()` handler is registered.

Rules for the application phase:

- Frames not taken are not kept for the next step.
- In a channel on which `receive()` has been called at least once, the frames not taken by the end of the step are counted in `frames_not_taken` (a channel used only through handlers does not count them).
- Handlers and `receive()` do not affect each other.

Rules for the transmit phase:

- A frame the port does not accept is counted in `frames_dropped` and queues a `TransmitEvent::Kind::Failed` (`send_time = now`, `time = now`) for delivery in a later step's `process()` (if the port's queue is full, that frame and all the remaining ones; on a failure, that frame).
- There are no retries.

When you can call what:

- `send()` (1-arg or 2-arg with `token`) and `receive()` can be called only inside the step (in a handler or the application). Calling them from outside is a `std::logic_error`.
- A handler can send on another channel of the same host (a gateway). That frame is transmitted in the transmit phase of the same step.
- `on_frame()` and `on_transmit()` can be called only before the host starts (`std::invalid_argument` for an empty handler, `std::logic_error` after start).

### Transmit notifications, tokens, and `outstanding` conservation

- `transmit_capabilities()` forwards the port's `TransmitCapabilities`.
- `TransmitEvent` carries `kind` (`Completed` or `Failed`), `token`, `send_time` (originating step time `T`), and `time` (backend completion/failure time, or `T` on `VirtualEthernetBus`, SIL Kit trivial simulation, and `flush()` rejections).
- `Accepted` (`send() == Ok`) is strictly separated from transmit completion: `on_transmit` is never invoked in step `T`, and events are delivered in `process()` at `T' > T` via `release_events_before(now)`.
- `token` (`std::uint64_t`) is opaque and scoped per channel; `0` is a normal valid token, never a sentinel.
- Synchronous `SendStatus::QueueFull` / `InvalidFrame` from `send()` increment `sends_rejected`, emit no `TransmitEvent`, and leave `outstanding` unchanged; the same token can be retried immediately on a later step without double completion.
- Once `send() == Ok` on a `Correlated` backend, the token stays in `outstanding` until delivered as `Completed`/`Failed`, accounted for by `EventOverflow` (`port_event_overflows`) or `transmit_event_overflows`, or observed remaining in `outstanding` after `close()` / shutdown. Reusing a token while it is still in flight makes the two events indistinguishable. On a `Correlated` port, uncorrelated or duplicate port events (`!event.correlated`) do not increment `transmit_completed` or `transmit_failed`, preserving the `outstanding` conservation identity. On a `None` backend, `Accepted` port sends do not increment `outstanding` and never fabricate `Completed`.
- Within a step `T' > T`, `TransmitEvent` notifications (both correlated port events with `time < now_` and `flush()` rejections with `fail.time < now_`) are ordered monotonically by result timestamp `time` (for example, on timed backends `flush()` failures at `time == T` precede later completions). Same-timestamp `send()` order is not guaranteed; callers correlate outcomes by `token`.
- For a `Correlated` backend, the `outstanding` conservation identity holds at every point in time:

```text
accepted_by_channel == in_step_outgoing + frames_sent + frames_dropped
accepted_by_channel == transmit_completed + transmit_failed
                     + transmit_event_overflows + port_event_overflows
                     + outstanding
```

- Setting `port_transmit_queue_capacity > 0` while leaving `port_event_capacity` and `transmit_event_capacity` at default `0` (so they auto-size `>= max(1024, 2 * send_capacity, port_transmit_queue_capacity + send_capacity)`) prevents `EventOverflow` by construction (excess bursts receive deterministic `QueueFull` or next-step `Failed`). Because `EventOverflow` and `transmit_event_overflows` report only a drop count and not which `token` values were lost, a `TxConfirmation`-driven ECU or test harness cannot selectively free individual buffers and must treat `port_event_overflows > 0 || transmit_event_overflows > 0` as an unrecoverable confirmation-loss error (fail the step/run or reset the channel/controller TX state, matching `ReceiveOverflow`).

### Order of frames with the same time

In SIL Kit, the arrival order of frames that different senders transmitted at the same time depends on thread timing. The channel sorts frames with the same time by source address (bytes 6–11) and keeps arrival order among frames with the same address (a stable sort). So the same ECU sees frames in the same order in its handlers and `receive()`, whether it runs on the virtual network or on SIL Kit. If 2 senders share an address, the order between those 2 is not defined.

### Step and time of reception

- A frame transmitted in a step at time t is received in the receiver's first step that starts after t.
- Transmission time is not simulated. If both sides use 1 ms steps, a round trip takes 2 ms of virtual time.
- The time of a received frame is the time of the step that transmitted it (for a sender in time synchronization).

### Limits (`ChannelOptions`)

| Value | Default | Meaning |
|---|---|---|
| `send_capacity` | 256 | Frames `send()` accepts in one step. Beyond that, `SendStatus::QueueFull` |
| `receive_capacity` | 1024 | Frames handed over in one step (those that passed the filter). The excess is dropped and counted in `receive_overflows` |
| `max_frame_bytes` | 1522 | The maximum length of a frame that can be sent or is handed over (without the FCS). From `kHeaderBytes` to `Frame::kCapacity` (2048) |
| `accept` | `All` | The filter for frames handed over (`All` or `Addressed`) |
| `transmit_event_capacity` | 0 (derived) | Channel buffer capacity for pending `TransmitEvent`s. When 0: `max(1024, 2 * send_capacity, port_transmit_queue_capacity + send_capacity)` |
| `port_inbox_capacity` | 0 (derived) | Port receive inbox capacity (`Channel::port_inbox_capacity()` returns `2 * receive_capacity` when 0) |
| `port_event_capacity` | 0 (derived) | Port event queue capacity (`Channel::port_event_capacity()` returns `max(1024, 2 * send_capacity, effective_transmit_event_capacity, port_transmit_queue_capacity)` when 0) |
| `port_transmit_queue_capacity` | 0 (backend default) | Maximum unacknowledged frames allowed at the port (`> 0` bounds in-flight sends) |

`Channel::validate(options)` also throws `std::invalid_argument` if `port_event_capacity > 0 && port_transmit_queue_capacity > port_event_capacity`.

`receive_capacity`:

- Which frames are dropped on overflow depends on the order of reception, so on SIL Kit it is not defined. Treat an overflow as an error.
- Make the port's receive queue larger than this (see "Port receive queue capacity" below).

`max_frame_bytes`:

- A received frame longer than this is dropped and counted in `frames_invalid`.

`accept`:

| Value | Frames handed over |
|---|---|
| `All` | Every frame on the network (what a promiscuous capture sees) |
| `Addressed` | Only frames addressed to `mac`, broadcasts, and frames addressed to the addresses listed in `multicast` |

Frames removed by the filter are counted in `frames_filtered`.

### Memory allocation

Both receive queues and the transmit-notification buffers (`flush_failed_`, `transmit_events_`) are allocated once, at construction. Nothing is allocated inside a step. Sorting reorders an array of indices and does not move the frames, which stay in the order received. `eth.steps_do_not_allocate` checks this.

One frame takes `Frame::kCapacity` bytes, so the default capacities use this much memory:

| Object | Memory |
|---|---|
| One channel | About 2.5 MB |
| Receive queue of a virtual network endpoint (default 2048 frames) | About 8 MB (including the copy used for sorting) |
| Receive queue of the SIL Kit port (default 2048 frames) | About 8 MB (including the copy used for sorting) |

### Port receive queue capacity

The port's receive queue (which holds frames across steps) is twice the channel's `receive_capacity` (what is handed over in one step). `Channel::port_inbox_capacity(options)` returns this value.

#### Why it is doubled

When the step at time t takes frames out, the receive queue holds the frames to hand over in this step (from the time of the previous step up to just before t). It also holds frames at time t from senders that have already finished their step at time t; those are for the next step. A sender never gets ahead of the receiver's next step time (`StepDriver` runs steps in time order, and SIL Kit time synchronization does the same). So each group is at most `receive_capacity`, and together they fit in twice that. This holds even when the sender's step size differs from the receiver's.

#### Guarantee

If the frames received from the network in one step (before the filter) are at most `receive_capacity`, the port drops no frame, whatever the order in which ECUs run and whatever the timing of SIL Kit threads. The recording is the same on the virtual network and on SIL Kit.

#### Not guaranteed

With a setup that receives more than `receive_capacity` frames in one step, either the channel (`receive_overflows`) or the port (`port_receive_overflows`) drops and counts frames. Which one drops, and which frames, depends on the order of execution and on timing. Frames from senders outside time synchronization go into the step current at the time they are received, so this guarantee does not cover them.

#### Where the capacity is set

- `Ecu::ethernet()` creates the port with this capacity, both in `Peer` and in `LocalBench`.
- The default of `VirtualEthernetBusOptions::inbox_capacity` and `SilKitEthPortOptions::inbox_capacity` (2048) matches the default `receive_capacity`.
- If you add a channel to a port yourself and increase `receive_capacity`, set the port to the value of `Channel::port_inbox_capacity()` too (on the virtual network, `attach(inbox_capacity)` sets it per endpoint).

### Statistics (`ChannelStatistics`)

The counters only increase (except `outstanding`, which tracks current unacknowledged in-flight frames).

| Name | What it counts |
|---|---|
| `frames_received` | Frames handed over |
| `frames_taken` | Frames taken with `receive()` |
| `frames_not_taken` | Frames left untaken in a channel that uses `receive()` |
| `receive_overflows` | (`receive_capacity` in "Limits") |
| `frames_filtered` | (`accept` in "Limits") |
| `frames_invalid` | (`max_frame_bytes` in "Limits") |
| `frames_sent` | Frames the port accepted |
| `sends_rejected` | Frames `send()` rejected |
| `frames_dropped` | Outgoing frames that `send()` took but the port did not accept |
| `transmit_completed`, `transmit_failed`, `port_receive_overflows` | Port events |
| `port_event_overflows` | Transmit acknowledgements that the port dropped among its events (`transmit_completed` and `transmit_failed` are lower by that much) |
| `transmit_event_overflows` | `TransmitEvent`s dropped because the channel's `transmit_event_capacity` buffer was full |
| `outstanding` | Currently unacknowledged in-flight frames (inspectable after `close()` / shutdown) |

## Virtual network (`VirtualEthernetBus`)

`VirtualEthernetBus` is in-process Ethernet. Like SIL Kit Ethernet without the Network Simulator, it behaves as a hub. `Endpoint::transmit_capabilities()` returns `TransmitCapabilities::Correlated`. It is not thread-safe; use it from one thread (the `StepDriver` loop).

| Situation | Behavior |
|---|---|
| `attach()` | Creates one endpoint (`Port`) per call using `VirtualEthernetBusOptions` (`inbox_capacity` 2048, `event_capacity` 1024, `transmit_queue_capacity` 0) |
| `attach(inbox_capacity)` | Sets the receive queue capacity of that endpoint (0 is `std::invalid_argument`) |
| `attach(VirtualEthernetEndpointOptions)` | Overrides per-endpoint `inbox_capacity`, `event_capacity`, and `transmit_queue_capacity` (`0` inherits from `VirtualEthernetBusOptions`) |
| Transmission at time t | Every other endpoint receives the frame at time t, whatever the destination address (the sender does not receive it) |
| Transmit acknowledgement | Queues a correlated `PortEvent::TransmitCompleted` (`time = t`, `send_time = t`, `token`, `correlated = true`) for the sending endpoint (released at `T' > t`); if `transmit_queue_capacity > 0` and `outstanding() >= limit`, `send()` returns `QueueFull` |
| Event queue overflow | Dropped acknowledgements are counted (`event_overflows()`) and emitted as `EventOverflow` via `OverflowCollector` |
| A frame shorter than `min_frame_bytes` (default 60) | Padded with zeros before it is handed to the receivers (0 turns padding off) |
| A frame with a time before what the receiver has already handed over | Rejected with `std::logic_error` (the hosts are not run in time order) |
| Receive queue (`inbox_capacity`, default 2048) overflow | Dropped and counted; `ReceiveOverflow` is issued |

- Short frames are padded with zeros so that the recording is the same as on SIL Kit.
- `StepDriver` keeps time order.
- `Endpoint::outstanding()` returns the number of accepted frames whose terminal event has not yet been released (or accounted for by `EventOverflow`).
- For how to choose the receive queue capacity, see "Port receive queue capacity".

## SIL Kit port (`SilKitEthPort`)

`SilKitEthPort` is a port on the Ethernet controller of a participant in time synchronization (`TimeDomain::Virtual`). There is no port for real time (without time synchronization) yet.

### Start and close

| When | What it does |
|---|---|
| Construction | Registers handlers for reception (RX direction only), transmit acknowledgements and state changes |
| `start()` | Calls `Activate()` on the controller |
| `close()` | Removes the handlers, calls `Deactivate()` (if started) and discards what it holds |

- Frames received by the handlers registered at construction go into the receive queue.
- Call `start()` from the participant's CommunicationReady handler (`runtime.add_communication_ready_handler([&port] { port.start(); })`).
- If a step arrives before `start()` (`release_before()` or `send()`), the result is a `std::logic_error` that says what to call.
- After `close()`, SIL Kit does not call the port.

CommunicationReady is the point at which SIL Kit recommends calling `Activate()`: on a SIL Kit thread, before the first step. For a coordinated participant, the services of all participants are visible at that point, so the Network Simulator is informed as well. An `Activate()` before discovery changes only the local state and cannot be redone.

### Receiving

The receive handler only puts the frame into the receive queue (`StepInbox`) with SIL Kit's time (the time of the sender's step). A frame longer than `Frame::kCapacity` is counted and dropped.

"At step T, every frame before T has been received" holds only for frames that the sender transmitted inside its own step (the range SIL Kit guarantees). For frames that another SIL Kit program transmitted outside a step, the step in which they are handed over can differ from run to run.

### Transmitting and transmit acknowledgements

`SilKitEthPort::transmit_capabilities()` returns `TransmitCapabilities::Correlated`. Transmission allocates a slot from a pre-allocated FIFO free-list ring and passes a `0xA7`-marked tag in `SendFrame()`'s `userContext` (`[63:56] = 0xA7`, 24-bit generation, 32-bit slot index), guarded by a mutex released before calling `SendFrame()` (and rolled back safely if `SendFrame()` throws).

Without the Network Simulator, the transmit acknowledgement comes back inside `SendFrame()`, so the port resolves `(token, send_time)` and frees the correlation slot immediately inside `on_transmit()` before pushing the correlated `PortEvent` into the event inbox (preventing slot leaks even if the event inbox overflows), and hands it over in the next step (it does not call the channel on the spot).

| SIL Kit status | Event |
|---|---|
| `Transmitted` | `TransmitCompleted` (`correlated = true`) |
| `ControllerInactive`, `LinkDown`, `Dropped`, `InvalidFrameFormat` | `TransmitFailed` (`correlated = true`) |

- `SilKitEthPort::validate(options)` checks `inbox_capacity > 0`, `event_capacity > 0`, and `transmit_queue_capacity <= event_capacity`.
- When `SilKitEthPortOptions::transmit_queue_capacity > 0` and in-flight `outstanding` reaches that limit, `send()` returns `QueueFull` and increments `SilKitEthPortStatistics::send_queue_full`. `SilKitEthPortStatistics::outstanding` remains inspectable after `close()`.
- **External `INetworkSimulator` `userContext` contract**: SIL Kit transports `userContext` across processes as 64 bits and requires simulators to echo `ack.userContext = request.userContext`. Because the full 64-bit `send_time` lives in the slot table, `SilKitEthPort` recovers the exact `send_time` and `token` regardless of simulator delay. If an external simulator drops or alters `userContext` (or disconnects mid-run without acknowledging), `SilKitEthPort` never guesses FIFO order and never fabricates completions: uncorrelated acknowledgements increment `SilKitEthPortStatistics::tx_uncorrelated` (without generating `EventOverflow`), and unacknowledged frames remain inspectable in `outstanding` (`> 0`).
- The statistics count each kind.
- Set `event_capacity` (default 1024) to at least the number of frames transmitted in one step (the channel's `send_capacity`).
- Uncorrelated acknowledgements use a separate `event_capacity` queue merged in `time` order at `release_events_before()` (without triggering `EventOverflow`), so correlated transmit completions never compete with uncorrelated events (`transmit_queue_capacity > 0` with `transmit_queue_capacity <= event_capacity` prevents correlated `EventOverflow` by construction). Correlated acknowledgements that overflow (`transmit_queue_capacity == 0`) are dropped, counted and reported with `EventOverflow` (`OverflowCollector`).

The port pads a frame shorter than 60 bytes with zeros to 60 bytes before giving it to SIL Kit. SIL Kit on the receiving side pads short frames too, but the Network Simulator may reject short frames, so the sending side makes them uniform.

### Senders outside time synchronization

SIL Kit stamps frames from a participant outside time synchronization (such as a bridge to a real network) with the time `std::chrono::nanoseconds::min()`. SIL Kit on the receiving side replaces this with the time of the receiver's current step, but frames received before the receiver's first step keep this value.

The port does not hand this time on. It gives a frame with a negative time the time of this participant's latest step at the moment of reception (0 before the first step), and hands it over in the next step, in the same way as a frame transmitted at that step's time. The step that hands it over is decided by when it was received, so it is not deterministic. `received_without_time` counts these frames.

The CAN virtual-time port (`SilKitCanPort`) follows the same rule.

### Shutdown order

Shut down in this order:

1. Stop the ParticipantRuntime (no more steps arrive).
2. Call `close()` on the port or destroy it.
3. Destroy the channel.
4. Destroy the ParticipantRuntime (it holds the controller).

## Use from an ECU (`Ecu::ethernet()`)

`Ecu::ethernet(network, ChannelOptions)` returns the ECU's `Channel`. An ECU has one channel per Ethernet network. For usage and where it sits in a step, see "Ethernet channels" in [execution.md](execution.md#ethernet-channels). This section gives the implementation rules.

### Adding to the host

A channel is a `step::Member`, so it is added as is to the ECU's host (`can::Host`) with `step::Host::add(Member&)` (`can::Host` has `using step::Host::add;` and puts it in the same list as the CAN pairs). The receive and transmit phases run in the order in which `j1939()`, `can()` and `ethernet()` were called.

### What `Peer` creates

`Peer` creates a SIL Kit Ethernet controller named after the network, a `SilKitEthPort`, a `detail::GatedEthPort` and a `Channel`. Virtual time only: a real-time `Peer` gives `std::logic_error` (there is no real-time port).

| Item | Rule |
|---|---|
| Port receive queue | `Channel::port_inbox_capacity(options)` (twice `receive_capacity` when `port_inbox_capacity == 0`; see "Port receive queue capacity") |
| Port event queue | `Channel::port_event_capacity(options)` (at least `max(1024, 2 * send_capacity, effective_transmit_event_capacity, port_transmit_queue_capacity)` when `port_event_capacity == 0`) |
| Port transmit queue limit | `options.port_transmit_queue_capacity` (`0` = unlimited) |
| `start()` | In the CommunicationReady handler (`ParticipantRuntime::add_communication_ready_handler()`), after the CAN ports and before LIN `init()` |
| `close()` | After `Peer::stop()` has stopped the steps (`outstanding` remains inspectable) |
| Option check | Checked first with `Channel::validate()` and `SilKitEthPort::validate()` (a SIL Kit controller cannot be removed once created) |

### What `LocalBench` creates

`LocalBench` validates options first (before creating or naming any bus), adds to a `VirtualEthernetBus` per name (default options) an endpoint configured via `attach(VirtualEthernetEndpointOptions)` using `Channel::port_inbox_capacity(options)`, `Channel::port_event_capacity(options)`, and `options.port_transmit_queue_capacity`, and creates a `GatedEthPort` and a `Channel` on it. The capacities are per endpoint, so ECUs with different options can connect to the same network. An endpoint that a test adds with `ethernet_bus(network).attach()` has the default capacity.

### `ecu.connected`

`GatedEthPort` forwards `transmit_capabilities()` and 2-arg `send()`. While the ECU is disconnected, `GatedEthPort` turns transmissions into `SendResult::Failed` (the channel counts `frames_dropped` and emits `TransmitEvent::Kind::Failed` with `send_time == T, time == T` in the next step's `process()` instead of leaking tokens) and drops received frames whose time falls in the disconnected period. The point at which this is decided is the same as for CAN (`BusConnection`). Port events (transmit acknowledgements, overflows) pass through.

### `--describe` output

`ChannelOptions::mac` is output as `NetworkDescription::mac` (`networks[].mac` in `--describe`). The check rules `ethernet-duplicate-mac` and `ethernet-mac-unknown` ([simulation-check.md](../simulation-check.md)) use it. The channel neither checks nor writes the source address of the frames it transmits (the ECU's stack decides the address).

### Unverified behavior

The behavior when the SIL Kit Network Simulator simulates the Ethernet network (acknowledgement times, link state, and whether Vector's simulator echoes `userContext` untouched on transmit acknowledgements) has not been verified. If an external simulator omits or alters `userContext`, `SilKitEthPort` counts `tx_uncorrelated` and leaves `outstanding > 0`.

## Unverified values

The following values come from general sources and from knowledge of the SIL Kit sources. They have not been verified against the standard. Where a value can be changed, the user can change it.

| Value | Where it is used | How to change it |
|---|---|---|
| Default maximum length of 1522 bytes (without the FCS: the untagged 1514 bytes plus room for a tag) | `ChannelOptions::max_frame_bytes` | Option. The limit is `Frame::kCapacity` (2048; larger jumbo frames are not supported) |
| Zero padding to a minimum length of 60 bytes | SIL Kit reception (upstream implementation), `VirtualEthernetBusOptions::min_frame_bytes` | An option on the virtual network. Cannot be changed on the SIL Kit side |
| The first 14 bytes (destination 6, source 6, EtherType or length 2) and the byte order of addresses | `kHeaderBytes`, `Frame::destination()`, `source()`, the shortest frame that can be sent | Cannot be changed |
| The group bit of an address (lowest bit of the first byte) and broadcast (all 0xFF) | `MacAddress::is_multicast()`, `is_broadcast()`, `Accept::Addressed` | Not used with `Accept::All` |
| EtherType 0x88B5 in tests and examples (a value known for local experimental use), and the locally administered addresses `02:00:00:...` and `03:00:00:...` | Tests and `examples/ethernet` only (the vehicle-sil libraries do not use them) | In the example, `--ethertype <number>` (`ethernet_example::Options::ether_type`). The addresses are constants in the example's header |

## Source compatibility and ABI

- **Source-compatible additions**: The 1-argument `Port::send()` and `Channel::send()` overloads remain alongside the 2-argument token overloads, custom `Port` subclasses that override only 1-argument `send()` continue to compile with `TransmitCapabilities::None`, and new fields on `PortEvent`, `ChannelOptions`, `ChannelStatistics`, `VirtualEthernetBusOptions`, `SilKitEthPortOptions`, and `SilKitEthPortStatistics` are appended at the end with default member initializers so existing aggregate initializers remain valid.
- **ABI impact**: Adding virtual functions to `eth::Port` (`transmit_capabilities()`, 2-arg `send()`) changes vtable layout, and adding fields to `PortEvent`, `ChannelOptions`, `ChannelStatistics`, `VirtualEthernetBusOptions`, `SilKitEthPortOptions`, `SilKitEthPortStatistics`, `Channel`, and `SilKitEthPort` changes struct and class layouts. Recompile all consumers against the updated headers.

## Test coverage

### `eth.*`

In `tests/eth`; links only `vehicle_sil::eth`.

- Value types
- Virtual network: hub, next step, padding, own frames, time order, overflow, per-endpoint receive queue capacity
- Channel: runs on a host and a driver, gateway, order at the same time, `receive()` and the count of frames not taken, transmit phase, limits, port receive queue capacity, filter, events, option errors
- `eth.steps_do_not_allocate`: checks that nothing is allocated inside a step by counting every allocation of the process. It is a dedicated executable, and it counts with `tests/unit/allocation_counter.hpp`, shared with the J1939 test of the same kind.
- `eth.boundary`: checks that SIL Kit is not included.

### `simulation.ethernet_*` and related tests

| Test | What it checks |
|---|---|
| `simulation.ethernet_*` (`tests/simulation/test_ethernet.cpp`) | `Ecu::ethernet()` on both the virtual network and SIL Kit |
| `simulation.disconnect_*` | `ecu.connected` disconnects Ethernet too |
| `peer.*` | Rejection in real time, conflicting controller names, activation at CommunicationReady |
| `launch.an_ethernet_network_has_one_mac_address_per_participant` | The check rule |
| `ethernet_example.*`, `launch_run.ethernet` | Runs the example (`examples/ethernet`) |

`simulation.ethernet_*` checks:

- Reception in the next step
- Order at the same time
- Handlers and `receive()`
- `Accept::Addressed`
- The address in the `--describe` output
- Rejection messages
- `LocalBench::ethernet()` and `ethernet_bus()`
- Up to `receive_capacity` frames in one step are not dropped, even when the sender runs first or the step sizes differ, and the recording is the same on both
- The excess is counted

### `eth_silkit.*`

- Port tests against a fake controller (including frames without a time and overflowing transmit acknowledgements).
- With an in-process registry, the recording of the same scenario matches between the virtual network and SIL Kit. This is checked with several combinations of step sizes (including 3 senders transmitting at the same time, padding of short frames, replies from a handler, `receive()` and `Accept::Addressed`).
- The node addresses are in a different order from the registration order, so the virtual network side alone also checks that frames with the same time are sorted by address.

## See also

- The machinery that runs steps (host, driver, time domain): [can.md](can.md)
- How to use `Ecu::ethernet()`: [execution.md](execution.md)
- Overview: [architecture.md](../architecture.md)
- Where things are: [code-map.md](../code-map.md)
- The other internals pages: "[Internals](../architecture.md#internals)" in architecture.md
