# Execution and test foundation

[日本語](../ja/internals/execution.md)

This page specifies the following classes and the guarantees their implementations keep: `Ecu`, which builds an ECU application; `Peer`, the participant that runs J1939 and LIN nodes; `LocalBench` and the test tools, which run on in-process virtual buses; and `Simulation`, which chooses between them and runs them.

| Section | Contents |
|---|---|
| [The `Ecu` class](#the-ecu-class-vehicle_silecu) | Setup functions, order within one step, ownership and destruction, variables, disconnecting from the bus, `describe()` output |
| [`Simulation`](#simulation-vehicle_silsimulation) | Choosing where to run, `run_for()` and `run()`, failure reporting |
| [Peer](#peer-vehicle_silpeer) | Participant setup, time modes, lifecycle, stopping and destruction, threads |
| [Local execution and the test kit](#local-execution-vehicle_sillocalbench-and-the-test-kit-vehicle_siltestkit) | `LocalBench` buses and ECUs, recording and fault injection |
| [Test coverage](#test-coverage) | What the tests verify |

`Peer` and `LocalBench` both use only the public API of the engine (`vehicle_sil::j1939`). The CTest tests `local.boundary`, `testkit.boundary`, `peer.boundary`, `ecu.boundary` and `simulation.boundary` verify that they use no internal headers and no test-only APIs. In the classification (the table in "Directories and targets" in [architecture.md](../architecture.md#directories-and-targets)), `Ecu` and `Simulation` are for users, `Peer` and `LocalBench` are foundation, and the test kit is for tests.

## The `Ecu` class (`vehicle_sil::Ecu`)

`Ecu` is the class that builds an ECU application (`vehicle_sil::ecu`, without SIL Kit). `Peer` (a SIL Kit participant) and `LocalBench::add_ecu()` (a host on the virtual bus) implement it, so a function or class that takes an `Ecu&` runs on either with the same code.

```cpp
class Engine {
public:
    explicit Engine(vehicle_sil::Ecu& ecu) {
        auto& node = ecu.j1939("J1939_CAN").add_node({.address = 0x00, .name = kName});
        codec::attach_sender<EngineStatus>(node, 20ms, [this](EngineStatus& m) { m.speed.set(rpm_); });
        ecu.every(10ms, [this](vehicle_sil::SimTime) { tick(); });
    }
};
```

### Setup functions

An ECU application builds itself with these functions of `Ecu`, usually in its constructor:

| Function | Creates |
|---|---|
| `j1939(network, NetworkOptions)` | A J1939 `Network` |
| `can(network, RawChannelOptions)` | A raw CAN channel |
| `lin_master(network, schedule, LinMasterOptions)`, `lin_slave(network, LinSlaveOptions)` | A LIN node |
| `ethernet(network, eth::ChannelOptions)` | A channel for raw Ethernet frames |
| `on_step(fn)`, `every(period, fn)`, `at(time, fn)` | Functions called inside a step |
| `emplace<T>(args...)` | An object that the ECU owns |

Call each of these functions only before the run starts. The rules for network names, and what happens when a rule is broken:

| Condition | Result |
|---|---|
| A setup function is called after the run has started | `std::logic_error` |
| `emplace` is called after the run has started | `std::logic_error`, without constructing `T` |
| The same network name is passed to `j1939()` twice on the same ECU (likewise for `can()`) | The second call throws `std::invalid_argument` |
| A LIN network shares its name with a CAN network | `std::invalid_argument` |
| An ECU has 2 LIN nodes on one LIN network | `std::invalid_argument` |
| `j1939("")` or `can("")` (an empty network name) is called | `std::invalid_argument` in both implementations |
| The constructor of `T` throws | The exception propagates, and `T` does not remain in the ECU |

`j1939()` and `can()` can use the same CAN network name. An Ethernet network name cannot be shared with CAN or LIN either, and an ECU has only one channel on an Ethernet network. Even when the constructor of `T` throws, what the constructor added to the ECU (networks, nodes, handlers, functions) remains.

### Order within one step

| Order | What happens |
|---|---|
| 1 | What `j1939()`, `can()` and `ethernet()` created receives and processes, in the order it was added |
| 2 | Variable writes whose time has come are applied (`Agent::on_step_begin()`) |
| 3 | The `on_step`, `every` and `at` functions are called, in the order they were added |
| 4 | `step(now)` of each LIN node is called, in the order the nodes were added |
| 5 | Variables are read (`Agent::on_step_end()`; every `sample_period`) |
| 6 | What `j1939()`, `can()` and `ethernet()` created transmits, in the same order as in 1 |

- A frame sent from a handler of one network to the other network goes out in the same step (a gateway is an ECU with 2 networks).
- LIN requests that the application makes in a step (`set_frame_data()`, `set_response()`, `request_sleep()` and so on) take effect from that step.
- What a LIN node receives is reported during that step's LIN processing, so the application reads it in the next step.
- The descriptions of `Peer`, `LocalBench` and `Ecu` follow the same order.

### Ownership and destruction

The ECU owns the LIN nodes, the networks (protocol parts first), the raw channels, the Ethernet channels, the Control Agent and the objects created with `emplace`. The references it returns are valid for the ECU's lifetime.

The destruction order is: the LIN nodes, the networks, the raw channels, the Ethernet channels, the Agent, then the objects created with `emplace` (the last added first). Handlers, protocol parts, and the pointers and functions of variables can refer to the `emplace` objects during destruction.

### Declaring variables

Declare the variables to read and write from outside the simulation with these functions. Their arguments, types and checks are those of the functions of the same name in `control::Agent`.

- `input(name, &value, options)`, `input(name, get, set, options)`, `input_enum(name, &value, items, options)`
- `output(name, &value, options)`, `output(name, get, options)`, `output_enum(name, get, items, options)`

The ECU's Agent (`agent()`) holds the declarations. An ECU has one Agent, created at this point:

| Case | When the Agent is created |
|---|---|
| A variable is declared or `agent()` is called | At the first declaration or `agent()` call |
| Neither | At the start of the run (`Peer::start()`, the first run of `LocalBench`; for `ecu_main`, right after setup) |

Every ECU has an Agent. When the Agent is created, it gets the input `ecu.connected`, which every ECU has, as its first variable (see "[Disconnecting from the bus](#disconnecting-from-the-bus-ecuconnected)" below). After the start, `agent()` returns the existing Agent.

| Condition | Result |
|---|---|
| `set_agent_options(AgentOptions)` is called after the Agent was created | `std::logic_error` |
| Invalid options (options that `Agent::check_options()` rejects) are passed to `set_agent_options()` | `std::invalid_argument` with the ECU's name (the options do not change) |
| A declaration or `set_agent_options()` is called after the run has started or after the schema is fixed | `std::logic_error` |
| The same name is declared twice on the same ECU | `std::invalid_argument` (the message names the ECU) |
| A name that starts with `peer.` or `ecu.` is declared | `std::invalid_argument` at declaration, in both implementations |
| A name is declared that clashes with a per-network monitoring output of the `Peer` (a name that starts with `j1939.N.`) | `std::invalid_argument`, not at declaration but when the schema is fixed |

Names that start with `peer.` are reserved for the monitoring outputs of the `Peer`, and names that start with `ecu.` for the variables that every ECU has.

The implementation holds the Agent in `detail::EcuAgent`. `detail::EcuAgent` is responsible for lazy creation, the options, the calls before and after the step, the final `finish()` and the check for transports that remain.

### Transport lifetime

A transport that uses the Agent from outside the ECU (`control_silkit::AgentTransport`) holds a `control::TransportLease` on the Agent for as long as it lives. The Agent counts them in `Agent::transports()`. Other transports can use the same type.

- Declare the transport after the ECU (`Peer`, `Simulation`, `LocalBench`), and destroy it first.
- Objects created with `emplace()` are destroyed after the Agent, so do not `emplace()` a transport into the ECU.
- If a `TransportLease` remains when the ECU destroys the Agent, the ECU prints its name and the fix to stderr and calls `std::terminate()`.

The ECU calls `std::terminate()` so that nothing can use the destroyed Agent. Because this happens in a destructor, it does not throw an exception.

### When variable writes are applied

A write is applied in the first step at or after `at`, before the application's processing. The network receive processing and the timers of protocol parts in that step run before the write is applied, so they use the new value from the next step on. Variables are read after the application and LIN processing. These points are the same in `Peer` and `LocalBench`.

When a `Peer` stops (`stop()` and destruction), it sets the pending writes to `Cancelled` once steps no longer run (`Agent::finish()`; a second call does nothing). A transport stopped after that also sends those results. For an ECU in `LocalBench`, no results are returned for pending writes, because the transport is destroyed before the ECU. Users do not call `finish()`.

### `every()` and `at()`

| Function | Step in which it is called | Where the rule is |
|---|---|---|
| `every(period, fn)` | The first step at or after each of the times 0, period, 2 period, … (at most once per step) | `vehicle_sil::periodic()` (`core/periodic.hpp`) |
| `at(time, fn)` | The first step whose start time is at or after `time`, once only | `vehicle_sil::once_at()` (same header) |

Each rule lives in one place, shared by the ECUs of `Peer` and `LocalBench`.

- The period of `every` need not be a multiple of the step size. Unlike `TickScheduler`, `every` does not check how the period relates to the step size.
- `at` calls the function even if the step size does not divide `time`. If `time` is before the first step, it calls the function in the first step.
- The functions of `at` are in the same list as those of `on_step` and `every`, in the order they were added.
- Passing an empty function to `at` throws `std::invalid_argument`.
- Use `at` for the ECU's scripted one-time actions (start or stop a subscription, send with `Node::send()`).
- `Node::send()` and the sending functions of protocol parts can be used only inside a step. Call them through `at()`, not directly from `main`.

### Raw CAN (`can::RawChannel`)

`RawChannel` is the foundation for protocols other than J1939. Receive with `on_frame(handler)` and send with `send(frame)`.

| Item | Specification |
|---|---|
| Where `send()` can be called | Only inside a step (elsewhere, `std::logic_error`) |
| Result of `send()` | `RawSendStatus` (`Ok`, `QueueFull`, `InvalidFrame`) |
| When frames are sent | At the end of that step, in the order of the `send()` calls, with the time of the step |
| Limits (`RawChannelOptions`) | Per step: 64 sent, 1024 received |
| Frames beyond the limits | Counted and dropped; not retried |
| Frames the channel sent itself | Not received |

If one ECU has both `j1939()` and `can()` on the same network name, they act as separate controllers and receive each other's frames. They receive them in the next step, but with the time of the step that sent them. A J1939 network ignores 11-bit frames. However, when a `Peer`'s J1939 network runs on a user port (`j1939(name, port)`, such as Vector XL), the two are on separate buses, because the raw channel is always on SIL Kit.

The order of frames received at the same time from different senders depends on the threads of the other participants on SIL Kit, and on the order of the `StepDriver` on the virtual bus. `RawChannel` does not reorder them.

### Public functions and virtual functions

`j1939()`, `can()`, `lin_master()`, `lin_slave()` and `ethernet()` are non-virtual functions that check for empty names. The work is done in private virtual functions (`add_j1939`, `add_can`, `add_lin_master`, `add_lin_slave`, `add_ethernet`).

`Peer` has a separate `j1939(name, ...)` that takes `J1939ChannelOptions`. This declaration hides the base class's functions of the same name, so the only `j1939` you can call through a `Peer&` is the `Peer`'s own. Call `j1939(network, NetworkOptions)`, which takes `NetworkOptions`, through an `Ecu&`.

### LIN nodes

`lin_master(network, schedule)` creates and returns a `lin::LinMaster` that runs at the ECU's step size. `lin_slave(network)` creates and returns a `lin::LinSlave`. Set the handlers (`on_result()`, `on_state()`, `on_frame()`) before the run. Nodes are initialized before the first step (`init()`).

| Implementation | Where the node is created | Limits |
|---|---|---|
| `Peer` | A SIL Kit LIN controller named after the LIN network, and a `SilKitLinPort` | Virtual time only (`std::logic_error` in real time) |
| `LocalBench` | An endpoint of the `VirtualLinBus` of that name | One master per bus. A second master throws `std::logic_error` with the names of both ECUs in the message. |

Errors in the schedule and options are `std::invalid_argument` from the engine's checks. `vehicle_sil::lin` does not include SIL Kit, so `vehicle_sil::ecu` and `vehicle_sil::local` stay free of SIL Kit.

### Ethernet channels

`ethernet(network, options)` creates and returns an `eth::Channel` that sends and receives raw frames. It works in units of frames. vehicle-sil has no TCP/IP stack and does not interpret the bytes after the source address. For channels, limits and unverified values, see [ethernet.md](ethernet.md).

| Item | Specification |
|---|---|
| Receiving (`on_frame(handler)`) | Called at the start of the step, in order of reception (set it before the run) |
| Receiving (`receive()`) | Takes that step's frames one at a time, from `on_step()` and so on |
| Sending (`send()`) | Can be called only inside a step. The frame is sent at the end of that step. |
| Channels that receive a frame | Every other channel on the same network (regardless of the destination address, as with a hub) |
| Step of reception | A frame sent in the step at time t is received in the receiving ECU's "first step that starts after t" |
| Order of frames with the same time | By source address; the order is the same in `Peer` and `LocalBench` |
| `options.mac` | The address (`mac`) in the `describe()` output. Also used for filtering with `Accept::Addressed`. Not written into frames. |

| Implementation | Where the channel is created |
|---|---|
| `Peer` | A SIL Kit Ethernet controller named after the network, and an `eth::SilKitEthPort` |
| `LocalBench` | An endpoint of the `eth::VirtualEthernetBus` of that name |

A `Peer`'s channel works in virtual time only. In real time, it throws `std::logic_error` with what to do in the message. The controller is activated at CommunicationReady. The transmit acknowledgement queue is made at least `send_capacity` long.

Both implementations make the port's receive queue twice `receive_capacity`. If at most `receive_capacity` frames arrive from the network in one step, no frame is dropped, whatever the order in which the ECUs run and the timing of the threads.

Option errors (`eth::Channel::validate()`) are rejected with `std::invalid_argument` before any controller or network is created, so you can call again with the same name.

### Disconnecting from the bus (`ecu.connected`)

`ecu.connected` is a bool input that every ECU has. The default is true. While it is false, on every bus of that ECU (J1939, raw CAN, LIN, Ethernet), what the ECU sends does not reach the bus, and the ECU receives nothing from the bus. The ECU's steps, timers and variables keep running (the process does not stop).

The implementation puts the ECU's ports behind `detail::GatedCanPort`, `detail::GatedLinPort` and `detail::GatedEthPort` (`ecu/detail/bus_connection.hpp`). `TapPort` sits above them, so it records what the stack saw.

#### When it takes effect

Like any other variable, a write is applied at the start of step P (before the application). It takes effect from that step's transmissions.

For reception, an event at time t (a frame, a LIN header and so on) is judged by the state last decided in a step before t. A change in P takes effect from events with a time after P. The result is the same whether an event at time P comes before or after the ECU's own step P (which depends on SIL Kit, or on the order of ECUs in `LocalBench`).

A LIN master judges the responses to its own headers by the state at the time it sent the header. The gate remembers whether it sent the header of that slot. If the ECU disconnects in a step in the middle of a slot, the response to a header it sent is passed to the ECU whenever it is received.

Writing false and then true in the same step counts as never having been disconnected. Nothing is blocked, and nothing is sent as on reconnection.

#### CAN

| While disconnected | What happens |
|---|---|
| Sending | The port returns `SendResult::Failed` |
| J1939 `Network` | Counts the frame in `send_failures` and drops it (frames are not held, so they are not sent all at once on reconnection) |
| Raw channel | Counts the frame in `frames_dropped` |
| J1939 transport protocol | The peer's responses do not arrive, so the transfer ends by timeout |
| Sender of a BAM | Counts as having finished sending, even though the packets are not sent |
| Bus state (`bus_state`) | Unchanged |

If the virtual bus has a timing model (`CanBusModel`), frames accepted before the disconnection and still waiting in the transmit queue reach the bus after the disconnection. This differs from disconnecting a controller on real hardware.

#### Ethernet

The timing rules are the same as for CAN.

- Sending while disconnected gives the port's `SendResult::Failed`, and the channel counts the frame in `frames_dropped` and drops it.
- Frames are dropped at the end of the step, so `send()` still returns `Ok`.
- Frames with a time while the ECU is disconnected are not passed to the channel.
- Nothing is sent on reconnection.

#### Reconnecting on J1939

Choose the behavior for each network with `NetworkOptions::on_reconnect` (`j1939::OnReconnect`). Whether either value matches what the standard expects has not been verified, so choose the one that matches the ECU you simulate. Reconnection is treated as reconnecting to the bus, not as a power cycle.

| Value | In the step of reconnection | Conflict with a node that took the same address while disconnected |
|---|---|---|
| `Announce` (default) | Each node that has a NAME and has started address claiming sends a Request for Address Claimed to the global destination, and its own Address Claimed (Cannot Claim if it has no address) (`detail::announce_again()`) | Resolved by the other nodes' responses, as for an ordinary claim |
| `Nothing` | Sends nothing | Not found until one of the nodes sends |

With `Announce`, the node's address and state stay as they are, and there is no 250 ms wait after the claim.

There is no option to restart address claiming from the beginning (including the wait), because there is no public function that restarts a node's claim.

#### LIN

| Disconnected ECU | What happens |
|---|---|
| Master | Keeps running the schedule, but neither the headers nor its own frames reach the bus, and the result of every slot is `NoResponse` |
| Slave | Receives neither the headers nor the master's frames and answers nothing, so for the master that slot is `NoResponse` |

- Sleep and wakeup requests of a disconnected ECU do not reach the bus either.
- If the master goes to sleep while disconnected, the SIL Kit controller stays operational and may not match the state of the `LinMaster` (wakeup after that has not been verified).

### `describe()` output

`describe()` returns the following in an `EcuDescription`:

- The ECU's name
- The step size
- The networks it connects to (in call order; for J1939 nodes, the values at the time they were added; for Ethernet, the address written in `options.mac`)
- The variables (`ecu.connected` first, even before the Agent is created)

It does not close declarations and can be called at any time. The content is the same in `Peer` and `LocalBench`. `to_json()` turns it into JSON of a fixed form. For the content and format, see [ecu-executable.md](../ecu-executable.md).

### Threads

Do the setup from one thread. Steps run on the implementation's thread, and functions, handlers and protocol parts are called there.

## `Simulation` (`vehicle_sil::Simulation`)

`Simulation` runs `Ecu`s in virtual time, either on in-process virtual buses or as SIL Kit participants, and reports failures as exceptions (`vehicle_sil::simulation`; its header does not include SIL Kit headers). You choose where to run in the constructor; the ECU code does not change. `Simulation` can be neither copied nor moved.

### Constructor

| Form | Where it runs |
|---|---|
| `Simulation()` | In-process virtual buses |
| `Simulation(SilKitOptions)` | Always SIL Kit |
| `Simulation(std::optional<SilKitOptions>)` | SIL Kit if it has a value; if empty, the same as `Simulation()` (use it to choose where to run at runtime) |
| `Simulation sim({.registry_uri = uri})` (without the type name) | SIL Kit |
| `Simulation sim{{}}` | Matches either constructor, so it does not compile (it does not silently become in-process) |

### Differences by where it runs

| | `Simulation()` | `Simulation(SilKitOptions)` |
|---|---|---|
| What an ECU is | A host from `LocalBench::add_ecu()`. It advances on the thread that called `run_for()`. | A `Peer` (coordinated, virtual time, a participant with the ECU's name). It advances on SIL Kit threads. |
| `run_for(d)` | Can be called any number of times; each call continues from where the last one stopped | Once only (a second call, or a call with `controller = false`, throws `std::logic_error`) |
| `run()` | `std::logic_error` | Waits until the end of the simulation, which any participant may end (once only) |
| Accessors | `local()` for the `LocalBench`, `ecu(name)` for an `Ecu` | `peer(name)` for a `Peer`, `registry_uri()`, `ecu(name)` for an `Ecu` |

### Steps that `run_for()` runs

`run_for(d)` (d > 0) runs only the steps that start before the end time. On the virtual buses, the end time is the current time plus `d`. On SIL Kit, other participants may run steps at or after stop_at until they receive the controller's stop. So the same limit is set on each ECU, which keeps the record the same as on the virtual buses.

A SIL Kit `Simulation` can run only once because, even when every participant is stopped at a time boundary, the step at the boundary may still be running. Reading the model from the main thread would race, and the boundary step would be off by one from the virtual buses. Run tests that stop and check along the way on the virtual buses.

### Failure reporting

An exception in an ECU's step is reported as `SimulationError`. The same applies to failures of ECUs added through `local()`. `what()` is `<ECU name>: <reason>` for each failed ECU, separated by `; `.

| Where it runs | On failure |
|---|---|
| Virtual buses | `run_for()` after the failure also throws `SimulationError` |
| SIL Kit | `run_for()` and `run()` watch for failures at short intervals. When they find one, they end the simulation and throw `SimulationError`. |

On SIL Kit, the watch is needed because SIL Kit does not end the simulation when a participant fails; virtual time just stops. There is no overall timeout.

Exceptions at the start (the required participants are not all present, `start_timeout`) propagate unchanged, after everything has been stopped. On SIL Kit, `run_for()` returns after every participant has stopped. What the steps wrote can be read from then on.

### What `run()` treats as failure

With `controller = true`:

| Participant state | Handling |
|---|---|
| Left in Error | Failure at once |
| Left in a state other than Error, and the simulation does not end within a few seconds | Failure |

The few seconds of waiting keep a simulation that ends with `request_stop()` from being treated as a failure by mistake.

With `controller = false`, `run()` does not check the work of the external controller; it checks only for failures of the ECUs of this process. When an ECU has failed, `run()` behaves as follows. The same applies to a process with only one ECU (`ecu_main`).

1. All ECUs of this process, including the failed one, wait without being stopped until the external controller aborts the simulation (up to 5 s). Meanwhile, the failed ECU stays in Error.
2. A stop requested during the wait (`request_stop()`, a stop request to `ecu_main`) is passed on as is.
3. If the simulation is neither aborted nor stopped within 5 s, the failed ECU is stopped first, regardless of the order in which the ECUs were added, and then the rest.

Waiting without stopping anything lets the controller see the failure. Stopping the failed ECU first would take it out of Error, and stopping a healthy ECU first would be an ordinary stop; either way, the controller would not see the failure.

A stop after the failed ECU has reached Shutdown is an ordinary stop. The whole simulation therefore stops, including the other participants (it never ends by leaving the participant in Error and the other processes still running).

### `SilKitOptions`

| Member | Meaning |
|---|---|
| `registry_uri` | If empty, the constructor starts a registry in the process |
| `controller` | Default `true` (the participant name is `SimulationController`) |
| `other_participants` | Other coordinated participants that must all be present before the start (usable only when `controller` is `true`) |
| `start_timeout` | Default 30 s |

### Argument and call checks

| Condition | Result |
|---|---|
| A name in `other_participants` is empty, duplicated or `SimulationController` | `std::invalid_argument` |
| The name in `add_ecu()` is empty, already used, the controller's name or a name in `other_participants` | `std::invalid_argument` |
| The name in `add_ecu()` is `SimulationController` (also with `controller = false`, and on the virtual buses) | Refused |
| `run_for()` or `run()` is called with no ECU | `std::logic_error` in both implementations |
| An ECU uses a network name that another ECU used for the other kind (CAN or LIN) | `std::invalid_argument` in both implementations |

For all ECUs of a `Simulation`, one network name is either a CAN network (`j1939()`, `can()`) or a LIN network (`lin_master()`, `lin_slave()`), never both. On a violation, the exception message contains both kinds. On SIL Kit, it also names the ECUs that use the name. On SIL Kit, this is checked only among the ECUs of this process; the participant names of other processes are not visible.

### `add<App>()`

`add<App>(name, step, args...)` is `add_ecu()` followed by an `emplace` that makes the ECU own `App{ecu, args...}`.

If the constructor of `App` throws, the exception propagates and that `Simulation` becomes unusable, because what the constructor added to the ECU may refer to the destroyed `App`. After that, `add_ecu()`, `add()`, `run_for()` and `run()` throw `std::logic_error` with the ECU's name.

### Ownership, destruction and threads

`Simulation` owns the ECUs and what they own. The references it returns are valid for the lifetime of the `Simulation`. On destruction, it destroys the ECUs, the last added first, then the controller, and last the in-process registry. The destructor stops everything and does not throw.

Call the functions of `Simulation` from one thread. On SIL Kit, steps run on SIL Kit threads (as in `Peer`).

## Peer (`vehicle_sil::Peer`)

`Peer` creates one SIL Kit participant and what runs in it, runs them, and destroys them in the right order. What runs in it: J1939 `Network`s and raw CAN on CAN, LIN masters and slaves, channels for raw Ethernet frames, and the application's processing in each step. `Peer` implements `Ecu`, so ECU code that takes an `Ecu&` works with it as is.

```cpp
vehicle_sil::Peer peer({.participant = "Engine", .registry_uri = uri,
                        .lifecycle = vehicle_sil::LifecycleMode::Coordinated,
                        .virtual_time = vehicle_sil::VirtualTimeOptions{.step = 10ms}});
auto& model = peer.emplace<EngineModel>();             // owned by the Peer; handlers may refer to it (it may be declared before the Peer)
auto& ecu = peer.j1939("J1939_CAN1").add_node({.address = 0x00, .name = name});
ecu.on_message(kPgnTsc1, [&](const j1939::Message& m) { model.set_request(m.data); });
peer.every(100ms, [&](SimTime) { static_cast<void>(ecu.send(kPgnEec1, model.encode())); });
peer.start();
peer.wait();
```

If you write the setup of several ECUs in functions or classes that take an `Ecu&`, the same code works on both `Peer` and `LocalBench` (example: `examples/j1939/ecu_pair`; a test verifies that both give the same result). `Simulation` lets you choose between them in one line.

### Peer setup functions

What the setup functions create when `Peer` implements them:

| Function | Creates |
|---|---|
| `j1939(network)` | A SIL Kit CAN controller, a `SilKitCanPort`, a `TapPort` and a `Network` |
| `j1939(name, port)` | A `Network` on a port that the user created (such as Vector XL's `ExternalPortAdapter`; the `Peer` owns it) |
| `can(network)` | A raw CAN channel and one SIL Kit CAN controller (named `<network>.raw`) |
| `Ecu::lin_master(network, schedule, options)`, `Ecu::lin_slave(network, options)` | A SIL Kit LIN controller named after the LIN network, a `SilKitLinPort` and the LIN node |
| `Ecu::ethernet(network)` | A SIL Kit Ethernet controller named after the network, an `eth::SilKitEthPort` and an `eth::Channel` |
| `on_step(fn)` | A function called in every step |
| `every(period, fn)` | A function called in the first step at or after each of the times 0, period, 2 period, … (at most once per step; the rule of `vehicle_sil::periodic()`) |
| `at(time, fn)` | A function called once, in the first step at or after `time` (the rule of `vehicle_sil::once_at()`) |

Setup (`j1939`, `can`, `lin_*`, `ethernet`, `on_step`, `every`, `emplace`) is allowed only before `start()` (after it, `std::logic_error`). Attach nodes, handlers, protocol parts, Observers and `TapPort` listeners before `start()` too. Nodes can also be added from inside a step, under the rules of `Network::add_node()`.

Get what was created with these functions:

| Function | Applies to |
|---|---|
| `network()`, `network_names()`, `tap()`, `silkit_port()` | J1939 only (`tap(name)` lets you observe raw frames; `network_names()` returns the list of names in the order added) |
| `raw_channel(network)` | The raw CAN channel |

LIN and Ethernet controllers start at the communication ready notification.

- LIN nodes call `init()` at the communication ready notification.
- A SIL Kit controller cannot be removed once created, so the LIN name, schedule and options are checked first (the engine's checks are `LinMaster::validate()` and `LinSlave::validate()`, the same checks the constructors make).
- A `lin_master()` or `lin_slave()` that threw leaves nothing behind and can be called again with the same name.
- The Ethernet controller is activated at the communication ready notification (`SilKitEthPort::start()`). If that fails, the participant goes to Error, with the network name in the message.

Overlapping controller names are refused as follows:

| Overlap | Result |
|---|---|
| The raw CAN controller name `<network>.raw` and another controller name of the same `Peer` | `std::invalid_argument` |
| J1939 controller names with each other | Refused |
| A CAN controller name and a LIN controller name (the LIN network name) | Refused |
| An Ethernet controller name (the network name) and a CAN controller name | Refused, in either order |

### Order within one Peer step

| Order | What happens |
|---|---|
| 1 | Each J1939 `Network`, raw CAN channel and Ethernet channel receives and processes, in the order it was added |
| 2 | Control Agent writes are applied |
| 3 | The application's processing (`on_step`, `every`, `at`) is called, in the order it was added |
| 4 | `step(now)` of each LIN node is called, in the order the nodes were added |
| 5 | Monitoring is recorded, and the Agent reads values |
| 6 | J1939, raw CAN and Ethernet transmit |

The Agent's processing before and after the application does not depend on when `on_step` or the variables were added. LIN requests from the application (`set_frame_data()`, `set_response()`) take effect from the same step. What a LIN node receives is reported during that step's LIN processing, so the application reads it in the next step (the same order as in `Ecu`).

### Time modes

| `virtual_time` | How it runs | Buses available |
|---|---|---|
| Set | Runs with SIL Kit time synchronization | J1939, LIN, Ethernet |
| Not set | Runs in real time. The `Peer` holds a `RealTimeDriver` and a clock (`SteadyRealTimeClock`, or `PeerOptions::real_time_clock`) and runs the SIL Kit CAN port in real time. | J1939 only (LIN and Ethernet throw `std::logic_error`) |

If the time domain of an external port differs from the `Peer`'s, the result is `std::logic_error`. There is no configuration without SIL Kit (XL only).

### Lifecycle

| Mode | Start and stop |
|---|---|
| `Coordinated` | `SimulationController` decides the start and the stop |
| `Autonomous` | Stops with `stop()`, or with `request_stop()` from inside a step |

- To start several `Peer`s and a controller in one process, use `start_together(controller, {&a, &b})`.
- `Autonomous` participants do not necessarily start at the same time. To run several participants deterministically from the same time, use `Coordinated`.
- In virtual time with `virtual_time.end` set (it must be after 0), the `Peer` stops itself after the last step before end.
- `wait()` waits for the end of the lifecycle.
- In `Coordinated` mode, the `start()` of another participant may see the end of the simulation before its own first step, and throw as a start failure.

#### `request_stop()` before Running

A `request_stop()` before the lifecycle reaches Running (before `start()`, or while waiting for other participants) is held, because SIL Kit ignores a stop before Running. A held request is delivered as follows:

| Case | When the stop is delivered |
|---|---|
| Virtual time | At the first step |
| Virtual time, the request comes after Running but before the first step | Waits for the first step (which normally comes at once) |
| Virtual time, in Error | At once |
| Real time | On reaching Running |
| Real time, Running is reached after `start()` threw on timeout | The held request is not delivered; stop with `stop()` |

`stop()` delivers the stop without waiting for the first step.

In virtual time, the request waits for the first step because the first step requires every participant that synchronizes on time to have started time. Stopping while a participant is not yet Running can leave that participant unable to stop. In a configuration where the controller does not use virtual time, the controller does not take part in this synchronization, so this is not guaranteed.

#### Mixing `Autonomous` and `Coordinated`

If an `Autonomous` virtual-time participant has already advanced virtual time, SIL Kit aborts the whole simulation when a `Coordinated` participant starts later. The participant that was there first also ends. One place in the SIL Kit documentation says that the participant goes to Error, but the implementation and changelog of v5.0.8 say abort. Only `Autonomous` participants can join partway.

#### Failures

`failed()` and `error()` return failures of the step (host, application, LIN, ports), of the step handler and of the real-time driver. When a step fails, the participant goes to Error in both virtual time and real time (in real time, the driver stops), and `wait()` returns.

A `request_stop()` after a failure delivers the stop once the participant is in Error. In real time, this includes requests made between the moment `failed()` becomes visible and the moment the driver reports the failure.

### Peer ownership and destruction

The `Peer` owns the participant, the controllers, the ports, the `Network`s, the LIN nodes and the Control Agent. The references it returns are valid for the `Peer`'s lifetime.

`stop()` (and destruction) stops things in this order:

1. Stop the Agent from waking the real-time driver.
2. Stop the steps (in real time, the driver and then the lifecycle; in virtual time, the lifecycle). No step comes after this.
3. Close the SIL Kit ports.
4. Set the Agent's pending writes to `Cancelled`.

After `stop()`, the `Network`s and the Agent remain, so you can read statistics, records and the last values.

Destruction then destroys, in this order:

1. The LIN nodes (each before its `SilKitLinPort`)
2. The `Network`s (protocol parts first)
3. The `TapPort`s
4. The ports (an external port closes in its destructor)
5. Raw CAN and its ports
6. The host
7. The participant
8. The Agent
9. The objects created with `emplace` (the last added first)

Objects created with `emplace` are destroyed after the participant, so their destructors must not use SIL Kit services.

### Peer threads

| Action | Thread |
|---|---|
| Setup, `start()`, `wait()` | Call from one thread |
| Steps | Run on a SIL Kit thread (virtual time) or the driver's thread (real time). Handlers, application processing, Observers and `TapPort` listeners are called there too. |
| `stop()` | Can be called from any thread except the step and SIL Kit handlers (from another thread, only after `start()` has returned) |
| `request_stop()`, `failed()`, `error()` | Can be called from anywhere, including inside a step |

Read records after `wait()` or `stop()`. Concurrent `stop()` calls are processed one after another. Destroy the `Peer` after `stop()` calls on other threads have returned.

## Local execution (`vehicle_sil::LocalBench`) and the test kit (`vehicle_sil::testkit`)

`LocalBench` runs ECUs on virtual buses and a `StepDriver`, without SIL Kit, and lets you set up J1939 tests. It reads no clock and advances on the calling thread. `LocalBench` (`vehicle_sil::local`, header `vehicle_sil/local/local_bench.hpp`) owns the virtual buses and the ECUs. `vehicle_sil::testkit` adds test tools (recording and fault injection). `testkit::LocalCan` is an alias of `LocalBench`.

```cpp
LocalBench bus;                                                  // virtual bus + StepDriver
install_engine(bus.add_ecu("engine", 10ms), model);              // Ecu& (an ECU with its own host; network J1939_CAN)
bus.add_ecu("rival", 5ms).j1939("J1939_CAN");                    // a network of the same name is the same bus
testkit::J1939Events events(bus.network("engine"));              // Observer
testkit::FrameLog raw(bus.tap("engine"));                        // TapListener
bus.endpoint("rival").set_fault_injector(
    testkit::faults::nth(1, can::FaultAction::drop(), testkit::faults::j1939_pgn(kPgnAddressClaimed)));
bus.run_until(SimTime{} + 1s);
CHECK(events.has(j1939::EventKind::AddressChanged));
```

### Buses and ECUs

`LocalBench` owns a `VirtualCanBus` (whose timing you can choose with `VirtualCanBusOptions`) and a `lin::VirtualLinBus` for each network name, and the ECUs on them.

| Function | What it does |
|---|---|
| `add_ecu(name, step)` | Creates an ECU and returns an `Ecu&` |
| `Ecu::j1939(network)` | Adds an endpoint to the bus named `network`, wraps it in a `TapPort` and creates a `Network` |
| `Ecu::can(network)` | Creates a raw channel. The raw channel also has its own endpoint on that bus. |
| `add_j1939(name, step, options)` | Shorthand that creates an ECU with one J1939 network |

- Each ECU has its own host and step size (the same structure as giving each ECU its own participant on SIL Kit).
- If there is no bus named `network`, `Ecu::j1939(network)` creates it with the constructor's options.
- ECUs that use the same name share the same bus.
- An ECU can have `Network`s on several buses (a gateway).

### Omitting the bus name

`add_j1939()`, `inject(time, frame)`, `injector()` and `bus()` use the one and only bus. Only ECUs give buses names.

| Situation | What happens |
|---|---|
| A form without a name is used while there is no bus yet | Creates a bus that has no name yet (`""` in name lookups) |
| `Ecu::j1939(network)` or `Ecu::can(network)` is called while a bus with no name exists | That bus gets that name; no new bus is created |
| A form without a name is used while there is one named bus | Uses that bus |
| A second name is used | Creates a second bus |
| An ECU was added with `add_j1939()` before the second name was used | `std::logic_error` |
| A form without a name is used while there are 2 or more buses | `std::logic_error` (the message lists the bus names) |

- Forms with and without a name refer to the same bus, in whichever order you write them.
- The second name throws `std::logic_error` because it is not clear which bus the ECU added with `add_j1939()` was meant for.
- That message contains the name of the ECU and the names of the existing buses.
- Add that ECU again with `add_ecu(name, step).j1939(network)`.
- An `add_j1939()`, `j1939()` or `can()` that threw creates no bus and names none.
- A raw channel needs a name (`Ecu::can("")` throws `std::invalid_argument`).

### Virtual Ethernet

`Ecu::ethernet(network)` attaches an endpoint to the virtual Ethernet network named `network` and creates an `eth::Channel`. The network is an `eth::VirtualEthernetBus`, created with default options when an ECU first uses the name.

- Using the name of a CAN or LIN bus throws `std::invalid_argument` with both kinds in the message, in either order.
- Get an ECU's channel with `ethernet(ecu, network)` and the network with `ethernet_bus(network)`.
- Passing an unknown name to `ethernet(ecu, network)` or `ethernet_bus(network)` throws `std::out_of_range`.
- A test can attach its own endpoint with `attach()` before the first run, then send frames to the ECUs and see the ECUs' frames.

### Virtual LIN bus

`Ecu::lin_master(network)` and `Ecu::lin_slave(network)` attach an endpoint named after the ECU to the LIN bus named `network`, create the node and initialize it at once. The LIN bus is created when an ECU first uses the name. The step of a LIN node comes after the ECU's application processing.

| Condition | Result |
|---|---|
| A CAN bus and a LIN bus are created with the same name | `std::invalid_argument` with both kinds in the message, in either order |
| 2 masters are put on one LIN bus | The second throws `std::logic_error` with the names of both ECUs in the message |
| An unknown name is passed to `lin_bus(network)` | `std::out_of_range` with the list of LIN buses in the message |

- The mechanism for a bus with no name (see "Omitting the bus name" above) is for CAN only; LIN is not involved.
- A `lin_master()` or `lin_slave()` that threw attaches no endpoint and creates no bus.
- Get them with `lin_bus(network)` (a `VirtualLinBus&`; add Observers here), `lin_master(ecu, network)` and `lin_slave(ecu, network)`.

### Functions that return what was created

| Function | Returns |
|---|---|
| `network(ecu, network)`, `endpoint()`, `tap()`, `host(ecu)`, `ecu(name)` | Objects looked up by ECU name and network name (`endpoint()` is used for fault injection and disconnection) |
| `raw_channel(ecu, network)` | The raw channel (not covered by the functions above) |
| `bus(network)`, `injector(network)`, `inject(network, time, frame)` | The bus of that name, and injection into that bus |
| `ecu_names()` | The list of ECU names |

- For an ECU with one network, a form that takes only the ECU's name also works (with 0 or with 2 or more networks, `std::logic_error`).
- `bus(network)`, `injector(network)` and `inject(network, time, frame)` only look up a bus by name. They create no bus and name none.
- If no bus has that name, they throw `std::out_of_range`, with the names of the existing buses or the fix in the message.
- For a bus that has no name yet, use a form without a name, or name the bus first with an ECU's `j1939(network)` or `can(network)`.
- You can also get the bus with no name with `bus("")`.
- `inject()` sends a raw frame (11-bit or 29-bit) from a dedicated endpoint that no network uses.
- The time given to `inject()` cannot be before `now()` (the end of the last `run_until()`).
- `on_step(name, fn)` adds application processing to an ECU.

### Steps and the setup deadline

`Ecu::on_step`, `every` and `at` share one list, in the order added. `every` uses the rule of `vehicle_sil::periodic()`, and `at` the rule of `vehicle_sil::once_at()`.

Setup is allowed only before the first run (after it, `std::logic_error`). This covers `add_ecu`, `add_j1939`, and the ECU's `j1939()`, `can()`, `lin_master()`, `lin_slave()`, `ethernet()`, `input()`, `output()`, `set_agent_options()`, `on_step()`, `every()`, `at()` and `emplace()`. Hosts are registered with the driver at the first run, in the order they were added.

### `LocalBench` variables

An ECU's Control Agent runs at the same points as in `Peer` (writes before the application's processing, reading after LIN processing), inside the step on the calling thread. The differences from `Peer`:

- The calling thread advances virtual time, so there is no waking of a real-time driver.
- There are no monitoring outputs (`PeerOptions::monitor`).
- The transport is destroyed before the ECU, so no results are returned for pending writes.
- A local Agent is read and written directly with `submit()` and `latest()`, and does not connect to a Control Server (the only transport is the one over SIL Kit).
- There is no time control (pause, step) and no API that starts a Control Server.

### Destruction order

ECUs are destroyed starting from the last added. For each ECU, the order is:

1. The host
2. The LIN nodes (the last added first)
3. The `Network`s (the last added first; each `Network` followed by its `TapPort`)
4. The raw channels
5. The Ethernet channels
6. The Control Agent
7. The objects created with `emplace` (the last added first)

After that, the driver is destroyed, and last the Ethernet networks, the LIN buses, and the CAN buses and their endpoints.

### The test kit

`J1939Events` (an Observer) and `FrameLog` (a TapListener) keep copies of events and raw frames. A network has one `J1939Events` and a tap one `FrameLog` (a second one is a `std::logic_error`). Beyond their capacity (default 100000 entries), they keep nothing more and count what they did not keep (`dropped()`). Queries count or extract entries by kind (direction) and predicate (`count`, `of`, `first`, `last`).

`faults` is a set of small functions that build a `FaultInjector` for the virtual bus.

- Functions: `when(pred, action)`, `between(from, until, action)`, `nth(n, action)`, `sequence({...})`, `first_of({...})`
- Predicates: `j1939_pgn(pgn)`, `j1939_source(sa)`, `can_id(id, format)`

All of them are deterministic, because their only state is the number of frames selected. Copies of `nth` and `sequence` share that count, so create one for each endpoint. The fault injection mechanism itself is in the virtual bus (see "Virtual bus (`VirtualCanBus`)" in [can.md](can.md#virtual-bus-virtualcanbus)); the test kit adds no injection mechanism of its own.

LIN tests can run on the virtual LIN bus (`LocalBench`) or on `Peer` with an in-process registry. The results are the same (see "Determinism of the virtual LIN bus" in [lin.md](lin.md#determinism-of-the-virtual-lin-bus)). The test kit has no LIN tools for fault injection or recording. To record, observe the virtual LIN bus (`LocalBench::lin_bus(network).add_observer()`). To simulate a disconnection, use `ecu.connected`.

## Test coverage

- `Peer` and the test kit (`peer.*`, `testkit.*`): J1939 and LIN with 3 participants, the same result as on the local virtual bus, real time and external ports, setup errors, failures, destruction order

## See also

- Writing an ECU: [j1939-guide.md](../j1939-guide.md), [ecu-executable.md](../ecu-executable.md)
- Overview: [architecture.md](../architecture.md)
- Where things are: [code-map.md](../code-map.md)
- Other internals pages: "[Internals](../architecture.md#internals)" in architecture.md
