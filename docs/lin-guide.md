# LIN のノードの作り方

LIN を使う ECU を書く人向けです。先に README の「[最初の ECU を動かす](../README.md#最初の-ecu-を動かす)」を読みます。次は [internals/lin.md](internals/lin.md)（スケジュール、送受信、診断フレーム、Sleep/Wakeup の規則と制約）です。

`vehicle_sil::lin` は、仮想時間で動く LIN の Master と Slave です（SIL Kit にも J1939 にも依存しません）。ECU のクラスの中で `Ecu::lin_master()` と `Ecu::lin_slave()` から使うと、`Simulation()`（プロセス内の仮想 LIN バス）でも `Simulation(SilKitOptions{})`（SIL Kit）でも同じ結果になります。例は [examples/vehicle](../examples/vehicle) です。`examples/lin` の 3 つのピアは、`Peer` を使わずに SIL Kit の上で 1 つのクラスタを作ります。

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

ECU のクラスでは、次のように使います。

```cpp
using namespace vehicle_sil::lin;
// Master: 0x10 は Master が送り、0x20 は Slave が応答する。Master は ECU のステップ幅で回る
class Body {
public:
    explicit Body(vehicle_sil::Ecu& ecu)
      : master_(ecu.lin_master("LIN_NET", {{0x10, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master},
                                           {0x20, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave}})) {
        master_.on_result([](const LinScheduleResult& r) { /* スロットの終わりに届く */ });
        // アプリの処理は LIN の処理より前: このステップのヘッダーから効く。必要なら request_sleep() / request_wakeup()
        ecu.on_step([this](vehicle_sil::SimTime) { master_.set_frame_data(0x10, command_); });
    }
private:
    LinMaster& master_;
    std::array<std::uint8_t, 2> command_{};
};

// Slave（別の ECU）: 0x10 を受信し、0x20 に応答する
class Sensor {
public:
    explicit Sensor(vehicle_sil::Ecu& ecu) : slave_(ecu.lin_slave("LIN_NET", {.subscriptions = {0x10}})) {
        slave_.on_frame([](const LinReceivedFrame& f) { /* ヘッダーの時刻付き。アプリは次のステップで読む */ });
        ecu.on_step([this](vehicle_sil::SimTime now) { slave_.set_response(now, 0x20, value_); }); // now より後のヘッダーへ
    }
private:
    LinSlave& slave_;
    std::array<std::uint8_t, 2> value_{};
};
```

`Peer` を使わずに組み立てるときは、SIL Kit の LIN コントローラの上に `vehicle_sil::lin::SilKitLinPort`（`vehicle_sil::lin_silkit`）を作り、`LinMaster(port, schedule, step)` と `LinSlave(port, options)` を自分のステップハンドラで回します（`examples/lin`）。

スケジュール、フレームの送受信、診断フレーム（0x3C、0x3D）、Sleep/Wakeup の規則と制約は、[docs/internals/lin.md](internals/lin.md) を参照してください。
