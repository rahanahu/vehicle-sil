# アーキテクチャ

vehicle-sil は、Vector SIL Kit の上で車載通信のシミュレーションピアを作るためのライブラリ群です。対象は J1939 over CAN、LIN、将来の Ethernet などです。ピアは Windows または Linux 上で独立したプロセスとして動きます。

この文書の「将来の機能が守る制約」は要件と制約であり、実装済みの機能ではありません。

## 構成

```
vehicle-sil/
├── libs/core/        vehicle_sil::core   SIL Kit participant、lifecycle、他参加者の監視
├── libs/lin/         vehicle_sil::lin    LIN の Master と Slave（スケジュールテーブル、フレームの送受信、診断フレーム、Sleep/Wakeup）、LIN のポートの抽象、プロセス内の仮想 LIN バス（SIL Kit と J1939 に依存しない）
├── libs/lin_silkit/  vehicle_sil::lin_silkit  vehicle_sil::lin のポートの SIL Kit 実装（仮想時間）
├── libs/can/         vehicle_sil::can    バックエンドに依存しない CAN の基盤（フレーム、ポート、ホスト、ステップドライバー、仮想バス。SIL Kit に依存しない）
├── libs/can_silkit/  vehicle_sil::can_silkit  vehicle_sil::can のポートの SIL Kit 実装（仮想時間と実時間）
├── libs/can_netsim/  vehicle_sil::can_netsim  簡易 CAN Network Simulator（SIL Kit の Network Simulator API）
├── libs/j1939/       vehicle_sil::j1939  J1939 スタック（vehicle_sil::can の上。SIL Kit に依存しない）と、そのプロトコル部品（parts/）、型付きメッセージ（codec/）
├── libs/ecu/         vehicle_sil::ecu    ECU のアプリケーションを組み立てる入口 Ecu（変数の宣言を含む。ヘッダーのみ。SIL Kit に依存しない）
├── libs/local/       vehicle_sil::local  プロセス内の仮想 CAN バスと仮想 LIN バスの上で ECU を仮想時間で回す LocalBench（SIL Kit に依存しない）
├── libs/testkit/     vehicle_sil::testkit  LocalBench の上の J1939 の試験の記録と故障注入（SIL Kit に依存しない）
├── libs/peer/        vehicle_sil::peer   1 つの participant と、その J1939、生の CAN、LIN のノード、Control Agent（監視の出力を含む）の組み立てと実行。Ecu の実装
├── libs/simulation/  vehicle_sil::simulation  ECU を仮想バスか SIL Kit で動かす Simulation
├── libs/control/     vehicle_sil::control  Control Agent と Control Server の中核（Hub）。通信と SIL Kit に依存しない
├── libs/control_grpc/ vehicle_sil::control_grpc  Control API の gRPC の層（ControlServer、AgentClient。VEHICLE_SIL_BUILD_CONTROL_SERVER のときだけ）
├── proto/            Control API の定義（vehicle_sil/control/v1/control.proto）
├── python/vehicle_sil_control/ Control API の Python クライアントと CLI
├── bridges/vector-xl/ Vector XL CAN Bridge（転送のコアは移植可能、XL バックエンドと実行ファイルは Windows 専用）
├── tools/common/     実行ファイル共通のコマンドライン解析、終了コード、ログ出力（内部用、ヘッダーのみ）
├── tools/sim_controller/ 仮想時間のシミュレーションを開始・停止する system controller（vehicle_sil_sim_controller）
├── tools/can_bus_simulator/ 簡易 CAN Network Simulator の実行ファイル（vehicle_sil_can_bus_simulator）
├── tools/control_server/ Control Server と system controller を兼ねる実行ファイル（vehicle_sil_control_server）
├── tools/j1939_codegen/ YAML の定義から J1939 の型付きメッセージ（C++ のヘッダー）を作る生成器（Python。ビルドのときに vehicle_sil_add_j1939_codec() が呼ぶ）
├── examples/getting_started/ 最初の例: Ecu& を受け取る 2 つの ECU を Simulation で動かす
├── examples/         やりたいこと別のサンプル（typed_messages、pid_stream、fault_injection、vehicle、low_level など。索引は examples/README.md）
├── examples/j1939/   部品で作った 2 つの ECU（ecu_pair。Simulation で動かし、試験も同じコードを使う）と、最小ピア（実時間の Peer で送る sender と、SIL Kit の CAN で直接受けて照合する receiver）
├── examples/virtual_time/ 仮想時間のピア（Peer の上の物理モデル、tick() 型の既存 ECU を載せた J1939 ECU、LIN Master と Slave）
├── examples/lin/     LIN クラスタのピア（Master と、役割の違う Slave。フレームの送受信と Sleep/Wakeup）
├── examples/control/  Control Agent を組み込んだピア（control_peer）と、1 プロセスのデモ（control_demo）
├── examples/fetchcontent_consumer/   外部プロジェクトからの利用例
├── tests/            ユニットテスト、統合テスト、FetchContent テスト、XL ドライバのフェイク（tests/vector_xl/fake_xl）
├── external/         SIL Kit（サブモジュール）
├── patches/          SIL Kit へのパッチ（ビルドツリーのコピーに当てる。上流への提出用の形式）
└── docs/
```

空のディレクトリや空のターゲットは作りません。

区分は、どこから使い始めるかの目安です。利用者向けは、ECU を書く人と ECU をまとめる人が直接使うライブラリで、入口は `vehicle_sil::Ecu` と `vehicle_sil::Simulation` です。土台は利用者向けのライブラリを通して使うもので、直接も使えますが、API は上の層の必要に合わせて変わります。試験用は、試験とサンプルのための道具です。実行ファイル、生成コード、CAN ブリッジは区分に入れていません（「—」）。

| ターゲット | 区分 | 依存先 | 内容 |
|---|---|---|---|
| `vehicle_sil::sim_base` | 土台 | スレッドライブラリ（`Threads::Threads`） | `SimTime` と `SimDuration`、`StepInbox`、`periodic()`（ヘッダーのみ。SIL Kit なしで使う） |
| `vehicle_sil::core` | 土台 | `SilKit::SilKit`、`vehicle_sil::sim_base` | `vehicle_sil::ParticipantRuntime`、`SimulationController`、`TickScheduler`、`RealTimePacer`。`sim_base` の型も使える |
| `vehicle_sil::lin` | 土台 | `vehicle_sil::sim_base` | `vehicle_sil::lin::LinMaster`、`vehicle_sil::lin::LinSlave`、`LinScheduleTable` と `validate_schedule()`、`LinPort`、`VirtualLinBus`（「LIN」） |
| `vehicle_sil::lin_silkit` | 土台 | `vehicle_sil::lin`、`vehicle_sil::core` | `vehicle_sil::lin::SilKitLinPort` |
| `vehicle_sil::can` | 土台 | `vehicle_sil::sim_base` | `vehicle_sil::can::Frame`、`Port`、`Steppable`、`Host`、`StepDriver`、`RealTimeDriver`、`VirtualCanBus`、`CanBusModel`、`TapPort`（「CAN の共通基盤」） |
| `vehicle_sil::can_silkit` | 土台 | `vehicle_sil::can`、`vehicle_sil::core` | `vehicle_sil::can::SilKitCanPort`、`bind_host()` |
| `vehicle_sil::can_netsim` | 土台 | `vehicle_sil::can_silkit`、`rapidyaml`（SIL Kit 付属） | 簡易 CAN Network Simulator（「簡易 CAN Network Simulator」） |
| `vehicle_sil::j1939` | 利用者向け | `vehicle_sil::can` | `vehicle_sil::j1939::Network`、`Node`、`Pgn`、`Identifier`、`Observer`、道具箱（「J1939」） |
| `vehicle_sil::j1939_broadcast_suppression`、`vehicle_sil::j1939_diagnostics` | 利用者向け | `vehicle_sil::j1939`（`j1939_diagnostics` は `j1939_broadcast_suppression` にも依存） | プロトコル部品の `BroadcastSuppression`（DM13）、`Diagnostics`、`Identification`（「プロトコル部品」） |
| `vehicle_sil::j1939_periodic_sender`、`vehicle_sil::j1939_requester`、`vehicle_sil::j1939_monitor`、`vehicle_sil::j1939_address_commander`、`vehicle_sil::j1939_exchange` | 利用者向け | `vehicle_sil::j1939`（`j1939_periodic_sender` は `j1939_broadcast_suppression` にも依存） | ECU アプリ向けのプロトコル部品 `PeriodicSender`、`Requester`、`Monitor`、`AddressCommander`、独自プロトコル向けの `Exchange`（「プロトコル部品」、[j1939-guide.md](j1939-guide.md)） |
| `vehicle_sil::j1939_codec`（ヘッダーのみ） | 利用者向け | `vehicle_sil::j1939`、`vehicle_sil::j1939_periodic_sender` | 型付きメッセージのランタイム（`codec.hpp`。エンジンの値の型だけを使う）と、ノードと `PeriodicSender` への接続（`codec_node.hpp`）。型は `vehicle_sil_add_j1939_codec()` が YAML から生成する（「型付きメッセージ」、[j1939-codec.md](j1939-codec.md)） |
| `vehicle_sil::ecu`（ヘッダーのみ） | 利用者向け | `vehicle_sil::can`、`vehicle_sil::j1939`、`vehicle_sil::lin`、`vehicle_sil::control` | `vehicle_sil::Ecu`、`detail::OwnedObjects`、`detail::EcuAgent`（「実行と試験の基盤」） |
| `vehicle_sil::local` | 土台 | `vehicle_sil::can`、`vehicle_sil::j1939`、`vehicle_sil::lin`、`vehicle_sil::ecu` | `vehicle_sil::LocalBench`（「実行と試験の基盤」） |
| `vehicle_sil::testkit` | 試験用 | `vehicle_sil::local`、`vehicle_sil::can`、`vehicle_sil::j1939`、`vehicle_sil::ecu` | `J1939Events`、`FrameLog`、`faults`、`LocalBench` の別名 `testkit::LocalCan`（「実行と試験の基盤」） |
| `vehicle_sil::peer` | 土台 | `vehicle_sil::core`、`vehicle_sil::can_silkit`、`vehicle_sil::j1939`、`vehicle_sil::lin_silkit`、`vehicle_sil::ecu`、`vehicle_sil::control` | `vehicle_sil::Peer`（Control Agent の監視の出力を含む）、`start_together()`（「実行と試験の基盤」） |
| `vehicle_sil::simulation` | 利用者向け | `vehicle_sil::ecu`、`vehicle_sil::peer`、`vehicle_sil::local` | `vehicle_sil::Simulation`、`SilKitOptions`、`SimulationError`（「実行と試験の基盤」） |
| `vehicle_sil_sim_controller`（実行ファイル） | — | `vehicle_sil::core` | system controller の CLI |
| `vehicle_sil::control` | 利用者向け | `vehicle_sil::sim_base` | `vehicle_sil::control::Agent`、`Hub`（「外部操作（Control API）と GUI」） |
| `vehicle_sil::control_proto`（`VEHICLE_SIL_BUILD_CONTROL_SERVER` のとき） | — | gRPC、Protocol Buffers | `control.proto` から生成したコード（`control_grpc` の内部） |
| `vehicle_sil::control_grpc`（同上） | 利用者向け | `vehicle_sil::control`、`vehicle_sil::core`（`control_proto` は非公開） | `vehicle_sil::control_grpc::ControlServer`、`AgentClient`、`time_control_of()` |
| `vehicle_sil_control_server`（同上、実行ファイル） | — | `vehicle_sil::control_grpc`、`vehicle_sil::core` | Control Server と system controller の CLI |
| `vehicle_sil::vector_xl_bridge_core` | — | `SilKit::SilKit`、`vehicle_sil::can`、`vehicle_sil::can_silkit` | `vehicle_sil::vector_xl::CanBridge`、`ExternalCanPort`、`ExternalPortAdapter`、チャネル選択、CLI の解析。全プラットフォームでビルドする |
| `vehicle_sil::vector_xl`（Windows のみ） | — | `vector_xl_bridge_core`、XL Driver Library | `vehicle_sil::vector_xl::XlDriver`、`XlCanPort` |
| `vehicle_sil_vector_xl_bridge`（Windows のみ、実行ファイル） | — | 上記と `vehicle_sil::core` | ブリッジの CLI |

利用側は必要なターゲットだけをリンクします。依存ライブラリは `EXCLUDE_FROM_ALL` で追加しているので、リンクしないものはビルドされません（外部プロジェクトから取り込んだときは vehicle-sil のライブラリも同じです）。

### 共通化の規則

- `core` に置くのは、どのプロトコルでも同じになる処理だけです。具体的には、SIL Kit への接続、lifecycle、他参加者の監視、仮想時間（ステップ、受信のバッファ、周期タスク、実時間との結合）、将来の外部操作です。
- `core` は CAN、LIN、J1939 に依存しません。バスのコントローラ（`ICanController` など）は、利用側が `ParticipantRuntime::participant()` から SIL Kit の API で作ります。
- J1939 と LIN のプロトコル処理は共通化しません。
- 将来のためだけの抽象クラスや空の実装は作りません。

## コンポーネント

| クラス | 責務 |
|---|---|
| `vehicle_sil::ParticipantRuntime` | SIL Kit の participant、lifecycle（autonomous または coordinated）、SystemMonitor、時刻同期（任意）を RAII で所有する。他参加者の状態（Running か、終了したか）を待てる |
| `vehicle_sil::SimulationController` | coordinated で起動するシミュレーションの system controller。必要な参加者がそろったら一斉に開始させ、指定した仮想時刻で停止させる |
| `vehicle_sil::StepInbox` | SIL Kit のスレッドで届いたメッセージをステップの境界までため、決まった順序で渡す |
| `vehicle_sil::TickScheduler` | 1 つの participant の中で、固定周期のタスク（既存 ECU の `tick()` など）をステップから呼ぶ |
| `vehicle_sil::RealTimePacer` | 仮想時間を壁時計に一定の比率で結合するための待ち時間を計算する |
| `vehicle_sil::Ecu` | ECU のアプリケーションを組み立てる入口（J1939 の `Network`、生の CAN、LIN の Master と Slave、ステップごとの処理、ECU が持つオブジェクト）。`Peer` と `LocalBench` の ECU が実装する（「実行と試験の基盤」） |
| `vehicle_sil::Simulation` | `Ecu` を仮想バスか SIL Kit の仮想時間で動かし、失敗を `SimulationError` で報告する（「実行と試験の基盤」） |
| `vehicle_sil::Peer` | 1 つの participant と、その J1939 の `Network`、LIN の Master と Slave、ステップごとのアプリの処理を組み立てて動かし、正しい順序で片付ける。`Ecu` を実装する（「実行と試験の基盤」） |
| `vehicle_sil::lin::LinMaster` | LIN のスケジュールテーブルを仮想時間で実行し、ヘッダーごとの結果（応答の衝突は Error）を返す。Master が送るフレーム、診断のマスターリクエスト、go-to-sleep コマンド、wakeup パルスを送る |
| `vehicle_sil::lin::LinSlave` | ステップで設定された値で Master のヘッダーに応答し（診断のスレーブレスポンスは 1 回だけ）、Master が送るフレームを受信する。休止と起床の状態を管理する |
| `vehicle_sil::lin::LinPort` | LIN のノードがバスに触れる唯一の口。実装は `VirtualLinBus` のエンドポイントと `SilKitLinPort` |
| `vehicle_sil::lin::VirtualLinBus` | プロセス内の LIN バス。送った側の呼び出しの中で、同期に配る。`LocalBench` が LIN のネットワーク名ごとに 1 つ作る |

### 公開 API の契約

- `ParticipantRuntime` のコンストラクタはレジストリに接続します。失敗すると `SilKit::SilKitError` を投げます。
- バスのコントローラは `start()` の前に作ります。`start()` が戻った時点で、接続済みの相手はそのコントローラの購読を確認済みです。
- `start(timeout)` は lifecycle が Running になるまで待ちます。タイムアウトや失敗のときは `std::runtime_error` を投げます。
- `stop()` は何度呼んでもよく、例外を投げません。
- `wait_for_peer()` は、相手の状態が条件を満たすまで待ちます。待っている間は、SIL Kit から状態を定期的に読み直します（[docs/dependencies.md](dependencies.md) の既知の不具合を参照）。SIL Kit のハンドラの中からは呼べません。
- コントローラを参照するもの（`SilKitCanPort`、`SilKitLinPort`）はコントローラを所有しません。participant より先に閉じるか破棄します。`LinMaster` と `LinSlave` はポートを所有しないので、ポートより先に破棄します。
- J1939 スタックはプロセス全体の状態を持ちません。1 つのプロセスに、いくつでも `Network` と `Peer` を置けます。

### 所有権と終了順序

```
ParticipantRuntime ──owns──> SilKit::IParticipant ──owns──> ICanController、ILinController
SilKitCanPort、SilKitLinPort ──refers to──> コントローラ（所有しない）
LinMaster、LinSlave ──refers to──> LinPort（所有しない）
Host ──refers to──> ポートと Network（所有しない）
```

`Peer` は次の順で終了します（自分で組み立てるときも同じ順にします）。

1. ステップを止める（実時間では `RealTimeDriver`、続いて lifecycle。仮想時間では lifecycle）。
2. SIL Kit のポートを閉じる（ハンドラを外し、コントローラを止める）。
3. LIN のノード（それぞれのポートより先）、`Network`（部品が先）、ポート、ホストを破棄する。
4. participant を破棄する。

### スレッドとキュー

| スレッド | 所属 | 処理 |
|---|---|---|
| SIL Kit I/O | SIL Kit | `SilKitCanPort` の受信ハンドラがフレームに時刻を付けて受け箱に積む。満杯なら捨てて `receive_overflows` に数え、`ReceiveOverflow` の事象にする。仮想時間では、ステップハンドラもこのスレッドで呼ばれる |
| ドライバー | vehicle-sil（実時間のみ） | `RealTimeDriver` がホストのステップ（受信、プロトコル処理、アプリ、送信）を回す |

プロトコル処理（`Network`、部品、ハンドラ）はロックを持たず、ステップを回すスレッドだけが触ります。仮想時間では SIL Kit のステップハンドラ、実時間ではドライバーのスレッドです。ほかのスレッドからは、ホストの `post()` で処理を渡します。

キューの容量は、ホストの `post()` の待ち行列を除いて固定です。生成時に確保します。`post()` の待ち行列は上限がなく、呼ぶたびに確保します（外部からの操作のための口で、ステップごとに使うものではありません）。

| キュー | 既定の容量 | 備考 |
|---|---|---|
| `SilKitCanPort` の受け箱 | 1024 フレーム | `SilKitCanPortOptions::inbox_capacity` |
| `SilKitCanPort` の事象の受け箱 | 1024 | `SilKitCanPortOptions::event_capacity` |
| `Network` の 1 ステップの受信数 | 1024 フレーム | `NetworkOptions::receive_capacity`。ほかの上限は「ステップ幅と資源の上限」 |
| サンプル Receiver の受信箱 | 256 | |

### 時刻（実時間モード）

`ParticipantRuntimeOptions::virtual_time`（`PeerOptions::virtual_time`）を指定しない場合（J1939 の最小ピアと Vector XL CAN Bridge）は、SIL Kit の時刻同期を使いません。autonomous lifecycle で、すべて実時間で動きます。

- J1939 のタイマー（Address Claim の後の待ちなど）は、ドライバーの時計（`RealTimeClock`）で進みます。
- 受信フレームの時刻は、ポートが時計で付けた到着時刻です。時刻同期をしない participant の SIL Kit のタイムスタンプは使いません（「実時間での駆動」）。

## CAN の共通基盤

`vehicle_sil::can` と `vehicle_sil::can_silkit` は、J1939 スタック（`vehicle_sil::j1939`）の土台です。CAN の 11 ビットと 29 ビットのフレームを扱い、J1939 以外のプロトコルも載せられます。API はまだ変わることがあります。

- `vehicle_sil::can` は SIL Kit と Vector XL に依存しません。SIL Kit をリンクしない単体テストの実行ファイルをビルドすることで、毎回確かめます。
- 時刻はすべて `SimTime` です。プロトコル処理はスレッドを持たず、時計を読みません。仮想時間では SIL Kit または `StepDriver` の時刻、実時間では `RealTimeDriver` の時計（開始からの経過時間）です。

### 値の型

- `Frame` はクラシック CAN のデータフレームです（RTR、CAN FD、CAN XL は表しません）。`length` はバイト数（0〜8）で、DLC の符号ではありません。比較は ID、形式（11 ビット / 29 ビット）、`length` までのデータで行います。
- `TimedFrame` は時刻付きのフレームです。受信ではバックエンドが付けた時刻、送信では送るステップの時刻です。

### ポートの契約（`Port`）

- `release_before(now, sink)` は、時刻が `now` より前の受信フレームを、時刻順、同じ時刻なら到着順に渡します。同じ時刻のフレームを並べ替えません。最終的な順序はプロトコル処理が決めます。時刻がちょうど `now` のフレームは次の呼び出しで渡します。
- 自分が送ったフレームは受信しません（エコーと送信の確認はポートで捨てます）。
- `send()` は同期の結果を返します。`Accepted`（受け付けた）、`QueueFull`（送信キューが満杯。後のステップで出し直せる）、`Failed`（送れないフレーム、バックエンドの誤り）です。
- `send()` の後にフレームがどうなったか（送信された、バックエンドが捨てた）をバックエンドが知るときは、後から事象（`PortEvent`）で知らせます。`release_events_before(now, sink)` は、時刻が `now` より前の事象を、時刻順、同じ時刻なら到着順に渡します。既定の実装は事象を出しません。
- `release_before()`、`release_events_before()`、`send()` は、ドライバーのスレッドだけから呼びます。
- ポートは時刻の領域を持ちます。`Virtual`（仮想時間。仮想バス、時刻同期ありの SIL Kit）と `RealTime`（実時間ドライバーの時計。時刻同期なしの SIL Kit、Vector XL）です。
- `RealTime` のポートは、受けたフレームと事象に、ドライバーと共有する時計で到着時刻を付けます（下の「実時間での駆動」）。到着のたびに `set_wakeup()` で渡された関数を到着時刻付きで呼び、ドライバーを起こします。`Virtual` のポートは `set_wakeup()` を無視します。

### ポートの事象（`PortEvent`）

自分が送ったフレームについて、`send()` の同期の結果の後に分かったことです。

| 種類 | 意味 | 時刻 |
|---|---|---|
| `TransmitCompleted` | フレームが送信された | バスを模擬するバックエンドでは送信の終わり。それ以外ではバックエンドが送信を確認した時刻 |
| `TransmitFailed` | `send()` が受け付けたフレームを、バックエンドが送らなかった（送信キューの満杯、取り消し、バスから切り離されている） | バックエンドが知らせた時刻 |
| `ReceiveOverflow` | 受け箱が満杯で受信フレームを捨てた。`count` は捨てた数（0 は数が分からない分を含む） | 捨てたフレームのうち最も早い時刻 |
| `BusStateChanged` | コントローラーのエラー状態が `bus_state`（`ErrorActive`、`ErrorPassive`、`BusOff`）になった | 変わった時刻 |
| `ErrorFrame` | バスでエラーフレームを見た | 見た時刻 |

- 送信の事象は CAN 識別子（`id`）と、分かれば送ったときの `TimedFrame::time`（`send_time`）を持ちます。同じ識別子のフレームは `send_time` で見分けます。データは持ちません。
- `ReceiveOverflow` は、捨てたフレームごとには出しません。ポートが次に事象を渡すまでに捨てた数を、1 つの事象にまとめます。この事象は事象の受け箱の枠を使わないので、受け箱が満杯でも届き、送信の事象を押し出しません。
- バックエンドが知り得ない種類は出しません（報告なし）。

| バックエンド | 出す事象 |
|---|---|
| 仮想バス | タイミングありなら `TransmitCompleted`。`ReceiveOverflow`。故障注入と通信断の `TransmitFailed`。注入した任意の事象 |
| `SilKitCanPort` | SIL Kit の送信確認から `TransmitCompleted` と `TransmitFailed`。SIL Kit のエラー状態の通知から `BusStateChanged`（ネットワークシミュレーターが出す。SIL Kit の簡易シミュレーションは出さない）。`ReceiveOverflow` |
| `ExternalPortAdapter`（Vector XL） | `ErrorFrame`。外部ポートのあふれは数の分からない `ReceiveOverflow`。バス状態と送信の結果は出さない |

- `Host` は、ステップの段階 2 で、そのステップのフレームより先に事象を `Steppable::on_port_event()` に渡します。`on_port_event()` は記録だけにします。送信はしません（送信は `flush()` で行います）。
- J1939 スタックは、事象を `NetworkStatistics`（`frames_transmitted`、`transmit_failures`、`port_receive_overflows`、`port_overflow_events`、`bus_state_changes`、`error_frames`）に数え、最新のバス状態を `Network::bus_state()` に保つだけです。プロトコルの処理には使いません（バスオフでも動作を変えません）。
- 種類はこれから増えることがあります。`Steppable` は知らない種類を無視します。

### `Steppable` の契約

CAN の上にプロトコルスタックを作るときの拡張点です。J1939 の `Network` はこの契約で作っています。

- ホストは 1 ステップに、`begin(now)`、ポートの事象ごとの `on_port_event()`、フレームごとの `receive()`、`process()`、（アプリの処理の後に）`flush(port)` の順で呼びます。最初のステップの前に `start(step_size)` を 1 回呼びます。
- `now` は呼び出しごとに増えます。`Steppable` はスレッドを持たず、時計を読みません。時刻は `now` と、フレームと事象の時刻だけです。呼び出しは常に 1 つのスレッド（ドライバーのスレッド）からで、ロックは要りません。
- `receive()` と `on_port_event()` では記録だけにします。処理は `process()` で、送信は `flush()` でポートに出します。
- `receive()` に渡るフレームの時刻は `now` より前です。同じ時刻の、別の送信者のフレームの順序はバックエンドによって違うので、`Steppable` が決まった規則で並べます。
- `start()` は、そのプロトコルが動けないステップ幅なら例外を投げます（ホストが失敗状態になります）。実時間では、ドライバーの `max_idle` がステップ幅として渡ります。
- `next_deadline()` は、何も届かなくてもステップが要る最も早い時刻（タイマーの期限）を返します。実時間のドライバーはそれまで眠ります。タイマーのない仕事（`QueueFull` で残したフレーム、1 ステップの上限で繰り越したフレーム）は含めません。それらは実時間ではドライバーの `max_idle` の後のステップで出ます。ステップの後も過去の期限を返し続けるのは契約違反です（手動の時計では次のステップを 1 ns 後に 1 回回すだけですが、壁時計ではステップを回し続けます）。
- 例外を投げると、ホストは失敗状態になり、以後のステップはすべて失敗します。

### ホスト（`Host`）

- 1 つの participant（またはテストのループ）の、プロトコル処理（`Steppable`）とポートの組を登録順に持ちます。1 ステップは、全部の組に対して段階ごとに進めます。(1) `begin(now)`、(2) ポートが渡す事象の `on_port_event()` とフレームの `receive()`、(3) `process()`、(4) アプリの処理を 1 回、(5) `flush(port)`。
- ステップの時刻は前のステップより大きくなければなりません。
- `step()` から例外が出ると（プロトコル処理、アプリ、ポートのどれからでも、使い方の誤りでも）、ホストは失敗状態になり、例外を呼び出し元に返します。以後の `step()` はすべて `std::logic_error` です。回復するには作り直します。
- 登録は最初のステップの前だけです。ホストは `Steppable` とポートを所有しません。
- ステップ幅: `set_step_size()` で受け取ったステップ幅を、最初のステップの `begin()` の前に、各 `Steppable` の `start()` に登録順で 1 回渡します（設定がなければ値なし）。`StepDriver::add()`、`bind_host()`、`RealTimeDriver` が設定します。`start()` が例外を投げると、ホストは失敗状態になります（そのプロトコルが動けないステップ幅の構成エラー）。
- `post(fn)`: ステップの外（別のスレッド、SIL Kit の他のハンドラ）からプロトコルスタックに処理を渡します。どのスレッドからでも呼べます。`fn` は次のステップの段階 4 の最初（アプリの処理の前）に、渡された順でドライバーのスレッドで実行します。ステップの中で渡した処理は次のステップです。`fn` の例外はアプリの例外と同じくホストを失敗させ、失敗したホストに渡した処理は実行しません。仮想時間でも使えますが、どのステップで実行されるかは `post()` を呼んだ時点で決まるので、決定的に扱いたいデータは受け箱（`StepInbox`）で受けてステップの中で処理します。
- 時間の領域: 1 つのホストのポートは同じ領域でなければなりません。ドライバーは自分の領域を要求します（`StepDriver` と `bind_host()` は `Virtual`、`RealTimeDriver` は `RealTime`）。混在は登録の時点で `std::logic_error` です（例: Vector XL のポートを SIL Kit の時刻同期で回すホストに入れる）。
- `next_deadline()` は全 `Steppable` の `next_deadline()` の最小です。

### 仮想時間での駆動

- `StepDriver` は、SIL Kit を使わずに複数のホストを仮想時間で進めます。各ホストのステップ幅は別々でよく、ステップは（開始時刻、登録順）の順に実行します。SIL Kit の時刻同期が許す順序の 1 つです。
- テストのループでの終了の順序: そのホストを進める `StepDriver` を使うのをやめる → プロトコル処理とホストを破棄 → ポート（仮想バスが所有）を破棄。
- SIL Kit では `bind_host(runtime, host)` を使います。`ParticipantRuntime` のステップハンドラから `host.step(now)` を呼びます。仮想時間を使わない `ParticipantRuntime` には結び付けられません。

### 実時間での駆動（`RealTimeDriver`）

