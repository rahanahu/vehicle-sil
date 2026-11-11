# vehicle-sil

[Vector SIL Kit](https://github.com/vectorgrp/sil-kit) の上で、車載通信のシミュレーションピアを作るための C++20 ライブラリです。J1939 over CAN と LIN に対応しています。ピアは Windows と Linux の独立したプロセスとして動きます。

> [!NOTE]
> vehicle-sil は最初のリリースの前です。公開 API は、互換性を保たずに変わることがあります。わかっている制限は「[既知の制限](#既知の制限)」に、未実装の項目と制約の詳細は [docs/architecture.md](docs/architecture.md) にあります。

使えるのは、vehicle-sil 自身の J1939 スタック（単一フレーム、Request と Acknowledgement、Address Claim、トランスポートプロトコル、診断、ECU アプリ向けの部品）、SIL Kit と Vector XL Driver をつなぐ CAN ブリッジ（実機は未検証）、LIN の Master/Slave（スケジュールテーブル、フレームの送受信、診断フレーム 0x3C/0x3D、応答の衝突の検出、Sleep/Wakeup）と、それらを同期させる共通仮想時間です。

## ライブラリ

区分は、どこから使い始めるかの目安です。利用者向けは、ECU を書く人と ECU をまとめる人が直接使うライブラリで、入口は `vehicle_sil::Ecu` と `vehicle_sil::Simulation` です。土台は利用者向けのライブラリを通して使うもので、直接も使えますが、API は上の層の必要に合わせて変わります。試験用は、試験とサンプルのための道具です。CAN ブリッジは区分に入れていません（「—」）。

| CMake ターゲット | 区分 | 内容 |
|---|---|---|
| `vehicle_sil::core` | 土台 | `vehicle_sil::ParticipantRuntime`（SIL Kit の participant、lifecycle、他参加者の状態、仮想時間のステップを RAII で管理する）、`SimulationController`（仮想時間のシミュレーションの開始と停止、一時停止、ステップ実行、速度）、`SimTime`、`StepInbox`、`TickScheduler`。プロトコルに依存しない |
| `vehicle_sil::lin` | 土台 | `vehicle_sil::lin::LinMaster`（スケジュールテーブル、Master が送るフレーム、マスターリクエスト、go-to-sleep と wakeup）と `vehicle_sil::lin::LinSlave`（ステップで設定した値で応答、スレーブレスポンス、Master のフレームの受信、休止と起床）、バスへの口 `LinPort` と、プロセス内の仮想 LIN バス `VirtualLinBus`。仮想時間で使う。SIL Kit にも J1939 にも依存しない。ふつうは `Ecu::lin_master()` と `Ecu::lin_slave()` から使う |
| `vehicle_sil::lin_silkit` | 土台 | `vehicle_sil::lin` のポートの SIL Kit 実装 `vehicle_sil::lin::SilKitLinPort`（`Peer` が使う） |
| `vehicle_sil::can` | 土台 | バックエンドに依存しない CAN の基盤。フレームの型、ポートの契約、複数のプロトコル処理をステップで回す `Host`、仮想時間の `StepDriver`、実時間の `RealTimeDriver`、テスト用の仮想バス（調停と送信時間の模擬、故障注入、ECU 単位の通信断を選べる）、生フレームを読み取り専用で観測する `TapPort`。SIL Kit に依存しない。J1939 スタックの土台 |
| `vehicle_sil::can_silkit` | 土台 | `vehicle_sil::can` のポートの SIL Kit 実装（仮想時間と、時刻同期なしの実時間）と、`Host` を `ParticipantRuntime` のステップにつなぐ `bind_host()` |
| `vehicle_sil::can_netsim` | 土台 | 簡易 CAN Network Simulator: SIL Kit の Network Simulator API で、CAN の調停、送信時間、バスの占有を仮想時間で模擬する（`CanBusSimulator`）。Vector SIL Kit Network Simulator と共用の設定ファイルの読み込み |
| `vehicle_sil::j1939` | 利用者向け | J1939 スタック（名前空間 `vehicle_sil::j1939`、ヘッダー `vehicle_sil/j1939/`）。単一フレームの送受信、Request、Acknowledgement、NAME と Address Claim（要求、競合、Cannot Claim、任意アドレスの選択、アドレス表、アドレス違反、Commanded Address の処理、状態の通知）、トランスポートプロトコル（BAM、RTS/CTS）、部品を作る道具箱（`Subscription`、`Timer`、`Node::attach`）、読み取り専用の観測口（`Observer`）。SIL Kit に依存しない |
| `vehicle_sil::j1939_broadcast_suppression`、`vehicle_sil::j1939_diagnostics` | 利用者向け | `vehicle_sil::j1939` の公開 API と道具箱だけで作ったプロトコル部品。DM13 による放送の抑制、DTC と DM1/DM2/DM3/DM11/DM22、識別情報（ソフトウェア ID、ECU ID、製品 ID、診断プロトコル ID） |
| `vehicle_sil::j1939_periodic_sender`、`vehicle_sil::j1939_requester`、`vehicle_sil::j1939_monitor`、`vehicle_sil::j1939_address_commander`、`vehicle_sil::j1939_exchange` | 利用者向け | ECU アプリ向けの部品（同じく公開 API と道具箱だけで作る）。PG の周期送信（`PeriodicSender`）、Request の送信と応答・ACK・時間切れの報告（`Requester`）、受信の最新値と途絶（`Monitor`）、Commanded Address の送信（`AddressCommander`）、独自プロトコルのコマンドと応答（`Exchange`。例は [examples/j1939/param_service](examples/j1939/param_service)）。使い方は [docs/j1939-guide.md](docs/j1939-guide.md) |
| `vehicle_sil::j1939_codec` | 利用者向け | J1939 の型付きメッセージ。YAML で PG と信号（位置、符号、スケーリング、範囲、列挙、Proprietary A/B、名前を付けた特別な値とそのプロファイル、埋め値）を定義すると、`vehicle_sil_add_j1939_codec()` がビルドのときに C++ の型を生成する。ビット操作を書かずに、信号を値として送受信する。使い方は [docs/j1939-codec.md](docs/j1939-codec.md) |
| `vehicle_sil::ecu` | 利用者向け | ECU のアプリケーションを組み立てる入口 `vehicle_sil::Ecu`: J1939 の `Network`、生の CAN チャネル（J1939 でないプロトコル用の `can::RawChannel`）、LIN の Master と Slave（`lin_master()`、`lin_slave()`）、ステップごとの処理（`on_step`、`every`）、ECU が持つオブジェクト（`emplace`）。`Peer` と `LocalBench` の ECU が実装するので、`Ecu&` を受け取るクラスは仮想バスでも SIL Kit でも同じコードで動く。SIL Kit に依存しない |
| `vehicle_sil::peer` | 土台 | 1 つの SIL Kit participant と、その J1939 の `Network`（SIL Kit の CAN か外部のポート）、生の CAN、LIN の Master/Slave をまとめて作り、仮想時間か実時間で動かす `vehicle_sil::Peer`。`Ecu` を実装する |
| `vehicle_sil::simulation` | 利用者向け | `vehicle_sil::Simulation`: ECU をプロセス内の仮想バスか、SIL Kit の participant として仮想時間で動かし、失敗を例外で報告する。動かす場所はコンストラクターで選ぶ（SIL Kit では、registry をプロセス内に立てられる）。入門は [examples/getting_started](examples/getting_started) |
| `vehicle_sil::control`、`vehicle_sil::peer_control` | 利用者向け | participant の Control Agent（型・範囲・単位つきの変数の公開、外からの書き込みを仮想時刻を指定してステップの中で反映、J1939 の通信状態の監視。固定容量のキュー）と、Control Server の中核の `Hub`。`attach(peer, agent)` で Peer に組み込む |
| `vehicle_sil::control_grpc`（`VEHICLE_SIL_BUILD_CONTROL_SERVER=ON` のとき） | 利用者向け | Control API（gRPC）の Server と、Agent の接続 `AgentClient`。認証（TLS、トークン、役割）。[使い方](docs/control-api.md) |
| `vehicle_sil::local` | 土台 | SIL Kit なしでプロセス内の仮想バスに ECU を載せて回す `vehicle_sil::LocalBench`: 仮想バスの上の ECU（`Ecu`）とその `Network` を名前で組み立て、呼んだスレッドで仮想時間で進める。`Simulation` のローカルの実行もこれを使う |
| `vehicle_sil::testkit`（サンプルと試験が使う） | 試験用 | `LocalBench` の上の J1939 の試験の道具: 出来事と生フレームの記録（`J1939Events`、`FrameLog`）、故障注入の関数（`faults`）。`testkit::LocalCan` は `LocalBench` の別名 |
| `vehicle_sil::vector_xl_bridge_core` | — | CAN ブリッジのコア（`vehicle_sil::vector_xl::CanBridge` など）と、外部 CAN ポートを `vehicle_sil::can` の実時間のポートにする `ExternalPortAdapter`。プラットフォームに依存しない |
| `vehicle_sil::vector_xl` | — | Vector XL Driver のバックエンド（`XlDriver`、`XlCanPort`）。Windows 専用で、XL Driver Library が必要 |

## 構成

| パス | 内容 |
|---|---|
| `libs/` | ライブラリ（`core`、`can`、`can_silkit`、`can_netsim`、`j1939`、`lin`、`lin_silkit`、`ecu`、`local`、`testkit`、`peer`、`simulation`、`control`、`control_grpc`） |
| `proto/` | Control API の定義（`vehicle_sil/control/v1/control.proto`） |
| `python/` | Control API の Python クライアントと CLI（`python -m vehicle_sil_control`） |
| `tools/control_server` | Control API の Server（とシミュレーションの調停、時間ゲート）`vehicle_sil_control_server` |
| `examples/control` | Agent を組み込んだ Peer `vehicle_sil_control_peer` と、1 つのプロセスで動くデモ `vehicle_sil_control_demo` |
| `tools/sim_controller` | 仮想時間のシミュレーションを開始・停止する `vehicle_sil_sim_controller` |
| `tools/can_bus_simulator` | 簡易 CAN Network Simulator の participant `vehicle_sil_can_bus_simulator` |
| `tools/j1939_codegen` | YAML の定義から J1939 の型付きメッセージを生成する `j1939_codegen.py` |
| `bridges/vector-xl` | Vector XL CAN Bridge（ライブラリと実行ファイル `vehicle_sil_vector_xl_bridge`） |
| `examples/getting_started` | 最初の例 `vehicle_sil_getting_started`: `Ecu&` を受け取る 2 つの ECU を、`Simulation` で仮想バスか SIL Kit に載せる |
| `examples/` のほかのサンプル | やりたいこと別の `typed_messages`、`pid_stream`、`fault_injection`、`vehicle`、`low_level`。索引は [examples/README.md](examples/README.md) |
| `examples/j1939` | `vehicle_sil_j1939_ecu_pair`（部品で作った 2 つの ECU を `Simulation` で動かす。既定は SIL Kit、`--local` で仮想バス）、`vehicle_sil_j1939_param_service`（独自プロトコル、つまりメーカー独自のパラメーターサービスを、`LocalBench` の仮想バスで動かす）、最小ピアの `vehicle_sil_j1939_sender`（`Peer` を実時間で動かして送る）と `vehicle_sil_j1939_receiver`（SIL Kit の CAN で直接受けて照合する） |
| `examples/virtual_time` | 仮想時間のピア（どれも `Peer` の上）: 物理モデル、既存 ECU（共有ライブラリの `tick()`）を載せた J1939 ECU、LIN センサー、ボディコントローラー |
| `examples/lin` | LIN クラスタのピア `vehicle_sil_lin_master` / `vehicle_sil_lin_slave`（フレームの送受信と Sleep/Wakeup） |
| `examples/fetchcontent_consumer` | 外部 CMake プロジェクトからの利用例 |
| `tools/` | `vehicle_sil_sim_controller`（仮想時間のシミュレーションの開始と停止）と、実行ファイル共通のコマンドライン処理（内部用） |
| `tests/` | ユニットテスト、複数プロセスの統合テスト、FetchContent テスト |
| `external/` | SIL Kit（Git サブモジュール、コミット固定） |
| `patches/` | SIL Kit に当てるパッチ（上流への提出用の形式） |
| `docs/` | [J1939 のノードの作り方](docs/j1939-guide.md)、[J1939 の型付きメッセージ](docs/j1939-codec.md)、[Control API の使い方](docs/control-api.md)、[アーキテクチャと将来の制約](docs/architecture.md)、[依存ライブラリ](docs/dependencies.md) |

## 必要なもの

| | Linux | Windows |
|---|---|---|
| コンパイラ | GCC（標準）または Clang。C++20 対応のもの | MSYS2 UCRT64 の MinGW-w64 GCC |
| ビルドツール | CMake 3.21 以上、Ninja | 同左（MSYS2 のパッケージ） |
| その他 | Git（SIL Kit へのパッチ適用にも使う） | 同左 |
| 試験と例のビルド、型付きメッセージの生成 | Python 3.8 以上と PyYAML | 同左（MSYS2 のパッケージ） |

Visual Studio と MSVC は不要です。

## 取得とビルド

```sh
git clone --recursive https://github.com/rahanahu/vehicle-sil.git
cd vehicle-sil
# --recursive を付けずにクローンした場合
git submodule update --init --recursive
```

### Linux

```sh
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
ctest --preset linux-gcc-release
```

| プリセット | 内容 |
|---|---|
| `linux-gcc-debug`, `linux-gcc-release` | GCC |
| `linux-clang-debug` | Clang |
| `linux-clang-asan`, `linux-clang-tsan` | Clang と AddressSanitizer + UBSan、または ThreadSanitizer |
| `linux-gcc-asan`, `linux-gcc-tsan` | GCC とサニタイザー。`libasan`、`libubsan`、`libtsan` のパッケージが必要 |

`-DVEHICLE_SIL_WARNINGS_AS_ERRORS=ON` を付けると、vehicle-sil 自身のターゲットの警告をエラーにします。

### Windows（MSYS2 UCRT64）

MSYS2 の「UCRT64」シェルで実行します。

```sh
pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-yaml
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
ctest --preset windows-mingw-release
```

`SilKit.dll` は、ビルド後に各実行ファイルの隣へ自動でコピーされます。MinGW のランタイム DLL（`libstdc++-6.dll` など）は、UCRT64 シェルの `PATH` から読み込まれます。

## 最初の ECU を動かす

SIL Kit の registry を起動する必要はありません。次のコマンドは、エンジンの ECU（回転数を周期送信する）と表示の ECU（受けて表示する）を、仮想バスの上で動かします。`--silkit` を付けると、同じ ECU を SIL Kit の participant として動かします（registry はプロセス内に立てます）。

```sh
B=build/linux-gcc-release
$B/examples/getting_started/vehicle_sil_getting_started
$B/examples/getting_started/vehicle_sil_getting_started --silkit
```

ECU は、`vehicle_sil::Ecu&` を受け取る普通のクラスです。どこで動かすかは、`Simulation` のコンストラクターだけで決まります。

```cpp
class Engine {
public:
    explicit Engine(vehicle_sil::Ecu& ecu) {
        auto& node = ecu.j1939("J1939_CAN").add_node({.address = 0x00});
        codec::attach_sender<EngineStatus>(node, 20ms, [this](EngineStatus& m) { m.speed.set(rpm_); });
        ecu.every(10ms, [this](vehicle_sil::SimTime) { rpm_ = rpm_ < 1500 ? rpm_ + 25 : rpm_; });   // モデルを進める
    }
private:
    int rpm_ = 0;
};   // Display も同じ形: codec::on_message<EngineStatus>(node, ...) で受ける

int main() {
    vehicle_sil::Simulation sim;                                // プロセス内の仮想バス
 // vehicle_sil::Simulation sim(vehicle_sil::SilKitOptions{});  // SIL Kit（プロセス内の registry）。変えるのはこの 1 行
    sim.add<Engine>("Engine", 10ms);                            // ステップ幅 10 ms の ECU。Engine は ECU が持つ
    auto& display = sim.add<Display>("Display", 5ms);
    sim.run_for(2s);                                            // 仮想時間で 2 秒。ステップの失敗は SimulationError
    // 戻った後は、ステップが書いた値（display の記録など）を読んでよい
}
```

- メッセージ（`EngineStatus`）は YAML から生成した型です（[docs/j1939-codec.md](docs/j1939-codec.md)）。完全なコードは [examples/getting_started](examples/getting_started) にあります。
- 動かす場所を実行時に選ぶときは、`std::optional<vehicle_sil::SilKitOptions>` を渡します。空ならプロセス内の仮想バス、値があれば SIL Kit です（`Simulation sim(silkit);`）。
- やりたいこと別のサンプル（型付きメッセージ、独自プロトコル、故障注入、複数の ECU と LIN など）は [examples/README.md](examples/README.md) にまとめています。
- 作り方、試験の書き方、`Peer` を直接使う場合は [docs/j1939-guide.md](docs/j1939-guide.md) にあります。
- 以降の「ピアを手動で動かす」「仮想時間で動かす」などは、`Peer` を別プロセスで動かす構成です。これらの実行ファイルは、結合試験でも使います。

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

// 1 ステップの順序: J1939 の受信と処理 → アプリ（on_step、every）→ LIN → J1939 の送信
peer.every(std::chrono::milliseconds(10), [&](vehicle_sil::SimTime) {
    ecu_tick();                                       // 既存 ECU の tick()
    // ステップの終わりに、ステップの時刻で出る。ただし Address Claim の後の 250 ms は保留され、待ちが明けたステップで出る
    static_cast<void>(ecu.send(pgn, ecu_output()));
});
peer.start(std::chrono::seconds(30));
peer.wait(std::chrono::minutes(10));
```

受信の扱い、J1939 と LIN の決定論性、LIN のステップ幅の制約は [docs/architecture.md](docs/architecture.md) の「共通仮想時間」を参照してください。`Peer` を使わずに `ParticipantRuntime` のステップハンドラで回すこともできます（`bind_host()`、LIN の例 `examples/lin`）。

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
- 規則、決定論の前提、制限は [docs/architecture.md](docs/architecture.md) の「バスのタイミング」と「簡易 CAN Network Simulator」にあります。

## LIN

`vehicle_sil::lin` は、仮想時間で動く LIN の Master と Slave です（SIL Kit にも J1939 にも依存しません）。ECU のクラスの中で `Ecu::lin_master()` と `Ecu::lin_slave()` から使うと、`Simulation()`（プロセス内の仮想 LIN バス）でも `Simulation(SilKitOptions{})`（SIL Kit）でも同じ結果になります。例は [examples/vehicle](examples/vehicle) です。`examples/lin` の 3 つのピアは、`Peer` を使わずに SIL Kit の上で 1 つのクラスタを作ります。

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

スケジュール、フレームの送受信、診断フレーム（0x3C、0x3D）、Sleep/Wakeup の規則と制約は、[docs/architecture.md](docs/architecture.md) の「LIN を仮想時間で動かす」を参照してください。

## Vector XL CAN Bridge（Windows）

`vehicle_sil_vector_xl_bridge` は、SIL Kit の CAN ネットワークと Vector XL の CAN チャネルの間で、クラシック CAN のフレームを双方向に転送します。XL のチャネルには、Vector Virtual CAN と、CANcase や VN16xx などの物理 CAN を使えます。

> [!WARNING]
> Windows と Vector のハードウェアでは、まだ動作を確認していません。Linux 上で、XL ドライバのフェイクを使った自動テストだけで確認しています。

### ビルド

[XL Driver Library](https://www.vector.com/)（`vxlapi.h` と `vxlapi64.lib`）は Vector Informatik GmbH のソフトウェアで、vehicle-sil には含まれていません。利用者が Vector から入手し、Vector のライセンス条件に従って使います。ビルドでは、その場所を指定します。

```sh
cmake --preset windows-mingw-release -DVEHICLE_SIL_VECTOR_XL_DIR="C:/path/to/XL Driver Library/bin"
cmake --build --preset windows-mingw-release
```

XL Driver Library が見つからない場合、ブリッジはビルドされず、ほかのターゲットのビルドは続きます。必須にしたいときは `-DVEHICLE_SIL_REQUIRE_VECTOR_XL=ON` を付けます。`vxlapi64.dll` が見つかれば、実行ファイルの隣にコピーされます。

### 使い方

```sh
# XL ドライバが提供する CAN チャネルを、接続先ごとに番号付きで表示する
vehicle_sil_vector_xl_bridge --list-channels

# Vector Virtual CAN の 1 番目のチャネルにつなぐ
vehicle_sil_vector_xl_bridge --registry-uri silkit://127.0.0.1:8500 --network J1939_CAN1 --xl-target virtual --xl-channel 1

# 物理 CAN（CANcase など）の 1 番目のチャネルに、250 kbit/s でつなぐ
vehicle_sil_vector_xl_bridge --registry-uri silkit://127.0.0.1:8500 --network J1939_CAN1 --xl-target physical --xl-channel 1 --bitrate 250000
```

- Ctrl+C で止まり、転送の統計を出力します。
- `--bitrate`（15000〜1000000 bit/s）は init 権限が必要です。ほかのアプリケーションがチャネルを設定している場合は、起動を失敗させます。
- `--bitrate` を指定しないと init 権限を取らないので、XL ドライバは自分の送信の受領（TX レシート）を返してきます。ブリッジはそれを転送せずに捨て、統計の `own_tx_dropped` に数えます。
- 終了コードは次のとおりです。
  - 0: 正常終了
  - 1: 実行中の SIL Kit 側の失敗（registry の停止、接続断）
  - 3: 起動の失敗（XL ドライバ、チャネル、SIL Kit）
  - 4: 引数の誤り

詳しくは [docs/architecture.md](docs/architecture.md) の「Vector XL CAN Bridge」を参照してください。転送ループの防止と、同じバスに 2 つのブリッジをつなげない制約も説明しています。

## テスト

| ラベル | 内容 |
|---|---|
| `unit` | コマンドライン解析。`core`、`lin`、`can`、`j1939`、`peer`、`testkit`、`simulation`、`vector_xl` の単体テストにもこのラベルが付く |
| `integration` | `sil-kit-registry`、receiver、sender（実時間の `Peer` と J1939 スタック）を別プロセスで起動し、終了コードで判定する。`address-claim-and-pgn` は Address Claim と PGN の CAN ID とペイロードの一致を、`receiver-timeout` はタイムアウト処理を検証する。`receiver-detects-wrong-name` と `receiver-detects-wrong-payload` は、receiver が誤った NAME と壊れたペイロードを失敗として報告することを確かめる |
| `vector_xl` | CAN ブリッジのテスト。全プラットフォームで、テスト専用の XL ドライバのフェイクに対して本番のコードを動かす。CanBridge の単体テスト、XL バックエンドのテスト、実際の SIL Kit を使うプロセス内のテスト、ブリッジの実行ファイルを使う E2E テストがある。E2E では、エコーノードとの往復（Virtual CAN と、ビットレートを設定する物理 CAN）、送信レシートが返ってくる場合のループ防止、`--list-channels`、存在しないチャネル、引数の誤り、ドライバがない場合を確かめる |
| `can` | `vehicle_sil::can` の単体テスト（SIL Kit なしでビルドする）、SIL Kit のポートのテスト（`can_silkit.*`）、簡易 CAN Network Simulator のテスト（`can_netsim.*`）。フレームの検証、仮想バスの受信順序とあふれ、ホストの段階の順序と失敗、ステップドライバーの順序、送信時間と調停（11 ビット / 29 ビット）、混雑、送信キュー、受信側のステップ幅によらないこと、ポートの事象（送信の結果、受け箱のあふれ、バス状態、エラーフレーム）、故障注入と ECU 単位の通信断、`post()`、時間の領域の混在の拒否、実時間のドライバー（差し替えた時計での決定的な試験と、壁時計での到着と順序）を確かめる。簡易 CAN Network Simulator では、プロセス内の participant と実行ファイルの両方で、仮想バスと同じ受信の時刻になること、繰り返しても同じ結果になること、送信時刻のない送信者、設定ファイルの読み込みを確かめる。プロセス内のレジストリで、同じシナリオのトレースが仮想バスと SIL Kit で一致すること（ステップ幅の組み合わせを変えて）と、送信の続く中で participant を決まった順序で止めて破棄できることを確かめる（`unit` ラベルも付く） |
| `j1939` | `vehicle_sil::j1939` の単体テスト（SIL Kit なしでビルドする）と SIL Kit 上の試験（`j1939_silkit.*`）。PGN と識別子、単一フレームの送受信、宛先の振り分け、ローカル折り返し、同じ時刻のフレームの順序とタイマーの発火がステップ幅と到着順によらないこと、Request への応答と ACK/NACK、NAME と Address Claim（複数ノードと複数 Network の競合、任意アドレスの選択、アドレス表、アドレス違反、Commanded Address を含む）、1 つのホストの複数の Network（ゲートウェイ、片方の失敗）、再入と失敗、ステップ幅の検査、資源の上限、定常状態のステップでメモリを確保しないことを確かめる。プロセス内のレジストリで、同じシナリオのトレースが仮想バスと SIL Kit で一致することも確かめる（簡易 CAN Network Simulator ありでも。`unit` ラベルも付く）。タイミングありの仮想バスで、プロトコルの結果が変わらないこと、混雑で転送が遅れ、バスが飽和すると時間切れになることも確かめる。1 つの ECU がバスから切り離されたとき（受信だけ、送受信とも）の TP の時間切れと回復、隠れていたアドレスの衝突の解決、時刻同期なしの SIL Kit での実時間の動作も確かめる。道具箱（解除と破棄の再入、Network の後の破棄、タイマーの起点と予約）とプロトコル部品（`j1939_parts.*`: DM13、診断、識別情報、送信の競合、後で返す ACK、ステップ幅によらない結果）も確かめ、`j1939_parts.boundary` が部品がエンジンの内部を使っていないことを検査する。型付きメッセージ（`j1939_codec.*`）は、ビット配置をビットごとの実装と照合し、raw 値と物理値の変換、丸めと範囲の端、特別な値（J1939 のプロファイル、利用者のプロファイル、継承と上書き、すべてのビットパターンを値として使う Proprietary B、raw 値の保持）、列挙、セレクター、埋め値、長さ、小さい信号のすべての raw 値の往復、ノードでの送受信を確かめる。`j1939_codec.generator` は生成器が誤った定義を止めることを確かめる |
| `peer` / `testkit` | `Peer` をプロセス内のレジストリで動かす試験（J1939 と LIN の 3 つの participant、仮想バスと同じ結果、実時間と外部ポート、組み立ての誤り、失敗の報告）と、試験キットの試験（アドレスの喪失、Address Claimed の欠落、TP のパケットの欠落、通信断、生フレームの注入、受信側のステップ幅と記録の有無によらない結果）。`local.boundary`、`testkit.boundary`、`peer.boundary` がエンジンの内部を使っていないことを検査する |
| `simulation` / `examples` | `Simulation` の試験（同じ ECU が仮想バスと SIL Kit で同じ記録を残すこと、ECU が持つオブジェクトの破棄、ステップの失敗が `SimulationError` になって戻ること、使い方の誤り（同じ入力を両方の実装で拒否すること、`App` のコンストラクターの例外）、`request_stop` で終わる実行、ほかの participant の失敗、開始の時間切れ、`local()` から足した ECU、別プロセスの controller や participant との併用、2 つのネットワークを持つゲートウェイ、生の CAN）と、サンプルの実行（`vehicle_sil_getting_started`（仮想バスと `--silkit`）、`typed_messages`、`pid_stream`、`fault_injection`、`vehicle`（SIL Kit と `--local`）、`low_level`）。サンプルは自分で結果を検査し、違えば 0 以外で終わる |
| `core` / `lin` | 仮想時間の単体テスト。プロセス内のレジストリで、ステップ幅の違う参加者の同期、最も遅い参加者を待つこと、実時間への結合、`TickScheduler`、`StepInbox`、LIN のスケジュール、応答、Master のフレームの受信、休止と起床、スケジュールの切り替えを確かめる。LIN は主なシナリオを仮想 LIN バスの上でも確かめ（`lin_local.*`）、仮想 LIN バスだけの試験（`lin_bus.*`）と、`vehicle_sil::lin` が SIL Kit を含まないことの検査（`lin.boundary`）もある（`unit` ラベルも付く） |
| `virtual_time` | registry、controller、4 つのピア（1 / 2 / 5 / 10 ms。どれも `Peer` の上）を別プロセスで動かす。トレースを照合して、データがピアの間を予測どおりの仮想時刻で渡ること、J1939 の ECU が Address Claim の 250 ms 後から送ることを確かめる。全速、遅い `tick()`、4 倍速、0.5 倍速の各条件でトレースが一致し、壁時計の所要時間が実時間より速い・遅いことも確かめる。`peer-failure` は、途中で失敗したピアを controller が検出して中断し、全プロセスが終了コード 1 で終わることを確かめる（`integration` ラベルも付く） |
| `lin`（`lin_cluster.*`） | registry、controller、LIN の Master と 2 つの Slave（5 / 2 / 4 ms）を別プロセスで動かす。トレースを照合して、スケジュール、両方向のフレーム、Master・Slave・bus idle による休止と起床を確かめる。Slave の 1 つを実時間に結合してもトレースが一致することも確かめる（`integration` ラベルも付く） |
| `fetchcontent` | 既定では無効。このリポジトリのコミットを FetchContent で取り込み、利用例をビルドして実行する |

各プロセスのログは `build/<preset>/tests/` の下の `integration/<シナリオ>/`、`virtual_time/<シナリオ>/`、`lin_cluster/<シナリオ>/`、`vector_xl/integration/<シナリオ>/` に残ります。失敗したときは、ランチャーがすべてのログを出力します。

- テストケースは、ソースの `TEST_CASE(名前)` から設定時に自動で登録されます（`<ラベル>.<名前>`）。CMake のリストを書き足す必要はありません。
- テストケースの登録漏れは、テスト実行ファイルごとの `<ラベル>.registration` テストが検出します（実行ファイルの `--list` と登録した名前を比べる）。
- クロスビルドでは、`CMAKE_CROSSCOMPILING_EMULATOR`（たとえば `wine`）を指定すると、`ctest` がテストの実行ファイルをそれで動かします（`registration` テストも含む）。Windows の DLL（MinGW の実行時ライブラリ）は、実行ファイルの隣に置くなどして見つかるようにしてください。
- テストプリセットは 8 並列で実行します。壁時計の所要時間を測るテスト（`virtual_time.*`、`core.virtual_time_*`、LIN の速度のテスト）は単独で実行されます。

```sh
ctest --preset linux-gcc-release -L integration
# FetchContent テスト（依存ライブラリを一から再ビルドするので時間がかかる）
cmake --preset linux-gcc-release -DVEHICLE_SIL_TEST_FETCHCONTENT=ON
ctest --preset linux-gcc-release -L fetchcontent
```

## 外部プロジェクトから使う（FetchContent）

```cmake
include(FetchContent)

FetchContent_Declare(
  vehicle_sil
  GIT_REPOSITORY https://github.com/rahanahu/vehicle-sil.git
  GIT_TAG <固定するコミットハッシュ>)

FetchContent_MakeAvailable(vehicle_sil)

add_executable(example main.cpp)
target_link_libraries(example PRIVATE vehicle_sil::simulation vehicle_sil::j1939_periodic_sender)   # core だけなら vehicle_sil::core
vehicle_sil_copy_runtime_dlls(example)   # Windows: SilKit.dll をコピーする（ほかの OS では何もしない）
```

SIL Kit は、vehicle-sil のサブモジュールとして自動で取得・設定されます。利用側で個別に設定する必要はありません。SIL Kit には、ビルドツリーにコピーしたソースに `patches/sil-kit/` のパッチを当てたものを使います（[docs/dependencies.md](docs/dependencies.md)）。パッチの適用に Git を使います。リンクしないライブラリはビルドされません。完全な例は [examples/fetchcontent_consumer](examples/fetchcontent_consumer) にあります。

最小の使い方は次のとおりです。

```cpp
#include <vehicle_sil/j1939/periodic_sender.hpp>
#include <vehicle_sil/simulation/simulation.hpp>

namespace j1939 = vehicle_sil::j1939;
using namespace std::chrono_literals;

// ECU は Ecu& を受け取るクラスにする。モデルも ECU が持つ（emplace）ので、宣言の順序を気にしなくてよい
class Engine {
public:
    explicit Engine(vehicle_sil::Ecu& ecu) {
        auto& node = ecu.j1939("J1939_CAN1").add_node(
            {.address = 0x00, .name = j1939::Name{}.set_identity_number(0x2345).set_manufacturer_code(0x7F0)});   // Address Claim はスタックが行う
        node.attach<j1939::PeriodicSender>(j1939::Pgn{0xFF10}, 100ms,   // アドレスを得てから 100 ms ごとに送る
                                           [this](std::span<std::uint8_t> out) { return model_.encode(out); });
        ecu.every(10ms, [this](vehicle_sil::SimTime) { model_.tick(); });
    }
private:
    EngineModel model_;
};

int main() {
    vehicle_sil::Simulation sim(vehicle_sil::SilKitOptions{});   // SIL Kit の仮想時間。registry はプロセス内に立つ
    sim.add<Engine>("Engine", 10ms);
    sim.run_for(3s);
}
```

受信、Request と応答、受信の監視、試験の書き方は [docs/j1939-guide.md](docs/j1939-guide.md) にあります。

信号を YAML で定義して型付きメッセージにするときは、`vehicle_sil_add_j1939_codec(<target> <定義.yaml>)` を呼び、`<target>` をリンクします（Python 3 と PyYAML が要ります。[docs/j1939-codec.md](docs/j1939-codec.md)）。

## 既知の制限

- 仮想時間の一時停止とステップ実行は、C++ の API（`SimulationController` の時間ゲート）と、Control API（`vehicle_sil_control_server --required ...`）からできます。`vehicle_sil_sim_controller` からはできません。
- Control API の GUI はまだありません。Peer の起動（Launcher）、リセット、CAN/LIN のチャネルの選択もありません。
- SIL Kit で動かすとき、LIN の Slave は、応答するフレームのスロット長より短いステップ幅で動かす必要があります。LIN の dynamic response など、SIL Kit の experimental API を使っています。
- LIN の Slave が受信できるのは、Master が送るフレームだけです。ほかの Slave の応答（slave-to-slave）は、仮想 LIN バスでは渡されず、SIL Kit では購読した ID なら渡されますが、受け取るステップと時刻は決定論的ではありません。
- 既存 ECU の DLL が内部で実時間（スリープ、時刻の取得）を使っている場合、vehicle-sil はそれを仮想時間に置き換えられません。
- J1939 スタックに ETP（1785 バイトを超えるメッセージ）はありません。制限と規格との一致を確かめていない点は [docs/architecture.md](docs/architecture.md) の「J1939 の既知の制限」と「J1939」の各節にあります。
- 仮想 LIN バスには、故障注入と記録の道具がありません。LIN は仮想時間だけで動きます（実時間の `Peer` では `std::logic_error`）。
- `LocalBench`（`vehicle_sil::local`）と `vehicle_sil::testkit` の API は固まっていません。
- 対応するのはクラシック CAN（8 バイト以下）だけです。RTR、CAN FD、CAN XL のフレームは、受信時に捨てて件数を数えます。
- `install()` とパッケージ化（`find_package`）には未対応です。使えるのは FetchContent と `add_subdirectory` だけです。
- 物理マシンの Windows 10/11 では未確認です。
- Vector XL CAN Bridge の、Vector のドライバとハードウェアでの動作は、まだ確認していません。
- Vector XL CAN Bridge が扱うのはクラシック CAN だけです。CAN FD には対応していません。

## 開発者向け

ドキュメントの管理ルールは [AGENTS.md](AGENTS.md) にあります。

## ライセンス

vehicle-sil は MIT License で公開しています（[LICENSE](LICENSE)）。

依存ライブラリのライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。

vehicle-sil は、Vector Informatik や SAE International とは関係のない、独立したプロジェクトです。文中の製品名と規格名は、それぞれの権利者のものです。
