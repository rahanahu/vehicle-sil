# LIN を仮想時間で動かす（`vehicle_sil::lin`）

vehicle-sil 自体を直す人と、LIN の Master と Slave の規則と制約を確かめたい人向けです。使い方の例は [lin-guide.md](../lin-guide.md) にあります。 先に [architecture.md](../architecture.md)（全体の形）と [code-map.md](../code-map.md)（どこに何があるか）を読みます。ほかの中の仕組みの文書は、architecture.md の「[中の仕組み](../architecture.md#中の仕組み)」にあります。

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

## 共通の規則

- Master も Slave も、`step(now)` をステップごとに 1 回、そのステップの時刻で呼びます。バスへの送信（ヘッダー、Master のフレーム、go-to-sleep コマンド、wakeup パルス）は、すべて `step()` の中で行います。要求（`set_frame_data()`、`send_master_request()`、`set_schedule()`、`request_sleep()`、`request_wakeup()`）は、同じステップで `step(now)` より前に呼べば、そのステップで反映されます。
- ステップ T で送ったものは、ほかのノードの T より後のステップまでに必ず届きます。仮想バスでは、送った呼び出しの中で届きます。SIL Kit では、Master が送るフレームは SIL Kit が 1 段遅れて送りますが、`ParticipantRuntime` がステップの完了をその後に回すので、同じ保証が成り立ちます（[virtual-time.md](virtual-time.md) の「参加と進み方」）。受信側は、受け取ったイベントを `StepInbox` にためて、`step(now)` で now より前のものだけを時刻順に処理します。そのため、状態の変化とフレームの受信は壁時計にも、同じ時刻のステップの順にもよりません。
- 作り方: `LinMaster(port, schedule, step, options)`（`step` はノードを回すステップ幅）、`LinSlave(port, options)`。ポートはノードより長く生きなければなりません。ノードはコンストラクタからデストラクタまでポートのリスナーです。ハンドラ（`LinMaster::on_result()`、`on_state()`、`LinSlave::on_frame()`、`on_state()`）は、最初のステップの前に設定します（スレッドセーフではありません）。空の関数は「なし」です。`Ecu` を通すと、ポートとステップ幅は実装が決めます。
- 状態の変化は `StateHandler(time, state)` に、時刻付きで通知します。Slave は受け取ったイベントの時刻（自分の操作ならそのステップの時刻）、Master は状態が変わったステップの時刻です。
- `init()`、`step()`、`request_*()` はノードのステップから呼びます。ポートはスレッドセーフではありませんが、SIL Kit はステップハンドラと受信ハンドラを同じ I/O スレッドで呼び、仮想バスは送った側のステップの中で配るので、ポートの呼び出しは 1 つのスレッドに収まります。`LinSlave::set_response()`、`clear_response()`、`set_slave_response()` は、どのスレッドから呼んでもかまいません。
- 両方の `init()` は、最初のステップの前に 1 回呼びます（`Ecu` を通すと実装が呼びます）。SIL Kit のポートの上で自分で組み立てるときは、`ParticipantRuntime::set_communication_ready_handler()` から呼びます。購読が確立する前に初期化すると、ほかの参加者がその設定を受け取れないためです。
- 例外の文言は `vehicle_sil::lin::LinMaster: `、`vehicle_sil::lin::LinSlave: `、`vehicle_sil::lin: `（スケジュール）で始まり、値と直し方を含みます。

## スケジュールテーブル（`LinMaster`）

- エントリは ID、データ長、チェックサム、スロット長、publisher（`Slave` か `Master`）を持ちます。テーブルは巡回します。最初のヘッダーは T = 0 で送ります。
- `validate_schedule()` の条件: 空でない、ID は 0〜63、データ長は 1〜8、スロット長は Master のステップ幅の正の倍数、同じ ID のエントリはデータ長・チェックサム・publisher が同じ、診断フレーム（0x3C は Master、0x3D は Slave が送る）は 8 バイトで Classic。満たさなければ `std::invalid_argument` です。チェックサムモデル（`LinChecksumModel`）は Classic と Enhanced だけで、受信側はヘッダーの設定と比べます。
- **ID の設定は変えられません**: `LinMaster` に渡したすべてのテーブル（コンストラクタと `set_schedule()`）を通して、1 つの ID のデータ長・チェックサム・publisher は同じでなければなりません。違えば `set_schedule()` が `std::invalid_argument` を投げます。SIL Kit が、一度設定した ID の設定の変更を受け付けないためです。結果がポートによらないよう、仮想バスでも同じ規則です。
- ヘッダーの結果（Ok、NoResponse、Error とデータ）は、そのスロットの終わり、つまり次のヘッダーを送るステップで返します。結果には、ヘッダーを送った時刻と publisher が付きます。
- `set_schedule()` は、次のスロット境界でテーブルを切り替え、新しいテーブルの先頭から始めます。休止中に呼ぶと、起床後の再開のときに切り替えます。

## フレームの送受信

- **Slave が送るフレーム**: `LinSlave` は、ステップで `set_response(now, id, data)` された値のうち、ヘッダーの時刻より厳密に前に設定された最新の値で応答します。
  - Slave はヘッダーをポートから受け（`LinPortListener::on_header()`）、その中で応答します（`LinPort::respond()`）。SIL Kit のポートは dynamic response（`InitDynamic`、`AddFrameHeaderHandler`、`SendDynamicResponse`。SIL Kit v5.0.8 では experimental）を使います。
  - 静的な TX バッファでは決定論的になりません。ヘッダーの時刻と Slave のステップが同じ時刻のとき、どちらが先に処理されるかが決まっていないためです（SIL Kit でも、仮想バスのステップの順でも）。
- **Master が送るフレーム**: `LinMaster::set_frame_data(id, data)` の最新の値を、ヘッダーを送るステップで送ります（SIL Kit の `MasterResponse`）。最初の設定までは 0 を送ります。`set_frame_data()` に渡せるのは、これまでに渡したテーブルで Master が送る ID だけで、データ長も一致しなければなりません。
- `LinSlave::clear_response(now, id)` を呼ぶと、now より後のヘッダーに応答しなくなります。
- 1 つのヘッダーに 2 つ以上のノードが応答すると（2 つの Slave、または Master が送るフレームに応答する Slave）、実際のバスと同じく衝突として Error を返し、データは付けません。応答はどれもスロットの終わりまでに届くので、結果は到着順によりません。
- 1 つの ID に、時間を分けて別々の Slave が応答するのはかまいません（`clear_response()` で応答をやめ、別の Slave が `set_response()` する。診断の 0x3D）。SIL Kit v5.0.8 では、2 つ目の Slave が応答したあと、その ID のヘッダーが送られなくなっていました。パッチで修正しています（[dependencies.md](../dependencies.md)、「SIL Kit へのパッチ」）。
- **Slave の受信**: `LinSlaveOptions::subscriptions` に挙げた ID のうち、Master が送るフレームを、`step()` から `FrameHandler` に渡します。時刻はヘッダーの時刻です。
- **制約**（SIL Kit）: 応答する Slave のステップ幅は、そのフレームのスロット長より厳密に小さくします。
  - Slave はヘッダーを受信ハンドラの中で応答するので、応答はステップに結びつきません。
  - Slave のステップ幅 D がスロット長 S より小さければ、ヘッダー T への応答は Master のステップ T + S より前に必ず届きます。D ≥ S では保証されず、NoResponse になるか、同じ ID の後のヘッダーへの応答として扱われることがあります。
  - 仮想バスでは、応答はヘッダーを送った呼び出しの中で届くので、この制約はありません。両方で同じ結果にするには、SIL Kit の制約を守ります。
- **制限**: Slave が、ほかの Slave の応答を受信すること（slave-to-slave）には対応していません。仮想バスは、Slave の応答をほかの Slave に配りません。SIL Kit では、応答のタイムスタンプは応答した Slave の `Now()` で、ヘッダーの時刻ではありません。また、到着の上限は応答した Slave のステップ幅で決まり、受信側からはわかりません。SIL Kit のポートは、Master のフレームとほかの Slave の応答を区別できないので、購読した ID をほかの Slave が送ると、そのフレームも渡しますが、時刻と渡すステップは決定論的ではありません。購読には Master が送る ID だけを入れます。Master が送るフレームに Slave が応答した衝突（設定の誤り）では、仮想バスはどの Slave にもフレームを配りませんが、SIL Kit では購読した Slave に Master のフレームと応答の両方が渡ることがあります。Master の結果（Error）はどちらも同じです。

## 診断フレーム（0x3C、0x3D）

LIN 2.x の診断用のフレームを、通常のフレームとは別の規則で扱います。LIN の診断（ISO 17987-2 のトランスポート層とサービス）そのものは実装しません。アプリケーションが、フレームの中身（NAD、PCI、SID とデータ）を組み立てて解釈します。

- **マスターリクエスト（0x3C）**: `LinMaster::send_master_request(data)` で 1 つ積み、テーブルの次の 0x3C のスロットで 1 回だけ送ります。積んだリクエストがない 0x3C のスロットでは、ヘッダーを送らず、結果も返しません。
  - 前のリクエストを送り終えるまでは、次のリクエストを積めません（`false` を返します）。`master_request_pending()` で確かめられます。休止中や 0x3C のないテーブルの間は、積んだまま待ちます。送る前のリクエストは `cancel_master_request()` で取り消せます（診断のタイムアウトなど）。
  - データは 8 バイトです。先頭のバイト（NAD）が 0 のものは go-to-sleep コマンドなので受け付けません（`request_sleep()` を使います）。0x3C に `set_frame_data()` は使えません。
- **スレーブレスポンス（0x3D）**: `LinSlave::set_slave_response(now, data)` で設定した 8 バイトで、now より後の最初の 0x3D のヘッダーに 1 回だけ応答します。そのあとは、次に設定するまで応答しません。0x3D のヘッダーは毎回送られ、応答する Slave がいなければ NoResponse です。
  - 呼ぶたびに、1 回だけ送る新しい応答になります。now より後のヘッダーに対しては、前に設定した応答を（送ったかどうかによらず）置き換えます。送る前の応答は `clear_response(now, 0x3D)` で取り消せます。
  - 0x3C と 0x3D には `set_response()` を使えません。
  - 全 Slave が 0x3D を共有します。どの Slave が応答するかは、アプリケーションが NAD で決めます。2 つの Slave が同じヘッダーに応答すると Error です。
- Slave は、0x3C を購読（`subscriptions`）すると、マスターリクエストを `FrameHandler` で受け取ります。go-to-sleep コマンドは渡しません。

## 休止と起床（Sleep/Wakeup）

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

## ポート（`LinPort`）と仮想 LIN バス（`VirtualLinBus`）

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

## 信号のエンコードとセンサーのピア

LIN の信号のエンコード（信号の値とフレームのデータの変換）と、センサーのピアを作る助けはありません。アプリケーションがフレームのデータを組み立てます。足すときは次を守ります。

- SIL Kit の LIN API を使い、SIL Kit が提供する機能は再実装しません。
- 次の要素は分けて扱います。
  - Master と Slave の役割
  - スケジュールテーブル
  - 信号のエンコード
- LIN Slave のセンサーピアは、センサー値を設定するだけで LIN の応答に反映されるようにします。

## 検証の範囲

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

プロセスを分けた統合テストでは、次のことを確かめています。

- LIN の統合テスト（`lin_cluster.*`）では、registry、`vehicle_sil_sim_controller`、Master（5 ms）、echo Slave（2 ms）、status Slave（4 ms）を別プロセスで動かします。
  - スケジュールの時刻と順序、Master のフレームの内容、両 Slave の受信時刻、Slave の応答の内容、各ノードの休止と起床の時刻を照合します。
  - echo Slave を実時間に結合した実行で、トレースがバイト単位で一致することを確かめます。Master のステップ幅をスロット長と同じ 10 ms にした条件（`cluster-master-10ms`）でも確かめます。各 Slave がフレームを処理したステップもトレースに記録し、ヘッダーの直後のステップであることを照合します。
