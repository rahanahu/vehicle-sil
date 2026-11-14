# LIN のノードの作り方

ECU に LIN の Master か Slave を持たせる方法と、LIN の例を別々のプロセスで動かす方法です。スケジュール、フレームの送受信、診断フレーム（0x3C、0x3D）、Sleep/Wakeup のルールと制約は、[internals/lin.md](internals/lin.md) にあります。

`vehicle_sil::lin` は、仮想時間で動く LIN の Master と Slave です（SIL Kit にも J1939 にも依存しません）。

| したいこと | 使うもの | 節 |
|---|---|---|
| ECU のクラスに Master か Slave を持たせる | `Ecu::lin_master()`、`Ecu::lin_slave()` | [ECU のクラスで使う](#ecu-のクラスで使う) |
| `Peer` を使わずに、SIL Kit の上で自分で組み立てる | `SilKitLinPort`、`LinMaster`、`LinSlave` | [`Peer` を使わずに組み立てる](#peer-を使わずに組み立てる) |

## ECU のクラスで使う

ECU のクラスの中で `Ecu::lin_master()` と `Ecu::lin_slave()` から使うと、`Simulation()`（プロセス内の仮想 LIN バス）でも `Simulation(SilKitOptions{})`（SIL Kit）でも同じ結果になります。

[examples/vehicle/main.cpp](../examples/vehicle/main.cpp) の `BodyController`（Master）と `DoorModule`（Slave。別の ECU）です。`#include`、定数、`main()` は、このファイルにあります。`lin` は `vehicle_sil::lin` の別名です。

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

| 役割 | 呼び出し | 動作 |
|---|---|---|
| Master | `ecu.lin_master(network, schedule)` | スケジュールを、ECU のステップ幅で進めます。 |
| Master | スケジュールの `LinPublisher::Master` と `LinPublisher::Slave` | `kCommand` は Master が送信し、`kDoorStatus` は Slave が応答します。 |
| Master | `on_result` | スロットの終わりに結果を受け取ります。 |
| Master | `ecu.on_step` の中の `set_frame_data` | アプリの処理は LIN の処理より前なので、このステップのヘッダーから効きます。 |
| Master | `request_sleep()`、`request_wakeup()` | 必要なら、クラスタを休止させ、起こします。 |
| Slave | `ecu.lin_slave(network, {.subscriptions = {...}})` | 指定した ID のフレームを受信します。 |
| Slave | `on_frame` | ヘッダーの時刻付きで受信し、アプリは次のステップで読みます。 |
| Slave | `set_response(now, id, data)` | `now` より後のヘッダーに応答します。 |

## `Peer` を使わずに組み立てる

`Peer` を使わずに組み立てるときは、SIL Kit の LIN コントローラの上に `vehicle_sil::lin::SilKitLinPort`（`vehicle_sil::lin_silkit`）を作り、`LinMaster(port, schedule, step)` と `LinSlave(port, options)` を自分のステップハンドラで動かします。

例は [examples/lin](../examples/lin) です（`master/main.cpp` と `slave/main.cpp`）。3 つのピアが、SIL Kit の上で 1 つのクラスタを作ります。

| ピア | participant 名 | ステップ幅 |
|---|---|---|
| `vehicle_sil_lin_master` | `LinMaster` | 5 ms |
| `vehicle_sil_lin_slave --role echo` | `LinEcho` | 2 ms |
| `vehicle_sil_lin_slave --role status` | `LinStatus` | 4 ms（`--step-us 4000`） |

リポジトリのルートで、`linux-gcc-release` のプリセットでビルドした後に実行します。Master は 200 ms でクラスタを休止させ、400 ms で起こします。

```sh
B=build/linux-gcc-release
URI=silkit://127.0.0.1:8500
$B/Release/sil-kit-registry --listen-uri $URI &
$B/tools/sim_controller/vehicle_sil_sim_controller --registry-uri $URI \
    --required LinMaster,LinEcho,LinStatus --stop-at-ms 1000 &
E=$B/examples/lin
# Master（5 ms）: 200 ms で休止させ、400 ms で起こす
$E/vehicle_sil_lin_master --registry-uri $URI --end-ms 1000 --sleep-at-ms 200 --wakeup-at-ms 400 --trace master.trace &
$E/vehicle_sil_lin_slave --role echo   --registry-uri $URI --end-ms 1000 --trace echo.trace &     # 2 ms
$E/vehicle_sil_lin_slave --role status --registry-uri $URI --end-ms 1000 --step-us 4000 --trace status.trace
```

### 確認

成功すると、3 つのピアと `vehicle_sil_sim_controller` が終了コード 0 で終わり、`master.trace`、`echo.trace`、`status.trace` ができます。`vehicle_sil_sim_controller` は、終わりに次の行を表示します。

```text
[sim-controller] result sim_ms=1000 wall_us=... speed=...
```

`sil-kit-registry` は動いたままなので、最後に `kill %1` で止めます。

## 関連するドキュメント

| 読むもの | 内容 |
|---|---|
| README の「[ECU の書き方](../README.md#ecu-の書き方)」 | この文書の前に読む、ECU の最小の形 |
| [internals/lin.md](internals/lin.md) | スケジュール、送受信、診断フレーム、Sleep/Wakeup のルールと制約 |
| [peers.md](peers.md) | ピアを別々のプロセスで動かす方法 |
