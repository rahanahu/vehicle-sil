# ピアを別々のプロセスで動かす（`Peer`、`vehicle_sil_sim_controller`）

`Simulation` や `ecu_main` を使わずに、`Peer` や SIL Kit の participant を自分で組み立て、別々のプロセスで動かす人向けです。先に README の「[最初の ECU を動かす](../README.md#最初の-ecu-を動かす)」と [j1939-guide.md](j1939-guide.md) を読みます。次は [internals/virtual-time.md](internals/virtual-time.md)（仮想時間の規則）と [internals/can.md](internals/can.md)（CAN の基盤）です。

ここに挙げる実行ファイルは、結合試験でも使います。コマンドの `B` は、ビルドのディレクトリ（`build/<プリセット>`）です。

## ピアを手動で動かす

```sh
B=build/linux-gcc-release
cmake --build $B --target sil-kit-registry   # テストを無効にしたビルドでは、明示的に作る
$B/Release/sil-kit-registry --listen-uri silkit://127.0.0.1:8500 &
$B/examples/j1939/vehicle_sil_j1939_receiver --registry-uri silkit://127.0.0.1:8500 --expect-name 0x20008200FE001234 &
$B/examples/j1939/vehicle_sil_j1939_sender   --registry-uri silkit://127.0.0.1:8500 --wait-for J1939Receiver --linger-for J1939Receiver
```

Sender は J1939 スタックのノードでアドレス 0x80 を主張し、自分のアドレスになったら PGN 0xFF10 を 5 回送ります。Receiver は CAN ID とペイロードを検証し、成功すると終了コード 0 を返します。

| 終了コード | 意味 |
|---|---|
| 0 | 成功 |
| 1 | 検証の失敗 |
| 2 | タイムアウト |
| 3 | 接続や起動の失敗 |
| 4 | 引数の誤り |

引数なしで起動すると、オプションの一覧が表示されます。

> [!NOTE]
> `sil-kit-registry` は、SIL Kit の出力規則により `build/<preset>/<CMAKE_BUILD_TYPE>/` に出力されます（Debug のプリセットなら `Debug/`）。

## 仮想時間で動かす

ピアごとにステップ幅を決めて、1 つの仮想時間で同期させます。全体は最も遅いピアに合わせて進みます。指定しなければ、できるだけ速く（実時間より速く）進みます。

```sh
B=build/linux-gcc-release
URI=silkit://127.0.0.1:8500
$B/Release/sil-kit-registry --listen-uri $URI &
# 必要な 4 ピアがそろったら T=0 から開始し、仮想時間 5 秒で停止する
$B/tools/sim_controller/vehicle_sil_sim_controller --registry-uri $URI \
    --required Physics,Ecu,LinSensor,BodyController --stop-at-ms 5000 &
E=$B/examples/virtual_time
$E/vehicle_sil_vt_physics         --registry-uri $URI --end-ms 5000 --trace physics.trace &   # 1 ms
$E/vehicle_sil_vt_ecu             --registry-uri $URI --end-ms 5000 --trace ecu.trace &       # 10 ms、J1939
$E/vehicle_sil_vt_lin_sensor      --registry-uri $URI --end-ms 5000 --trace sensor.trace &    # 2 ms、LIN Slave
$E/vehicle_sil_vt_body_controller --registry-uri $URI --end-ms 5000 --trace body.trace        # 5 ms、LIN Master
```

- `--real-time-factor 1` を付けたピアは、実時間に合わせて進みます。ほかのピアもそれを待ちます。`4` で 4 倍速、`0.5` で半分の速さです。
- `vehicle_sil_vt_ecu --tick-cost-us 25000` は、`tick()` が壁時計で 25 ms かかる遅い ECU を模擬します。全体が実時間より遅くなりますが、トレースの内容は変わりません。
- controller は終了時に、仮想時間 0 から停止時刻までにかかった壁時計の時間（`result sim_ms=... wall_us=...`）を出力します。
- `--stop-at-ms` を省くと、controller は Ctrl+C まで動き続けます。各ピアは `--end-ms` より前のステップでだけ処理とトレースを行います。ピアの `--timeout-ms` は起動待ちの上限で、実行全体の上限は `--run-timeout-ms` で指定します（省略時は上限なし）。

自分のピアでは、次のように使います。

```cpp
vehicle_sil::PeerOptions options;
options.participant = "MyEcu";
options.registry_uri = "silkit://127.0.0.1:8500";
options.lifecycle = vehicle_sil::LifecycleMode::Coordinated;
options.virtual_time = vehicle_sil::VirtualTimeOptions{std::chrono::milliseconds(10)};
vehicle_sil::Peer peer(options);
auto& ecu = peer.j1939("J1939_CAN1").add_node({0x80, name});

// 1 ステップの順序: J1939 の受信と処理 → アプリ（on_step、every、at）→ LIN → J1939 の送信
peer.every(std::chrono::milliseconds(10), [&](vehicle_sil::SimTime) {
    ecu_tick();                                       // 既存 ECU の tick()
    // ステップの終わりに、ステップの時刻で出る。ただし Address Claim の後の 250 ms は保留され、待ちが明けたステップで出る
    static_cast<void>(ecu.send(pgn, ecu_output()));
});
peer.start(std::chrono::seconds(30));
peer.wait(std::chrono::minutes(10));
```

> [!NOTE]
> `PeerOptions` の既定は、autonomous の lifecycle で仮想時間です。この組み合わせのピアを先に起動して仮想時間が進んでいると、後から始まる coordinated の participant（`vehicle_sil_launch run` が起動する ECU など）は Error になります（SIL Kit の決まりです）。coordinated の participant と一緒に動かすピアは、上の例のように `Coordinated` にして必須の participant に入れるか、coordinated の participant が全部始まった後に起動します。

受信の扱い、J1939 と LIN の決定論性、LIN のステップ幅の制約は [docs/internals/virtual-time.md](internals/virtual-time.md) と [docs/internals/lin.md](internals/lin.md) を参照してください。`Peer` を使わずに `ParticipantRuntime` のステップハンドラで回すこともできます（`bind_host()`、LIN の例 `examples/lin`）。

## CAN バスの調停と送信時間を模擬する

SIL Kit の標準の CAN は、送ったフレームを送った時刻のまま全員に届けます。調停と送信時間を仮想時間で再現するには、簡易 CAN Network Simulator を participant として加えます。各ピアのコードは変わりません。

```sh
cat > netsim.yaml <<'YAML'
SchemaVersion: 1
SimulationStepSizeNanoseconds: 1000000
SimulatedNetworks:
  - Name: CAN1
    Type: CAN
YAML
$B/tools/can_bus_simulator/vehicle_sil_can_bus_simulator --connect-uri $URI --default-bitrate 250000 netsim.yaml &
# システムコントローラーの必須 participant に NetworkSimulator を加える
$B/tools/sim_controller/vehicle_sil_sim_controller --registry-uri $URI --required NetworkSimulator,Ecu1,Ecu2 --stop-at-ms 5000
```

- 設定ファイルとコマンドラインは、Vector SIL Kit Network Simulator と同じ形にしています。実行ファイルを Vector のもの（CANoe のライセンスが必要）に替えて動かすことは、まだ確かめていません。模擬するのは CAN のネットワークだけで、スタッフビット、エラー、CAN FD は扱いません。
- 受信のフレームの時刻は送信の終わりになり、同じ時刻に送ったフレームは CAN ID の順に届きます（同じコントローラーのフレームは送った順）。結果は送信時刻とシミュレーターのステップ幅だけで決まります。
- `SilKitCanPort` を使うピアでは、`SilKitCanPortOptions::transmit_queue_capacity` を `--transmit-queue`（既定 64）以下にしてください。シミュレーターの送信キューがあふれると、フレームは送られません。
- 規則、決定論の前提、制限は [docs/internals/can.md](internals/can.md) の「バスのタイミング」と「簡易 CAN Network Simulator」にあります。
