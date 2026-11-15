# Writing LIN nodes

[日本語](ja/lin-guide.md)

This page shows how to give an ECU a LIN master or slave, and how to run the LIN example as separate processes. For the rules and constraints of the schedule, frame transmission and reception, diagnostic frames (0x3C, 0x3D) and Sleep/Wakeup, see [internals/lin.md](internals/lin.md).

`vehicle_sil::lin` provides a LIN master and slave that run in virtual time. It depends on neither SIL Kit nor J1939.

| Goal | Use | Section |
|---|---|---|
| Give an ECU class a master or a slave | `Ecu::lin_master()`, `Ecu::lin_slave()` | [Use LIN in an ECU class](#use-lin-in-an-ecu-class) |
| Build the nodes yourself on SIL Kit, without `Peer` | `SilKitLinPort`, `LinMaster`, `LinSlave` | [Build without `Peer`](#build-without-peer) |

## Use LIN in an ECU class

Inside an ECU class, create the nodes with `Ecu::lin_master()` and `Ecu::lin_slave()`. The result is then the same with `Simulation()` (the in-process virtual LIN bus) and with `Simulation(SilKitOptions{})` (SIL Kit).

The code below is `BodyController` (the master) and `DoorModule` (a slave in another ECU) from [examples/vehicle/main.cpp](../examples/vehicle/main.cpp). That file contains the `#include` lines, the constants and `main()`. `lin` is an alias for `vehicle_sil::lin`.

```cpp
class BodyController // a J1939 node on "Body" and the LIN master of "Door"
{
public:
    explicit BodyController(vehicle_sil::Ecu& ecu)
    {
        ecu.j1939("Body").add_node({.address = 0x40}).on_message(kSpeed, [this](const j1939::Message& m) {
            speed = static_cast<unsigned>(m.data[0] | (m.data[1] << 8));
        });
        auto& master = ecu.lin_master("Door", {{kCommand, 1, lin::LinChecksumModel::Enhanced, 20ms, lin::LinPublisher::Master},
                                               {kDoorStatus, 1, lin::LinChecksumModel::Enhanced, 20ms, lin::LinPublisher::Slave}});
        master.on_result([this](const lin::LinScheduleResult& r) {
            if (r.id == kDoorStatus && r.status == lin::LinResultStatus::Ok) door_locked = r.data[0];
        });
        ecu.on_step([this, &master](SimTime) { // runs before the master's step: the header of this step carries it
            master.set_frame_data(kCommand, std::array{static_cast<std::uint8_t>((std::min)(speed, 255u))});
        });
    }
    unsigned speed = 0;       // the last speed from the gateway
    unsigned door_locked = 0; // the last answer of the door module (LIN)
};

class DoorModule // a LIN slave on "Door"
{
public:
    explicit DoorModule(vehicle_sil::Ecu& ecu)
    {
        auto& slave = ecu.lin_slave("Door", {.subscriptions = {kCommand}});
        slave.on_frame([this](const lin::LinReceivedFrame& f) { speed = f.data[0]; });
        ecu.on_step([this, &slave](SimTime now) { // locks the doors above 10 km/h
            slave.set_response(now, kDoorStatus, std::array<std::uint8_t, 1>{speed > 10 ? std::uint8_t{1} : std::uint8_t{0}});
        });
    }
    unsigned speed = 0; // the last command from the body controller (LIN)
};
```

| Role | Call | Behavior |
|---|---|---|
| Master | `ecu.lin_master(network, schedule)` | Advances the schedule by the ECU's step size. |
| Master | `LinPublisher::Master` and `LinPublisher::Slave` in the schedule | The master sends `kCommand`, and the slave responds to `kDoorStatus`. |
| Master | `on_result` | Receives the result at the end of each slot. |
| Master | `set_frame_data` inside `ecu.on_step` | The application runs before the LIN processing, so the data takes effect from the header of this step. |
| Master | `request_sleep()`, `request_wakeup()` | Puts the cluster to sleep and wakes it up when you need to. |
| Slave | `ecu.lin_slave(network, {.subscriptions = {...}})` | Receives the frames with the given IDs. |
| Slave | `on_frame` | Receives each frame with the time of its header. The application reads it in the next step. |
| Slave | `set_response(now, id, data)` | Responds to the headers after `now`. |

## Build without `Peer`

To build the nodes without `Peer`, create a `vehicle_sil::lin::SilKitLinPort` (`vehicle_sil::lin_silkit`) on top of a SIL Kit LIN controller. Then run `LinMaster(port, schedule, step)` and `LinSlave(port, options)` from your own step handler.

[examples/lin](../examples/lin) is the example (`master/main.cpp` and `slave/main.cpp`). Its 3 peers form one cluster on SIL Kit:

| Peer | Participant name | Step size |
|---|---|---|
| `vehicle_sil_lin_master` | `LinMaster` | 5 ms |
| `vehicle_sil_lin_slave --role echo` | `LinEcho` | 2 ms |
| `vehicle_sil_lin_slave --role status` | `LinStatus` | 4 ms (`--step-us 4000`) |

Build with the `linux-gcc-release` preset, then run these commands from the repository root. The master puts the cluster to sleep at 200 ms and wakes it up at 400 ms.

```sh
B=build/linux-gcc-release
URI=silkit://127.0.0.1:8500
$B/Release/sil-kit-registry --listen-uri $URI &
$B/tools/sim_controller/vehicle_sil_sim_controller --registry-uri $URI \
    --required LinMaster,LinEcho,LinStatus --stop-at-ms 1000 &
E=$B/examples/lin
# Master (5 ms): puts the cluster to sleep at 200 ms, wakes it up at 400 ms
$E/vehicle_sil_lin_master --registry-uri $URI --end-ms 1000 --sleep-at-ms 200 --wakeup-at-ms 400 --trace master.trace &
$E/vehicle_sil_lin_slave --role echo   --registry-uri $URI --end-ms 1000 --trace echo.trace &     # 2 ms
$E/vehicle_sil_lin_slave --role status --registry-uri $URI --end-ms 1000 --step-us 4000 --trace status.trace
```

### Verify the LIN run

On success, the 3 peers and `vehicle_sil_sim_controller` end with exit code 0, and `master.trace`, `echo.trace` and `status.trace` are created. At the end, `vehicle_sil_sim_controller` prints this line:

```text
[sim-controller] result sim_ms=1000 wall_us=... speed=...
```

The registry (`sil-kit-registry`) keeps running. Stop it with `kill %1` when you are done.

## See also

| Page | Contents |
|---|---|
| "[Writing an ECU](../README.md#writing-an-ecu)" in the README | The minimal form of an ECU. Read it before this page. |
| [internals/lin.md](internals/lin.md) | Rules and constraints of the schedule, transmission and reception, diagnostic frames and Sleep/Wakeup |
| [peers.md](peers.md) | How to run peers as separate processes |
