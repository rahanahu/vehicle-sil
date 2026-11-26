# CAN の共通基盤

[English](../../internals/can.md)

CAN のポート、ホスト、ドライバー、仮想バス、SIL Kit のポート、バスのタイミング、簡易 CAN Network Simulator について、仕様と、実装が守る保証と制限を書きます。CAN の上に自分のプロトコルを作るときに守るインターフェース（`Port`、`Steppable`）の仕様も、ここにあります。

| 節 | 書いてあること |
|---|---|
| [値の型](#値の型) | `Frame`、`TimedFrame` |
| [ポートの仕様（`Port`）](#ポートの仕様port) | 受信の順序、`send()` の結果、時刻の領域 |
| [ポートのイベント（`PortEvent`）](#ポートのイベントportevent) | 送信の結果、あふれ、バス状態の通知と、バックエンドごとの対応 |
| [`Steppable` の仕様](#steppable-の仕様) | プロトコルスタックを作るときに守る呼び出しの順序とルール |
| [生チャネル（`RawChannel`）](#生チャネルrawchannel) | アプリ向けの送信通知、トークンのルール、`outstanding` の保存則、容量オプション |
| [ホスト（`Host`）](#ホストhost) | 1 ステップの段階、失敗状態、`post()`、時間の領域 |
| [仮想時間での駆動](#仮想時間での駆動) | `StepDriver`、`bind_host()` |
| [実時間での駆動（`RealTimeDriver`）](#実時間での駆動realtimedriver) | ステップを実行する条件、到着時刻、終了の順序 |
| [仮想バス（`VirtualCanBus`）](#仮想バスvirtualcanbus) | タイミングの有無、故障注入、通信断 |
| [生フレームの観測（`TapPort`）](#生フレームの観測tapport) | 通知する内容と順序、リスナーの制約 |
| [SIL Kit のポート（`SilKitCanPort`）](#sil-kit-のポートsilkitcanport) | 開始と終了、時刻、送信確認、オプション |
| [バスのタイミング（`CanBusModel`）](#バスのタイミングcanbusmodel) | 送信時間、調停、模擬しないもの |
| [簡易 CAN Network Simulator](#簡易-can-network-simulatorvehicle_silcan_netsimvehicle_sil_can_bus_simulator) | 時刻と決定論、ビットレート、コントローラーのモード、設定、既知の制限 |
| [ソース互換性と ABI](#ソース互換性と-abi) | 追加によるソース互換性と、バイナリ配置・vtable の変更 |

`vehicle_sil::can` と `vehicle_sil::can_silkit` は、J1939 スタック（`vehicle_sil::j1939`）の基盤です。CAN の 11 ビットと 29 ビットのフレームを扱い、J1939 以外のプロトコルも載せられます。API はまだ安定していません。

`vehicle_sil::can` は SIL Kit と Vector XL に依存しません。SIL Kit をリンクしない単体テストの実行ファイルをビルドすることで、毎回確認します。

ステップを実行する仕組み（ホスト、ドライバー、時計、時間の領域）は、バスの種類によらない `vehicle_sil::step` にあります。`vehicle_sil::can` の `Host` はその上に作ってあり、`StepDriver`、`RealTimeDriver`、`RealTimeClock`、`TimeDomain` などは `vehicle_sil::step` のものの別名です（どちらの名前でも同じ型です）。

時刻はすべて `SimTime` です。プロトコル処理はスレッドを持たず、時計を読みません。時刻は、仮想時間では SIL Kit または `StepDriver` の時刻、実時間では `RealTimeDriver` の時計（開始からの経過時間）です。

## 値の型

- `Frame` はクラシック CAN のデータフレームです（RTR、CAN FD、CAN XL は表しません）。
  - `length` はバイト数（0〜8）で、DLC の符号ではありません。
  - 比較は ID、形式（11 ビット / 29 ビット）、`length` までのデータで行います。
- `TimedFrame` は時刻付きのフレームで、時刻は、受信ではバックエンドが付けた時刻、送信では送信するステップの時刻です。

## ポートの仕様（`Port`）

### 受信

- `release_before(now, sink)` は、時刻が `now` より前の受信フレームを、時刻順、同じ時刻なら到着順に渡します。
- 同じ時刻のフレームを並べ替えず、最終的な順序はプロトコル処理が決めます。
- 時刻がちょうど `now` のフレームは次の呼び出しで渡します。
- 自分が送信したフレームは受信しません（エコーと送信の確認はポートで捨てます）。

### 送信

`Port` は送信完了の通知能力を `transmit_capabilities()`（`TransmitCapabilities`）で返します。

| 値 | 意味 |
|---|---|
| `None`（既定） | バックエンドがトークン対応のフレーム単位の送信結果を返さない。 |
| `Correlated` | `Accepted` になった各送信が、トークンと対応付いた `TransmitCompleted` または `TransmitFailed`（あるいは `EventOverflow`、または終了時の `outstanding` 残留による検知）を生む。 |

`send()` には 2 つのオーバーロード（`send(const TimedFrame&)` と `send(const TimedFrame&, std::uint64_t token)`）があり、既定の 2 引数版は `token` を無視して 1 引数版の `send()` を呼びます。`token` はポート単位でスコープされる不透明な呼び出し元識別子であり、`0` はセンチネルではなく通常の有効なトークンです。

`send()` は同期の結果を返します。

| 結果 | 意味 |
|---|---|
| `Accepted` | 受け付けた。送信の完了とは厳密に区別され、送信したステップ内で完了イベントを出すことはない。 |
| `QueueFull` | 送信キューが満杯で、後のステップで（同じトークンで）送信し直せる。 |
| `Failed` | 送信できないフレームか、バックエンドのエラー。 |

`send()` の後にフレームがどうなったか（送信された、バックエンドが捨てた）をバックエンドが知るときは、後からイベント（`PortEvent`）で通知します。`release_events_before(now, sink)` は、時刻が `time < now` のイベントだけを、時刻順、同じ時刻なら到着順に渡します。完了や失敗の時刻が `time == T`（タイミングなしの仮想バスや SIL Kit の簡易シミュレーションなど）であってもステップ `T` では解放されず、より後のステップ `T' > T` で解放されます。既定の `Port` 実装はイベントを出さず、`TransmitCapabilities::None` のポートは `Accepted` から `TransmitCompleted` を捏造しません。

### スレッドと時刻の領域

`release_before()`、`release_events_before()`、`send()` は、ドライバーのスレッドだけから呼びます。

ポートは時刻の領域を持ちます。

| 領域 | 時刻 | 該当するポート | `set_wakeup()` |
|---|---|---|---|
| `Virtual` | 仮想時間 | 仮想バス、時刻同期ありの SIL Kit | 無視する。 |
| `RealTime` | 実時間ドライバーの時計 | 時刻同期なしの SIL Kit、Vector XL | 到着のたびに、渡された関数を到着時刻付きで呼び、ドライバーを起こす。 |

`RealTime` のポートは、受信したフレームとイベントに、ドライバーと共有する時計で到着時刻を付けます（下の「実時間での駆動」）。

## ポートのイベント（`PortEvent`）

ポートで起きたことを、後から知らせます。自分が送信したフレームのその後（`send()` の同期の結果の後に分かったこと）と、受信キューとコントローラーの状態です。

| 種類 | 意味 | 時刻 |
|---|---|---|
| `TransmitCompleted` | フレームが送信された | バスを模擬するバックエンドでは送信の終わり、それ以外ではバックエンドが送信を確認した時刻 |
| `TransmitFailed` | `send()` が受け付けたフレームを、バックエンドが送信しなかった（送信キューの満杯、取り消し、バスから切断されている） | バックエンドが通知した時刻 |
| `ReceiveOverflow` | 受信キューが満杯で受信フレームを捨てた（`count` は捨てた数。まとめた中に数の分からないあふれが 1 つでもあれば、合計が分からないので 0） | 捨てたフレームのうち最も早い時刻 |
| `BusStateChanged` | コントローラーのエラー状態が `bus_state`（`ErrorActive`、`ErrorPassive`、`BusOff`）になった | 変わった時刻 |
| `ErrorFrame` | バスでエラーフレームを見た | 見た時刻 |
| `EventOverflow` | イベントキューが満杯で送信確認を捨てた（`count` は捨てた数） | 捨てたイベントのうち最も早い時刻 |

- 送信のイベントは、CAN 識別子（`id`）、`send_time`（分かれば送信ステップの `TimedFrame::time`）、`token`、`correlated` を持ち、データは持ちません。
- `correlated`（`bool`、既定 `false`）が対応付きイベントかどうかを明示的に区別します（これにより `token == 0` を非対応イベントと混同せず、`FaultAction::Duplicate` などの複製フレームで 2 重の完了を発火しません）。
- `ReceiveOverflow` と `EventOverflow` は `OverflowCollector` が管理します。どちらもイベントキューの枠を消費せず、キューが満杯でも送信確認を押し出さずに解放され、`OverflowCollector::release_merged` が各あふれを `event.time > overflow.time` となる最初のキュー済みイベントの直前に差し込みます。
- `ReceiveOverflow` は捨てたフレームごとには出さず、ポートが次にイベントを渡すまでに捨てた数を、1 つのイベントにまとめます。
- バックエンドが知り得ない種類は出しません（報告なし）。
- 種類はこれから増えることがあり、`Steppable` は知らない種類を無視します。

### バックエンドごとのイベント

| バックエンド | `transmit_capabilities()` | 出すイベント |
|---|---|---|
| `VirtualCanBus` | `Correlated` | `TransmitCompleted`（タイミングありは `tx.end`、タイミングなしは `frame.time`）、`TransmitFailed`（故障注入と通信断）、`ReceiveOverflow`、`EventOverflow`、注入した任意のイベント |
| `SilKitCanPort`（仮想時間） | `Correlated` | SIL Kit の送信確認からの `TransmitCompleted` と `TransmitFailed`、`EventOverflow`、SIL Kit のエラー状態の通知からの `BusStateChanged`（ネットワークシミュレーターは出し、SIL Kit の簡易シミュレーションは出さない）、`ReceiveOverflow` |
| `SilKitCanPort`（実時間） | `None` | 非対応の `TransmitCompleted` / `TransmitFailed`、`BusStateChanged`、`ReceiveOverflow` |
| `ExternalPortAdapter`（Vector XL） | `None` | `ErrorFrame` と、外部ポートのあふれを表す数の分からない `ReceiveOverflow`（バス状態と送信の結果は出さない） |

### イベントの渡し先

`Host` は、ステップの段階 2 で、そのステップのフレームより先にイベントを `Steppable::on_port_event()` に渡します。`on_port_event()` は記録だけにします。送信はしません（送信は `flush()` で行います）。

J1939 スタックは、イベントを `NetworkStatistics`（`frames_transmitted`、`transmit_failures`、`port_receive_overflows`、`port_overflow_events`、`bus_state_changes`、`error_frames`）に数え、最新のバス状態を `Network::bus_state()` に保つだけです。プロトコルの処理には使いません（バスオフでも動作を変えません）。

## `Steppable` の仕様

CAN の上にプロトコルスタックを作るときの拡張点です。J1939 の `Network` はこの仕様で作っています。

### 呼び出しの順序

最初のステップの前に、ホストは `start(step_size)` を 1 回呼びます。`can::Host::add(Steppable&, Port&)` で組を登録すると、`RawChannel` などが `start()` 前の構築段階で `transmit_capabilities()` を参照できるよう、`Steppable` の `bind_port(const Port&)` も呼びます。1 ステップでは、次の順で呼びます。

| 順 | 呼び出し | `Steppable` がすること |
|---|---|---|
| 1 | `begin(now)` | — |
| 2 | ポートのイベントごとの `on_port_event()` | 記録だけにする。 |
| 3 | フレームごとの `receive()` | 記録だけにする。 |
| 4 | `process()` | 処理する。 |
| 5 | （アプリの処理の後に）`flush(port)` | ポートに送信する。 |

### 守るルール

- `now` は呼び出しごとに増えます。
- `Steppable` はスレッドを持たず、時計を読まず、使う時刻は `now` と、フレームとイベントの時刻だけです。
- 呼び出しは常に 1 つのスレッド（ドライバーのスレッド）からで、ロックは要りません。
- `receive()` に渡るフレームの時刻は `now` より前です。
- 同じ時刻の、別の送信側のフレームの順序はバックエンドによって違うので、`Steppable` が決まったルールで並べます。
- `start()` は、そのプロトコルが動けないステップ幅なら例外を投げます（ホストが失敗状態になります）。
- 実時間では、ドライバーの `max_idle` がステップ幅として `start()` に渡ります。
- 例外を投げると、ホストは失敗状態になり、以後のステップはすべて失敗します。

### `next_deadline()`

`next_deadline()` は、何も受信しなくてもステップが要る最も早い時刻（タイマーの期限）を返します。実時間のドライバーはそれまで眠ります。

タイマーのない処理（`QueueFull` で残したフレーム、1 ステップの上限で繰り越したフレーム）は含めません。それらは実時間ではドライバーの `max_idle` の後のステップで送信されます。

ステップの後も過去の期限を返し続ける実装は、この仕様に反します（手動の時計では次のステップを 1 ns 後に 1 回実行するだけですが、壁時計ではステップを実行し続けます）。

## 生チャネル（`RawChannel`）

`RawChannel` は、プロトコルスタックを持たずに CAN の 11 ビットと 29 ビットの生フレームを送受信するアプリケーション向けの `Steppable` です。セットアップ（`on_frame()`、`on_transmit()`）は `start()` の前だけ許されます。空のハンドラーを渡すと `std::invalid_argument`、`start()` の後に呼ぶと `std::logic_error` です。

### 送信、トークン、送信通知

- `send(frame)` と `send(frame, token)` はステップの中（`begin()` から `flush()` まで）だけで呼べます。ステップの外から呼ぶと `std::logic_error` です。
- `token`（`std::uint64_t`）はチャネル単位でスコープされる不透明な値であり、`0` はセンチネルではなく通常の有効なトークンです。
- 同期の `RawSendStatus::QueueFull`（そのステップの `send_capacity` 到達）と `RawSendStatus::InvalidFrame` は受け付けられなかった送信です。`sends_rejected` を増やし、`TransmitEvent` を出さず、`outstanding` も変えません。呼び出し元は後のステップで同じトークンをそのまま再試行でき、二重送信や二重完了になりません。
- `TransmitCapabilities::Correlated` のバックエンド（`transmit_capabilities()` は結び付けられたポートを参照する）で `send()` が `Ok` を返すと、そのトークンは完了結果に至るまで送信中（`outstanding`）に留まります。完了結果は `TransmitEvent::Kind::Completed` または `Failed` としての配送、`EventOverflow`（`port_event_overflows`）または `transmit_event_overflows` による計上、あるいは `close()` / 終了後の `outstanding` 残留の観測です。完了結果の前に同じトークンを再利用すると、送信中の 2 つのイベントを呼び出し元から見分けられなくなります。
- `TransmitCapabilities::None` のバックエンドでは、ポートが `Accepted` を返した送信は `outstanding` を増やさず、`Completed` イベントも生成しません。

### 完了タイムスタンプとステップ内順序

`TransmitEvent` は `kind`（`Completed` または `Failed`）、`token`、`id`、`send_time`（送信元ステップ時刻 `T`）、`time` を持ちます。

| 発生元 | `send_time` | `time` |
|---|---|---|
| タイミングあり仮想バスまたはネットワークシミュレーター（タイミングあり `VirtualCanBus`、`CanBusSimulator`） | `T` | バスまたはシミュレーターの完了・失敗時刻（`tx.end` またはシミュレーターの時刻） |
| タイミングなし `VirtualCanBus` または SIL Kit 簡易シミュレーション | `T` | `T` |
| `flush()` での拒否（`ecu.connected == false`、ポートの `QueueFull`、ポートの `Failed`） | `T` | `T` |

- `Accepted`（`send() == Ok`）と送信完了は厳密に分離されます。`on_transmit` がステップ `T` の中（`send()` の中や `flush()` の中）で呼ばれることはありません。
- ステップ `T` の `send()` で受け付けられた後に `flush()` で拒否されたフレームは、`frames_dropped` を増やし、事前確保された `flush_failed_` 領域（容量 `send_capacity`）に `TransmitEvent{Failed, token, id, T, T}` を追加します。
- ステップ `T' > T` の中では、`TransmitEvent` の通知（`release_events_before(T')` が解放した対応付きポートイベントと `fail.time < now_` の `flush()` 拒否の双方）は結果のタイムスタンプ `time` の単調順に並びます（たとえばタイミングありバスでは `time == T` の `flush()` 失敗が `tx.end > T` のタイミング付き完了より先になります）。同一タイムスタンプの `send()` 呼び出し順は保証されず、呼び出し元は `token` で結果を対応付けます。
- ドライバーのスレッド上の `process()` の中では、`RawChannel` は `transmit_completed` / `transmit_failed` の更新と `outstanding` の減算を先に行ってから `on_transmit(event)` ハンドラーを呼びます（`on_transmit` ハンドラーが未登録でも取り出しと計数は行います）。同じ `process()` 呼び出しの中で、すべての `on_transmit` ハンドラーは `on_frame` ハンドラーより先に、同じステップ・スレッドのルールで実行されます。
- すべてのバッファ（`received_`、`outgoing_`、`flush_failed_`、`transmit_events_`）は生成時に確保され、ステップ内ではヒープ確保を行いません（`can.steps_do_not_allocate`）。

### `outstanding` の保存則とあふれの計上

`Correlated` バックエンド上のチャネルでは、すべての時点（`send()` 直後のステップ途中、ステップ間、あふれ発生後、`close()` / 終了後）で次の保存則が成り立ちます。

```text
accepted_by_channel == in_step_outgoing + frames_sent + frames_dropped
accepted_by_channel == transmit_completed + transmit_failed
                     + transmit_event_overflows + port_event_overflows
                     + outstanding
```

- `EventOverflow(count)` は `port_event_overflows += count` を増やし、`outstanding` を `min(outstanding, count)` 減らします。
- チャネル内部の `transmit_events_` バッファのあふれは `transmit_event_overflows` を増やし、捨てたイベント 1 件につき `outstanding` を `1` 減らします。
- `TransmitCapabilities::None` バックエンドでは、`flush()` での拒否だけが次のステップの `process()` で `Failed` として通知されるまで `outstanding` を増やします。
- **防止と回復契約（`EventOverflow`）**: `port_event_capacity` と `transmit_event_capacity` を既定の `0` のままにして（`max(1024, 2 * send_capacity, port_transmit_queue_capacity + send_capacity)` 以上へ自動サイジングさせる）`port_transmit_queue_capacity > 0` を設定すると、`EventOverflow` は構造上防げます（超過バーストは決定的に `QueueFull` または次ステップの `Failed` を受けます）。`EventOverflow` と `transmit_event_overflows` は失われた `token` 値ではなく件数だけを報告するため、`TxConfirmation` 駆動の ECU やテストハーネスは個別の送信バッファを選択的に解放できず、`port_event_overflows > 0 || transmit_event_overflows > 0` を回復不能な確認欠落エラーとして扱う必要があります（`ReceiveOverflow` と同じく、ステップ／実行を失敗させるかチャネル／コントローラーの TX 状態をリセットします）。

### 容量オプション（`RawChannelOptions`）

| オプション | 既定 | 意味 |
|---|---|---|
| `send_capacity` | `64` | 1 ステップで `send()` が受け付ける最大フレーム数（`> 0`）。 |
| `receive_capacity` | `1024` | 1 ステップで保持する最大受信フレーム数（`> 0`）。 |
| `transmit_event_capacity` | `0`（導出） | 未配送 `TransmitEvent` 用のチャネルバッファ容量。`0` のときは `max(1024, 2 * send_capacity, port_transmit_queue_capacity + send_capacity)`。 |
| `port_inbox_capacity` | `0`（導出） | ポート受信キュー容量（`RawChannel::port_inbox_capacity()` は `0` のとき `2 * receive_capacity` を返す）。 |
| `port_event_capacity` | `0`（導出） | ポートイベントキュー容量（`RawChannel::port_event_capacity()` は `0` のとき `max(1024, 2 * send_capacity, effective_transmit_event_capacity, port_transmit_queue_capacity)` を返す）。 |
| `port_transmit_queue_capacity` | `0`（バックエンド既定） | ポートで許容する未確認送信の上限（`> 0` で送信中フレーム数を制限）。 |
| `port_bitrate` | `0`（未設定） | CAN ビットレート（bit/s。`Peer` は `CommunicationReady` で `SetBaudRate()` するため `SilKitCanPortOptions::bitrate` へ渡す。`LocalBench` では未使用）。 |

`RawChannel::validate(options)` は、`send_capacity == 0`、`receive_capacity == 0`、または `port_event_capacity > 0 && port_transmit_queue_capacity > port_event_capacity` の場合に `std::invalid_argument` を投げます。

## ホスト（`Host`）

ホストには 2 つの層があります。

- `vehicle_sil::step::Host` は、ステップの参加者（`step::Member`）を登録順に持って段階ごとに進め、バスの種類を知りません。
  - `Member` は 1 つのチャネルのプロトコル処理とそのポートの組です。
  - `Member` は `begin(now)`、`receive(now)`（ポートから `now` より前のものを受け取る）、`process()`、`flush()`、`start(step_size)`、`next_deadline()`、`time_domain()`、`set_wakeup()` を持ちます。
- `vehicle_sil::can::Host` は `step::Host` の派生で、`add(Steppable&, Port&)` で渡された組を中で `Member` にします。
  - `receive(now)` は、ポートのイベントを `on_port_event()` に、フレームを `receive()` に渡します。
  - 組の入れ物はホストが持ちますが、`Steppable` とポートは持ちません。

以下は CAN の組で見た仕様です。

### 1 ステップの段階

ホストは、1 つの participant（またはテストのループ）の、プロトコル処理（`Steppable`）とポートの組を登録順に持ちます。1 ステップは、全部の組に対して段階ごとに進めます。

| 段階 | すること |
|---|---|
| 1 | `begin(now)` |
| 2 | ポートが渡すイベントの `on_port_event()` とフレームの `receive()` |
| 3 | `process()` |
| 4 | アプリの処理を 1 回 |
| 5 | `flush(port)` |

ステップの時刻は前のステップより大きくなければなりません。

### 失敗状態

`step()` から例外が出ると（プロトコル処理、アプリ、ポートのどれからでも、使い方のエラーでも）、ホストは失敗状態になり、例外を呼び出し元に返します。以後の `step()` はすべて `std::logic_error` です。回復するには作り直します。

### 登録

登録は最初のステップの前だけです。ホストは `Steppable` とポートを所有しません。

wakeup の関数が設定された後の登録では、ホストがポートの `set_wakeup()` を呼びます。それが例外を投げると、組は登録されずに例外が返ります（同じ組をもう一度登録できます）。

### ステップ幅

`set_step_size()` で受け取ったステップ幅を、最初のステップの `begin()` の前に、各 `Steppable` の `start()` に登録順で 1 回渡します（設定がなければ値なし）。`StepDriver::add()`、`bind_host()`、`RealTimeDriver` が設定します。`start()` が例外を投げると、ホストは失敗状態になります（そのプロトコルが動けないステップ幅の設定エラー）。

### `post(fn)`

ステップの外（別のスレッド、SIL Kit の他のハンドラー）からプロトコルスタックに処理を渡します。

- どのスレッドからでも呼べます。
- `fn` は次のステップの段階 4 の最初（アプリの処理の前）に、渡された順でドライバーのスレッドで実行します。
- ステップの中で渡した処理は次のステップで実行します。
- `fn` の例外はアプリの例外と同じくホストを失敗させ、失敗したホストに渡した処理は実行しません。

仮想時間でも使えますが、どのステップで実行されるかは `post()` を呼んだ時点で決まります。決定的に扱いたいデータは `StepInbox` で受け取り、ステップの中で処理します。

### 時間の領域

1 つのホストのポートは同じ領域でなければなりません。ドライバーは自分の領域を要求します（`StepDriver` と `bind_host()` は `Virtual`、`RealTimeDriver` は `RealTime`）。混在は登録の時点で `std::logic_error` です（例: Vector XL のポートを SIL Kit の時刻同期で駆動するホストに入れる）。

### `next_deadline()`

ホストの `next_deadline()` は全 `Steppable` の `next_deadline()` の最小です。

## 仮想時間での駆動

- `StepDriver`（`vehicle_sil::step`）は、SIL Kit を使わずに複数のホストを仮想時間で進めます。
  - 各ホストのステップ幅は別々でよく、ステップは（開始時刻、登録順）の順に実行します。
  - この順は、SIL Kit の時刻同期が許す順序の 1 つです。
- テストのループでの終了の順序: そのホストを進める `StepDriver` を使うのをやめる → プロトコル処理とホストを破棄 → ポート（仮想バスが所有）を破棄。
- SIL Kit では `bind_host(runtime, host)` を使い、`ParticipantRuntime` のステップハンドラーから `host.step(now)` を呼びます。
  - 仮想時間を使わない `ParticipantRuntime` には結び付けられません。
  - 本体は `vehicle_sil::core` の `vehicle_sil::bind_host()`（`step::Host` を受け取る）で、`vehicle_sil::can::bind_host` は同じ関数です。

## 実時間での駆動（`RealTimeDriver`）

`RealTimeDriver` と時計は `vehicle_sil::step` のもので、どの `step::Host` も駆動できます。時刻同期なしの SIL Kit や Vector XL のポートを持つホストを、壁時計で駆動します。決定論はありません。壁時計のテストでは、到着と順序だけを確認します。

### スレッド

participant ごとに 1 つ、スレッドを 1 本持ちます。プロトコルスタックにはロックがなく、アクセスするのはこのスレッドだけです。アプリはホストの `post()` とアプリの処理で関わります。

ステップが例外を投げると、スレッドは止まり、`failed()` と `error()` で理由が分かります（ホストも失敗状態）。

### 時計

時計（`RealTimeClock`）は差し替えられます。ドライバーとホストのポートは同じ時計を使います。

| 用途 | 時計 |
|---|---|
| 製品 | `SteadyRealTimeClock`（生成時からの経過時間） |
| テスト | `ManualRealTimeClock`（`advance_to()` で進めるとドライバーを起こす） |

`wait_idle(timeout)` は、スレッドがすることなく眠るまで待ちます。`ManualRealTimeClock` と組み合わせると、`advance_to()` の後に、その時刻までのステップが終わったことを確認できます。

### ステップを実行する時点

登録（`Host::add()`、`set_application()`）は `start()` の前に済ませます。ステップを実行するのは、次の時点です。

- 開始の直後（`start()` の直後に、ドライバーのスレッドで最初のステップを実行します）
- ポートへの到着や `post()` のとき
- ホストの `next_deadline()` の 1 ns 後（期限より後で発火するタイムアウトのため）
- 前のステップから `max_idle`（既定 10 ms）が過ぎたとき

### ステップの時刻と到着時刻

ステップの時刻は時計の値ですが、前のステップより少なくとも 1 ns 後、起こした到着の時刻より少なくとも 1 ns 後にします。時計が進んでいなくても、受信したフレームは次のステップで必ず渡ります。

到着時刻（`RealTimeReceiver`）は、ロックの中で、時計の値、前回渡した `now`、前の到着時刻 + 1 ns の最大を付けます。処理済みのステップより前の時刻にはならず、到着の順も保たれます。

### 終了の順序

1. `RealTimeDriver` を止める（`stop()` か破棄。ホストとポートから wakeup の関数を外し、スレッドを join する）。
2. ポートを閉じる。
3. プロトコルスタックとホストを破棄する。
4. `ParticipantRuntime`（またはドライバー）を破棄する。

`stop()` をステップの中から呼んだ場合は、スレッドに終わるよう伝えるだけで、join は後の `stop()` で行います。

## 仮想バス（`VirtualCanBus`）

`attach()`（およびエンドポイントごとに `inbox_capacity`、`event_capacity`、`transmit_queue_capacity` を上書きする `attach(VirtualCanBusEndpointOptions)`。`0` は `VirtualCanBusOptions` を継承し、`transmit_queue_capacity > event_capacity` のとき `attach()` は `std::invalid_argument` を投げる）が、バス上の 1 つの CAN コントローラーにあたるポートを作ります。`Endpoint::transmit_capabilities()` は `TransmitCapabilities::Correlated` を返し、`Endpoint::outstanding()` は終端のポートイベントがまだ解放（または `EventOverflow` として計上）されていない受け付け済みフレーム数を返します。仮想バスは 1 つのスレッドから使います。

### タイミングの有無

| 設定 | フレームの扱い |
|---|---|
| タイミングなし（既定） | 時刻 t で送信したフレームを、他の全ポートが時刻 t のまま受信し（調停と送信時間はない）、送信したポートには対応付きの `TransmitCompleted` イベント（`time = t`、`send_time = t`、`token`、`correlated = true`）が積まれる（より後のステップ `T' > t` で解放）。 |
| タイミングあり（`VirtualCanBusOptions::timing`） | フレームは下の「バスのタイミング」のルールでバスに出て、他のポートは送信の終わりの時刻でフレームを、送信したポートは同じ時刻の対応付き `TransmitCompleted` を受信する。 |

`QueueFull` を決める送信キュー容量には 2 つの異なる設定があります。

- **バスモデルの FIFO 容量（`VirtualCanBusTiming::transmit_queue_capacity`、既定 `64`）**: `CanBusSimulatorOptions::transmit_queue_capacity` に相当します。タイミングありで、送信してバスを決めた時点でまだ送信が始まっていないフレームが送信側ごとの FIFO 上限に達すると、`send()` は `QueueFull` を返します（`Host` は送信する前に同じステップの受信を取り出すので、ステップの時刻の状態です）。
- **エンドポイントごとの未確認フレーム上限（`VirtualCanBusEndpointOptions::transmit_queue_capacity`、既定 `0` = 無制限）**: `> 0` のとき、タイミングの有無、通信断、`FaultAction::TransmitFailed` にかかわらず、`outstanding_ >= limit` であれば `Endpoint::send()` は `QueueFull` を返します。

エンドポイントの対応付きイベントキューがあふれた場合は、捨てた対応付き送信確認を数えて `OverflowCollector` 経由で `PortEvent::Kind::EventOverflow` を出します。非対応イベント（`FaultAction::Duplicate` の複製側や `inject_event()`）は `release_events_before()` 時に時刻順でマージされる別の `event_capacity` キューを使うため、対応付き送信完了が非対応イベントと枠を奪い合うことはありません（`transmit_queue_capacity > 0` かつ `transmit_queue_capacity <= event_capacity` とすることで対応付きの `EventOverflow` を構造上防げます）。

タイミングありのバスは遅れて決めます。どれかのポートが `now` より前のフレームかイベントを取り出すとき、`now` より前に始まる送信をすべて決めます。

### 送信できる時刻

すでに決まった時刻より古いフレームは送信できません（`std::logic_error`）。「決まった時刻」は、タイミングなしでは受信側がすでに受け取った時刻、タイミングありでは決めたバスの時刻です。このエラーは、ホストを時刻順に進めなかったことを示します。`StepDriver` は常に時刻順に進めます。

### 受信キューのあふれ

各ポートの受信キューは容量が固定です。入らないフレームは捨てて数えます（`overflows()`）。あふれは `ReceiveOverflow` でも通知します。仮想バスではあふれも決定的ですが、フレームが失われるので、テストではあふれがないことを確認します。

### 故障注入（テスト用）

`Endpoint::set_fault_injector(f)` で、そのポートが送信するフレームごとに `f` が `FaultAction` を返します。不正なフレームは `f` を呼ぶ前に `Failed` です。`f` は送信した順に 1 回ずつ呼ばれるので、決定的な関数なら結果も決定的です。

| `FaultAction` | `send()` の結果と配送 | タイミングありでは |
|---|---|---|
| `Deliver` | 通常どおり。 | — |
| `Drop` | `Accepted` を返して、バスの他のポートに配送しない。 | フレームはバスを占有してからどこにも配送されない（送信したポートは `TransmitCompleted` を受信する）。 |
| `Duplicate` | 同じ時刻で 2 回配送する（タイミングなしでは対応付き `TransmitCompleted` を 1 つ出す）。 | 2 回送信する（最初の 1 つだけが `correlated = true` と呼び出し元の `token` を持ち、複製側は `correlated = false, token = 0` で、どちらも `send_time` は保持する）。 |
| `Delay` | 時刻 + 遅れで配送する。 | 送信した時刻 + 遅れからバスを争う。 |
| `QueueFull`、`Failed` | その結果を返して配送しない。 | — |
| `FaultAction::transmit_failed()` | `Accepted` を返してどこにも配送せず、送信側に送信時刻の `TransmitFailed` を出す（非同期の送信失敗）。 | バスを使わない。 |

- `Drop` でも、送信側の Network の中のローカル折り返しは `Accepted` なので配送されます。
- `Delay` のフレームは後から送信したフレームに追い越されるので、並べ替えもこれで作ります。
- `Delay` の遅れは 0 より大きくします。

`Endpoint::inject_event(event)` は、任意のイベント（バス状態、エラーフレームなど）をそのエンドポイントに積みます。

### ECU 単位の通信断

`Endpoint::add_outage(from, until, direction)` は、そのエンドポイントを [from, until) の間バスから切断します。フレームの時刻で決まるので、結果は決定的です。

| `direction` | 起きること | 数える所 |
|---|---|---|
| `Receive` | その間に受信する時刻のフレームを受信しない。 | `outage_dropped()` |
| `Send` | その間の送信時刻のフレームをどこにも配送せず、送信側に `TransmitFailed` を出す。 | `outage_lost()` |
| `Both` | 両方。 | 両方 |

`Send` では、故障注入の関数は呼びません。タイミングありでもバスを使いません。送信は送信時刻で判定するので、タイミングありでは、断の前に送信してバスを待っているフレームは断の間にも出ます。

## 生フレームの観測（`TapPort`）

`TapPort(inner, listener)` はポートを包み、すべてを `inner` に渡します（`transmit_capabilities()` と 2 引数版の `send(frame, token)` も転送します）。11 ビットと 29 ビットの両方をそのまま運びます。

### 通知する内容と順序

次の 3 つを、通った順に `TapListener::on_tap()` へ通知します。

| 種類 | 対象 | 付く値 |
|---|---|---|
| `Received` | プロトコルスタックが受け取ったフレーム | フレームの時刻 |
| `Sent` | 送信したフレーム | 送信したステップの時刻、`send()` の戻り値、`token` |
| `Event` | ポートのイベント | イベントの時刻 |

`Received` の順はポートの解放順（時刻順、同じ時刻なら到着順）で、SIL Kit のポートでは同じ時刻の別の送信側のフレームの順が実行ごとに変わりえます。実行の間で比べるときは（時刻、CAN ID）で並べ直すか、並べ替えた後の `Observer` のイベントを使います。

### リスナーの制約

- 読み取り専用で、注入はしません（テストの刺激は仮想バスのエンドポイントから送信します）。
- 例外を投げないリスナーなら、リスナーの有無で、スタックが受け取るものと送信した結果は変わりません。
- リスナーの例外は呼び出し元に伝わり、ホストは失敗します。
- 受信のフレームとイベントはスタックに渡す前に通知するので、リスナーが例外を投げたフレームはスタックに渡りません。
- リスナーの中からその `TapPort` の `send()`、`release_before()`、`release_events_before()`、`set_listener()` を呼ぶと `std::logic_error` です。
- 時間の領域と wakeup は包んだポートのものです。
- 包んだポートと同じく、ドライバーのスレッドから使います。
- リスナーは 1 つです。設定してある間に `set_listener()` で別のリスナーを設定すると、そのことを伝える `std::logic_error` になり、先のリスナーが聞き続けます。先のものからフレームを読むか（`TapPort` ごとに `testkit::FrameLog` を 1 つ）、`set_listener(nullptr)` で外してから設定します。同じリスナーをもう一度設定しても何も変わりません。
- リスナーと `TapPort` は互いの登録を覚えているので、どちらを先に破棄しても安全です。

## SIL Kit のポート（`SilKitCanPort`）

### 開始（`start()`）

生成時に、CAN コントローラーに受信（RX 方向だけ）と送信確認のハンドラーを付けます（ここから受信するフレームは受信キューに入ります）。

コントローラーを始めるのは `start()` で、participant の CommunicationReady のハンドラーから呼びます（`runtime.add_communication_ready_handler([&port] { port.start(); })`。`Peer` は自分のポートについて呼びます）。SIL Kit が `SetBaudRate()` と `Start()` を呼ぶよう勧めている時点です: SIL Kit のスレッドの上で、最初のステップより前で、coordinated の participant では全員のサービスが見えています。そのため、ネットワークシミュレーターは、起動の順によらずビットレートと開始を受信します。autonomous の participant には、この保証がありません（まだ見つけていないシミュレーターは受信せず、送信し直されません）。

| 条件 | 起きること |
|---|---|
| `start()` の前にステップが来る（`release_before()` か `send()`） | `std::logic_error`（メッセージ「... before start(): the controller is not started ...」が、何を呼ぶかを示す）。 |
| `close()` の後の `start()` | 何もしない。 |
| 2 回目の `start()` | 何もしない。 |

`start()` の前のステップをエラーにするのは、SIL Kit の簡易シミュレーションが、始めていないコントローラーの送信を、送信の確認なしに捨てるためです。

### 終了（`close()`）

`close()` はハンドラーを外し、（始めていれば）コントローラーを止め、コールバックが走らなくなった後で対応表を解放し、受信キューを空にします。`close()` から戻った後は、SIL Kit がポートを呼びません。終了時に未確認のまま残った送信中フレームを検知できるよう、`outstanding()` は `close()` 後も参照できます。

終了の順序は次のとおりです。

1. `ParticipantRuntime` を止める（実時間では `RealTimeDriver` を止める）。
2. ポートを `close()` する（または破棄する）。
3. プロトコル処理とホストを破棄する。
4. `ParticipantRuntime` を破棄する（コントローラーを所有しているため最後）。

### 受信

受信のコールバックは、SIL Kit のタイムスタンプ（送信側のステップ時刻）を付けて受信キューに積むだけです。クラシック CAN のデータフレーム以外は捨てて数えます。

「ステップ T の時点で、T より前のフレームを全部受信している」のは、送信側が自分のステップの中で送信したフレームだけです（SIL Kit が保証する範囲。vehicle-sil のポートはステップの中で送信します）。ほかの SIL Kit のプログラムがステップの外（別のスレッド、受信のハンドラー）から送信したフレームは、どのステップで渡るかが実行ごとに変わりえます。

### 時刻同期に参加していない送信側のフレーム（仮想時間）

時刻同期に参加していない送信側（Vector XL CAN Bridge など）のフレームには、SIL Kit が、受信側のこの participant の今のステップの時刻を付けます。ただし、この participant の最初のステップの前に受信したフレームは `std::chrono::nanoseconds::min()` のままです。

ポートはこの時刻を渡しません。時刻が負のフレームとイベントには、受信した時点でのこの participant の最新のステップの時刻（最初のステップの前なら 0）を付け、次のステップで渡します。数は統計の `received_without_time` にあります。どのステップで渡るかは受信した時点で決まるので、決定的ではありません。

### 送信

送信は `SendFrame()` です。時刻は SIL Kit が送信側のステップ時刻を付けます。SIL Kit の簡易シミュレーションには同期の結果がないので、正しいフレームは `Accepted` です。

仮想時間（`transmit_capabilities() == TransmitCapabilities::Correlated`）では、事前確保された FIFO 空きスロットリング（同時送信中の上限 `kMaxCorrelatedInFlight = 16,384`）からスロットを確保し、送信時刻と 16 ビットの `(2 ビット世代, 14 ビットスロット添字)` タグを `SendFrame()` の userContext に入れます（上位 `[63:56]` が目印 `0xA6`、`[55:40]` が 16 ビット FIFO タグ、`[39:0]` が `TimedFrame::time` の ns 下位 40 ビット）。従来の `encode_send_time()`（目印 `0xA5`、56 ビット ns）も `0xA6` とともに引き続き扱えます。SIL Kit は userContext を送信確認で送信側に返すだけで、他の受信者には渡しません。64 ビットのプラットフォームだけを対象にします。

- 対応表のミューテックスは `SendFrame()` を呼ぶ前に解放するため、簡易シミュレーションで `SendFrame()` の中から送信確認コールバックが同期実行されても再入ロックしません。
- `SendFrame()` が例外を投げたときは、未使用のままであればスロットを安全に回収します。
- 送信確認のコールバック内では、正確な 64 ビット `SimTime` の `send_time` と呼び出し元の `token` を取り出し、イベントの受信箱へ積む直前にスロットを解放します。そのためイベントの受信箱があふれても対応スロットは漏れません。
- **外部の `INetworkSimulator` の `userContext` 契約**: SIL Kit は `userContext` をプロセス間で 64 ビットそのまま運び、ネットワークシミュレーターには `ack.userContext = request.userContext` をそのまま返すことを要求します。完全な 64 ビットの `send_time` をスロット表に保持しているため、シミュレーターがフレームを `2^40 ns`（約 18.3 分）以上遅らせても、`SilKitCanPort` は正確な `send_time` と `token` を復元できます。外部シミュレーターが `userContext` を落としたり書き換えたりした場合（または実行途中で確認を返さず離脱した場合）、`SilKitCanPort` は FIFO や CAN ID 順を推測せず、完了も捏造しません。仮想時間の非対応の送信確認は `SilKitCanPortStatistics::tx_uncorrelated` に数え（`EventOverflow` や `RawChannel` の対応付きカウンタには影響させない）、未確認のままのフレームは `outstanding`（`> 0`）から観測できます。

### 送信確認とイベント

送信確認は数えて、`PortEvent` としてイベントのキューに積みます。

| SIL Kit の通知 | `PortEvent` |
|---|---|
| 送信確認 `Transmitted` | `TransmitCompleted`（仮想時間では `correlated = true`） |
| 送信確認 `Canceled`、`TransmitQueueFull` | `TransmitFailed`（仮想時間では `correlated = true`） |
| エラー状態の通知 | `BusStateChanged`（`NotAvailable` は出さない） |
| 受信キューのあふれ | `ReceiveOverflow` |
| 捨てた送信確認（イベントキュー満杯） | `EventOverflow`（`OverflowCollector` で `release_events_before()` 時にマージ） |

SIL Kit のハンドラーからプロトコル処理は呼びません（簡易シミュレーションでは送信確認が `SendFrame()` の中で返るので、呼ぶと再入になります）。送信確認の時刻は、簡易シミュレーションでは送信したステップの時刻、ネットワークシミュレーターでは送信の終わりです。

仮想時間では、非対応のポートイベント（`BusStateChanged`、`ErrorFrame`、非対応のシミュレーター送信確認）は `release_events_before()` 時に時刻順でマージされる別の `event_capacity` キューを使うため、対応付き送信完了が非対応イベントと枠を奪い合うことはありません（`transmit_queue_capacity > 0` かつ `transmit_queue_capacity <= event_capacity` で対応付きの `EventOverflow` を構造上防げます）。`transmit_queue_capacity == 0` のときに対応付き送信確認のバーストが `event_capacity` を超えた場合は、捨てた送信確認を `event_overflows` に数え、`OverflowCollector` 経由の `PortEvent::Kind::EventOverflow`（`release_events_before()` で `ReceiveOverflow` とともにマージ）として通知します。

### ビットレート（`SilKitCanPortOptions::bitrate`）

`bitrate` を設定すると、`start()` が `Start()` の前に `SetBaudRate()` で渡します。簡易シミュレーションは使いません。ネットワークシミュレーターは送信時間に使います。

既定の 0 では `SetBaudRate()` を呼びません。SIL Kit のドキュメントは、使う前に必ず設定するよう書いています。簡易シミュレーションと簡易 CAN Network Simulator では問題になりません。ほかのネットワークシミュレーターが、設定のないコントローラーをどう扱うかは確認していません。そういうシミュレーターと使うときは、ビットレートを設定してください。

### 送信キューの上限（`SilKitCanPortOptions::transmit_queue_capacity`）

`SilKitCanPort::validate(options)` は、`inbox_capacity > 0`、`event_capacity > 0`、`transmit_queue_capacity <= event_capacity`、および仮想時間（`real_time_clock == nullptr`）で `transmit_queue_capacity <= kMaxCorrelatedInFlight`（`16,384`）であることを検査します。

`transmit_queue_capacity`（既定 0 = 上限なし）を設定すると、送信したが送信確認のイベントをまだ渡していないフレームがその数に達したとき、`send()` が `QueueFull` を返します。プロトコル処理はフレームを残して次のステップで送信し直します。

- ネットワークシミュレーターは自分の送信キューを超えた要求を送信せずに捨てる（`TransmitQueueFull` の送信確認）ので、シミュレーターを使うときはその大きさ以下にします。
- `event_capacity`（および仮想時間では `kMaxCorrelatedInFlight`）より大きくはできません。
- 送信確認の来ないフレーム（開始していないコントローラーでの送信など）は枠を返さないので、統計の `outstanding` で確認します。
- SIL Kit の簡易シミュレーションでは送信確認が次のステップで渡るので、上限は 1 ステップに送信できる数になります。

### 実時間（`SilKitCanPortOptions::real_time_clock`）

`real_time_clock` を設定すると、時刻同期なしの participant のポートになり、時刻の領域は `RealTime` です。`transmit_capabilities()` は `TransmitCapabilities::None` を返し、`Accepted` から `TransmitCompleted` を捏造しません。`RealTimeDriver` で駆動します。

- SIL Kit のタイムスタンプ（同期なしの送信側では使えない）の代わりに、時計で到着時刻を付け、到着のたびにドライバーを起こします。
- userContext に送信時刻や対応タグは入れません（ネットワークシミュレーターは仮想時間が前提のため）。

### 仮想バスとの一致

同じシナリオを仮想バスと SIL Kit（仮想時間）で動かすと、受信と送信のトレースがバイト単位で一致します。ステップ幅の違う participant の組み合わせを含めて、テストで確認しています。

前提として、同じ時刻の別の送信側のフレームについて、ポートが渡す順序は SIL Kit では決まりません（送信側ごとの順序だけが保たれます）。そのため、プロトコル処理がそれらを決まったルールで並べ直す必要があります。テストのプロトコルは（時刻、送信側）で並べ直しています。

## バスのタイミング（`CanBusModel`）

タイミングありの仮想バスと簡易 CAN Network Simulator は、同じバスモデルを使います。SIL Kit に依存しません。

### 送信時間

スタッフビットを含めないビット数（11 ビット ID は 47 + 8 × バイト数、29 ビット ID は 67 + 8 × バイト数。3 ビットのインターミッションを含む）÷ ビットレートです。ns 単位に切り上げます。実際のフレームはスタッフビットのぶん（最大 2 割ほど）長くなります。

### 調停と送信キュー

バスが空いたときに、準備のできた送信キューの先頭どうしで、調停フィールドの小さいものが勝ちます。比べる順は次のとおりです。

1. 11 ビットの基本 ID を比べます。
2. 同じなら、11 ビットのフレームが 29 ビットのフレームに勝ちます（RTR が優性、SRR が劣性）。
3. 29 ビットどうしは拡張 ID を比べます。

同じ ID と形式のフレームどうし（実バスでは解決しない）は、短いデータ、データのバイトの順、送信側の順（仮想バスでは `attach()` の順）で決めます。

送信中のフレームには割り込みません。送信中に準備のできたフレームは、送信の終わりに争います。

送信キューは送信側ごとの FIFO です。後ろのフレームは、調停で勝てる場合でも、前のフレームが出るまで待ちます。

### 模擬しないもの

スタッフビット、エラーフレーム、ACK エラーと自動再送、エラー状態、リモートフレーム、CAN FD、コントローラーの送信バッファの優先度。

### バスを決める時点（`VirtualCanBusTiming::simulator_step`）

| `simulator_step` | バスの決め方 | 受信の時刻 |
|---|---|---|
| 0（厳密） | フレームは送信した時刻からバスを争う。 | 送信時刻だけで決まり、受信側のステップ幅によらない。 |
| Δ > 0 | バスを Δ ごとに決める。 | Δ によって変わるが、受信側のステップ幅にはよらない。 |

Δ > 0 では、境界 T で、T より前に送信されたフレームをキューに加え、[T, T + Δ) に始まる送信を決めます。時刻 t に送信したフレームは、t の後の最初の境界より前には始まりません。簡易 CAN Network Simulator（`SimulationStepSizeNanoseconds` = Δ）と同じルールで、同じ送信には同じ受信の時刻を出します（テストで確認しています）。

### タイミングを確認するときの配置

タイミングの確認は、ノードを別々のポート（別々の CAN コントローラー）に置いて行います。同じ `Network` のノードどうしはローカル折り返しで配送され、バスを通りません。

## 簡易 CAN Network Simulator（`vehicle_sil::can_netsim`、`vehicle_sil_can_bus_simulator`）

SIL Kit の簡易シミュレーション（trivial simulation）は、調停もバスの占有も再現しません。簡易 CAN Network Simulator は、SIL Kit の Network Simulator API（experimental）で、指定したネットワークの CAN を上のバスモデルで模擬する SIL Kit の participant です。CANoe のライセンスなしで、調停、送信時間、混雑を仮想時間で再現します。

### 送受信の扱い

- そのネットワークのコントローラーは SIL Kit が自動で詳細シミュレーションに切り替えるので、各 participant のコードは変わりません。
- 受信のフレームの時刻は送信の終わりです。
- 送信側には、同じ時刻の TX のフレームと送信確認（`Transmitted`）を、送信側の userContext を付けて返します。
- コントローラーごとの送信キュー（既定 64）があふれた要求には `TransmitQueueFull` の送信確認を返し、送信しません。
- CAN FD とリモートフレームの要求は捨てて数えます。
- 要求のキュー（ネットワークごとに 4096 要求）があふれたら、捨てて数え、警告します。
- 要求のキューがあふれた実行は決定的ではありません。
- 設定にあっても、まだどのコントローラーも参加していないネットワークは何もしません（SIL Kit は最初のコントローラーが現れたときにフレームを配送する手段を渡します）。
- そのため、Vector のものと共用する設定に、使わない CAN ネットワークがあってもかまいません。

### 時刻と決定論

時刻同期に参加し、ステップ T で、T より前に送信された要求（`decode_send_time(user_context, reference)` で、`0xA6` の 40 ビット周回タイムスタンプを直近ステップ `last` 付近で非負の剰余展開したものと、従来の `0xA5` 56 ビットタイムスタンプの両方を受け取り、`Transmitted`、`Canceled`、`TransmitQueueFull` で `request.userContext` をビット単位でそのまま返します）を取り込み、[T, T + ステップ幅) に始まる送信を決めます。`0xA6` のタイムスタンプは `[last - limit, last + limit]`（`limit = max(1 s, ステップ幅)`）の範囲だけを受け入れ、`last >= 2^39 ns`（約 550 秒）で後ろ向きに展開された剰余が過去のステップへ並ぶのを防ぎます（範囲外の要求は `last` の目印なしとして扱います）。結果は送信時刻とステップ幅だけで決まり、他の participant のステップ幅やスレッドのタイミングによりません。

次の場合は、決定的ではありません。

| 場合 | 扱い |
|---|---|
| 目印のない要求（vehicle-sil 以外の participant）と、目印の時刻が上限（最後のステップから 1 秒とステップ幅の大きいほう）を超えて先の要求 | 最後のステップの時刻で扱い、数える。 |
| 送信を待つフレームがあるコントローラーが `Reset()` を呼ぶ実行 | どのフレームが `Transmitted` で、どれが `Canceled` かが、1 ステップぶん決まらない（下の「コントローラーのモード」の `Reset()`）。 |

送信時刻の目印の上限は、最後のステップから 1 秒とステップ幅の大きいほうです。

### ビットレート

そのネットワークで最初に `SetBaudRate()` を呼んだコントローラーの値、なければ `--default-bitrate`（既定 500 kbit/s）です。後から違う値が来たら数えて警告し、使いません。使っているビットレートと、それが既定値かコントローラーの設定かを、ログに出力します。

| `SetBaudRate()` を呼ぶもの | シミュレーターが値を受信するか |
|---|---|
| coordinated の participant の vehicle-sil のポート（`vehicle_sil_launch run` の ECU、`Simulation`） | 起動の順によらず受信する（ポートは CommunicationReady で `SetBaudRate()` を呼ぶ）。 |
| autonomous の participant、コントローラーを作ってすぐ `SetBaudRate()` を呼ぶほかのプログラム | シミュレーターを見つける前だと受信しない（SIL Kit の挙動。[dependencies.md](../dependencies.md)）。 |

ポートにビットレートを設定しない場合と、値を受信できない participant のために、ネットワークのビットレートは `--default-bitrate` でも指定してください。

### コントローラーのモード

送信を捨てる条件と、受信がモードによらないことは、SIL Kit の簡易シミュレーションと同じです。`Reset()` の取り消し（`Canceled` の送信確認）は、送信を待つフレームがあるこのシミュレーターだけのものです。

| 場面 | 起きること |
|---|---|
| モードの変更（`Start()`、`Stop()`、`Sleep()`、`Reset()`） | 次のステップで状態の変化を返す。 |
| `Started` 以外のモードを伝えてきたコントローラーが、その後に送信した要求 | 捨てて数え（`rejected`）、送信確認は返さない。 |
| モードを変える前に送信した要求 | そのまま送信する（コントローラーが呼んだ順で決まり、スレッドのタイミングによらない）。 |
| モードを一度も伝えてこないコントローラー | 始めたものとして扱う。 |
| 受信 | フレームを、受信側のモードによらず、送信側以外の接続中の全コントローラーに配送する。 |
| エラー処理のリセットの求め | 何もしない（エラー状態を模擬しないため）。 |

モードを一度も伝えてこないコントローラーを始めたものとして扱うのは、シミュレーターを見つける前の `Start()` を受信できないためです（autonomous の participant。[dependencies.md](../dependencies.md)）。listen only にあたるモードは、SIL Kit の CAN コントローラーにありません。

#### 捨てたときの警告

コントローラーごとに、最初の 1 回だけ警告を出します（始め直してまた止めても、2 回目は出しません）。警告は、フレームの ID、コントローラーの番号、伝えてきたモード（stopped、asleep、reset）を示します。番号は SIL Kit がそのコントローラーに付けたもので、シミュレーターが見つけた順に、全部のネットワークを通して 1 から数えます（participant やコントローラーの名前は、Network Simulator API からは取れません）。

#### `Reset()`

`Reset()` は送信の要求の取り消しを伴います。そのコントローラーの、まだ送信が始まっていないフレームは、バスに出さずに捨てて数え（`rejected`）、`Canceled` の送信確認を返します。取り消しが有効になるのは、シミュレーターが `Reset()` を受信した後に始まる最初のステップです（その前のステップで送信が決まったフレームは配送されます）。`Reset()` と同じ時刻のシミュレーターのステップは、`Reset()` を受信する前に実行されることも後に実行されることもあるので、そのステップで始まるはずだったフレームは、`Transmitted` のことも `Canceled` のこともあります。`Reset()` の後は、`Start()` を呼ぶまで送信できません。

### 設定ファイルとコマンドライン

設定ファイルとコマンドラインは、Vector SIL Kit Network Simulator と共用です。

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

#### 設定ファイル

形式は Vector SIL Kit Network Simulator の設定ファイル（`SchemaVersion: 1`、YAML か JSON）です。`Type: CAN` のネットワークだけを模擬します。ほかの種類のネットワーク、`Switches`、知らないキーは受け付けて警告します。必須の項目がない、`SchemaVersion` が 1 でない、種類が CAN、LIN、Ethernet、FlexRay のどれでもない、名前が重複している、といったファイルは使いません。

#### コマンドライン

Vector のものと同じ形です。

```
vehicle_sil_can_bus_simulator [--name <participant>] [--connect-uri <uri>] [--log <level>] [--participant-configuration <file>] <configuration>
```

- 短い形 `-n -u -l -c` も同じで、participant 名の既定は `NetworkSimulator` です。
- Windows では、引数をワイド文字のコマンドラインから UTF-8 で読みます（ASCII 以外の文字を含むパスと名前を使えます）。
- `-c` のファイルの中の相対パスは、実行時のカレントディレクトリから解決します。
- `-c` の設定には、書いていないキーに vehicle-sil の既定を追加します（待ち受けるアドレス、Windows の TCP、`Experimental.TimeSynchronization.DynamicSimulationStep: false`）。
- `ParticipantName` が `--name` と違う、`Middleware.RegistryUri` が `--connect-uri` と違う、`DynamicSimulationStep` が `true`、のどれかなら、接続する前に準備の失敗（終了コード 3）にして、直し方を示します（[core.md](core.md) の「SIL Kit の設定の既定」）。
- この実行ファイルだけの設定（`--default-bitrate`、`--transmit-queue`、`--start-timeout-ms`）は、Vector のものが同じファイルを読めるように、コマンドラインで渡し、設定ファイルには書きません。

#### 終了コード

| 終了コード | 意味 |
|---|---|
| 0 | 正常に終了 |
| 1 | シミュレーション中の失敗 |
| 3 | 準備の失敗（設定ファイル、接続） |
| 4 | 使い方のエラー |

### Vector SIL Kit Network Simulator との差し替え

実行ファイルを替えるだけで済むように作っています。どちらも同じ participant 名（既定 `NetworkSimulator`）でネットワーク名の模擬を引き受けるので、各ピアのコード、設定ファイル、システムコントローラーの必須 participant の一覧は変わらない想定です。Vector のものでの動作はまだ確認していません。Vector のものは CANoe のライセンスが要ります。

- Vector のものは、スタッフビットを含む送信時間、相手のいない送信の ACK エラーとエラー状態、CAN FD、LIN、Ethernet、FlexRay を模擬しますが、簡易版はどれもしないので、同じ設定でも時刻は一致しません。
- vehicle-sil のポートはどちらのシミュレーターでも userContext に送信時刻と対応タグを入れます。Vector のものにとっては意味のない値であり、Vector のシミュレーターが送信確認で `userContext` をそのまま返すかも確認していません（省略または変更された場合、`SilKitCanPort` は推測せず `tx_uncorrelated` を数えて `outstanding > 0` を保ちます）。
- Vector のものでの決定論とタイミングは確認していません。

### 簡易 CAN Network Simulator の既知の制限

| 制限 | 影響と対処 |
|---|---|
| 送信の開始は、送信した時刻の後の最初のステップの境界より前にならない。 | その間に始まった優先度の低いフレームが先に出ることがある（ずれは最大でステップ幅 + 1 フレーム）。 |
| 送信キューがあふれたときの扱いが、タイミングありの仮想バス（`send()` が同期で `QueueFull`、`RawChannel` は送信ステップ `T` で `Failed` を通知）と簡易版（非同期の `TransmitQueueFull` の送信確認、`RawChannel` はシミュレーターのステップ `T_sim` で `Failed` を通知）で違う。 | あふれる場面では両者のトレースが一致しないので、フレームを失わず失敗時刻（`T` での `Failed`）もそろえるには、`RawChannelOptions::port_transmit_queue_capacity`（またはポートの `transmit_queue_capacity`）を `--transmit-queue` 以下にする（上の「SIL Kit のポート」）。 |
| 同じ時刻に別のコントローラーから同じ ID、データのフレームが出たときの順序は、各コントローラーが最初の要求を出した順（シミュレーターが受信した順）で、決定的ではない。 | 受信側が受信する内容は同じ。 |
| 送信を起点とするプロトコルのタイマー（J1939 の T3 など）は、ポートに渡したステップの時刻から数える。 | バスの待ちのぶん、実際の送信より早く数え始める（[j1939.md](j1939.md) の「トランスポートプロトコル」の制限）。 |
| Network Simulator API は SIL Kit の experimental な API である。 | 使うのは `libs/can_netsim/src/can_bus_simulator.cpp` だけで、SIL Kit を更新したら、このシミュレーターのテストを実行する。 |

## ソース互換性と ABI

- **追加によるソース互換性**: 1 引数版の `Port::send(frame)` と `RawChannel::send(frame)` は 2 引数版とともに残っており、1 引数版の `send()` だけをオーバーライドする独自の `Port` 派生クラスは `TransmitCapabilities::None` としてそのままコンパイルできます。また、`0xA5` 送信時刻エンコードは `0xA6` とともに引き続き受け入れられ、`PortEvent`、`TapRecord`、`*Options`、`*Statistics` の新しいフィールドはすべて既定メンバー初期化子付きで末尾に追加されているため、既存の集成体初期化も有効なままです。
- **ABI への影響**: `can::Port`（`transmit_capabilities()`、2 引数版 `send()`）と `can::Steppable`（`bind_port()`）への仮想関数追加は vtable 配置を変え、`PortEvent`、`TapRecord`、`RawChannelOptions`、`RawChannelStatistics`、`SilKitCanPortStatistics`、`VirtualCanBus::Endpoint`、`RawChannel`、`SilKitCanPort` へのフィールド追加は構造体・クラスのメモリ配置を変えます。利用側は更新後のヘッダーに対して再コンパイルしてください。

## 関連するドキュメント

- J1939 スタックがこの基盤をどう使うか: [j1939.md](j1939.md)
- 簡易 CAN Network Simulator を使う participant の組み立て: [peers.md](../peers.md)
- 全体像: [architecture.md](../architecture.md)
- どこに何があるか: [code-map.md](../code-map.md)
- ほかの内部の仕組みのドキュメント: architecture.md の「[内部の仕組み](../architecture.md#内部の仕組み)」
