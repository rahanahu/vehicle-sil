# LIN in virtual time (`vehicle_sil::lin`)

[日本語](../ja/internals/lin.md)

This page gives the rules and constraints that the LIN master and slave (`LinMaster`, `LinSlave`), the port (`LinPort`) and the virtual LIN bus (`VirtualLinBus`) keep. For usage examples, see [lin-guide.md](../lin-guide.md).

| Section | Contents |
|---|---|
| [Library structure](#library-structure) | Dependencies, port implementations, what each header holds |
| [Common rules](#common-rules) | Steps, when requests take effect, reception guarantees, threads, `init()`, exceptions |
| [Schedule table (`LinMaster`)](#schedule-table-linmaster) | Entry requirements, ID settings, when results are returned, switching tables |
| [Sending and receiving frames](#sending-and-receiving-frames) | Values the slave and the master send, collisions, slave reception, the SIL Kit constraint, a limitation |
| [Diagnostic frames (0x3C, 0x3D)](#diagnostic-frames-0x3c-0x3d) | Rules for the master request and the slave response |
| [Sleep and wakeup](#sleep-and-wakeup) | Behavior per request, priority at the same time, SIL Kit controller state |
| [Port (`LinPort`) and virtual LIN bus (`VirtualLinBus`)](#port-linport-and-virtual-lin-bus-virtuallinbus) | Port functions and notifications, `SilKitLinPort`, delivery and misuse on the virtual bus |
| [Signal encoding and sensor peers](#signal-encoding-and-sensor-peers) | What does not exist, and the rules for adding it |
| [Test coverage](#test-coverage) | What the tests check |

## Library structure

`vehicle_sil::lin` depends only on `vehicle_sil::sim_base`. It depends on neither SIL Kit nor J1939. The CTest test `lin.boundary` checks that its sources and headers do not include SIL Kit.

The engines (`LinMaster`, `LinSlave`) use the bus only through a port (`LinPort`). There are 2 port implementations:

| Implementation | Library | Bus |
|---|---|---|
| `VirtualLinBus` | `vehicle_sil::lin` (the same library) | In-process |
| `SilKitLinPort` | `vehicle_sil::lin_silkit` | SIL Kit LIN controller (trivial simulation) |

`Ecu::lin_master()` and `Ecu::lin_slave()` create the node on the port that fits where the ECU runs: the virtual bus in `LocalBench`, SIL Kit in `Peer`.

The files are split by role:

| Header | Contents |
|---|---|
| `lin_types.hpp` | Common types (`LinId`, `LinChecksumModel`, `LinFrame`, `LinRole`, `LinResultStatus`, `LinPublisher`, `LinNodeState`, `LinReceivedFrame`) |
| `lin_schedule.hpp` | Schedule table (`LinScheduleEntry`, `LinScheduleTable`, `validate_schedule()`) |
| `lin_port.hpp` | `LinPort`, `LinPortListener` |
| `lin_master.hpp` | `LinMaster` |
| `lin_slave.hpp` | `LinSlave` |
| `virtual_lin_bus.hpp` | `VirtualLinBus`, `LinBusEvent` |
| `lin_silkit/silkit_lin_port.hpp` (`vehicle_sil::lin_silkit`) | `SilKitLinPort` |

## Common rules

### Steps and when requests take effect

- Both the master and the slave call `step(now)` once per step, with the time of that step.
- Everything sent on the bus (headers, master frames, go-to-sleep commands, wakeup pulses) is sent inside `step()`.
- A request (`set_frame_data()`, `send_master_request()`, `set_schedule()`, `request_sleep()`, `request_wakeup()`) made in a step before `step(now)` takes effect in that step.

### Reception guarantees

Whatever is sent in step T is always received by the other nodes by their first step after T. On the virtual bus it is received inside the call that sent it. On SIL Kit, SIL Kit sends the frames that the master sends one stage later, but `ParticipantRuntime` completes the step only after that, so the same guarantee holds (see "Joining and advancing" in [virtual-time.md](virtual-time.md#joining-and-advancing)).

The receiver collects the events it gets in a `StepInbox`, and `step(now)` processes only those before now, in time order. State changes and frame reception therefore depend neither on the wall clock nor on the order of steps at the same time.

### Creating nodes and their lifetime

- Create nodes with `LinMaster(port, schedule, step, options)` and `LinSlave(port, options)`. `step` is the step size the node runs at.
- The port must outlive the node. The node is the port's listener from its constructor to its destructor.
- Set the handlers (`LinMaster::on_result()`, `on_state()`, `LinSlave::on_frame()`, `on_state()`) before the first step (they are not thread-safe). An empty function means no handler.
- Through `Ecu`, the implementation chooses the port and the step size.

### State notifications

State changes are reported to `StateHandler(time, state)` with a time.

| Node | Time reported |
|---|---|
| Slave | The time of the event it received (the time of the step, for its own action) |
| Master | The time of the step in which the state changed |

### Threads

Call `init()`, `step()` and `request_*()` from the node's step. Ports are not thread-safe. However, SIL Kit calls the step handler and the receive handlers on the same I/O thread, and the virtual bus delivers inside the sender's step, so all calls to a port stay on one thread.

`LinSlave::set_response()`, `clear_response()` and `set_slave_response()` can be called from any thread.

### When to call `init()`

Call `init()` of both nodes once, before the first step (through `Ecu`, the implementation calls it). When you assemble a node on a SIL Kit port yourself, call it from `ParticipantRuntime::add_communication_ready_handler()` (or `set_communication_ready_handler()`). If the node initializes before the subscriptions are established, other participants do not receive its settings.

### Exception messages

Exception messages start with `vehicle_sil::lin::LinMaster: `, `vehicle_sil::lin::LinSlave: ` or `vehicle_sil::lin: ` (schedule), and include the value and how to fix it.

## Schedule table (`LinMaster`)

An entry has an ID, a data length, a checksum, a slot length and a publisher (`Slave` or `Master`). The table repeats. The first header is sent at T = 0.

### Entry requirements (`validate_schedule()`)

If any of these requirements is not met, the result is `std::invalid_argument`.

| Subject | Requirement |
|---|---|
| Table | Not empty |
| ID | 0–63 |
| Data length | 1–8 |
| Slot length | A positive multiple of the master's step size |
| Entries with the same ID | Same data length, checksum and publisher |
| Diagnostic frames (0x3C sent by the master, 0x3D by a slave) | 8 bytes and Classic |

The only checksum models (`LinChecksumModel`) are Classic and Enhanced. The receiver compares the model with the setting of the header.

### Changing ID settings

Across all the tables given to a `LinMaster` (in the constructor and with `set_schedule()`), the data length, checksum and publisher of an ID must stay the same. If they differ, `set_schedule()` throws `std::invalid_argument`. The reason is that SIL Kit does not accept a change to the settings of an ID once they are set. The virtual bus applies the same rule so that results do not depend on the port.

### When results are returned, and switching tables

- The result of a header (Ok, NoResponse or Error, with the data) is returned at the end of its slot, that is, in the step that sends the next header.
- The result carries the time the header was sent and the publisher.
- `set_schedule()` switches to the new table at the next slot boundary and starts from the top of the new table.
- If `set_schedule()` is called while asleep, the switch happens when the schedule resumes after wakeup.

## Sending and receiving frames

### Frames a slave sends

`LinSlave` responds with the latest value set by `set_response(now, id, data)` in a step whose time is strictly before the time of the header.

The slave receives the header from the port (`LinPortListener::on_header()`) and responds inside that call (`LinPort::respond()`). The SIL Kit port uses dynamic response (`InitDynamic`, `AddFrameHeaderHandler`, `SendDynamicResponse`; experimental in SIL Kit v5.0.8).

A static TX buffer is not deterministic: when the header and the slave's step have the same time, which one is processed first is not defined (on SIL Kit, and in the step order of the virtual bus alike).

After `LinSlave::clear_response(now, id)`, the slave no longer responds to headers after now.

### Frames the master sends

The latest value from `LinMaster::set_frame_data(id, data)` is sent in the step that sends the header (SIL Kit `MasterResponse`). Until the first value is set, zeros are sent. `set_frame_data()` accepts only IDs that the master sends in a table given so far, and the data length must match.

### Responses from several nodes

| Case | Result |
|---|---|
| 2 or more nodes respond to one header (2 slaves, or a slave responding to a frame the master sends) | A collision, as on a real bus: Error, with no data |
| Different slaves respond to one ID at different times (one slave stops with `clear_response()` and another calls `set_response()`; diagnostic 0x3D) | Allowed |

In a collision, every response is received by the end of the slot, so the result does not depend on arrival order.

In SIL Kit v5.0.8, after a second slave responded, headers for that ID were no longer sent. A patch fixes this (see "Patches to SIL Kit" in [dependencies.md](../dependencies.md#patches-to-sil-kit)).

### Slave reception

Of the IDs listed in `LinSlaveOptions::subscriptions`, frames that the master sends are passed from `step()` to the `FrameHandler`. Their time is the time of the header.

### Constraint: step size of a responding slave (SIL Kit)

On SIL Kit, the step size of a responding slave must be strictly less than the slot length of the frame.

The slave responds to a header inside the receive handler, so the response is not tied to a step. With slave step size D and slot length S:

| Condition | Response to header T |
|---|---|
| D < S | Always received before the master's step at T + S |
| D ≥ S | Not guaranteed. It may become NoResponse, or be taken as the response to a later header for the same ID |

On the virtual bus the response is received inside the call that sent the header, so this constraint does not apply. To get the same result on both, keep the SIL Kit constraint.

### Limitation: slaves receiving each other's responses (slave-to-slave)

A slave receiving another slave's response (slave-to-slave) is not supported. Put only IDs that the master sends in the subscriptions.

The virtual bus does not deliver a slave's response to other slaves.

On SIL Kit, the timestamp of a response is the responding slave's `Now()`, not the time of the header. The latest arrival is set by the responding slave's step size, which the receiver cannot know. The SIL Kit port cannot tell a master frame from another slave's response, so if another slave sends a subscribed ID, the port hands that frame over too, but its time and the step in which it is handed over are not deterministic.

In a collision where a slave responds to a frame that the master sends (a configuration error), what reaches the slaves depends on the port. The master's result (Error) is the same on both.

| Port | What reaches the slaves |
|---|---|
| Virtual bus | No frame is delivered to any slave |
| SIL Kit | A subscribed slave may receive both the master's frame and the response |

## Diagnostic frames (0x3C, 0x3D)

The LIN 2.x diagnostic frames are handled with rules separate from ordinary frames. LIN diagnostics themselves (the ISO 17987-2 transport layer and services) are not implemented. The application builds and interprets the frame contents (NAD, PCI, SID and data).

### Master request (0x3C)

`LinMaster::send_master_request(data)` registers one request, which is sent once in the next 0x3C slot of the table. In a 0x3C slot with no registered request, no header is sent and no result is returned.

- The next request cannot be registered until the previous one has been sent (the call returns `false`). Check with `master_request_pending()`.
- While asleep, or while the table has no 0x3C, the request stays registered and waits.
- A request not yet sent can be cancelled with `cancel_master_request()` (on a diagnostic timeout, for example).
- The data is 8 bytes.
- Data whose first byte (NAD) is 0 is a go-to-sleep command and is not accepted (use `request_sleep()`).
- `set_frame_data()` cannot be used for 0x3C.
- A slave that subscribes to 0x3C (`subscriptions`) receives master requests through its `FrameHandler`. Go-to-sleep commands are not passed on.

### Slave response (0x3D)

With the 8 bytes set by `LinSlave::set_slave_response(now, data)`, the slave responds once to the first 0x3D header after now. After that it does not respond until a new response is set. The 0x3D header is sent every time; if no slave responds, the result is NoResponse.

- Each call makes a new response that is sent once.
- For headers after now, it replaces the response set before (whether or not that one was sent).
- A response not yet sent can be cancelled with `clear_response(now, 0x3D)`.
- `set_response()` cannot be used for 0x3C and 0x3D.
- All slaves share 0x3D. The application decides which slave responds, by NAD.
- If 2 slaves respond to the same header, the result is Error.

## Sleep and wakeup

### Requests and behavior

| Request or condition | What happens |
|---|---|
| `LinMaster::request_sleep()` | At the next slot boundary, the master sends a go-to-sleep command (ID 0x3C, `00 FF FF FF FF FF FF FF`) instead of the next header, and goes to sleep |
| `LinMaster::request_wakeup()` (while asleep) | A wakeup pulse is sent in the next `step()` |
| `LinSlave::request_wakeup()` (while asleep) | A wakeup pulse is sent in the next `step()` |
| A wakeup pulse sent by a slave at time T | The master receives it in its first step after T and wakes up |
| A slave receives a go-to-sleep command | It goes to sleep |
| The bus is silent for `LinSlaveOptions::bus_idle_timeout` (default 4 s; `std::nullopt` disables it) | The slave goes to sleep |
| A slave receives a wakeup pulse or a header | It wakes up |
| A request that does not fit the state (wakeup while awake, sleep while asleep) | Discarded |
| Sleep and wakeup requested in the same step while asleep | Only the wakeup happens |
| Sleep requested between wakeup and the resumption of the schedule | At the slot boundary where the schedule resumes, the go-to-sleep command is sent without a header |

When the master goes to sleep through `request_sleep()`, the result of the slot that ended there is returned as usual.

A master that has woken up resumes the schedule from the top of the table. It resumes in the first step at least `LinMasterOptions::wakeup_delay` (default 100 ms; in LIN 2.x, the time slaves need to get ready) after the time of the pulse.

### A go-to-sleep command and a wakeup pulse at the same time

When a go-to-sleep command and a wakeup pulse have the same time, the go-to-sleep command wins. The master does not wake up on a pulse at that time, and a slave processes the command after the pulse.

### Responses from a sleeping slave

A sleeping slave still responds to headers. The break field also acts physically as a wakeup signal. Moreover, a rule that a sleeping slave does not respond would make the result depend on whether another slave's pulse or the header is received first.

### SIL Kit controller state

On the SIL Kit port, the master's controller goes to Sleep on go-to-sleep and returns to Operational on wakeup (`send_wakeup()` uses `Wakeup`; `resume()`, when woken by another node's pulse, uses `WakeupInternal`).

A slave's sleep state exists only inside `LinSlave`; the port (the SIL Kit controller) always stays Operational. A controller in Sleep drops frames, including the go-to-sleep frame, so whether the command was received would depend on arrival order. `send_wakeup()` of the SIL Kit slave port puts the controller into Sleep only right before it sends the pulse (because `Wakeup()` issues a warning on an Operational controller).

## Port (`LinPort`) and virtual LIN bus (`VirtualLinBus`)

The port is the only interface through which a node uses the bus. Its role (master or slave) is fixed when the port is created; functions of the other role give `std::logic_error`.

### Port functions

| Role | Function | What it does |
|---|---|---|
| Master | `send_header(now, id, length, checksum)` | Sends the header of a frame that a slave responds to |
| Master | `send_frame(now, frame)` | Sends a header and the master's own response |
| Master | `send_go_to_sleep(now)` | Sends a go-to-sleep command |
| Slave | `respond(header_time, frame)` | Responds (only from inside `on_header()`) |
| Both | `send_wakeup(now)` | Sends a wakeup pulse |
| Both | `resume()` | Called when woken by another node's pulse (sends nothing on the bus) |
| Both | `open()` | Called by `init()` |
| Both | `set_listener()` | Sets the listener |

- Arguments: `send_header()` and `send_frame()` throw `std::invalid_argument` if the ID is above 63 or the length is outside 1–8 (both implementations; `LinMaster` never sends such values).
- Time: `now` is the time of the calling step. The SIL Kit port does not use it; SIL Kit stamps the participant's time.
- Threads: the SIL Kit port calls the listener from the SIL Kit I/O thread. The virtual bus calls it synchronously, inside the sender's call.
- Call `set_listener()` only while no events arrive (before and after the run).

### Listener notifications (`LinPortListener`)

The engine implements the listener.

| Notification | Port notified | Contents |
|---|---|---|
| `on_header(time, id)` | Slave only | Every header the master sends (also for frames the master responds to itself) |
| `on_response(header_time, frame, status)` | Master | The result of its own header |
| `on_response()` | Slave | A frame the master responded to (Ok) |
| `on_go_to_sleep(time)` | Slave only | A go-to-sleep command |
| `on_wakeup(time)` | Both | Only pulses from other nodes (never its own) |

The results that `on_response()` reports on the master's port:

| Outcome | Status |
|---|---|
| Its own frame was sent | Ok |
| A slave's response | Ok, with the data |
| No response | NoResponse (may arrive synchronously from inside `send_header()`) |
| Mismatched length or checksum model | Error |

On the master's port, `on_response()` can arrive 2 or more times for one header (several responses). `LinMaster` decides whether there is a collision.

On a slave's port, `on_response()` never carries a go-to-sleep command or the slave's own response. Responses from other slaves are not delivered by the virtual bus; the SIL Kit port cannot tell them apart and hands them over (see "Limitation: slaves receiving each other's responses (slave-to-slave)" above).

### SIL Kit port (`SilKitLinPort`)

`SilKitLinPort(controller, role)` registers SIL Kit handlers in its constructor and removes them in its destructor. It does not own the controller, so destroy it before the participant. Type conversion (`LinFrame`, checksum model, state) happens only inside this port.

- `open()` calls `Init` on the master (with an empty frame response configuration) and `InitDynamic` on a slave.
- Only the slave's header handler is registered after `InitDynamic` in `open()`. The SIL Kit C API treats registration before that as a state error. Both happen inside the same handler, so no header is received in between.
- If the ID has not yet been configured for reception, `send_header()` configures it with the header's length and checksum model (`SetFrameResponse`) before sending the header. The IDs of the first schedule are configured through the same path.

### Virtual LIN bus (`VirtualLinBus`)

`VirtualLinBus` is an in-process LIN bus. One master and any number of slaves can connect (`attach(role, name)`; a second master gives `std::logic_error` naming both). The bus owns the endpoints (`VirtualLinBus::Endpoint`, `LinPort`). It has no threads and no queues; it delivers everything inside the sender's call, in the order the endpoints were attached.

| Call | Delivery |
|---|---|
| `send_header()` | Calls `on_header()` of every slave and collects the `respond()` calls made inside it |
| `send_frame()` | Calls `on_header()` of every slave |
| `send_go_to_sleep()` | Calls `on_go_to_sleep()` of every slave |
| `send_wakeup()` | Calls `on_wakeup()` of every endpoint except the sender |
| `open()`, `resume()` | Nothing |

The slaves' responses decide the result of `send_header()` and `send_frame()`.

| Call | Responses | Result |
|---|---|---|
| `send_header()` | None | NoResponse to the master, once |
| `send_header()` | Some | Once per response (Ok if the length and checksum model match the header, otherwise Error) |
| `send_frame()` | No slave responded | The frame is delivered with Ok to the master and all slaves |
| `send_frame()` | A slave responded (collision) | The master's own frame and each response are delivered to the master (2 or more times); nothing is delivered to the slaves |

An observer (`add_observer()`) sees each event once, after it has been delivered to the endpoints (`LinBusEvent`: Frame, NoResponse, Collision, GoToSleep or Wakeup, with the time, the sender's name and the frame). Events that different ECUs send at the same time are in step order.

Limitations:

- Endpoints cannot be removed. Even after the master node is destroyed, no new master can connect to that bus.
- The virtual bus has no fault injection (dropping or corrupting responses).

### Misuse of the virtual LIN bus and exceptions

| Misuse | Exception |
|---|---|
| `respond()` outside `on_header()` | `std::logic_error` |
| A second `respond()` within one `on_header()` | `std::logic_error` |
| Sending from inside a listener or an observer (reentry) | `std::logic_error` |
| `attach()` or `add_observer()` during delivery | `std::logic_error` |
| A response with an ID or time different from the header, or a wrong length | `std::invalid_argument` |

The message of a `std::logic_error` includes the endpoint's name and how to fix the problem.

An exception thrown by a listener or an observer propagates to the sender's call. What was already delivered is not undone, and the endpoints and observers after it do not get that event. The bus stays usable.

### Determinism of the virtual LIN bus

`StepDriver` runs steps by (start time, registration order), so a step at time T runs only after every step of the other hosts before T has finished. The engines use only values set strictly before T and events before now, so the order of steps at the same time does not change the result. These are the same rules as on SIL Kit, so the results are the same too. Tests run the same ECU class with `Simulation()` and with `Simulation(SilKitOptions{})` and check that the results, receptions, state changes and statistics match.

## Signal encoding and sensor peers

There is no LIN signal encoding (conversion between signal values and frame data) and no helper for building sensor peers. The application builds the frame data. If you add them, follow these rules:

- Use the SIL Kit LIN API, and do not reimplement what SIL Kit provides.
- Keep these concerns separate:
  - Master and slave roles
  - Schedule tables
  - Signal encoding
- A LIN slave sensor peer must make a sensor value appear in the LIN response just by setting the value.

## Test coverage

### In-process tests

| Test name | Runs on |
|---|---|
| `lin.*` | SIL Kit ports |
| `lin_local.*` | The virtual LIN bus of `LocalBench` (the main scenarios, with the same expected values as `lin.*`) |
| `lin_bus.*` | The virtual LIN bus alone |

They check:

- Response values follow the header-time rule and depend neither on wall-clock speed nor on the order of steps at the same time (the order in which ECUs were added)
- 2 slaves receive a frame the master sends, and the master receives a slave's response
- Sleep and wakeup (master request, slave pulse, bus idle, headers while asleep, go-to-sleep and pulse at the same time), schedule switching (including while asleep), cancelling a response, Error on a data length mismatch
- Boundaries and error cases: ID 0 and 0x3F, data lengths 1 and 8, mixed slot lengths, 2 schedule switches in the same slot, Error on a checksum model mismatch (both directions), Error on a collision of 2 responses (the same at other speeds), 2 slaves responding to the same ID at different times, the default bus idle (exactly 4 s), 2 wakeup pulses at the same time, `wakeup_delay` 0, overflow of a slave's receive queue (counted, and recovered afterwards), rejection of misused diagnostic frames
- Diagnostic frames: a master request is sent exactly once and the addressed slave responds exactly once; replacing, cancelling and colliding responses; cancelling a request; a multi-frame exchange (the same at other speeds); a request registered while asleep is sent after the schedule resumes
- Cluster traces match when the master's or slaves' speed changes
- Delivery on the virtual LIN bus (receivers, order, synchronous NoResponse, mismatches, collisions, go-to-sleep, wakeup, observers, misuse exceptions), and the name check of `Ecu::lin_master()` and `lin_slave()` and their exception after the start
- The same LIN ECU class run with `Simulation()` and with `Simulation(SilKitOptions{})` gives matching recordings (including Error on a length mismatch and the collision of a slave responding to the master's frame; `simulation.*`)
- `vehicle_sil::lin` does not include SIL Kit (`lin.boundary`)

### Multi-process integration tests

The LIN integration tests (`lin_cluster.*`) run the registry, `vehicle_sil_sim_controller`, a master (5 ms), an echo slave (2 ms) and a status slave (4 ms) as separate processes.

- They check the schedule times and order, the contents of master frames, the reception times of both slaves, the contents of slave responses, and the sleep and wakeup times of each node.
- A run with the echo slave coupled to real time checks that the trace matches byte for byte.
- They also check a configuration with the master's step size equal to the slot length, 10 ms (`cluster-master-10ms`).
- Each slave also records in the trace the step in which it processed a frame, and the test checks that it is the step right after the header.

## See also

- Usage examples: [lin-guide.md](../lin-guide.md)
- How virtual time advances and the order of reception: [virtual-time.md](virtual-time.md)
- Overview: [architecture.md](../architecture.md)
- Where things are: [code-map.md](../code-map.md)
- The other internals pages: "[Internals](../architecture.md#internals)" in architecture.md