- 時刻同期なしの SIL Kit や Vector XL のポートを持つホストを、壁時計で回します。participant ごとに 1 つ、スレッドを 1 本持ちます。プロトコルスタックにはロックがなく、触るのはこのスレッドだけです。アプリはホストの `post()` とアプリの処理で関わります。
- 時計（`RealTimeClock`）は差し替えられます。製品では `SteadyRealTimeClock`（生成時からの経過時間）、試験では `ManualRealTimeClock`（`advance_to()` で進めるとドライバーを起こす）を使います。ドライバーとホストのポートは同じ時計を使います。
- 登録（`Host::add()`、`set_application()`）は `start()` の前に済ませます。`start()` の直後に、ドライバーのスレッドで最初のステップが回ります。
- ステップを回すのは、開始の直後、ポートへの到着や `post()` のとき、ホストの `next_deadline()` の 1 ns 後（期限より後で発火するタイムアウトのため）、前のステップから `max_idle`（既定 10 ms）が過ぎたときです。
- ステップの時刻は時計の値ですが、前のステップより少なくとも 1 ns 後、起こした到着の時刻より少なくとも 1 ns 後にします。時計が進んでいなくても、届いたフレームは次のステップで必ず渡ります。
- 到着時刻（`RealTimeReceiver`）: ロックの中で、時計の値、前回渡した `now`、前の到着時刻 + 1 ns の最大を付けます。処理済みのステップより前の時刻にはならず、到着の順も保たれます。
- ステップが例外を投げると、スレッドは止まり、`failed()` と `error()` で理由が分かります（ホストも失敗状態）。
- `wait_idle(timeout)` は、スレッドがやることなく眠るまで待ちます。`ManualRealTimeClock` と組み合わせると、`advance_to()` の後に、その時刻までのステップが終わったことを確かめられます。
- 終了の順序: `RealTimeDriver` を止める（`stop()` か破棄。ホストとポートから起こしを外し、スレッドを join する）→ ポートを閉じる → プロトコルスタックとホストを破棄 → `ParticipantRuntime`（またはドライバー）を破棄。`stop()` をステップの中から呼んだ場合は、スレッドに終わるよう伝えるだけで、join は後の `stop()` で行います。
- 決定論はありません。壁時計の試験では、到着と順序だけを確かめます。

### 仮想バス（`VirtualCanBus`）

- `attach()` が、バス上の 1 つの CAN コントローラーにあたるポートを作ります。
- タイミングなし（既定）: 時刻 t で送ったフレームは、他の全ポートに時刻 t のまま届きます。調停と送信時間はありません。
- タイミングあり（`VirtualCanBusOptions::timing`）: フレームは下の「バスのタイミング」の規則でバスに出て、送信の終わりの時刻で他のポートに届きます。送ったポートには同じ時刻の `TransmitCompleted` が届きます。ポートごとの送信キュー（`transmit_queue_capacity`、既定 64）が満杯なら `send()` は `QueueFull` です。数えるのは、送って、バスを決めた時点でまだ始まっていないフレームです（`Host` は送る前に同じステップの受信を取り出すので、ステップの時刻の状態です）。バスは遅れて決めます。どれかのポートが `now` より前のフレームか事象を取り出すとき、`now` より前に始まる送信をすべて決めます。
- すでに決まった時刻より古いフレームは送れません（`std::logic_error`）。タイミングなしでは受信側がすでに受け取った時刻、タイミングありでは決めたバスの時刻です。ホストを時刻順に進めなかったことを示します。`StepDriver` は常に時刻順に進めます。
- 各ポートの受け箱は容量が固定です。入らないフレームは捨てて数えます（`overflows()`）。仮想バスではあふれも決定的ですが、フレームが失われるので、テストではあふれがないことを確かめます。
- 故障注入（試験用）: `Endpoint::set_fault_injector(f)` で、そのポートが送るフレームごとに `f` が `FaultAction` を返します。`Deliver`（通常）、`Drop`（`Accepted` を返して、バスの他のポートに届けない。送った側の Network の中のローカル折り返しは `Accepted` なので届く）、`Duplicate`（同じ時刻で 2 回届ける）、`Delay`（時刻 + 遅れで届ける。後から送ったフレームに追い越されるので、並べ替えもこれで作る。遅れは 0 より大きいこと）、`QueueFull`、`Failed`（その結果を返して届けない）です。不正なフレームは `f` を呼ぶ前に `Failed` です。`f` は送った順に 1 回ずつ呼ばれるので、決定的な関数なら結果も決定的です。タイミングありでは、`Drop` のフレームはバスを占有してからどこにも届かず（送ったポートには `TransmitCompleted` が届きます）、`Duplicate` は 2 回送信し、`Delay` は送った時刻 + 遅れからバスを争います。
- 受け箱のあふれは `ReceiveOverflow` でも知らせます。`Endpoint::inject_event(event)` は、任意の事象（バス状態、エラーフレームなど）をその端点に積みます。`FaultAction::transmit_failed()` は、`Accepted` を返してどこにも届けず、送った側に送信時刻の `TransmitFailed` を出します（非同期の送信失敗。タイミングありでもバスを使いません）。
- ECU 単位の通信断: `Endpoint::add_outage(from, until, direction)` は、その端点を [from, until) の間バスから切り離します。`Receive` はその間に届く時刻のフレームを受けず（`outage_dropped()`）、`Send` はその間の送信時刻のフレームをどこにも届けず、送った側に `TransmitFailed` を出します（`outage_lost()`。故障注入の関数は呼びません。タイミングありでもバスを使いません）。送信は送信時刻で判定するので、タイミングありでは、断の前に送ってバスを待っているフレームは断の間にも出ます。`Both` は両方です。フレームの時刻で決まるので、結果は決定的です。
- 1 つのスレッドから使います。

### 生フレームの観測（`TapPort`）

- `TapPort(inner, listener)` はポートを包み、すべてを `inner` に渡します。プロトコルスタックが受け取ったフレーム（`Received`、フレームの時刻）、送ったフレーム（`Sent`、送ったステップの時刻と `send()` の戻り値）、ポートの事象（`Event`、事象の時刻）を、通った順に `TapListener::on_tap()` へ知らせます。11 ビットと 29 ビットの両方をそのまま運びます。`Received` の順はポートの解放順（時刻順、同じ時刻なら到着順）で、SIL Kit のポートでは同じ時刻の別の送信者のフレームの順が実行ごとに変わりえます。実行の間で比べるときは（時刻、CAN ID）で並べ直すか、並べ替えた後の `Observer` の事象を使います。
- 読み取り専用です。注入はしません（試験の刺激は仮想バスの端点から送ります）。例外を投げないリスナーなら、リスナーの有無で、スタックが受け取るものと送った結果は変わりません。リスナーの例外は呼び出し元に伝わり（ホストは失敗します）、受信のフレームと事象はスタックに渡す前に知らせるので、そのフレームはスタックに渡りません。リスナーの中からその `TapPort` の `send()`、`release_before()`、`release_events_before()`、`set_listener()` を呼ぶと `std::logic_error` です。
- 時間の領域と wakeup は包んだポートのものです。包んだポートと同じく、ドライバーのスレッドから使います。リスナーは 1 つです。リスナーと `TapPort` は互いの登録を覚えており、どちらを先に破棄しても安全です。

### SIL Kit のポート（`SilKitCanPort`）

- 生成時に、CAN コントローラーに受信（RX 方向だけ）と送信確認のハンドラを付け、コントローラーを開始します。`close()` はハンドラを外し、コントローラーを止め、受け箱を空にします。`close()` から戻った後は、SIL Kit がポートを呼びません。
- 受信のコールバックは、SIL Kit のタイムスタンプ（送信側のステップ時刻）を付けて受け箱に積むだけです。クラシック CAN のデータフレーム以外は捨てて数えます。
- 送信は `SendFrame()` です。時刻は SIL Kit が送信側のステップ時刻を付けます。SIL Kit の簡易シミュレーションには同期の結果がないので、正しいフレームは `Accepted` です。
- `SendFrame()` の userContext には、送ったステップの時刻（`TimedFrame::time`）を目印付きで入れます（`encode_send_time()`。上位 8 ビットが目印 0xA5、下位 56 ビットが ns）。簡易 CAN Network Simulator はこれで送信時刻を知ります。SIL Kit は userContext を送信確認で送信側に返すだけで、他の受信者には渡しません。64 ビットのプラットフォームだけを対象にします。
- 送信確認は数えて、`PortEvent` として事象の受け箱に積みます（`Transmitted` は `TransmitCompleted`、`Canceled` と `TransmitQueueFull` は `TransmitFailed`）。SIL Kit のハンドラからプロトコル処理は呼びません（簡易シミュレーションでは送信確認が `SendFrame()` の中で返るので、呼ぶと再入になります）。時刻は、簡易シミュレーションでは送ったステップの時刻、ネットワークシミュレーターでは送信の終わりです。
- `SilKitCanPortOptions::bitrate` を設定すると、開始の前に `SetBaudRate()` で渡します。簡易シミュレーションは使いません。ネットワークシミュレーターは送信時間に使います。
- `SilKitCanPortOptions::transmit_queue_capacity`（既定 0 = 上限なし）を設定すると、送ったが送信確認の事象をまだ渡していないフレームがその数に達したとき、`send()` が `QueueFull` を返します。プロトコル処理はフレームを残して次のステップで出し直します。ネットワークシミュレーターは自分の送信キューを超えた要求を送らずに捨てる（`TransmitQueueFull` の送信確認）ので、シミュレーターを使うときはその大きさ以下にします。`event_capacity` より大きくはできません。送信確認の来ないフレーム（開始していないコントローラーでの送信など）は枠を返さないので、統計の `outstanding` で確かめます。SIL Kit の簡易シミュレーションでは送信確認が次のステップで渡るので、上限は 1 ステップに送れる数になります。
- SIL Kit のエラー状態の通知は `BusStateChanged` の事象にします（`NotAvailable` は出しません）。事象の受け箱に、送信確認をまだ受けていないフレームの数の空きを残せないときは、捨てて `event_overflows` に数えます（送信確認を失って `transmit_queue_capacity` の枠が減らないように）。受け箱のあふれは `ReceiveOverflow` です。
- 実時間（`SilKitCanPortOptions::real_time_clock` を設定）: 時刻同期なしの participant のポートになり、時刻の領域は `RealTime` です。SIL Kit のタイムスタンプ（同期なしの送信者では使えない）の代わりに、時計で到着時刻を付け、到着のたびにドライバーを起こします。userContext に送信時刻は入れません（ネットワークシミュレーターは仮想時間が前提のため）。`RealTimeDriver` で回します。
- 終了の順序: `ParticipantRuntime` を止める（実時間では `RealTimeDriver` を止める）→ ポートを `close()`（または破棄）→ プロトコル処理とホストを破棄 → `ParticipantRuntime` を破棄（コントローラーを所有しているため最後）。
- 同じシナリオを仮想バスと SIL Kit（仮想時間）で動かすと、受信と送信のトレースがバイト単位で一致します。ステップ幅の違う participant の組み合わせを含めて、テストで確かめています。前提として、同じ時刻の別の送信者のフレームについて、ポートが渡す順序は SIL Kit では決まりません（送信者ごとの順序だけが保たれます）。そのため、プロトコル処理がそれらを決まった規則で並べ直す必要があります。テストのプロトコルは（時刻、送信者）で並べ直しています。

### バスのタイミング（`CanBusModel`）

タイミングありの仮想バスと簡易 CAN Network Simulator は、同じバスモデルを使います。SIL Kit に依存しません。

- 送信時間: スタッフビットを含めないビット数（11 ビット ID は 47 + 8 × バイト数、29 ビット ID は 67 + 8 × バイト数。3 ビットのインターミッションを含む）÷ ビットレート。ns 単位に切り上げます。実際のフレームはスタッフビットのぶん（最大 2 割ほど）長くなります。
- 調停: バスが空いたときに、準備のできた送信キューの先頭どうしで、調停フィールドの小さいものが勝ちます。先に 11 ビットの基本 ID を比べ、同じなら 11 ビットのフレームが 29 ビットのフレームに勝ち（RTR が優性、SRR が劣性）、29 ビットどうしは拡張 ID を比べます。送信中のフレームには割り込みません。送信中に準備のできたフレームは、送信の終わりに争います。
- 同じ ID と形式のフレームどうし（実バスでは解決しない）: 短いデータ、データのバイトの順、送信者の順（仮想バスでは `attach()` の順）で決めます。
- 送信キューは送信者ごとの FIFO です。後ろのフレームは、調停で勝てる場合でも、前のフレームが出るまで待ちます。
- 模擬しないもの: スタッフビット、エラーフレーム、ACK エラーと自動再送、エラー状態、リモートフレーム、CAN FD、コントローラーの送信バッファの優先度。
- 決め方の 2 つの形（`VirtualCanBusTiming::simulator_step`）:
  - 0（厳密）: フレームは送った時刻からバスを争います。受信の時刻は送信時刻だけで決まり、受信側のステップ幅によりません。
  - Δ > 0: バスを Δ ごとに決めます。境界 T では、T より前に送られたフレームをキューに加え、[T, T + Δ) に始まる送信を決めます。時刻 t に送ったフレームは、t の後の最初の境界より前には始まりません。簡易 CAN Network Simulator（`SimulationStepSizeNanoseconds` = Δ）と同じ規則で、同じ送信には同じ受信の時刻を出します（テストで確かめています）。結果は Δ によって変わりますが、受信側のステップ幅にはよりません。
- タイミングの確認は、ノードを別々のポート（別々の CAN コントローラー）に置いて行います。同じ `Network` のノードどうしはローカル折り返しで届き、バスを通りません。

### 簡易 CAN Network Simulator（`vehicle_sil::can_netsim`、`vehicle_sil_can_bus_simulator`）

SIL Kit の簡易シミュレーション（trivial simulation）は、調停もバスの占有も再現しません。簡易 CAN Network Simulator は、SIL Kit の Network Simulator API（experimental）で、指定したネットワークの CAN を上のバスモデルで模擬する SIL Kit の participant です。CANoe のライセンスなしで、調停、送信時間、混雑を仮想時間で再現します。

- そのネットワークのコントローラーは、SIL Kit が自動で詳細シミュレーションに切り替えます。各 participant のコードは変わりません。
- 受信のフレームの時刻は送信の終わりです。送信側には、同じ時刻の TX のフレームと送信確認（`Transmitted`）を、送信側の userContext を付けて返します。コントローラーごとの送信キュー（既定 64）があふれた要求には `TransmitQueueFull` の送信確認を返し、送信しません。
- 時刻と決定論: 時刻同期に参加し、ステップ T で、T より前に送られた要求（vehicle-sil のポートが userContext に入れた送信時刻）を取り込み、[T, T + ステップ幅) に始まる送信を決めます。結果は送信時刻とステップ幅だけで決まり、他の participant のステップ幅やスレッドのタイミングによりません。目印のない要求（vehicle-sil 以外の participant）と、最後のステップより 1 秒以上先の時刻の要求は、最後のステップの時刻で扱い、数えます。この扱いは決定的ではありません。
- ビットレート: そのネットワークで最初に `SetBaudRate()` を呼んだコントローラーの値、なければ `--default-bitrate`（既定 500 kbit/s）です。後から違う値が来たら数えて警告し、使いません。コントローラーがシミュレーターを見つける前に呼んだ `SetBaudRate()` は届かないので（SIL Kit の挙動。docs/dependencies.md）、ネットワークのビットレートは `--default-bitrate` でも指定してください。
- コントローラーのモードの変更（`Start()`、`Stop()` など）には、次のステップで状態の変化を返します。フレームは、モードによらず、送信者以外の接続中の全コントローラーに届けます。CAN FD とリモートフレームの要求は捨てて数えます。
- 受け箱（ネットワークごとに 4096 要求）があふれたら、捨てて数え、警告します。あふれた実行は決定的ではありません。
- 設定にあっても、まだどのコントローラーも参加していないネットワークは何もしません（SIL Kit は最初のコントローラーが現れたときにフレームを配る手段を渡します）。Vector のものと共用する設定に、使わない CAN ネットワークがあってもかまいません。
- 使っているビットレートと、それが既定値かコントローラーの設定かを、ログに出します。
- 送信時刻の目印の上限は、最後のステップから 1 秒とステップ幅の大きいほうです。

設定ファイルとコマンドライン（Vector SIL Kit Network Simulator と共用）:

```yaml
SchemaVersion: 1
Description: 任意の説明
SimulationStepSizeNanoseconds: 1000000   # 省略時 1 ms
SimulatedNetworks:
  - Name: CAN1
    Type: CAN
  - Name: LIN1          # CAN 以外は模擬せず、警告する
    Type: LIN
```

- 形式は Vector SIL Kit Network Simulator の設定ファイル（`SchemaVersion: 1`、YAML か JSON）です。`Type: CAN` のネットワークだけを模擬します。ほかの種類のネットワーク、`Switches`、知らないキーは受け付けて警告します。必須の項目がない、`SchemaVersion` が 1 でない、種類が CAN、LIN、Ethernet、FlexRay のどれでもない、名前が重複している、といったファイルは使いません。
- コマンドラインは Vector のものと同じ形です: `vehicle_sil_can_bus_simulator [--name <participant>] [--connect-uri <uri>] [--log <level>] [--participant-configuration <file>] <configuration>`（短い形 `-n -u -l -c` も同じ。participant 名の既定は `NetworkSimulator`）。`-c` のファイルの中の相対パスは、実行時のカレントディレクトリから解決します。この実行ファイルだけの設定（`--default-bitrate`、`--transmit-queue`、`--start-timeout-ms`）はコマンドラインで渡し、設定ファイルには書きません。Vector のものが同じファイルを読めるようにするためです。
- 終了コード: 0 正常に終了、1 シミュレーション中の失敗、3 準備の失敗（設定ファイル、接続）、4 使い方の誤り。

Vector SIL Kit Network Simulator との差し替え:
- 実行ファイルを替えるだけで済むように作っています。どちらも同じ participant 名（既定 `NetworkSimulator`）でネットワーク名の模擬を引き受けるので、各ピアのコード、設定ファイル、システムコントローラーの必須 participant の一覧は変わらない想定です。Vector のものでの動作はまだ確かめていません。Vector のものは CANoe のライセンスが要ります。
- Vector のものは、スタッフビットを含む送信時間、相手のいない送信の ACK エラーとエラー状態、CAN FD、LIN、Ethernet、FlexRay を模擬します。簡易版はどれもしません。同じ設定でも時刻は一致しません。
- vehicle-sil のポートは、どちらのシミュレーターでも userContext に送信時刻を入れます。Vector のものにとっては意味のない値です。Vector のものでの決定論とタイミングは確かめていません。

既知の制限:
- 送信の開始は、送った時刻の後の最初のステップの境界より前になりません。その間に始まった優先度の低いフレームが先に出ることがあります（ずれは最大でステップ幅 + 1 フレーム）。
- 送信キューがあふれたときの扱いが、タイミングありの仮想バス（`send()` が同期で `QueueFull`）と簡易版（非同期の `TransmitQueueFull` の送信確認。フレームは送られない）で違います。あふれる場面では、両者のトレースは一致しません。J1939 などのフレームを失わないために、ポートの `transmit_queue_capacity` を `--transmit-queue` 以下にします（上の「SIL Kit のポート」）。
- 同じ時刻に別のコントローラーから同じ ID、データのフレームが出たときの順序は、各コントローラーが最初の要求を出した順（届いた順）で、決定的ではありません。受信側に届く内容は同じです。
- 送信を起点とするプロトコルのタイマー（J1939 の T3 など）は、ポートに渡したステップの時刻から数えます。バスの待ちのぶん、実際の送信より早く数え始めます（「トランスポートプロトコル」の制限）。
- Network Simulator API は SIL Kit の experimental な API です。使うのは `libs/can_netsim/src/can_bus_simulator.cpp` だけです。SIL Kit を更新したら、このシミュレーターのテストを回します。

## J1939

`vehicle_sil::j1939`（名前空間 `vehicle_sil::j1939`、ヘッダー `vehicle_sil/j1939/`）は、vehicle-sil の J1939 スタックです。外部の J1939 ライブラリを使いません。API はまだ変わることがあります。今あるのは、識別子と PGN、単一フレームの送受信、Request（PGN 59904）への応答、Acknowledgement（PGN 59392）、タイマーの仕組み、NAME と Address Claim（任意アドレスの選択、アドレス表、Commanded Address の処理、アドレス違反を含む）、トランスポートプロトコル（BAM と RTS/CTS。9〜1785 バイト）、部品を作るための道具箱（`Subscription`、`Timer`、`Node::attach`）、読み取り専用の観測口（`Observer`）、プロトコル部品の DM13（放送の抑制）、診断（DM1、DM2、DM3、DM11、DM22）、識別情報、ECU アプリ向けの周期送信（`PeriodicSender`）、Request の送信と応答待ち（`Requester`）、受信の監視（`Monitor`）、Commanded Address の送信（`AddressCommander`）、独自プロトコルのコマンドと応答（`Exchange`）です。ノードの作り方は [j1939-guide.md](j1939-guide.md) にあります。ETP はありません。NAME を指定しないノードは、生成時に指定した固定のアドレスで動きます（試験用）。

- `vehicle_sil::can` だけに依存します（SIL Kit なし）。スレッドを持たず、時計を読みません。`Network` が `Steppable` で、ホストがステップで回します。
- 時刻は `SimTime` です。受信したメッセージの時刻はフレームの時刻（仮想時間では送信側のステップ時刻）、送信の時刻は送ったステップの時刻です。

### 責務の境界（エンジン、道具箱、部品）

機能を足すときは、次の 3 つのどこに置くかを先に決めます。規格に含まれるという理由だけでエンジンに入れません。

- エンジン（`Network` と `Node`）: J1939 の通信を成り立たせるもの、ノード全体の状態と通信上の正しさ、複数の部品が共有する通信資源、部品ごとに持つと競合や不整合が起きるもの。今ある機能（識別子と PGN、送受信と配送、NAME とアドレスと Address Claim、Commanded Address の受信、Request の配送と NACK、Acknowledgement の送信、トランスポートプロトコルのセッション・タイムアウト・中断・送信キュー、統計とポートの事象の集計）はすべてエンジンです。
- 道具箱: 部品を安全かつ簡潔に書くための小さな共通機能（イベントの登録の RAII、仮想時間のタイマー、ノードが部品を所有する仕組み）。足すのは、複数の部品が共通に必要とし、既存の API では安全さや簡潔さが足りず、特定のプロトコルの処理を含まないものだけです。
- プロトコル部品: 固有の手順や状態機械を持ち、ノード全体の状態や共有資源を直接変えず、他の部品と独立に足し外しできるもの（周期送信、Request を送って応答を待つ処理、診断、機能安全通信、Memory Access、独自プロトコルなど）。
- 部品は、ノードの公開 API と道具箱だけで作ります。スレッドやイベントループを持たず、ステップの中だけで動きます。エンジンの内部を操作するためだけの公開 API は足しません。同じ機能をエンジンと部品の両方に作りません。
- 境界をまたぐ機能は、通信の整合を保証する部分をエンジンに、プロトコルの手順を部品に分けます（例: Request の配送はエンジン、Request の発行と応答待ちと時間切れは部品。Commanded Address の送信は部品、受信したときのアドレスの変更とトランスポートプロトコルのセッションとの整合はエンジン）。
- 部品は `on_any_request` を使いません（応答者のない PGN への既定の NACK を奪うため）。1 つの PGN の応答者は 1 つです（同じ PGN に複数あると、先に登録したものだけが呼ばれます）。
- PG と SPN の定義（信号の配置、スケーリング、特別な値）は、エンジンにも部品にも入れません。型付きメッセージ（`vehicle_sil::j1939_codec`）がバイト列との変換だけを受け持ち、エンジンと部品はバイト列を扱います。
- DM13 による放送の停止は、エンジンでは行いません。エンジンはどの送信が「放送」かを区別できないためです。抑制の状態は部品（`BroadcastSuppression`）がノードに 1 つ持ち、自分で周期的に放送する部品（DM1 など）がそれを読みます。アプリが `Node::send()` で直接送るメッセージは抑制されません。

### 値の型

- `Pgn` は 18 ビット（拡張データページ、データページ、PF、PS）です。PF が 240 未満（PDU1）の PGN は宛先を持ち、PGN の PS は 0 です。PS が 0 でない PDU1 の値と 18 ビットを超える値は不正です。予約ビット（拡張データページ）は PGN の一部として扱い、フレームを捨てません。
- `Identifier` は 29 ビットの識別子の各フィールドです。PDU1 では PS が宛先、PDU2 では宛先は常にグローバル（255）です。不正な PGN、優先度 8 以上、PDU2 で宛先がグローバル以外のものは `encode()` が `std::invalid_argument` を投げます。
- アドレス 254 は未取得のアドレス、255 はグローバル宛てです。ノードのアドレスは 0〜253 です。

### Network と Node

- `Network` は 1 つの CAN チャネルにある、このプロセスのノード（`Node`）の集まりです。ノードは `add_node()` で作り、`Network` が所有します（`Network` の寿命まで有効）。ノードの追加は、ステップの外か、ホストのアプリの処理の中か、同じホストの別の `Network` のハンドラの中です。ステップの中で追加したノードは、その `Network` の次の `begin()` から加わります（それまでは受信せず、Address Claim も始めません。ホストへの登録順によらず同じです）。自分の `Network` のハンドラ、応答者、タイマーからの追加は使い方の誤りです。別のホストから追加する場合は、その `Network` から見てステップの外なので、次のステップから加わります（ホストを回す順で決まります）。
- 受信の振り分け: 11 ビットのフレーム、送信元 255 のフレーム、送信元 254（アドレスを持たないノード）からの Address Claimed と Request 以外のフレームは、無視して数えます。各ノードには、宛先が自分のアドレスかグローバルのフレームだけを渡します（他のアドレス宛ての PDU1 は渡しません）。
- ローカル折り返し: 同じ `Network` のノードが送ったフレームは、ポートが受け付けた（`Accepted`）ものだけが、次のステップで他のノードに届きます。送ったノード自身には届きません。同じアドレスのノード同士も互いのフレームを受けます。
- ハンドラ（`on_message`、`on_any_message`）に渡す `Message` のデータは、呼び出しの間だけ有効です。ハンドラの登録は、配送中のフレームが終わった時点で反映します（そのフレームでは呼ばれません）。解除は、配送中でもその時点から効き、そのフレームの残りでも呼ばれません（ハンドラの関数オブジェクト自体は配送の終わりに破棄します）。関数オブジェクトの破棄（キャプチャのデストラクタ）の中から、ほかのハンドラを解除・登録してもかまいません。

### 1 ステップの処理

- `process()` は、ポートから受けたフレームと前のステップのローカル折り返しを、時刻順に処理します。同じ時刻のフレームは、送信元アドレスごとの列（列の中は到着順。同じ時刻・同じアドレスでは外部のフレームが折り返しより先）を作り、列の先頭の CAN ID が小さいものから 1 つずつ取ります（CAN の調停の近似）。そのため、同じ送信者の順序は保たれ、結果はポートの到着順とステップ幅によりません。ただし、同じアドレスを使う別の送信者（アドレスの衝突中）が同じ時刻に送ったフレームは 1 つの列に入り、その中は到着順です。
- タイマーは 2 種類です。タイムアウトは、期限より後の時刻のフレームを処理する直前か、ステップの最後で期限 < now のときに発火します（期限と同じ時刻のフレームが先）。予定（scheduled）は期限 ≤ now の最初のステップで発火します。どちらも、期限がフレームの時刻より前ならそのフレームの前に発火します。同時に発火できるタイマー同士は（期限、開始の順）です。タイマーの中で開始したタイマーは、期限がそれを開始したタイマーの期限より後なら、同じ発火の機会に（期限、開始の順に）発火します。そのため、期限から始め直す周期は、周期がステップ幅より短くても、ステップの中ですべての期限の分だけ発火し、遅れがたまりません（実時間で長く止まった後は、遅れた分を 1 ステップで取り戻します）。期限がその期限と同じか前のもの（同じ期限での始め直しなど）は、次の発火の機会（次のフレームの前か、ステップの最後。ステップの最後なら次のステップ）まで発火しません。期限がちょうど now のときは予定だけが発火し、同じ期限のタイムアウトは次のステップになります。タイムアウトの判定はステップ幅によりません。`next_deadline()` は最も早い期限を返します（タイムアウトは期限より後の最初のステップで発火します）。
- ハンドラ、Request の応答者、タイマーから送ったフレームは、そのステップの `flush()` で出ます。
- `flush()` の順序: 制御フレーム（Address Claimed、Acknowledgement、トランスポートプロトコルの TP.CM）、続いてデータキュー、最後にトランスポートプロトコルのデータパケット（TP.DT。データキューと合わせて 1 ステップの上限 `max_frames_per_step` まで。残りは次のステップ）。ポートが `QueueFull` を返したら、そのフレームと後続を残して次のステップで出し直します（順序を保つ）。`Failed` のフレームは捨てて数え、後続は続けます。

### 送信

- `Node::send()` は 8 バイトまでなら単一フレーム、9〜1785 バイトならトランスポートプロトコル（下）です。結果は戻り値で返します（`Ok`、`QueueFull`、`TooLong`（1786 バイト以上）、`InvalidArgument`（不正な PGN、優先度 8 以上、単一フレームの PDU2 を 1 つの宛先へ、宛先 254）、`NoAddress`（アドレスを失ったノード）、`Busy`（下））。
- 送信はステップの中（ハンドラ、応答者、タイマー、ホストのアプリの処理）だけです。ステップの外では `std::logic_error` です。
- `Node::request(pgn, destination)` は Request（3 バイト、優先度 6）を送ります。応答を待つ、時間切れで送り直す、といった要求側の手順はエンジンではなく部品（`Requester`）にあります。
- `Node::can_send_now()` は、送ったものがすぐ自分のアドレスから出るときに true です（NAME なし、`Claimed`、待たない `Claiming`）。要求の後の待ちの間、最初の要求の前、アドレスがないときは false です。値はアドレスか状態の変化（`on_address_change`）でだけ変わるので、自分で送る部品はその通知で始めて止めます。

### Request と Acknowledgement

- 宛先が自分かグローバルの Request を処理します。3 バイト未満の Request は無視して数え、3 バイトを超えるものは先頭 3 バイトを要求された PGN とします。
- 送信元 254 の Request には、Address Claimed（PGN 60928）の Request を除いて応答しません。Address Claimed の Request は Address Claim の手順で答え（下）、NACK も ACK もしません。NAME のないノードは答えません。
- 応答者（`on_request(pgn, ...)`、`on_any_request(...)`）がいれば呼びます。PGN ごとの応答者が先で、同じ PGN に複数あれば先に登録したものです。応答者は `RequestContext` で、`respond(data)`（要求された PG を送る。PDU1 の PG を宛先指定の Request で求められたら要求元へ、それ以外はグローバルへ）か `acknowledge(code)` を呼びます。どちらも呼ばなければ何も送りません。
- 後で答える（何ステップかかかる処理の後で答える）ときは、`RequestContext::defer()` で要求を値（`PendingRequest`）として取っておき、後で `Node::respond(pending, data)` か `Node::acknowledge(pending, code)` で答えます。宛先と送り方は `RequestContext` で答えたときと同じです。ステップの中だけで呼べます。その間にノードがアドレスを失うか変えると、答えは送らず `NoAddress`（`acknowledge` は false）を返します（要求は前のアドレスへのものだったため）。別のノードの要求を渡すと `std::invalid_argument` です。
- 9 バイト以上の `respond()` はトランスポートプロトコルで送ります（PDU2 の PGN でも、宛先指定の Request には要求元へ RTS/CTS。下の「トランスポートプロトコル」）。
- `acknowledge()` は宛先指定の Request のときだけ送ります。グローバルの Request には送りません。
- `acknowledge()` は、制御キューがあふれて捨てたときにも false を返します。
- 応答者がいなければ、宛先指定の Request に NACK を返し、グローバルの Request には何も返しません。要求元のアドレス要求（Address Claim）を見ているかどうかは条件にしません。
- Acknowledgement のフレーム: 優先度 6、宛先グローバル、データは [制御バイト、0xFF、0xFF、0xFF、要求元のアドレス、要求された PGN（3 バイト、受けた値のまま）]。
- `parse_acknowledgement(message)` は、受けた Acknowledgement（PGN 59392、8 バイト以上）の欄（`Acknowledgement`: 制御バイト、グループ機能、名指しされたアドレス、PGN）を返します（それ以外は空）。形を知っているのはエンジンだけにし、部品は自分で解析しません。
- 規格との一致を確かめていない点: 要求された PGN が不正な値（PS が 0 でない PDU1、18 ビット超）のときの扱い（一致する応答者がないので宛先指定なら NACK）、拡張データページのビットが立ったフレームの扱い、Acknowledgement の 2〜4 バイト目の値。

