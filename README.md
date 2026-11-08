# vehicle-sil

[Vector SIL Kit](https://github.com/vectorgrp/sil-kit) の上で、車載通信のシミュレーションピアを作るための C++20 ライブラリです。J1939 over CAN と LIN に対応しています。ピアは Windows と Linux の独立したプロセスとして動きます。

> [!NOTE]
> 使えるのは、vehicle-sil 自身の J1939 スタック（開発中。単一フレーム、Request と Acknowledgement、Address Claim、トランスポートプロトコル、診断、ECU アプリ向けの部品）、SIL Kit と Vector XL Driver をつなぐ CAN ブリッジ（実機は未検証）、LIN の Master/Slave（スケジュールテーブル、フレームの送受信、診断フレーム 0x3C/0x3D、応答の衝突の検出、Sleep/Wakeup）と、それらを同期させる共通仮想時間です。未実装の項目と制約は [docs/architecture.md](docs/architecture.md) にあります。

## ライブラリ

| CMake ターゲット | 内容 |
|---|---|
| `vehicle_sil::core` | `vehicle_sil::ParticipantRuntime`（SIL Kit の participant、lifecycle、他参加者の状態、仮想時間のステップを RAII で管理する）、`SimulationController`（仮想時間のシミュレーションの開始と停止）、`SimTime`、`StepInbox`、`TickScheduler`。プロトコルに依存しない |
| `vehicle_sil::lin` | `vehicle_sil::lin::LinMaster`（スケジュールテーブル、Master が送るフレーム、マスターリクエスト、go-to-sleep と wakeup）と `vehicle_sil::lin::LinSlave`（ステップで設定した値で応答、スレーブレスポンス、Master のフレームの受信、休止と起床）。仮想時間で使う。`vehicle_sil::core` を含み、J1939 には依存しない |
| `vehicle_sil::can`（開発中） | バックエンドに依存しない CAN の基盤。フレームの型、ポートの契約、複数のプロトコル処理をステップで回す `Host`、仮想時間の `StepDriver`、実時間の `RealTimeDriver`、テスト用の仮想バス（調停と送信時間の模擬、故障注入、ECU 単位の通信断を選べる）、生フレームを読み取り専用で観測する `TapPort`。SIL Kit に依存しない。J1939 スタックの土台で、API はまだ変わることがある |
| `vehicle_sil::can_silkit`（開発中） | `vehicle_sil::can` のポートの SIL Kit 実装（仮想時間と、時刻同期なしの実時間）と、`Host` を `ParticipantRuntime` のステップにつなぐ `bind_host()` |
| `vehicle_sil::can_netsim`（開発中） | 簡易 CAN Network Simulator: SIL Kit の Network Simulator API で、CAN の調停、送信時間、バスの占有を仮想時間で模擬する（`CanBusSimulator`）。Vector SIL Kit Network Simulator と共用の設定ファイルの読み込み |
| `vehicle_sil::j1939`（開発中） | J1939 スタック（名前空間 `vehicle_sil::j1939`、ヘッダー `vehicle_sil/j1939/`）。単一フレームの送受信、Request、Acknowledgement、NAME と Address Claim（要求、競合、Cannot Claim、任意アドレスの選択、アドレス表、アドレス違反、Commanded Address の処理、状態の通知）、トランスポートプロトコル（BAM、RTS/CTS）、部品を作る道具箱（`Subscription`、`Timer`、`Node::attach`）、読み取り専用の観測口（`Observer`）。SIL Kit に依存しない |
| `vehicle_sil::j1939_broadcast_suppression`、`vehicle_sil::j1939_diagnostics`（開発中） | `vehicle_sil::j1939` の公開 API と道具箱だけで作ったプロトコル部品。DM13 による放送の抑制、DTC と DM1/DM2/DM3/DM11/DM22、識別情報（ソフトウェア ID、ECU ID、製品 ID、診断プロトコル ID） |
| `vehicle_sil::j1939_periodic_sender`、`vehicle_sil::j1939_requester`、`vehicle_sil::j1939_monitor`、`vehicle_sil::j1939_address_commander`（開発中） | ECU アプリ向けの部品（同じく公開 API と道具箱だけで作る）。PG の周期送信（`PeriodicSender`）、Request の送信と応答・ACK・時間切れの報告（`Requester`）、受信の最新値と途絶（`Monitor`）、Commanded Address の送信（`AddressCommander`）。使い方は [docs/j1939-guide.md](docs/j1939-guide.md) |
| `vehicle_sil::j1939_codec`（開発中） | J1939 の型付きメッセージ。YAML で PG と信号（位置、符号、スケーリング、範囲、Not Available と Error、列挙、Proprietary A/B）を定義すると、`vehicle_sil_add_j1939_codec()` がビルドのときに C++ の型を生成する。ビット操作を書かずに、信号を値として送受信する。使い方は [docs/j1939-codec.md](docs/j1939-codec.md) |
| `vehicle_sil::peer`（開発中、内部向け） | 1 つの SIL Kit participant と、その J1939 の `Network`（SIL Kit の CAN か外部のポート）と LIN の Master/Slave をまとめて作り、仮想時間か実時間で動かす `vehicle_sil::Peer` |
| `vehicle_sil::testkit`（開発中、内部向け） | SIL Kit なしの J1939 の試験の組み立て: 仮想バスの上の `Network` を名前で組み立てて回す `LocalCan`、出来事と生フレームの記録（`J1939Events`、`FrameLog`）、故障注入の関数（`faults`） |
| `vehicle_sil::vector_xl_bridge_core` | CAN ブリッジのコア（`vehicle_sil::vector_xl::CanBridge` など）と、外部 CAN ポートを `vehicle_sil::can` の実時間のポートにする `ExternalPortAdapter`。プラットフォームに依存しない |
| `vehicle_sil::vector_xl` | Vector XL Driver のバックエンド（`XlDriver`、`XlCanPort`）。Windows 専用で、XL Driver Library が必要 |

## 構成

| パス | 内容 |
|---|---|
| `libs/` | ライブラリ（`core`、`can`、`can_silkit`、`can_netsim`、`j1939`、`lin`、`testkit`、`peer`） |
| `tools/sim_controller` | 仮想時間のシミュレーションを開始・停止する `vehicle_sil_sim_controller` |
| `tools/can_bus_simulator` | 簡易 CAN Network Simulator の participant `vehicle_sil_can_bus_simulator` |
| `tools/j1939_codegen` | YAML の定義から J1939 の型付きメッセージを生成する `j1939_codegen.py` |
| `bridges/vector-xl` | Vector XL CAN Bridge（ライブラリと実行ファイル `vehicle_sil_vector_xl_bridge`） |
| `examples/j1939` | 部品で作った 2 つの ECU を 2 つの participant で動かす `vehicle_sil_j1939_ecu_pair`、最小ピア `vehicle_sil_j1939_sender`（`Peer` を実時間で動かして送る）/ `vehicle_sil_j1939_receiver`（SIL Kit の CAN で直接受けて照合する） |
| `examples/virtual_time` | 仮想時間のピア（どれも `Peer` の上）: 物理モデル、既存 ECU（共有ライブラリの `tick()`）を載せた J1939 ECU、LIN センサー、ボディコントローラー |
| `examples/lin` | LIN クラスタのピア `vehicle_sil_lin_master` / `vehicle_sil_lin_slave`（フレームの送受信と Sleep/Wakeup） |
| `examples/fetchcontent_consumer` | 外部 CMake プロジェクトからの利用例 |
| `tools/` | `vehicle_sil_sim_controller`（仮想時間のシミュレーションの開始と停止）と、実行ファイル共通のコマンドライン処理（内部用） |
| `tests/` | ユニットテスト、複数プロセスの統合テスト、FetchContent テスト |
| `external/` | SIL Kit（Git サブモジュール、コミット固定） |
| `patches/` | SIL Kit に当てるパッチ（上流への提出用の形式） |
| `docs/` | [J1939 のノードの作り方](docs/j1939-guide.md)、[J1939 の型付きメッセージ](docs/j1939-codec.md)、[アーキテクチャと将来の制約](docs/architecture.md)、[依存ライブラリ](docs/dependencies.md) |

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

`vehicle_sil::lin` は、仮想時間で動く LIN の Master と Slave です（J1939 には依存しません）。`examples/lin` の 3 つのピアで 1 つのクラスタを作れます。

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

自分のピアでは、次のように使います。

```cpp
using namespace vehicle_sil::lin;
// Master: 0x10 は Master が送り、0x20 は Slave が応答する
LinMaster master(*runtime.participant().CreateLinController("LIN1", "LIN_NET"),
                 LinScheduleTable{{0x10, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Master},
                                  {0x20, 2, LinChecksumModel::Enhanced, 10ms, LinPublisher::Slave}},
                 LinMasterOptions{5ms}, [](const LinScheduleResult& r) { /* スロットの終わりに届く */ });
runtime.set_communication_ready_handler([&] { master.init(); });
runtime.set_step_handler([&](vehicle_sil::SimTime now, vehicle_sil::SimDuration) {
    master.set_frame_data(0x10, command);  // 必要なら request_sleep() / request_wakeup()
    master.step(now);
});

// Slave（別のピア）: 0x10 を受信し、0x20 に応答する
LinSlave slave(*controller, LinSlaveOptions{{0x10}}, [](const LinReceivedFrame& f) { /* ヘッダーの時刻付き */ });
runtime.set_communication_ready_handler([&] { slave.init(); });
runtime.set_step_handler([&](vehicle_sil::SimTime now, vehicle_sil::SimDuration) {
    slave.step(now);                        // 受信、休止と起床の処理
    slave.set_response(now, 0x20, value);  // 以後のヘッダーへの応答
});
```

スケジュール、フレームの送受信、診断フレーム（0x3C、0x3D）、Sleep/Wakeup の規則と制約は、[docs/architecture.md](docs/architecture.md) の「LIN を仮想時間で動かす」を参照してください。

## Vector XL CAN Bridge（Windows）

`vehicle_sil_vector_xl_bridge` は、SIL Kit の CAN ネットワークと Vector XL の CAN チャネルの間で、クラシック CAN のフレームを双方向に転送します。XL のチャネルには、Vector Virtual CAN と、CANcase や VN16xx などの物理 CAN を使えます。

> [!WARNING]
> Windows と Vector のハードウェアでは、まだ動作を確認していません。Linux 上で、XL ドライバのフェイクを使った自動テストだけで確認しています。

### ビルド

[XL Driver Library](https://www.vector.com/)（`vxlapi.h` と `vxlapi64.lib`）を Vector から入手し、その場所を指定してビルドします。XL Driver Library は vehicle-sil には含まれていません。

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
| `unit` | コマンドライン解析。`core`、`lin`、`can`、`j1939`、`peer`、`testkit`、`vector_xl` の単体テストにもこのラベルが付く |
| `integration` | `sil-kit-registry`、receiver、sender（実時間の `Peer` と J1939 スタック）を別プロセスで起動し、終了コードで判定する。`address-claim-and-pgn` は Address Claim と PGN の CAN ID とペイロードの一致を、`receiver-timeout` はタイムアウト処理を検証する。`receiver-detects-wrong-name` と `receiver-detects-wrong-payload` は、receiver が誤った NAME と壊れたペイロードを失敗として報告することを確かめる |
| `vector_xl` | CAN ブリッジのテスト。全プラットフォームで、テスト専用の XL ドライバのフェイクに対して本番のコードを動かす。CanBridge の単体テスト、XL バックエンドのテスト、実際の SIL Kit を使うプロセス内のテスト、ブリッジの実行ファイルを使う E2E テストがある。E2E では、エコーノードとの往復（Virtual CAN と、ビットレートを設定する物理 CAN）、送信レシートが返ってくる場合のループ防止、`--list-channels`、存在しないチャネル、引数の誤り、ドライバがない場合を確かめる |
| `can` | `vehicle_sil::can` の単体テスト（SIL Kit なしでビルドする）、SIL Kit のポートのテスト（`can_silkit.*`）、簡易 CAN Network Simulator のテスト（`can_netsim.*`）。フレームの検証、仮想バスの受信順序とあふれ、ホストの段階の順序と失敗、ステップドライバーの順序、送信時間と調停（11 ビット / 29 ビット）、混雑、送信キュー、受信側のステップ幅によらないこと、ポートの事象（送信の結果、受け箱のあふれ、バス状態、エラーフレーム）、故障注入と ECU 単位の通信断、`post()`、時間の領域の混在の拒否、実時間のドライバー（差し替えた時計での決定的な試験と、壁時計での到着と順序）を確かめる。簡易 CAN Network Simulator では、プロセス内の participant と実行ファイルの両方で、仮想バスと同じ受信の時刻になること、繰り返しても同じ結果になること、送信時刻のない送信者、設定ファイルの読み込みを確かめる。プロセス内のレジストリで、同じシナリオのトレースが仮想バスと SIL Kit で一致すること（ステップ幅の組み合わせを変えて）と、送信の続く中で participant を決まった順序で止めて破棄できることを確かめる（`unit` ラベルも付く） |
| `j1939` | `vehicle_sil::j1939` の単体テスト（SIL Kit なしでビルドする）と SIL Kit 上の試験（`j1939_silkit.*`）。PGN と識別子、単一フレームの送受信、宛先の振り分け、ローカル折り返し、同じ時刻のフレームの順序とタイマーの発火がステップ幅と到着順によらないこと、Request への応答と ACK/NACK、NAME と Address Claim（複数ノードと複数 Network の競合、任意アドレスの選択、アドレス表、アドレス違反、Commanded Address を含む）、1 つのホストの複数の Network（ゲートウェイ、片方の失敗）、再入と失敗、ステップ幅の検査、資源の上限、定常状態のステップでメモリを確保しないことを確かめる。プロセス内のレジストリで、同じシナリオのトレースが仮想バスと SIL Kit で一致することも確かめる（簡易 CAN Network Simulator ありでも。`unit` ラベルも付く）。タイミングありの仮想バスで、プロトコルの結果が変わらないこと、混雑で転送が遅れ、バスが飽和すると時間切れになることも確かめる。1 つの ECU がバスから切り離されたとき（受信だけ、送受信とも）の TP の時間切れと回復、隠れていたアドレスの衝突の解決、時刻同期なしの SIL Kit での実時間の動作も確かめる。道具箱（解除と破棄の再入、Network の後の破棄、タイマーの起点と予約）とプロトコル部品（`j1939_parts.*`: DM13、診断、識別情報、送信の競合、後で返す ACK、ステップ幅によらない結果）も確かめ、`j1939_parts.boundary` が部品がエンジンの内部を使っていないことを検査する。型付きメッセージ（`j1939_codec.*`）は、ビット配置をビットごとの実装と照合し、raw 値と物理値の変換、丸めと範囲の端、Not Available と Error、列挙、セレクター、長さ、小さい信号のすべての raw 値の往復、ノードでの送受信を確かめる。`j1939_codec.generator` は生成器が誤った定義を止めることを確かめる |
| `peer` / `testkit` | `Peer` をプロセス内のレジストリで動かす試験（J1939 と LIN の 3 つの participant、仮想バスと同じ結果、実時間と外部ポート、組み立ての誤り、失敗の報告）と、試験キットの試験（アドレスの喪失、Address Claimed の欠落、TP のパケットの欠落、通信断、生フレームの注入、受信側のステップ幅と記録の有無によらない結果）。`testkit.boundary` と `peer.boundary` がエンジンの内部を使っていないことを検査する |
| `core` / `lin` | 仮想時間の単体テスト。プロセス内のレジストリで、ステップ幅の違う参加者の同期、最も遅い参加者を待つこと、実時間への結合、`TickScheduler`、`StepInbox`、LIN のスケジュール、応答、Master のフレームの受信、休止と起床、スケジュールの切り替えを確かめる（`unit` ラベルも付く） |
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
target_link_libraries(example PRIVATE vehicle_sil::peer vehicle_sil::j1939_periodic_sender)   # core だけなら vehicle_sil::core
vehicle_sil_copy_runtime_dlls(example)   # Windows: SilKit.dll をコピーする（ほかの OS では何もしない）
```

SIL Kit は、vehicle-sil のサブモジュールとして自動で取得・設定されます。利用側で個別に設定する必要はありません。SIL Kit には、ビルドツリーにコピーしたソースに `patches/sil-kit/` のパッチを当てたものを使います（[docs/dependencies.md](docs/dependencies.md)）。パッチの適用に Git を使います。リンクしないライブラリはビルドされません。完全な例は [examples/fetchcontent_consumer](examples/fetchcontent_consumer) にあります。

最小の使い方は次のとおりです。

```cpp
#include <vehicle_sil/j1939/periodic_sender.hpp>
#include <vehicle_sil/peer/peer.hpp>

namespace j1939 = vehicle_sil::j1939;
using namespace std::chrono_literals;

EngineModel model;   // ハンドラが使うものは Peer より先に宣言する（Peer が先に破棄されるように）
vehicle_sil::PeerOptions options;
options.participant = "Engine";
options.registry_uri = "silkit://localhost:8500";
options.virtual_time = vehicle_sil::VirtualTimeOptions{10ms};   // 仮想時間。std::nullopt なら実時間
vehicle_sil::Peer peer(options);
auto& ecu = peer.j1939("J1939_CAN1").add_node(
    {.address = 0x00, .name = j1939::Name{}.set_identity_number(0x2345).set_manufacturer_code(0x7F0)});   // Address Claim はスタックが行う
ecu.attach<j1939::PeriodicSender>(j1939::Pgn{0xFF10}, 100ms,   // アドレスを得てから 100 ms ごとに送る
                                  [&](std::span<std::uint8_t> out) { return model.encode(out); });
peer.every(10ms, [&](vehicle_sil::SimTime) { model.tick(); });
peer.run();   // start() と wait()
```

受信、Request と応答、受信の監視、試験の書き方は [docs/j1939-guide.md](docs/j1939-guide.md) にあります。

信号を YAML で定義して型付きメッセージにするときは、`vehicle_sil_add_j1939_codec(<target> <定義.yaml>)` を呼び、`<target>` をリンクします（Python 3 と PyYAML が要ります。[docs/j1939-codec.md](docs/j1939-codec.md)）。`vehicle_sil::Peer` はまだ内部向けで、API は変わることがあります。

## 既知の制限

- 仮想時間の一時停止とステップ実行は未実装です。
- LIN の Slave は、応答するフレームのスロット長より短いステップ幅で動かす必要があります。LIN の dynamic response など、SIL Kit の experimental API を使っています。
- LIN の Slave が決まったステップで受信できるのは、Master が送るフレームだけです。ほかの Slave の応答（slave-to-slave）も購読した ID なら渡されますが、受け取るステップと時刻は決定論的ではありません。
- 既存 ECU の DLL が内部で実時間（スリープ、時刻の取得）を使っている場合、vehicle-sil はそれを仮想時間に置き換えられません。
- J1939 スタックは開発中です。ETP（1785 バイトを超えるメッセージ）はありません。制限と規格との一致を確かめていない点は [docs/architecture.md](docs/architecture.md) の「J1939 の既知の制限」と「J1939」の各節にあります。
- 対応するのはクラシック CAN（8 バイト以下）だけです。RTR、CAN FD、CAN XL のフレームは、受信時に捨てて件数を数えます。
- `install()` とパッケージ化（`find_package`）には未対応です。使えるのは FetchContent と `add_subdirectory` だけです。
- 物理マシンの Windows 10/11 では未確認です。
- Vector XL CAN Bridge の、Vector のドライバとハードウェアでの動作は、まだ確認していません。
- Vector XL CAN Bridge が扱うのはクラシック CAN だけです。CAN FD には対応していません。

## 開発者向け

ドキュメントの管理ルールは [AGENTS.md](AGENTS.md) にあります。

## ライセンス

依存ライブラリのライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。
