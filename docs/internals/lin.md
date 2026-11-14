# LIN を仮想時間で動かす（`vehicle_sil::lin`）

LIN の Master と Slave（`LinMaster`、`LinSlave`）、ポート（`LinPort`）、仮想 LIN バス（`VirtualLinBus`）が守るルールと制約を書きます。使い方の例は [lin-guide.md](../lin-guide.md) にあります。

| 節 | 書いてあること |
|---|---|
| [ライブラリの構成](#ライブラリの構成) | 依存、ポートの実装、ヘッダーごとの内容 |
| [共通のルール](#共通のルール) | ステップ、要求が反映される時点、受信の保証、スレッド、`init()`、例外 |
| [スケジュールテーブル（`LinMaster`）](#スケジュールテーブルlinmaster) | エントリの条件、ID の設定、結果を返す時点、切り替え |
| [フレームの送受信](#フレームの送受信) | Slave と Master が送信する値、衝突、Slave の受信、SIL Kit での制約、制限 |
| [診断フレーム（0x3C、0x3D）](#診断フレーム0x3c0x3d) | マスターリクエストとスレーブレスポンスのルール |
| [休止と起床（Sleep/Wakeup）](#休止と起床sleepwakeup) | 要求ごとの動き、同じ時刻の優先、SIL Kit のコントローラーの状態 |
| [ポート（`LinPort`）と仮想 LIN バス（`VirtualLinBus`）](#ポートlinportと仮想-lin-バスvirtuallinbus) | ポートの関数と通知、`SilKitLinPort`、仮想バスの配信と誤用 |
| [信号のエンコードとセンサーのピア](#信号のエンコードとセンサーのピア) | ないものと、追加するときに守ること |
| [検証の範囲](#検証の範囲) | テストが確認していること |

## ライブラリの構成

`vehicle_sil::lin` は `vehicle_sil::sim_base` だけに依存し、SIL Kit にも J1939 にも依存しません。CTest の `lin.boundary` が、ソースとヘッダーが SIL Kit を含まないことを確認します。

エンジン（`LinMaster`、`LinSlave`）は、ポート（`LinPort`）だけを通してバスを使います。ポートの実装は 2 つです。

| 実装 | ライブラリ | バス |
|---|---|---|
| `VirtualLinBus` | `vehicle_sil::lin`（同じライブラリ） | プロセス内 |
| `SilKitLinPort` | `vehicle_sil::lin_silkit` | SIL Kit の LIN コントローラー（trivial simulation） |

`Ecu::lin_master()` と `Ecu::lin_slave()` は、実行する場所に合ったポートの上にノードを作ります。`LocalBench` では仮想バス、`Peer` では SIL Kit です。

ファイルは役割ごとに分けています。

| ヘッダー | 内容 |
|---|---|
| `lin_types.hpp` | 共通の型（`LinId`、`LinChecksumModel`、`LinFrame`、`LinRole`、`LinResultStatus`、`LinPublisher`、`LinNodeState`、`LinReceivedFrame`） |
| `lin_schedule.hpp` | スケジュールテーブル（`LinScheduleEntry`、`LinScheduleTable`、`validate_schedule()`） |
| `lin_port.hpp` | `LinPort`、`LinPortListener` |
| `lin_master.hpp` | `LinMaster` |
| `lin_slave.hpp` | `LinSlave` |
| `virtual_lin_bus.hpp` | `VirtualLinBus`、`LinBusEvent` |
| `lin_silkit/silkit_lin_port.hpp`（`vehicle_sil::lin_silkit`） | `SilKitLinPort` |

## 共通のルール

### ステップと要求の反映

- Master も Slave も、`step(now)` をステップごとに 1 回、そのステップの時刻で呼びます。
- バスへの送信（ヘッダー、Master のフレーム、go-to-sleep コマンド、wakeup パルス）は、すべて `step()` の中で行います。
- 要求（`set_frame_data()`、`send_master_request()`、`set_schedule()`、`request_sleep()`、`request_wakeup()`）は、同じステップで `step(now)` より前に呼べば、そのステップで反映されます。

### 受信の保証

ステップ T で送信したものは、ほかのノードの T より後のステップまでに必ず受信されます。仮想バスでは、送信した呼び出しの中で受信されます。SIL Kit では、Master が送信するフレームは SIL Kit が 1 段遅れて送信しますが、`ParticipantRuntime` がステップの完了をその後にするので、同じ保証が成り立ちます（[virtual-time.md](virtual-time.md) の「参加と進み方」）。

受信側は、受け取ったイベントを `StepInbox` にためて、`step(now)` で now より前のものだけを時刻順に処理します。そのため、状態の変化とフレームの受信は壁時計にも、同じ時刻のステップの順にもよりません。

### ノードの作り方と生存期間

- 作り方は `LinMaster(port, schedule, step, options)` と `LinSlave(port, options)` です。`step` はノードを実行するステップ幅です。
- ポートはノードより長く生きなければなりません。ノードはコンストラクターからデストラクターまでポートのリスナーです。
- ハンドラー（`LinMaster::on_result()`、`on_state()`、`LinSlave::on_frame()`、`on_state()`）は、最初のステップの前に設定します（スレッドセーフではありません）。空の関数は「なし」です。
- `Ecu` を通すと、ポートとステップ幅は実装が決めます。

### 状態の通知

状態の変化は `StateHandler(time, state)` に、時刻付きで通知します。

| ノード | 通知する時刻 |
|---|---|
| Slave | 受け取ったイベントの時刻（自分の操作ならそのステップの時刻） |
| Master | 状態が変わったステップの時刻 |

### スレッド

`init()`、`step()`、`request_*()` はノードのステップから呼びます。ポートはスレッドセーフではありません。ただし、SIL Kit はステップハンドラーと受信ハンドラーを同じ I/O スレッドで呼び、仮想バスは送信側のステップの中で配信するので、ポートの呼び出しは 1 つのスレッドに収まります。

`LinSlave::set_response()`、`clear_response()`、`set_slave_response()` は、どのスレッドから呼んでもかまいません。

### `init()` を呼ぶ時点

両方の `init()` は、最初のステップの前に 1 回呼びます（`Ecu` を通すと実装が呼びます）。SIL Kit のポートの上で自分で組み立てるときは、`ParticipantRuntime::add_communication_ready_handler()`（か `set_communication_ready_handler()`）から呼びます。購読が確立する前に初期化すると、ほかの participant がその設定を受け取れないためです。

### 例外のメッセージ

例外のメッセージは `vehicle_sil::lin::LinMaster: `、`vehicle_sil::lin::LinSlave: `、`vehicle_sil::lin: `（スケジュール）で始まり、値と直し方を含みます。

## スケジュールテーブル（`LinMaster`）

エントリは ID、データ長、チェックサム、スロット長、publisher（`Slave` か `Master`）を持ちます。テーブルは巡回します。最初のヘッダーは T = 0 で送信します。

### エントリの条件（`validate_schedule()`）

次の条件を満たさなければ `std::invalid_argument` です。

| 対象 | 条件 |
|---|---|
| テーブル | 空でない |
| ID | 0〜63 |
| データ長 | 1〜8 |
| スロット長 | Master のステップ幅の正の倍数 |
| 同じ ID のエントリ | データ長・チェックサム・publisher が同じ |
| 診断フレーム（0x3C は Master、0x3D は Slave が送信する） | 8 バイトで Classic |

チェックサムモデル（`LinChecksumModel`）は Classic と Enhanced だけで、受信側はヘッダーの設定と比べます。

### ID の設定の変更

`LinMaster` に渡したすべてのテーブル（コンストラクターと `set_schedule()`）を通して、1 つの ID のデータ長・チェックサム・publisher は同じでなければなりません。違えば `set_schedule()` が `std::invalid_argument` を投げます。SIL Kit が、一度設定した ID の設定の変更を受け付けないためです。結果がポートによらないよう、仮想バスでも同じルールです。

### 結果を返す時点と切り替え

- ヘッダーの結果（Ok、NoResponse、Error とデータ）は、そのスロットの終わり、つまり次のヘッダーを送信するステップで返します。
- 結果には、ヘッダーを送信した時刻と publisher が付きます。
- `set_schedule()` は、次のスロット境界でテーブルを切り替え、新しいテーブルの先頭から始めます。
- 休止中に `set_schedule()` を呼ぶと、起床後の再開のときに切り替えます。

## フレームの送受信

### Slave が送信するフレーム

`LinSlave` は、ステップで `set_response(now, id, data)` された値のうち、ヘッダーの時刻より厳密に前に設定された最新の値で応答します。

Slave はヘッダーをポートから受け取り（`LinPortListener::on_header()`）、その中で応答します（`LinPort::respond()`）。SIL Kit のポートは dynamic response（`InitDynamic`、`AddFrameHeaderHandler`、`SendDynamicResponse`。SIL Kit v5.0.8 では experimental）を使います。

静的な TX バッファでは決定論的になりません。ヘッダーの時刻と Slave のステップが同じ時刻のとき、どちらが先に処理されるかが決まっていないためです（SIL Kit でも、仮想バスのステップの順でも）。

`LinSlave::clear_response(now, id)` を呼ぶと、now より後のヘッダーに応答しなくなります。

### Master が送信するフレーム

`LinMaster::set_frame_data(id, data)` の最新の値を、ヘッダーを送信するステップで送信します（SIL Kit の `MasterResponse`）。最初の設定までは 0 を送信します。`set_frame_data()` に渡せるのは、これまでに渡したテーブルで Master が送信する ID だけで、データ長も一致しなければなりません。

### 複数のノードの応答

| 場合 | 結果 |
|---|---|
| 1 つのヘッダーに 2 つ以上のノードが応答する（2 つの Slave、または Master が送信するフレームに応答する Slave） | 実際のバスと同じく衝突として Error を返し、データは付けない |
| 1 つの ID に、時間を分けて別々の Slave が応答する（`clear_response()` で応答をやめ、別の Slave が `set_response()` する。診断の 0x3D） | かまわない |

衝突の場合、応答はどれもスロットの終わりまでに受信されるので、結果は到着順によりません。

SIL Kit v5.0.8 では、2 つ目の Slave が応答したあと、その ID のヘッダーが送信されなくなっていました。パッチで修正しています（[dependencies.md](../dependencies.md)、「SIL Kit へのパッチ」）。

### Slave の受信

`LinSlaveOptions::subscriptions` に挙げた ID のうち、Master が送信するフレームを、`step()` から `FrameHandler` に渡します。時刻はヘッダーの時刻です。

### 制約: 応答する Slave のステップ幅（SIL Kit）

SIL Kit では、応答する Slave のステップ幅は、そのフレームのスロット長より厳密に小さくします。

Slave はヘッダーを受信ハンドラーの中で応答するので、応答はステップに結びつきません。Slave のステップ幅 D とスロット長 S の関係で、次のようになります。

| 条件 | ヘッダー T への応答 |
|---|---|
| D < S | Master のステップ T + S より前に必ず受信される |
| D ≥ S | 保証されず、NoResponse になるか、同じ ID の後のヘッダーへの応答として扱われることがある |

仮想バスでは、応答はヘッダーを送信した呼び出しの中で受信されるので、この制約はありません。両方で同じ結果にするには、SIL Kit の制約を守ります。

### 制限: Slave どうしの応答の受信（slave-to-slave）

Slave が、ほかの Slave の応答を受信すること（slave-to-slave）には対応していません。購読には Master が送信する ID だけを入れます。

仮想バスは、Slave の応答をほかの Slave に配信しません。

SIL Kit では、応答のタイムスタンプは応答した Slave の `Now()` で、ヘッダーの時刻ではありません。また、到着の上限は応答した Slave のステップ幅で決まり、受信側からはわかりません。SIL Kit のポートは、Master のフレームとほかの Slave の応答を区別できないので、購読した ID をほかの Slave が送信すると、そのフレームも渡しますが、時刻と渡すステップは決定論的ではありません。

Master が送信するフレームに Slave が応答した衝突（設定のエラー）では、ポートによって Slave に渡るものが違います。Master の結果（Error）はどちらも同じです。

| ポート | Slave に渡るもの |
|---|---|
| 仮想バス | どの Slave にもフレームを配信しない |
| SIL Kit | 購読した Slave に Master のフレームと応答の両方が渡ることがある |

## 診断フレーム（0x3C、0x3D）

LIN 2.x の診断用のフレームを、通常のフレームとは別のルールで扱います。LIN の診断（ISO 17987-2 のトランスポート層とサービス）そのものは実装しません。アプリケーションが、フレームの中身（NAD、PCI、SID とデータ）を組み立てて解釈します。

### マスターリクエスト（0x3C）

`LinMaster::send_master_request(data)` で 1 つ登録し、テーブルの次の 0x3C のスロットで 1 回だけ送信します。登録したリクエストがない 0x3C のスロットでは、ヘッダーを送信せず、結果も返しません。

- 前のリクエストを送信し終えるまでは、次のリクエストを登録できません（`false` を返します）。`master_request_pending()` で確認できます。
- 休止中や 0x3C のないテーブルの間は、登録したまま待ちます。
- 送信する前のリクエストは `cancel_master_request()` で取り消せます（診断のタイムアウトなど）。
- データは 8 バイトです。
- 先頭のバイト（NAD）が 0 のものは go-to-sleep コマンドなので受け付けません（`request_sleep()` を使います）。
- 0x3C に `set_frame_data()` は使えません。
- Slave は、0x3C を購読（`subscriptions`）すると、マスターリクエストを `FrameHandler` で受け取ります。go-to-sleep コマンドは渡しません。

### スレーブレスポンス（0x3D）

`LinSlave::set_slave_response(now, data)` で設定した 8 バイトで、now より後の最初の 0x3D のヘッダーに 1 回だけ応答します。そのあとは、次に設定するまで応答しません。0x3D のヘッダーは毎回送信され、応答する Slave がいなければ NoResponse です。

- 呼ぶたびに、1 回だけ送信する新しい応答になります。
- now より後のヘッダーに対しては、前に設定した応答を（送信したかどうかによらず）置き換えます。
- 送信する前の応答は `clear_response(now, 0x3D)` で取り消せます。
- 0x3C と 0x3D には `set_response()` を使えません。
- 全 Slave が 0x3D を共有します。どの Slave が応答するかは、アプリケーションが NAD で決めます。
- 2 つの Slave が同じヘッダーに応答すると Error です。

## 休止と起床（Sleep/Wakeup）

### 要求と動き

| 要求、条件 | 起きること |
|---|---|
| `LinMaster::request_sleep()` | 次のスロット境界で、次のヘッダーの代わりに go-to-sleep コマンド（ID 0x3C、`00 FF FF FF FF FF FF FF`）を送信し、休止する |
| `LinMaster::request_wakeup()`（休止中） | 次の `step()` で wakeup パルスを送信する |
| `LinSlave::request_wakeup()`（休止中） | 次の `step()` で wakeup パルスを送信する |
| Slave が送信した時刻 T の wakeup パルス | Master は、T より後の最初のステップで受け取って起床する |
| Slave が go-to-sleep コマンドを受信する | 休止する |
| バスが `LinSlaveOptions::bus_idle_timeout`（既定 4 s。`std::nullopt` で無効）のあいだ無通信 | Slave が休止する |
| Slave が wakeup パルスかヘッダーを受信する | 起床する |
| 状態に合わない要求（起きているときの wakeup、休止中の sleep） | 捨てる |
| 休止中に sleep と wakeup を同じステップで要求する | 起床だけが行われる |
| 起床してからスケジュールを再開するまでの間に sleep を要求する | 再開のスロット境界で、ヘッダーを送信せずに go-to-sleep コマンドを送信する |

`request_sleep()` で休止するとき、そこで終わったスロットの結果は通常どおり返します。

起床した Master は、スケジュールをテーブルの先頭から再開します。再開するのは、パルスの時刻から `LinMasterOptions::wakeup_delay`（既定 100 ms。LIN 2.x で Slave が準備を終えるまでの時間）以上あとの最初のステップです。

### 同じ時刻の go-to-sleep コマンドと wakeup パルス

同じ時刻の go-to-sleep コマンドと wakeup パルスでは、go-to-sleep が優先します。Master はその時刻のパルスでは起床せず、Slave はパルスのあとにコマンドを処理します。

### 休止中の Slave の応答

休止中の Slave も、ヘッダーには応答します。ブレークフィールドは物理的にも wakeup 信号になるためです。また、休止中は応答しないというルールにすると、ほかの Slave のパルスとヘッダーのどちらが先に受信されるかで結果が変わってしまいます。

### SIL Kit のコントローラーの状態

SIL Kit のポートでは、Master のコントローラーは go-to-sleep で Sleep になり、起床で Operational に戻ります（`send_wakeup()` は `Wakeup`、ほかのノードのパルスで起きたときの `resume()` は `WakeupInternal`）。

Slave の休止の状態は `LinSlave` の中だけにあり、ポート（SIL Kit のコントローラー）は常に Operational のままです。Sleep のコントローラーは go-to-sleep フレームも含めてフレームを捨てるので、コマンドを受け取ったかどうかが到着順で決まってしまうためです。SIL Kit の Slave のポートの `send_wakeup()` は、パルスを送信する直前にだけコントローラーを Sleep にします（`Wakeup()` が Operational のコントローラーに警告を出すため）。

## ポート（`LinPort`）と仮想 LIN バス（`VirtualLinBus`）

ポートは、ノードがバスを使う唯一のインターフェースです。役割（Master か Slave）はポートを作るときに決まり、ほかの役割の関数は `std::logic_error` です。

### ポートの関数

| 役割 | 関数 | すること |
|---|---|---|
| Master | `send_header(now, id, length, checksum)` | Slave が応答するフレームのヘッダーを送信する |
| Master | `send_frame(now, frame)` | ヘッダーと Master 自身の応答を送信する |
| Master | `send_go_to_sleep(now)` | go-to-sleep コマンドを送信する |
| Slave | `respond(header_time, frame)` | 応答する（`on_header()` の中からだけ） |
| 両方 | `send_wakeup(now)` | wakeup パルスを送信する |
| 両方 | `resume()` | ほかのノードのパルスで起きたときに呼ぶ（バスには何も送信しない） |
| 両方 | `open()` | `init()` が呼ぶ |
| 両方 | `set_listener()` | リスナーを設定する |

- 引数: `send_header()` と `send_frame()` は、ID が 63 を超えるか長さが 1〜8 の外なら `std::invalid_argument` です（両方の実装。`LinMaster` はそうした値を送信しません）。
- 時刻: `now` は呼び出したステップの時刻です。SIL Kit のポートは使わず、SIL Kit が participant の時刻を付けます。
- スレッド: SIL Kit のポートは、リスナーを SIL Kit の I/O スレッドから呼びます。仮想バスは、送信側の呼び出しの中で同期に呼びます。
- `set_listener()` は、イベントが来ない間（実行の前と後）にだけ呼びます。

### リスナーへの通知（`LinPortListener`）

リスナーはエンジンが実装します。

| 通知 | 通知先のポート | 内容 |
|---|---|---|
| `on_header(time, id)` | Slave だけ | Master が送信したすべてのヘッダー（Master が自分で応答するフレームも） |
| `on_response(header_time, frame, status)` | Master | 自分のヘッダーの結果 |
| `on_response()` | Slave | Master が応答したフレーム（Ok） |
| `on_go_to_sleep(time)` | Slave だけ | go-to-sleep コマンド |
| `on_wakeup(time)` | 両方 | ほかのノードのパルスだけ（自分のは来ない） |

Master のポートの `on_response()` が通知する結果は、次のとおりです。

| 結果 | 状態 |
|---|---|
| 自分のフレームが送信された | Ok |
| Slave の応答 | Ok とデータ |
| 応答なし | NoResponse（`send_header()` の中から同期で来ることがある） |
| 長さかチェックサムモデルの不一致 | Error |

Master のポートの `on_response()` は、1 つのヘッダーに 2 回以上来ることがあります（複数の応答）。衝突の判定は `LinMaster` がします。

Slave のポートの `on_response()` には、go-to-sleep コマンドと自分の応答は来ません。ほかの Slave の応答は、仮想バスは配信せず、SIL Kit のポートは区別できずに渡します（上の「制限: Slave どうしの応答の受信（slave-to-slave）」）。

### SIL Kit のポート（`SilKitLinPort`）

`SilKitLinPort(controller, role)` は、コンストラクターで SIL Kit のハンドラーを登録し、デストラクターで外します。コントローラーを所有しないので、participant より先に破棄します。型の変換（`LinFrame`、チェックサムモデル、状態）はこのポートの中だけにあります。

- `open()` は、Master では `Init`（フレームの応答の設定は空）、Slave では `InitDynamic` です。
- Slave のヘッダーのハンドラーだけは、`open()` の `InitDynamic` の後に登録します。SIL Kit の C API は、その前の登録を状態のエラーとしています。同じハンドラーの中なので、間にヘッダーは受信されません。
- `send_header()` は、その ID をまだ受信用に設定していなければ、ヘッダーの長さとチェックサムモデルで設定（`SetFrameResponse`）してからヘッダーを送信します。最初のスケジュールの ID も同じ経路で設定します。

### 仮想 LIN バス（`VirtualLinBus`）

`VirtualLinBus` はプロセス内の LIN バスです。Master 1 つと Slave をいくつでも接続できます（`attach(role, name)`。2 つ目の Master は両方の名前を示す `std::logic_error`）。エンドポイント（`VirtualLinBus::Endpoint`、`LinPort`）はバスが持ちます。スレッドもキューも持たず、すべてを送信側の呼び出しの中で、接続した順に配信します。

| 呼び出し | 配信 |
|---|---|
| `send_header()` | すべての Slave の `on_header()` を呼び、その中の `respond()` を集める |
| `send_frame()` | すべての Slave の `on_header()` を呼ぶ |
| `send_go_to_sleep()` | すべての Slave の `on_go_to_sleep()` を呼ぶ |
| `send_wakeup()` | 送信した以外のすべてのエンドポイントの `on_wakeup()` を呼ぶ |
| `open()`、`resume()` | 何もしない |

`send_header()` と `send_frame()` の結果は、Slave の応答で決まります。

| 呼び出し | 応答 | 結果 |
|---|---|---|
| `send_header()` | ない | Master に NoResponse を 1 回 |
| `send_header()` | ある | 応答ごとに 1 回（長さとチェックサムモデルがヘッダーと合えば Ok、合わなければ Error） |
| `send_frame()` | 応答した Slave がいない | Master と全 Slave に Ok でフレームを配信する |
| `send_frame()` | 応答した Slave がいる（衝突） | Master に自分のフレームと各応答を配信し（2 回以上）、Slave には配信しない |

Observer（`add_observer()`）は、エンドポイントに配信した後に、1 つのイベントを 1 回見ます（`LinBusEvent`: Frame、NoResponse、Collision、GoToSleep、Wakeup と、時刻、送信側の名前、フレーム）。同じ時刻に違う ECU が送信したイベントの順は、ステップの順です。

次の制限があります。

- エンドポイントは外せません。Master のノードを破棄しても、そのバスに新しい Master は接続できません。
- 仮想バスに故障注入（応答を落とす、壊す）はありません。

### 仮想 LIN バスの誤用と例外

| 誤用 | 例外 |
|---|---|
| `on_header()` の外からの `respond()` | `std::logic_error` |
| 1 回の `on_header()` の中での 2 回目の `respond()` | `std::logic_error` |
| リスナーや Observer の中からの送信（再入） | `std::logic_error` |
| 配信している間の `attach()` と `add_observer()` | `std::logic_error` |
| ヘッダーと違う ID や時刻、長さの誤った応答 | `std::invalid_argument` |

`std::logic_error` のメッセージは、エンドポイントの名前と直し方を含みます。

リスナーや Observer が投げた例外は、送信側の呼び出しに伝わります。それまでに配信したものは取り消さず、後ろのエンドポイントと Observer にはそのイベントが配信されません。バスはその後も使えます。

### 仮想 LIN バスの決定性

`StepDriver` は（開始時刻、登録順）でステップを実行するので、時刻 T のステップは、ほかのホストの T より前のステップがすべて終わってから実行されます。エンジンは「T より厳密に前」に設定された値と「now より前」のイベントだけを使うので、同じ時刻のステップの順は結果を変えません。SIL Kit の上と同じルールなので、結果も同じになります。テストで、同じ ECU のクラスを `Simulation()` と `Simulation(SilKitOptions{})` で動かし、結果、受信、状態の変化、統計が一致することを確認しています。

## 信号のエンコードとセンサーのピア

LIN の信号のエンコード（信号の値とフレームのデータの変換）と、センサーのピアを作るヘルパーはありません。アプリケーションがフレームのデータを組み立てます。追加するときは次を守ります。

- SIL Kit の LIN API を使い、SIL Kit が提供する機能は再実装しません。
- 次の要素は分けて扱います。
  - Master と Slave の役割
  - スケジュールテーブル
  - 信号のエンコード
- LIN Slave のセンサーピアは、センサー値を設定するだけで LIN の応答に反映されるようにします。

## 検証の範囲

### プロセス内のテスト

| テストの名前 | 動かす場所 |
|---|---|
| `lin.*` | SIL Kit のポートの上 |
| `lin_local.*` | `LocalBench` の仮想 LIN バスの上（主なシナリオを `lin.*` と同じ期待値で） |
| `lin_bus.*` | 仮想 LIN バスだけ |

確認していることは次のとおりです。

- 応答値がヘッダー時刻のルールに従い、壁時計の速度にも、同じ時刻のステップの順（ECU を追加した順）にもよらないこと
- Master が送信するフレームを 2 つの Slave が受信し、Slave の応答を Master が受信すること
- 休止と起床（Master の要求、Slave のパルス、bus idle、休止中のヘッダー、同時刻の go-to-sleep とパルス）、スケジュールの切り替え（休止中を含む）、応答の取り消し、データ長の不一致による Error
- 境界と異常系: ID 0 と 0x3F・データ長 1 と 8・長さの違うスロットの混在、同じスロットでの 2 回のスケジュール切り替え、チェックサムモデルの不一致（両方向）による Error、2 つの応答の衝突による Error（速度を変えても同じ）、時間を分けて同じ ID に応答する 2 つの Slave、既定の bus idle（4 s ちょうど）、同時刻の 2 つの wakeup パルス、`wakeup_delay` 0、Slave の受信キューのあふれ（数えられ、その後は回復する）、診断フレームの誤用の拒否
- 診断フレーム: マスターリクエストが 1 回だけ送信され、該当する Slave が 1 回だけ応答すること、応答の置き換え・取り消し・衝突、リクエストの取り消し、マルチフレームのやり取り（速度を変えても同じ）、休止中に登録したリクエストが再開後に送信されること
- クラスタのトレースが、Master や Slave の速度を変えても一致すること
- 仮想 LIN バスの配信（相手、順、同期の NoResponse、不一致、衝突、go-to-sleep、wakeup、Observer、誤用の例外）と、`Ecu::lin_master()`、`lin_slave()` の名前の検査と開始後の例外
- 同じ LIN の ECU のクラスを `Simulation()` と `Simulation(SilKitOptions{})` で動かし、記録が一致すること（長さの不一致による Error と、Master のフレームへの Slave の応答による衝突を含む。`simulation.*`）
- `vehicle_sil::lin` が SIL Kit を含まないこと（`lin.boundary`）

### プロセスを分けた統合テスト

LIN の統合テスト（`lin_cluster.*`）では、registry、`vehicle_sil_sim_controller`、Master（5 ms）、echo Slave（2 ms）、status Slave（4 ms）を別プロセスで動かします。

- スケジュールの時刻と順序、Master のフレームの内容、両 Slave の受信時刻、Slave の応答の内容、各ノードの休止と起床の時刻を照合します。
- echo Slave を実時間に結合した実行で、トレースがバイト単位で一致することを確認します。
- Master のステップ幅をスロット長と同じ 10 ms にした条件（`cluster-master-10ms`）でも確認します。
- 各 Slave がフレームを処理したステップもトレースに記録し、ヘッダーの直後のステップであることを照合します。

## 関連するドキュメント

- 使い方の例: [lin-guide.md](../lin-guide.md)
- 仮想時間の進み方と受信の順序: [virtual-time.md](virtual-time.md)
- 全体の形: [architecture.md](../architecture.md)
- どこに何があるか: [code-map.md](../code-map.md)
- ほかの内部の仕組みのドキュメント: architecture.md の「[中の仕組み](../architecture.md#中の仕組み)」