### NAME と Address Claim

- `Name` は 64 ビットの NAME です。下位ビットから、Identity Number（21 ビット）、Manufacturer Code（11）、ECU Instance（3）、Function Instance（5）、Function（8）、予約（1）、Vehicle System（7）、Vehicle System Instance（4）、Industry Group（3）、Arbitrary Address Capable（1）です。値が小さいほど優先されます（最上位の Arbitrary Address Capable のビットが立った NAME は、立っていない NAME より常に低い優先度です）。バス上では 8 バイトで、下位バイトが先です。 例と試験で使う Manufacturer Code `0x7F0` は仮の値です。実際の ECU には、その製造者に割り当てられた値を設定します。
- `NodeOptions::name` を指定したノードは、`address` を要求します。任意アドレス可能（Arbitrary Address Capable）な NAME なら、`address` に 254 を指定でき、そのときは任意アドレスの範囲（下）の最初の空きを要求します。状態は `Node::address_state()` で読めます（`NotStarted`、`Claiming`、`Claimed`、`CannotClaim`。NAME のないノードは `Fixed`）。
- 開始: ノードが加わった最初のステップで、そのステップのフレームを処理した後に、Address Claimed（優先度 6、宛先グローバル、データは NAME）を送ります。事前に Request for Address Claimed は送りません。
- 要求の後の待ち: 送ったステップの時刻から 250 ms の間（`Claiming`）、そのノードが送るもの（`send()`、`request()`、Request への応答、ACK/NACK）をノードの保留領域（`NetworkOptions::hold_capacity`、既定 16 件）に持ち、待ちが明けたステップで、その時点のアドレスで送ります。保留領域があふれた `send()` は `QueueFull` です。Address Claim の手順のフレーム（Address Claimed、Cannot Claim、Request for Address Claimed とその応答）は待ちません。例外として、任意アドレス不可で、アドレスが 0〜127 か 248〜253 のノードは待ちません（`Claiming` の間もすぐに送ります）。待ちの終わりは予定のタイマーなので、期限以後の時刻に競合する Address Claimed が届き、それが期限と同じステップで処理される場合は、ステップ幅によって、保留を送ってから負けるか、先に負けて保留を捨てるかが変わります（下の制限）。待ちが明けたときにデータキューがあふれていれば、`send()` が `Ok` を返したメッセージでも捨てて数えます（`held_dropped`）。
- 競合: 自分のアドレスで、他のノードの Address Claimed（8 バイト以上）を受けたら NAME を比べます。自分の NAME が小さければ、Address Claimed を送り直します。同じ時刻の原因（競合の Address Claimed、アドレス違反、移った原因のフレームか Commanded Address、最初の要求ならそのステップの時刻）には 1 回だけ送ります（同じ時刻の要求は互いに届くため。回数はフレームの時刻だけで決まり、ステップ幅によりません）。大きいか同じならアドレスを失います。保留中のメッセージと、アドレスが変わる前に送信キューに入ったフレーム（元のアドレスに戻った後でも。Cannot Claim も含み、送信元 254 の Request だけは除く）は捨てて数えます。
  - 任意アドレス可能なノードは、任意アドレスの範囲（`NetworkOptions::arbitrary_address_first`〜`arbitrary_address_last`、既定 128〜247）のうち、アドレス表にも同じ `Network` の他のノードにもない最小のアドレスを選び、すぐに Address Claimed を送って、待ちをやり直します（`Claiming`）。負けたアドレスは、勝った NAME が表に入るので選びません。
  - それ以外のノードと、空きのない任意アドレス可能なノードは `CannotClaim`（アドレス 254）になり、`cannot_claim_delay(name)` の後に Cannot Claim（送信元 254 の Address Claimed）を送ります。遅延は 0〜153 ms（0.6 ms 刻み）で、NAME から決まる擬似乱数です（同じ NAME からは常に同じ値）。起点は負けを知ったフレームの時刻です。最初の要求で空きがなかったときは、そのステップの時刻です。
- Request for Address Claimed（宛先がグローバルか自分のアドレス）: `Claiming` と `Claimed` のノードは Address Claimed（宛先グローバル）を要求の数だけ送り、`CannotClaim` のノードは遅延の後に Cannot Claim を送ります（未送信の Cannot Claim があればまとめて 1 回）。ノード自身が送る Request for Address Claimed（グローバルか自分宛て）には、そのノードも答えます。最初の要求の前のノードが送る Request for Address Claimed は送信元 254 で出し、そのノード自身は答えません（要求はすぐ後に送るため。ほかの送信は保留します）。アドレスを失ったノードが送れるのは、Request for Address Claimed（送信元 254）だけです。
- アドレスを失ったノードは、宛先指定のフレームを受けず（グローバル宛てだけ）、Request for Address Claimed 以外の Request に応答しません。
- アドレス違反: `Claiming` か `Claimed` のノードは、他の送信者が自分のアドレスを送信元にしたフレーム（Address Claimed と Request for Address Claimed を除く。8 バイト未満の Address Claimed は含む）を受けたら、Address Claimed を送り直します（原因の時刻ごとに 1 回。競合への応答と同じ規則）。状態は変えません。そのフレームは普通に配送します。数は `address_violations` です。NAME のないノードは検出しません。
- Commanded Address（PGN 65240。NAME 8 バイトと新しいアドレス 1 バイト）: 宛先がグローバルか自分のアドレスで、NAME が自分のものなら処理します。任意アドレス可能なノードは、新しいアドレスへ移ります（保留中のメッセージと元のアドレスのフレームを捨て、Address Claimed を送って待ちをやり直します。`CannotClaim` のノードも移ります。予定の Cannot Claim は送りません）。新しいアドレスが今のアドレスと同じなら、Address Claimed を送り直すだけです。新しいアドレスに優先される NAME がいれば、競合で負けて、任意アドレスの範囲の空きへ移ります。任意アドレス不可のノード、9 バイトでないもの、新しいアドレスが 254 か 255 のもの、最初の要求の前のノードへのものは無視して数えます（`commanded_addresses_ignored`。8 バイト未満のものは NAME が分からないので数えません）。Commanded Address は 9 バイトなので、トランスポートプロトコル（BAM か RTS/CTS）で届きます。
- アドレス表（`Network::address_table()`）: 同じ `Network` のノード以外の制御機能について、アドレスと NAME の対応を持ちます（`name_at(address)`、`address_of(name)`、254 件固定）。受けた Address Claimed（8 バイト以上）で更新します。同じ NAME が新しいアドレスを要求したら、古いアドレスの対応を外します。自分のノードがアドレスを要求すると、そのアドレスにいた優先度の低い NAME を外します（Address Claimed を送信キューに入れた時点で外すので、それが送れなかった場合も外れたままです。相手が要求し直せば入ります）。Cannot Claim を受けたら、その NAME の対応を外します。同じアドレスを複数の NAME が要求したら、優先される（小さい）NAME を残し、要求の届いた順によりません。自分のノード（`Claiming` か `Claimed`）が持つアドレスへの要求は、その NAME が自分のノードの NAME より大きい（優先度が低く、負ける）ときは入れません。ローカルのノードの Address Claimed（折り返し）では更新しません。8 バイト未満の Address Claimed は要求として扱わず、数えます（`address_claims_malformed`）。
- 今の対応（`Network::address_of(name)`、`Network::name_at(address)`）: 同じ `Network` のノード（`Claiming` か `Claimed`）とアドレス表の両方から引きます。相手が同じプロセスのノードでも、ほかの participant でも同じに使えます。受けた Address Claimed と Cannot Claim は、ノードのハンドラが呼ばれる前に表へ入るので、そのハンドラの中では新しい対応が見えます。同じ `Network` のノードが相手のときは、そのノードの状態が変わった時点で変わります（別のノードがそのアドレスを要求したときは、相手のノードがその要求を処理した後、つまり自分のノードのハンドラより後のことがあります。相手のノードの Cannot Claim か新しいアドレスの Address Claimed（折り返し）で見えます）。
- 状態の通知: `Node::on_address_change(handler)` は、アドレスか状態が変わるたびに `AddressChange`（新旧の状態とアドレス、原因の時刻）で呼ばれます。ハンドラと同じく、ステップの中で呼ばれ、送信できます。登録と解除の規則もハンドラと同じです。
- 規格との一致を確かめていない点: 同じ NAME の別の送信者からの Address Claimed を負けとして扱うこと、Cannot Claim の遅延の起点と擬似乱数の作り方、待ちの間の ACK/NACK を保留すること、任意アドレスの範囲と最小の空きを選ぶこと、アドレス違反への応答の頻度（原因の時刻ごとに 1 回）と 8 バイト未満の Address Claimed を違反に含めること、任意アドレス不可のノードが Commanded Address を無視すること、同じアドレスの要求が重なったときにアドレス表に小さい NAME を残すこと（その NAME の制御機能がいなくなっていても、要求し直すまで残ります）。
- 制限: 待ちの終わりは予定のタイマーなので、期限と同じステップの中で期限以後の時刻に届いた競合でも、保留を送ってから負けることがあります（このとき `on_address_change` の時刻は、Claimed の後に、それより前の時刻の CannotClaim になります）。同じアドレスの別の送信者が同じ時刻に送ったフレームの処理順（SIL Kit では決まらない）で、バスに出るフレームと結果は変わりませんが、`held_dropped` と `address_violations` の数は変わることがあります。Address Claimed は送信キューから出た時刻ではなく、入れたステップの時刻から待ちを数えます。制御キューがあふれて Address Claimed を捨てた場合も、状態は進みます（`control_frames_dropped` で数えます）。`timer_capacity` は、要求するノード 1 つにつき 2 つ必要です（足りなければ `add_node()` が `std::invalid_argument`）。`CannotClaim` になった任意アドレス可能なノードは、後でアドレスが空いても自分からは要求し直しません（Commanded Address で移れます）。
- まだないもの: ノードの削除、バスオフの後の扱い。Commanded Address の送信は部品（`AddressCommander`）です。応答しない制御機能を表から外すこと（表から外れるのは、その NAME が別のアドレスを要求したとき、Cannot Claim を送ったとき、自分のノードがそのアドレスを要求して勝つときだけです）。

### トランスポートプロトコル（J1939-21 の TP）

9〜1785 バイトのメッセージを、TP.CM（PGN 60416）と TP.DT（PGN 60160）のフレームに分けて送り、受けたものを組み立てます。どちらのフレームも優先度 7 です。TP.CM と TP.DT のフレームはハンドラに渡しません。組み立てたメッセージを、単一フレームと同じ経路（`on_message`、`on_any_message`、Request と Commanded Address の処理）で渡します。

送信:
- 宛先がグローバルなら BAM、1 つのアドレスなら RTS/CTS です。PDU2 の PGN も、宛先を指定すれば RTS/CTS で送ります（単一フレームでは不正です）。`priority` は検査だけして使いません。
- 1 つのノードが同時に送れるのは、BAM 1 本と、宛先ごとに RTS/CTS 1 本です。走っている転送と同じ種類の送信は `Busy` です。送信セッションの空きがない（`bam_transmit_sessions`、`rts_transmit_sessions`、既定は各 4）ときは `QueueFull` です。
- 結果は `Node::on_transfer_end(handler)` に `TransferEnd`（PGN、宛先、長さ、結果、Abort の理由、原因の時刻、転送の識別子 `id`、データのパケットをすべて出したか `all_packets_sent`）で届きます。`send()`、`respond()`（`RequestContext` と `Node`）が転送を受け付けると、その直後の `Node::last_transfer_id()` がその転送の `TransferId` を返します（1 フレームの送信と、受け付けなかった呼び出しでは空）。ID は `Network` の中で一意で、応答待ち行列で待った転送も、Claim の後の待ちで保留した転送も、受け付けたときの ID で終わります。同じノードが同じ PGN と宛先の転送を重ねても、終わりを取り違えません。`all_packets_sent` が false なら、受信側はメッセージを組み立てられていません（最後のパケットの前に終わった、始まらなかった）。true で完了でなければ（EndOfMsgACK が来ない、その間にアドレスが変わった）、受信側が受け取ったかは分かりません。`send()` と `RequestContext::respond()` が `Ok` を返した転送には、必ず 1 回だけ届きます（`Network` を止めたときに走っていたものを除く）。結果は `Completed`（RTS/CTS は EndOfMsgACK を受けた、BAM は最後のパケットを出した）、`AbortedByPeer`、`Aborted`（このノードが Abort を送った）、`Cancelled`（フレームを出さずに終えた: 下のアドレスの変化と、まれに、Claim の後の待ちで保留した転送が、待ちが明けたときに制御キューに入らなかった場合）です。BAM の `Completed` は、最後のパケットを出した次のステップで届きます（時刻は最後のパケットを出したステップの時刻。その間にアドレスが変わっても `Completed` です）。
- 受ける転送は `Node::on_incoming_transfer(handler)` に `IncomingTransfer`（開始か終わり、ID、PGN、送り手、宛先、長さ、終わりの結果と理由、原因の時刻）で届きます。RTS/CTS は受け入れた宛先のノードだけに、BAM は送り手を除く、開始のときに参加していたすべてのノードに届きます（その後で足したノードには、開始も終わりも届きません）。開始を届けた転送には、終わりを必ず 1 回届けます（`Network` が失敗するか破棄される前に限ります）。CTS を制御キューに入れられずに受け入れなかった RTS は、開始も終わりも届けません。終わりの結果は `Completed`（組み立てたメッセージはこの通知の直後に配送）、`AbortedByPeer`、`Aborted`（このノードが Abort を送った。時間切れなど）、`Lost`（BAM のパケットの欠落か時間切れ）、`Cancelled`（アドレスの変化、同じ送り手の同じ転送のやり直し、CTS が送れなかった）です。ID は送る転送と同じ番号の列から振ります。ハンドラの規則は `on_transfer_end` と同じで、ステップの中で呼ばれます。
- BAM: 告知を出したステップから `bam_packet_interval`（既定 50 ms、10〜200 ms）ごとに 1 パケットを出します。間隔は前のフレームを出したステップの時刻から数えるので、ステップ幅で割り切れなければ切り上がります（短くはなりません）。BAM への Abort は受けても無視します。
- RTS/CTS: RTS のバイト 5（1 回の CTS で送れる最大のパケット数）は `rts_max_packets_per_cts`（既定 255 = 制限なし）です。CTS で求められたパケットを、CTS を受けたステップでまとめて出します。CTS の許可数が残りより多ければ最後のパケットまで出します。0 個の CTS は保留（Hold）です。
  - 送信側が Abort を送るとき: CTS の次のパケット番号が 0、総数より大きい、最後に求められた窓の先頭より前、出した最大の番号 + 1 より後のとき（理由 7）、許可数が RTS のバイト 5 を超えたとき（理由 250）、前の窓のパケットを出し終える前に次の CTS が来たとき（理由 4）、T3 と T4（理由 3）。
  - 全パケットを出す前の EndOfMsgACK、別の PGN の CTS・EndOfMsgACK・Abort は無視します。
- Request への長い応答（`RequestContext::respond()` の 9 バイト以上）: 宛先指定の Request には要求元へ RTS/CTS、グローバルの Request には BAM で送ります。同じノードの同じ種類の転送が走っているか、セッションの空きがなければ、ネットワークの応答待ち行列（`response_queue_capacity`、既定 8）に入れ、`flush()` の始めに入れた順に始めます。待ち行列があふれたら `QueueFull` です。順序は待ち行列の中だけで保ちます。同じノードの `send()` は、セッションが空いていれば待ち行列より先に始まります。
- `send()` の 9 バイト以上の送信は、既定では同じノードの転送が走っていると `Busy` です。`WhenBusy::Queue` を指定すると、`respond()` と同じ応答待ち行列（容量を共有）で待ち、前の転送が終わってから始まります。複数の部品が同じノードから長いメッセージを送るとき（DM1 の BAM と、識別情報の応答の BAM など）に、部品ごとに再試行を書かないためです。

受信:
- BAM: 送信元ごとに 1 本です。告知の宛先がグローバルで、9〜1785 バイト、パケット数がサイズと合い、PGN が正しいものだけを受けます。それ以外の告知は無視して数え、走っている BAM には影響しません。同じ送信元の新しい正しい告知は、走っている BAM を捨てて置き換えます。パケットの番号が期待と違えば（欠落、重複、順序違い、0）メッセージを捨てます（BAM は再送を求められないため。Abort は送りません）。受信のセッションが満杯（`bam_receive_sessions`、既定 8）なら、新しい告知を無視して数えます。パケットの間隔の下限は検査しません。送信元の Address Claim を見ていなくても受けます。
- RTS/CTS: 送信元と宛先の組ごとに 1 本です。宛先は、アドレスを持つ（`Claiming`、`Claimed`、NAME なしの固定）ローカルのノードです。グローバル宛ての RTS と、ローカルのノードがいない宛先の RTS は無視します。
  - RTS を Abort で断るとき: 1785 バイト超（理由 9）、9 バイト未満・パケット数の不一致・バイト 5 が 0・不正な PGN（理由 250）、受信のセッションが満杯（`rts_receive_sessions`、既定 8。理由 1）、同じ送信元から別の PGN の RTS（理由 1。走っている転送は続けます）。同じ PGN の RTS は、走っている転送をフレームを出さずに捨てて、新しく始めます。
  - CTS の許可数は min(RTS のバイト 5, `cts_window`（既定 16）, 残り) です。窓を受け終えたら次の CTS、全部受けたら EndOfMsgACK を送ってメッセージを渡します。
  - 期待どおりの番号のパケットを受けます。直前のパケットの重複と、今の窓の先頭より前の番号（前の窓の遅れたパケット）は無視します。それ以外（先の番号、0、窓の中の古い番号）と、データが足りない（DLC が短い）パケットは欠落として扱い、窓の最後の番号のパケットが来たら、欠けた先頭から CTS で再送を求めます（欠落の後、再送の CTS までのパケットは捨てます）。再送の要求は、窓を受け終えるまでに 2 回までで、3 回目が要るときは Abort（理由 5）です。窓の最後のパケットが欠けたときは T1 で終わります。
  - 受信側は保留（0 個の CTS）を送りません。送信元からの CTS と EndOfMsgACK は無視します。送信元の Abort でフレームを出さずに終えます。
- パケットの優先度は問いません。組み立てたメッセージの優先度は、RTS か BAM の告知の優先度です。時刻は最後のパケットの時刻です。
- 組み立てた Address Claimed（PGN 60928）と Request（PGN 59904）は、単一フレームの PG なので配らずに数えて捨てます。Commanded Address など、ほかの PGN は単一フレームと同じ経路で渡します。

タイマー（タイムアウトはすべて Abort（理由 3）。BAM の受信だけはフレームを出さずに捨てます）:

| タイマー | 値 | 起点 |
|---|---|---|
| BAM の受信の T1 | 750 ms | 告知かパケットのフレームの時刻 |
| 受信側の T1（パケットの間） | 750 ms | そのセッションに届いた最後のパケット（無視したものも含む）のフレームの時刻 |
| 受信側の T2（CTS の後の最初のパケット） | 1250 ms | CTS を出したステップの時刻 |
| 送信側の T3（RTS か窓の最後のパケットの後） | 1250 ms | それを出したステップの時刻 |
| 送信側の T4（保留の後） | 1050 ms | 保留の CTS のフレームの時刻（保留が続けば数え直す） |

- タイムアウトは期限より後の時刻で判定します（期限ちょうどのフレームは受け付けます）。判定はステップ幅によりません。送信を起点とするタイマーは、フレームをポートに渡したステップの時刻から数えます（ポートが `Failed` を返した場合も同じ）。ポートの `QueueFull` で出し直しが続けば、そのぶん遅れて数え始めます。

アドレスとの関係:
- Claim の後の待ちの間に送った転送は、セッションを確保して待ち、待ちが明けたとき保留したほかのメッセージと一緒に、保留した順に始めます。待ちの間に受けた RTS への CTS も、待ちが明けてから出します。
- ノードがアドレスを失うか変えると、そのノードの送受信の転送と応答待ち行列の応答を、フレームを出さずに終えます（送信は `Cancelled`）。古いアドレスから Abort は出せないためです。キューに残っていた TP.CM も出しません。最初の Claim では終えません。この通知（Claim の後の待ちで保留していた転送を含む）の時点で、ノードの `address()` と `address_state()` は変化の後のもので、`on_address_change` はその後に届きます。`on_transfer_end` のハンドラがその通知の中で送り直した転送は、同じアドレスの変化では終えません（新しいアドレスと状態で扱います）。
- アドレス表で、あるアドレスの NAME が外れるか別の NAME に替わると、そのアドレスとの RTS/CTS の転送、その送信元の BAM の受信、そのアドレスへの応答待ち行列の応答を、フレームを出さずに終えます（時刻は原因の Address Claimed のフレームの時刻）。アドレス表にない相手（Address Claim を見ていない相手）と、同じ Network のノード（アドレス表に入らない）は対象外です。同じ Network のノード同士の転送では、相手のノードのアドレスが変わっても、こちらのセッションはタイムアウトで終わります。
- 終わったか置き換わったセッションの TP.CM がキューに残っていれば、出さずに捨てて数えます（`transport_stale_dropped`）。

規格との一致を確かめていない点: 送信側が T3・T4 で Abort（理由 3）を送ること、再送の要求の回数（窓ごとに 2 回）と、欠落の後のパケットを捨てて窓の最後で再送を求めること、直前のパケットの重複を無視して古い番号を欠落とすること、DLC が短いパケットの扱い、RTS を断る理由の選び方（9 バイト未満・バイト 5 が 0・不正な PGN に 250、セッションの満杯に 1）、CTS の次のパケット番号の有効範囲、許可数が RTS のバイト 5 を超えたときの理由 250、同じ送信元の新しい BAM で置き換えること、BAM の告知の検査（8 バイト以下を無視）、Abort のバイト 3〜5 を 0xFF にすること、組み立てたメッセージの優先度と時刻。

制限:
- 停止したときに走っている転送は、フレームを出さずに捨てます（Abort を送ってから止める仕組みはありません）。
- 受信側は保留を送りません。対応する PGN を登録して RTS や BAM を断る仕組みはありません。ETP（1785 バイト超）はありません。
- 送信を起点とするタイマーと BAM の間隔は、ポートに渡したステップの時刻から数えます。バスの混雑で実際の送信が遅れると、そのぶん早く数え始め、BAM のバス上の間隔は縮むことがあります。
- 同じ Network のノードどうしの転送も、ローカル折り返し（次のステップに届く）で同じ手順で進みます（アドレスが変わったときの違いは上）。
- `next_deadline()` は、CTS で求められたパケットなど、タイマーを持たない次の flush の仕事を含みません。
- 再送を求めた後に、前の窓の遅れたパケットのうち、再送の窓の中の番号のもの（例: 5 が欠けて 5〜16 を求めた後に届いた元の 10）は欠落として扱うので、再送の要求をもう 1 回使うことがあります（回復はします。窓の先頭より前の番号は無視します）。

### 道具箱（`Subscription`、`Timer`、`Node::attach`）

部品とアプリが安全に書けるようにするための、プロトコルに依らない 3 つの仕組みです（ヘッダー `vehicle_sil/j1939/toolbox.hpp` と `network.hpp`）。

- `Subscription{node, node.on_message(...)}`: ハンドラや応答者の登録（`HandlerId`）を持ち、破棄か `reset()` で解除します。解除の後は、配送の途中でも（自分のハンドラの中でも、別のハンドラの中でも）呼ばれません。ムーブできます。`Network` の破棄の後に破棄しても安全です（何もしません）。
- `Timer(node_or_network, kind, callback)`: 仮想時間のタイマーです。生成時に `timer_capacity` の枠を 1 つ予約し、破棄で返します。`start_at(t)` と `start_after(d)` は動いているタイマーを始め直し、ハンドラ、応答者、タイマーの中、アプリの処理、ステップの外のどこからでも呼べます。過ぎた期限も受け付け、次の機会（次のフレームの前かステップの最後、なければ次のステップ）に発火します。発火の中で始めたタイマーは、それを始めたタイマーの期限より後の期限のときだけ、同じ回で発火します（「1 ステップの処理」）。
  - `start_after(d)` の起点は、処理中の事象の時刻（`Network::event_time()`）です。ハンドラ（応答者を含む）では受けたメッセージの時刻、アドレスの変化と転送の終わりの通知ではその原因の時刻（`AddressChange::time`、`TransferEnd::time`）、`Timer` のコールバックではその期限、それ以外ではステップの時刻（最初のステップの前は 0）です。部品が時間の間隔を数えるときもこの時刻を使うと、結果がステップ幅によりません。コールバックの中で `start_after()` で始め直す周期は、ステップ幅によらず期限がずれず、発火の回数も変わりません（発火はステップ幅の分だけ遅れうる。周期がステップ幅より短いと、1 ステップに何回も発火します）。メッセージを送った時刻から数える間隔（応答の待ち時間など）は、送信がそのステップの `flush()` で出るので、`start_at(*network.now() + d)` で数えます。
  - コールバックの中で、自分自身を含むタイマーを始め直し、取り消し、破棄できます。`Network` の破棄の後の破棄は安全で、`start_at()` は `std::logic_error` です。
  - OS の時計やスレッドは使いません。実時間で動かすときは、ドライバーが `next_deadline()` まで眠ってからステップを回します。
- `node.attach<T>(args...)`: 部品 `T(node, args...)` を作り、`Network` に所有させます。`node.find<T>()` は、そのノードに最初に付けた `T` を返します（なければ nullptr）。どこからでも呼べます（ハンドラの中で付けた部品のハンドラは、次の配送から呼ばれます）。部品は `attach` を使わず、アプリが所有してもかまいません（`Network` より先に、その `Network` のハンドラの外で破棄します）。
- 破棄の順序: `Network` の破棄では、まず全ノードの付けた部品を、最後に付けたものから順に破棄します（ハンドラは呼びません）。その間ノードとタイマーはまだ使えます。部品 A が部品 B を使うなら、B を先に付けます。その後でエンジンを破棄します。部品のデストラクタからは送信できません（ステップの外のため）。
- 部品が自分の利用者に通知するループも、エンジンと同じ規則を守ります。ループは開始時の範囲だけを扱い、通知の後に要素への参照を使わず、通知の前に自分の状態を確定します。

### プロトコル部品（`libs/j1939/parts`）

部品は 1 つずつ別の CMake ターゲットで、エンジン（`vehicle_sil::j1939`）の公開ヘッダーと道具箱だけを使います。部品のターゲットの include パスにはエンジンの `src/` が入らず、エンジンの内部は private です。CTest の `j1939_parts.boundary` が、部品のソースに内部ヘッダーの include、試験用の入口（`detail::NetworkTestAccess`）、クラスを friend にする宣言（隠れた friend の関数は可）や `#define private` などがないこと、部品の CMakeLists.txt がコンパイルの定義・オプションを足さず、include パスに自分の `include/` しか足さず、`vehicle_sil::j1939` と他の部品のターゲットだけをリンクしていることを確かめます。部品は `Observer` を設定しません（`set_observer` も禁止語です。`Network` の観測口は 1 つで、試験と記録のためのものです。部品はハンドラで加わります）。

| ターゲット | 部品 | 内容 |
|---|---|---|
| `vehicle_sil::j1939_broadcast_suppression` | `BroadcastSuppression` | DM13 による放送の抑制の状態（ノードに 1 つ） |
| `vehicle_sil::j1939_diagnostics` | `Diagnostics`、`Identification` | DTC と DM1/DM2/DM3/DM11/DM22、識別情報。`BroadcastSuppression` を使う |
| `vehicle_sil::j1939_periodic_sender` | `PeriodicSender` | 1 つの PG の周期送信。ノードのアドレスと DM13 に従う。`BroadcastSuppression` を使う |
| `vehicle_sil::j1939_requester` | `Requester` | Request の送信と、応答・ACK・時間切れの報告、再送、同じ要求の相乗りと間隔の抑制 |
| `vehicle_sil::j1939_monitor` | `Monitor` | 1 つの PG の最新の受信と途絶（送信元はアドレスか NAME） |
| `vehicle_sil::j1939_address_commander` | `AddressCommander` | Commanded Address の送信と、相手が移ったことの確認 |
| `vehicle_sil::j1939_exchange` | `Exchange` | 独自プロトコルのコマンドを 1 つの相手へ送り、応答を待つ（時間切れ、送り直し、途中の応答、取り消し、自分と相手のアドレスの変化、長いコマンドと長い応答の転送） |

部品の API の「どこからでも呼べる」は、ステップを回すスレッドの上でのこと（ハンドラ、タイマー、アプリの処理、ステップの外）です。部品はロックを持たないので、ほかのスレッドからはホストの `post()` で渡します（「スレッドとキュー」）。

部品が予約するタイマーの数（`timer_capacity` から）: `BroadcastSuppression` が 3、`Diagnostics` が 3、`Identification` が 0、`PeriodicSender` が 1（送る PG ごと。DM13 に従うときは、ノードにまだなければ `BroadcastSuppression` も付けるので、その 3 も）、`Requester` が 2、`Monitor` が 1（`timeout` があるとき）、`AddressCommander` が 2、`Exchange` が 2。NAME を持ち診断を付けたノードは、Address Claim の 2 と合わせて 8 です（既定の 64 で 8 ノード）。周期送信の多いノードでは `timer_capacity` を上げます（足りなければ部品の生成が `std::length_error` で、メッセージに `timer_capacity` と出ます）。周期送信ごとにタイマーを 1 つ使うのは、期限の並べ替えと発火の回の規則をエンジンの `Timer` にだけ持たせるためです（部品の中で同じ仕組みを作り直しません）。

