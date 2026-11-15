# Writing J1939 nodes

[日本語](ja/j1939-guide.md)

This page shows, in C++, how to use the J1939 stack of vehicle-sil to create ECU nodes, send and receive messages, and run and test the ECUs in a simulation. For the API's guarantees and rules in detail, see [internals/j1939.md](internals/j1939.md). The API may still change.

| Goal | Use | Section |
|---|---|---|
| Create a node | `Ecu::j1939()`, `Network::add_node()` | [Create a node](#create-a-node) |
| Relay between 2 networks | `Ecu::j1939()` with another network name | [ECUs with several networks](#ecus-with-several-networks) |
| Send and receive frames that are not J1939 | `Ecu::can()` | [Protocols other than J1939 (raw CAN)](#protocols-other-than-j1939-raw-can) |
| Receive messages | `codec::on_message`, `Node::on_message` | [Receive messages](#receive-messages) |
| Send a message once, at a set time or periodically | `Node::send`, `Ecu::at`, `PeriodicSender` | [Send messages](#send-messages) |
| Respond to a Request | `Node::on_request` | [Respond to a Request](#respond-to-a-request) |
| Send a Request and receive the response | `Requester` | [Send a Request and receive the response](#send-a-request-and-receive-the-response) |
| Read the latest value and detect message loss | `Monitor` | [Monitor reception](#monitor-reception) |
| Read the DTCs of other nodes | `parse_dtc_list()` | [Read other nodes' DTCs (DM1, DM2)](#read-other-nodes-dtcs-dm1-dm2) |
| Change the address of another node | `AddressCommander` | [Change another node's address](#change-another-nodes-address) |
| Write periodic processing | `PeriodicSender`, `Ecu::every` and others | [Choose a periodic mechanism](#choose-a-periodic-mechanism) |
| Handle send failures and usage errors | `SendStatus`, exceptions | [Handle errors](#handle-errors) |
| Implement a manufacturer-specific protocol | `Exchange`, `Timer`, `Subscription` | [Implement your own protocol](#implement-your-own-protocol) |
| Run ECUs in a simulation | `Simulation` | [Run in a simulation](#run-in-a-simulation) |
| Test an ECU | `Simulation::local()`, `vehicle_sil::testkit` | [Test an ECU](#test-an-ecu) |
| Disconnect an ECU from the bus | The variable `ecu.connected` | [Disconnect an ECU from the bus (`ecu.connected`)](#disconnect-an-ecu-from-the-bus-ecuconnected) |
| Receive bus traffic without sending anything | `j1939::Listener`, `Ecu::can()` | [Listen-only participants](#listen-only-participants) |
| Read and write an ECU's values from a UI or a test | `Ecu::input`, `Ecu::output` | [Read and write signals from outside (UI, tests)](#read-and-write-signals-from-outside-ui-tests) |
| Work with signals without writing bit positions | Typed messages | [Data layout (Codec)](#data-layout-codec) |

To run something first, use [examples/getting_started](../examples/getting_started). The complete example is [examples/j1939/ecu_pair](../examples/j1939/ecu_pair). It builds 2 ECUs, an engine and a cab, from protocol parts only, and runs them with `Simulation`, either on the virtual bus or as 2 SIL Kit participants. The tests use the same code.

## Reading the code examples

The code on this page consists of excerpts. They do not compile as is: they leave out `#include` lines, `using` declarations and variable declarations. Each excerpt names the file and function that hold the complete, compilable code. An excerpt that shortens a file of the repository says "shortened".

The excerpts use the same aliases as [examples/j1939/ecu_pair/ecu_pair.hpp](../examples/j1939/ecu_pair/ecu_pair.hpp):

```cpp
using namespace std::chrono_literals;
namespace j1939 = vehicle_sil::j1939;
namespace codec = vehicle_sil::j1939::codec;
```

## What you write and what the stack handles

| You write | The stack handles |
|---|---|
| The node's address and NAME | Address Claim (contention, arbitrary address capability, receiving Commanded Address, the 250 ms wait) |
| The PGs to send, their periods and how to build their data | Keeping the periods, sending only while the node holds an address, stopping on DM13 |
| The PGs to receive and what to do with them | Sorting by destination, reassembling transport protocol messages (9–1785 bytes) |
| Whom to send a Request to, and what to do with the response | Waiting for the response, timeouts, retries, merging identical requests into one |
| The commands of your own protocol, and how to recognize their responses | Sending once the node can send, waiting for the response, interim responses, retries, address changes of the node and of the peer, transferring long commands |
| The model (plain C++) | Virtual time, steps, threads, SIL Kit participants and controllers |

## Create a node

You create nodes on a `j1939::Network`. A `Network` is the connection to one CAN network (channel), and `Ecu::j1939(network)` creates it.

`vehicle_sil::Ecu` is the interface for building an ECU application. A SIL Kit participant (`Peer`) implements it, and so does an ECU on the in-process virtual bus (`LocalBench::add_ecu()`). If you write the ECU as a function or class that takes an `Ecu&`, the same code runs on either.

Here is `install_engine()` from [examples/j1939/ecu_pair/ecu_pair.hpp](../examples/j1939/ecu_pair/ecu_pair.hpp):

```cpp
inline j1939::Node& install_engine(vehicle_sil::Ecu& ecu, EngineModel& engine)
{
    auto& node = ecu.j1939("J1939_CAN").add_node({.address = kEngineAddress, .name = kEngineName});
    ecu.every(10ms, [&engine](vehicle_sil::SimTime) { engine.tick(); });
    codec::attach_sender<EngineStatus>(node, 20ms, [&engine](EngineStatus& m) {
        m.speed.set(engine.rpm);
        m.state.set(engine.rpm > 0 ? EngineStatus::State::running : EngineStatus::State::stopped);
    });
    codec::on_message<SpeedRequest>(node, [&engine](const SpeedRequest& m) {
        if (const auto rpm = m.speed.value()) // not when the cab sends "not available"
        {
            engine.requested = static_cast<std::uint16_t>(*rpm);
        }
    });
    const std::string software[] = {"ENGINE", "1.0.3"};
    node.attach<j1939::Identification>().set_software_identification(software);
    return node;
}
```

| Line | What it does |
|---|---|
| `add_node()` | Creates the node (the stack performs the Address Claim). |
| `ecu.every()` | Advances the model every 10 ms. |
| `codec::attach_sender` | Sends `EngineStatus` every 20 ms. |
| `codec::on_message` | Receives `SpeedRequest` and passes it to the model. |

`EngineStatus` and `SpeedRequest` are typed messages generated from a YAML definition (see [j1939-codec.md](j1939-codec.md)). You read and write their signals as values, without writing bit positions or scaling. You can also work with the bytes directly (see the sections below).

Nodes and the objects an ECU owns follow these rules:

| Item | Rule |
|---|---|
| A node with `NodeOptions::name` | Claims its address in the first step. |
| A node without a NAME | Uses the given address at once (for tests). |
| Protocol parts attached with `node.attach<T>(...)` | The `Network` owns them and destroys them in the correct order. |
| Model and application objects that handlers refer to | Let the ECU own them with `ecu.emplace<T>(args...)` (`emplace` works only before the run starts). |
| Destruction of `emplace`d objects | The ECU destroys them after the networks, the last one emplaced first, so handlers and protocol parts may still refer to them during destruction. |
| Thread that calls protocol parts and handlers | The thread that runs the step (to work from other threads, see "Pass data in from outside a step" under "Choose a periodic mechanism"). |

Within one step, the ECU does the following, in this order:

1. Receiving and processing on the networks
2. Applying the variable writes that are due
3. Calling `on_step`, `every` and `at` (in the order they were added)
4. Processing the LIN nodes (in the order they were added)
5. Recording variables
6. Transmitting on the networks

These calls have limits:

| Call | Limit |
|---|---|
| `j1939()` with the same network name | Only once per ECU. |
| `j1939()`, `on_step()`, `every()`, `at()`, `emplace()` after the start | Throw `std::logic_error`. |
| Adding nodes | Follows the rules of `Network::add_node()`; also possible from within a step. |

A program that creates a `j1939::Network` itself (one that does not use `Ecu`) can change a node's address and part of its NAME (identity number, function instance, ECU instance) with `NetworkOptions::node_overrides`, without changing the node's code. It is the way to put 2 copies of the same node code on one bus. A line picks a node by the address in the code (`from`; without it, the only node of that network). `add_node()` creates the picked node with the changed values: `Node::address()`, `requested_address()` and `name()` return the changed values, and only `Node::coded_address()` returns the address in the code. The lines apply only to nodes added before the first `begin()` (`Network::close_node_overrides()` also closes them and returns the numbers of the lines that were not used); nodes added later keep the values of the code. A line that cannot be applied (a NAME field for a node without a NAME, for example), and a line that would apply to a second node, make `add_node()` throw `std::invalid_argument`. A network created with `Ecu::j1939()` does not accept `node_overrides` (`std::invalid_argument`): the program that runs the ECU changes the values of the ECU's nodes from outside (`SilKitOptions::j1939_nodes` and `PeerOptions::j1939_nodes` under "Run in a simulation" below).

### ECUs with several networks

A gateway calls `j1939()` with different network names. When a handler of one network sends on the other, the message goes out in the same step.

Here is `Gateway` from [examples/vehicle/main.cpp](../examples/vehicle/main.cpp) (2 CAN networks and 1 LIN network):

```cpp
class Gateway // one ECU on two networks: what it receives on one it sends on the other
{
public:
    explicit Gateway(vehicle_sil::Ecu& ecu)
    {
        auto& to_body = ecu.j1939("Body").add_node({.address = 0x31});
        ecu.j1939("Powertrain").add_node({.address = 0x30}).on_message(kSpeed, [&to_body](const j1939::Message& m) {
            static_cast<void>(to_body.send(kSpeed, m.data));
        });
    }
};
```

### Protocols other than J1939 (raw CAN)

`ecu.can(network)` returns a raw CAN channel (`can::RawChannel`) that bypasses J1939. Receive with `on_frame(handler)` and send with `send(frame)`. The channel has no timers and no segmentation, so write periodic processing with `every()` or `on_step()`.

For code that uses `ecu.can()`, see `Engine` and `Dashboard` in `tests/simulation/test_disconnect.cpp`.

| Item | Rule |
|---|---|
| Where you can call `send()` | Only within a step (calling it from a constructor throws `std::logic_error`). |
| Result of `send()` | A `RawSendStatus` (`Ok`, `QueueFull`, `InvalidFrame`). |
| Transmission time | Frames are sent at the end of the step, with the step's time. |
| Order of frames received from different ECUs at the same time | Not defined on SIL Kit. Do not write processing that depends on it. |

One ECU can call both `j1939()` and `can()` with the same network name. They are separate controllers, so each receives the other's frames (the J1939 network ignores 11-bit frames).

The exception is a `Peer` whose J1939 network runs on a user port (`j1939(name, port)`, such as Vector XL). The two are then on different buses and do not receive each other's frames, because raw CAN always runs on SIL Kit.

## Receive messages

A fragment (`node` is the node, `engine` the model). For the complete `codec::on_message`, see `install_engine()` above; for `Node::on_message`, see `Gateway` above.

```cpp
codec::on_message<SpeedRequest>(node, [&](const SpeedRequest& m) { engine.target_rpm = m.speed.value_or(800.0); });
node.on_message(kPgnRaw, [&](const j1939::Message& m) { log(m.data); });   // as bytes
```

| Item | Rule |
|---|---|
| Messages received | Those addressed to the node's own address and those sent to the global address. |
| Messages of 9 bytes or more | Received once, reassembled by the transport protocol. |
| `codec::on_message<M>` | Passes only the messages that decode (long enough, with a matching selector). |
| `Message::data` | Valid only during the call. Copy it to keep it. |
| Registrations that you need to remove | Let a `j1939::Subscription` hold them (destroying it removes them). |
| You need only the latest value and loss detection | Use `Monitor` (see "Monitor reception"). |

For a reassembled message, `Message::time` is the time the transfer ended (the time of the last packet), not the time the sender started sending. To check the sender's period from reception times, use a single-frame message.

## Send messages

| How | Use |
|---|---|
| Once, right now | `node.send(pgn, data, destination)` |
| Once, at a set time | `send()` inside `ecu.at(time, fn)` |
| Periodically | `PeriodicSender` |

### Send once

`node.send()` works only within a step, that is, in application code such as handlers, timers, `Ecu::every`, `Ecu::on_step` and `Ecu::at`. It returns the result as a `SendStatus` (not as an exception).

You cannot call `send()`, or a sending function of a protocol part (such as `subscribe()` and `unsubscribe()` in `examples/pid_stream`), directly from `main`, because that is outside a step.

### Send once at a set time

Write the ECU's scripted one-time actions (send a request, start a subscription, stop it) with `ecu.at(time, fn)`.

| Item | Rule |
|---|---|
| Step in which `fn` is called | Called once, in the first step whose start time is at or after `time` (also when the step size does not divide `time`). |
| The run does not reach `time` | Not called. |
| When you can register it | Only before the first run. |

Here is the constructor of `Tool` from [examples/pid_stream/main.cpp](../examples/pid_stream/main.cpp):

```cpp
        auto& node = ecu.j1939("J1939_CAN").add_node({.address = 0x80});
        auto& client = node.attach<pid_stream::PidClient>(format);
        auto record = [this](const pid_stream::Reply& r) { replies_.push_back(r); };
        // The tool's script. Each action runs once, in the first step at or after its time (also
        // with a step size that does not divide it), from the step: subscribe() sends.
        ecu.at(SimTime{} + 100ms, [&client, record](SimTime) {
            client.subscribe(0x00, {kRpm, kCoolant, kLoad}, 50ms, record); // 11 bytes: transport protocol
        });
        ecu.at(SimTime{} + 300ms, [&client, record](SimTime) {
            client.subscribe(0x00, {kRpm, 0x7E}, 100ms, record); // renew: new period, 0x7E is unknown
        });
        ecu.at(SimTime{} + 600ms, [&client](SimTime) { client.unsubscribe(); });
```

### Send periodically (`PeriodicSender`)

A fragment (`node` is the node; `fill`, `encoder` and `bytes` are yours). For the complete `codec::attach_sender`, see `install_engine()` above. `tests/j1939/test_j1939_periodic_sender.cpp` tests changing the period and the destination, and stopping transmission.

```cpp
codec::attach_sender<EngineStatus>(node, 20ms, fill);                                   // fills a new EngineStatus with fill for each transmission
codec::send(node, request, 0x80);                                                        // a typed message, once
auto& status = node.attach<j1939::PeriodicSender>(kPgnEngineStatus, 20ms, encoder);   // calls encoder for each transmission
auto& fixed = node.attach<j1939::PeriodicSender>(kPgnVersion, 1s, std::span(bytes)); // fixed bytes
fixed.set_data(new_bytes);                                                              // from the next transmission
status.trigger();                                                                       // sends at the next opportunity
status.set_period(50ms);                                                                // restarts the count from now
```

There are 3 ways to build the data:

| Way | Details |
|---|---|
| `codec::attach_sender` | Sends with the priority and length of the definition. |
| `Encoder` (a function that takes a span) | Writes into the part's buffer (`max_length` bytes, default 8) and returns the length written. |
| `PeriodicSender::VectorEncoder` (a function that appends to a `std::vector<std::uint8_t>&`) | Builds messages whose length changes from one transmission to the next. |

Messages of 9 bytes or more (raise `max_length` for them) are sent with the transport protocol. To send a PDU1 PG to one address, use `PeriodicSenderOptions{.destination = address}`.

`VectorEncoder` follows these rules:

| Item | Rule |
|---|---|
| The vector passed in | Empty. Its size becomes the message length. |
| Memory allocation | The part reuses the vector, so if you only append, nothing is allocated per transmission. |
| Parameter type | Write it explicitly (`std::vector<std::uint8_t>&` or `auto&`). |
| A function that takes a span | Treated as an `Encoder` that returns the length. |
| Exceeding `max_length` | The `Network` fails, as with the span form. |

A fragment (`ids`, `value_of` and `client` are yours). `PidStreamer` in [examples/pid_stream/pid_stream.hpp](../examples/pid_stream/pid_stream.hpp) has code of the same form.

```cpp
auto& reply = node.attach<j1939::PeriodicSender>(kPgnReply, 100ms, [&](std::vector<std::uint8_t>& out) {
    for (const auto id : ids) { out.push_back(id); out.push_back(value_of(id)); }
}, j1939::PeriodicSenderOptions{.destination = client, .max_length = 64});
```

You can change the period, the destination and the priority later:

| Function | Takes effect |
|---|---|
| `set_period()` | Restarts counting with the new period from the call (to send at once, call `trigger()` too). |
| `set_destination()`, `set_priority()` | From the next transmission. The starting point of the period does not change. |
| `trigger()` | Called from application code (such as `Ecu::every`), it sends in the next step. |

- If you change them while the sender is paused, it uses the new values when it resumes.
- An invalid value throws `std::invalid_argument` and changes nothing.
- To answer a new request from the same peer, change the period instead of re-creating the part (`examples/pid_stream`).
- To send in the step of the call, use `node.send()` instead of `trigger()`.

The node's address state decides when sending starts and stops. The sender sends nothing until the node can send (this includes the 250 ms wait after the Address Claim). As soon as the node can send, it sends once and then starts counting the period. It stops when the node loses its address, and resumes when the node claims an address again.

While broadcasts are stopped by DM13 (another node's DM13, or the node's own `BroadcastSuppression::suspend()`), the behavior depends on the destination:

| Transmission | While stopped by DM13 |
|---|---|
| Broadcast to the global address | Not sent. |
| To one address | Not stopped by default, because its treatment in the standard has not been verified. |

By default, the current destination decides whether a transmission stops (calling `set_destination()` decides again). To change a sender created for one address to the global address, attach `BroadcastSuppression::of(node)` during setup; without it, the change throws `std::logic_error`. For either kind of transmission, you can fix the behavior with `PeriodicSenderOptions::suppressible` (true or false).

## Respond to a Request

A fragment (`node` is the node; `encode(model)` is yours). `tests/j1939/test_j1939_request.cpp` tests `on_request` and `respond()`.

```cpp
node.on_request(kPgnSomething, [&](j1939::RequestContext& r) { r.respond(encode(model)); });
```

| Case | Behavior, and how to write it |
|---|---|
| A Request to a specific destination for a PGN with no responder | The stack returns a NACK. |
| Identification, such as the software ID | The `Identification` part responds (`node.attach<j1939::Identification>().set_software_identification(fields)`). |
| Diagnostics (DM1 and others) | The `Diagnostics` part responds. |
| Responding over several steps | Keep the request as a value with `r.defer()`, and call `node.respond(pending, data)` later. |

## Send a Request and receive the response

Here is part of `install_cab()` from [examples/j1939/ecu_pair/ecu_pair.hpp](../examples/j1939/ecu_pair/ecu_pair.hpp) (`cab` is the node, `dash` the dashboard model):

```cpp
    j1939::Requester::of(cab).request(kPgnSoftwareId, kEngineAddress, [&dash](const j1939::RequestResult& r) {
        if (r.outcome == j1939::RequestOutcome::Response)
        {
            dash.engine_software.assign(r.data.begin(), r.data.end());
        }
    });
```

| `RequestOutcome` | Meaning |
|---|---|
| `Response` | The requested PG arrived (`data`, `source`). The transport protocol reassembles a long PG. |
| `Acknowledged` | The peer returned an Acknowledgement (`ack`: positive, negative, access denied or cannot respond). |
| `Timeout` | No response arrived, even after the retries (for a global request: the waiting time ended). |
| `NotSent` | The request could not be sent (`send_status`; `NoAddress` if the node cannot claim an address). |

| Item | Rule |
|---|---|
| When you can call `request()` | Any time (it waits until the node has an address before sending). |
| A request to the global address (`kGlobalAddress`) | Each response received during the waiting time arrives with `last == false`, and `Timeout` arrives at the end. |
| The same PG requested again from the same peer | One Request is sent, and every caller receives the same response. |
| Waiting time, number of retries, minimum interval | Set them in `RequesterOptions`. |

Pass `RequesterOptions` with `j1939::Requester::attach(node, options)`. Call it before anything uses `of()` (a node has one `Requester`).

## Monitor reception

A fragment (`cab` is the node, `dash` the dashboard model). For the complete code that attaches the `Monitor`, see `install_cab()` in [examples/j1939/ecu_pair/ecu_pair.hpp](../examples/j1939/ecu_pair/ecu_pair.hpp). `tests/j1939/test_j1939_codec_node.cpp` tests reading `Monitor::data()` as a type.

```cpp
auto& eec = cab.attach<j1939::Monitor>(kPgnEngineStatus,
    j1939::MonitorOptions{.name = kEngineName, .timeout = 100ms,
                          .on_state = [&](const j1939::MonitorChange& c) { dash.silent = c.state == j1939::MonitorState::Lost; }});
...
if (const auto status = EngineStatus::decode(eec.data())) { use(status->speed.value()); }   // empty until something is received
```

| `MonitorState` | When |
|---|---|
| `Waiting` | Until the first reception (this is not counted as loss) |
| `Receiving` | While messages arrive, and when they arrive again after a loss |
| `Lost` | When `timeout` has passed since the last reception |

- If you identify the sender by NAME, the monitor follows it when its address changes.
- If you pass a function to `MonitorOptions::accept`, only the messages it accepts count as the latest value and as receptions.
- Messages that it does not accept are counted in `rejected()` and are not used for loss detection either.

Here `accept` accepts only one type. Checks of sequence numbers or check values take the same form.

```cpp
.accept = [](const j1939::Message& m) { return j1939::codec::decode<EngineStatus>(m).has_value(); }
```

## Read other nodes' DTCs (DM1, DM2)

A fragment (`node` is the node; `dash` and `log` are yours). `tests/j1939/test_j1939_diagnostics.cpp` tests `parse_dtc_list()`.

```cpp
node.on_message(j1939::kPgnDm1, [&](const j1939::Message& m) {
    const auto list = j1939::parse_dtc_list(m.data);   // no allocation; only points into m.data
    if (!list.valid()) { return; }                        // the length does not match
    dash.amber = list.lamp(j1939::Lamp::AmberWarning).has_value();
    dash.low_oil = list.has(0x7F0, 1);
    for (const j1939::ReadDtc& dtc : list) { log(dtc.spn, dtc.fmi, dtc.occurrences); }
});
```

`parse_dtc_list()` (`j1939/diagnostics.hpp`, CMake target `vehicle_sil::j1939_diagnostics`) is the inverse of the `Diagnostics` part's encoding: it reads 2 lamp bytes, followed by 4 bytes per DTC (diagnostic trouble code: SPN, FMI, occurrence count). It reads bytes of any length safely.

| Bytes passed | How they are read |
|---|---|
| 8 bytes or less | One DTC (the rest is padding). |
| More than 8 bytes | Unless the length is 2 + 4 × count, `valid()` is false and `size()` is 0. |
| A single DTC of 4 zero bytes (a list with no DTCs) | `valid()` is true and `size()` is 0. |

| Item | How to read it |
|---|---|
| Return value | A view (no allocation; it only points into the bytes you passed). |
| Lifetime | Only while the bytes you passed are valid (in a handler, only during the call). |
| To keep entries | Copy the `ReadDtc` values. |
| `lamp(Lamp)` | Returns the flash rate (`LampFlash`) if the lamp is on, and empty if it is off. |
| Lamp values that `Diagnostics` does not send | A status value reads as off, a flash value as on (`Solid`). |
| You need another interpretation | Read the raw bytes from `data()`. |
| `ReadDtc::raw` | The 4 bytes of the entry, raw. |
| `ReadDtc::occurrences` | The low 7 bits of byte 4. |
| `ReadDtc::byte4_high_bit` | The most significant bit of byte 4, as is, without interpretation. |

The meaning of the most significant bit of byte 4 has not been verified against the standard, so you decide how to interpret it. `Diagnostics` sends it as 0.

## Change another node's address

A fragment (`service` is the node). `tests/j1939/test_j1939_monitor.cpp` tests `AddressCommander`.

```cpp
auto& tool = service.attach<j1939::AddressCommander>();
tool.command(kCabName, 0x81, [](const j1939::CommandResult& r) { /* r.outcome == CommandOutcome::Moved */ });
```

When the peer sends Address Claimed with the new address, the result is `Moved`. A peer that is not arbitrary address capable does not comply (`Timeout`).

## Choose a periodic mechanism

### Run on a period or at a set time

| Use | Suited to | Timing |
|---|---|---|
| `PeriodicSender` | Sending a PG periodically | Deadlines stay on the virtual-time grid without drift; each message is sent in the first step after its deadline (one timer per PG) |
| `Ecu::every(period, fn)` | Model computation (`tick()`), periodic application processing | The first step at or after 0, period, 2 * period, … (at most once per step) |
| `Ecu::on_step(fn)` | Processing in every step | The time of the step (after reception processing, before transmission) |
| `Ecu::at(time, fn)` | The ECU's scripted one-time actions (send a request, start a subscription, stop it) | Once, in the first step whose start time is at or after `time` (`time` need not be a multiple of the step size) |
| `TickScheduler` | Existing fixed-period models (a DLL's `tick()`). Used outside `Ecu`, in the step handler of `ParticipantRuntime` | Only periods that are multiples of the step size (any other period throws) |
| `j1939::Timer` | Time inside a protocol part (waiting times, delays, custom periods) | Counted from the time of the event (independent of the step size) |

The period of `Ecu::every` need not be a multiple of the step size. With a step size of 10 ms and a period of 25 ms, `fn` is called at 0, 30, 50, 80, 100 ms, …

Write the model's period and the messages' periods separately. Advance the model with `every`, and build each PG to send from the model's current values, in the `Encoder` of a `PeriodicSender`.

Keep the step size at or below the shortest message period; the number of transmissions then does not depend on the step size. If the step size is longer, the deadlines that fall within one step are merged into one transmission.

### Pass data in from outside a step

| Use | Suited to | Determinism |
|---|---|---|
| `StepInbox` | Data received on other threads or in SIL Kit handlers, to be taken out in a step | Deterministic only if you follow the 2 rules below |
| Control Agent writes | Variable writes from the Control API | Deterministic if you give a virtual time (`Command::at`); without one, applied in the next step |
| `Host::post(fn)` (`LocalBench::host(ecu)`, `Peer::host()`) | Operations from other threads, or from outside a step | No time; the step in which `fn` runs depends on when it was posted |

Put items into a `StepInbox` with a time. In each step, the items with times before that step are taken out in order of time, tie-break and arrival. The result is deterministic only if you follow these 2 rules:

| Rule | Reason |
|---|---|
| Choose a tie-break that tells the sources apart | Otherwise, the order of items from different sources with the same time is not defined. |
| Size the capacity for the worst case, and treat overflow as an error | Overflow near the limit is not deterministic. |

Pass data that must be deterministic through a `StepInbox`, not through `Host::post`.

## Handle errors

| What happens | How it shows |
|---|---|
| A send fails | The `SendStatus` return value (`QueueFull`, `Busy`, `NoAddress` and others) |
| Usage error (invalid PGN, out-of-range value) | An exception such as `std::invalid_argument`, when the part is created or the function is called |
| Not enough timer slots | Creating the part throws `std::length_error` (the message names `timer_capacity`) |
| An exception thrown from a handler, an `Encoder` or a callback | The `Network` enters the failed state |

- Protocol parts count send failures (`statistics()`) and send again at the next opportunity.
- With `Simulation`, the failed state becomes a `SimulationError` (see "Run in a simulation").
- When you use `Peer` directly, `Peer::failed()` and `error()` report the failure.
- To see the state of communication, look at `Network::statistics()`, the parts' `statistics()` and the state of each `Monitor`.

### Timer slots (`timer_capacity`)

`NetworkOptions::timer_capacity` sets the number of timer slots (default 64). For a node with many periodic messages, raise it with `Ecu::j1939(network, NetworkOptions{.timer_capacity = ...})` (for `Peer`, `J1939ChannelOptions::network.timer_capacity`). For the number of timers each protocol part uses, see the comments in the part's header and "Protocol parts" in [internals/j1939.md](internals/j1939.md#protocol-parts-libsj1939parts).

If a handler creates a protocol part (`PeriodicSender`, `Timer` and others) for each request it receives, check the free slots first. Compare `node.network().timers_free()` (the number of free slots) with the `kTimers` of the part (such as `PeriodicSender::kTimers`), and refuse the request if there are not enough.

A `std::length_error` inside a handler makes the `Network` fail. So without this check, requests sent from many addresses are enough to stop that ECU's communication. For code that does the check, see `PidStreamer` in [examples/pid_stream/pid_stream.hpp](../examples/pid_stream/pid_stream.hpp).

## Implement your own protocol

You can implement Proprietary A/B and manufacturer-specific protocols as your own protocol parts, without changing vehicle-sil. They use only the node's public API, the toolbox (`Subscription`, `Timer`, `Node::attach`), the existing protocol parts and typed messages.

| Example | Contents |
|---|---|
| [examples/pid_stream](../examples/pid_stream) | The first example to follow (a part that receives a request and replies periodically). |
| [examples/j1939/param_service](../examples/j1939/param_service) | The complete example (a service tool reads and writes the parameters and blocks of an engine ECU over Proprietary A). |

`param_service` covers opening and closing a session, interim responses during a write, blocks sent with the transport protocol, error responses, closing a session after inactivity and loss of the peer. Its test is `tests/testkit/test_param_service.cpp`.

### Define the messages

Define fixed-length messages in YAML (see [j1939-codec.md](j1939-codec.md)). To carry several messages on one PGN, make the operation byte the `selector`.

Here is one entry of `messages` in [examples/j1939/param_service/param_service_messages.yaml](../examples/j1939/param_service/param_service_messages.yaml):

```yaml
  - name: Read
    proprietary_a: {}
    selector: {start_bit: 0, length: 8, value: 0x11}
    signals:
      - {name: session, start_bit: 8, length: 8}
      - {name: parameter, start_bit: 16, length: 16}
```

Handle variable-length data (such as the contents of a block) as bytes, with `node.send()` and `Message::data`. You can mix typed messages and bytes on the same PGN (a type's `decode()` returns empty for a different selector).

### The requesting side (`Exchange`)

`j1939::Exchange` sends a command to one peer and waits for the response (CMake target `vehicle_sil::j1939_exchange`).

Here are `ParamClient::run()` and `answer_to()` from [examples/j1939/param_service/param_service.cpp](../examples/j1939/param_service/param_service.cpp), shortened to reads only. The repository does not contain the code in this exact form.

```cpp
j1939::Exchange exchange(node, {.timeout = 200ms, .retries = 1});
messages::Read read;
read.session.set_raw(session);
read.parameter.set_raw(0x0100);
exchange.start(kPgn, read.bytes(), kEngineName,          // the peer by NAME (an address works too)
    [](const j1939::Message& m) {                         // what the received message is (checks the PGN too)
        if (const auto ok = j1939::codec::decode<messages::ReadOk>(m); ok && ok->parameter.raw() == 0x0100) {
            return j1939::ReplyMatch::Final;              // the response to this read
        }
        if (const auto e = j1939::codec::decode<messages::Error>(m); e && e->request.raw() == kRead) {
            return j1939::ReplyMatch::Final;              // an error for this read
        }
        if (const auto p = j1939::codec::decode<messages::Pending>(m); p && p->request.raw() == kRead) {
            return j1939::ReplyMatch::Pending;
        }
        return j1939::ReplyMatch::Ignore;
    },
    [](const j1939::ExchangeResult& r) {                  // called exactly once
        if (r.outcome == j1939::ExchangeOutcome::Reply) { use(j1939::codec::decode<messages::ReadOk>(r.reply)); }
    },
    {.on_address_change = j1939::AddressChangePolicy::SendAgain}); // a read may run twice
```

| `ExchangeOutcome` | Meaning |
|---|---|
| `Reply` | `match` returned `Final` (`reply`). |
| `Timeout` | No response arrived, even after the retries (after a `Pending`, there are no retries, and `timeout` counts from the last `Pending`). |
| `NotSent` | The command did not reach the peer (table below). |
| `Interrupted` | The command was sent, but the response can no longer be received (table below). Whether the peer executed it is unknown. |
| `PeerLost` | The peer is gone (table below). |

| `ExchangeOutcome` | When |
|---|---|
| `NotSent` | The command could never be sent (`send_status`; `NoAddress` if the node can no longer claim an address). |
| `NotSent` | The transfer of a long command ended before the last packet, or the peer aborted it (`transfer`, `reason`). |
| `Interrupted` | The node's own address changed or was lost. |
| `Interrupted` | After the last packet of a long command, this node aborted the transfer on a timeout. |
| `Interrupted` | A retry could not be sent. |
| `PeerLost` | The peer lost its address, moved, or had its address taken by another NAME. |
| `PeerLost` | A peer given by NAME did not claim an address within `timeout`. |

#### Match responses (`match`)

`match` receives every message from the peer's address, whatever its PGN (messages to this node and to the global address, including the peer's periodic broadcasts). `M::decode(data)` looks only at the bytes, so use `j1939::codec::decode<M>(message)`, which also checks the PGN.

To tell whether a message is the response to your command, look at the operation byte, the session and the item the command refers to (such as the parameter number).

When `match` returns `Pending` (an interim response), the waiting time restarts and retries stop.

Late responses to earlier commands (to the original of a retried command, or to a command you cancelled with `cancel()`) also reach `match`. In a protocol whose responses carry no item or number, such a response is mistaken for the response to the next exchange. If responses can arrive late, give commands and responses an item or a sequence number, and match on it.

`cancel()` does not take back a command already sent (its response still arrives).

#### Per-command options (`CommandOptions`)

`CommandOptions` is the last argument of `start()`. Items you leave out take the values set for the whole `Exchange` (`ExchangeOptions`).

| Item | Meaning |
|---|---|
| `priority` | Priority (default 6) |
| `timeout` | Waiting time for this command's response (make it longer only for slow commands, such as an erase) |
| `retries` | Number of retries after a timeout (set 0 for writes and erases) |
| `long_answer` | PGN of a response that arrives over the transport protocol (see "Long commands and long responses" below) |
| `on_address_change` | What to do if the node's own address changes after sending: `Stop` (default; ends with `Interrupted`) or `SendAgain` (sends again from the new address) |

The waiting time counts from the step in which the command was sent. It restarts:

- On a retry (from the step that sent the retry)
- When the transfer of a long command ends
- When a `Pending` arrives (from that time)
- When the transfer of the `long_answer` ends (from that time)

#### Retries

There are 2 kinds of retry, configured separately.

| Retry | Setting | Default | Commands to use it for |
|---|---|---|---|
| After a timeout | `retries` | 0 | Only protocols where receiving the same command twice does no harm (reads, writes of the same value, numbered commands) |
| When the node's own address changes | `on_address_change` (not counted in `retries`) | No retry; `Interrupted` | Set `SendAgain` only for commands that may run twice |

For writes and erases, leave `on_address_change` at its default. When you receive `Interrupted`, let your procedure decide what to do, for example by reading the value back.

To the peer, a retry with `SendAgain` comes from a different source address. So in a protocol where the peer keeps a session, the peer may respond "no session" (in the `param_service` example, the tool then reopens the session).

#### Long commands and long responses

| Case | Behavior |
|---|---|
| A command of 9 bytes or more (raise `max_length`) | Sent with the transport protocol. The waiting time starts after the transfer ends. |
| A long response whose PGN you give in `long_answer` | Does not time out while it is being received. |
| A long response without `long_answer` | Its transfer time counts toward `timeout`. |

Use `long_answer` especially for BAM responses (packets at least 50 ms apart). It covers only transfers of that PGN that the peer sends to this node or to the global address (an unrelated broadcast does not hold the timeout).

The timeout can be held for at most `ExchangeOptions::answer_transfer_limit` per attempt (default 60 s). So the wait always ends, even for a transfer that never finishes or keeps starting again.

#### The peer's address and NAME

| Case | Behavior |
|---|---|
| When you can call `start()` | Any time (also outside a step, and before the node has an address). The command is sent once the node can send. |
| A peer given by NAME has not claimed an address yet (right after startup) | Waits for the peer's Address Claimed, then sends. |
| A peer given by NAME moves during the wait | Ends with `PeerLost`, even if retries remain (the peer may have lost the state of the exchange). |

After the peer has moved, the next exchange goes to its new address.

#### Out of scope, and limits

`Exchange` handles only exchanges with one peer. It does not handle:

- Queries to all nodes that collect several responses (use a global request of `Requester`)
- Responses that come from an address other than the peer's (such as through a gateway)

| Limit | Value |
|---|---|
| Exchanges running at the same time | `capacity` (default 4) |
| Timers | 2 per `Exchange` |

From inside the callbacks and `match`, you can start the next command with `start()`, call `cancel()` and destroy the `Exchange`. After the `Exchange` is destroyed, the callbacks of its remaining exchanges are not called.

### Write a sequence (several messages)

Chain a sequence by calling `start()` for the next command in the callback of the previous response. For sequences where the previous response decides the next command, such as sessions, and for sequences that run operations one at a time, your protocol part keeps a queue of operations.

The example `ParamClient` ([examples/j1939/param_service/param_service.cpp](../examples/j1939/param_service/param_service.cpp)) does the following:

| Situation | What `ParamClient` does |
|---|---|
| No session | Opens one, then performs the operation. |
| The response is "no session" | Reopens the session once. |
| Read, Open, Close | Uses `SendAgain`. |
| Write | Keeps the default `Stop`. If the address changes, it returns `Interrupted` to its user. |
| Block read | Gives `long_answer`. |

Callbacks to the part's users follow 2 rules:

- Settle the part's own state before calling a user's callback.
- Do not call a user's callback from inside that user's call, such as `read()` (the caller's state may be half-updated).

The example starts the next operation before it calls the callback of the previous one. That makes it safe to destroy the part inside the callback.

### The responding side

Write the responding side with `on_message` and the node's `send()` (there is no dedicated part).

Here are the constructor of `ParamServer` and `ParamServer::on_command()` from [examples/j1939/param_service/param_service.cpp](../examples/j1939/param_service/param_service.cpp), shortened. The repository does not contain the code in this exact form.

```cpp
ParamServer(j1939::Node& node, ParameterTable& table)
  : node_(node), table_(table),
    commands_(node, node.on_message(kPgn, [this](const j1939::Message& m) { on_command(m); })),
    claims_(node, node.on_message(j1939::kPgnAddressClaimed, [this](const j1939::Message&) { on_claim(); })),
    idle_(node, j1939::TimerKind::Timeout, [this] { end_session(); }) {}

void on_command(const j1939::Message& m) {
    if (m.destination != node_.address()) { return; }              // ignores Proprietary A sent to the global address
    ...
    j1939::codec::send(node_, reply, m.source);                    // the response goes to the sender's address
    idle_.start_after(2s);                                          // counts from the time of the received message
}
```

| Situation | How |
|---|---|
| A response of 9 bytes or more | Send it with `node.send(pgn, data, to, priority, j1939::WhenBusy::Queue)` (it waits for the previous transfer to the same peer to end). |
| Slow work (such as writing to flash) | Respond later, with a `Timer`. |
| The node's own address changed (`on_address_change`) | The peer sent its commands to the old address, so end the session. |
| A peer whose NAME is unknown | End the session on an inactivity timeout. |

If the responder periodically sends an interim response such as `Pending` during slow work, the requester's `Exchange` keeps waiting.

Detect the loss of the peer as follows (this also covers Cannot Claim, and the address being taken by another NAME):

1. When the session opens, record the peer's NAME with `node.network().name_at(m.source)`.
2. Each time Address Claimed arrives, check that `node.network().address_of(name)` is still the same address.

### Common rules

| Item | Rule |
|---|---|
| Time | Count it with `Timer` (virtual time), not with threads, event loops or the OS clock. |
| Removal | Let a `Subscription` hold each registration, a `Timer` each timer and an `Exchange` each request. Destroying the part then removes them automatically. |
| Owner of a protocol part | Let the `Network` own the part with `attach`, or destroy the part before the `Network`. |
| Creating a part inside a handler | Check the slots with `timers_free()` first (see "Handle errors"). |
| The standard Request (PGN 59904) | Use `Requester::of(node)` (do not rebuild it with `Exchange`). |
| Loops that call the part's users | Settle the part's own state before notifying, and do not handle items added during a notification in the same loop (see "Loops that notify the user" under "Toolbox" in [internals/j1939.md](internals/j1939.md#loops-that-notify-the-user)). |
| `on_any_request` and `Network::set_observer` | Do not use them. |

You may destroy a part that the application owns (in a `std::unique_ptr` or similar) inside a handler or a timer callback, for example to stop when the peer's Stop arrives. The handlers of a destroyed part are not called, even in the rest of the same delivery, and its timers do not fire.

You may also destroy a part inside the functions that it calls (the `Encoder` of `PeriodicSender`, the callbacks of `Monitor` and `Exchange`). The exceptions are the callback of `AddressCommander` and `on_clear` of `Diagnostics`.

### Test your protocol

Besides the normal cases, the example's test (`tests/testkit/test_param_service.cpp`) checks the following. All of it runs in virtual time, and the same input produces the same frames.

| To cause | How |
|---|---|
| No response from the peer | `bus.endpoint("engine").add_outage(from, until)` |
| Loss and moves of the peer's address or the node's own | Inject an Address Claimed with a higher-priority NAME with `bus.inject()` |
| Closing a session after inactivity | Wait with `bus.run_for()` |
| A second tool | Add another network to the same bus with `add_j1939()` |
| Destroying the part (during processing, inside a callback) | Hold the part in a `std::unique_ptr` and `reset()` it midway |

To read the time inside a callback, use the network's `now()` (the time of the step). `LocalBench::now()` is the end time of the last `run_until()`.

## Run in a simulation

`vehicle_sil::Simulation` (CMake target `vehicle_sil::simulation`) runs the ECUs. The constructor decides where they run; the ECU code stays the same. To choose at run time, pass a `std::optional<SilKitOptions>` (empty means the in-process virtual bus).

Here is `main()` from [examples/j1939/ecu_pair/main.cpp](../examples/j1939/ecu_pair/main.cpp), shortened by removing the argument handling and the output. For an example that builds the layers by hand, see [examples/low_level](../examples/low_level).

```cpp
vehicle_sil::Simulation sim;                                   // in-process virtual bus
// vehicle_sil::Simulation sim(vehicle_sil::SilKitOptions{});  // SIL Kit participants (the registry runs in this process)
auto& engine_ecu = sim.add_ecu("Engine", 10ms);                // an ECU that advances every 10 ms
install_engine(engine_ecu, engine_ecu.emplace<EngineModel>());
auto& cab_ecu = sim.add_ecu("Cab", 5ms);
install_cab(cab_ecu, cab_ecu.emplace<Dashboard>());
sim.run_for(3s);                                               // 3 s of virtual time
```

`sim.add<App>("Engine", 10ms, args...)` is short for `add_ecu()` plus an `emplace` that lets the ECU own `App{ecu, args...}`.

| | `Simulation()` | `Simulation(SilKitOptions)` |
|---|---|---|
| ECU | A host on the virtual bus (`LocalBench`). Advances on the calling thread | A coordinated participant in virtual time (`Peer`). Advances on SIL Kit threads |
| `run_for(d)` | Can be called any number of times; each call continues from the last | Once only (a second call throws `std::logic_error`) |
| Returns | When it has advanced by `d` | After every participant has stopped |
| After an exception in a step | The next `run_for()` throws `SimulationError` too | `run_for()` ends the simulation, then throws |
| Low level | `local()`: buses (CAN and LIN), fault injection, raw frames, taps (`vehicle_sil/local/local_bench.hpp`) | `peer(name)`: taps, Control Agent, SIL Kit ports (`vehicle_sil/peer/peer.hpp`). `registry_uri()` |

`run_for(d)` runs only the steps that start before the end time (on the virtual bus, the current time plus `d`).

On SIL Kit, `run_for()` runs only once: the step at the stop time may still be running, and reading the model from the main thread would race with it. To stop, inspect and continue, use the virtual bus.

An exception in a step is thrown from `run_for()` as a `SimulationError`. Its `what()` is `<ECU name>: <reason>`. On SIL Kit, a failed participant does not end the simulation; it only stops virtual time. So when `run_for()` finds a failure, it ends the simulation, then throws.

| Item | Rule |
|---|---|
| Threads on SIL Kit | Steps run on SIL Kit threads, so read the values that steps write only after `run_for()` returns. |
| `Simulation` functions | Call them from one thread. |
| Destruction order | ECUs are destroyed in reverse order of addition, together with what they own (`emplace`), then the controller, and last the in-process registry. |
| LIN master and slave | Create them with `Ecu::lin_master(network, schedule)` and `Ecu::lin_slave(network)` (the result is the same on the virtual bus and on SIL Kit; see [internals/lin.md](internals/lin.md)). |

### SIL Kit options (`SilKitOptions`)

With `SilKitOptions`, you can use an existing registry (`registry_uri`) or run together with participants in other processes.

| Item | Meaning |
|---|---|
| `registry_uri` | Registry to connect to. If empty, a registry runs in the process (`registry_uri()` tells you where) |
| `controller` | Whether this process starts and ends the simulation (default `true`). If `false`, an outside controller (`vehicle_sil_sim_controller`, the Control Server) decides, and `run()` waits until the end |
| `other_participants` | Coordinated participants in other processes that must be present before the start (only when `controller` is `true`) |
| `start_timeout` | Time allowed until the start (default 30 s) |
| `network_names` | Network name map (simulation name to the name in the ECU's code). Applies to every ECU of this process |
| `j1939_nodes` | Changes the address and part of the NAME of J1939 nodes without changing the ECU's code (ECU name to a list of lines). See below |

`j1939_nodes` is for adding 2 copies of the same ECU code. Without it, the 2 ECUs request the same address with the same NAME, and neither of them gets an address.

```cpp
vehicle_sil::SilKitOptions options;
options.j1939_nodes["Engine2"] = {{"VehicleCAN", {.from = 0x00, .address = 0x01, .identity_number = 2}}};
vehicle_sil::Simulation sim(options);
install_engine(sim.add_ecu("Engine1", 10ms), ...);   // 0x00, as in the code
install_engine(sim.add_ecu("Engine2", 10ms), ...);   // 0x01, identity number 2
```

- A line picks a node by the network (the simulation's name; for a network renamed with `network_names`, the new name) and the node's address in the code (`from`; without it, the only node of that ECU on that network). It can change `address` (254 only for a node whose NAME is arbitrary address capable), `identity_number`, `function_instance` and `ecu_instance`.
- Unlike `network_names`, it is written per ECU: the same lines for every ECU would put 2 ECUs of the same network on the same address again.
- It applies only to nodes added before the start. Nodes added after the start keep the values of the code.
- Only that node changes. Other ECUs that have this node's address or whole NAME in their code do not change.
- `std::invalid_argument` is thrown for: an empty network name, a value out of range, 2 lines with the same network and `from`, a line without `from` together with another line for the same network (by `add_ecu()`); a line that cannot be applied (a NAME field for a node without a NAME, 254 for a NAME that is not arbitrary address capable), a line that would apply to a second node (by `add_node()`); the name of an ECU that this simulation does not have, a line that applied to no node (by `run()` and `run_for()`, which then start nothing).
- The in-process virtual bus (`Simulation()`) does not have this option.

`run()` waits until the simulation ends, whoever ends it (SIL Kit only). What counts as a failure depends on `controller`:

| `controller` | What `run()` treats as a failure |
|---|---|
| `true` | A participant that left with Error |
| `true` | A participant that left without Error and did not end the simulation within a few seconds |
| `false` | Only failures of this process's ECUs |

### Use `Peer` directly

Use `Peer` directly in these cases (see "Peer" in [internals/execution.md](internals/execution.md#peer-vehicle_silpeer), and [peers.md](peers.md)):

- To run in real time
- To run as one participant in a separate process
- To embed a Control Agent

`Peer` is also an `Ecu`, so the same functions work with it, as in `install_engine(peer, model)`. To split ECUs into separate participants, make them `Coordinated` and start them with a `SimulationController` (in one process, `start_together`). In `Peer`, write the same lines in `PeerOptions::j1939_nodes` (the list of lines of that ECU). If a line applies to no node, `Peer::start()` throws `std::invalid_argument` and starts nothing (`Peer::unused_j1939_nodes()` gives the reason for each line beforehand). With `start_together`, if any `Peer` has such a line, it starts none of them and throws with the same message as that `Peer`'s `start()`. In a program that runs steps itself through `host()` before `start()`, the lines apply only to nodes added before the first step.

## Test an ECU

Test your ECUs on the virtual bus of `Simulation`. Time is virtual, so you need no `sleep`. The same input gives the same result.

A fragment (`CHECK` is the test macro). `tests/testkit/test_ecu_personas.cpp` tests the same 2 ECUs with the same tools, and [examples/fault_injection](../examples/fault_injection) is an example of fault injection and observation.

```cpp
vehicle_sil::Simulation sim;                               // virtual bus
auto& engine_ecu = sim.add_ecu("engine", 10ms);
install_engine(engine_ecu, engine_ecu.emplace<EngineModel>());
auto& cab_ecu = sim.add_ecu("cab", 5ms);
auto& dash = cab_ecu.emplace<Dashboard>();
install_cab(cab_ecu, dash);
auto& bus = sim.local();                                   // LocalBench (vehicle_sil/local/local_bench.hpp)
testkit::J1939Events events(bus.network("cab"));           // stack events (read-only)
testkit::FrameLog wire(bus.tap("engine"));                 // bus frames (read-only)
sim.run_for(1s);                                           // advance a little at a time, checking after each
bus.endpoint("engine").add_outage(sim.now(), sim.now() + 300ms);   // disconnects the engine from the bus
sim.run_for(1s);
CHECK(bus.network("cab").node(0).find<j1939::Monitor>()->losses() == 1);
```

| Use | What you can do |
|---|---|
| `testkit::faults`, `add_outage` | Fault injection (dropped frames, delays, disconnection) |
| `inject()` | Inject raw frames |
| `Monitor` | Watch for loss of communication |
| `testkit::J1939Events` | Watch send requests, frames put on the bus, ends of transfers and address changes |
| `testkit::FrameLog` | Watch the frames on the bus |

The recording and fault injection tools (`testkit::J1939Events`, `FrameLog`, `faults`) are in `vehicle_sil::testkit`. Link it to use them (`vehicle_sil::simulation` does not link it).

`LocalBench` functions take names:

| Names passed | Rule |
|---|---|
| ECU and network | Pass both, as in `network("cab", "J1939_CAN")` (for an ECU with one network, the one-name form works too). |
| Bus | With one bus, you can leave it out, as in `inject(t, frame)`. |
| Bus (with 2 or more buses) | Pass it, as in `inject("J1939_CAN", t, frame)`. |

### Use `LocalBench` directly

`LocalBench::add_ecu(name, step)` returns an `Ecu&`. `add_j1939(name, step)` is shorthand for an ECU with one network.

The forms that take a bus name and the forms that leave it out differ as follows. Whichever order you call them in, they refer to the same bus.

| Form | Functions | Behavior |
|---|---|---|
| Name left out | `add_j1939()`, `bus()`, `injector()`, `inject(time, frame)` | Use the only bus, and create it if it does not exist yet. |
| Name given | `bus(network)`, `injector(network)`, `inject(network, time, frame)` | Only look up the bus by name; they do not give a bus its name (`std::out_of_range` if there is no bus with that name). |

A bus created by a form without a name takes the name of the `Ecu::j1939(network)` or `Ecu::can(network)` that is called later.

| Usage | Result |
|---|---|
| Leaving out the name when there are 2 or more buses | `std::logic_error` |
| Using the name of a second bus after adding an ECU with `add_j1939()` | `std::logic_error` (the bus of that ECU would be ambiguous) |

To use several buses, add the ECUs with `add_ecu(name, step).j1939(network)`, and pass the bus name, as in `inject(network, time, frame)`.

### Disconnect an ECU from the bus (`ecu.connected`)

Every ECU has a bool input, `ecu.connected`. Writing false disconnects the ECU from all buses while it keeps running. Unlike `add_outage`, it works the same with `Simulation()` (the virtual bus) and on SIL Kit, and from a scenario (see "Disconnecting an ECU from the bus" in [scenario.md](scenario.md#disconnecting-an-ecu-from-the-bus)), the Control API or `agent()`.

A fragment (`sim` is a `Simulation`). `tests/simulation/test_disconnect.cpp` tests the same writes.

```cpp
sim.ecu("engine").agent().submit({1, {{"ecu.connected", false}}, SimTime{} + 300ms});
sim.ecu("engine").agent().submit({2, {{"ecu.connected", true}}, SimTime{} + 600ms});
```

On J1939, a disconnection looks like this (for the guarantees, see "Disconnecting from the bus" under "The `Ecu` class" in [internals/execution.md](internals/execution.md#disconnecting-from-the-bus-ecuconnected)):

| Where you look | While disconnected |
|---|---|
| `send()` of the disconnected ECU | Returns `Ok`, but the frame does not go on the bus: it is counted in the `Network`'s `send_failures` and dropped. |
| Reception by the disconnected ECU | None. |
| Periodic transmissions, timers and address state of the disconnected ECU | Unchanged. |
| Transport protocol of the disconnected ECU | Ends on a timeout, because the peer's responses do not arrive. |
| Reception by the other ECUs | The disconnected ECU's messages stop arriving (detect it as message loss, for example as `Lost` of a `Monitor`). |
| Address tables of the other ECUs | Unchanged. |

Dropped frames are not kept, so they are not sent in a burst on reconnection.

Choose the behavior on reconnection with the network's `NetworkOptions::on_reconnect`. Neither option has been verified against the standard; choose the one that matches the behavior of the ECU you simulate.

| `on_reconnect` | Behavior |
|---|---|
| `j1939::OnReconnect::Announce` (default) | Each node with a NAME sends a Request for Address Claimed to the global address, and also sends its own Address Claimed. |
| `j1939::OnReconnect::Nothing` | Sends nothing (simulates an ECU that does not notice it was disconnected). |

With `Announce`, the other nodes' responses resolve conflicts with any node that took the same address during the disconnection. The node does not do the 250 ms wait after the claim. There is no option yet to restart the Address Claim from the beginning (with the wait).

A fragment (`ecu` is an `Ecu&`, `kName` a NAME). `tests/simulation/test_disconnect.cpp` tests both values of `on_reconnect`.

```cpp
j1939::NetworkOptions options;
options.on_reconnect = j1939::OnReconnect::Nothing;
auto& node = ecu.j1939("J1939_CAN", options).add_node({.address = 0x00, .name = kName});
```

### Listen-only participants

To check the communication between other ECUs in a scenario, add an ECU that sends nothing to the configuration file, and write what it receives to `output` variables (see "Read and write signals from outside"). If it sends nothing, it puts nothing on the bus.

What the other ECUs receive and send, their transmit acknowledgements, and the times of all of these are the same with or without that ECU, and do not depend on its step size (tested on the virtual bus, on SIL Kit's trivial simulation and on the simple CAN network simulator).

The repository does not contain this code. A `main()` with `ecu_main` is in [examples/ecu_main/engine/main.cpp](../examples/ecu_main/engine/main.cpp). `tests/j1939/test_j1939_listener.cpp` tests the `Listener`, and `tests/j1939/test_j1939_silkit.cpp` tests that a listen-only participant does not change the communication of the other ECUs.

```cpp
// A watcher: takes the Listener instead of creating it, and adds handlers. It keeps the registration as a member (removed when destroyed)
class StatusWatch {
public:
    StatusWatch(vehicle_sil::Ecu& ecu, j1939::Listener& listener)
      : status_(listener, listener.on_message(kPgnStatus, [this](const j1939::Message& m) {   // any destination; long messages too
            if (m.time < latest_) { return; }   // the order of delivery can vary: compare times to find the newest
            latest_ = m.time;
            ++seen_;
            to_ = m.destination;
        })) {
        ecu.output("watch.status_seen", &seen_);
        ecu.output("watch.status_to", &to_);
    }

private:
    vehicle_sil::SimTime latest_{};
    int seen_ = 0;
    int to_ = 0;
    j1939::ListenerSubscription status_;   // last member: destroyed first, so the handler is removed
};

class BusWatch {
public:
    explicit BusWatch(vehicle_sil::Ecu& ecu)
      : network_(ecu.j1939("J1939_CAN")),   // no node added: sends not a single frame
        listener_(network_),
        status_(ecu, listener_) {
        // let scenarios see what was missed, and messages that may differ from the receiver's
        ecu.output("watch.missed", [this] {
            const auto& s = listener_.sniffer_statistics();
            return static_cast<std::int64_t>(s.no_session + s.incomplete + s.replaced + s.lost + s.conflicting_packets +
                                             network_.statistics().receive_overflows);
        });
    }

private:
    j1939::Network& network_;
    j1939::Listener listener_;
    StatusWatch status_;
};

int main(int argc, char** argv) { return vehicle_sil::ecu_main<BusWatch>(argc, argv, {.name = "Watch", .step = 10ms}); }
```

| How it receives | Use | What it sees |
|---|---|---|
| On J1939 | A `Network` with no nodes (`ecu.j1939(network)`), and a `j1939::Listener` on it | The messages and frames that other participants sent, whatever their destination, with their times. |
| On raw CAN | `on_frame` of `ecu.can(network)` | Every frame (including 11-bit frames, TP.CM and TP.DT). |

- A `Network` with no nodes does no Address Claim, does not respond to Requests and sends no ACK or NACK.
- Create one `j1939::Listener` (header `vehicle_sil/j1939/listener.hpp`) on that `Network` and add handlers to it. The `Listener` only listens and sends nothing. Its handlers:
  - `on_message(pgn, handler)` and `on_any_message(handler)`: messages sent by other participants, whatever their destination: single frames, messages sent with BAM, and messages sent over a connection (RTS/CTS) between 2 ECUs. The handler takes the same `j1939::Message` type as a node's handler, so nodes and listeners can share the functions that read it. You can add several handlers for the same PGN; they are called in the order they were added.
  - `on_transfer(handler)`: how a connection between other ECUs ended (`SniffedTransfer`: completed, aborted, timed out or missed).
  - `on_frame(handler)`: one frame at a time (including TP.CM, TP.DT and frames to other addresses). Use it to reassemble messages yourself.
- What it does not hear:
  - What nodes of the same `Network` sent. A `Listener` passes "what other participants said". Count the transmissions of your own nodes with the results on the sending side (`Node::on_transfer_end`, the results of `Requester`). With `ListenerOptions::hear_local`, frames and messages looped back between nodes of the same `Network` are passed too (with `ReceivedFrame::local` set), but loopback happens only with 2 or more nodes, so on a `Network` with one node it adds nothing.
  - 11-bit frames, and frames that the `Network` did not accept (`receive_overflows` in `Network::statistics()`). To see 11-bit frames, listen on raw CAN.
  - Connections that the `Listener` could not follow. Announcements beyond the number it can follow at the same time (`ListenerOptions::sniffer.sessions`, default 32) count in `no_session`, transfers with missed packets in `incomplete`, and transfers announced again before they ended in `replaced` (`Listener::sniffer_statistics()`). Write these to a variable as in the example, and a scenario can tell "did not come" from "missed".
- Messages that may differ from the receiver's: a message of a connection between other ECUs has the same bytes as the receiving node's message when the transfer's `conflicting_packets` (packets seen with the sequence number of an earlier packet but different bytes) is 0. Otherwise the bus does not show which of the two the receiver took (the `Listener` passes the one it saw first). `Message` carries no mark for this. There are 2 ways to tell:
  - Write the statistic `sniffer_statistics().conflicting_packets` to a variable, as in the example. While it stays 0, every message passed is the same as the receiver's.
  - Use the values in the `on_message` handler, and discard the value just taken if the `on_transfer` handler, called right after for the same transfer (same PGN, source and destination), sees it `Completed` with `SniffedTransfer::conflicting_packets` not 0. Other messages (single frames, messages sent with BAM, messages addressed to a node of this `Network`) get no `on_transfer` call, and they cannot differ from the receiver's.
- A participant with its own nodes can use a `Listener` too. The `Network` handles connections addressed to its nodes and connections they send, and a message addressed to a node is passed once (it does not appear in `on_transfer`).
- Handlers only read. Sending or registering a handler from inside a handler is a `std::logic_error`, and the participant fails and stops (also if the exception is swallowed). Update variables in handlers and send from `ecu.on_step`. Register the handlers before the simulation starts (in the constructor).
- Time and order: the `Message::time` of a message sent over a connection is the time at which the receiver received the message (the last packet), the same time that the receiving node sees. The handler, however, is called after the receiver has answered that it received the message. So a message with an earlier time can be passed after a message with a later time. To find the latest value or a period, compare `time`, not the order of the calls. What follows from frames (messages, Aborts) does not depend on the step size of the listening participant. What follows from elapsed time (such as `TimedOut`) has a `time` that does not depend on the step size, but the step in which the handler is called does, so compare `time` here too.
- How to write watchers: a `Network` has one `Listener` (a second one is a `std::logic_error`). A watcher class or function takes a `Listener&` and adds handlers instead of creating its own `Listener`. Then you can put any number of watchers in one participant without rewriting them.
- Handler lifetime: an added handler is called until it is removed or the `Listener` is destroyed. What the handler captured (`this` and so on) must live that long. A watcher that can be destroyed before the `Listener` keeps the return value of the registration (`Listener::HandlerId`) in a `j1939::ListenerSubscription` member (`StatusWatch` in the example; the same shape as `j1939::Subscription` for a node's handlers). A `ListenerSubscription` removes the handler when destroyed or on `reset()`. It works within a step and from inside that handler itself, and the handler is not called after that. Destroying it after the `Listener` is safe. A handler whose return value was discarded stays registered until the `Listener` is destroyed.
- The rules for following connections follow the receiving side of this repository's engine and were not compared with the standard. Values that can differ in other stacks (timeouts, when a transfer counts as complete, and so on) can be changed with `ListenerOptions::sniffer` (see "Hearing what other participants say (`Listener`)" and "Following other ECUs' transfers (`TransferSniffer`)" in [internals/j1939.md](internals/j1939.md#hearing-what-other-participants-say-listener)). On a network with a physical bridge, neither times nor order are exact (see "Physical CAN and virtual time" in [bridges.md](bridges.md#physical-can-and-virtual-time)).
- To see the events of a `Network` (including transmissions and address changes) for recording or tests, use a `j1939::Observer` (see "Observation (`Observer`)" in [internals/j1939.md](internals/j1939.md#observation-observer)). It can be set together with a `Listener`.
- Raw CAN sends nothing unless you call `send()`. If you read the frames yourself but want connections between other ECUs followed and reassembled for you, pass the frames to a `j1939::TransferSniffer`.

Not verified: whether a CAN controller that sends nothing changes the behavior of the bus (such as the number of nodes that return ACK) on the Vector Network Simulator.

## Read and write signals from outside (UI, tests)

To read and write the values of an ECU from a UI or a test, declare variables in the ECU and use the Control API. The Control API handles declared variables and time control; it does not handle J1939 frames.

| Value | Declare it as |
|---|---|
| Values to show outside, such as received signals | `output` |
| Values set from outside | `input` |

Here is part of the constructor of `Engine` from [examples/ecu_main/engine/main.cpp](../examples/ecu_main/engine/main.cpp):

```cpp
ecu.input("engine.throttle", &throttle_,
          {.unit = "%", .min = 0.0, .max = 100.0, .description = "accelerator pedal position"});
ecu.output("engine.speed", &rpm_, {.unit = "rpm"});   // the value the transmitted PG is built from
```

For the declaration rules, reading and writing from the same process, and how to connect to the Control Server, see [control-api.md](control-api.md).

## Data layout (Codec)

Protocol parts and the engine handle bytes only (the `Encoder` of a `PeriodicSender` is a function that writes into a buffer and returns the length, and reception gives a `std::span` in `Message::data`). Typed messages generated from YAML ([j1939-codec.md](j1939-codec.md)) handle signal layout and scaling, and the functions in `codec_node.hpp` connect the two.

Your own protocol parts can define their message layouts in the same YAML (with a `selector` when several messages share one PGN, as with Proprietary A). [examples/typed_messages](../examples/typed_messages) is an example that defines messages in YAML and uses them.

## See also

| Page | Contents |
|---|---|
| "[Writing an ECU](../README.md#writing-an-ecu)" in the README | The minimal form of an ECU. Read it before this page. |
| [j1939-codec.md](j1939-codec.md) | Typed messages that read and write signals as values |
| [ecu-executable.md](ecu-executable.md) | How to build an ECU as an executable |
| [internals/j1939.md](internals/j1939.md) | Details of the API's guarantees and rules |
