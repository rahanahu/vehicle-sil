# アーキテクチャ

ECU を作る人、ECU をまとめる人、vehicle-sil 自体を直す人が、最初に全体の形をつかむための文書です。中の仕組み（スレッド、順序、不変条件）は、下の「[中の仕組み](#中の仕組み)」に挙げた文書に分けています。直す人は、[code-map.md](code-map.md)（どこに何があるか）の次にこれを読み、その後に [dependencies.md](dependencies.md) と、変える場所の中の仕組みの文書を読みます。

vehicle-sil は、Vector SIL Kit の上で車載通信のシミュレーションピアを作るためのライブラリ群と道具です。対象は CAN（J1939 と生の CAN）と LIN です。ピアは Windows（MinGW）または Linux の上で、独立したプロセスとして動きます。同じ ECU のコードを、SIL Kit を使わないプロセス内の仮想バスでも動かせます。

## 全体の形

ECU ごとに実行ファイルを作り（`ecu_main()`）、まとめる人が構成ファイルで 1 つのシミュレーションにします（`vehicle_sil_launch run`）。ランチャーは SIL Kit の registry と controller を自分のプロセスに持ち、ECU の実行ファイルを子プロセスで起動します（`manual` の participant は利用者が起動します）。

```mermaid
flowchart LR
    ecu["ECU executables<br>(ecu_main)"] -- "CAN, LIN, time sync, variables" --- silkit(["SIL Kit"])
    launch["vehicle_sil_launch run<br>(registry, controller, Hub)"] --- silkit
    bridge["Vector XL CAN Bridge<br>(real time)"] --- silkit
    bridge --- bus["physical CAN,<br>Vector Virtual CAN"]
    client["Python, CLI, GUI server"] -- "gRPC (Control API)" --> launch
```

- バスと仮想時間は SIL Kit が運びます。ECU の変数（Control Agent）も SIL Kit の publish/subscribe でランチャーの Hub に届きます（[internals/control.md](internals/control.md)）。
- 外からの操作は Control API（gRPC）で、ランチャーの Control Server（`run --control`）が受けます（[control-api.md](control-api.md)）。
- GUI は別のプロセスです。GUI サーバー（`gui/server`）がランチャーを動かし、Control API のクライアントになって、ブラウザの画面に中継します（[gui.md](gui.md)）。
- 実機の CAN には、Vector XL CAN Bridge が実時間でつなぎます（[bridges.md](bridges.md)）。

## 構成

```
vehicle-sil/
├── libs/core/        vehicle_sil::core   SIL Kit participant、lifecycle、他参加者の監視、時間ゲート
├── libs/lin/         vehicle_sil::lin    LIN の Master と Slave（スケジュールテーブル、フレームの送受信、診断フレーム、Sleep/Wakeup）、LIN のポートの抽象、プロセス内の仮想 LIN バス（SIL Kit と J1939 に依存しない）
├── libs/lin_silkit/  vehicle_sil::lin_silkit  vehicle_sil::lin のポートの SIL Kit 実装（仮想時間）
├── libs/can/         vehicle_sil::can    バックエンドに依存しない CAN の基盤（フレーム、ポート、ホスト、ステップドライバー、仮想バス。SIL Kit に依存しない）
├── libs/can_silkit/  vehicle_sil::can_silkit  vehicle_sil::can のポートの SIL Kit 実装（仮想時間と実時間）
├── libs/can_netsim/  vehicle_sil::can_netsim  簡易 CAN Network Simulator（SIL Kit の Network Simulator API）
├── libs/j1939/       vehicle_sil::j1939  J1939 スタック（vehicle_sil::can の上。SIL Kit に依存しない）と、そのプロトコル部品（parts/）、型付きメッセージ（codec/）
├── libs/ecu/         vehicle_sil::ecu    ECU のアプリケーションを組み立てる入口 Ecu（変数の宣言と、自己申告 describe() とその JSON を含む。SIL Kit に依存しない）
├── libs/local/       vehicle_sil::local  プロセス内の仮想 CAN バスと仮想 LIN バスの上で ECU を仮想時間で回す LocalBench（SIL Kit に依存しない）
├── libs/testkit/     vehicle_sil::testkit  LocalBench の上の J1939 の試験の記録と故障注入（SIL Kit に依存しない）
├── libs/peer/        vehicle_sil::peer   1 つの participant と、その J1939、生の CAN、LIN のノード、Control Agent（監視の出力を含む）の組み立てと実行。Ecu の実装
├── libs/simulation/  vehicle_sil::simulation  ECU を仮想バスか SIL Kit で動かす Simulation と、ECU の実行ファイルの main()（ecu_main）
├── libs/process/     vehicle_sil::process  子プロセス（ログファイルへの出力、標準出力と標準エラーの取り込みと時間切れ、行ごとの受け渡しと止める要求とプロセスの木ごとの終了。POSIX と Win32。内部用）
├── libs/launch/      vehicle_sil::launch  シミュレーションの構成ファイル、ECU の実行ファイルの自己申告との照合、シミュレーション全体の実行（内部用）
├── libs/control/     vehicle_sil::control  Control Agent と Control Server の中核（Hub）、両者の間のメッセージのバイト列の形式。通信と SIL Kit に依存しない
├── libs/control_silkit/ vehicle_sil::control_silkit  Agent と Hub の間を SIL Kit の publish/subscribe で運ぶ層（AgentTransport、HubTransport）
├── libs/control_grpc/ vehicle_sil::control_grpc  Control API の gRPC の層（クライアント向けの ControlServer。VEHICLE_SIL_BUILD_CONTROL_SERVER のときだけ）
├── proto/            Control API の定義（vehicle_sil/control/v1/control.proto）
├── python/vehicle_sil_control/ Control API の Python クライアントと CLI、`vehicle_sil_launch run --control` を子プロセスで動かす助け
├── bridges/vector-xl/ Vector XL CAN Bridge（転送のコアは移植可能、XL バックエンドと実行ファイルは Windows 専用）
├── tools/common/     実行ファイル共通のコマンドライン解析、終了コード、ログ出力（内部用、ヘッダーのみ）
├── tools/sim_controller/ 仮想時間のシミュレーションを開始・停止する system controller（vehicle_sil_sim_controller）
├── tools/can_bus_simulator/ 簡易 CAN Network Simulator の実行ファイル（vehicle_sil_can_bus_simulator）
├── tools/launch/     構成ファイルの照合とシミュレーション全体の実行をする vehicle_sil_launch
├── tools/j1939_codegen/ YAML の定義から J1939 の型付きメッセージ（C++ のヘッダー）を作る生成器（Python。ビルドのときに vehicle_sil_add_j1939_codec() が呼ぶ）
├── examples/getting_started/ 最初の例: Ecu& を受け取る 2 つの ECU を Simulation で動かす
├── examples/         やりたいこと別のサンプル（typed_messages、pid_stream、fault_injection、vehicle、low_level など。索引は examples/README.md）
├── examples/j1939/   部品で作った 2 つの ECU（ecu_pair。Simulation で動かし、試験も同じコードを使う）、独自プロトコルのパラメーターサービス（param_service）と、最小ピア（実時間の Peer で送る sender と、SIL Kit の CAN で直接受けて照合する receiver）
├── examples/virtual_time/ 仮想時間のピア（Peer の上の物理モデル、tick() 型の既存 ECU を載せた J1939 ECU、LIN Master と Slave）
├── examples/lin/     LIN クラスタのピア（Master と、役割の違う Slave。フレームの送受信と Sleep/Wakeup）
├── examples/ecu_main/ ECU ごとの実行ファイル（ecu_main）と、それをまとめる構成ファイル、シナリオ、ダッシュボード
├── examples/launch_control/ ランチャーの Control Server に Python からつなぐ例（write_before_start.py、pause_and_step.py）
├── examples/gui_client/ GUI サーバーの口を JavaScript から使う例
├── examples/gui_starter/ GUI サーバーに配らせる自分の画面のひな形
├── examples/fetchcontent_consumer/   外部プロジェクトからの利用例
├── tests/            ユニットテスト、統合テスト、FetchContent テスト、XL ドライバのフェイク（tests/vector_xl/fake_xl）
├── external/         SIL Kit（サブモジュール）
├── patches/          SIL Kit へのパッチ（ビルドツリーのコピーに当てる。上流への提出用の形式）
├── gui/server/       ブラウザの GUI のサーバー（Python、vehicle-sil-gui。ランチャーを動かし、HTTP と WebSocket の口を出す）
├── gui/protocol/     GUI サーバーの HTTP と WebSocket の口の契約と例
├── gui/client/       GUI サーバーのクライアントのライブラリ（TypeScript）
├── gui/web/          サンプルの画面（Angular）
├── template/         ECU のプロジェクトのひな形（ビルド済みの道具を取ってくる get_tools.py を含む）
├── scripts/          開発と CI のスクリプト（verify.py、package.py など。AGENTS.md の「検証とスクリプト」）
├── cmake/            依存の取り込み、試験の登録、境界の検査
└── docs/
```

空のディレクトリや空のターゲットは作りません。

区分は、どこから使い始めるかの目安です。利用者向けは、ECU を書く人と ECU をまとめる人が直接使うライブラリで、入口は `vehicle_sil::Ecu` と `vehicle_sil::Simulation` です。土台は利用者向けのライブラリを通して使うもので、直接も使えますが、API は上の層の必要に合わせて変わります。試験用は、試験とサンプルのための道具です。実行ファイル、生成コード、CAN ブリッジは区分に入れていません（「—」）。

各ターゲットがリンクするもの（依存の向き）は、[code-map.md](code-map.md) の「依存の向き」の図にあります。

| ターゲット | 区分 | 内容 |
|---|---|---|
| `vehicle_sil::sim_base` | 土台 | `SimTime` と `SimDuration`、`StepInbox`、`periodic()` と `once_at()`（ヘッダーのみ。SIL Kit なしで使う） |
| `vehicle_sil::core` | 土台 | `vehicle_sil::ParticipantRuntime`、`SimulationController`、`TickScheduler`、`RealTimePacer`。`sim_base` の型も使える |
| `vehicle_sil::lin` | 土台 | `vehicle_sil::lin::LinMaster`、`vehicle_sil::lin::LinSlave`、`LinScheduleTable` と `validate_schedule()`、`LinPort`、`VirtualLinBus`（[internals/lin.md](internals/lin.md)） |
| `vehicle_sil::lin_silkit` | 土台 | `vehicle_sil::lin::SilKitLinPort` |
| `vehicle_sil::can` | 土台 | `vehicle_sil::can::Frame`、`Port`、`Steppable`、`Host`、`StepDriver`、`RealTimeDriver`、`VirtualCanBus`、`CanBusModel`、`TapPort`（[internals/can.md](internals/can.md)） |
| `vehicle_sil::can_silkit` | 土台 | `vehicle_sil::can::SilKitCanPort`、`bind_host()` |
| `vehicle_sil::can_netsim` | 土台 | 簡易 CAN Network Simulator（[internals/can.md](internals/can.md)） |
| `vehicle_sil::j1939` | 利用者向け | `vehicle_sil::j1939::Network`、`Node`、`Pgn`、`Identifier`、`Observer`、道具箱（[internals/j1939.md](internals/j1939.md)） |
| `vehicle_sil::j1939_broadcast_suppression`、`vehicle_sil::j1939_diagnostics` | 利用者向け | プロトコル部品の `BroadcastSuppression`（DM13）、`Diagnostics`、`Identification`（[internals/j1939.md](internals/j1939.md) の「プロトコル部品」） |
| `vehicle_sil::j1939_periodic_sender`、`vehicle_sil::j1939_requester`、`vehicle_sil::j1939_monitor`、`vehicle_sil::j1939_address_commander`、`vehicle_sil::j1939_exchange` | 利用者向け | ECU アプリ向けのプロトコル部品 `PeriodicSender`、`Requester`、`Monitor`、`AddressCommander`、独自プロトコル向けの `Exchange`（[internals/j1939.md](internals/j1939.md) の「プロトコル部品」、[j1939-guide.md](j1939-guide.md)） |
| `vehicle_sil::j1939_codec`（ヘッダーのみ） | 利用者向け | 型付きメッセージのランタイム（`codec.hpp`。エンジンの値の型だけを使う）と、ノードと `PeriodicSender` への接続（`codec_node.hpp`）。型は `vehicle_sil_add_j1939_codec()` が YAML から生成する（[internals/j1939.md](internals/j1939.md) の「型付きメッセージ（`vehicle_sil::j1939_codec`）」、[j1939-codec.md](j1939-codec.md)） |
| `vehicle_sil::ecu` | 利用者向け | `vehicle_sil::Ecu`、`EcuDescription` と `to_json()`、`detail::OwnedObjects`、`detail::EcuAgent`（[internals/execution.md](internals/execution.md)、[ecu-executable.md](ecu-executable.md)） |
| `vehicle_sil::local` | 土台 | `vehicle_sil::LocalBench`（[internals/execution.md](internals/execution.md)） |
| `vehicle_sil::testkit` | 試験用 | `J1939Events`、`FrameLog`、`faults`、`LocalBench` の別名 `testkit::LocalCan`（[internals/execution.md](internals/execution.md)） |
| `vehicle_sil::peer` | 土台 | `vehicle_sil::Peer`（Control Agent の監視の出力を含む）、`start_together()`（[internals/execution.md](internals/execution.md)） |
| `vehicle_sil::simulation` | 利用者向け | `vehicle_sil::Simulation`、`SilKitOptions`、`SimulationError`（[internals/execution.md](internals/execution.md)）、`ecu_main()` と `EcuMainOptions`（[ecu-executable.md](ecu-executable.md)） |
| `vehicle_sil_sim_controller`（実行ファイル） | — | system controller の CLI |
| `vehicle_sil::process`（内部用） | — | 子プロセス: `ChildProcess`（ログファイルへ）、`run_captured()`（標準出力と標準エラーを別々に取り込み、時間切れで止める）、`Process`（標準出力と標準エラーを行ごとに渡す。作業ディレクトリ、止める要求、起動したプロセスごとの終了）。`run_captured()` と `Process` の子は、自分のプロセスグループ（Windows ではプロセスグループとジョブオブジェクト）で動き、標準の 3 つのほかにハンドルを受け継がず、Linux では呼んだスレッドが終わると SIGKILL を受けます。POSIX は `fork` と `exec`（`ChildProcess` は `posix_spawn`）、Windows は `CreateProcessW`（パスと引数は UTF-8 から変換する） |
| `vehicle_sil::launch`（内部用。SIL Kit をサブモジュールからビルドしたとき） | — | 構成ファイルの読み込み、`--describe` の JSON の読み込み、照合の規則（プロセスを起動しない関数）、実行（registry と `SimulationController` をプロセスの中に持ち、participant を子プロセスで動かす。実行の判定はプロセスを起動しない関数）、人向けと JSON の出力（[simulation-config.md](simulation-config.md)） |
| `vehicle_sil_launch`（実行ファイル） | — | `vehicle_sil_launch check`、`vehicle_sil_launch run`（[simulation-config.md](simulation-config.md)）。`-DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON` のビルドでは `run --control` の Control Server も持つ |
| `vehicle_sil::control` | 利用者向け | `vehicle_sil::control::Agent`、`Hub`、`wire`（[internals/control.md](internals/control.md)） |
| `vehicle_sil::control_silkit` | 利用者向け | `vehicle_sil::control_silkit::AgentTransport`、`HubTransport`（[internals/control.md](internals/control.md) の「SIL Kit の上の運び手」） |
| `vehicle_sil::control_proto`（`VEHICLE_SIL_BUILD_CONTROL_SERVER` のとき） | — | `control.proto` から生成したコード（`control_grpc` の内部） |
| `vehicle_sil::control_grpc`（同上） | 利用者向け | `vehicle_sil::control_grpc::ControlServer`、`time_control_of()` |
| `vehicle_sil::vector_xl_bridge_core` | — | `vehicle_sil::vector_xl::CanBridge`、`ExternalCanPort`、`ExternalPortAdapter`、チャネル選択、CLI の解析。全プラットフォームでビルドする |
| `vehicle_sil::vector_xl`（Windows のみ） | — | `vehicle_sil::vector_xl::XlDriver`、`XlCanPort` |
| `vehicle_sil_vector_xl_bridge`（Windows のみ、実行ファイル） | — | ブリッジの CLI |

利用側は必要なターゲットだけをリンクします。依存ライブラリは `EXCLUDE_FROM_ALL` で追加しているので、リンクしないものはビルドされません（外部プロジェクトから取り込んだときは vehicle-sil のライブラリも同じです）。

主なクラスは次のとおりです。

| クラス | 責務 |
|---|---|
| `vehicle_sil::ParticipantRuntime` | SIL Kit の participant、lifecycle（autonomous または coordinated）、SystemMonitor、時刻同期（任意）を RAII で所有する。他参加者の状態（Running か、終了したか）を待てる |
| `vehicle_sil::SimulationController` | coordinated で起動するシミュレーションの system controller。必要な参加者がそろったら一斉に開始させ、指定した仮想時刻で停止させる |
| `vehicle_sil::StepInbox` | SIL Kit のスレッドで届いたメッセージをステップの境界までため、決まった順序で渡す |
| `vehicle_sil::TickScheduler` | 1 つの participant の中で、固定周期のタスク（既存 ECU の `tick()` など）をステップから呼ぶ |
| `vehicle_sil::RealTimePacer` | 仮想時間を壁時計に一定の比率で結合するための待ち時間を計算する |
| `vehicle_sil::Ecu` | ECU のアプリケーションを組み立てる入口（J1939 の `Network`、生の CAN、LIN の Master と Slave、ステップごとの処理、ECU が持つオブジェクト）。`Peer` と `LocalBench` の ECU が実装する（[internals/execution.md](internals/execution.md)） |
| `vehicle_sil::Simulation` | `Ecu` を仮想バスか SIL Kit の仮想時間で動かし、失敗を `SimulationError` で報告する（[internals/execution.md](internals/execution.md)） |
| `vehicle_sil::Peer` | 1 つの participant と、その J1939 の `Network`、LIN の Master と Slave、ステップごとのアプリの処理を組み立てて動かし、正しい順序で片付ける。`Ecu` を実装する（[internals/execution.md](internals/execution.md)） |
| `vehicle_sil::lin::LinMaster` | LIN のスケジュールテーブルを仮想時間で実行し、ヘッダーごとの結果（応答の衝突は Error）を返す。Master が送るフレーム、診断のマスターリクエスト、go-to-sleep コマンド、wakeup パルスを送る |
| `vehicle_sil::lin::LinSlave` | ステップで設定された値で Master のヘッダーに応答し（診断のスレーブレスポンスは 1 回だけ）、Master が送るフレームを受信する。休止と起床の状態を管理する |
| `vehicle_sil::lin::LinPort` | LIN のノードがバスに触れる唯一の口。実装は `VirtualLinBus` のエンドポイントと `SilKitLinPort` |
| `vehicle_sil::lin::VirtualLinBus` | プロセス内の LIN バス。送った側の呼び出しの中で、同期に配る。`LocalBench` が LIN のネットワーク名ごとに 1 つ作る |

`ParticipantRuntime` の契約、所有と終了の順序、スレッドとキューは [internals/core.md](internals/core.md) にあります。

### 共通化の規則

- `core` に置くのは、どのプロトコルでも同じになる処理だけです。具体的には、SIL Kit への接続、lifecycle、他参加者の監視、仮想時間（ステップ、受信のバッファ、周期タスク、実時間との結合、時間ゲート）、失敗の判定（`FailureCheck`）です。外部操作（Control Agent と Hub）は `control` にあります。Control API の時間の操作は `core` の時間ゲートを使います。
- `core` は CAN、LIN、J1939 に依存しません。バスのコントローラ（`ICanController` など）は、利用側が `ParticipantRuntime::participant()` から SIL Kit の API で作ります。
- J1939 と LIN のプロトコル処理は共通化しません。
- 将来のためだけの抽象クラスや空の実装は作りません。

## 設計の決まり

### 時間の扱い

- 実時間（壁時計）とシミュレーション時間（SIL Kit の仮想時刻）を、型と名前で区別します（`SimTime`）。
- 時間管理は `core` に置きます。J1939 や LIN の内部には埋め込みません。
- 一時停止とステップ実行は、`SimulationController` の時間ゲートで行います（[internals/virtual-time.md](internals/virtual-time.md) の「一時停止、ステップ実行、速度（時間ゲート）」）。Control API はこれを操作します。
- 受信ハンドラから反応的に送ったメッセージは、SIL Kit の到着保証の対象外です。新しいプロトコルを仮想時間に載せるときは、送信をステップの中で行うか、LIN と同じように到着の上限を示してから使います。

### 実行モードと通信バックエンド

- 実行モード（仮想時間か実時間か。`PeerOptions::virtual_time`）と、バスの口（SIL Kit のポートか、利用者が渡すポート。Vector XL の `ExternalPortAdapter` など）は、別々に選びます。
- 実時間でも、SIL Kit の中だけで動かせます（J1939 の最小ピア `examples/j1939/sender`）。
- 仮想時間を壁時計に合わせるのは、ピアの `VirtualTimeOptions::real_time_factor` か、時間ゲートの速度です（[internals/virtual-time.md](internals/virtual-time.md) の「実時間との結合」）。
- 物理 CAN と Vector Virtual CAN は実時間で動かします。仮想時間のシミュレーションには参加させません（[bridges.md](bridges.md) の「物理 CAN と仮想時間」）。

## 中の仕組み

vehicle-sil 自体を直す人と、契約や制限を確かめたい人が、必要なものだけを読みます。

| 文書 | 内容 |
|---|---|
| [internals/core.md](internals/core.md) | `ParticipantRuntime` の契約、所有と終了の順序、スレッドとキュー、実時間モードの時刻 |
| [internals/can.md](internals/can.md) | CAN の共通基盤: ポート、事象、`Steppable`、ホスト、ドライバー、仮想バス、`TapPort`、SIL Kit のポート、バスのタイミング、簡易 CAN Network Simulator |
| [internals/j1939.md](internals/j1939.md) | J1939 スタック: エンジン、Address Claim、トランスポートプロトコル、道具箱、プロトコル部品、型付きメッセージ、`Observer`、資源の上限、既知の制限 |
| [internals/lin.md](internals/lin.md) | LIN の Master と Slave、スケジュール、診断フレーム、休止と起床、ポートと仮想 LIN バス |
| [internals/execution.md](internals/execution.md) | `Ecu`、`Simulation`、`Peer`、`LocalBench` と試験キット（1 ステップの順序、所有と破棄、バスからの切り離し） |
| [internals/virtual-time.md](internals/virtual-time.md) | 共通仮想時間: 進み方、実時間との結合、時間ゲート（一時停止、ステップ実行、速度、遅れ）、受信の順序、固定周期 `tick()` の統合 |
| [internals/control.md](internals/control.md) | Control API の中: Control Agent、Hub、SIL Kit の上の運び手、Control Server、ランチャーの Control Server、シナリオ |
| [bridges.md](bridges.md) | Vector XL CAN Bridge: 使い方、転送の契約、バスの監視、スレッド、検証の範囲 |