#### DM13（`BroadcastSuppression`）

- ノードに 1 つです。`BroadcastSuppression::of(node)`（なければ既定の設定で付ける）か `BroadcastSuppression::attach(node, options)` で作ります。同じノードに 2 つ目を付けると `std::logic_error` です。コンストラクタは非公開の鍵の型を取るので、それ以外の方法では作れません（自分で生成したものは `of()` と部品から見えず、別の状態になるため。コンパイル時に検出します）。自分で放送する部品はこれを参照し、`suspended()` の間は放送しません。
- 他のノードの DM13: 自分のネットワーク（`BroadcastSuppressionOptions::network`、既定は J1939 Network #1）のコマンドが Stop なら止め、Start なら再開します。自分のネットワークのコマンドが Stop でも Start でもなければ、Current Data Link のコマンドを使います。止めている間は DM13 を受けるたびに（Start でなければ、ほかのネットワークへのものや Hold Signal だけのものでも）6 秒を数え直し、6 秒間 DM13 が来なければ再開します。最後の DM13 からちょうど 6 秒の時刻に届いた DM13 は、まだ止めている間に届いたものとして扱います。8 バイト未満の DM13 は無視します。受けた DM13 の Suspend Signal と Suspend Duration は、送信元自身の一時停止の告知として扱い、停止の指示には使いません。
- 自分の一時停止: `suspend(duration, scope)` で放送を止め、次の機会に DM13 を全体へ送って告げます（全ネットワークのコマンドと Hold Signal は「指定なし」、Suspend Signal は全部の停止か一部の停止、バイト 5〜6 は秒数で、期間なしは 0xFFFF）。期間が過ぎるか `resume()` で再開します（再開は告げません。まだ送っていない告知は取り消します）。
- 抑制は、その状態を読む部品の放送だけに効きます。Request への応答、Address Claim、Acknowledgement、トランスポートプロトコルの制御フレーム、アプリが `Node::send()` で直接送るメッセージは止まりません。

#### 診断（`Diagnostics`）

- DTC は SPN（19 ビット）と FMI で区別し、点灯するランプと点滅の速さを持ちます。`set_active(dtc, active)` で有効・無効にします（どこからでも呼べ、DM1 は次の機会に出ます）。数は `DiagnosticsOptions::dtc_capacity`（1〜445。445 件の DM1 が 1 回の BAM に収まる）までで、超えると `std::length_error` です。有効にするたびに発生回数（1〜126）が増えます。無効にした DTC は「以前に有効」として残ります。
- DM1（有効な DTC）は、ノードがアドレスを持っている間（Claimed か NAME なし）、1 秒ごとに全体へ送ります。アドレスを得たときに始め、失うと止めます。DTC の状態が変わったときは次の機会にすぐ送ります。ただし同じ DTC の変化による送信は 1 秒に 1 回までで、それ以内の変化は次の DM1 に載ります。この送信の後の周期は、変化の時刻から数えます（過ぎた期限で始めた `Timer` の中の変化のように、その時刻から 1 秒がもう過ぎているときは、ステップの時刻から数えます）。`BroadcastSuppression` が抑制している間は送らず（周期は進み、後から埋め合わせません）、9 バイト以上の DM1 は、同じノードの前の転送が終わるのを待つ BAM（`WhenBusy::Queue`）です。前の DM1 の BAM が終わるまで、次の DM1 の BAM は始めません。
- DM1 と DM2（以前に有効な DTC）の Request には、抑制中も答えます。8 バイト以下は全体へ 1 フレーム、9 バイト以上は宛先指定の Request なら要求元へ RTS/CTS、グローバルなら BAM です。全体へ送る答えは、同じ時刻に届いた複数の要求に 1 回だけ送り、同じ PGN の BAM が送信中なら新たに送りません。1 秒の制限と答えの重複の判定は事象の時刻で数えるので、結果はステップ幅によりません。
- DM1 と DM2 の BAM が送信中かどうかは、そのノードの転送の終わり（PGN と宛先グローバル）で数えます。同じノードからアプリが DM1 や DM2 を BAM で送ると、その数え方がずれます（診断を付けたノードでは、アプリはこの 2 つを送らない前提です）。
- データの形: ランプの状態とランプの点滅の 2 バイト（載せる DTC から合成。点滅は速い > 遅い > 点灯）、DTC ごとに 4 バイト（SPN、FMI、発生回数）。DTC がなければ 4 バイトの 0。8 バイト未満は 0xFF で埋めます。
- DM3 は以前に有効な DTC を消し、DM11 は有効な DTC を以前に有効にします。宛先指定の Request には消し終えたときに肯定の ACK（消すものがなくても）を返し、グローバルの Request には返しません。`DiagnosticsOptions::on_clear` で、アプリが消すか、断る（NACK）か、後で消すかを決められます。後で消すときは、宛先指定の要求を `deferred_capacity` まで取っておき（あふれた分には「応答できない」の ACK）、アプリが `complete_clear(kind, cleared)` を呼ぶと（`on_clear` の中からでもよい）、次の機会に肯定か否定の ACK を返します。その間にノードがアドレスを変えると、取っておいた要求には答えません。
- DM22 は自分宛てのもの（8 バイト以上）だけを処理し、DTC を 1 つ消して（有効なものは以前に有効へ、以前に有効なものは削除）要求元に DM22 で答えます。否定の理由は、DTC がない、または有効と以前に有効の取り違えです。同じステップの複数の DM22 には処理の順に答えます。

#### 識別情報（`Identification`）

- ソフトウェア ID（フィールド数と、`*` で終わる各フィールド。未設定は 8 バイトの 0xFF）、ECU ID（部品番号、製造番号、場所、種類、製造者）、製品 ID（コード、ブランド、モデル）、診断プロトコル ID（J1939-73 だけ）の Request に答えます。8 バイト未満は 0xFF で埋め、9 バイト以上は宛先指定の Request なら要求元へ RTS/CTS、グローバルなら BAM です。同じ要求元への 2 つ目の応答は、1 つ目の転送が終わってから始まります。

#### 周期送信（`PeriodicSender`）

- `node.attach<PeriodicSender>(pgn, period, data_or_encoder, options)`。データは、持たせたバイト列（`set_data()` で入れ替え）か、送るたびに呼ぶ `Encoder`（部品のバッファに書いて長さを返す。バッファは生成時に `max_length` バイト、既定 8）です。9 バイト以上はトランスポートプロトコルです。`PeriodicSenderOptions` で宛先（PDU1 なら 1 つのアドレス）、優先度、遅れた期限の扱い、DM13 に従うか、`max_length` を選びます。
- 送るのは `Node::can_send_now()` の間だけです（要求の後の待ちの間は送りません。エンジンが保留して 250 ms 後にまとめて出すのを避けるため）。送れるようになった通知の中ですぐ 1 回送り（そのステップに出ます）、そこから周期を数えます。生成時にすでに送れるノードは、次の機会に始めます。アドレスを失うか変えると止め、送れるようになったらまた始めます。
- 期限は `Timer` の期限（事象の時刻）で数えるので、ステップ幅によらず格子（始めた時刻 + k × 周期）からずれません。送るのは期限以後の最初のステップで、フレームの時刻はそのステップの時刻です。ステップ幅が周期以下なら、1 ステップに入る期限は高々 1 つなので、送信の回数と期限はステップ幅によりません（試験で確かめています）。この主張は、送れるようになった通知から始まった周期についてのものです。`trigger()`、`resume()`、途中での生成で始め直したときは、最初の発火でもう周期が過ぎていれば（アプリの処理やステップの外から始め直したとき、ステップ幅が周期以上のときに起こります）、そのステップから周期を数え直します（まとめも追いつきもしません）。そのため始め直しを含む送信の時刻と回数は、ステップ幅によることがあります。
- 遅れた期限（ステップ幅が周期より長く、1 ステップに期限が複数入るとき）: `MissedDeadlines::Coalesce`（既定）はまとめて 1 回送り（まとめた数は `statistics().coalesced`）、次の期限はそのステップより後の最初の格子点です。`CatchUp` は期限ごとに送り、同じ時刻のフレームが並びます（9 バイト以上は前の転送が終わるまで `Busy` で、失敗として数えます）。
- DM13: `suppressible` が true なら、`BroadcastSuppression::of(node).suspended()` の間は送りません（送れるようになったときの最初の送信も含みます。期限は進み、埋め合わせません。`statistics().suppressed`）。false なら `BroadcastSuppression` を使いません。指定しなければ（既定）、全体宛て（`kGlobalAddress`）の放送だけが従い、1 つのアドレス宛ての送信は従いません。宛先指定の周期送信を DM13 で止めるかは確かめていないので、利用者が選びます（下の「確かめていない点」）。
- `trigger()` は次の機会に送り、周期をそこから数え直します（ハンドラの中ならそのステップ、`Timer` のコールバックの中ならそのステップか次のステップ（発火中のタイマーと同じ期限で始めたタイマーは次の回に回るため）、アプリの処理とステップの外なら次のステップ。アプリの処理ですぐ送るなら `Node::send()` を使います）。`pause()` と `resume()` で止めて再開します。
- 送信の失敗（`SendStatus`）は数えて（`statistics().failures`、`last_failure`）、次の期限でまた送ります。例外にしません。`Encoder` の例外と、`max_length` を超える長さは `Network` を失敗させます。

#### Request の送信（`Requester`）

- `Requester::of(node).request(pgn, destination, callback)`。`Requester` はノードに 1 つで、ノードに付けて部品とアプリが共有します（相乗りと間隔の抑制はノード単位です）。`of()` はなければ既定の設定で付けます。設定を選ぶときは、何かが `of()` を使う前に `Requester::attach(node, options)` で付けます（2 つ目は `std::logic_error`。コンストラクタは非公開の鍵の型を取るので、それ以外の方法では作れません）。要求ごとに待ち時間を変えることはできません。
- `request()` はどこからでも呼べます（ステップの外、アドレスを得る前も）。Request は次の機会（`trigger()` と同じ）に、ノードが送れるようになってから出ます（`can_send_now()`。待ちの間は出しません）。待ち時間（`timeout`、既定 1250 ms）は Request を出したステップの時刻から数えます（`flush()` で出るため）。
- 宛先が 1 つのアドレス: そのアドレスからの要求した PG か、そのアドレスからの、このノードと PGN を名指しした Acknowledgement で終わります（`RequestOutcome::Response`、`Acknowledged`）。来なければ `retries`（既定 2）回まで送り直し、その後は `Timeout` です。送り直しは時間切れのステップで出ます。
- グローバル: `timeout` の間に来た応答をそのたびに報告し（`last == false`）、最後に `Timeout`（`last == true`）を報告します。送り直しません。
- 同じ PGN と宛先の要求が待っている間に来た要求は、その要求に相乗りします（Request を送らず、同じ答えを受けます。`statistics().joined`）。`min_interval`（既定 0）で、同じ PGN と宛先の Request の間隔の下限を持てます。待っている間（送る前、送り直す前）に来た答えでも終わります（周期的に放送されている PG を受けた場合も含みます。最新の値であることは同じです）。
- ノードがアドレスを失うか変えると、送った要求は送れるようになってから送り直します（答えは古いアドレスへ来るため。この送り直しは `retries` に数えません）。アドレスを要求できなくなる（`CannotClaim`）と、すべて `NotSent`（`NoAddress`）で終えます。送信の失敗も `NotSent` です。
- コールバックはステップの中（ハンドラかタイマー）で呼ばれ、`request()` と `cancel()` を呼べます。`cancel()` した要求のコールバックは呼びません。`RequestResult::data` は呼び出しの間だけ有効です。`capacity`（既定 8）の要求が待っていると、`request()` は空の `RequestId` を返します。

#### 受信の監視（`Monitor`）

- `node.attach<Monitor>(pgn, options)`。1 つの PG について、最新の受信（`data()`。`max_length`、既定 8 バイトまで）、時刻、送信元、受信数を持ちます。送信元は、どれでも、1 つのアドレス（`source`）、NAME（`name`。受けるたびにアドレス表と同じ `Network` のノードから今のアドレスを引くので、送信元が移っても追います）のどれかで絞ります。
- `accept`（任意）で、受け入れるメッセージを利用者の関数で絞れます（送信元の条件の後に呼びます）。セレクターで複数の型を載せた PGN の 1 つの型だけを見るときや、通し番号や検査値を確かめるときに使います。受け入れなかったメッセージは保持せず、受信として数えず（`Lost` を終えず、期限も延ばしません）、`rejected()` に数えます。確かめる前のデータが `data()` に出ることはありません。`accept` はステップの中で呼ばれ、その中で `Monitor` を破棄してはいけません。
- 状態は `Waiting`（まだ受けていない）、`Receiving`、`Lost`（最後の受信から `timeout` の間来ない）です。途絶の時間は最初の受信から数えます（起動の遅れを途絶にしません）。期限はメッセージの時刻から数え、期限ちょうどの受信は間に合ったとします。そのため結果はステップ幅によりません（試験で確かめています）。変化は `on_state`、受信は `on_message` で知らせます。
- 何も送りません。`set_observer` は使いません（ハンドラだけで作ります）。

#### Commanded Address の送信（`AddressCommander`）

- `commander.command(target_name, address, done, destination)`。Commanded Address（NAME と新しいアドレスの 9 バイト）を、グローバルなら BAM、1 つのアドレスなら RTS/CTS で送ります（ノードの前の転送が終わるまで待ちます）。`request()` と同じく、どこからでも呼べ、ノードが送れるようになってから出ます。
- 転送の終わり（`on_transfer_end`、`TransferEnd::id` で照合します）から待ちを数えます。完了しなかった転送（自分のアドレスの変化による取り消し、受信側の Abort、時間切れ）は 1 回だけ送り直し、2 回目も完了しなければ `NotSent`（`CommandResult::transfer` と `reason`）です。
- 相手がその NAME で新しいアドレスの Address Claimed を送ったら `Moved`、転送の完了から `timeout`（既定 1250 ms）までに来なければ `Timeout`、`send()` が受け付けなければ `NotSent`（`send_status`）です。任意アドレス不可の相手は従わないので `Timeout` になります。

#### 独自プロトコルのコマンドと応答（`Exchange`）

規格にない通信手順（Proprietary A/B、メーカー独自の手順）の要求側を書くための部品です。1 回の「コマンドを送り、応答を待つ」を扱い、手順（複数のコマンドの順序、セッション）は利用者の部品が持ちます。応答側には専用の部品がなく、`on_message` と `send` で書きます（[j1939-guide.md](j1939-guide.md)）。

- `Exchange(node, options)` か `node.attach<Exchange>(options)`。`start(pgn, command, peer, match, done, command_options)` はどこからでも呼べ、コマンド（`max_length`、既定 8 バイトまで。写して持ちます）を、ノードが送れるようになってから次の機会に送ります。ハンドラの中からならそのステップ、アプリの処理とステップの外からなら次のステップです。`capacity`（既定 4）のやり取りが動いていると空の `ExchangeId` を返します。
- 相手（`peer`）はアドレス（0〜253）か NAME です。PDU1 のコマンドは相手のアドレスへ、PDU2 のコマンドはグローバルへ送ります。NAME は送るたびに `Network::address_of()` で引きます。最初に送るときにまだどの制御機能もその NAME を要求していなければ（起動の直後）、その Address Claimed を `timeout` まで待ってから送り、来なければ `PeerLost` です。
- 応答の照合: 相手のアドレスからのメッセージ（このノード宛てかグローバル、PGN によらない）を、送った後のやり取りの `match` に、古いものから順に渡します。前のコマンド（送り直した元、`cancel()` したもの）への遅れた応答も渡るので、取り違えないかは `match` が決めます（`cancel()` は送ったコマンドを取り消しません）。PGN も確かめる型付きの読み出しは `codec::decode<M>(message)` です。`match` は `Ignore`、`Pending`（途中の応答。そのメッセージの時刻から `timeout` を数え直し、以後は送り直さない）、`Final`（応答。`Reply` で終わる）を返します。1 つのメッセージは、`Ignore` を返さなかった最も古いやり取りだけが使います（同じ相手への同じコマンドは、応答を順に受けます）。
- コマンドごとの設定（`CommandOptions`）: `priority`、`timeout`（空なら `ExchangeOptions::timeout`、既定 1250 ms）、`retries`（空なら `ExchangeOptions::retries`、既定 0）、`long_answer`（長い応答の PGN）、`on_address_change`（自分のアドレスが変わったときの方針）。
- 待ち時間の起点: コマンドを送ったステップの時刻です（送り直しも、その送り直しを送ったステップから）。9 バイト以上のコマンドはトランスポートプロトコルで送り（`WhenBusy::Queue`）、転送の完了（`TransferEnd::id` で照合）から数えます。`Pending` の応答を受けたら、その時刻から数え直します。
- 長い応答（`long_answer`）: 送った後、相手からその PGN の受信の転送が始まると（`Node::on_incoming_transfer`。このノードへの RTS/CTS か BAM）、待ちを止めます。その転送が終わると（完了、中断、時間切れ、BAM の欠落）、終わりの時刻から `timeout` を数え直します（完了なら組み立てたメッセージがすぐ後に `match` に渡ります）。ほかの PGN、ほかの送り手、ほかのノード宛ての転送では止めません。止めておけるのは、1 回の試行につき、最初にその転送が始まってから `ExchangeOptions::answer_transfer_limit`（既定 60 秒。規格の値ではありません）までです。それを過ぎると、転送の開始も終わりも待ちの期限を変えません（待ちは遅くとも上限の `timeout` 後に終わります）。指定しなければ、受信の転送があっても待ちは進みます（長い応答の時間を `timeout` に含めます）。
- 時間切れの送り直し（`retries`）: 時間切れの後に送り直し、送り直しは時間切れのステップで出ます。長いコマンドの転送の失敗（受信側の Abort、このノードの時間切れ）も 1 回の試行です。`Pending` の後は送り直しません。既定で送り直さないのは、コマンドが冪等とは限らないためです。
- 自分のアドレスが変わったとき（`on_address_change`）: 送った後にノードがアドレスを失うか変えると、応答は古いアドレスへ送られるので、このノードには届きません。既定の `AddressChangePolicy::Stop` では `Interrupted` で終えます。`SendAgain` なら、送れるようになってから新しいアドレスで送り直します（`retries` に数えず、`Pending` の後でも送り直します）。長いコマンドの転送が最後のパケットの後に取り消された（`Cancelled`）ときも同じ扱いです。最後のパケットの前に取り消された転送と、まだ送っていないコマンドは、相手に届いていないので、方針によらず待って新しいアドレスから送ります。相手のアドレスが取られたための取り消しなら、続く Address Claimed で `PeerLost` になります。

やり取りの結果（`ExchangeOutcome`）は次のとおりです。`NotSent` は「相手に届いていない」、`Interrupted` は「送ったが、相手が実行したかは分からない」で、非冪等な操作を手順の側で扱えるように分けています。最後のパケットまで出したかは `TransferEnd::all_packets_sent` で判断します。

| 結果 | なるとき |
|---|---|
| `Reply` | `match` が `Final` を返した |
| `Timeout` | 試行が尽きても応答がなかった（`Pending` の後は、最後の `Pending` から `timeout`） |
| `NotSent` | 一度も送らないうちに `send()` が受け付けなかった（その `SendStatus`）か、アドレスを要求できなくなった（`NoAddress`）。最後の試行の長いコマンドの転送が、最後のパケットの前に終わったか、受信側に中断された（`transfer`、`reason`） |
| `Interrupted` | 送った後に自分のアドレスが変わった（`Stop`）か、アドレスを要求できなくなった（`NoAddress`）。最後のパケットの後に転送が取り消された（`transfer` が `Cancelled`）か、このノードが時間切れで中断した（`Aborted`）。送り直しを `send()` が受け付けなかった（`QueueFull` など） |
| `PeerLost` | 相手のアドレスがなくなった（下の「相手の消失」） |

- 相手の消失（`PeerLost`）: 相手の NAME（指定した NAME か、最初に送ったときにそのアドレスにあった NAME）が、別のアドレスを要求する、Cannot Claim を送る、別の NAME にアドレスを取られる、のどれかを受けたときです。Address Claimed と Cannot Claim を受けるたびに `address_of()` で確かめ、送り直しが残っていても終えます。NAME の解決を待つ期限は、その間に自分のアドレスが変わっても延びません。NAME の分からないアドレス（NAME のない試験用のノードなど）が相手なら、時間切れだけで終わります。相手が同じ `Network` のノードで、別のノードにアドレスを取られたときは、相手の Cannot Claim か新しいアドレスの Address Claimed で分かります。
- コールバック（`done`）はやり取りごとに 1 回、ステップの中で呼ばれ、`cancel()` したものには呼ばれません。`ExchangeResult::reply` のデータは呼び出しの間だけ有効です。`match` と `done` の中から `start()`、`cancel()`、`cancel_all()` を呼べ、`Exchange` を破棄できます（その後は何も呼びません）。通知のループは開始時のやり取りだけを扱い、状態を確定してから呼びます。
- 破棄: 残りのやり取りはコールバックなしで終わります。`Network` より後に破棄してもかまいません。`Network` の破棄の後の `start()` は `std::logic_error` です。

#### 規格との一致を確かめていない点（ECU アプリ向けの部品）

- `Requester` の待ち時間と送り直しの回数の既定値（1250 ms、2 回）は、J1939-21 と J1939-82 の要求側の値に合わせています。確かめていないのは、Busy などの理由の ACK（`AckCode::CannotRespond` など）を受けたら送り直さずに終えること、グローバルの Request を送り直さないこと、相乗りと `min_interval` の扱い、まだ送っていない要求を同じ PG の受信で終えることです。
- DM13 が 1 つのアドレス宛ての周期送信も止めるか（`PeriodicSender` は既定では止めず、`suppressible` で選べます）。`AddressCommander` の待ち時間の既定値と、Address Claimed で移ったと判断すること。

#### 規格との一致を確かめていない点（診断）

- DM13: 6 秒ちょうどの扱い、6 秒を数え直す DM13 の範囲、Stop の後に DM13 が一度も来ない場合の扱い、Stop と Start が同じ DM13 で別々のネットワークに指定されたときの扱い。
- DM3/DM11 で後から返す ACK の期限（アプリが `complete_clear()` を呼ばなければ答えません）、DM22 の否定の理由のうち使っていないもの、グローバル宛ての DM22 を無視すること。
- DM2 のランプのバイトを以前に有効な DTC から作ること、発生回数の上限、識別情報が未設定のときの答え（ソフトウェア ID の 0xFF、空のフィールド）。

### 型付きメッセージ（`vehicle_sil::j1939_codec`）

PG の信号を YAML で定義し、ビルドのときに C++ の型を生成します。書式と API は [j1939-codec.md](j1939-codec.md) にあります。

- 責務: 型は信号の値とバイト列の変換だけを持ちます。送受信、周期、宛先、時間は持ちません。`codec.hpp` は `vehicle_sil/j1939/types.hpp`（`Pgn`、`Priority`）だけを include し、`Network` と `Node` を知りません。ノードと部品への接続は `codec_node.hpp` の関数（`attach_sender`、`encoder`、`on_message`、`send`）で、どれもノードの公開 API と `PeriodicSender` の上の薄い関数です。エンジンは型付きメッセージを知りません。
- 生成: `vehicle_sil_add_j1939_codec()`（`cmake/VehicleSilJ1939Codec.cmake`）がビルドのときに `tools/j1939_codegen/j1939_codegen.py` を呼び、生成物はコミットしません。生成器は Python 3.8 以上と PyYAML を使い、この関数を呼んだときだけ要ります（vehicle-sil の試験と例をビルドするときも要ります）。特別な値のプロファイルの選択と継承（ファイル、メッセージ、信号）は生成器が解決し、信号ごとの特別な値の配列だけを生成します。生成器は定義の誤り（重なり、範囲外、名前、知らないキー、有効範囲の内側の特別な値、規則のないプロファイルの直接の指定、ファイルやメッセージで選んだ自前のプロファイルがどの信号にも合わないこと、同じ PGN の型の区別）を生成の時点で止め、生成したヘッダーは同じ定義から同じ内容になります。生成するのは信号の配置の定数とメンバーだけで、ビット操作はランタイムのテンプレートの 1 か所にあります。
- 値: 信号は受けた raw のビットをそのまま持ち、状態（`Valid`、`Error`、`NotAvailable`、`Special`、`OutOfRange`、`Undefined`）と、`Valid` のときだけの物理値を返します。特別な値は Codec 全体では予約せず、信号の定義だけで決まります（定義のない信号ではすべてのビットパターンが値）。特別な値が数値に暗黙に変わることはなく、物理値を設定して特別な値になることもありません（有効範囲から外すため）。物理値と raw 値の変換は `double` の四則演算と `std::round` だけで、乱数や時計を使いません。raw × scale は `double` に丸めてから offset を足します（積を一度格納し、コンパイラーが積和を 1 回の丸めにまとめないようにしています。clang の最適化ビルドでも同じ値になることを試験で確かめています）。
- 境界の検査: CTest の `j1939_codec.independent_of_the_engine` が `codec.hpp` の include を `types.hpp` だけに限り、`j1939_codec.boundary` が `libs/j1939/codec` がエンジンの内部を使わないことを確かめます。
- 資源: 型は状態を持たない値で、`encode`、`decode`、`codec_node.hpp` のハンドラと `Encoder` は定常状態でメモリを確保しません（試験で確かめています）。`codec::encoder` は送るたびに新しいメッセージをスタックに作ります。
- 決定論と時間: 型付きメッセージは時間に関わりません。周期と送信の時刻は `PeriodicSender` の規則のとおりです。
- SAE J1939 の PG と SPN の定義データ（J1939DA）は同梱しません。例と試験は、独自（Proprietary）の PG と架空の配置だけを使います。
- 確かめていない点: 組み込みの `j1939` プロファイルの値と、規則を設けていない長さ（[j1939-codec.md](j1939-codec.md) の「規格との一致」）。利用者はファイルで `j1939` を定義し直せます。

### 観測（`Observer`）

`Network::set_observer(&observer)` で、ネットワークの出来事を読み取り専用で受け取ります（ヘッダー `vehicle_sil/j1939/observer.hpp`）。試験と記録のための口で、通信には加わりません。

- 出来事（`Event`、種類は `EventKind`）は 1 つの値の型です。欄の意味は種類ごとに決まっています（ヘッダーに記載）。`node` はローカルのノードの番号（`Network::node()`）で、なければ -1 です。`data` は呼び出しの間だけ有効です。

| 種類 | いつ | 時刻 |
|---|---|---|
| `AddressChanged` | ノードのアドレスか状態が変わったとき。そのノードの `on_address_change` のハンドラより先 | `AddressChange::time` |
| `SendRequested` | `send()`、`request()`、Request への応答（`respond()`。ノードがもう持たないアドレスへの要求への応答は `NoAddress`）が戻り値を返したとき。`held` は、Claim の後の待ちのために保留したこと。`acknowledge()` は含めません（ACK は `FrameSent` で見えます） | ステップの時刻 |
| `MessageDropped` | 送るはずのメッセージを送らなかったとき（`DropReason`）: 保留中のメッセージか ACK のノードがアドレスを失った、保留を出すときにデータキュー（ACK は制御キュー）が満杯だった、アドレスを失う前に送信キューに入れたフレーム | 原因の時刻（保留）かステップの時刻（送信キュー） |
| `FrameSent` | ポートにフレームを渡したとき（`send_result` はポートの戻り値。`QueueFull` で残したフレームは次の試みでも出ます） | ステップの時刻 |
| `MessageReceived` | 受けたメッセージ（単一フレームと、トランスポートプロトコルで組み立てたもの）をノードに配る直前。`local` は同じ `Network` の他のノードから（`node` が送ったノード）。TP.CM と TP.DT は出ません | メッセージの時刻 |
| `TransferStarted` | 転送の開始。送信側は `send()` か `respond()` が受け付けたとき（応答待ち行列や Claim の後の待ちに入るものを含む）、受信側は RTS か BAM を受け付けたとき（BAM は `node` が -1） | 送信側はステップの時刻、受信側は RTS か BAM の時刻 |
| `TransferEnded` | 転送の終わり（`result`、`reason`）。開始した転送は 1 回だけ終わります（`Network` が失敗するか破棄される前に限ります）。送信側の `source` は送った時のアドレスです（保留した転送が始まる前にノードが移ると、開始の `source` と違います） | 原因の時刻 |
| `PortEvent` | ポートの事象（`on_port_event()`） | 事象の時刻 |

- 受信側の転送の終わり: `Completed`（続いてそのメッセージの `MessageReceived`）、`AbortedByPeer`（送信側が Connection Abort を送った）、`Aborted`（このノードが送った。タイムアウト、再送の上限など）、`Lost`（BAM のパケットの欠落は `reason` が `BadSequence`、時間切れは `Timeout`。何も送りません）、`Cancelled`（送信側のアドレスを別の NAME が要求したか手放した、ローカルのノードがアドレスを失った、同じ送信者の新しい RTS か BAM が置き換えた、CTS を送信キューに入れられなかった、単一フレームの PGN（Address Claimed、Request）を運んでいた）。`TransferResult::Lost` は受信側にだけ使い、`on_transfer_end` には渡りません。
- 読み取り専用: `on_event()` の中から、その `Network` の送信（`send`、`request`、`respond`、`acknowledge`）、ハンドラの登録、ノードや部品の追加、`Timer` の生成と開始、ステップの関数、`set_observer()` を呼ぶと `std::logic_error` で、`Network` は失敗状態になります。ハンドラの解除と `Timer` の取り消し・破棄は、デストラクタから呼ばれるので例外を投げず、`Network` を失敗状態にします（解除は `false` を返します）。`on_event()` からの例外も `Network` を失敗させます。
- `on_event()` はステップの中で同期的に、上の表の時点で呼ばれます。Observer の有無で、送るフレームと時刻、ハンドラの呼ばれ方は変わりません（試験で確かめています）。未設定なら出来事ごとにポインタの比較 1 回で、エンジンはメモリを確保しません。
- 設定と解除はステップの外だけです（失敗状態の後はどこでも）。Observer は 1 つのネットワークだけを見ます。別のネットワークに設定すると前のネットワークから外れます（前のネットワークがステップの中なら `std::logic_error`）。Observer の破棄で自動で外れ、`Network` を先に破棄しても安全です。`on_event()` の中で Observer を破棄してはいけません。

### ステップ幅と資源の上限

