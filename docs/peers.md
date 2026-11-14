# ピアを別々のプロセスで動かす（`Peer`、`vehicle_sil_sim_controller`）

`Simulation` や `ecu_main` を使わずに、`Peer` や SIL Kit の participant を自分で組み立て、別々のプロセスで動かす方法です。例の実行ファイルを起動するコマンドと、自分のピアを書くときの `PeerOptions` の設定を示します。

| したいこと | 使うもの | 節 |
|---|---|---|
| 2 つのピアを起動して、J1939 の送受信を確認する | `vehicle_sil_j1939_sender`、`vehicle_sil_j1939_receiver` | [ピアを手動で動かす](#ピアを手動で動かす) |
| 複数のピアを 1 つの仮想時間で同期させる | `vehicle_sil_sim_controller`、`PeerOptions` | [仮想時間で動かす](#仮想時間で動かす) |
| CAN バスの調停と送信時間を再現する | `vehicle_sil_can_bus_simulator` | [CAN バスの調停と送信時間を模擬する](#can-バスの調停と送信時間を模擬する) |

ここに挙げる実行ファイルは、結合テストでも使います。

## 前提

コマンドは、リポジトリのルートで、ビルドした後に実行します。コマンドの `B` は、ビルドのディレクトリ（`build/<プリセット>`）です。

| 実行ファイル | 場所 |
|---|---|
| `sil-kit-registry` | `$B/<CMAKE_BUILD_TYPE>/`（Release のプリセットなら `Release/`、Debug のプリセットなら `Debug/`） |
| 例のピア | `$B/examples/` の下 |
| `vehicle_sil_sim_controller`、`vehicle_sil_can_bus_simulator` | `$B/tools/` の下 |

`sil-kit-registry` の場所は、SIL Kit の出力のルールで決まります。テストを無効にしたビルドでは、`sil-kit-registry` を明示的にビルドします。

```sh
cmake --build build/linux-gcc-release --target sil-kit-registry
```

## ピアを手動で動かす

```sh
B=build/linux-gcc-release
$B/Release/sil-kit-registry --listen-uri silkit://127.0.0.1:8500 &
$B/examples/j1939/vehicle_sil_j1939_receiver --registry-uri silkit://127.0.0.1:8500 --expect-name 0x20008200FE001234 &
$B/examples/j1939/vehicle_sil_j1939_sender   --registry-uri silkit://127.0.0.1:8500 --wait-for J1939Receiver --linger-for J1939Receiver
```

| ピア | すること |
|---|---|
| Sender | J1939 スタックのノードでアドレス 0x80 を主張し、自分のアドレスになったら PGN 0xFF10 を 5 回送信します。 |
| Receiver | CAN ID とペイロードを検証し、成功すると終了コード 0 を返します。 |

### 確認

成功すると、Sender と Receiver がどちらも終了コード 0 で終わります。

| 終了コード | 意味 |
|---|---|
| 0 | 成功 |
| 1 | 検証の失敗 |
| 2 | タイムアウト |
| 3 | 接続や起動の失敗 |
| 4 | 引数のエラー |

引数なしで起動すると、エラーの行に続けてオプションの一覧が表示されます。

## 仮想時間で動かす

ピアごとにステップ幅を決めて、1 つの仮想時間で同期させます。全体は最も遅いピアに合わせて進みます。指定しなければ、できるだけ速く（実時間より速く）進みます。

| ピア | participant 名 | ステップ幅 | 内容 |
|---|---|---|---|
| `vehicle_sil_vt_physics` | `Physics` | 1 ms | 物理モデル |
| `vehicle_sil_vt_ecu` | `Ecu` | 10 ms | J1939 の ECU |
| `vehicle_sil_vt_lin_sensor` | `LinSensor` | 2 ms | LIN Slave |
| `vehicle_sil_vt_body_controller` | `BodyController` | 5 ms | LIN Master |

`vehicle_sil_sim_controller` は、必要な 4 つのピアがそろったら T=0 から開始し、仮想時間 5 秒で停止します。

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

### 確認

成功すると、4 つのピアと controller が終了コード 0 で終わり、4 つのトレース（`physics.trace` など）ができます。controller は終了時に、仮想時間 0 から停止時刻までにかかった壁時計の時間を表示します。

```text
[sim-controller] result sim_ms=5000 wall_us=... speed=...
```

### 引数

| 引数 | 対象 | 動作 |
|---|---|---|
| `--real-time-factor 1` | ピア | 実時間に合わせて進みます（ほかのピアもそれを待ちます）。 |
| `--real-time-factor 4`、`0.5` | ピア | 4 倍速、半分の速度で進みます。 |
| `--tick-cost-us 25000` | `vehicle_sil_vt_ecu` | `tick()` が壁時計で 25 ms かかる遅い ECU を模擬します。 |
| `--stop-at-ms` | controller | 省くと、controller は Ctrl+C まで動き続けます。 |
| `--end-ms` | ピア | ピアは、この時刻より前のステップでだけ処理とトレースを行います。 |
| `--timeout-ms` | ピア | 起動待ちの上限です。 |
| `--run-timeout-ms` | ピア | 実行全体の上限です（省略時は上限なし）。 |

`--tick-cost-us` を付けると全体が実時間より遅くなりますが、トレースの内容は変わりません。

### 自分のピアを書く

[examples/virtual_time/common/vt_peer.hpp](../examples/virtual_time/common/vt_peer.hpp) の `peer_options()`、`run_until_finished()` と、[examples/virtual_time/ecu/main.cpp](../examples/virtual_time/ecu/main.cpp) を短くしたものです。この形のままのコードは、リポジトリにはありません（`name`、`pgn`、`ecu_tick()`、`ecu_output()` は利用者のもの）。

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
> `PeerOptions` の既定は、autonomous の lifecycle で仮想時間です。この組み合わせのピアを先に起動して仮想時間が進んでいると、後から coordinated の participant（`vehicle_sil_launch run` が起動する ECU など）が始まった時点で、SIL Kit がシミュレーション全体を abort します（先にいたピアも終わります）。

coordinated の participant と一緒に動かすピアは、次のどちらかにします。

- 上の例のように `Coordinated` にして、必須の participant に入れる
- coordinated の participant が全部始まった後に起動する

受信の扱い、J1939 と LIN の決定論性、LIN のステップ幅の制約は [internals/virtual-time.md](internals/virtual-time.md) と [internals/lin.md](internals/lin.md) を参照してください。`Peer` を使わずに `ParticipantRuntime` のステップハンドラで動かすこともできます（`bind_host()`、LIN の例 `examples/lin`）。

## CAN バスの調停と送信時間を模擬する

SIL Kit の標準の CAN は、送信したフレームを送信した時刻のまま全員に届けます。調停と送信時間を仮想時間で再現するには、簡易 CAN Network Simulator を participant として加えます。各ピアのコードは変わりません。

`B` と `URI` は、上の「仮想時間で動かす」と同じです。`Ecu1,Ecu2` は、自分のピアの participant 名に置き換えます。

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

| 項目 | 動作 |
|---|---|
| 受信のフレームの時刻 | 送信の終わりになります。 |
| 同じ時刻に送信したフレーム | CAN ID の順に届きます（同じコントローラーのフレームは送信した順）。 |
| 結果を決めるもの | 送信時刻とシミュレーターのステップ幅だけです。 |
| 模擬する範囲 | CAN のネットワークだけで、スタッフビット、エラー、CAN FD は扱いません。 |
| 設定ファイルとコマンドライン | Vector SIL Kit Network Simulator と同じ形にしています。 |

- 実行ファイルを Vector のもの（CANoe のライセンスが必要）に替えて動かすことは、まだ確認していません。
- `SilKitCanPort` を使うピアでは、`SilKitCanPortOptions::transmit_queue_capacity` を `--transmit-queue`（既定 64）以下にしてください。
- シミュレーターの送信キューがあふれると、フレームは送信されません。
- ルール、決定論の前提、制限は [internals/can.md](internals/can.md) の「バスのタイミング」と「簡易 CAN Network Simulator」にあります。

## 関連するドキュメント

| 読むもの | 内容 |
|---|---|
| README の「[ECU の書き方](../README.md#ecu-の書き方)」、[j1939-guide.md](j1939-guide.md) | この文書の前に読む、ECU とノードの書き方 |
| [internals/virtual-time.md](internals/virtual-time.md) | 仮想時間のルール |
| [internals/can.md](internals/can.md) | CAN の基盤 |
