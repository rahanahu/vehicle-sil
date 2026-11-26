# CAN foundation

[日本語](../ja/internals/can.md)

This page specifies the CAN port, host, drivers, virtual bus, SIL Kit port, bus timing and the simple CAN Network Simulator: how they behave, and the guarantees and limits the implementation keeps. It also gives the contract of the interfaces (`Port`, `Steppable`) that you implement to build your own protocol on CAN.

| Section | Contents |
|---|---|
| [Value types](#value-types) | `Frame`, `TimedFrame` |
| [The port contract (`Port`)](#the-port-contract-port) | Receive order, the result of `send()`, time domains |
| [Port events (`PortEvent`)](#port-events-portevent) | Transmit results, overflows, bus state notifications, and which backend emits which |
| [The `Steppable` contract](#the-steppable-contract) | The call order and rules to follow when you build a protocol stack |
| [Raw channel (`RawChannel`)](#raw-channel-rawchannel) | Application-level transmit notifications, token rules, `outstanding` conservation, capacity options |
| [The host (`Host`)](#the-host-host) | Phases of one step, failed state, `post()`, time domain |
| [Driving in virtual time](#driving-in-virtual-time) | `StepDriver`, `bind_host()` |
| [Driving in real time (`RealTimeDriver`)](#driving-in-real-time-realtimedriver) | When a step runs, arrival times, shutdown order |
| [Virtual bus (`VirtualCanBus`)](#virtual-bus-virtualcanbus) | With and without timing, fault injection, disconnection |
| [Observing raw frames (`TapPort`)](#observing-raw-frames-tapport) | What is reported and in what order, listener constraints |
| [SIL Kit port (`SilKitCanPort`)](#sil-kit-port-silkitcanport) | Start and close, times, transmit acknowledgements, options |
| [Bus timing (`CanBusModel`)](#bus-timing-canbusmodel) | Transmission time, arbitration, what is not simulated |
| [Simple CAN Network Simulator](#simple-can-network-simulator-vehicle_silcan_netsim-vehicle_sil_can_bus_simulator) | Timing and determinism, bit rate, controller modes, configuration, known limitations |
| [Source compatibility and ABI](#source-compatibility-and-abi) | Additive source compatibility and binary layout / vtable changes |

`vehicle_sil::can` and `vehicle_sil::can_silkit` are the foundation of the J1939 stack (`vehicle_sil::j1939`). They handle 11-bit and 29-bit CAN frames and can carry protocols other than J1939. The API is not stable yet.

`vehicle_sil::can` does not depend on SIL Kit or Vector XL. Every build verifies this by building a unit test executable that does not link SIL Kit.

The machinery that runs steps (host, drivers, clocks, time domains) is in `vehicle_sil::step`, which does not depend on the kind of bus. The `Host` of `vehicle_sil::can` is built on it. `StepDriver`, `RealTimeDriver`, `RealTimeClock`, `TimeDomain` and the like are aliases of the `vehicle_sil::step` types (both names give the same type).

All times are `SimTime`. Protocol processing has no threads of its own and does not read a clock. In virtual time, the time is that of SIL Kit or of `StepDriver`; in real time, it is the clock of `RealTimeDriver` (the time elapsed since start).

## Value types

- `Frame` is a classical CAN data frame (it does not represent RTR, CAN FD or CAN XL).
  - `length` is the number of bytes (0–8), not the DLC code.
  - Frames compare equal by ID, format (11-bit / 29-bit) and the data up to `length`.
- `TimedFrame` is a frame with a time. For a received frame, it is the time the backend gave it; for a frame to send, it is the time of the step that sends it.

## The port contract (`Port`)

### Receiving

- `release_before(now, sink)` passes the received frames whose time is before `now`, in time order, and in arrival order for equal times.
- The port does not reorder frames with the same time. The protocol processing decides their final order.
- A frame whose time is exactly `now` is passed on the next call.
- A port does not receive the frames it sent itself (the port drops echoes and transmit acknowledgements).

### Transmitting

`Port` exposes its transmit-completion capability via `transmit_capabilities()` (`TransmitCapabilities`):

| Value | Meaning |
|---|---|
| `None` (default) | The backend does not emit token-correlated per-frame transmit outcomes. |
| `Correlated` | Every `Accepted` send produces a token-correlated `TransmitCompleted` or `TransmitFailed` event (or is accounted for by `EventOverflow` or observed remaining in `outstanding` at shutdown). |

`send()` comes in two overloads (`send(const TimedFrame&)` and `send(const TimedFrame&, std::uint64_t token)`; the default 2-arg overload ignores `token` and delegates to 1-arg `send()`). `token` is an opaque caller identifier scoped per port; `0` is a normal valid token, never a sentinel.

`send()` returns a synchronous result.

| Result | Meaning |
|---|---|
| `Accepted` | The port accepted the frame. This is strictly separate from transmit completion and never triggers a completion event in the sending step. |
| `QueueFull` | The transmit queue is full. The frame can be sent again in a later step (with the same token). |
| `Failed` | The frame cannot be sent, or the backend reported an error. |

When the backend learns what happened to a frame after `send()` (it was sent, or the backend dropped it), the port reports it later as an event (`PortEvent`). `release_events_before(now, sink)` passes only events whose `time < now`, in time order and in arrival order for equal times. Even when a completion or failure carries `time == T` (such as on a timing-less virtual bus or SIL Kit's trivial simulation), it is never released in step `T`; it is released at a later step `T' > T`. The default `Port` implementation emits no events, and ports with `TransmitCapabilities::None` never fabricate `TransmitCompleted` from `Accepted`.

### Threads and time domains

Call `release_before()`, `release_events_before()` and `send()` only from the driver's thread.

Each port has a time domain.

| Domain | Time | Ports | `set_wakeup()` |
|---|---|---|---|
| `Virtual` | Virtual time | Virtual bus, SIL Kit with time synchronization | Ignored. |
| `RealTime` | The clock of the real-time driver | SIL Kit without time synchronization, Vector XL | On each arrival, calls the given function with the arrival time, which wakes the driver. |

A `RealTime` port stamps received frames and events with their arrival time, using the clock it shares with the driver (see "Driving in real time" below).

## Port events (`PortEvent`)

A port event reports, after the fact, what happened at the port: what became of a frame the port sent (known after the synchronous result of `send()`), and the state of the receive queue and of the controller.

| Kind | Meaning | Time |
|---|---|---|
| `TransmitCompleted` | The frame was sent. | For a backend that simulates the bus, the end of the transmission; otherwise, the time the backend confirmed the transmission |
| `TransmitFailed` | The backend did not send a frame that `send()` had accepted (transmit queue full, cancelled, disconnected from the bus). | The time the backend reported |
| `ReceiveOverflow` | The receive queue was full and received frames were dropped (`count` is the number dropped; it is 0 when any of the merged overflows has an unknown number, because the total is then unknown). | The earliest time among the dropped frames |
| `BusStateChanged` | The controller's error state changed to `bus_state` (`ErrorActive`, `ErrorPassive`, `BusOff`). | The time of the change |
| `ErrorFrame` | An error frame was seen on the bus. | The time it was seen |
| `EventOverflow` | The event queue was full and transmit acknowledgements were dropped (`count` is the number dropped). | The earliest time among the dropped events |

- A transmit event carries the CAN identifier (`id`), `send_time` (when known, the `TimedFrame::time` of the sending step), `token`, and `correlated`. It carries no data.
- `correlated` (`bool`, default `false`) explicitly distinguishes correlated events (so `token == 0` is never confused with an uncorrelated event, and duplicate copies such as from `FaultAction::Duplicate` do not fire a second correlated completion).
- `ReceiveOverflow` and `EventOverflow` are managed by `OverflowCollector`: neither takes a slot in the event queue, both are released even when the queue is full without pushing out transmit acknowledgements, and `OverflowCollector::release_merged` emits each overflow immediately before the first queued event whose `event.time > overflow.time`.
- `ReceiveOverflow` is not emitted for each dropped frame. The frames dropped until the port next releases events are counted in one event.
- A backend does not emit a kind it cannot know about; it reports nothing for it.
- More kinds may be added. A `Steppable` ignores kinds it does not know.

### Events by backend

| Backend | `transmit_capabilities()` | Events emitted |
|---|---|---|
| `VirtualCanBus` | `Correlated` | `TransmitCompleted` (both with timing at `tx.end` and without timing at `frame.time`), `TransmitFailed` (fault injection and outage), `ReceiveOverflow`, `EventOverflow`, any injected event |
| `SilKitCanPort` (virtual time) | `Correlated` | `TransmitCompleted` and `TransmitFailed` from SIL Kit transmit acknowledgements, `EventOverflow`, `BusStateChanged` from SIL Kit error state notifications (a network simulator sends them; SIL Kit's trivial simulation does not), `ReceiveOverflow` |
| `SilKitCanPort` (real time) | `None` | Uncorrelated `TransmitCompleted`/`TransmitFailed`, `BusStateChanged`, `ReceiveOverflow` |
| `ExternalPortAdapter` (Vector XL) | `None` | `ErrorFrame`, and `ReceiveOverflow` with an unknown count for overflows of the external port (no bus state and no transmit results) |

### Where events are delivered

In phase 2 of a step, `Host` passes the events to `Steppable::on_port_event()`, before the frames of that step. `on_port_event()` only records. It does not send (sending happens in `flush()`).

The J1939 stack only counts the events in `NetworkStatistics` (`frames_transmitted`, `transmit_failures`, `port_receive_overflows`, `port_overflow_events`, `bus_state_changes`, `error_frames`) and keeps the latest bus state in `Network::bus_state()`. It does not use them in protocol processing (its behavior does not change even at bus-off).

## The `Steppable` contract

`Steppable` is the extension point for building a protocol stack on CAN. The J1939 `Network` is built on this contract.

### Call order

Before the first step, the host calls `start(step_size)` once. When a pair is registered in `can::Host::add(Steppable&, Port&)`, the host also calls `bind_port(const Port&)` on the `Steppable` so components such as `RawChannel` can inspect `transmit_capabilities()` during setup before `start()`. In each step, it calls the following, in this order.

| Order | Call | What the `Steppable` does |
|---|---|---|
| 1 | `begin(now)` | — |
| 2 | `on_port_event()` for each port event | Records only. |
| 3 | `receive()` for each frame | Records only. |
| 4 | `process()` | Processes. |
| 5 | `flush(port)` (after the application runs) | Sends to the port. |

### Rules to follow

- `now` increases with each call.
- A `Steppable` has no threads and does not read a clock. The only times it uses are `now` and the times of frames and events.
- All calls come from one thread (the driver's thread), so no locks are needed.
- The frames passed to `receive()` have times before `now`.
- The order of same-time frames from different senders differs between backends, so the `Steppable` orders them by a fixed rule.
- `start()` throws an exception if the protocol cannot work with the step size (the host enters the failed state).
- In real time, the driver's `max_idle` is passed to `start()` as the step size.
- If a call throws, the host enters the failed state and every later step fails.

### `next_deadline()`

`next_deadline()` returns the earliest time at which a step is needed even if nothing is received (a timer deadline). The real-time driver sleeps until then.

It does not include work that has no timer (frames held back after `QueueFull`, frames carried over by the per-step limit). In real time, that work is sent in the step that runs after the driver's `max_idle`.

An implementation that keeps returning a past deadline after the step violates this contract (with a manual clock, the driver only runs the next step once, 1 ns later; with a wall clock, it keeps running steps).

## Raw channel (`RawChannel`)

`RawChannel` is a `Steppable` for applications that send and receive raw 11-bit and 29-bit CAN frames without a protocol stack. Setup (`on_frame()`, `on_transmit()`) is allowed only before `start()`; passing an empty handler throws `std::invalid_argument`, and calling after `start()` throws `std::logic_error`.

### Sending, tokens, and transmit notifications

- `send(frame)` and `send(frame, token)` can be called only inside a step (`begin()` through `flush()`); outside a step, they throw `std::logic_error`.
- `token` (`std::uint64_t`) is opaque and scoped per channel; `0` is a normal valid token, never a sentinel.
- Synchronous `RawSendStatus::QueueFull` (`send_capacity` reached in the step) and `RawSendStatus::InvalidFrame` are unaccepted: they increment `sends_rejected`, emit no `TransmitEvent`, and do not change `outstanding`. The caller may retry the same token immediately on a later step without double send or double completion.
- Once `send()` returns `Ok` on a `TransmitCapabilities::Correlated` backend (`transmit_capabilities()` queries the bound port), the token remains in flight (`outstanding`) until it reaches a terminal outcome: delivered as `TransmitEvent::Kind::Completed` or `Failed`, accounted for by `EventOverflow` (`port_event_overflows`) or `transmit_event_overflows`, or observed remaining in `outstanding` after `close()` / shutdown. Reusing a token before its terminal outcome makes the two in-flight events indistinguishable to the caller.
- On a `TransmitCapabilities::None` backend, `Accepted` port sends do not increment `outstanding` and do not synthesize `Completed` events.

### Timestamp semantics and intra-step ordering

`TransmitEvent` carries `kind` (`Completed` or `Failed`), `token`, `id`, `send_time` (the originating step time `T`), and `time`:

| Source | `send_time` | `time` |
|---|---|---|
| Timed bus or network simulator (`VirtualCanBus` with timing, `CanBusSimulator`) | `T` | Bus/simulator completion or failure time (`tx.end` or simulator timestamp) |
| Timing-less `VirtualCanBus` or SIL Kit trivial simulation | `T` | `T` |
| `flush()` rejection (`ecu.connected == false`, port `QueueFull`, or port `Failed`) | `T` | `T` |

- `Accepted` (`send() == Ok`) is strictly separated from transmit completion: `on_transmit` is never invoked in step `T` (neither inside `send()` nor inside `flush()`).
- Frames accepted by `send()` in step `T` that are rejected in `flush()` increment `frames_dropped` and append `TransmitEvent{Failed, token, id, T, T}` to pre-allocated `flush_failed_` storage (capacity `send_capacity`).
- Within a step `T' > T`, `TransmitEvent` notifications (both correlated port events released by `release_events_before(T')` and `flush()` rejections with `fail.time < now_`) are ordered monotonically by result timestamp `time` (for example, on timed buses `flush()` failures at `time == T` precede timed completions at `tx.end > T`). Same-timestamp `send()` order is not guaranteed; callers correlate outcomes by `token`.
- Within `process()` on the driver's thread, `RawChannel` updates `transmit_completed` / `transmit_failed` and decrements `outstanding` before invoking `on_transmit(event)` handlers (still draining and updating counters even when no `on_transmit` handler is registered). All `on_transmit` handlers run before `on_frame` handlers in the same `process()` call, following the same step and threading rules.
- All buffers (`received_`, `outgoing_`, `flush_failed_`, `transmit_events_`) are allocated at construction; steps perform zero heap allocations (`can.steps_do_not_allocate`).

### `outstanding` conservation and overflow accounting

For a channel on a `Correlated` backend, the following conservation identity holds at all times (mid-step after `send()`, between steps, after overflows, and after `close()` / shutdown):

```text
accepted_by_channel == in_step_outgoing + frames_sent + frames_dropped
accepted_by_channel == transmit_completed + transmit_failed
                     + transmit_event_overflows + port_event_overflows
                     + outstanding
```

- `EventOverflow(count)` increments `port_event_overflows += count` and decrements `outstanding` by `min(outstanding, count)`.
- Overflow of the channel's internal `transmit_events_` buffer increments `transmit_event_overflows` and decrements `outstanding` by `1` per dropped event.
- On a `TransmitCapabilities::None` backend, only `flush()` rejections increment `outstanding` until delivered as `Failed` in the next step's `process()`.
- **Prevention and recovery (`EventOverflow`)**: Setting `port_transmit_queue_capacity > 0` while leaving `port_event_capacity` and `transmit_event_capacity` at default `0` (so they auto-size `>= max(1024, 2 * send_capacity, port_transmit_queue_capacity + send_capacity)`) prevents `EventOverflow` by construction (excess bursts receive deterministic `QueueFull` or next-step `Failed`). Because `EventOverflow` and `transmit_event_overflows` report only a drop count and not which `token` values were lost, a `TxConfirmation`-driven ECU or test harness cannot selectively free individual buffers and must treat `port_event_overflows > 0 || transmit_event_overflows > 0` as an unrecoverable confirmation-loss error (fail the step/run or reset the channel/controller TX state, matching `ReceiveOverflow`).

### Capacity options (`RawChannelOptions`)

| Option | Default | Meaning |
|---|---|---|
| `send_capacity` | `64` | Maximum frames `send()` accepts in one step (`> 0`). |
| `receive_capacity` | `1024` | Maximum received frames kept in one step (`> 0`). |
| `transmit_event_capacity` | `0` (derived) | Channel buffer capacity for pending `TransmitEvent`s. When `0`: `max(1024, 2 * send_capacity, port_transmit_queue_capacity + send_capacity)`. |
| `port_inbox_capacity` | `0` (derived) | Port receive inbox capacity (`RawChannel::port_inbox_capacity()` returns `2 * receive_capacity` when `0`). |
| `port_event_capacity` | `0` (derived) | Port event queue capacity (`RawChannel::port_event_capacity()` returns `max(1024, 2 * send_capacity, effective_transmit_event_capacity, port_transmit_queue_capacity)` when `0`). |
| `port_transmit_queue_capacity` | `0` (backend default) | Maximum unacknowledged frames allowed at the port (`> 0` bounds in-flight sends). |
| `port_bitrate` | `0` (unset) | CAN bit rate in bit/s (`Peer` forwards this to `SilKitCanPortOptions::bitrate` for `SetBaudRate()` at `CommunicationReady`; unused on `LocalBench`). |

`RawChannel::validate(options)` throws `std::invalid_argument` if `send_capacity == 0`, `receive_capacity == 0`, or `port_event_capacity > 0 && port_transmit_queue_capacity > port_event_capacity`.

## The host (`Host`)

The host has 2 layers.

- `vehicle_sil::step::Host` holds the step members (`step::Member`) in registration order and advances them phase by phase. It does not know the kind of bus.
  - A `Member` is the pair of one channel's protocol processing and its port.
  - A `Member` has `begin(now)`, `receive(now)` (takes what is before `now` from the port), `process()`, `flush()`, `start(step_size)`, `next_deadline()`, `time_domain()` and `set_wakeup()`.
- `vehicle_sil::can::Host` derives from `step::Host`. It wraps each pair passed to `add(Steppable&, Port&)` in a `Member`.
  - Its `receive(now)` passes port events to `on_port_event()` and frames to `receive()`.
  - The host owns the containers of the pairs, but not the `Steppable` or the port.

The rest of this section describes the contract in terms of CAN pairs.

### Phases of one step

The host holds, in registration order, the pairs of protocol processing (`Steppable`) and port of one participant (or of one test loop). A step advances all pairs phase by phase.

| Phase | What happens |
|---|---|
| 1 | `begin(now)` |
| 2 | `on_port_event()` for the events the port releases, and `receive()` for its frames |
| 3 | `process()` |
| 4 | The application runs once |
| 5 | `flush(port)` |

The time of a step must be greater than that of the previous step.

### Failed state

If an exception leaves `step()` (from the protocol processing, the application or a port, and also for a usage error), the host enters the failed state and rethrows the exception to the caller. Every later `step()` throws `std::logic_error`. To recover, create the host again.

### Registration

Register only before the first step. The host does not own the `Steppable` or the port.

If you register after the wakeup function is set, the host calls the port's `set_wakeup()`. If that throws, the pair is not registered and the exception propagates (you can register the same pair again).

### Step size

Before `begin()` of the first step, the host passes the step size it received with `set_step_size()` to `start()` of each `Steppable`, once, in registration order (no value if none was set). `StepDriver::add()`, `bind_host()` and `RealTimeDriver` set it. If `start()` throws, the host enters the failed state (a configuration error: a step size the protocol cannot work with).

### `post(fn)`

`post(fn)` hands work to the protocol stack from outside a step (another thread, another SIL Kit handler).

- You can call it from any thread.
- `fn` runs on the driver's thread at the start of phase 4 of the next step (before the application), in the order posted.
- Work posted during a step runs in the next step.
- An exception from `fn` fails the host, as an exception from the application does. Work posted to a failed host does not run.

`post()` also works in virtual time, but the step in which the work runs depends on when `post()` was called. Data that you want to handle deterministically should be received through `StepInbox` and processed inside the step.

### Time domain of the host

All ports of one host must be in the same time domain. Each driver requires its own domain (`Virtual` for `StepDriver` and `bind_host()`, `RealTime` for `RealTimeDriver`). Mixing them is a `std::logic_error` at registration (for example, adding a Vector XL port to a host driven by SIL Kit time synchronization).

### `next_deadline()` of the host

The host's `next_deadline()` is the minimum of the `next_deadline()` of all its `Steppable`s.

## Driving in virtual time

- `StepDriver` (`vehicle_sil::step`) advances several hosts in virtual time without SIL Kit.
  - Each host can have its own step size. Steps run in order of (start time, registration order).
  - This order is one of the orders that SIL Kit time synchronization allows.
- Shutdown order in a test loop: stop using the `StepDriver` that advances the host → destroy the protocol processing and the host → destroy the ports (owned by the virtual bus).
- With SIL Kit, use `bind_host(runtime, host)`: the step handler of the `ParticipantRuntime` calls `host.step(now)`.
  - It cannot be bound to a `ParticipantRuntime` that does not use virtual time.
  - The implementation is `vehicle_sil::bind_host()` in `vehicle_sil::core` (it takes a `step::Host`). `vehicle_sil::can::bind_host` is the same function.

## Driving in real time (`RealTimeDriver`)

`RealTimeDriver` and the clocks belong to `vehicle_sil::step` and can drive any `step::Host`. They drive a host whose ports are SIL Kit ports without time synchronization or Vector XL ports, by the wall clock. There is no determinism. Wall-clock tests check only arrival and order.

### Threads

The driver has one thread per participant. The protocol stack has no locks; only this thread accesses it. The application takes part through the host's `post()` and through its own processing in each step.

If a step throws, the thread stops, and `failed()` and `error()` give the reason (the host is in the failed state too).

### Clock

The clock (`RealTimeClock`) is replaceable. The driver and the host's ports use the same clock.

| Used for | Clock |
|---|---|
| Product | `SteadyRealTimeClock` (time elapsed since its creation) |
| Tests | `ManualRealTimeClock` (advancing it with `advance_to()` wakes the driver) |

`wait_idle(timeout)` waits until the thread sleeps with nothing to do. Combined with `ManualRealTimeClock`, it lets you confirm after `advance_to()` that the steps up to that time have finished.

### When a step runs

Finish registration (`Host::add()`, `set_application()`) before `start()`. A step runs at these points:

- Right after start (the driver's thread runs the first step right after `start()`)
- On an arrival at a port, and on `post()`
- 1 ns after the host's `next_deadline()` (for timeouts that fire after their deadline)
- When `max_idle` (default 10 ms) has passed since the previous step

### Step time and arrival time

The time of a step is the clock's value, but at least 1 ns after the previous step and at least 1 ns after the arrival that woke the driver. A received frame is always passed in the next step, even if the clock has not advanced.

The arrival time (`RealTimeReceiver`) is set under a lock to the maximum of the clock's value, the `now` passed last, and the previous arrival time + 1 ns. It is never earlier than a step already processed, and the order of arrivals is preserved.

### Shutdown order

1. Stop the `RealTimeDriver` (`stop()` or destruction; it removes the wakeup functions from the host and the ports and joins the thread).
2. Close the ports.
3. Destroy the protocol stack and the host.
4. Destroy the `ParticipantRuntime` (or the driver).

If you call `stop()` from inside a step, it only tells the thread to end. A later `stop()` joins the thread.

## Virtual bus (`VirtualCanBus`)

`attach()` (and `attach(VirtualCanBusEndpointOptions)`, which overrides per-endpoint `inbox_capacity`, `event_capacity`, and `transmit_queue_capacity`; `0` inherits from `VirtualCanBusOptions`, and `attach()` throws `std::invalid_argument` if `transmit_queue_capacity > event_capacity`) creates a port that stands for one CAN controller on the bus. `Endpoint::transmit_capabilities()` returns `TransmitCapabilities::Correlated`, and `Endpoint::outstanding()` returns the number of accepted frames whose terminal port event has not yet been released (or accounted for by `EventOverflow`). Use a virtual bus from one thread.

### With and without timing

| Setting | How frames are handled |
|---|---|
| Without timing (default) | A frame sent at time t is delivered to every other port at time t (no arbitration and no transmission time), and a correlated `TransmitCompleted` event (`time = t`, `send_time = t`, `token`, `correlated = true`) is queued for the sending port (released in a later step `T' > t`). |
| With timing (`VirtualCanBusOptions::timing`) | Frames go on the bus by the rules in "Bus timing" below. The other ports receive the frame at the end of its transmission, and the sending port receives a correlated `TransmitCompleted` with the same time. |

Two distinct transmit queue capacities govern `QueueFull`:

- **Bus-model FIFO capacity (`VirtualCanBusTiming::transmit_queue_capacity`, default `64`)**: Analogous to `CanBusSimulatorOptions::transmit_queue_capacity`. With timing enabled, `send()` returns `QueueFull` when the bus model's per-sender FIFO holds frames that were sent but whose transmission had not started when the bus was resolved (`Host` takes the step's received frames before it sends, so this is the state at the step's time).
- **Per-endpoint unacknowledged-frame bound (`VirtualCanBusEndpointOptions::transmit_queue_capacity`, default `0` = unlimited)**: When `> 0`, `Endpoint::send()` returns `QueueFull` whenever `outstanding_ >= limit`, across both timed and timing-less modes, outages, and `FaultAction::TransmitFailed`.

If the per-endpoint correlated event queue overflows, dropped correlated transmit acknowledgements are counted and emitted as `PortEvent::Kind::EventOverflow` via `OverflowCollector`. Uncorrelated events (`FaultAction::Duplicate` copy 1 and `inject_event()`) use a separate `event_capacity` queue merged in `time` order at `release_events_before()`, so correlated transmit completions never compete with uncorrelated events (`transmit_queue_capacity > 0` with `transmit_queue_capacity <= event_capacity` prevents correlated `EventOverflow` by construction).

A bus with timing is resolved lazily. When any port takes frames or events before `now`, the bus resolves every transmission that starts before `now`.

### When a frame can be sent

A frame older than the time already resolved cannot be sent (`std::logic_error`). The resolved time is, without timing, the time the receivers have already taken, and with timing, the time up to which the bus is resolved. This error means that the hosts were not advanced in time order. `StepDriver` always advances them in time order.

### Receive queue overflow

The receive queue of each port has a fixed capacity. Frames that do not fit are dropped and counted (`overflows()`). Overflows are also reported with `ReceiveOverflow`. On the virtual bus, overflows are deterministic too, but frames are lost, so tests check that there is no overflow.

### Fault injection (for tests)

With `Endpoint::set_fault_injector(f)`, `f` returns a `FaultAction` for each frame the port sends. An invalid frame gets `Failed` before `f` is called. `f` is called once per frame, in send order, so a deterministic function gives deterministic results.

| `FaultAction` | Result of `send()` and delivery | With timing |
|---|---|---|
| `Deliver` | As usual. | — |
| `Drop` | Returns `Accepted` and does not deliver the frame to the other ports on the bus. | The frame occupies the bus, then is delivered nowhere (the sending port receives `TransmitCompleted`). |
| `Duplicate` | Delivers the frame twice with the same time (and emits 1 correlated `TransmitCompleted` without timing). | Sends the frame twice (copy 0 sets `correlated = true` with the caller's `token`; copy 1 sets `correlated = false, token = 0`, while both keep `send_time`). |
| `Delay` | Delivers the frame at its time + the delay. | The frame contends for the bus from its send time + the delay. |
| `QueueFull`, `Failed` | Returns that result and does not deliver the frame. | — |
| `FaultAction::transmit_failed()` | Returns `Accepted`, delivers the frame nowhere and gives the sender a `TransmitFailed` at the send time (an asynchronous transmit failure). | Does not use the bus. |

- Even with `Drop`, the local loopback inside the sender's `Network` delivers the frame, because the result is `Accepted`.
- Frames sent later overtake a `Delay` frame, so `Delay` also produces reordering.
- The delay of `Delay` must be greater than 0.

`Endpoint::inject_event(event)` queues any event (bus state, error frame and so on) on that endpoint.

### Per-ECU disconnection

`Endpoint::add_outage(from, until, direction)` disconnects the endpoint from the bus during [from, until). The frame times decide what is affected, so the result is deterministic.

| `direction` | What happens | Counted in |
|---|---|---|
| `Receive` | Frames whose receive time falls in the interval are not received. | `outage_dropped()` |
| `Send` | Frames whose send time falls in the interval are delivered nowhere, and the sender gets `TransmitFailed`. | `outage_lost()` |
| `Both` | Both. | Both |

With `Send`, the fault injection function is not called, and even with timing the frame does not use the bus. Whether a send falls in the outage is decided by its send time, so with timing, a frame sent before the outage that is waiting for the bus still goes out during the outage.

## Observing raw frames (`TapPort`)

`TapPort(inner, listener)` wraps a port and passes everything to `inner` (forwarding `transmit_capabilities()` and 2-arg `send(frame, token)`). It carries both 11-bit and 29-bit frames unchanged.

### What is reported, and in what order

The `TapPort` reports the following 3 kinds to `TapListener::on_tap()`, in the order they pass through it.

| Kind | Covers | Comes with |
|---|---|---|
| `Received` | Frames the protocol stack takes | The frame's time |
| `Sent` | Frames sent | The time of the sending step, the return value of `send()`, and `token` |
| `Event` | Port events | The event's time |

`Received` follows the port's release order (time order, arrival order for equal times). With the SIL Kit port, the order of same-time frames from different senders can change from run to run. To compare runs, sort by (time, CAN ID), or use the `Observer` events, which come after the reordering.

### Listener constraints

- The listener is read-only and injects nothing (send test stimuli from a virtual bus endpoint).
- With a listener that does not throw, having a listener or not changes neither what the stack receives nor the results of what it sends.
- An exception from the listener propagates to the caller, and the host fails.
- Received frames and events are reported before they reach the stack, so a frame on which the listener threw does not reach the stack.
- Calling `send()`, `release_before()`, `release_events_before()` or `set_listener()` of the same `TapPort` from inside the listener is a `std::logic_error`.
- The time domain and the wakeup are those of the wrapped port.
- Use the `TapPort` from the driver's thread, like the wrapped port.
- There is one listener. `set_listener()` with another listener while one is set is a `std::logic_error` that says so, and the first one keeps listening: read the frames from that one (one `testkit::FrameLog` per `TapPort`), or remove it with `set_listener(nullptr)` first. Setting the same listener again changes nothing.
- The listener and the `TapPort` each remember the registration, so either can be destroyed first.

## SIL Kit port (`SilKitCanPort`)

### Start (`start()`)

On construction, the port attaches a receive handler (RX direction only) and a transmit acknowledgement handler to the CAN controller. Frames received from then on go into the receive queue.

`start()` starts the controller. Call it from the participant's CommunicationReady handler (`runtime.add_communication_ready_handler([&port] { port.start(); })`; `Peer` calls it for its own ports). This is the point at which SIL Kit recommends calling `SetBaudRate()` and `Start()`: on a SIL Kit thread, before the first step, and, for coordinated participants, with the services of every participant visible. A network simulator therefore receives the bit rate and the start regardless of start-up order. Autonomous participants have no such guarantee (a simulator they have not discovered yet does not receive them, and they are not sent again).

| Condition | What happens |
|---|---|
| A step comes before `start()` (`release_before()` or `send()`) | `std::logic_error` (the message "... before start(): the controller is not started ..." says what to call). |
| `start()` after `close()` | Nothing. |
| A second `start()` | Nothing. |

A step before `start()` is an error because SIL Kit's trivial simulation drops frames sent on a controller that has not started, without a transmit acknowledgement.

### Close (`close()`)

`close()` removes the handlers, stops the controller (if it was started), clears the correlation slot table once callbacks can no longer run, and empties the receive queue. Once `close()` returns, SIL Kit no longer calls the port. `outstanding()` remains inspectable after `close()` so any unacknowledged in-flight frames at shutdown can be detected.

The shutdown order:

1. Stop the `ParticipantRuntime` (in real time, stop the `RealTimeDriver`).
2. `close()` the port (or destroy it).
3. Destroy the protocol processing and the host.
4. Destroy the `ParticipantRuntime` (last, because it owns the controller).

### Receiving through SIL Kit

The receive callback only puts the frame in the receive queue with the SIL Kit timestamp (the sender's step time). Frames other than classical CAN data frames are dropped and counted.

"At step T, every frame with a time before T has been received" holds only for frames that the sender sent within its own step (the range SIL Kit guarantees; vehicle-sil ports send within steps). For frames that another SIL Kit program sends outside its steps (from another thread or a receive handler), the step in which they are passed can change from run to run.

### Frames from senders outside time synchronization (virtual time)

For frames from a sender that does not take part in time synchronization (such as the Vector XL CAN Bridge), SIL Kit sets the time of the current step of this receiving participant. Frames received before this participant's first step, however, keep `std::chrono::nanoseconds::min()`.

The port does not pass this time on. It gives frames and events with a negative time the time of this participant's latest step at the moment of reception (0 before the first step) and passes them in the next step. The statistic `received_without_time` counts them. The step in which they are passed is decided at the moment of reception, so it is not deterministic.

### Transmitting through SIL Kit

The port sends with `SendFrame()`. SIL Kit stamps the frame with the sender's step time. SIL Kit's trivial simulation has no synchronous result, so a valid frame gets `Accepted`.

In virtual time (`transmit_capabilities() == TransmitCapabilities::Correlated`), the port allocates a slot from a pre-allocated FIFO free-list table (up to `kMaxCorrelatedInFlight = 16,384` concurrent in-flight correlated requests) and encodes both the send time and a 16-bit `(2-bit generation, 14-bit slot index)` tag into `SendFrame()`'s `userContext` (`0xA6` marker in bits `[63:56]`, 16-bit FIFO tag in bits `[55:40]`, and 40-bit nanosecond residue of `TimedFrame::time` in bits `[39:0]`). Legacy `encode_send_time()` (`0xA5` marker, 56-bit ns) remains supported alongside `0xA6`. SIL Kit returns `userContext` only to the sender with the transmit acknowledgement and does not pass it to other receivers. Only 64-bit platforms are supported.

- The correlation table is guarded by a mutex released before calling `SendFrame()`, so synchronous trivial-simulation callbacks inside `SendFrame()` never re-enter the lock.
- If `SendFrame()` throws, the slot is safely reclaimed if still unused.
- Inside the transmit acknowledgement callback, the port resolves the exact 64-bit `SimTime` `send_time` and caller `token` and frees the slot immediately before pushing to the event inbox; therefore, event-inbox overflow never leaks correlation slots.
- **External `INetworkSimulator` `userContext` contract**: SIL Kit transports `userContext` across processes as 64 bits and requires network simulators to echo `ack.userContext = request.userContext`. Because the full 64-bit `send_time` lives in the slot table, `SilKitCanPort` recovers the exact `send_time` and `token` even if a simulator delays a frame past `2^40 ns` (~18.3 minutes). If an external simulator drops or alters `userContext` (or disconnects mid-run without acknowledging), `SilKitCanPort` never guesses FIFO or CAN-ID order and never fabricates completions: uncorrelated acknowledgements in virtual time increment `SilKitCanPortStatistics::tx_uncorrelated` (without generating `EventOverflow` or touching correlated `RawChannel` counters), and unacknowledged frames remain inspectable in `outstanding` (`> 0`).

### Transmit acknowledgements and events

The port counts transmit acknowledgements and puts them in the event queue as `PortEvent`s.

| SIL Kit notification | `PortEvent` |
|---|---|
| Transmit acknowledgement `Transmitted` | `TransmitCompleted` (`correlated = true` in virtual time) |
| Transmit acknowledgement `Canceled`, `TransmitQueueFull` | `TransmitFailed` (`correlated = true` in virtual time) |
| Error state notification | `BusStateChanged` (none for `NotAvailable`) |
| Receive queue overflow | `ReceiveOverflow` |
| Dropped transmit acknowledgement (event inbox full) | `EventOverflow` (`OverflowCollector`, merged in `release_events_before()`) |

The SIL Kit handlers do not call the protocol processing (in the trivial simulation, the transmit acknowledgement comes back inside `SendFrame()`, so calling it would re-enter). The time of a transmit acknowledgement is the time of the sending step in the trivial simulation, and the end of the transmission with a network simulator.

In virtual time, uncorrelated port events (`BusStateChanged`, `ErrorFrame`, and uncorrelated simulator acknowledgements) use a separate `event_capacity` queue merged in `time` order at `release_events_before()`, so correlated transmit completions never compete with uncorrelated events (`transmit_queue_capacity > 0` with `transmit_queue_capacity <= event_capacity` prevents correlated `EventOverflow` by construction). If a burst of correlated transmit acknowledgements overflows `event_capacity` (`transmit_queue_capacity == 0`), the dropped acknowledgements are counted in `event_overflows` and reported as `PortEvent::Kind::EventOverflow` via `OverflowCollector` (merged alongside `ReceiveOverflow` in `release_events_before()`).

### Bit rate (`SilKitCanPortOptions::bitrate`)

If you set `bitrate`, `start()` passes it with `SetBaudRate()` before `Start()`. The trivial simulation does not use it. A network simulator uses it for transmission times.

With the default 0, the port does not call `SetBaudRate()`. The SIL Kit documentation says to always set it before use. This causes no problem with the trivial simulation or the simple CAN Network Simulator. How other network simulators handle a controller without a bit rate is not verified. If you use such a simulator, set the bit rate.

### Transmit queue limit (`SilKitCanPortOptions::transmit_queue_capacity`)

`SilKitCanPort::validate(options)` checks that `inbox_capacity > 0`, `event_capacity > 0`, `transmit_queue_capacity <= event_capacity`, and in virtual time (`real_time_clock == nullptr`) `transmit_queue_capacity <= kMaxCorrelatedInFlight` (`16,384`).

If you set `transmit_queue_capacity` (default 0 = no limit), `send()` returns `QueueFull` when the number of frames that were sent and whose transmit acknowledgement event has not been released yet reaches that number. The protocol processing keeps the frame and sends it again in the next step.

- A network simulator drops requests beyond its own transmit queue without sending them (a `TransmitQueueFull` transmit acknowledgement). With a simulator, set the limit to that queue size or less.
- The limit cannot be larger than `event_capacity` (nor `kMaxCorrelatedInFlight` in virtual time).
- A frame that never gets a transmit acknowledgement (for example, one sent on a controller that has not started) does not give its slot back. Check the statistic `outstanding`.
- In SIL Kit's trivial simulation, transmit acknowledgements are released in the next step, so the limit becomes the number of frames you can send in one step.

### Real time (`SilKitCanPortOptions::real_time_clock`)

If you set `real_time_clock`, the port belongs to a participant without time synchronization, and its time domain is `RealTime`. `transmit_capabilities()` returns `TransmitCapabilities::None`, and the port never fabricates `TransmitCompleted` from `Accepted`. Drive it with `RealTimeDriver`.

- Instead of the SIL Kit timestamp (unusable with unsynchronized senders), the port stamps the arrival time from the clock, and it wakes the driver on each arrival.
- The port does not put the send time or correlation tag in `userContext` (network simulators assume virtual time).

### Consistency with the virtual bus

When you run the same scenario on the virtual bus and on SIL Kit (virtual time), the receive and transmit traces match byte for byte. Tests verify this, including combinations of participants with different step sizes.

This relies on one condition. With SIL Kit, the order in which the port passes same-time frames from different senders is not fixed (only the order per sender is preserved). The protocol processing must therefore reorder them by a fixed rule. The test protocol reorders them by (time, sender).

## Bus timing (`CanBusModel`)

The virtual bus with timing and the simple CAN Network Simulator use the same bus model. It does not depend on SIL Kit.

### Transmission time

The transmission time is the number of bits without stuff bits (47 + 8 × bytes for an 11-bit ID, 67 + 8 × bytes for a 29-bit ID, including the 3-bit intermission) divided by the bit rate, rounded up to whole ns. Real frames are longer by their stuff bits (up to about 20 %).

### Arbitration and the transmit queue

When the bus becomes idle, the heads of the ready transmit queues contend, and the one with the smallest arbitration field wins. The comparison order:

1. Compare the 11-bit base IDs.
2. If they are equal, an 11-bit frame wins over a 29-bit frame (RTR is dominant, SRR is recessive).
3. Between 29-bit frames, compare the extended IDs.

Between frames with the same ID and format (which a real bus does not resolve), the shorter data wins, then the data bytes in order decide, then the order of the senders (on the virtual bus, the order of `attach()`).

A frame being transmitted is never interrupted. Frames that become ready during a transmission contend at its end.

Each sender's transmit queue is a FIFO. A frame further back waits until the frames ahead of it have gone out, even if it would win arbitration.

### What is not simulated

Stuff bits, error frames, ACK errors and automatic retransmission, error states, remote frames, CAN FD, and the priority of the controller's transmit buffers.

### When the bus is resolved (`VirtualCanBusTiming::simulator_step`)

| `simulator_step` | How the bus is resolved | Receive time |
|---|---|---|
| 0 (exact) | A frame contends for the bus from its send time. | Decided by the send times only, independent of the receivers' step sizes. |
| Δ > 0 | The bus is resolved every Δ. | Depends on Δ, but not on the receivers' step sizes. |

With Δ > 0, at boundary T the bus adds the frames sent before T to the queues and resolves the transmissions that start in [T, T + Δ). A frame sent at time t does not start before the first boundary after t. This is the same rule as in the simple CAN Network Simulator (`SimulationStepSizeNanoseconds` = Δ), and both give the same receive times for the same transmissions (verified by tests).

### Setup for verifying timing

To verify timing, put the nodes on separate ports (separate CAN controllers). Nodes of the same `Network` reach each other through local loopback and do not go through the bus.

## Simple CAN Network Simulator (`vehicle_sil::can_netsim`, `vehicle_sil_can_bus_simulator`)

SIL Kit's trivial simulation reproduces neither arbitration nor bus occupancy. The simple CAN Network Simulator is a SIL Kit participant that uses the SIL Kit Network Simulator API (experimental) to simulate CAN on the specified networks with the bus model above. It reproduces arbitration, transmission time and congestion in virtual time, without a CANoe license.

### Transmission and reception

- SIL Kit automatically switches the controllers on those networks to detailed simulation, so the code of each participant does not change.
- The time of a received frame is the end of its transmission.
- The sender gets back the TX frame and the transmit acknowledgement (`Transmitted`) with the same time, both carrying the sender's userContext.
- A request that overflows a controller's transmit queue (default 64) gets a `TransmitQueueFull` transmit acknowledgement and is not sent.
- Requests for CAN FD and remote frames are dropped and counted.
- If the request queue (4096 requests per network) overflows, requests are dropped and counted, and a warning is printed.
- A run in which the request queue overflowed is not deterministic.
- A network that is in the configuration but that no controller has joined yet does nothing (SIL Kit hands over the means to deliver frames when the first controller appears).
- So a configuration shared with Vector's simulator can contain CAN networks that are not used.

### Timing and determinism

The simulator takes part in time synchronization. At step T, it takes in the requests sent before T (decoding both `0xA6` 40-bit wraparound timestamps via non-negative modular unwrapping near the last step `last` and legacy `0xA5` 56-bit timestamps via `decode_send_time(user_context, reference)`, echoing `request.userContext` bit-for-bit on `Transmitted`, `Canceled`, and `TransmitQueueFull`) and resolves the transmissions that start in [T, T + step size). `0xA6` timestamps are accepted only within `[last - limit, last + limit]` (`limit = max(1 s, step size)`), preventing backward-wrapped residues at `last >= 2^39 ns` (~550 s) from sorting into earlier steps (out-of-window requests are treated as untagged at `last`). The result depends only on the send times and the step size, not on the step sizes of the other participants or on thread timing.

The following cases are not deterministic.

| Case | Handling |
|---|---|
| Requests without a marker (from participants other than vehicle-sil), and requests whose marked time is beyond the limit (the larger of 1 s and the step size after the last step) | Treated as sent at the time of the last step, and counted. |
| A run in which a controller with frames waiting to be sent calls `Reset()` | Which frames get `Transmitted` and which get `Canceled` is undecided for one step (see `Reset()` under "Controller modes" below). |

The limit for a marked send time is the larger of 1 s and the step size, counted from the last step.

### Bit rate in the simulator

The bit rate of a network is the value from the first controller on that network that calls `SetBaudRate()`, or, if none does, `--default-bitrate` (default 500 kbit/s). A different value that arrives later is counted, causes a warning and is not used. The log shows the bit rate in use and whether it is the default or a controller's setting.

| Calls `SetBaudRate()` | Does the simulator receive the value? |
|---|---|
| vehicle-sil ports of coordinated participants (the ECUs of `vehicle_sil_launch run`, `Simulation`) | Yes, regardless of start-up order (the port calls `SetBaudRate()` at CommunicationReady). |
| Autonomous participants, and other programs that call `SetBaudRate()` right after creating the controller | Not if they call it before they discover the simulator (SIL Kit behavior; see [dependencies.md](../dependencies.md)). |

Also give the network's bit rate with `--default-bitrate`, to cover ports that set no bit rate and participants whose value the simulator cannot receive.

### Controller modes

The conditions under which sends are dropped, and the fact that reception does not depend on the mode, are the same as in SIL Kit's trivial simulation. Cancellation by `Reset()` (`Canceled` transmit acknowledgements) of frames waiting to be sent is specific to this simulator.

| Situation | What happens |
|---|---|
| A mode change (`Start()`, `Stop()`, `Sleep()`, `Reset()`) | The simulator returns the state change in the next step. |
| Requests that a controller sends after it reported a mode other than `Started` | Dropped and counted (`rejected`). No transmit acknowledgement is returned. |
| Requests sent before the mode change | Sent as usual (decided by the order of the controller's calls, not by thread timing). |
| A controller that never reports a mode | Treated as started. |
| Reception | A frame is delivered to every connected controller except the sender, regardless of the receiver's mode. |
| A request to reset error handling | Nothing happens (error states are not simulated). |

A controller that never reports a mode is treated as started because the simulator cannot receive a `Start()` that comes before the controller discovers the simulator (autonomous participants; see [dependencies.md](../dependencies.md)). The SIL Kit CAN controller has no mode equivalent to listen-only.

#### Warning for dropped frames

The simulator warns only once per controller (it does not warn again, even if the controller is restarted and stopped again). The warning gives the frame ID, the controller number and the mode the controller reported (stopped, asleep, reset). The number is the one SIL Kit gave that controller. It counts from 1 across all networks, in the order the simulator discovered the controllers (the Network Simulator API does not give the names of participants or controllers).

#### `Reset()`

`Reset()` cancels pending send requests. The controller's frames whose transmission has not started are dropped without going on the bus and counted (`rejected`), and each gets a `Canceled` transmit acknowledgement. The cancellation takes effect from the first step that starts after the simulator receives `Reset()` (frames whose transmission was resolved in an earlier step are delivered). The simulator's step at the same time as `Reset()` can run before or after the simulator receives `Reset()`, so a frame that would have started in that step can get either `Transmitted` or `Canceled`. After `Reset()`, the controller cannot send until it calls `Start()`.

### Configuration file and command line

The configuration file and the command line are shared with the Vector SIL Kit Network Simulator.

```yaml
SchemaVersion: 1
Description: Any description
SimulationStepSizeNanoseconds: 1000000   # 1 ms if omitted
SimulatedNetworks:
  - Name: CAN1
    Type: CAN
  - Name: LIN1          # not CAN: not simulated, with a warning
    Type: LIN
```

#### Configuration file

The format is the configuration file of the Vector SIL Kit Network Simulator (`SchemaVersion: 1`, YAML or JSON). Only networks with `Type: CAN` are simulated. Other kinds of networks, `Switches` and unknown keys are accepted with a warning. The simulator does not use a file in which a required item is missing, `SchemaVersion` is not 1, a kind is none of CAN, LIN, Ethernet and FlexRay, or a name is duplicated.

#### Command line

The form is the same as that of Vector's simulator.

```
vehicle_sil_can_bus_simulator [--name <participant>] [--connect-uri <uri>] [--log <level>] [--participant-configuration <file>] <configuration>
```

- The short forms `-n -u -l -c` work the same. The default participant name is `NetworkSimulator`.
- On Windows, the arguments are read as UTF-8 from the wide-character command line (paths and names with non-ASCII characters work).
- Relative paths inside the `-c` file are resolved from the current directory at run time.
- For keys that the `-c` configuration does not set, the vehicle-sil defaults are added (the listen address, TCP on Windows, `Experimental.TimeSynchronization.DynamicSimulationStep: false`).
- If `ParticipantName` differs from `--name`, `Middleware.RegistryUri` differs from `--connect-uri`, or `DynamicSimulationStep` is `true`, the simulator fails with a setup failure (exit code 3) before it connects, and says how to fix it (see "SIL Kit configuration defaults" in [core.md](core.md#sil-kit-configuration-defaults)).
- Settings specific to this executable (`--default-bitrate`, `--transmit-queue`, `--start-timeout-ms`) go on the command line, not in the configuration file, so that Vector's simulator can read the same file.

#### Exit codes

| Exit code | Meaning |
|---|---|
| 0 | Ended normally |
| 1 | Failure during the simulation |
| 3 | Setup failure (configuration file, connection) |
| 4 | Usage error |

### Swapping in the Vector SIL Kit Network Simulator

The simulator is designed so that swapping the executable is all it takes. Both simulators take over the simulation of the named networks under the same participant name (default `NetworkSimulator`), so the code of each peer, the configuration file and the system controller's list of required participants are expected not to change. Operation with Vector's simulator is not verified yet. Vector's simulator requires a CANoe license.

- Vector's simulator simulates transmission times with stuff bits, ACK errors and error states for sends that no node receives, CAN FD, LIN, Ethernet and FlexRay. The simple simulator does none of these, so the times do not match even with the same configuration.
- vehicle-sil ports put the send time and correlation tag in `userContext` with either simulator; the value means nothing to Vector's simulator, and whether Vector's simulator echoes `userContext` untouched on transmit acknowledgements is not verified (if omitted or altered, `SilKitCanPort` counts `tx_uncorrelated` and leaves `outstanding > 0` rather than guessing).
- Determinism and timing with Vector's simulator are not verified.

### Known limitations of the simple CAN Network Simulator

| Limitation | Impact and workaround |
|---|---|
| A transmission never starts before the first step boundary after its send time. | A lower-priority frame that started in between can go out first (the shift is at most the step size + 1 frame). |
| The virtual bus with timing (`send()` returns `QueueFull` synchronously; `RawChannel` notifies `Failed` at send step `T`) and the simple simulator (an asynchronous `TransmitQueueFull` transmit acknowledgement; `RawChannel` notifies `Failed` at simulator step `T_sim`) handle a full transmit queue differently. | When the queue overflows, the traces of the two do not match. To avoid losing frames and align failure timing (`Failed` at `T`), set `RawChannelOptions::port_transmit_queue_capacity` (or the port's `transmit_queue_capacity`) to `--transmit-queue` or less (see "SIL Kit port" above). |
| When frames with the same ID and data come from different controllers at the same time, their order is the order in which each controller made its first request (the order the simulator received them), which is not deterministic. | The receivers receive the same content. |
| Protocol timers that start at a send (such as J1939 T3) count from the time of the step that passed the frame to the port. | They start counting earlier than the actual transmission, by the time the frame waits for the bus (see "Transport protocol limitations" in [j1939.md](j1939.md#transport-protocol-limitations)). |
| The Network Simulator API is an experimental SIL Kit API. | Only `libs/can_netsim/src/can_bus_simulator.cpp` uses it. When you update SIL Kit, run the tests of this simulator. |

## Source compatibility and ABI

- **Source-compatible additions**: The 1-argument `Port::send(frame)` and `RawChannel::send(frame)` overloads remain alongside the 2-argument token overloads, custom `Port` subclasses that override only 1-argument `send()` continue to compile with `TransmitCapabilities::None`, `0xA5` send-time encoding remains accepted alongside `0xA6`, and new fields on `PortEvent`, `TapRecord`, `*Options`, and `*Statistics` are appended at the end with default member initializers so existing aggregate initializers remain valid.
- **ABI impact**: Adding virtual functions to `can::Port` (`transmit_capabilities()`, 2-arg `send()`) and `can::Steppable` (`bind_port()`) changes vtable layouts, and adding fields to `PortEvent`, `TapRecord`, `RawChannelOptions`, `RawChannelStatistics`, `SilKitCanPortStatistics`, `VirtualCanBus::Endpoint`, `RawChannel`, and `SilKitCanPort` changes struct and class layouts. Recompile all consumers against the updated headers.

## See also

- How the J1939 stack uses this foundation: [j1939.md](j1939.md)
- How to set up participants that use the simple CAN Network Simulator: [peers.md](../peers.md)
- The overall picture: [architecture.md](../architecture.md)
- Where things are: [code-map.md](../code-map.md)
- The other internals pages: "[Internals](../architecture.md#internals)" in architecture.md