- ステップ幅が 50 ms を超えると警告（`NetworkOptions::warning`、既定は標準エラー出力）、200 ms を超えると `start()` が `std::invalid_argument` を投げ、ホストが失敗状態になります（Request への応答とパケット間隔の 200 ms を守れないため）。推奨は 10 ms 以下です。`RealTimeDriver` で回すときは、その `max_idle` で同じ検査をします。
- 実時間では、タイマーの期限（`next_deadline()`）にドライバーが起きます。CTS で求められたパケットは CTS を受けたステップで出ますが、ポートの `QueueFull` で残したフレームと 1 ステップの上限で繰り越したフレームは、`max_idle` の後のステップで出ます。
- バッファはすべて生成時に確保します（`NetworkOptions`: 1 ステップの受信数、ノード数、データキュー、1 ステップの送信数、タイマー数、ノードごとの保留領域、トランスポートプロトコルのセッション（各 1785 バイトのバッファとタイマー 1 つ。タイマーは `timer_capacity` とは別に確保）と応答待ち行列）。`timer_capacity` は予約制です。NAME を持つノードの追加が 2 つ（Claim 後の待ちと Cannot Claim）、`Timer` の生成が 1 つを予約し、足りなければ `add_node()` が `std::invalid_argument`、`Timer` の生成が `std::length_error` を投げます。予約があるので、タイマーの開始が枠不足で失敗することはありません。フレームの受信と配送、Request への応答、送信（ポートの `QueueFull` と `Failed`、送信数の上限による繰り越しを含む）、ローカル折り返し、タイマー、Address Claim（任意アドレスの選択、アドレス表、アドレス違反、Commanded Address、状態の通知を含む）、トランスポートプロトコル（再送の要求、タイムアウト、Abort、応答待ち行列を含む）では、メモリを確保しません（テストで `operator new` を数えて確かめています）。道具箱と部品（`Timer` の開始と取り消し、DM1 の周期送信と BAM、DM13、DM1/DM2/識別情報の応答、DM22、後で返す ACK、`PeriodicSender`、`Requester` の応答と時間切れと送り直し、`Monitor`、`AddressCommander`、`Exchange` の応答と途中の応答と時間切れと送り直しと長いコマンド）と `Observer` への通知と、型付きメッセージの送受信（`codec::attach_sender`、`codec::on_message`）も、定常状態ではメモリを確保しません。メモリを確保するのは、ノードの追加、ハンドラと応答者の登録と解除、部品の生成、識別情報の設定のときと、`Requester::request()`、`AddressCommander::command()`、`Exchange::start()` がコールバックをコピーするとき（大きなキャプチャ）だけです。
- 上限に達したとき: 受信はそのステップの残りを捨てて数え、データキューは `send()` が `QueueFull` を返し、制御キュー（受信の上限、ノード数、保留領域の大きさ、セッション数、応答待ち行列の大きさから決めた容量。通常は満杯にならない）はあふれた分を捨てて数え、ノード数は `add_node()` が `std::length_error` を投げます。トランスポートプロトコルの上限は上の節のとおりです。
- 失敗: ハンドラ、応答者、タイマー、ポートからの例外と、使い方の誤り（その `Network` のハンドラ、応答者、タイマーからの `start`/`begin`/`receive`/`process`/`flush`/`add_node`、時刻が増えない、段階の順序の誤り）で、`Network` は失敗状態になります。以後のステップの呼び出しはすべて `std::logic_error` です。ハンドラが例外を握りつぶしても失敗状態のままです。

## 実行と試験の基盤

この節で扱うのは、ECU のアプリケーションの入口（`Ecu`）、J1939 と LIN のノードを動かす participant（`Peer`）、プロセス内の仮想バスでの実行（`LocalBench`）と試験の道具、それらを選んで動かす `Simulation` です。`Peer` と `LocalBench` は、どちらもエンジン（`vehicle_sil::j1939`）の公開 API だけを使います（CTest の `local.boundary`、`testkit.boundary`、`peer.boundary` が、内部ヘッダーや試験用の入口を使っていないことを確かめます）。区分（「構成」の表）は、`Ecu` と `Simulation` が利用者向け、`Peer` と `LocalBench` が土台、試験キットが試験用です。

### ECU の入口（`vehicle_sil::Ecu`）

ECU のアプリケーションを組み立てる入口です（`vehicle_sil::ecu`、ヘッダーのみ、SIL Kit なし）。`Peer`（SIL Kit の participant）と `LocalBench::add_ecu()`（仮想バスのホスト）が実装するので、`Ecu&` を受け取る関数やクラスは、どちらの上でも同じコードで動きます。

```cpp
class Engine {
public:
    explicit Engine(vehicle_sil::Ecu& ecu) {
        auto& node = ecu.j1939("J1939_CAN").add_node({.address = 0x00, .name = kName});
        codec::attach_sender<EngineStatus>(node, 20ms, [this](EngineStatus& m) { m.speed.set(rpm_); });
        ecu.every(10ms, [this](vehicle_sil::SimTime) { tick(); });
    }
};
```

- 組み立て: `j1939(network, NetworkOptions)`（J1939 の `Network`）、`can(network, RawChannelOptions)`（生の CAN）、`lin_master(network, schedule, LinMasterOptions)`、`lin_slave(network, LinSlaveOptions)`（LIN のノード）、`on_step(fn)`、`every(period, fn)`、`emplace<T>(args...)`。どれも実行の開始前にだけ呼べます。開始後に呼ぶと `std::logic_error` です。`j1939()` と `can()` は、ECU ごとにネットワーク名を 1 回だけ使えます（2 回目は `std::invalid_argument`）。`j1939()` と `can()` は同じ CAN のネットワーク名を使えますが、LIN のネットワーク名は CAN と共有できません。ECU は 1 つの LIN のネットワークに LIN のノードを 1 つだけ持ちます（どちらに反しても `std::invalid_argument`）。`emplace` は開始後に呼ぶと、`T` を作らずに `std::logic_error` を投げます。`j1939("")` と `can("")`（空のネットワーク名）は、どちらの実装でも `std::invalid_argument` です。`T` のコンストラクターが例外を投げたときは、その例外が伝わり、`T` は ECU に残りません。ただし、コンストラクターが ECU に足したもの（ネットワーク、ノード、ハンドラ、関数）は残ります。
- 1 ステップの順序: `j1939()` と `can()` で作ったもの（足した順）が受信して処理 → 変数への書き込みのうち時刻が来たものを反映（`Agent::on_step_begin()`）→ `on_step` と `every` の関数（足した順）→ LIN のノードの `step(now)`（足した順）→ 変数の読み出し（`Agent::on_step_end()`。`sample_period` ごと）→ `j1939()` と `can()` で作ったものが同じ順で送信。片方のハンドラから他方へ送ると、同じステップで出ます（ゲートウェイは、ネットワークを 2 つ持つ ECU です）。アプリがステップで出した LIN の要求（`set_frame_data()`、`set_response()`、`request_sleep()` など）はそのステップから効き、LIN のノードが受けたものはそのステップの LIN の処理で通知されるので、アプリは次のステップで読みます。`Peer`、`LocalBench`、`Ecu` の説明は同じ順序です。
- 所有と破棄: ECU が LIN のノード、ネットワーク（部品が先）、生チャネル、Control Agent、`emplace` したオブジェクトを持ち、返す参照は ECU の寿命まで有効です。破棄の順は、LIN のノード、ネットワーク、生チャネル、Agent、`emplace` したオブジェクト（最後に足したものから先）です。ハンドラ、部品、変数のポインターと関数は、`emplace` したオブジェクトを破棄の間も参照できます。
- 変数: `input(name, &value, options)`、`input(name, get, set, options)`、`input_enum(name, &value, items, options)`、`output(name, &value, options)`、`output(name, get, options)`、`output_enum(name, get, items, options)` で、外から読み書きする変数を宣言します。引数、型、検査は `control::Agent` の同名の関数と同じです。宣言は ECU の Agent（`agent()`）が持ちます。Agent は最初の宣言か `agent()` で作り、ECU に 1 つです。変数も `agent()` も使わない ECU には Agent がなく、ステップで何もしません。`set_agent_options(AgentOptions)` は Agent を作る前だけです（後は `std::logic_error`）。不正なオプション（`Agent::check_options()` が断るもの）は、`set_agent_options()` が ECU の名前つきの `std::invalid_argument` で断り、オプションは変わりません。宣言と `set_agent_options()` は実行の開始前だけで、開始後とスキーマの確定の後は `std::logic_error`、同じ ECU での同じ名前は `std::invalid_argument` です（文言に ECU の名前）。Peer の監視の出力との名前の衝突は、宣言のときではなく、スキーマを確定するときに `std::invalid_argument` になります。`agent()` は開始後も、ある Agent を返します。開始後に Agent がなければ `std::logic_error` です。実装は Agent を `detail::EcuAgent`（遅延の生成、オプション、ステップの前後の呼び出し、終わりの `finish()`、通信の側の残りの検査）で持ちます。
- 通信の側の寿命: 自分のスレッドで Agent を使う通信の側（`control_grpc::AgentClient`）は、生きている間 Agent に `control::TransportLease` を持ちます（`Agent::transports()` で数える。ほかの通信の側も同じ型を使えます）。通信の側は ECU（`Peer`、`Simulation`、`LocalBench`）より後に宣言して先に破棄し、ECU に `emplace()` しません（`emplace()` したオブジェクトは Agent より後に壊れるため）。ECU が Agent を破棄するときに `TransportLease` が残っていれば、ECU の名前と直し方を標準エラーに出して `std::terminate()` を呼びます（破棄した Agent を使わせないため。デストラクターなので例外にはしません）。
- 変数の時点: 書き込みは、`at` 以後の最初のステップで、アプリの処理の前に反映されます。そのステップのネットワークの受信処理と部品のタイマーは反映より前に走るので、新しい値を使うのは次のステップからです。読み出しは、アプリと LIN の処理の後です。`Peer` と `LocalBench` で同じ時点です。`Peer` は止まるとき（`stop()` と破棄）、ステップがもう走らなくなった後に、待っている書き込みを `Cancelled` にします（`Agent::finish()`。2 回目以降は何もしない）。その後で止めた通信の側は、その結果も送ります。`LocalBench` の ECU では、待っている書き込みの結果は届きません（通信の側は ECU より先に破棄するため）。利用者は `finish()` を呼びません。
- `every(period, fn)` は、0、period、2 period … の時刻以後の最初のステップで呼びます（1 ステップに 1 回まで。period がステップ幅の倍数でなくてもよい）。規則は `vehicle_sil::periodic()`（`core/periodic.hpp`）の 1 か所にあり、`Peer` と `LocalBench` の ECU が共用します。`TickScheduler` と違って、周期とステップ幅の関係を検査しません。
- 生の CAN（`can::RawChannel`）は、J1939 でないプロトコルの土台です。`on_frame(handler)` で受け、`send(frame)` で送ります。`send()` はステップの中でだけ呼べます（それ以外は `std::logic_error`）。結果は `RawSendStatus`（`Ok`、`QueueFull`、`InvalidFrame`）です。送ったフレームは、そのステップの最後に、送った順に、ステップの時刻で出ます。上限は 1 ステップあたり送信 64、受信 1024 です（`RawChannelOptions`）。超えた分は数えて捨てます。再試行はしません。自分が送ったフレームは受けません。同じ ECU が同じネットワーク名で `j1939()` と `can()` を持つと、別のコントローラーとして互いのフレームを受けます。受けるのは次のステップですが、時刻は送ったステップのものです。J1939 のネットワークは 11 ビットのフレームを無視します。ただし、`Peer` の J1939 のネットワークが利用者のポート（`j1939(name, port)`。Vector XL など）の上にあるときは、別のバスになります。生チャネルは常に SIL Kit の上にあるためです。違う送り手から同じ時刻に届いたフレームの順は、SIL Kit では他の participant のスレッド次第で、仮想バスでは `StepDriver` の順です。`RawChannel` は並べ替えません。
- 型の形: `j1939()`、`can()`、`lin_master()`、`lin_slave()` は非仮想の関数で、空の名前を検査し、実装は private の仮想関数（`add_j1939`、`add_can`、`add_lin_master`、`add_lin_slave`）で行います。`Peer` には、引数が `J1939ChannelOptions` の `j1939(name, ...)` が別にあります。この宣言が基底クラスの同じ名前の関数を隠すので、`Peer&` から呼べる `j1939` は `Peer` のものだけです。`NetworkOptions` を渡す `j1939(network, NetworkOptions)` は `Ecu&` を通して使います。
- LIN: `lin_master(network, schedule)` は、ECU のステップ幅で回る `lin::LinMaster` を、`lin_slave(network)` は `lin::LinSlave` を作って返します。ハンドラ（`on_result()`、`on_state()`、`on_frame()`）は実行の前に設定します。ノードは最初のステップの前に初期化されます（`init()`）。`Peer` では LIN のネットワーク名を名前にした SIL Kit の LIN コントローラと `SilKitLinPort`（仮想時間だけ。実時間では `std::logic_error`）、`LocalBench` ではその名前の `VirtualLinBus` のエンドポイント（1 つのバスに Master は 1 つ。2 つ目は両方の ECU の名前を言う `std::logic_error`）の上に作ります。スケジュールとオプションの誤りは、エンジンの検査による `std::invalid_argument` です。`vehicle_sil::lin` は SIL Kit を含まないので、`vehicle_sil::ecu` と `vehicle_sil::local` は SIL Kit なしのままです。
- スレッド: 組み立ては 1 つのスレッドから行います。ステップは実装のスレッドで走り、関数、ハンドラ、部品もそこで呼ばれます。

### `Simulation`（`vehicle_sil::Simulation`）

`Ecu` を、プロセス内の仮想バスか、SIL Kit の participant として仮想時間で動かし、失敗を例外で報告します（`vehicle_sil::simulation`。ヘッダーは SIL Kit のヘッダーを含みません）。動かす場所はコンストラクターで選び、ECU のコードは変わりません。`Simulation(std::optional<SilKitOptions>)` は、値があれば SIL Kit、空なら `Simulation()` と同じです。動かす場所を実行時に選ぶときに使います。`Simulation(SilKitOptions)` はいつも SIL Kit です。型名を書かない `Simulation sim({.registry_uri = uri})` も SIL Kit です。`Simulation sim{{}}` は、どちらのコンストラクターとも取れるのでコンパイルエラーになります（黙ってプロセス内にはなりません）。`Simulation` はコピーもムーブもできません。

| | `Simulation()` | `Simulation(SilKitOptions)` |
|---|---|---|
| ECU の実体 | `LocalBench::add_ecu()` のホスト。`run_for()` を呼んだスレッドで進む | `Peer`（Coordinated、仮想時間、ECU の名前の participant）。SIL Kit のスレッドで進む |
| `run_for(d)` | 何度でも呼べて、続きから進む | 1 回だけ。2 回目と、`controller = false` では `std::logic_error` |
| `run()` | `std::logic_error` | 誰が終わらせてもよい simulation の終わりまで待つ（1 回だけ） |
| 取り出す | `local()`: `LocalBench`。`ecu(name)`: `Ecu` | `peer(name)`: `Peer`、`registry_uri()`。`ecu(name)`: `Ecu` |

- `run_for(d)`（d > 0）は、終わりの時刻（仮想バスでは今の時刻から `d` 進めた時刻）より前に始まるステップだけを動かします。SIL Kit では、controller の停止が届くまで他の participant が stop_at 以降のステップを走らせうるので、各 ECU に同じ上限を置き、仮想バスと同じ記録になるようにしています。
- SIL Kit の `Simulation` が 1 回しか走らないのは、すべての participant を時刻の境界で止めても、その境界のステップが走っていることがあるためです。メインスレッドからモデルを読むと競合し、仮想バスとは境界のステップが 1 つずれます。止めながら確かめる試験は、仮想バスで行います。
- 失敗: ECU のステップの例外は `SimulationError` で（`local()` から足した ECU の失敗も同じです）、`what()` は失敗した ECU ごとの `<ECU 名>: <理由>` を `; ` で区切ったものです。仮想バスでは、失敗した後の `run_for()` も `SimulationError` です。SIL Kit は、participant が失敗しても simulation を終えず、仮想時間が止まるだけです。そのため `run_for()` と `run()` は、短い間隔で失敗を見張り、見つけたら simulation を終えて `SimulationError` を投げます。全体の時間切れはありません。始めるときの例外（必要な participant がそろわない、`start_timeout`）は、全部を止めてからそのまま伝えます。SIL Kit では、`run_for()` は全 participant が止まった後に戻ります。ステップが書いたものはそれから読めます。
- `run()` で `controller = true` のとき、Error で離脱した participant はすぐ失敗にします。Error 以外で離脱した participant が、数秒のうちに simulation を終えなければ、それも失敗です（`request_stop()` で終わる simulation を、誤って失敗にしないためです）。`controller = false` では、外の controller の仕事は見ず、このプロセスの ECU の失敗だけを見ます。
- `SilKitOptions`: `registry_uri`（空ならコンストラクターがプロセス内に registry を立てる）、`controller`（既定 `true`。participant 名 `SimulationController`）、`other_participants`（始める前にそろうべき他の Coordinated な participant。`controller` が `true` のときだけ。空、重複、`SimulationController` は `std::invalid_argument`）、`start_timeout`（既定 30 秒）。`add_ecu()` は、空の名前、使用済みの名前、controller と `other_participants` の名前を `std::invalid_argument` にします。`SimulationController` という名前は、`controller = false` でも、仮想バスでも拒否します。ECU が 1 つもないまま `run_for()` と `run()` を呼ぶと、どちらの実装でも `std::logic_error` です。
- `add<App>(name, step, args...)` は、`add_ecu()` と、`App{ecu, args...}` を ECU に持たせる `emplace` です。`App` のコンストラクターが例外を投げると、その例外が伝わり、その `Simulation` は使えなくなります。コンストラクターが ECU に足したものが、破棄された `App` を参照しうるためです。以後の `add_ecu()`、`add()`、`run_for()`、`run()` は、ECU の名前を添えた `std::logic_error` です。
- ネットワークの名前: 1 つの名前は、`Simulation` のすべての ECU にとって、CAN（`j1939()`、`can()`）か LIN（`lin_master()`、`lin_slave()`）のどちらか一方のネットワークです。ほかの ECU が別の種類で使った名前は、どちらの実装でも `std::invalid_argument` です（文言に両方の種類を言います。SIL Kit では、使っている ECU の名前も言います）。SIL Kit では、確かめるのはこのプロセスの ECU の間だけです。ほかのプロセスの participant の名前は見えません。
- 所有と破棄: `Simulation` が ECU とその持ち物を持ち、返す参照は `Simulation` の寿命まで有効です。破棄は、後から足した ECU から先に ECU を壊し、次に controller、最後にプロセス内の registry です。デストラクターは全部を止め、例外を投げません。
- スレッド: `Simulation` の関数は 1 つのスレッドから呼びます。SIL Kit では、ステップは SIL Kit のスレッドで走ります（`Peer` と同じ）。

### Peer（`vehicle_sil::Peer`）

1 つの SIL Kit participant と、その中で動くもの（CAN の上の J1939 の `Network` と生の CAN、LIN の Master と Slave、ステップごとのアプリの処理）をまとめて作り、動かし、正しい順序で片付けます。`Ecu` を実装するので、`Ecu&` を受け取る ECU のコードを、そのまま載せられます。

```cpp
vehicle_sil::Peer peer({.participant = "Engine", .registry_uri = uri,
                        .lifecycle = vehicle_sil::LifecycleMode::Coordinated,
                        .virtual_time = vehicle_sil::VirtualTimeOptions{.step = 10ms}});
auto& model = peer.emplace<EngineModel>();             // Peer が持つ。ハンドラが参照してよい（Peer より先に宣言してもよい）
auto& ecu = peer.j1939("J1939_CAN1").add_node({.address = 0x00, .name = name});
ecu.on_message(kPgnTsc1, [&](const j1939::Message& m) { model.set_request(m.data); });
peer.every(100ms, [&](SimTime) { static_cast<void>(ecu.send(kPgnEec1, model.encode())); });
peer.start();
peer.wait();
```

- `j1939(network)` は SIL Kit の CAN コントローラ、`SilKitCanPort`、`TapPort`、`Network` を作ります（`tap(name)` で生フレームを観測できます。`network_names()` は追加した順の名前の一覧）。`j1939(name, port)` は利用者が作ったポート（Vector XL の `ExternalPortAdapter` など）を受け取り、Peer が所有します。`Ecu::lin_master(network, schedule, options)` と `Ecu::lin_slave(network, options)` は、LIN のネットワーク名を名前にした SIL Kit の LIN コントローラ、`SilKitLinPort`、LIN のノードを作り、communication ready の通知で `init()` を呼びます。SIL Kit のコントローラは作ると外せないので、名前、スケジュール、オプションを先に検査します（エンジンの検査は `LinMaster::validate()` と `LinSlave::validate()`。コンストラクターと同じ検査です）。例外を投げた `lin_master()` と `lin_slave()` は何も残さず、同じ名前で呼び直せます。`on_step(fn)` は毎ステップ、`every(period, fn)` は 0、period、2 period … の時刻以後の最初のステップで呼びます（1 ステップに 1 回まで。`vehicle_sil::periodic()` の規則）。`can(network)` の生の CAN は、SIL Kit の CAN コントローラ（名前は `<network>.raw`）を 1 つ作ります。`network()`、`network_names()`、`tap()`、`silkit_port()` は J1939 だけです。生の CAN のチャネルは `raw_channel(network)` で引きます。生の CAN のコントローラ名 `<network>.raw` が、同じ Peer の他のコントローラ名と重なると `std::invalid_argument` です。J1939 のコントローラ名どうしの重なりと、CAN のコントローラ名と LIN のコントローラ名（LIN のネットワーク名）の重なりも拒否します。
- 1 ステップの順序: J1939 の各 `Network` と生の CAN の受信と処理（追加した順）→ Control Agent の書き込みの反映 → アプリの処理（`on_step` と `every`、追加した順）→ LIN のノードの `step(now)`（追加した順）→ 監視の記録と Agent の読み出し → J1939 と生の CAN の送信。Agent の前後の処理は、`on_step` や変数をいつ足したかによりません。アプリが出した LIN の要求（`set_frame_data()`、`set_response()`）は同じステップから有効になり、LIN のノードが受けたものはそのステップの LIN の処理で通知されるので、アプリは次のステップで読みます（`Ecu` の順序と同じです）。
- 時間のモード: `virtual_time` があれば SIL Kit の時間同期で回します（J1939 と LIN）。なければ実時間で、Peer が `RealTimeDriver` と時計（`SteadyRealTimeClock`、または `PeerOptions::real_time_clock`）を持ち、SIL Kit の CAN ポートを実時間にします（J1939 だけ。LIN は `std::logic_error`）。外部のポートの時間の領域が Peer と違えば `std::logic_error` です。SIL Kit を持たない構成（XL だけ）はありません。
- lifecycle: `Coordinated` では `SimulationController` が開始と停止を決めます。1 つのプロセスで複数の Peer と controller を起動するときは `start_together(controller, {&a, &b})` を使います。`Autonomous` では、`stop()` か `request_stop()`（ステップの中から）で止まります。lifecycle が Running になる前（`start()` の前や、ほかの participant を待っている間）の `request_stop()` は保っておき、仮想時間では最初のステップで、実時間では Running になったときに渡します（SIL Kit は Running の前の停止を無視するため）。仮想時間で最初のステップまで待つのは、最初のステップには時刻で同期するすべての participant が時刻を始めている必要があるからです。まだ Running でない participant を残して止めると、その participant が止まれなくなることがあります（controller が仮想時間を使わない構成では、controller はこの同期に入らないので保証しません）。仮想時間では Running になった後でも最初のステップの前の要求は最初のステップまで待ち（ふつうはすぐ来る）、Error のときはすぐ渡します。`stop()` は最初のステップを待たずに渡します。Coordinated では、ほかの participant の `start()` が、自分の最初のステップの前にシミュレーションの終わりを見て、開始の失敗として例外を投げることがあります。実時間で `start()` が時間切れで投げた後に Running になったときは、保った要求は渡らないので `stop()` で止めます。複数の participant を同じ時刻から決定的に動かすときは `Coordinated` にします（`Autonomous` の participant どうしは、開始の時刻がそろうとは限りません）。仮想時間で `virtual_time.end` があれば（0 より後であること）、end の前の最後のステップの後で自分で止まります。`wait()` は lifecycle の終わりを待ちます。`failed()` と `error()` は、ステップ（ホスト、アプリ、LIN、ポート）、ステップのハンドラ、実時間のドライバーの失敗を返します。仮想時間でステップが失敗すると participant は Error になり、`wait()` が戻ります。実時間ではドライバーが止まるだけで lifecycle は続き、`wait()` はその失敗では戻らないので、`failed()` を見て `stop()` します。
- 複数の ECU の組み立ては、`Ecu&` を受け取る関数やクラスに書くと、同じコードを `Peer` と `LocalBench` の両方で使えます（例: `examples/j1939/ecu_pair`。試験で両者の結果が同じことを確かめています）。`Simulation` は、これを 1 行で選べるようにしたものです。
- 組み立て（`j1939`、`can`、`lin_*`、`on_step`、`every`、`emplace`）は `start()` の前だけです（後は `std::logic_error`）。ノード、ハンドラ、部品、Observer、`TapPort` のリスナーも `start()` の前に付けます（ノードは `Network::add_node()` の規則どおりステップの中からも足せます）。
- 所有と破棄: Peer が participant、コントローラ、ポート、`Network`、LIN のノード、Control Agent を持ち、返す参照は Peer の寿命まで有効です。`stop()`（と破棄）は、Agent が実時間のドライバーを起こすのをやめさせ、ステップを止め（実時間ではドライバー、続いて lifecycle。仮想時間では lifecycle。以後ステップは来ません）、SIL Kit のポートを閉じ、Agent の待っている書き込みを `Cancelled` にします。`stop()` の後も `Network` と Agent は残るので、統計、記録、最後の値を読めます。破棄では続いて LIN のノード（それぞれ、その `SilKitLinPort` より先）、`Network`（部品が先）、`TapPort`、ポート（外部のポートはそのデストラクタで閉じます）、生の CAN とそのポート、ホスト、participant、Agent を破棄し、最後に `emplace` したオブジェクト（最後に足したものから先）を破棄します。`emplace` したオブジェクトは participant より後に壊れるので、そのデストラクターは SIL Kit のサービスを使ってはいけません。
- スレッド: 組み立て、`start()`、`wait()` は 1 つのスレッドから呼びます。ステップは SIL Kit のスレッド（仮想時間）かドライバーのスレッド（実時間）で動き、ハンドラ、アプリの処理、Observer、`TapPort` のリスナーもそこで呼ばれます（記録は `wait()` か `stop()` の後に読みます）。`stop()` は、ステップと SIL Kit のハンドラ以外のどのスレッドからでも呼べます（別のスレッドからは `start()` が戻った後）。同時の呼び出しは順に処理します（Peer の破棄は、他のスレッドの `stop()` が戻った後）。`request_stop()`、`failed()`、`error()` はステップの中を含めてどこからでも呼べます。

### ローカルの実行（`vehicle_sil::LocalBench`）と試験キット（`vehicle_sil::testkit`）

SIL Kit を使わずに、仮想バスと `StepDriver` の上で ECU を動かし、J1939 の試験を組み立てます。何も時計を読まず、呼んだスレッドで進みます。`LocalBench`（`vehicle_sil::local`、ヘッダーは `vehicle_sil/local/local_bench.hpp`）が仮想バスと ECU を持ち、`vehicle_sil::testkit` は試験の道具（記録と故障注入）を足します。`testkit::LocalCan` は `LocalBench` の別名です。

```cpp
LocalBench bus;                                                  // 仮想バス + StepDriver
install_engine(bus.add_ecu("engine", 10ms), model);              // Ecu&（自分のホストを持つ ECU。ネットワーク J1939_CAN）
bus.add_ecu("rival", 5ms).j1939("J1939_CAN");                    // 同じ名前のネットワークは同じバス
testkit::J1939Events events(bus.network("engine"));              // Observer
testkit::FrameLog raw(bus.tap("engine"));                        // TapListener
bus.endpoint("rival").set_fault_injector(
    testkit::faults::nth(1, can::FaultAction::drop(), testkit::faults::j1939_pgn(kPgnAddressClaimed)));
bus.run_until(SimTime{} + 1s);
CHECK(events.has(j1939::EventKind::AddressChanged));
```

