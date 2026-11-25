# Participant foundation (`vehicle_sil::core`)

[日本語](../ja/internals/core.md)

`ParticipantRuntime` owns the SIL Kit participant and its lifecycle. This page describes the guarantees of its public API, ownership and shutdown order, threads and queues, time in real-time mode and the SIL Kit configuration defaults. For how virtual time advances and for the time gate, see [virtual-time.md](virtual-time.md).

| Section | Contents |
|---|---|
| [Public API guarantees](#public-api-guarantees) | `start()`, `stop()`, CommunicationReady handlers, stopping a failed participant, `bind_host()` |
| [Ownership and shutdown order](#ownership-and-shutdown-order) | What owns what, and the order in which `Peer` shuts down |
| [Threads and queues](#threads-and-queues) | Which thread does what, and queue capacities |
| [Time (real-time mode)](#time-real-time-mode) | The clock used without time synchronization |
| [SIL Kit configuration defaults](#sil-kit-configuration-defaults) | The default participant configuration, the keys added to your own configuration, registry settings |

## Public API guarantees

### Constructor and controllers

- The `ParticipantRuntime` constructor connects to the registry. On failure it throws `SilKit::SilKitError`.
- Create bus controllers before `start()`. When `start()` returns, every connected peer has confirmed its subscription to those controllers.
- Objects that refer to a controller (`SilKitCanPort`, `SilKitLinPort`) do not own it. Close or destroy them before the participant.
- `LinMaster` and `LinSlave` do not own their port. Destroy them before the port.
- The J1939 stack has no process-wide state. A process can hold any number of `Network` and `Peer` objects.

### State when `start()` returns

`start(timeout)` waits until the lifecycle is Running. On a timeout or a failure it throws `std::runtime_error`.

Right after it returns, `state()` is Running or a later state. A short run may already be over by the time you look; that still counts as a successful start. `start()` never returns in a state before Running (such as ReadyToRun).

To keep this guarantee, the wait in `start()` keeps 2 orderings.

First, it waits until the state leaves ReadyToRun. For a participant without time synchronization, SIL Kit calls the starting handler in ReadyToRun and switches to Running only after the handler returns. So `start()` does not return on the flag that says the handler was called; it waits until the state has left ReadyToRun.

Second, it reads the flags before the state. Inside the wait, it reads the flags (Running status, starting handler, first step) first and the state after them. Reversing this order opens a path that returns in ReadyToRun. Conversely, when the wait sees a stopping or ended state, it reads the flags again, so that a run that passed through Running and stopped or was aborted before its first step (after the flags were read) does not count as a failure. For the same reason it reads the flags again when `give_up` tells it to stop waiting: `give_up` looks at the simulation later than the wait did, and a short run can have begun and ended in between. If this participant has begun, the answer is not taken and `start()` returns as for any start.

An Error before the participant begins to run is a failed start, even if the state says otherwise when `start()` looks. `start()` goes by the Error that SIL Kit published for this participant (the status handler remembers it), not only by the current state: a SIL Kit without patch 0006 can replace Error with the next state of the start and go on, up to Running (see "Known upstream bugs" in [dependencies.md](../dependencies.md#known-upstream-bugs)). With the patch, the participant stays in Error, and both ways of looking agree. `start()` then throws as for any start that ended in Error, and the step handler is not called for the steps that SIL Kit may still run.

The tests `core.participant_start_*` check both orders, using a test hook in `libs/core/src/participant_runtime_probe.hpp` to force the order (only tests use it; it is not installed).

### CommunicationReady handlers

| Function | What it does |
|---|---|
| `add_communication_ready_handler(handler)` | Adds a handler that is called on SIL Kit's CommunicationReady |
| `set_communication_ready_handler_async(handler)` and `complete_communication_ready()` | Keeps the participant before the first step (CommunicationInitialized), even after the handler returns, until `complete_communication_ready()` is called |

#### `add_communication_ready_handler()`

The handler is called on a SIL Kit thread, before the first step. The thread is the same for an autonomous participant.

Start the controllers in this handler: `SilKitCanPort::start()`, `SilKitEthPort::start()`, LIN `init()` and the bridge's `start_controller()`. SIL Kit recommends calling CAN `SetBaudRate` and `Start`, LIN `Init` and Ethernet `Activate` at this point.

- You can call it only before `start()`.
- Added handlers are called in the order they were added, before the handler of `set_communication_ready_handler()` or `set_communication_ready_handler_async()`. They do not replace each other.
- An exception from an added handler is reported as a failure of the participant (`report_failure()`, "CommunicationReady handler failed: ..."). The remaining handlers are still called.
- Keep everything the handler uses alive until `stop()` returns (or until the `ParticipantRuntime` is destroyed).

`Peer` adds its own controllers (CAN `start()`, LIN `init()`) with this function too. Setting a handler on `peer.runtime()` with `set_communication_ready_handler()` therefore does not replace them. `Peer` passes the name of the network whose controller failed to `report_failure()`.

#### `set_communication_ready_handler_async()` and `complete_communication_ready()`

In coordinated mode every participant waits before the first step, so participants can exchange data over publish/subscribe during that wait. They cannot do so inside the handler. Call `complete_communication_ready()` from another thread that the handler starts.

| Situation | What happens |
|---|---|
| `complete_communication_ready()` is called inside the handler | Not allowed |
| It is called before the handler is called, or a second time | Does nothing and returns false |
| The wait before the first step begins after `stop()` has returned | The handler is not called; the runtime completes by itself |

No participant's step starts until the wait is completed. So always complete it, even when the work done during the wait fails. `SimulationController::set_start_window()` uses this function (see "Transport over SIL Kit (`vehicle_sil::control_silkit`)" in [control.md](control.md#transport-over-sil-kit-vehicle_silcontrol_silkit)).

### Notifications of other participants' states

`set_peer_status_observer(observer)` calls the observer with a name and a state each time the lifecycle state of another participant (or of this one) becomes known.

- The observer is called on the thread that learned the state (a SIL Kit thread or the thread that re-reads the states), one call at a time.
- It may be called again with the same state.
- Keep the observer short, and do not call this runtime from it except `peer_status()`.
- Register it before `start()`.

`wait_for_peer()` waits until the peer's state meets a condition. While it waits, it re-reads the states from SIL Kit periodically (see "SIL Kit: status updates lost while `AddParticipantStatusHandler` registers" in [dependencies.md](../dependencies.md#sil-kit-status-updates-lost-while-addparticipantstatushandler-registers)). It cannot be called from inside a SIL Kit handler.

### Guarantees of `stop()`

- You can call `stop()` any number of times. It does not throw.
- After `stop()` returns, neither the step handler nor any CommunicationReady handler (set, added or asynchronous) is called.
- `stop()` waits for a running handler to finish.
- You can destroy what the handlers use (ports, bridges, stacks) after `stop()`.

CommunicationReady handlers need this guarantee when the start fails before Running (a timeout, `give_up`). SIL Kit does not accept a stop before Running, so after `stop()` the lifecycle stays alive until the `ParticipantRuntime` is destroyed. If a peer it was waiting for arrives late, the lifecycle moves on to CommunicationReady.

| When CommunicationReady arrives | Handlers |
|---|---|
| After `stop()` has returned | Not called; the lifecycle moves on (no controller starts and the step handler is not called). The stop that `stop()` kept is passed where the lifecycle can take it, and the lifecycle stops there: in virtual time at the first step, otherwise when the participant gets to Running |
| While `stop()` waits for the shutdown | Called as usual |

The tests are `core.communication_ready_after_stop_*` and `core.communication_ready_handler_that_runs_is_waited_for_by_stop`.

`stop()` cannot be called from inside a SIL Kit handler (step or CommunicationReady), because it would wait for itself. Use `request_stop()` there. `report_failure()` and `request_stop()` can be called from inside any handler.

### Stopping a failed participant

When a participant fails (in the step handler, in the real-time driver, or through `report_failure()`), its own `stop()` and `request_stop()` do not pass a stop to the lifecycle until the lifecycle is in Error. If an ordinary stop got there first, in coordinated mode the whole simulation would stop normally and the controller would not see the failure.

Setting the failure flag and reporting Error to SIL Kit happen under one lock. A stop issued after the failure became visible therefore never overtakes the report. The implementation does not rely on the order in which SIL Kit runs work it has deferred.

`stop()` waits at most `shutdown_timeout` for Error and Shutdown together. After that, it passes the stop even before Error.

`stop()` normally waits for the future of the final state, which SIL Kit sets at Shutdown once the other participants have replied. After a start that failed by an Error before the participant began to run, it also ends the wait when the lifecycle is in Shutdown: SIL Kit runs one wait for replies at a time, and a stop that comes while the start still waits for replies loses its own wait, so the future is never set.

In these cases the participant does not go through Error, or reaches Error after Stopping:

- A `request_stop()` before the failure (before the failing step handler starts, or before `report_failure()`) is an ordinary stop.
- The same applies when an abort by the controller or a stop by another participant comes first.

What `report_failure()` does depends on when it is called:

| When it is called | What happens |
|---|---|
| Before `start()` begins the lifecycle (before `start()`) | Nothing (SIL Kit ignores it) |
| In a CommunicationReady handler, or after SIL Kit has called it | Reported at once, also when `start()` has not yet returned from beginning the lifecycle |
| Otherwise, while `start()` is beginning the lifecycle | Remembered; reported by `start()`, or when SIL Kit calls the CommunicationReady handler, whichever comes first |

SIL Kit calls the CommunicationReady handler only after the lifecycle was started, so it takes an error reported there. The handler can be called before `start()` returns from beginning the lifecycle, and a short run can even end before that: a failure remembered until then would be reported after Shutdown, where SIL Kit drops it. `ParticipantRuntime` registers a CommunicationReady handler with SIL Kit for every participant, so this also holds for a participant that was given none. The tests `core.participant_start_reports_a_failure_*` and `core.participant_start_does_not_lose_*` check this, using the test hook to force the order.

### `bind_host()`

`bind_host(runtime, host)` (`vehicle_sil/core/bind_host.hpp`) makes the step handler of `ParticipantRuntime` call `step(now)` of a `vehicle_sil::step::Host`. Before the first step, it passes the participant's step size to `Host::set_step_size()`.

| Situation | Result |
|---|---|
| Used on a runtime that is not in virtual time | `std::invalid_argument` |
| The host's time domain is not `Virtual` | `std::logic_error` |
| The host throws | A failure of the step handler: the participant goes to Error |

- Call it before `start()`.
- Keep the host alive longer than the runtime's steps (stop the runtime first).
- For this, `core` links `vehicle_sil::step`, but it does not depend on any bus type (see "Driving in virtual time" in [can.md](can.md#driving-in-virtual-time)).

## Ownership and shutdown order

| Owner or referrer | Object | Relation |
|---|---|---|
| `ParticipantRuntime` | `SilKit::IParticipant` | Owns |
| `SilKit::IParticipant` | `ICanController`, `ILinController` | Owns |
| `SilKitCanPort`, `SilKitLinPort` | Controller | Refers to (does not own) |
| `LinMaster`, `LinSlave` | `LinPort` | Refers to (does not own) |
| `Host` | Ports and `Network` | Refers to (does not own) |

`Peer` shuts down in this order. Use the same order when you assemble the parts yourself.

1. Stop the steps (in real time, `RealTimeDriver` and then the lifecycle; in virtual time, the lifecycle).
2. Close the SIL Kit ports (remove their handlers and stop the controllers).
3. Destroy the LIN nodes (each before its port), `Network` (its protocol parts first), the ports and the host.
4. Destroy the participant.

## Threads and queues

| Thread | Belongs to | Work |
|---|---|---|
| SIL Kit I/O | SIL Kit | Calls the receive handler of `SilKitCanPort` and the virtual-time step handler |
| Driver | vehicle-sil (real time only) | `RealTimeDriver` runs the host's step (receive, protocol processing, application, transmit) |

The receive handler of `SilKitCanPort` stamps each frame with a time and pushes it to the receive queue. If the queue is full, it drops the frame, counts it in `receive_overflows` and turns it into a `ReceiveOverflow` event.

Protocol processing (`Network`, protocol parts, handlers) holds no locks. Only the thread that runs the step touches it: the SIL Kit step handler in virtual time, the driver thread in real time. Other threads hand work over with the host's `post()`.

Queue capacities are fixed and allocated at construction, except the queue behind the host's `post()`. That queue has no limit and allocates on every call. `post()` is an API for operations from outside; it is not meant for use every step.

| Queue | Default capacity | Notes |
|---|---|---|
| `SilKitCanPort` receive queue | 1024 frames | `SilKitCanPortOptions::inbox_capacity` |
| `SilKitCanPort` event queue | 1024 | `SilKitCanPortOptions::event_capacity` |
| Frames `Network` receives in one step | 1024 frames | `NetworkOptions::receive_capacity`. For the other limits, see "Step size and resource limits" in [j1939.md](j1939.md#step-size-and-resource-limits) |
| Receive queue of the example Receiver | 256 | |

## Time (real-time mode)

In these cases the participant does not use SIL Kit time synchronization. It runs with the autonomous lifecycle, entirely in real time.

- `ParticipantRuntimeOptions::virtual_time` is empty (the default).
- `std::nullopt` is passed as `PeerOptions::virtual_time` (the minimal J1939 peer and the Vector XL CAN Bridge).

The default of `PeerOptions::virtual_time` is virtual time with a step size of 1 ms.

- J1939 timers (such as the wait after Address Claim) advance on the driver's clock (`RealTimeClock`).
- The time of a received frame is the receive time the port took from that clock. The SIL Kit timestamps of a participant without time synchronization are not used (see "Driving in real time" in [can.md](can.md#driving-in-real-time-realtimedriver)).

## SIL Kit configuration defaults

### Without a configuration

If `ParticipantRuntimeOptions::participant_configuration` is `"{}"` (the default) or empty, `ParticipantRuntime` creates the participant with `default_participant_configuration(registry_uri)` (`participant_configuration.hpp`). It changes 3 things from the SIL Kit defaults:

| Condition | Key it changes |
|---|---|
| The host of the registry URI is a loopback address | `Middleware.AcceptorUris`, to listen on loopback addresses only |
| Windows | `Middleware.EnableDomainSockets: false` (see "SIL Kit (Windows): closing a local-domain socket to a process with suspended threads may not return" in [dependencies.md](../dependencies.md#sil-kit-windows-closing-a-local-domain-socket-to-a-process-with-suspended-threads-may-not-return)) |
| Always | `Experimental.TimeSynchronization.DynamicSimulationStep: false` |

A participant without the `DynamicSimulationStep` key has its step shrunk by SIL Kit to the smallest step of all participants when another participant (a network simulator, for example) joins with `true`. With `false`, it keeps its own step size.

### Listen address

`is_loopback_registry_uri()` decides whether the address is a loopback address. These count as loopback: `127.0.0.0/8` (in the numeric notations that `getaddrinfo()` reads), `::1`, `127.x.x.x` mapped to IPv6, and `localhost`. It does not resolve names. It is in `vehicle_sil/core/registry_uri.hpp` (header only, part of sim_base), so code that must not link SIL Kit can use it.

On loopback, the participant listens on TCP port 0 of `127.0.0.1` or `::1`. On systems other than Windows, it also listens on a local-domain socket with a new name in the temporary directory. When the internal environment variable `VEHICLE_SIL_SOCKET_DIR` (`kSocketDirectoryVariable`, POSIX only) names an existing writable absolute directory whose socket path fits `sockaddr_un::sun_path`, that directory is used instead; otherwise it falls back to the system temporary directory (or omits the local-domain socket if that path does not fit either). `RunParticipants` creates a `0700` per-run directory for loopback runs, passes it to managed child processes, and clears the variable for non-loopback runs.

`ParticipantRuntimeOptions::listen_on_network` (`std::optional<bool>`; `SimulationControllerOptions` has the same option) controls whether `AcceptorUris` is added.

| `listen_on_network` | `AcceptorUris` |
|---|---|
| `true` | Not added |
| `false` | Decided by the URI alone |
| Empty | Read from the environment variable `VEHICLE_SIL_LISTEN_ON_NETWORK` (`kListenOnNetworkVariable`) |

| Value of the environment variable | `AcceptorUris` |
|---|---|
| `1` | Not added |
| `0`, empty, or variable not set | Decided by the URI |
| Any other value | `std::invalid_argument` |

`vehicle_sil_launch run` passes the value to the controller and always passes the variable to its child processes (see "Listen addresses" in [simulation-run.md](../simulation-run.md#listen-addresses)).

### With your own configuration

If you pass your own configuration (a YAML or JSON string), `merged_participant_configuration()` merges the defaults above into it. The `ParticipantRuntime` constructor calls it. `Peer`, `SimulationController` and `Simulation` use this constructor too.

First, SIL Kit reads the configuration (`ParticipantConfigurationFromString()`: schema check and resolution of `Includes`). Errors stay SIL Kit's `ConfigurationError`. If the string cannot be read as YAML, " (at line <line>, column <column> of the configuration text)" is appended to SIL Kit's message, because SIL Kit's message sometimes has no location.

The content of the configuration is taken from what SIL Kit read (`ParticipantConfigurationToJson()`, also read with rapidyaml). A key counts the same whichever file in `Includes` it is written in.

Missing keys are added as follows. They are added to the passed-in string itself.

| State of the key | What is added |
|---|---|
| `Middleware.AcceptorUris` is missing | Added by the rules in "Listen address" above |
| On Windows, `Middleware.EnableDomainSockets` is not `false` | Set to `false` |
| `DynamicSimulationStep` is missing | `false` is added |

On Windows, `EnableDomainSockets` is set to `false` even when the configuration says `true`, because SIL Kit's JSON does not write the default `true`. A configuration that says `true` cannot be told apart from one that does not set the key.

To add keys, the string is read with the rapidyaml bundled with SIL Kit, the keys are added, and the result is written back as YAML.

- Other keys stay as written.
- A key with nothing under it, such as `Experimental:` (also when only a comment is under it), counts as missing for SIL Kit, so the new key is added under it.
- SIL Kit's JSON is never used in place of the configuration, because it leaves out part of what was read (such as `Format` of a log sink).
- A configuration that needs nothing added is left unchanged, byte for byte.

### Rejected settings

`merged_participant_configuration()` throws `std::invalid_argument` if the configuration passed in has any of these settings. The message says which key to change and how.

- `ParticipantName` differs from the participant name.
- `Middleware.RegistryUri` differs from the registry URI (not compared if the URI is empty).
- `DynamicSimulationStep` is `true`.

SIL Kit takes the name and the URI from the configuration, so accepting a different value would make the participant join under another name or connect to another registry. Identical values are accepted.

### `participant_configuration_from_file()`

`participant_configuration_from_file()` returns the content of the file as a string. If the file has `Includes`, it adds the file's directory to `Includes.SearchPathHints`, because for a configuration given as a string SIL Kit searches only the working directory and `SearchPathHints`.

It does not check the content. For these files it returns the string unchanged, and SIL Kit reports the error when the string is used:

- A file that cannot be read as YAML
- A file with nothing under `Includes`
- A file whose `SearchPathHints` is not a list

### Builds without rapidyaml

A build without rapidyaml (when SIL Kit is not built from the submodule) uses the configuration passed in as it is.

| Action | Build without rapidyaml |
|---|---|
| Adding keys | Not done (the listen address is also whatever the configuration says) |
| The 3 checks above (`ParticipantName`, `Middleware.RegistryUri`, `DynamicSimulationStep: true`) | Not done |
| `merged_participant_configuration()` has SIL Kit read the configuration | Not done (a configuration SIL Kit does not accept shows up when the participant is created) |
| `participant_configuration_from_file()` adds `SearchPathHints` | Not done |
| Defaults when no configuration is passed (`{}`) | Same as in other builds |

In such a build, write `Middleware: {EnableDomainSockets: false}` (on Windows) and `"Experimental":{"TimeSynchronization":{"DynamicSimulationStep":false}}` yourself, and do not write `ParticipantName` or `Middleware.RegistryUri`.

### Step size check

If SIL Kit gives a virtual-time `ParticipantRuntime` a step whose size differs from `VirtualTimeOptions::step`, the runtime fails that step without calling the step handler. The participant goes to Error. `step_error()` is "SIL Kit gave this participant a step of ... ns at t=... ns, not its own ... ns: ...", and the message names the key to set to `false` in the configuration. The check exists because the step grid, `TickScheduler` and the time gate assume a fixed step size.

This does not happen with the default configuration or with your own configuration, because the key is added. It happens only in a build without rapidyaml, when you pass a configuration without the key or with `true`.

The comparison is `detail::step_width_error()` in `libs/core/src/participant_runtime_checks.hpp`. The test `core.a_step_that_silkit_cut_fails_with_the_key_that_keeps_the_step` checks it without SIL Kit.

### Registry settings

The registry that vehicle-sil starts (`vehicle_sil_launch run`, `Simulation`) is created with `default_registry_configuration()`.

- The registry listens on the address of its own URI (it does not use `AcceptorUris`).
- `Middleware.EnableDomainSockets: false` on every OS (TCP only).

The reason for TCP only: a SIL Kit registry that cannot listen on its TCP port keeps running on a local-domain socket alone, and takes over the local-domain path of another registry with the same URI. With TCP only, `StartListening()` fails at once on a port that is in use.

### Recreating the participant when the name is taken

`ParticipantRuntime` creates the participant with `create_participant()`. While the registry rejects the name as in use ("A participant with the same name ... already exists"), it creates the participant again every 50 ms, for up to `ParticipantRuntimeOptions::name_in_use_wait`.

- The default of `name_in_use_wait` is 5000 ms. `PeerOptions` has the same option. `Simulation` uses a value no greater than `start_timeout`.
- The retry exists because, for a process restarted with the same name, the registry may not yet have noticed that the previous connection ended (on a slow computer, or across a network).
- When the time runs out, it throws `NameInUseError` (a `SilKit::ProtocolError`) with SIL Kit's message, extended with how long it waited and the hint that another participant may be using the same name.
- After a join that timed out waiting for replies from known participants ("Timeout while waiting for replies from known participants"; see "SIL Kit: a participant that joins while another one leaves waits for the one that left" in [dependencies.md](../dependencies.md#sil-kit-a-participant-that-joins-while-another-one-leaves-waits-for-the-one-that-left)), it creates the participant once more and writes one line saying so to `std::cerr`. A second such timeout is thrown as a `SilKit::ProtocolError` with SIL Kit's message and a note that it tried again. This retry happens once in all and does not extend `name_in_use_wait`, so the whole wait is at most `name_in_use_wait` and two join timeouts.
- Any other failure is thrown at once.
- Programs that create a SIL Kit participant themselves (such as the raw Agent in the tests) can use this function too.

### One place that builds the configuration

Every place in `libs/`, `tools/`, `bridges/` and `tests/` that builds a SIL Kit configuration from a string uses these functions (test `core.silkit_configuration_defaults`).

## See also

- Overview: [architecture.md](../architecture.md)
- Where things are: [code-map.md](../code-map.md)
- The other internals pages: "[Internals](../architecture.md#internals)" in architecture.md