- `LocalBench`: `VirtualCanBus`（`VirtualCanBusOptions` でタイミングも選べる）と `lin::VirtualLinBus` をネットワーク名ごとに持ち、その上の ECU を持ちます。`add_ecu(name, step)` は `Ecu&` を返します。ECU ごとに自分のホストとステップ幅を持ちます（SIL Kit で ECU ごとに participant を分けたときと同じ構成です）。`Ecu::j1939(network)` は、名前 `network` のバス（なければコンストラクターのオプションで作る）に端点を足し、`TapPort` で包んで `Network` を作ります。同じ名前を使う ECU は同じバスを共有し、ECU は複数のバスに `Network` を持てます（ゲートウェイ）。`Ecu::can(network)` の生チャネルも、そのバスに自分の端点を持ちます。`add_j1939(name, step, options)` は、J1939 のネットワークを 1 つ持つ ECU を作る略記です。
- バスの名前を省く形: `add_j1939()`、`inject(time, frame)`、`injector()`、`bus()` は、ただ 1 つのバスを使います。バスがまだなければ、名前の決まっていないバスを作ります（名前の検索では `""`）。バスに名前を付けるのは ECU だけです。このバスは、次に来た `Ecu::j1939(network)` か `Ecu::can(network)` の名前になり、新しいバスは作りません。名前つきのバスが 1 つあるときに名前を省く形が来ても、そのバスを使います。どちらの順で書いても同じバスになります。2 つ目の名前が来ると 2 つ目のバスを作りますが、それまでに `add_j1939()` で ECU を足していたら `std::logic_error` です（その ECU がどのバスのつもりか決まらないためです。文言にその ECU の名前と、今あるバスの名前が入ります。その ECU を `add_ecu(name, step).j1939(network)` で足し直します）。バスが 2 つ以上あるときに名前を省く形を使うと、`std::logic_error` です（文言にバスの名前の一覧が入ります）。例外を投げた `add_j1939()`、`j1939()`、`can()` は、バスを作らず、名前も付けません。生チャネルには名前が要ります（`Ecu::can("")` は `std::invalid_argument`）。
- LIN: `Ecu::lin_master(network)` と `Ecu::lin_slave(network)` は、名前 `network` の LIN バス（ECU が最初にその名前を言ったときに作る）に、ECU の名前のエンドポイントを付けてノードを作り、すぐ初期化します。同じ名前の CAN バスと LIN バスは作れません（どちらの順でも、両方の種類を言う `std::invalid_argument`）。名前のないバスの仕組み（上の項）は CAN だけのもので、LIN は関わりません。1 つの LIN バスに Master は 1 つです（2 つ目は両方の ECU の名前を言う `std::logic_error`）。例外を投げた `lin_master()` と `lin_slave()` は、エンドポイントを付けず、バスも作りません。`lin_bus(network)`（`VirtualLinBus&`。Observer はここで足す。知らない名前は LIN バスの一覧を言う `std::out_of_range`）、`lin_master(ecu, network)`、`lin_slave(ecu, network)` で引きます。LIN のノードのステップは、ECU のアプリの処理の後です。
- 参照: ECU の名前とネットワークの名前で、`network(ecu, network)`、`endpoint()`（故障注入と通信断）、`tap()`、`host(ecu)`、`ecu(name)` を引きます。ネットワークが 1 つの ECU には、ECU の名前だけの形も使えます（0 個や 2 個以上は `std::logic_error`）。生チャネルはこれらの対象に入らず、`raw_channel(ecu, network)` で引きます。`bus(network)`、`injector(network)`、`inject(network, time, frame)` は名前でバスを引くだけで、バスを作らず、名前も付けません。その名前のバスがなければ `std::out_of_range` です（文言に、今あるバスの名前か、直し方が入ります。名前の決まっていないバスなら、名前を省く形を使うか、ECU の `j1939(network)` か `can(network)` で先に名前を付けます）。名前のないバスを `bus("")` で引くこともできます。ECU の名前の一覧は `ecu_names()` で取れます。`inject()` は、どのネットワークも使わない専用の端点から生フレーム（11 ビットか 29 ビット）を送ります。時刻は `now()`（最後の `run_until()` の end）より前にできません。`on_step(name, fn)` は ECU のアプリの処理を足します。
- ステップ: `Ecu::on_step` と `every` は同じ列に、足した順に並びます。`every` は `vehicle_sil::periodic()` を通します。組み立て（`add_ecu`、`add_j1939`、ECU の `j1939()`、`can()`、`lin_master()`、`lin_slave()`、`input()`、`output()`、`set_agent_options()`、`on_step()`、`every()`、`emplace()`）は最初の実行の前だけです（後は `std::logic_error`）。ホストは最初の実行で、足した順にドライバーへ登録されます。
- 変数: ECU の Control Agent は、`Peer` と同じ時点（アプリの処理の前に書き込み、LIN の処理の後に読み出し）で、呼んだスレッドのステップの中で動きます。実時間のドライバーを起こす処理はありません（仮想時間で、呼んだスレッドが進めるため）。監視の出力（`PeerOptions::monitor`）はありません。待っている書き込みの結果は届きません（通信の側は ECU より先に破棄するため）。ローカルの Agent は、`submit()` と `latest()` で直接か、`control_grpc::AgentClient` で既存の Control Server につないで読み書きします。時刻の制御（一時停止、ステップ実行）と、Control Server を起動する口はありません。
- 破棄の順は、後に足した ECU から順に、ホスト、LIN のノード（後に足したものから）、`Network`（後に足したものから。各 `Network` の後にその `TapPort`）、生チャネル、Control Agent、`emplace` したオブジェクト（最後に足したものから先）です。その後にドライバー、最後に LIN バスと CAN バスと端点を破棄します。
- `J1939Events`（Observer）と `FrameLog`（TapListener）は、出来事と生フレームをコピーして持ちます。容量（既定 100000 件）を超えた分は持たずに数えます（`dropped()`）。問い合わせは種類（向き）と述語で数えたり取り出したりします（`count`、`of`、`first`、`last`）。
- `faults`: 仮想バスの `FaultInjector` を作る小さな関数です。`when(pred, action)`、`between(from, until, action)`、`nth(n, action)`、`sequence({...})`、`first_of({...})` と、述語 `j1939_pgn(pgn)`、`j1939_source(sa)`、`can_id(id, format)` です。どれも、状態は選んだフレームの数だけなので決定的です。`nth` と `sequence` のコピーは数を共有するので、端点ごとに作ります。故障注入の仕組みそのものは仮想バスにあり（「仮想バス」の節）、試験キットは新しい注入の仕組みを持ちません。
- LIN の試験は、仮想 LIN バスの上（`LocalBench`）でも、`Peer` と in-process の registry の上でも行えます。結果は同じです（「LIN」の節の決定性）。LIN の故障注入と記録の道具はありません。

## Vector XL CAN Bridge

```
SIL Kit CAN ネットワーク ── ICanController ── CanBridge ── ExternalCanPort ── XlCanPort ── XL Driver ── Virtual CAN / CANcase
                                 （participant は ParticipantRuntime が所有）
```

| クラス | 責務 |
|---|---|
| `vehicle_sil::vector_xl::CanBridge` | SIL Kit の `ICanController` と `ExternalCanPort` の間で、クラシック CAN を双方向に転送する。プラットフォームに依存しない |
| `vehicle_sil::vector_xl::ExternalCanPort` | 外部 CAN のインターフェース。本番の実装は `XlCanPort`、テストではフェイク |
| `vehicle_sil::vector_xl::XlDriver` | `xlOpenDriver` / `xlCloseDriver` を RAII で管理し、`xlGetDriverConfig` からチャネルを列挙する |
| `vehicle_sil::vector_xl::XlCanPort` | XL のチャネル 1 つを開き（`xlOpenPort`）、送信（`xlCanTransmit`）と受信スレッド（`xlReceive` と通知イベント）を担う |
| `vehicle_sil::vector_xl::ExternalPortAdapter` | `ExternalCanPort` を CAN の共通基盤のポート（`RealTime`）にする。プロトコルスタックを XL のチャネルに直接載せるときに使う。プラットフォームに依存しない |

### プロトコルスタックから使う（`ExternalPortAdapter`）

- `ExternalPortAdapter(external, clock)` は外部ポートを開始し（バスに参加）、破棄か `close()` で止めます。`RealTimeDriver` と同じ時計を渡し、そのドライバーで回します。時刻同期のホストには登録できません（構成エラー）。
- 受信は外部ポートの受信スレッドで到着時刻を付けて受け箱に積みます（ハードウェアのタイムスタンプは別の時計なので使いません）。リモートフレームは捨てて数えます。自分の送信のレシートは捨てます（同じスタックのノードへの配送はスタックが行います）。エラーフレームは `ErrorFrame`、外部の受信キューのあふれは数の分からない `ReceiveOverflow` の事象です。バス状態は外部ポートが知らせないので出しません。
- 送信は `transmit()` の同期の結果をそのまま返します（`Ok` は `Accepted`）。その後バスに出たかは知らせません（XL の送信レシートは init 権限の有無で変わるため）。
- 実機での確認はしていません（「検証の範囲」）。

### 転送の契約

- 識別子（11 bit / 29 bit）、IDE、RTR、DLC、データを変えずに転送します。ただし SIL Kit から XL へは、DLC 9〜15（クラシック CAN ではどれもデータ 8 バイト）を DLC 8 として送ります。`xlCanTransmit` が受け付ける DLC は 0〜8 のためです（XL Driver Library のマニュアル）。XL から SIL Kit へは、受け取った DLC のまま転送します。
- CAN FD、CAN XL、不正な ID や DLC のフレームは転送せず、`silkit_rejected` または `xl_rejected` に数えます。XL 側の ID は受信イベントから 11/29 ビットで取り出すので、`xl_rejected` に数えるのは実際には DLC が 15 を超えるフレームです。
- XL のエラーフレームは転送せず、`error_frames` に数えます。XL の受信キューのあふれは `overruns` に数えます。
- 統計値は終了時に 1 行で出力します。対象は次のとおりです: `silkit_to_xl`、`xl_to_silkit`、`own_tx_dropped`、`error_frames`、`overruns`、`silkit_rejected`、`xl_rejected`、`xl_queue_full`、`xl_tx_failures`、`silkit_tx_failures`。

### 転送ループの防止

1. SIL Kit 側の受信ハンドラは、RX 方向だけで登録します。ブリッジが SIL Kit に送ったフレームは、ブリッジ自身には届きません。
2. XL 側は、init 権限があるとき（`--bitrate` を指定したとき）だけ、`xlCanSetChannelMode(tx=0, txrq=0)` で送信レシートを無効にします。この関数は init 権限を必要とします（`vxlapi.h`）。init 権限がなければ、ドライバの既定（送信レシートあり）のままです。
3. どちらの場合も、`XL_CAN_MSG_FLAG_TX_COMPLETED` または `XL_CAN_MSG_FLAG_TX_REQUEST` が付いた受信イベントは、自分の送信として捨て、`own_tx_dropped` に数えます。init 権限がなければ、送ったフレームと同じ数だけ数えられます。

同じ SIL Kit ネットワークと同じ XL のバスを、2 つのブリッジでつなぐ構成ではループを防げません。この構成には対応しません。

### キューとスレッド

- ブリッジは独自の転送キューを持ちません。
  - SIL Kit から XL へは、SIL Kit の受信スレッドで `xlCanTransmit` を呼びます。XL の送信キューが満杯ならフレームを捨て、`xl_queue_full` に数えます。
  - XL から SIL Kit へは、`XlCanPort` の受信スレッドで `ICanController::SendFrame` を呼びます。
- XL の受信キューの容量は `--rx-queue-size` で決まります（2 のべき乗、16〜32768、既定 16384）。ドライバが実際に保持できるのは、その値から 1 を引いたイベント数です。
- `xlReceive` は 1 回に 1 イベントずつ呼びます。Vector のサンプルも 1 件ずつ受け取っており、複数件を一度に受け取る使い方は確かめていないためです。
- チャネルは `XL_ACTIVATE_NONE` で有効にします。ブリッジはタイムスタンプを使わず、`XL_ACTIVATE_RESET_CLOCK` は Vector の新しい時刻同期プロトコルと併用できないためです。
- 1 つのポートへの XL の呼び出しは、mutex で直列化します。
- **起動順**: `CanBridge::start()` は、participant の lifecycle を開始する（`ParticipantRuntime::start()`）前に呼びます。ほかの参加者は、ブリッジが Running になったのを見た時点で送信を始めます。その前にハンドラとコントローラが動いていないと、最初のフレームが失われます。
- 終了は次の順で行います。
  1. XL の受信を止める（受信スレッドを join し、チャネルを非アクティブにする）。
  2. SIL Kit のハンドラを解除し、コントローラを止める。
  3. lifecycle を止める。
  4. ポートを閉じる。
  5. ドライバを閉じる。

### 接続先とチャネルの選択（CLI）

```
vehicle_sil_vector_xl_bridge --list-channels
vehicle_sil_vector_xl_bridge --registry-uri <uri> --xl-target <virtual|physical> --xl-channel <n> [--bitrate <bit/s>] [--network <name>]
```

- `--xl-target virtual` は Vector Virtual CAN（`XL_HWTYPE_VIRTUAL`）を選びます。`physical` は、それ以外のハードウェアの CAN チャネル（CANcase、VN16xx など）を選びます。
- `--xl-channel` は、そのターゲットの CAN 対応チャネルの中での 1 始まりの番号です。並びはドライバの設定順で、`--list-channels` の表示と同じです。CAN に対応していないチャネル（LIN 専用など）は数えません。
- 存在しない番号を指定すると、使えるチャネルの一覧を出して終了コード 3 で終わります。
- `--bitrate` は 15000〜1000000 bit/s です（`xlCanSetChannelBitrate` の範囲）。指定するときは、init 権限が必要です。ほかのアプリケーションが init 権限を持っている場合は、黙って別のビットレートで動かずに、起動を失敗させます。指定しなければ、チャネルの設定を変えません。
- 終了コードは次のとおりです: 0（Ctrl+C / SIGTERM での正常終了）、1（実行中に SIL Kit の participant が Running でなくなった。registry の停止や接続断）、3（XL ドライバ、チャネル選択、SIL Kit の失敗）、4（引数の誤り）。XL 側の受信エラー（ハードウェアを抜いたなど）では終了せず、`last_error` に残ります。

### ビルド

- XL バックエンド（`vehicle_sil::vector_xl`）と実行ファイルは Windows 専用です。Vector の XL Driver Library（`vxlapi.h`、`vxlapi64.lib`）が必要です。
- XL Driver Library は vehicle-sil に含めません。`VEHICLE_SIL_VECTOR_XL_DIR` で場所を指定します。
- 見つからない場合は、ブリッジをビルドせずに続けます。`VEHICLE_SIL_REQUIRE_VECTOR_XL=ON` を指定すると、その場合に構成を失敗させます。
- Linux のビルドに XL Driver Library は不要です。

### 検証の範囲

- XL バックエンドは 2 つに分かれます。判断と状態を持つロジック（`xl_driver.cpp`）は、vehicle-sil 独自の内部インターフェース（`src/xl_backend.hpp`）だけに依存し、どのプラットフォームでもビルドします。XL Driver Library を呼ぶのは `xl_backend_vector.cpp` だけで、このファイルが行うのは型と値の変換だけです（Windows で XL Driver Library があるときだけビルドします）。
- 自動テストは、内部インターフェースを実装したテスト専用のフェイク（`tests/vector_xl/fake_xl`）に対して、本番と同じ `xl_driver.cpp` と `main.cpp` を動かします。フェイクは、チャネルごとのバス、送信レシート、init 権限、受信キューのあふれ、エラーフレームを模擬します（送信レシートの設定とビットレートは init 権限が必要、送信の DLC は 0〜8、受信キューは指定した容量から 1 を引いた数、受信は 1 回に 1 件）。
- `xl_backend_vector.cpp` の変換（関数、定数、構造体のメンバーの対応）は自動テストの対象外です。公式の XL Driver Library でのビルドとリンクで確かめます。
- 次の点は、Vector のドライバとハードウェアでまだ確かめていません。
  - 実際のドライバの挙動（Virtual CAN と CANcase での送受信、送信レシートの無効化）
  - Virtual CAN の Channel 1 と 2 が同じ仮想バスにつながっているか（フェイクはチャネルごとに別のバスとして扱います）
  - 同じチャネルを使うほかのアプリケーション（CANoe など）のフレームが、ブリッジに通常の受信として届くか。フェイクはそう扱いますが、実際のドライバが送信レシートとして届ける場合、ブリッジはそれを `own_tx_dropped` として捨てます。確かめるまでは、ほかのアプリケーションとは別のチャネルを使います。
  - CAN FD モードで動いているチャネルを、クラシック CAN のポート（`XL_INTERFACE_VERSION`）で開いたときの挙動
  - Windows では E2E テストがブリッジを正常に止められないので、統計（`own_tx_dropped` などのループ防止の確認）と終了コードを検査しません
  - `ExternalPortAdapter` で XL のチャネルにプロトコルスタックを載せた動作（自動テストは `ExternalCanPort` のフェイクに対してだけです）

## 共通仮想時間

SIL Kit の `ITimeSyncService` で、すべてのピアが 1 つの仮想時間を共有します。ピアごとにステップ幅を決められます（例: 物理モデル 1 ms、LIN Slave 2 ms、LIN Master 5 ms、J1939 ECU 10 ms）。ピアはそれぞれ独立したプロセスで、同じ PC でも別の PC でも動きます。

### 時間の型

- シミュレーション時間は `vehicle_sil::SimTime`（`std::chrono::time_point<SimClock, nanoseconds>`）と `SimDuration` で表します。
- `SimClock` は壁時計ではありません。`now()` を持たず、`steady_clock` や `system_clock` の時刻とは型が違うので、混ぜるとコンパイルエラーになります。
- `SimTime{}` はシミュレーションの開始時刻（T = 0）です。

### 参加と進み方

- `ParticipantRuntimeOptions::virtual_time` を指定すると、participant は時刻同期に参加します。ハンドラは非同期版（`SetSimulationStepHandlerAsync`）で、利用者のハンドラが戻るとすぐに `CompleteSimulationStep()` を呼びます。利用者から見ると、ハンドラが戻ったときにステップが終わる点はブロッキング版と同じです。
  - SIL Kit はステップハンドラと受信ハンドラを同じ I/O スレッドで呼ぶので、ステップの実行中、その participant のほかのハンドラは呼ばれません。
  - `CompleteSimulationStep()` は完了の処理を I/O スレッドのタスクとして積みます。そのため、ステップで送ったメッセージだけでなく、SIL Kit がそれを自分に配送するときに送るメッセージ（LIN の Master が送るフレーム）も、次のステップの時刻を通知する前に送られます。ブロッキング版では、この 2 段目の送信が通知より後になり、受信側のステップに間に合う保証がありません（[dependencies.md](dependencies.md)）。
- 利用者は `set_step_handler()` でステップごとの処理を渡します。`start()` より前に渡さないと、`start()` が `std::logic_error` を投げます。
- 開始と停止は `LifecycleMode::Coordinated` でそろえます。`SimulationController`（または `vehicle_sil_sim_controller`）が、必要な参加者がそろってから workflow configuration を配ります。その結果、全員が T = 0 から同時に始まります。
- SIL Kit は、participant がステップ T を実行するのを、ほかの全員が T より前のステップを終えるまで待たせます。全体の進み方は最も遅いピアで決まります。
  - ステップハンドラが壁時計で長くかかると（重い `tick()` など）、ほかのピアはそれを待ちます。ステップは飛ばしません。
  - そのため、実行速度に関係なく、各ピアのステップの並びと受け取るデータは同じになります。
- `SimulationController` の `stop_at` を指定すると、controller は stop_at だけのステップで時刻同期に参加し、時刻 stop_at で全体を停止します。このとき、ほかの全ピアは stop_at より前のステップをすべて終えています。stop_at と同時刻のステップを実行するかどうかは決まっていません。
  - そのため、各ピアの作業範囲は `VirtualTimeOptions::end` で決めます。ステップハンドラは end より前に始まるステップでだけ呼ばれます。end 以降のステップも時刻同期と実時間との結合には参加しますが、ハンドラは呼ばれず `steps_completed()` にも数えません。end ≤ stop_at にすれば、最後に処理するステップは停止のタイミングに左右されません。end 以降はハンドラが受信バッファ（`StepInbox`、`SilKitCanPort` の受け箱）を空にしないので、ほかのピアが送り続けるとあふれます。あふれの判定は、処理したステップの範囲で行います。
  - `vehicle_sil_sim_controller` の `--timeout-ms` は、起動待ちの上限です。`--stop-at-ms` を指定したときは、起動後の実行にも同じ値を上限として別に適用します。実行中に必要な参加者が Error で抜けると（ステップの失敗、接続断）すぐに、ほかの状態で抜けて 5 秒たってもシミュレーションが終わらなければ、controller はシミュレーションを中断して終了コード 1 で終わります（参加者が自分で止めたシミュレーションを、誤って失敗にしないための猶予です）。この判定は `vehicle_sil::Simulation` と `vehicle_sil_control_server` と共通の `vehicle_sil::FailureCheck`（`vehicle_sil/core/failure_check.hpp`）で、ログに参加者の名前、状態、理由を出します。SIL Kit は失敗した参加者がいても全体を止めず、仮想時間が進まなくなるだけだからです。`--stop-at-ms` がなければ、シミュレーションは Ctrl+C / SIGTERM まで続きます。起動待ちの間の Ctrl+C は、プロセスをそのまま終了させます。
- ステップハンドラが例外を投げると、`ParticipantRuntime` はそれを捕まえて SIL Kit にエラーを報告します。その participant は Error になり、`step_error()` がメッセージを返します。`wait_until_finished()` は Error も終了として返します。SIL Kit は Error の participant を停止するまでそのままにするので、`stop()` かデストラクタで Shutdown にします。
- 例のピア（`examples/virtual_time`）は、シミュレーションが `--end-ms` より前の最後のステップに届かずに終わった場合（中断）、Shutdown でも終了コード 1 で終わります。
- `stop()` が戻った時点で、ステップハンドラは実行中でなく、以後も呼ばれません。実行中のハンドラがあれば終わるまで待ちます。ハンドラが使うオブジェクトは、`stop()`（またはデストラクタ）のあとに破棄します。
- ステップハンドラの中からは `stop()` ではなく `request_stop()` を呼びます。coordinated では、どの participant が停止しても全体が止まります。

### 実時間との結合

- 何も指定しなければ、仮想時間はできるだけ速く進みます（実時間より速くなりえます）。
- `VirtualTimeOptions::real_time_factor` を指定すると、そのピアは各ステップを壁時計に合わせて保持します。1.0 で実時間、4.0 で 4 倍速、0.5 で半分の速さです。
  - 最初のステップで壁時計と仮想時間を対応づけ、ステップ [T, T+Δ) は「対応づけた時刻 + (T + Δ − 開始時刻) / factor」まで終わりません（`RealTimePacer`）。
  - 1 つのピアを結合すれば、ほかのピアもそれを待つので全体が同じ速さになります。
  - 待つことしかしないので、目標より遅いピアを速くはできません。
- SIL Kit の `AnimationFactor`（experimental）は使いません。

### 一時停止、ステップ実行、速度（時間ゲート）

`SimulationControllerOptions::time_control` を指定すると、`SimulationController` は `TimeControlOptions::step`（既定 10 ms）のステップで時刻同期に参加します。そのステップの完了を保留して、シミュレーション全体を止めます（時間ゲート）。

- 止まる位置: controller がステップ P を保留すると、時刻同期に参加しているほかの全ピアは「開始時刻が P 以下のステップ」をすべて実行し、P より後に始まるステップを実行しません。ステップ幅が違っても同じです（P = 50 ms なら、1 ms のピアは 50 ms、3 ms のピアは 48 ms のステップまで）。これを「P で止まっている」と呼びます。止まる位置は controller のステップの倍数です。
- 操作（スレッド安全で、待たない）:
  - `pause()`: ペース調整で保留しているステップがあればそこで、なければ次のステップで止める。
  - `pause_at(T)`: T 以上で最初の controller のステップで止める（T を過ぎていれば次のステップ）。
  - `resume()`: 再開する（止める予定も消す）。
  - `run_until(T)`: T 以上で最初の controller のステップまで進めて止める。すでに T 以降で止まっていれば何もしない。`step(d)` は `run_until(time_state().now + d)`。
  - `set_speed(f)`: 仮想時間の秒 / 壁時計の秒（空ならできるだけ速く）。controller が各ステップの完了を壁時計の期限まで遅らせる。規則は `RealTimePacer` と同じ。速度を変えたときは待ち終えた分を保ち、再開のときは壁時計との対応づけをやり直す。いちばん遅いピアより速くはできない。
  - `time_state()` で現在の位置（止まっているか、P、止める予定、速度）を読み、`wait_until_paused(timeout)` で止まるのを待つ（ライフサイクルが終わったら空を返す）。
  - `TimeControlOptions::start_paused` で、T = 0 で止めて始められる（全ピアは時刻 0 のステップだけを実行して止まる）。
- 保留はステップハンドラの外で行います（`ParticipantRuntime::set_step_hold()` と `release_step()`）。controller の SIL Kit のスレッドは空いたままなので、止まっている間も状態の通知と停止は届きます。SIL Kit の HealthCheck は保留しているステップを時間切れにしません。止まっている間の `stop()` と `abort()` は、そのまま全体を終わらせます。
- 決定論性: 時間ゲートは止める位置と速さを決めるだけです。途中で止めた実行、1 ステップずつ進めた実行、速度を変えた実行のどれでも、各ピアが実行するステップと受け取るデータは、止めずに走った実行と同じです。
- `stop_at` と併用すると、controller は stop_at 以上で最初の自分のステップで停止します。`wall_time_to_stop_at()` には、止まっていた時間と速度の調整で待った時間も入ります。
- `set_speed()` で同じ値を設定し直しても、何も変わりません。ペース調整で保留しているステップの速度を変えると、待ち終えた分はそのままで、残りを新しい速度で待ちます。ステップの間で変えると、次のステップをその時点から新しい速度で待ちます。止まっている状態から再開すると、そこから壁時計との対応づけをやり直します。
- 制限:
  - 止まるのは時刻同期に参加しているピアだけです。仮想時間を使わないピア（実時間のピア、Vector XL のブリッジなど）は走り続けます。
  - controller のステップを小さくすると、ステップごとの同期の往復が増え、全体が遅くなります。
  - ピア自身の `real_time_factor` と `set_speed()` は両方かかります（遅いほうが効く）。速度を controller で決めるときは、ピアの `real_time_factor` を使いません。
  - 巻き戻しと、終わったシミュレーションのやり直し（リセット）はできません。やり直すにはプロセスを起動し直します。
  - 速度の調整は controller のステップごとです。ピアの (P, P + controller のステップ] のステップは、controller が保留を解いてからまとめて進みます。
  - `wait_until_paused()` は、同じプロセスのピアのステップの中から呼びません（そのステップが終わらないと止まる位置に届かないので、必ず時間切れになります）。ほかの操作はステップの中からも呼べます。
  - time_control があるとき、controller の `runtime()` の `set_step_hold()` と `release_step()` は時間ゲートが使っているので、呼びません。
  - `ParticipantRuntime::set_step_hold()` の保留の判断は、`stop()` が戻った後と、停止の要求の後には呼ばれません。判断の中から `release_step()` と `stop()` は呼びません（`request_stop()` は呼べます）。`VirtualTimeOptions::end` 以降のステップでは呼ばれます。

### 受信の扱い（決定論性）

SIL Kit が保証するのは、ステップ T の時点で T より前のタイムスタンプのメッセージがすべて届いていることだけです。タイムスタンプがちょうど T のメッセージがステップ T の前に届くかどうかは決まっていません。そこで、次のようにしています。

- ステップの外で届いたメッセージは `StepInbox` にためます。ステップ T では、タイムスタンプが T より前のものだけを、(タイムスタンプ, タイブレーク, 到着順) の順に渡します。T ちょうどのものは次のステップで渡します。
- 別のピアがステップ T で送ったデータは、受信側では T より後の最初のステップで処理されます。たとえば 1 ms の物理モデルが T で出した値を、10 ms の ECU はステップ T+1 ms 以降（実際には次の 10 ms 境界）で受け取ります。
- `StepInbox` の容量は固定です。満杯になると捨てて数えます。
- CAN のフレームも同じ規則です。`SilKitCanPort` は、タイムスタンプが T より前のフレームだけをステップ T で渡します。同じ時刻に別の送信者が送ったフレームの到着順は SIL Kit では決まらないので、J1939 の `Network` は、送信者ごとの列を CAN の調停の順に取り出して並べます（「1 ステップの処理」）。結果はステップ幅と到着順によりません。

### J1939 を仮想時間で動かす

- J1939 の `Network` は SIL Kit の `SilKitCanPort` の上で、ホストのステップとして動きます。`Peer` を使うか、`bind_host(runtime, host)` でホストを `ParticipantRuntime` のステップハンドラに結び付けます。スタックはスレッドを作らず、時計を読みません。タイマー（Address Claim の後の 250 ms など）は仮想時間で進みます。
- 1 ステップの中では、受信と処理、アプリの処理、送信の順です（「1 ステップの処理」、`Peer` は「Peer」）。アプリがステップの中で送ったメッセージは、そのステップの終わりに、ステップの時刻で送られます。送信を起点とするタイマー（T3 など）も、この時刻から数えます。

### LIN を仮想時間で動かす（`vehicle_sil::lin`）

`vehicle_sil::lin` は `vehicle_sil::sim_base` だけに依存し、SIL Kit にも J1939 にも依存しません（CTest の `lin.boundary` が、ソースとヘッダーが SIL Kit を含まないことを確かめます）。エンジン（`LinMaster`、`LinSlave`）は、ポート（`LinPort`）だけを通してバスに触れます。ポートの実装は 2 つです。プロセス内の `VirtualLinBus`（同じライブラリ）と、SIL Kit の LIN コントローラ（trivial simulation）の上の `SilKitLinPort`（`vehicle_sil::lin_silkit`）です。`Ecu::lin_master()` と `Ecu::lin_slave()` は、実行する場所に合ったポートの上にノードを作ります（`LocalBench` は仮想バス、`Peer` は SIL Kit）。ファイルは役割ごとに分けています。

| ヘッダー | 内容 |
|---|---|
| `lin_types.hpp` | 共通の型（`LinId`、`LinChecksumModel`、`LinFrame`、`LinRole`、`LinResultStatus`、`LinPublisher`、`LinNodeState`、`LinReceivedFrame`） |
| `lin_schedule.hpp` | スケジュールテーブル（`LinScheduleEntry`、`LinScheduleTable`、`validate_schedule()`） |
| `lin_port.hpp` | `LinPort`、`LinPortListener` |
| `lin_master.hpp` | `LinMaster` |
| `lin_slave.hpp` | `LinSlave` |
| `virtual_lin_bus.hpp` | `VirtualLinBus`、`LinBusEvent` |
| `lin_silkit/silkit_lin_port.hpp`（`vehicle_sil::lin_silkit`） | `SilKitLinPort` |

#### 共通の規則

- Master も Slave も、`step(now)` をステップごとに 1 回、そのステップの時刻で呼びます。バスへの送信（ヘッダー、Master のフレーム、go-to-sleep コマンド、wakeup パルス）は、すべて `step()` の中で行います。要求（`set_frame_data()`、`send_master_request()`、`set_schedule()`、`request_sleep()`、`request_wakeup()`）は、同じステップで `step(now)` より前に呼べば、そのステップで反映されます。
- ステップ T で送ったものは、ほかのノードの T より後のステップまでに必ず届きます。仮想バスでは、送った呼び出しの中で届きます。SIL Kit では、Master が送るフレームは SIL Kit が 1 段遅れて送りますが、`ParticipantRuntime` がステップの完了をその後に回すので、同じ保証が成り立ちます（「参加と進み方」）。受信側は、受け取ったイベントを `StepInbox` にためて、`step(now)` で now より前のものだけを時刻順に処理します。そのため、状態の変化とフレームの受信は壁時計にも、同じ時刻のステップの順にもよりません。
- 作り方: `LinMaster(port, schedule, step, options)`（`step` はノードを回すステップ幅）、`LinSlave(port, options)`。ポートはノードより長く生きなければなりません。ノードはコンストラクタからデストラクタまでポートのリスナーです。ハンドラ（`LinMaster::on_result()`、`on_state()`、`LinSlave::on_frame()`、`on_state()`）は、最初のステップの前に設定します（スレッドセーフではありません）。空の関数は「なし」です。`Ecu` を通すと、ポートとステップ幅は実装が決めます。
- 状態の変化は `StateHandler(time, state)` に、時刻付きで通知します。Slave は受け取ったイベントの時刻（自分の操作ならそのステップの時刻）、Master は状態が変わったステップの時刻です。
- `init()`、`step()`、`request_*()` はノードのステップから呼びます。ポートはスレッドセーフではありませんが、SIL Kit はステップハンドラと受信ハンドラを同じ I/O スレッドで呼び、仮想バスは送った側のステップの中で配るので、ポートの呼び出しは 1 つのスレッドに収まります。`LinSlave::set_response()`、`clear_response()`、`set_slave_response()` は、どのスレッドから呼んでもかまいません。
- 両方の `init()` は、最初のステップの前に 1 回呼びます（`Ecu` を通すと実装が呼びます）。SIL Kit のポートの上で自分で組み立てるときは、`ParticipantRuntime::set_communication_ready_handler()` から呼びます。購読が確立する前に初期化すると、ほかの参加者がその設定を受け取れないためです。
- 例外の文言は `vehicle_sil::lin::LinMaster: `、`vehicle_sil::lin::LinSlave: `、`vehicle_sil::lin: `（スケジュール）で始まり、値と直し方を含みます。

#### スケジュールテーブル（`LinMaster`）

- エントリは ID、データ長、チェックサム、スロット長、publisher（`Slave` か `Master`）を持ちます。テーブルは巡回します。最初のヘッダーは T = 0 で送ります。
- `validate_schedule()` の条件: 空でない、ID は 0〜63、データ長は 1〜8、スロット長は Master のステップ幅の正の倍数、同じ ID のエントリはデータ長・チェックサム・publisher が同じ、診断フレーム（0x3C は Master、0x3D は Slave が送る）は 8 バイトで Classic。満たさなければ `std::invalid_argument` です。チェックサムモデル（`LinChecksumModel`）は Classic と Enhanced だけで、受信側はヘッダーの設定と比べます。
- **ID の設定は変えられません**: `LinMaster` に渡したすべてのテーブル（コンストラクタと `set_schedule()`）を通して、1 つの ID のデータ長・チェックサム・publisher は同じでなければなりません。違えば `set_schedule()` が `std::invalid_argument` を投げます。SIL Kit が、一度設定した ID の設定の変更を受け付けないためです。結果がポートによらないよう、仮想バスでも同じ規則です。
- ヘッダーの結果（Ok、NoResponse、Error とデータ）は、そのスロットの終わり、つまり次のヘッダーを送るステップで返します。結果には、ヘッダーを送った時刻と publisher が付きます。
- `set_schedule()` は、次のスロット境界でテーブルを切り替え、新しいテーブルの先頭から始めます。休止中に呼ぶと、起床後の再開のときに切り替えます。

#### フレームの送受信

- **Slave が送るフレーム**: `LinSlave` は、ステップで `set_response(now, id, data)` された値のうち、ヘッダーの時刻より厳密に前に設定された最新の値で応答します。
  - Slave はヘッダーをポートから受け（`LinPortListener::on_header()`）、その中で応答します（`LinPort::respond()`）。SIL Kit のポートは dynamic response（`InitDynamic`、`AddFrameHeaderHandler`、`SendDynamicResponse`。SIL Kit v5.0.8 では experimental）を使います。
  - 静的な TX バッファでは決定論的になりません。ヘッダーの時刻と Slave のステップが同じ時刻のとき、どちらが先に処理されるかが決まっていないためです（SIL Kit でも、仮想バスのステップの順でも）。
- **Master が送るフレーム**: `LinMaster::set_frame_data(id, data)` の最新の値を、ヘッダーを送るステップで送ります（SIL Kit の `MasterResponse`）。最初の設定までは 0 を送ります。`set_frame_data()` に渡せるのは、これまでに渡したテーブルで Master が送る ID だけで、データ長も一致しなければなりません。
- `LinSlave::clear_response(now, id)` を呼ぶと、now より後のヘッダーに応答しなくなります。
- 1 つのヘッダーに 2 つ以上のノードが応答すると（2 つの Slave、または Master が送るフレームに応答する Slave）、実際のバスと同じく衝突として Error を返し、データは付けません。応答はどれもスロットの終わりまでに届くので、結果は到着順によりません。
- 1 つの ID に、時間を分けて別々の Slave が応答するのはかまいません（`clear_response()` で応答をやめ、別の Slave が `set_response()` する。診断の 0x3D）。SIL Kit v5.0.8 では、2 つ目の Slave が応答したあと、その ID のヘッダーが送られなくなっていました。パッチで修正しています（[dependencies.md](dependencies.md)、「SIL Kit へのパッチ」）。
- **Slave の受信**: `LinSlaveOptions::subscriptions` に挙げた ID のうち、Master が送るフレームを、`step()` から `FrameHandler` に渡します。時刻はヘッダーの時刻です。
- **制約**（SIL Kit）: 応答する Slave のステップ幅は、そのフレームのスロット長より厳密に小さくします。
  - Slave はヘッダーを受信ハンドラの中で応答するので、応答はステップに結びつきません。
  - Slave のステップ幅 D がスロット長 S より小さければ、ヘッダー T への応答は Master のステップ T + S より前に必ず届きます。D ≥ S では保証されず、NoResponse になるか、同じ ID の後のヘッダーへの応答として扱われることがあります。
  - 仮想バスでは、応答はヘッダーを送った呼び出しの中で届くので、この制約はありません。両方で同じ結果にするには、SIL Kit の制約を守ります。
- **制限**: Slave が、ほかの Slave の応答を受信すること（slave-to-slave）には対応していません。仮想バスは、Slave の応答をほかの Slave に配りません。SIL Kit では、応答のタイムスタンプは応答した Slave の `Now()` で、ヘッダーの時刻ではありません。また、到着の上限は応答した Slave のステップ幅で決まり、受信側からはわかりません。SIL Kit のポートは、Master のフレームとほかの Slave の応答を区別できないので、購読した ID をほかの Slave が送ると、そのフレームも渡しますが、時刻と渡すステップは決定論的ではありません。購読には Master が送る ID だけを入れます。Master が送るフレームに Slave が応答した衝突（設定の誤り）では、仮想バスはどの Slave にもフレームを配りませんが、SIL Kit では購読した Slave に Master のフレームと応答の両方が渡ることがあります。Master の結果（Error）はどちらも同じです。

#### 診断フレーム（0x3C、0x3D）

LIN 2.x の診断用のフレームを、通常のフレームとは別の規則で扱います。LIN の診断（ISO 17987-2 のトランスポート層とサービス）そのものは実装しません。アプリケーションが、フレームの中身（NAD、PCI、SID とデータ）を組み立てて解釈します。

- **マスターリクエスト（0x3C）**: `LinMaster::send_master_request(data)` で 1 つ積み、テーブルの次の 0x3C のスロットで 1 回だけ送ります。積んだリクエストがない 0x3C のスロットでは、ヘッダーを送らず、結果も返しません。
  - 前のリクエストを送り終えるまでは、次のリクエストを積めません（`false` を返します）。`master_request_pending()` で確かめられます。休止中や 0x3C のないテーブルの間は、積んだまま待ちます。送る前のリクエストは `cancel_master_request()` で取り消せます（診断のタイムアウトなど）。
  - データは 8 バイトです。先頭のバイト（NAD）が 0 のものは go-to-sleep コマンドなので受け付けません（`request_sleep()` を使います）。0x3C に `set_frame_data()` は使えません。
- **スレーブレスポンス（0x3D）**: `LinSlave::set_slave_response(now, data)` で設定した 8 バイトで、now より後の最初の 0x3D のヘッダーに 1 回だけ応答します。そのあとは、次に設定するまで応答しません。0x3D のヘッダーは毎回送られ、応答する Slave がいなければ NoResponse です。
  - 呼ぶたびに、1 回だけ送る新しい応答になります。now より後のヘッダーに対しては、前に設定した応答を（送ったかどうかによらず）置き換えます。送る前の応答は `clear_response(now, 0x3D)` で取り消せます。
  - 0x3C と 0x3D には `set_response()` を使えません。
  - 全 Slave が 0x3D を共有します。どの Slave が応答するかは、アプリケーションが NAD で決めます。2 つの Slave が同じヘッダーに応答すると Error です。
- Slave は、0x3C を購読（`subscriptions`）すると、マスターリクエストを `FrameHandler` で受け取ります。go-to-sleep コマンドは渡しません。

#### 休止と起床（Sleep/Wakeup）

- `LinMaster::request_sleep()`: 次のスロット境界で、次のヘッダーの代わりに go-to-sleep コマンド（ID 0x3C、`00 FF FF FF FF FF FF FF`）を送り、休止します。そこで終わったスロットの結果は通常どおり返します。
- `LinMaster::request_wakeup()`: 休止中なら、次の `step()` で wakeup パルスを送ります。
- `LinSlave::request_wakeup()`: 休止中なら、次の `step()` で wakeup パルスを送ります。Master は、時刻 T のパルスを T より後の最初のステップで受けて起床します。
- 同じ時刻の go-to-sleep コマンドと wakeup パルスでは、go-to-sleep が優先します。Master はその時刻のパルスでは起床せず、Slave はパルスのあとにコマンドを処理します。
- 起床した Master は、スケジュールをテーブルの先頭から、パルスの時刻から `LinMasterOptions::wakeup_delay`（既定 100 ms。LIN 2.x で Slave が準備を終えるまでの時間）以上あとの最初のステップで再開します。
- Slave は、go-to-sleep コマンドを受けるか、バスが `LinSlaveOptions::bus_idle_timeout`（既定 4 s。`std::nullopt` で無効）のあいだ無通信だと休止します。wakeup パルスかヘッダーを受けると起床します。
- 休止中の Slave も、ヘッダーには応答します。ブレークフィールドは物理的にも wakeup 信号になるためです。また、休止中は応答しないという規則にすると、ほかの Slave のパルスとヘッダーのどちらが先に届くかで結果が変わってしまいます。
- 状態に合わない要求（起きているときの wakeup、休止中の sleep）は捨てます。休止中に sleep と wakeup を同じステップで要求すると、起床だけが行われます。起床してからスケジュールを再開するまでの間に sleep を要求すると、再開のスロット境界で、ヘッダーを送らずに go-to-sleep コマンドを送ります。
- SIL Kit のポートでは、Master のコントローラは go-to-sleep で Sleep になり、起床で Operational に戻ります（`send_wakeup()` は `Wakeup`、ほかのノードのパルスで起きたときの `resume()` は `WakeupInternal`）。
- Slave の休止の状態は `LinSlave` の中だけにあり、ポート（SIL Kit のコントローラ）は常に Operational のままです。Sleep のコントローラは go-to-sleep フレームも含めてフレームを捨てるので、コマンドを受け取ったかどうかが到着順で決まってしまうためです。SIL Kit の Slave のポートの `send_wakeup()` は、パルスを送る直前にだけコントローラを Sleep にします（`Wakeup()` が Operational のコントローラに警告を出すため）。

#### ポート（`LinPort`）と仮想 LIN バス（`VirtualLinBus`）

ポートは、ノードがバスに触れる唯一の口です。役割（Master か Slave）はポートを作るときに決まり、ほかの役割の関数は `std::logic_error` です。

- Master の関数: `send_header(now, id, length, checksum)`（Slave が応答するフレームのヘッダー）、`send_frame(now, frame)`（ヘッダーと Master 自身の応答）、`send_go_to_sleep(now)`。Slave の関数: `respond(header_time, frame)`（`on_header()` の中からだけ）。両方: `send_wakeup(now)`、`resume()`（ほかのノードのパルスで起きた。バスには何も出さない）、`open()`（`init()` が呼ぶ）、`set_listener()`。
- リスナー（`LinPortListener`、エンジンが実装）への通知:
  - `on_header(time, id)`: Slave のポートだけ。Master が送ったすべてのヘッダー（Master が自分で応答するフレームも）。
  - `on_response(header_time, frame, status)`、Master のポート: 自分のヘッダーの結果。自分のフレームが出た（Ok）、Slave の応答（Ok とデータ）、応答なし（NoResponse。`send_header()` の中から同期で来ることがある）、長さかチェックサムモデルの不一致（Error）。1 つのヘッダーに 2 回以上来ることがあり（複数の応答）、衝突の判定は `LinMaster` がします。
  - `on_response()`、Slave のポート: Master が応答したフレーム（Ok）。go-to-sleep コマンドと自分の応答は来ません。ほかの Slave の応答は、仮想バスは配らず、SIL Kit のポートは区別できずに渡します（上の「制限」）。
  - `on_go_to_sleep(time)`: Slave のポートだけ。`on_wakeup(time)`: ほかのノードのパルスだけ（自分のは来ません）。
- 引数: `send_header()` と `send_frame()` は、ID が 63 を超えるか長さが 1〜8 の外なら `std::invalid_argument` です（両方の実装。`LinMaster` はそうした値を送りません）。
- 時刻: `now` は呼び出したステップの時刻です。SIL Kit のポートは使わず、SIL Kit が participant の時刻を付けます。
- スレッド: SIL Kit のポートは、リスナーを SIL Kit の I/O スレッドから呼びます。仮想バスは、送った側の呼び出しの中で同期に呼びます。`set_listener()` は、イベントが来ない間（実行の前と後）にだけ呼びます。
- `SilKitLinPort(controller, role)`: コンストラクタで SIL Kit のハンドラを登録し（Slave はヘッダーのハンドラも）、デストラクタで外します。コントローラを所有しないので、participant より先に破棄します。`open()` は、Master では `Init`（フレームの応答の設定は空）、Slave では `InitDynamic` です。`send_header()` は、その ID をまだ受信用に設定していなければ、ヘッダーの長さとチェックサムモデルで設定（`SetFrameResponse`）してからヘッダーを送ります。最初のスケジュールの ID も同じ経路で設定します。型の変換（`LinFrame`、チェックサムモデル、状態）はこのポートの中だけにあります。

`VirtualLinBus` はプロセス内の LIN バスです。Master 1 つと Slave をいくつでも付けられ（`attach(role, name)`。2 つ目の Master は両方の名前を言う `std::logic_error`）、エンドポイント（`VirtualLinBus::Endpoint`、`LinPort`）はバスが持ちます。スレッドもキューも持たず、すべてを送った側の呼び出しの中で、付けた順に配ります。

- `send_header()`: すべての Slave の `on_header()` を呼び、その中の `respond()` を集めます。応答がなければ Master に NoResponse を 1 回、あれば応答ごとに 1 回（長さとチェックサムモデルがヘッダーと合えば Ok、合わなければ Error）です。
- `send_frame()`: すべての Slave の `on_header()` を呼びます。応答した Slave がいなければ、Master と全 Slave に Ok でフレームを配ります。いれば衝突で、Master に自分のフレームと各応答を配り（2 回以上）、Slave には配りません。
- `send_go_to_sleep()`: すべての Slave の `on_go_to_sleep()`。`send_wakeup()`: 送った以外のすべてのエンドポイントの `on_wakeup()`。`open()` と `resume()` は何もしません。
- Observer（`add_observer()`）は、エンドポイントに配った後に、1 つの出来事を 1 回見ます（`LinBusEvent`: Frame、NoResponse、Collision、GoToSleep、Wakeup と、時刻、送った側の名前、フレーム）。同じ時刻に違う ECU が出した出来事の順は、ステップの順です。
- 誤用（`std::logic_error`。エンドポイントの名前と直し方を言う）: `on_header()` の外からの `respond()`、1 回の `on_header()` の中での 2 回目の `respond()`、リスナーや Observer の中からの送信（再入）、配っている間の `attach()` と `add_observer()`。ヘッダーと違う ID や時刻、長さの誤った応答は `std::invalid_argument` です。
- リスナーや Observer が投げた例外は、送った側の呼び出しに伝わります。それまでに配ったものは取り消さず、後ろのエンドポイントと Observer にはその出来事が届きません。バスはその後も使えます。
- エンドポイントは外せません。Master のノードを破棄しても、そのバスに新しい Master は付けられません。
- 決定性: `StepDriver` は（開始時刻、登録順）でステップを回すので、時刻 T のステップは、ほかのホストの T より前のステップがすべて終わってから走ります。エンジンは「T より厳密に前」に設定された値と「now より前」のイベントだけを使うので、同じ時刻のステップの順は結果を変えません。SIL Kit の上と同じ規則なので、結果も同じになります（試験で、同じ ECU のクラスを `Simulation()` と `Simulation(SilKitOptions{})` で動かし、結果、受信、状態の変化、統計が一致することを確かめています）。
- 仮想バスに故障注入（応答を落とす、壊す）はありません。

### 固定周期 tick() の統合

- 既存の ECU モデル（固定周期の `tick()` を持つ DLL など）は、ステップハンドラから `TickScheduler` で呼びます。
- `TickScheduler` のタスクの周期とオフセットは、participant のステップ幅の倍数でなければなりません。そうでなければ `add()` が例外を投げます。周期がずれたまま黙って動くことはありません。
- `Ecu` の `every(period, fn)` でも呼べますが、`every` は周期とステップ幅の関係を検査しません（周期の時刻以後の最初のステップで、1 ステップに 1 回まで呼びます）。例の ECU は、周期がステップ幅の倍数であることを起動時に `on_step_grid()` で確かめてから `every` を使います。
- 1 つの participant に、周期の違う複数のタスクを載せられます（例: ステップ 5 ms で、10 ms の `tick()` と 20 ms の診断タスク）。
- ステップハンドラの中で動くので、次のことが言えます。
  - `tick()` の実行中、その participant のほかの SIL Kit ハンドラは呼ばれません。受信コールバックとの排他は不要です。
  - 長い `tick()` は全体を待たせます。
- `tick()` の中で、同じステップのうちに別のピアの応答を待つことはできません（デッドロックする）。必要になったら、ハンドラの終わりではなく応答を待ってから `CompleteSimulationStep()` を呼ぶ形を検討します。
- DLL の内部で実時間に依存している箇所（スリープ、`GetTickCount` などでの時刻の取得、XL ドライバのタイムスタンプ）は、vehicle-sil では置き換えられません。統合する DLL ごとに確認が必要です。

### 検証の範囲

単体テストは、プロセス内のレジストリか偽の CAN コントローラを使い、仮想時間で動きます。速さを変えて結果を比べるテストを除き、壁時計の速さによりません。確認していることは次のとおりです。

- 共通仮想時間
  - ステップ幅の違う参加者が SIL Kit の進行条件を守ること（ステップ T を始めた時点で、ほかの全員が T より前のステップを終えている）
  - 遅い参加者を全体が待つこと
  - `real_time_factor` で指定した速さで進むこと
  - 例外が報告されること
  - `TickScheduler` の周期
  - `StepInbox` の順序と容量
- J1939（`j1939.*`、`j1939_parts.*`、`tests/j1939/`）: 仮想バスの上で、テストが相手ノードのフレームを仮想時間で 1 つずつ送り、スタックが送るフレームとその時刻を確かめます。
  - Address Claim とアドレスの管理（J1939-81）: 主張の後の待ち、優先度による勝ち負け、Cannot Claim とその遅延、任意アドレスの選択、Request for Address Claimed、Commanded Address、アドレス違反、アドレス表、同じ Network の複数のノード、2 つの Network
  - Request と Acknowledgement（J1939-21）: グローバルと宛先指定、NACK の条件、null アドレスからの要求、不正な長さ、後で答える要求
  - トランスポートプロトコル（BAM、RTS/CTS）: 最小・最大・境界のサイズ、CTS のウィンドウと再送の要求、hold、各タイムアウト、Abort の理由、同時セッションと上限、応答の待ち行列。仮想バスの故障注入（欠落、遅延、重複、並べ替え、ポートの `QueueFull` と `Failed`、受け箱のあふれ、ECU 単位の通信断）
  - プロトコル部品（J1939-73）: DM13 による放送の抑制、DM1 の周期と内容、DM2、DM3、DM11、DM22、識別情報。部品がエンジンの公開 API と道具箱だけを使うこと（`j1939_parts.boundary`、`local.boundary`、`testkit.boundary`、`peer.boundary`）
  - 道具箱、Observer、バスのタイミング（調停と送信時間）、実時間のドライバーでの動作、定常状態のステップでメモリを確保しないこと
  - 規格との一致を確かめていない点は、「J1939」の各節にまとめています。
- J1939 と SIL Kit（`j1939_silkit.*`）: 同じシナリオ（Request、Address Claim、BAM、RTS/CTS）を仮想バスと SIL Kit（プロセス内のレジストリ、ステップ幅の違う participant）で動かし、トレースが一致することを確かめます。SIL Kit の簡易シミュレーションと簡易 CAN Network Simulator の両方で、プロトコルの結果を確かめます。
- `Peer` と試験キット（`peer.*`、`testkit.*`）: 3 つの participant での J1939 と LIN、ローカルの仮想バスと同じ結果になること、実時間と外部のポート、組み立ての誤り、失敗、破棄の順序
- LIN（`lin.*` は SIL Kit のポートの上、`lin_local.*` は `LocalBench` の仮想 LIN バスの上で主なシナリオを同じ期待値で、`lin_bus.*` は仮想 LIN バスだけ）
  - 応答値がヘッダー時刻の規則に従い、壁時計の速さにも、同じ時刻のステップの順（ECU を足した順）にもよらないこと
  - Master が送るフレームを 2 つの Slave が受信し、Slave の応答が Master に届くこと
  - 休止と起床（Master の要求、Slave のパルス、bus idle、休止中のヘッダー、同時刻の go-to-sleep とパルス）、スケジュールの切り替え（休止中を含む）、応答の取り消し、データ長の不一致による Error
  - 境界と異常系: ID 0 と 0x3F・データ長 1 と 8・長さの違うスロットの混在、同じスロットでの 2 回のスケジュール切り替え、チェックサムモデルの不一致（両方向）による Error、2 つの応答の衝突による Error（速さを変えても同じ）、時間を分けて同じ ID に応答する 2 つの Slave、既定の bus idle（4 s ちょうど）、同時刻の 2 つの wakeup パルス、`wakeup_delay` 0、Slave の受信キューのあふれ（数えられ、その後は回復する）、診断フレームの誤用の拒否
  - 診断フレーム: マスターリクエストが 1 回だけ送られ、該当する Slave が 1 回だけ応答すること、応答の置き換え・取り消し・衝突、リクエストの取り消し、マルチフレームのやり取り（速さを変えても同じ）、休止中に積んだリクエストが再開後に送られること
  - クラスタのトレースが、Master や Slave の速さを変えても一致すること
  - 仮想 LIN バスの配り方（相手、順、同期の NoResponse、不一致、衝突、go-to-sleep、wakeup、Observer、誤用の例外）と、`Ecu::lin_master()`、`lin_slave()` の名前の検査と開始後の例外
  - 同じ LIN の ECU のクラスを `Simulation()` と `Simulation(SilKitOptions{})` で動かし、記録が一致すること（長さの不一致による Error と、Master のフレームへの Slave の応答による衝突を含む。`simulation.*`）
  - `vehicle_sil::lin` が SIL Kit を含まないこと（`lin.boundary`）
- 上流へのパッチ（[dependencies.md](dependencies.md)）には、どれも、パッチを外すと失敗する vehicle-sil のテストがあります。
プロセスを分けた統合テストでは、次のことを確かめています。

- 統合テスト（`virtual_time.*`）では、registry、`vehicle_sil_sim_controller`、物理モデル（1 ms）、ECU（10 ms、共有ライブラリの `tick()` と J1939）、LIN センサー（2 ms）、ボディコントローラー（5 ms、LIN Master と J1939 の受信）を別プロセスで動かします。どのピアも `Peer` の上で動きます。
  - 各ピアのトレースを照合し、データがピアの間を予測どおりの仮想時刻で渡ることを確かめます。
  - ECU は最初のステップで Address Claimed を送り、その 250 ms 後に自分のアドレスを持ちます。CCVS はそれ以後だけ送ります。ボディコントローラーが受けた Address Claimed と CCVS の時刻と内容を、ECU のトレースと照合します。
  - 全速、遅い `tick()`、4 倍速への結合、1 つのピアだけ 0.5 倍速への結合の各条件で、トレースがバイト単位で一致することを確かめます。
  - 壁時計の所要時間が、実時間より速い（全速、4 倍速）または遅い（遅い `tick()`、0.5 倍速）ことを確かめます。
- LIN の統合テスト（`lin_cluster.*`）では、registry、`vehicle_sil_sim_controller`、Master（5 ms）、echo Slave（2 ms）、status Slave（4 ms）を別プロセスで動かします。
  - スケジュールの時刻と順序、Master のフレームの内容、両 Slave の受信時刻、Slave の応答の内容、各ノードの休止と起床の時刻を照合します。
  - echo Slave を実時間に結合した実行で、トレースがバイト単位で一致することを確かめます。Master のステップ幅をスロット長と同じ 10 ms にした条件（`cluster-master-10ms`）でも確かめます。各 Slave がフレームを処理したステップもトレースに記録し、ヘッダーの直後のステップであることを照合します。

## 統合テストで検証する J1939 のやり取り

Sender は `Peer` を実時間で動かし、J1939 スタックのノードを 1 つ作ります。NAME は industry group 2、function 130、manufacturer 0x7F0、identity 0x1234 です。Receiver が動き出してからノードを足し、アドレス 0x80 を主張させます。自分のアドレスになったら、PGN 0xFF10 を 5 回ブロードキャストします。フレームはすべて Peer の SIL Kit の CAN ポートを通って送られます。

Receiver は J1939 スタックを使いません。SIL Kit の CAN API で直接受信し、J1939 の定義から計算した期待値と、順序と内容をバイト単位で照合します。

| 順序 | 内容 | CAN ID | DLC | データ |
|---|---|---|---|---|
| 1 | Address Claim | `0x18EEFF80` | 8 | `34 12 00 FE 00 82 00 20`（NAME `0x20008200FE001234`、リトルエンディアン） |
| 2–6 | PGN 0xFF10 | `0x18FF1080` | 8 | 送信番号から決まるテストデータ |

スタックは自分の Address Claim から始めます。起動時に Request for Address Claim は送りません。

次の場合は検証失敗（終了コード 1）です。

- 拡張フレームでない
- 順序が違う（Address Claim より前の PGN、2 回目の Address Claim）
- 未知の ID がある
- 期待するフレームがそろう前に、余分なフレームが届く
- ペイロードが一致しない

期待するフレームが時間内にそろわなければタイムアウト（終了コード 2）、接続や起動に失敗すれば終了コード 3 です。

## J1939 の既知の制限

J1939 スタックの制限と、規格との一致を確かめていない点は、「J1939」の各節に書いています。実際の ECU やツールにつなぐときに影響する主なものは次のとおりです。

- ETP（1785 バイトを超えるメッセージ）はありません。
- 停止したときに走っている転送は、Abort を送らずに捨てます。受信側は保留（CTS 0）を送りません。
- DM13 で止まるのは、抑制の状態を読む部品の放送（DM1）だけです。アプリが `Node::send()` で直接送るメッセージは止まりません。自分の一時停止を終えたときは、再開を告げません（「DM13」）。
- 実際の Vector のハードウェアと Vector の Network Simulator での動作は確かめていません（「Vector XL CAN Bridge」の「検証の範囲」）。

## 将来の機能が守る制約

### 時間の扱い

- 実時間（壁時計）とシミュレーション時間（SIL Kit の仮想時刻）を、型と名前で区別します（`SimTime`）。
- 時間管理は `core` に置きます。J1939 や LIN の内部には埋め込みません。
- 一時停止とステップ実行は、`SimulationController` の時間ゲートで行います（「一時停止、ステップ実行、速度（時間ゲート）」）。Control API はこれを操作します。
- 受信ハンドラから反応的に送ったメッセージは、SIL Kit の到着保証の対象外です。新しいプロトコルを仮想時間に載せるときは、送信をステップの中で行うか、LIN と同じように到着の上限を示してから使います。

### 既存 ECU DLL の統合で残っている確認事項

- vehicle-sil は、DLL から使える共通ライブラリとして提供します。DLL そのものは取り込みません。
- 実際の DLL について、次の点を統合の前に確認します。
  - DLL 内部の実時間依存（スリープ、時刻の取得、XL ドライバのタイムスタンプ）
  - `tick()` の中で同じステップ内の応答を待つ処理の有無。ある場合は、ハンドラが戻ってもステップを完了させず、応答を待ってから `CompleteSimulationStep` を呼ぶ形が必要になる

### 物理 CAN と仮想時間（Vector XL CAN Bridge に関して）

- 物理 CAN に接続するときは、原則として実時間で動かします。
- Vector Virtual CAN も、SIL Kit の仮想時間には自動で同期しないものとして扱います。
- Vector XL CAN Bridge は、autonomous lifecycle を使い、実時間で動きます。仮想時間のシミュレーションには参加させません。
- プロトコルスタックを XL のチャネルに直接載せる場合（`ExternalPortAdapter`）も実時間だけです。時刻同期で回すホストに登録すると、構成エラーになります。

### 実行モードと通信バックエンド

- 実行モード（Pure SIL / Real-time）と通信バックエンド（SIL Kit だけか、Vector XL Bridge 経由か）は、別々の設定にします。
- Real-time モードでも、SIL Kit の中だけで動かせるようにします。
- 実時間への結合は `VirtualTimeOptions::real_time_factor` で行います（「実時間との結合」）。

### 外部操作（Control API）と GUI

外部からシミュレーションを操作・監視する Control API を設けます。担当は次のように分けます。

- **SIL Kit**: CAN/LIN の通信と仮想時間の同期（データプレーン）
- **Control API**: 操作と監視

両者は分離します。Control API は gRPC（Protocol Buffers、`proto/vehicle_sil/control/v1/control.proto` の v1）で、SIL Kit とは別の接続です。利用者向けの使い方は [control-api.md](control-api.md) にあります。

構成要素は次の3つです。

| 構成要素 | 役割 | 実装 |
|---|---|---|
| Control Agent | 各 ECU が持つ。ECU の変数（と Peer の状態）を公開し、操作を受け付ける | `vehicle_sil::control::Agent`、`Ecu::input()`、`output()`、`agent()`。Server への接続は `vehicle_sil::control_grpc::AgentClient` |
| Control Server | 複数の PC（Windows/Linux）にまたがる Peer の Agent を受け付け、操作を中継し、状態を集約する。時間ゲートで時間を操作する | `vehicle_sil::control::Hub`、`vehicle_sil::control_grpc::ControlServer`、実行ファイル `vehicle_sil_control_server` |
| クライアント | GUI、CLI、Python。いずれも共通の Control API を使う | Python のパッケージと CLI（`python/vehicle_sil_control`）。GUI はまだない |

#### Control Agent（`vehicle_sil::control`）

ECU の中で、外から操作できる変数を公開し、外からの書き込みをステップの中の決まった時刻に反映します。通信（Control Server との接続）は含みません。SIL Kit にも依存しません。ECU（`Ecu`）が 1 つ持ち、ECU のクラスが変数を宣言します（「ECU の入口」）。

```cpp
class Body {
public:
    explicit Body(vehicle_sil::Ecu& ecu) {
        ecu.input("cabin.parking_brake", &parking_brake_, {.description = "Parking brake switch"});
        ecu.input("engine.coolant", &coolant_, {.unit = "degC", .min = -40.0, .max = 210.0});
        ecu.input_enum("trans.gear", &gear_, {{"P", 0}, {"R", 1}, {"N", 2}, {"D", 3}});
        ecu.output("vehicle.speed", [this] { return speed_; }, {.unit = "km/h"});
    }
    // ...
};
```

- 変数: 型は bool、整数（C++ の型の範囲で検査。64 ビットの符号なしは入力にできず、出力では int64 の最大で止めて読む）、実数（`float` は `float` の範囲で検査）、列挙（値の一覧。型に収まり、名前と値が重複しないこと）。役割（FMI の causality に合わせる）は Input（走行中も書ける）、Output（読むだけ）、Parameter（最初のステップより前だけ書ける）。メタデータは名前（`.` で階層）、単位、最小、最大（含む。NaN は不可）、刻み（編集の目安、強制しない）、説明、列挙の項目で、`schema()` が登録順に返します。変数は、スキーマが確定する前に登録します（後は `std::logic_error`。誤用の検出で、同期ではないので、登録と `submit()` を別のスレッドで同時に行わない）。スキーマは、`complete_schema()`（通信の側が `schema()` を初めて読む前に呼ぶ。`AgentClient` はコンストラクターで呼ぶ。ドライバーは最初のステップの前に呼ぶ）、最初の `submit()`、最初のステップのうち、最初のもので確定します。`complete_schema()` は、`bind_driver()` に渡された関数（ドライバーが足す変数。Peer の監視の出力）を 1 回呼んでから登録を閉じます。その関数が例外を投げれば、例外が伝わり、スキーマは確定しません。ポインタや関数の参照先は Agent より長く生きること（ECU の Agent では、ECU が持つもの）。
- 書き込み（`Command`）: 1 つ以上の書き込みを、同じステップでまとめて反映します。`submit()` が名前、型、範囲、列挙、書き込めるかを確かめ、だめなら `Invalid` で何も積みません。整数の変数は整数か整数値の実数を、実数の変数は整数か有限の実数を受け付けます。整数の変数の最小と最大が型の範囲の外にあれば、登録のときに `std::invalid_argument` です。範囲外の値は丸めません（整数の範囲は整数のまま比べます）。`finish()` の後の `submit()` も `Invalid` です。
- 反映の時刻: `Command::at` 以上の開始時刻を持つ最初のステップで、アプリの処理の前に反映します（アプリはそのステップで新しい値を見ます）。Peer では、J1939 の受信処理、ハンドラ、部品のタイマー（`PeriodicSender` の Encoder など）はアプリの処理より前に動くので、それらが新しい値を使うのは次のステップからです。`at` がなければ次のステップです。同じステップで反映するものは (at, seq) の順です（`at` のないものは、そのステップの時刻として並べます）。participant が `at` 以降に始まるステップを受け取る前に実行していれば、その操作は遅すぎます。既定では反映せずに `Late` を返し、`LatePolicy::ApplyNext` なら次のステップで反映して `late` を立てます。Parameter の書き込みは、最初のステップが始まった後の `submit()` なら `Invalid`、始まる前に積んで時刻が最初のステップより後なら `Rejected` です。関数で登録した入力の setter が投げたら、その操作は `Rejected` で、同じ操作の中で先に書いた値はそのまま残ります（ステップは失敗しません）。
- 結果: キューに積んだ操作には、必ず 1 つの `CommandResult`（`Applied` と反映したステップの時刻、`Late`、`Rejected`、`Cancelled`）が返ります。シミュレーションが終わると `finish()` が、残っている操作を `Cancelled` にします（ECU の Agent では ECU が呼びます。「ECU の入口」の変数の時点）。`VirtualTimeOptions::end` 以降の時刻を指定した操作と、ステップが失敗した Peer への操作も、`finish()` まで確定しません。実時間の Peer では、`at` はその Peer のドライバーの時計の時刻です。
- 値: ステップの終わり（Peer では LIN の処理の後）に、0、`sample_period`、2 `sample_period` … の時刻以後の最初のステップ（`Ecu::every()` と同じ数え方。既定 10 ms、0 なら毎ステップ）で全変数を読み、`latest()` で最新の 1 組を返します。受け渡しは 3 つのバッファで、ロックはありません。
- スレッドと上限: ステップのスレッド（`on_step_begin`、`on_step_end`、`log`、`finish`。1 本）と通信の側（`submit`、`drain_results`、`drain_events`、`latest`、`stats`。互いにスレッド安全で、複数のスレッドから呼べる）で使います。ステップのスレッドは通信の側を待ちません。通信の側は、`finish()` の間と、実時間のドライバーを起こす `Host::post()` の短いロックを待つことがあります。`bind_driver()` に渡す起こしの関数の中から `submit()` と `clear_wakeup()` は呼びません。受け渡しは固定容量の SPSC リングです。処理中の操作（積んでから結果を `drain_results()` で受け取るまで）は `max_pending`（既定 256）までで、超えた `submit()` はすぐ `Busy` を返します。そのため結果は捨てません。出来事は `max_events`（既定 1024）までで、あふれたものは捨てて数えます。変数のアクセサはステップのスレッドだけで呼ぶので、モデルにロックは要りません。ステップのスレッドは通信のスレッドが持つロックを取りません（使い終えた操作の解放と、結果の理由の文字列の確保はステップのスレッドで行います。量は 1 ステップに `max_pending` まで）。
- 決定論性: Agent は、反映の時刻を決めるだけで、ステップの並びとデータを変えません。Agent がない実行、Agent があって値を読み続けている実行で、通信の結果は同じです。時刻を指定した書き込みは、participant がその時刻以降に始まるステップを実行する前に届けば、何度実行しても同じステップで反映されます。保証はこの範囲だけです。
  - 時間ゲートで止めてから書くとき: 止まっている位置 P（`TimeState::now`）に始まるステップを、すでに実行した、または実行中の participant があります。そのため `at` は P より後（たとえば P + 1 ns）にします。`at` が P 以下なら `Late` になりえて、`at` がなければ P と次のステップのどちらで反映されるかは届いた時刻で決まります。
  - 同じステップで反映する操作の順（seq）は、Hub が受け付けた順です。複数のクライアントが同じ変数に同じ時刻で書くと、残る値は届いた順で決まり、実行ごとに変わりえます。
- ECU が動かす: ECU は Agent を作ったときに自分をドライバーとして結び付け（`bind_driver()`）、アプリの処理の直前に `on_step_begin()`、LIN の処理の直後に `on_step_end()` を呼び、止まるときに `finish()` を呼びます（「ECU の入口」）。実時間の Peer では、操作を積むとドライバーを起こします（`Host::post()`。未処理の起こしは 1 つまで）。Peer は止まり始めると起こさせるのをやめ、ステップがもう走らなくなってから `finish()` を呼ぶので、その後の `submit()` は `Invalid` で、Peer には触れません。Peer は `start()` で、Agent があればスキーマを確定させます。
- Peer の監視（`PeerOptions::monitor`、既定で有効）: Agent のスキーマが確定するときに、Peer が ECU の変数の後に次の出力を足します。値は Agent の読み出しの時にステップのスレッドで読みます。Agent のない Peer では何もしません。
  - `peer.steps`（ステップの数）、`peer.speed`（前の読み出しからの仮想時間の秒 / 壁時計の秒。最初の読み出しでは 0、止まっていた後は小さくなる）、`peer.app_time_max_us`（前の読み出しから、アプリと LIN の処理にかかった最長の時間。J1939 の処理と書き込みの反映は含まない）
  - J1939 のネットワーク N ごとに `j1939.N.bus_state`（列挙: unknown、error_active、error_passive、bus_off）と、統計の `frames_received`、`frames_sent`、`send_failures`、`send_queue_full`、`receive_overflows`、`held_dropped`、`address_violations`、`transport_aborts_sent`、`transport_aborts_received`
  - N のノード i ごとに `j1939.N.node<i>.address` と `j1939.N.node<i>.address_state`（列挙: fixed、not_started、claiming、claimed、cannot_claim）
  - 出来事: ノードのアドレスの状態かアドレスが変わったとき（本文は「j1939.N.node<i>: <状態> at address <a>」で、状態の名前は列挙の項目と同じ。cannot_claim は Warning）
  - 対象は、スキーマが確定する時点（`AgentClient` を作る、最初の `submit()`、`start()` のうち最初のもの）にあるネットワークとノードです。その後の `Peer::j1939()`（両方の形）は、監視されないネットワークになるので `std::logic_error` です（文言に Peer とネットワークの名前、直し方: `AgentClient` を作る前、最初の `submit()` の前に足すか、`PeerOptions::monitor = false`）。その後に足したノードは監視されません。生の CAN と LIN は監視の対象ではありません。名前が ECU の変数と重なれば、確定させる呼び出しが `std::invalid_argument`（何も変えず、スキーマは確定しない。文言に Peer の名前と `PeerOptions::monitor = false`）。ステップの失敗はここでは知らせません（失敗の後はステップが走らないため）。`Peer::failed()` と `error()`（スレッド安全）を通信の側で読みます。

#### Control Server の中核（`vehicle_sil::control::Hub`）

Control Server の、通信の方式によらない部分です。接続した Agent の一覧、書き込みの振り分け、Peer ごとの結果、最新の値、上限つきの購読を受け持ちます。全関数がスレッド安全です。

- 通信の層は、接続ごとに `agent_connected(hello, link)` を呼んで `ConnectionId` を受け取り、以後 Agent から届いたもの（`agent_submitted`、`agent_result`、`agent_values`、`agent_events`、`agent_status`）と切断（`agent_disconnected`）を、その ID で渡します。`hello` は Agent を Peer に `attach()` した後で作ります（監視の出力がスキーマに入るため）。`agent_status` には `Peer::failed()` と `error()` を渡します（ステップの失敗は監視の出力では届かないため）。
- `AgentLink::send()`: Hub は自分のロックを持たずに呼びます。1 つの Peer には 1 本のスレッドから、通し番号の順に 1 つずつ呼ぶので、短く待っても、中から Hub を呼び返してもかまいません。`false` を返せば、その操作は Invalid、例外なら Unknown になります。`write()` を呼んだスレッドが、同じ Peer への前の操作の送信も引き受けることがあるので、`write()` は `send()` の分だけ待つことがあります。Hub はリンクの最後の参照を、ロックの外で手放します。`agent_connected()` には、空でない名前、リンク、0 より大きい `max_pending` が要ります。
- Agent は participant 名（空は不可）と起動ごとの ID（incarnation）で名乗ります。
  - 同じ名前と同じ ID: 同じプロセスのつなぎ直しです。新しい接続が古い接続に代わり、未完了の操作は続きます。古い接続の切断の知らせが後から届いても、Peer は切断になりません（その接続に渡して積まれたと知らされていない操作だけが Unknown になります）。hello の `open_seqs` に、Agent がまだ持っている操作（積んだが、結果を通信の層がまだ届けていないもの）を入れると、それらは新しい接続で Queued になり（古い接続の終わりの影響を受けません）、それ以外の未確定の操作は Unknown で確定します。通信の層は、届けられなかった結果を次の接続で送り直します。`open_seqs` を入れなければ何も確定させないので、結果を失った操作は未確定のまま残り、その Peer の `max_pending` の枠を使い続けます。
  - 同じ名前で別の ID: プロセスの再起動です。前の ID の未完了の操作は Unknown になり、前の ID の値は消えます。
  - どちらの場合も、置き換えた古い接続は `AgentLink::close()` で閉じます。同じ名前の 2 つのプロセスが同時に動いている（設定の誤り）ときも、古いほうは黙って無視されるのではなく、接続が切れたことを知ります。gRPC の層は置き換えた接続を `ABORTED` で終え、置き換えた新しい接続の識別子（hello の `connection`。`AgentClient` が接続ごとに作る）を trailing metadata の `vehicle-sil-replaced-by` で知らせます。識別子は 1〜64 文字で、使える文字は `A-Z a-z 0-9 . _ -` だけです。gRPC は metadata の値に許さないバイトがあるとプロセスを止めるので、Server は hello を受けたところでほかの値を `INVALID_ARGUMENT` で断り、metadata に入れる前にも同じ検査をします。識別子のない hello（送らない古いクライアント）は受け付けますが、置き換えのときに見分けられません。新しい接続が勝ちます。`AgentClient` は、その識別子が自分の接続のものなら（Server がまだ古いストリームを持っていた、つなぎ直し）ふつうにつなぎ直し、ほかのプロセスのもの（値が空、つまり識別子を送らないクライアントを含む）なら、つなぎ直しをやめます（`replaced()` が真、`last_error()` に participant 名、置き換えた接続の incarnation と接続元、同じ名前のプロセスが 2 つ動いていないか確かめること。`on_replaced` を 1 回呼び、指定がなければ標準エラーに 1 行出す）。この metadata のない `ABORTED`（間の proxy などが返したもの）は、ほかの切断と同じにつなぎ直します。Server は置き換えを監査に残し、別の incarnation が置き換えたときは、新しい接続の Peer の出来事（Warning。時刻は Server の見たシミュレーションの時刻）としても購読に流します。
  - 切断: Agent が積んだ操作は Disconnected（未確定）になり、同じ ID でつなぎ直した後に結果が届けば、それで確定します。リンクに渡したが積まれたと知らされていない操作は Unknown、まだリンクに渡していない操作は Invalid です。`forget_peer()` で切断した Peer を消すと、その未確定の操作は Unknown になり、購読者には `removed` の印つきの PeerUpdate が届きます。
  - 結果は、操作を送った ID のどの接続から届いても受け付けます。別の ID からの結果は使いません。値、出来事、状態は、いまの接続からのものだけを使います。スキーマと数が合わない値と、前より古い値は捨てます。
- `write(request)` は、Peer ごとのまとめをロックの外で行い、操作に Hub 全体で増える通し番号（既定の初めの値は壁時計のマイクロ秒で、Server を起動し直しても前より大きい）を振り、Peer ごとにまとめた `Command`（その番号を `seq` に持つ）を送ります。Hub が確かめるのは、Peer がつながっていること、変数があって書けること、その Peer の未完了の操作が `max_pending`（hello で受け取る）より少ないこと（多ければ Busy）だけです。型、範囲、反映するステップは Agent が決めます。Peer ごとの状態は Sent → Queued（Agent が積んだ）→ Applied、Late、Rejected、Cancelled のどれか、または Disconnected、Invalid、Busy、Unknown です。Sent、Queued、Disconnected 以外は確定で、二度と変わりません。
- `wait_queued(seq)` は、Sent の目標がなくなるまで待ち、そのときの操作を返します（時間切れと、知らない番号は空）。止まっている位置より後の時刻を指定して書き込み（Control Agent の「決定論性」）、すべての目標が Queued になったことを確かめてから再開すれば、遅すぎる書き込みは起きません。`wait_done(seq)` は確定まで待って操作を返すので、直後に操作が忘れられても結果はわかります（待っている間に忘れられたときも空を返すので、`max_operations` は待つ操作の数より大きくします）。
- 購読（`subscribe()`）: 始めると、その時点の Peer の一覧と最新の値がまず届きます。Peer、値、操作は、それぞれ Peer ごと・操作ごとに最新の状態だけを持ちます（読む前に変われば置き換える）。上限（`capacity`）があるのは出来事と操作で、あふれたら、出来事は古いものから、操作はいちばん前に変わったものから捨てて数えます。値には、そのスキーマの incarnation が付きます。`poll()` の順は、Peer、操作（番号の順）、出来事（届いた順）、値です。`close()` で待っている `poll()` を起こして終えます。
- 覚えておく操作: 確定した操作は `max_operations` までで、超えたら古いものから忘れます。未確定の操作は忘れません（数は Peer ごとの `max_pending` で決まります）。

#### Control Server と通信の層（`vehicle_sil::control_grpc`）

`VEHICLE_SIL_BUILD_CONTROL_SERVER`（既定 OFF）のときだけ作ります。gRPC は `find_package(gRPC CONFIG)` で探し、なければ FetchContent で版を固定して取り込みます（[dependencies.md](dependencies.md)）。ほかのライブラリは gRPC に依存しません。

- サービス: クライアント向けの `ControlService`（Peer の一覧、値、書き込み、操作の結果と待ち、購読、Peer を忘れる、時間の操作）と、Agent 向けの `AgentService`（1 接続 1 本の双方向ストリーム。Agent の hello を受け付けると、Server はどのコマンドよりも先に Welcome を返す）。列挙はどれも 0 が UNSPECIFIED で、知らない値は安全側に読みます（変数は書けないものとして、操作の結果は受け取らなかったものとして）。
- `ControlServer`: Hub に 2 つのサービスを載せます。
  - 時間: `TimeControl`（`time_control_of(controller)` で SimulationController の時間ゲートを使う）があれば時間の操作を受けます。なければ時間の操作は `FAILED_PRECONDITION` で、`GetTime` は `available = false` を返します。`Stop` は停止を要求するだけで待ちません（複数のクライアントから同時に呼べます）。理由は全 participant と SIL Kit のログに渡るので、制御文字を `?` にして 200 バイトまで（UTF-8 の文字の途中では切らない）にし、監査にも残します。シミュレーションが始まる前の `Stop` は、始まった直後に止めます（`ParticipantRuntime::request_stop()` は、lifecycle が Running になる前の要求を保ち、最初のステップで渡します。SIL Kit は Running の前の Stop を無視するため）。
  - シミュレーションの監視（`SimulationMonitor`、`ControlServer::simulation()`）: `TimeControl` があれば、Server は 1 本のスレッドで `ServerOptions::monitor_period`（既定 50 ms）ごとに `TimeControl` を読み、時間の状態、participant ごとの SIL Kit の状態、失敗した participant と理由（`FailureCheck`。猶予は `leave_grace`。一度失敗を見たら消さない）、終わったか、を 1 つのスナップショットに持ちます。変わったときだけ購読者（監視のスレッドか、`poll()` を呼んだスレッド、つまり時間の呼び出しを処理する gRPC のスレッドで、1 つずつ呼ばれる。短く、中から購読の登録や解除をしない）に知らせます。`ListPeers`、`Watch`、`vehicle_sil_control_server` の終わりの判定は、どれもこのスナップショットを使うので、購読の数によらず `TimeControl` を読むのは監視の 1 か所だけです。状態を読めなかったとき（`TimeControl` が投げた）は、前に読めた値を残し、理由を添えます（`ListPeers` の `simulation_error`、`SimulationState::error`。participant と時間の両方を読めなければ両方の理由）。participant を読めなかった回は失敗を判定しないので、いなくなった participant の猶予は数え直さずに続きます。`SimulationMonitor` は `TimeControl` だけに依存します。`TimeControl::left_participant()` は純粋仮想です（実装し忘れると、失敗を判定しないまま動くため）。SIL Kit の実装は、participant の状態を最初の 1 回だけ SIL Kit から読み直し（ハンドラーの登録中に届いた状態を SIL Kit が渡さないことがあるため）、以後はハンドラーが受けた状態を読みます。
  - Peer の一覧（`ListPeers`）: つながった Agent に、controller の必須の participant（`SimulationControllerOptions::required_participants`、`vehicle_sil_control_server` の `--required`。どの PC でも、Agent がなくても）を合わせたものです。必須でない participant は、Agent があるときだけ（`simulation_state` は空で）出ます。各 Peer に、Agent の有無（`has_agent`）と SIL Kit のライフサイクルの状態（`simulation_state`。まだ見えていなければ空）が付きます。`Watch` の Peer も同じ変換で作るので、同じ Peer は `ListPeers` と同じ内容で届きます。
  - 誤り: gRPC の状態コードで返します（引数の誤りは `INVALID_ARGUMENT`、ない操作や Peer は `NOT_FOUND`、待ちの時間切れは `DEADLINE_EXCEEDED`）。
  - 待つ呼び出し（`WaitOperation`、`WaitPaused`）: `max_wait`（既定 60 秒）で打ち切り、クライアントの取り消しと Server の終了でもすぐに終わります。シミュレーションが終わった後の `WaitPaused` は、待たずに `FAILED_PRECONDITION` を返します。
  - 待つ呼び出しと `Watch` は、それぞれ Server のスレッドを 1 本使います。同時に `max_waiting_calls`（既定 256）までで、超えたら `RESOURCE_EXHAUSTED` です。
  - 購読（`Watch`）: 始めにその時点の Peer と最新値（`time` を求めれば時間の状態、`simulation` を求めればシミュレーションの状態も）を送ってから `initial_done` を送り、以後は変化を送ります。時間とシミュレーションの状態は、監視が知らせたスナップショットを購読ごとの待ち行列（64 件）に入れておき、Hub の変化を待つ間（最長 100 ms）ごとに、順に、変わったものを送ります。待ち行列があふれると古いものから捨て、捨てた数を `dropped` に足します（遅いクライアントには途中の状態が届かないことがあります。止まるのを待つには `WaitPaused` を使います）。失敗と終わりは一度立つと後のスナップショットすべてに残るので、捨てられても失われません。シミュレーションの状態（`SimulationState`）は、必須の participant とその SIL Kit の状態、失敗した participant とその状態と理由、終わったか、読めなかった理由です。シミュレーションを調停しない Server は、`simulation` を求めた購読に `available = false` を 1 回だけ送ります。Peer は `ListPeers` と同じ一覧（Agent のない必須の participant を含む）で、SIL Kit の状態が変わった Peer は送り直します。Agent の Peer が忘れられても必須の participant なら、Agent のない Peer として送ります（`removed_peer` は一覧から消えたときだけ）。時間の状態の `running` が偽のとき、`ended` で「まだ始まっていない」と「終わった」を区別します。時間を変える呼び出し（一時停止、再開、ステップ、指定時刻まで、速度、停止）と `WaitPaused` は、その場で監視に読ませるので、その結果の状態は、待ち行列があふれない限り購読に届きます。`capacity` は `max_watch_capacity`（既定 65536）までです。
  - 接続: keepalive（10 秒ごと、5 秒で応答がなければ切断）を使うので、黙って切れた接続も数十秒以内に見つかります。
- `AgentClient`: Agent の通信の側を自分のスレッドで動かします。Server へ接続し、Welcome を受けたらつながったとみなします。切れたら指数バックオフ（`reconnect_min` から `reconnect_max`）でつなぎ直し、Server に断られたら（認証、hello の誤り）`reconnect_max` 待ちます（理由は `last_error()`）。同じ participant 名の別のプロセスに置き換えられたら、つなぎ直しをやめ、`on_replaced` で知らせます（上の「同じ名前と同じ ID」の項）。シミュレーションは止めません。受けたコマンドを `submit()` し、結果、値（新しい読み出しだけ）、出来事、Peer の状態を `publish_period`（既定 50 ms、壁時計）ごとに送ります。取り出した結果は送る前に「最近の結果」（`max_pending` 件）に入れ、つなぎ直すときは、Agent がまだ持っている操作の番号（`open_seqs`。積んで結果を送り終えていないものと、最近の結果）と、最近の結果を送り直します。Server はすでに確定した結果を無視するので、二重には反映されません。起動ごとの ID（incarnation）は既定で時刻と乱数から作ります。接続の識別子は incarnation から作らず、クライアントごとの乱数（16 進 16 桁）と接続ごとの連番から作るので、incarnation にはどの文字を使ってもかまいません。`token` は表示できる ASCII（0x20〜0x7E）だけです。ほかの文字があれば、コンストラクタが `std::invalid_argument` を投げます。コンストラクターで Agent のスキーマを確定させるので（`Agent::complete_schema()`。Peer の Agent では監視の出力も足す）、ECU が変数を宣言した後、実行の前に作ります（最初のステップより前に Server につながる）。最近の結果の窓（`max_pending` 件）を越えて送れなかった結果の操作は、つなぎ直した後に Unknown で確定します。終えるときは ECU を止めてから（`Peer::stop()` が `Agent::finish()` を呼ぶ）`stop()` を呼ぶと、残りの結果を送ってから閉じます（送り切れなければ約 3 秒で打ち切り、ストリームの終わりをさらに最長 1.5 秒待つ）。`AgentClient` は、Agent を持つ ECU（`Peer`、`Simulation`、`LocalBench`）と、`status` が参照するもの（Peer など）より先に破棄します（後に宣言する）。`tls` を立てて PEM を渡さなければ、システムのルート証明書で TLS を使います。
- 認証とアクセス制御:
  - TLS: `SecurityOptions` にサーバーの証明書と秘密鍵（PEM）を渡すと TLS で待ち受けます。クライアントと `AgentClient` は、ルート証明書（PEM）を渡して TLS でつなぎます。
  - トークン: 認証情報（`Credential`: 名前、トークン、役割、Agent なら名乗れる participant 名）を渡すと、すべての呼び出しに `authorization: Bearer <token>`（`Bearer` の大文字と小文字は問わない）が要ります。資格情報のファイル（`load_credentials()`）では、同じトークンの重複、Agent 以外の行の participant 名、participant 名のない Agent の行を誤りにします（どの名前でもよいときは `*` を書きます）。`SecurityOptions` に認証情報を直接渡しても同じ検査をし（`std::invalid_argument`）、証明書と秘密鍵の片方だけでも `std::invalid_argument` です。ないか合わなければ `UNAUTHENTICATED`、役割が足りなければ `PERMISSION_DENIED` です。トークンは長さによらない時間で比べます。認証情報を渡さなければ、誰でも何でもできます。
  - 役割: viewer（読む: 一覧、値、操作、購読、時刻）< operator（変える: 書き込み、一時停止、再開、ステップ、速度、Peer を忘れる）< admin（停止）。agent は Agent の接続だけができ、クライアントの呼び出しはできません。agent の認証情報には、名乗れる participant 名を必ず書きます（`Credential::participants`）。`*`（`kAnyParticipant`）を書いた認証情報はどの名前でも名乗れ、つながっている Peer を置き換えられます（名前は予約しません）。書いていない名前で名乗ると、認証情報の名前、名乗った名前、名乗れる名前を添えて `PERMISSION_DENIED` で断ります。viewer は、値、スキーマ、出来事、操作（書いた人の認証情報の名前と接続元を含む）を読めます。 RPC ごとに要る役割と監査の有無は、Server の 1 つの表（`rpc_rules()`）にあり、各 RPC はその表で認可してから処理します。表にない RPC は、誰が呼んでも `PERMISSION_DENIED` です（RPC を足して表を忘れると、試験が落ちます）。
  - ループバック以外のアドレスで待ち受けるには、TLS と認証情報の両方が要ります（`allow_insecure` を明示しない限り、`std::invalid_argument` で起動しません）。ループバックとみなすのは `localhost`、`[::1]`、数字の `127.x.y.z`、`unix:` のアドレスだけです。
  - 監査: `audit` に、変更する呼び出し（書き込み、時間の操作、Peer を忘れる、Agent の接続と置き換え）と、断った呼び出し（Agent の hello を含む）を、誰が（認証情報の名前と接続元）、何を、結果、の 1 行で渡します。制御文字は `?` に置き換え（名前に改行を入れて偽の行を書けない）、1 行は約 1000 バイトで切ります。書き込みの行は最初の 16 個の名前と残りの数です。断った呼び出しはポートに届く誰でも起こせるので、1 秒に 20 行までにし、記録しなかった数は、次に記録する断った呼び出しの行の前に知らせます。監査の関数が投げた例外は無視します。
  - SIL Kit のレジストリとデータプレーンには認証がありません。閉じたネットワークで使います。
- 上限: Server の未完了の操作は Peer ごとの `max_pending`（hello の値。65536 まで）、確定した操作は `max_operations`、購読は `capacity`（既定 1024、`max_watch_capacity` まで）、Agent のストリームの送信待ちは未完了の操作の数まで、1 回の `Write` の書き込みは `max_writes_per_request`（既定 4096。超えたら `INVALID_ARGUMENT`）、待つ呼び出しと `Watch` は `max_waiting_calls` までです。hello のスキーマに空の名前や重複があるとき、`open_seqs` が `max_pending` の 2 倍（積んだ操作と最近の結果）を超えるとき、`connection` が空でなく、1〜64 文字の `A-Z a-z 0-9 . _ -` でないときは断ります。1 つのメッセージは gRPC の既定の 4 MB までです。
- Server の再起動: Hub の状態（操作、値）は引き継ぎません。前の Server の操作の番号を問い合わせると `NOT_FOUND` です。Agent がまだ持っている前の Server の操作（時刻つきを含む）はそのまま反映され、結果は新しい Server が捨てます。`vehicle_sil_control_server` は system controller も兼ねるので、起動し直せばシミュレーションも終わります。

Control API で扱う機能は次のとおりです。

- Peer の一覧と状態の取得
- 開始（必要な participant がそろったとき）、停止、一時停止、再開（リセットはない。やり直すにはプロセスを起動し直す）
- 仮想時間の取得、ステップ実行、指定時刻まで実行、速度設定
- スイッチ、レバー、センサー値などの操作（仮想時刻を指定した操作を含む）
- 通信状態（J1939 のアドレスと統計、バスの状態）、エラー、出来事、性能情報の監視
- 状態変更のリアルタイム通知（購読）
- CAN/LIN チャネルと Vector デバイスの選択はまだない（起動時の設定。変えるには再起動が要り、Launcher と一緒に扱う）

守る制約は次のとおりです。

- **tick をブロックしない**: Agent のネットワーク処理が、シミュレーションのステップ処理（tick）をブロックしないようにします。操作は上限付きのキューで受け渡します。
- **仮想時刻に合わせる**: 操作は、シミュレーション時刻に合わせて反映します。
- **上限を設ける**: キューとメモリの使用量には上限を設けます。
- **認証する**: 複数の PC の間の通信には、認証とアクセス制御を設けます。
- **パラメータを自己記述する**: 各 Peer は、操作できるパラメータの名前、型、範囲、単位を公開します。GUI はこれを使って操作画面を自動で生成します。
- **GUI を分ける**: GUI はシミュレーションの実行環境とは別のプロセスにし、できるだけ単体のバイナリで配布します。第一候補は Slint です。公開ライブラリ（`vehicle_sil::*`）には、GUI に固有の依存を入れません。
- **Launcher は後回し**: 停止している Peer を起動する仕組み（Launcher）は、必要になった段階で検討します。
- **チャネル選択のモデルを共通にする**: CAN チャネルと Vector デバイスの選択では、CLI と同じモデル（`vehicle_sil::vector_xl::XlTarget` とチャネル番号）を使います。

### LIN

`LinMaster` と `LinSlave`（「共通仮想時間」の節）は実装済みです。未実装の信号のエンコードとセンサーの作成支援では、次の制約を守ります。

- SIL Kit の LIN API を使い、SIL Kit が提供する機能は再実装しません。
- 次の要素は分けて扱います。
  - Master と Slave の役割
  - スケジュールテーブル
  - 信号のエンコード
- LIN Slave のセンサーピアは、センサー値を設定するだけで LIN の応答に反映されるようにします。
