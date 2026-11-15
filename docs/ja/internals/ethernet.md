# イーサネットの基盤

[English](../../internals/ethernet.md)

イーサネットのフレームの型、ポート、チャネル、仮想ネットワーク、SIL Kit のポートについて、仕様と、実装が守る保証と制限を書きます。イーサネットのフレームを直接扱うプロトコルやテストを作るときの仕様も、ここにあります。API はまだ安定していません。

| 節 | 書いてあること |
|---|---|
| [ライブラリの構成](#ライブラリの構成) | 依存、ECU から使う関数、フレームの中身を解釈しないこと |
| [値の型](#値の型) | `Frame`、`MacAddress`、`TimedFrame` |
| [ポートの仕様（`Port`）](#ポートの仕様port) | ポートの実装が守ること、イベント（`PortEvent`） |
| [チャネル（`Channel`）](#チャネルchannel) | 1 ステップの動き、同じ時刻の順、受信するステップと時刻、上限、受信キューの容量、統計 |
| [仮想ネットワーク（`VirtualEthernetBus`）](#仮想ネットワークvirtualethernetbus) | プロセスの中のイーサネットの振る舞い |
| [SIL Kit のポート（`SilKitEthPort`）](#sil-kit-のポートsilkitethport) | 開始と終了、受信、送信、時刻同期に参加していない送信側、終了の順序 |
| [ECU から使う（`Ecu::ethernet()`）](#ecu-から使うecuethernet) | `Peer` と `LocalBench` が作るもの、`ecu.connected`、`--describe` の出力 |
| [未確認の値](#未確認の値) | 規格で確認していない値と、その変え方 |
| [検証の範囲](#検証の範囲) | テストが確認していること |

## ライブラリの構成

- `vehicle_sil::eth` は SIL Kit に依存しません。`vehicle_sil::step` と `sim_base` だけをリンクし、`eth.boundary` が SIL Kit のヘッダーを含まないことを確認します。
- `vehicle_sil::eth_silkit` は、`vehicle_sil::eth` のポートを SIL Kit の Ethernet のコントローラーの上に作ります（仮想時間だけ）。
- ECU からは `Ecu::ethernet(network, options)` で使います（`options` は `ChannelOptions`）。`Peer` と `LocalBench` がチャネルとポートを作ります（下の「ECU から使う」）。
- vehicle-sil はフレームの中身を解釈しません。読むのは宛先と送信元のアドレスだけで、EtherType、VLAN のタグ、payload はバイト列のまま通します。
- TCP/IP などのスタックは持ちません。

ステップを実行する仕組み（ホスト、ドライバー、時間の領域）は [can.md](can.md) にあり、イーサネットも同じものを使います。

## 値の型

| 型 | 内容 |
|---|---|
| `Frame` | 宛先のアドレスから payload の終わりまでのバイト列（FCS を含まない） |
| `MacAddress` | 6 バイトのアドレス |
| `TimedFrame` | 時刻付きのフレーム |

`Frame`:

- バイトはオブジェクトの中に持ちます（`Frame::kCapacity` まで。メモリを確保しません）。
- コピーするときは長さの分だけコピーします。
- 比較は長さとバイトで行います。
- `destination()` は 0〜5 バイト目、`source()` は 6〜11 バイト目です。

`MacAddress`:

- `parse()` は `02:00:00:00:00:01` の形（`:` か `-` で区切った 16 進の 2 桁が 6 つ）を読みます。
- `to_string()` は小文字と `:` で書きます。
- 順序（`<=>`）はバイトの辞書順です。

`TimedFrame` の時刻:

| 向き | 時刻 |
|---|---|
| 受信 | 送信側のステップの時刻（時刻同期した送信側の場合。そうでない送信側は「[時刻同期に参加していない送信側](#時刻同期に参加していない送信側)」） |
| 送信 | 送信するステップの時刻 |

## ポートの仕様（`Port`）

`vehicle_sil::can::Port` と同じルールです。

- `release_before(now, sink)` は、時刻が `now` より前の受信フレームを、時刻順、同じ時刻なら到着順に渡します。
- 時刻がちょうど `now` のフレームは次の呼び出しで渡します。
- 同じ時刻のフレームを並べ替えるのはチャネルです。
- 自分が送信したフレームは受信しません。
- `send()` は同期の結果を返します（`Accepted`、`QueueFull`、`Failed`）。`kHeaderBytes`（14）より短いフレームは `Failed` です。
- `release_events_before(now, sink)` は、時刻が `now` より前のイベントを同じ順で渡します。ホストは同じステップで `release_before()` より先に呼びます。

### ポートのイベント（`PortEvent`）

イベント（`PortEvent`）の種類は次のとおりです。

| 種類 | 意味 | 時刻 |
|---|---|---|
| `TransmitCompleted` | ポートに渡したフレームをバックエンドが送信した | バックエンドが確認した時刻 |
| `TransmitFailed` | `send()` が受け付けたフレームを、バックエンドが送信しなかった（リンクが落ちている、コントローラーが動いていない、捨てた） | バックエンドが通知した時刻 |
| `ReceiveOverflow` | 受信キューが満杯で受信フレームを捨てた。`count` は捨てた数 | 捨てたフレームのうち最も早い時刻 |
| `EventOverflow` | イベントのキューが満杯で、送信の確認のイベントを捨てた。`count` は捨てた数 | 捨てたイベントのうち最も早い時刻 |

`ReceiveOverflow` と `EventOverflow` はイベントのキューの容量を使いません（`OverflowCollector`）。送信の確認を押し出さず、キューが満杯でも渡されます。

## チャネル（`Channel`）

アプリケーションが生のフレームを送受信する API です。`step::Member` なので、`step::Host` に追加すると、ほかのメンバーと同じステップで、追加した順に実行されます。メンバーの関数（`begin()`、`receive(now)`、`process()`、`flush()` など）はホストのためのもので、`Channel` では private です。

### 1 ステップの動き

時刻 `now` の 1 ステップは、次の順に進みます。

| 段 | すること |
|---|---|
| 1. 受信 | ポートが `now` より前に渡したフレームのうち、フィルターと長さの検査を通ったものを、時刻、送信元のアドレス、到着の順に並べる |
| 2. 処理 | `on_frame()` で追加したハンドラーを、フレームごとに、追加した順に呼ぶ |
| 3. アプリケーション | `receive()` が同じフレームを同じ順に 1 つずつ返す |
| 4. 送信 | そのステップの `send()` のフレームを、呼んだ順に、時刻 `now` でポートに渡す |

受信の段では、ポートのイベントは数えるだけです。

アプリケーションの段のルール:

- 取らなかったフレームは、次のステップには残りません。
- 一度でも `receive()` を呼んだチャネルでは、ステップの終わりに取らなかったフレームを `frames_not_taken` に数えます（ハンドラーだけのチャネルは数えません）。
- ハンドラーと `receive()` は互いに影響しません。

送信の段のルール:

- ポートが受け付けなかったフレームは数えて捨てます（キューが満杯ならそのフレームと残り全部、失敗ならそのフレーム）。
- 送信し直しはしません。

呼べる時点:

- `send()` と `receive()` は、ステップの中（ハンドラーかアプリケーション）だけで呼べます。外から呼ぶと `std::logic_error` です。
- ハンドラーから同じホストのほかのチャネルに送信することもできます（ゲートウェイ）。そのフレームは同じステップの送信の段で送信されます。
- `on_frame()` はホストが始まる前だけです。

### 同じ時刻のフレームの順

SIL Kit では、違う送信側から同じ時刻に受信したフレームの到着の順が、スレッドの都合で決まります。チャネルは、同じ時刻のフレームを送信元のアドレス（6〜11 バイト目）の順に並べ、同じアドレスの中は到着の順のままにします（安定な並べ替え）。そのため、同じ ECU を仮想ネットワークで動かしても SIL Kit で動かしても、ハンドラーと `receive()` に渡る順が同じになります。同じアドレスの送信側が 2 つあると、その 2 つの間の順は決まりません。

### 受信するステップと時刻

- 時刻 t のステップで送信したフレームは、受信側の「t より後に始まる最初のステップ」で受信されます。
- 伝送の時間は模擬しません。両方が 1 ms のステップなら、往復は 2 ms の仮想時間です。
- 受信したフレームの時刻は、送信したステップの時刻です（時刻同期した送信側の場合）。

### 上限（`ChannelOptions`）

| 値 | 既定 | 意味 |
|---|---|---|
| `send_capacity` | 256 | 1 ステップで `send()` が受け取るフレームの数。超えると `SendStatus::QueueFull` |
| `receive_capacity` | 1024 | 1 ステップで渡すフレームの数（フィルターを通ったもの）。超えた分は捨てて `receive_overflows` に数える |
| `max_frame_bytes` | 1522 | 送信できるフレームと、渡すフレームの最大の長さ（FCS なし）。`kHeaderBytes` から `Frame::kCapacity`（2048）まで |
| `accept` | `All` | 渡すフレームのフィルター（`All` か `Addressed`） |

`receive_capacity`:

- 超えたときにどれが捨てられるかは受信した順で決まるので、SIL Kit では決まりません。あふれはエラーとして扱います。
- ポートの受信キューは、これより大きくします（下の「ポートの受信キューの容量」）。

`max_frame_bytes`:

- これより長い受信フレームは捨てて `frames_invalid` に数えます。

`accept`:

| 値 | 渡すフレーム |
|---|---|
| `All` | ネットワークの全部のフレーム（プロミスキャスのキャプチャと同じ見え方） |
| `Addressed` | `mac` 宛て、ブロードキャスト、`multicast` に挙げたアドレス宛てだけ |

フィルターで除いたフレームは `frames_filtered` に数えます。

### メモリの確保

どちらの受信キューも、生成のときに 1 回だけ確保します。ステップの中では確保しません。並べ替えは、受信した順のフレームを動かさずに、添字の配列を並べます。`eth.steps_do_not_allocate` が確認します。

1 フレームは `Frame::kCapacity` バイトを持つので、既定の容量では次の大きさになります。

| もの | 使うメモリ |
|---|---|
| チャネル 1 つ | 約 2.5 MB |
| 仮想ネットワークのエンドポイントの受信キュー（既定 2048 フレーム） | 約 8 MB（並べ替えのためのコピーを含む） |
| SIL Kit のポートの受信キュー（既定 2048 フレーム） | 約 8 MB（並べ替えのためのコピーを含む） |

### ポートの受信キューの容量

ポートの受信キュー（ステップをまたいでためる側）は、チャネルの `receive_capacity`（1 ステップで渡す側）の 2 倍にします。`Channel::port_inbox_capacity(options)` がこの値を返します。

#### 2 倍にする理由

時刻 t のステップがフレームを取り出すとき、受信キューには、このステップで渡す分（前のステップの時刻から t より前まで）のほかに、先に時刻 t のステップを終えた送信側の「時刻 t」のフレームが入っています。こちらは次のステップで渡す分です。送信側は受信側の次のステップの時刻より先へは進まないので（`StepDriver` は時刻の順に実行し、SIL Kit の時間同期も同じです）、どちらも `receive_capacity` 以下で、合わせて 2 倍に収まります。送信側のステップ幅が受信側と違っても同じです。

#### 保証

ネットワークから 1 ステップに受信するフレーム（フィルターの前）が `receive_capacity` 以下なら、ECU を実行する順にも、SIL Kit のスレッドのタイミングにもよらず、ポートはフレームを捨てません。記録は仮想ネットワークと SIL Kit で同じです。

#### 保証の対象外

1 ステップに `receive_capacity` より多く受信する設定では、チャネル（`receive_overflows`）かポート（`port_receive_overflows`）が捨てて数えます。どちらが、どのフレームを捨てるかは、実行する順とタイミングで変わります。時刻同期に参加していない送信側のフレームは、受信した時点のステップに入るので、この保証に入りません。

#### 容量を決める所

- `Ecu::ethernet()` は、`Peer` でも `LocalBench` でもこの容量でポートを作ります。
- `VirtualEthernetBusOptions::inbox_capacity` と `SilKitEthPortOptions::inbox_capacity` の既定（2048）は、既定の `receive_capacity` に合わせた値です。
- チャネルを自分でポートに追加し、`receive_capacity` を大きくするときは、ポートの側も `Channel::port_inbox_capacity()` の値にします（仮想ネットワークは `attach(inbox_capacity)` でエンドポイントごとに決められます）。

### 統計（`ChannelStatistics`）

数は増えるだけです。

| 名前 | 数えるもの |
|---|---|
| `frames_received` | 渡した |
| `frames_taken` | `receive()` で取った |
| `frames_not_taken` | `receive()` を使うチャネルで、取らずに終わった |
| `receive_overflows` | （「上限」の `receive_capacity`） |
| `frames_filtered` | （「上限」の `accept`） |
| `frames_invalid` | （「上限」の `max_frame_bytes`） |
| `frames_sent` | ポートが受け付けた |
| `sends_rejected` | `send()` が拒否した |
| `frames_dropped` | 受け取ったがポートが受け付けなかった |
| `transmit_completed`、`transmit_failed`、`port_receive_overflows` | ポートのイベント |
| `port_event_overflows` | ポートのイベントのうち、ポートが捨てた送信の確認（その分 `transmit_completed` と `transmit_failed` が少ない） |

## 仮想ネットワーク（`VirtualEthernetBus`）

プロセスの中のイーサネットです。SIL Kit の Network Simulator なしの Ethernet と同じく、ハブとして振る舞います。スレッドセーフではありません。1 つのスレッド（`StepDriver` のループ）から使います。

| 場面 | 振る舞い |
|---|---|
| `attach()` | 呼ぶごとに 1 つのエンドポイント（`Port`）を作る |
| `attach(inbox_capacity)` | そのエンドポイントの受信キューの容量を決める（0 は `std::invalid_argument`） |
| 時刻 t での送信 | 宛先のアドレスによらず、ほかの全部のエンドポイントが時刻 t で受信する（自分は受信しない） |
| 送信の確認 | イベントを出さない |
| `min_frame_bytes`（既定 60）より短いフレーム | 受信側に 0 で埋めて渡す（0 にすると埋めない） |
| 受信側がすでに渡した時刻より前のフレーム | `std::logic_error` で拒否する（ホストを時刻の順に実行していない） |
| 受信キュー（`inbox_capacity`、既定 2048）のあふれ | 捨てて数え、`ReceiveOverflow` を出す |

- 短いフレームを 0 で埋めるのは、SIL Kit と同じ記録にするためです。
- `StepDriver` は時刻の順を守ります。
- 受信キューの容量の決め方は「ポートの受信キューの容量」にあります。

## SIL Kit のポート（`SilKitEthPort`）

時刻同期した participant の Ethernet のコントローラーの上のポートです（`TimeDomain::Virtual`）。実時間（時刻同期なし）のポートはまだありません。

### 開始と終了

| 時点 | すること |
|---|---|
| 生成 | 受信（RX の向きだけ）、送信の確認、状態の変化のハンドラーを登録する |
| `start()` | コントローラーを `Activate()` する |
| `close()` | ハンドラーを外し、（始めていれば）`Deactivate()` し、ためたものを捨てる |

- 生成のときに登録したハンドラーから受信したフレームは、受信キューに入ります。
- `start()` は、participant の CommunicationReady のハンドラーから呼びます（`runtime.add_communication_ready_handler([&port] { port.start(); })`）。
- `start()` の前にステップが来ると（`release_before()` か `send()`）、何を呼ぶかを示す `std::logic_error` です。
- `close()` の後、SIL Kit はポートを呼びません。

CommunicationReady は、SIL Kit が `Activate()` を呼ぶよう勧めている時点です。SIL Kit のスレッドの上、最初のステップの前です。coordinated の participant では全員のサービスが見えているので、ネットワークシミュレーターにも伝わります。見つける前の `Activate()` は手元の状態だけを変え、やり直せません。

### 受信

受信のハンドラーは、フレームを受信キュー（`StepInbox`）に SIL Kit の時刻（送信側のステップの時刻）で入れるだけです。`Frame::kCapacity` より長いフレームは数えて捨てます。

「ステップ T の時点で、T より前のフレームは全部受信している」のは、送信側が自分のステップの中で送信したフレームだけです（SIL Kit が保証する範囲）。ほかの SIL Kit のプログラムがステップの外から送信したフレームは、どのステップで渡るかが実行ごとに変わりえます。

### 送信と送信の確認

送信は、そのステップの時刻で `SendFrame()` を呼びます。送信の確認は、Network Simulator がなければ `SendFrame()` の中で返るので、イベントとしてキューに入れて次のステップで渡します（チャネルをその場で呼びません）。

| SIL Kit の状態 | イベント |
|---|---|
| `Transmitted` | `TransmitCompleted` |
| `ControllerInactive`、`LinkDown`、`Dropped`、`InvalidFrameFormat` | `TransmitFailed` |

- 種類ごとの数は統計にあります。
- `event_capacity`（既定 1024）は、1 ステップに送信するフレームの数（チャネルの `send_capacity`）以上にします。
- あふれた確認は捨てて数え、`EventOverflow` で通知します。

60 バイトより短いフレームは、ポートが 0 で 60 バイトまで埋めてから SIL Kit に渡します。受信側の SIL Kit も短いフレームを埋めますが、Network Simulator は短いフレームを拒否することがあるため、送信する側でそろえます。

### 時刻同期に参加していない送信側

時刻同期に参加していない participant（実際のネットワークへのブリッジなど）のフレームには、SIL Kit は `std::chrono::nanoseconds::min()` の時刻を付けます。受信側の SIL Kit は、これを受信側の今のステップの時刻に置き換えますが、受信側の最初のステップの前に受信したフレームはこの値のままです。

ポートはこの時刻をそのまま渡しません。時刻が負のフレームには、受信した時点でのこの participant の最新のステップの時刻（最初のステップの前なら 0）を付け、次のステップで渡します。そのステップの時刻に送信されたフレームと同じ扱いです。どのステップで渡すかは受信した時点で決まるので、決定的ではありません。数は `received_without_time` にあります。

CAN の仮想時間のポート（`SilKitCanPort`）も同じルールです。

### 終了の順序

次の順に終了します。

1. ParticipantRuntime を止める（ステップが来なくなる）。
2. ポートを `close()` するか破棄する。
3. チャネルを破棄する。
4. ParticipantRuntime を破棄する（コントローラーを持つ）。

## ECU から使う（`Ecu::ethernet()`）

`Ecu::ethernet(network, ChannelOptions)` は、その ECU の `Channel` を返します。ECU は、1 つのイーサネットのネットワークにチャネルを 1 つ持ちます。使い方と 1 ステップの中の位置は [execution.md](execution.md#イーサネットのチャネル) の「イーサネットのチャネル」にあります。ここには実装のルールを書きます。

### ホストへの追加

チャネルは `step::Member` なので、ECU のホスト（`can::Host`）に `step::Host::add(Member&)` でそのまま追加します（`can::Host` は `using step::Host::add;` で、CAN の組と同じ列に並べます）。受信と送信の段は、`j1939()`、`can()`、`ethernet()` を呼んだ順です。

### `Peer` が作るもの

`Peer` は、ネットワーク名を名前にした SIL Kit の Ethernet コントローラー、`SilKitEthPort`、`detail::GatedEthPort`、`Channel` を作ります。仮想時間だけで、実時間の `Peer` では `std::logic_error` です（実時間のポートがないため）。

| 項目 | ルール |
|---|---|
| ポートの受信キュー | `receive_capacity` の 2 倍（`Channel::port_inbox_capacity()`。「ポートの受信キューの容量」） |
| 送信の確認のキュー | `send_capacity` 以上（既定の 1024 より小さくはしない） |
| `start()` | CommunicationReady のハンドラー（`ParticipantRuntime::add_communication_ready_handler()`）で、CAN のポートの後、LIN の `init()` の前 |
| `close()` | `Peer::stop()` がステップを止めた後 |
| オプションの検査 | `Channel::validate()` で先に確認する（SIL Kit のコントローラーは作ると外せないため） |

### `LocalBench` が作るもの

`LocalBench` は、名前ごとの `VirtualEthernetBus`（既定のオプション）に、受信キューが `receive_capacity` の 2 倍のエンドポイント（`attach(Channel::port_inbox_capacity(options))`）を追加し、その上に `GatedEthPort` と `Channel` を作ります。受信キューはエンドポイントごとなので、`receive_capacity` の違う ECU が同じネットワークに接続できます。テストが `ethernet_bus(network).attach()` で追加するエンドポイントは、既定の容量です。

### `ecu.connected`

`GatedEthPort` が、切断している間の送信を `SendResult::Failed` にし（チャネルの `frames_dropped`）、切断している間の時刻のフレームを捨てます。判定の時点は CAN と同じです（`BusConnection`）。ポートのイベント（送信の確認、あふれ）は通します。

### `--describe` の出力

`ChannelOptions::mac` を `NetworkDescription::mac` として出力します（`--describe` の `networks[].mac`）。照合のルール `ethernet-duplicate-mac` と `ethernet-mac-unknown`（[simulation-check.md](../simulation-check.md)）が使います。チャネルは、送信するフレームの送信元のアドレスを確認せず、書き込みもしません（アドレスを決めるのは ECU のスタックです）。

### 確認していない振る舞い

SIL Kit の Network Simulator がイーサネットのネットワークを模擬しているときの振る舞い（確認の時刻、リンクの状態）は、確認していません。

## 未確認の値

次の値は、一般の資料と SIL Kit のソースの知識によるもので、規格では確認していません。変えられる値は、利用者が変えられるようにしてあります。

| 値 | 使う所 | 変え方 |
|---|---|---|
| 最大の長さの既定 1522 バイト（FCS なし。タグのない 1514 バイトに、タグの分の余裕を足したもの） | `ChannelOptions::max_frame_bytes` | オプション。上限は `Frame::kCapacity`（2048。これより大きいジャンボフレームは扱わない） |
| 最小の長さ 60 バイトへの 0 の埋め | SIL Kit の受信（上流の実装）、`VirtualEthernetBusOptions::min_frame_bytes` | 仮想ネットワークはオプション。SIL Kit の側は変えられない |
| 先頭の 14 バイト（宛先 6、送信元 6、EtherType か長さ 2）と、アドレスのバイトの並び | `kHeaderBytes`、`Frame::destination()`、`source()`、送信できる最短のフレーム | 変えられない |
| アドレスのグループのビット（1 バイト目の最下位のビット）と、ブロードキャスト（全部 0xFF） | `MacAddress::is_multicast()`、`is_broadcast()`、`Accept::Addressed` | `Accept::All` なら使わない |
| テストと例の EtherType 0x88B5（ローカルの実験用として知られる値）と、ローカル管理のアドレス `02:00:00:...`、`03:00:00:...` | テストと `examples/ethernet` だけ（vehicle-sil のライブラリは使わない） | 例は `--ethertype <数>`（`ethernet_example::Options::ether_type`）で変えられる。アドレスは例のヘッダーの定数 |

## 検証の範囲

### `eth.*`

`tests/eth` にあり、`vehicle_sil::eth` だけをリンクします。

- 値の型
- 仮想ネットワーク: ハブ、次のステップ、埋め、自分のフレーム、時刻の順、あふれ、エンドポイントごとの受信キューの容量
- チャネル: ホストとドライバーの上で実行されること、ゲートウェイ、同じ時刻の順、`receive()` と取らなかった数、送信の段、上限、ポートの受信キューの容量、フィルター、イベント、オプションのエラー
- `eth.steps_do_not_allocate`: ステップの中でメモリを確保しないことを、プロセスの全部の確保を数えて確認します。専用の実行ファイルで、数え方は J1939 の同じテストと共通の `tests/unit/allocation_counter.hpp` です。
- `eth.boundary`: SIL Kit を含まないことを確認します。

### `simulation.ethernet_*` と関連するテスト

| テスト | 確認すること |
|---|---|
| `simulation.ethernet_*`（`tests/simulation/test_ethernet.cpp`） | `Ecu::ethernet()` を仮想ネットワークと SIL Kit の両方で |
| `simulation.disconnect_*` | `ecu.connected` でイーサネットも切断されること |
| `peer.*` | 実時間での拒否、コントローラー名の重なり、CommunicationReady で有効になること |
| `launch.an_ethernet_network_has_one_mac_address_per_participant` | 照合のルール |
| `ethernet_example.*`、`launch_run.ethernet` | 例（`examples/ethernet`）を動かす |

`simulation.ethernet_*` が確認することは、次のとおりです。

- 次のステップで受信すること
- 同じ時刻の順
- ハンドラーと `receive()`
- `Accept::Addressed`
- `--describe` の出力のアドレス
- 拒否のメッセージ
- `LocalBench::ethernet()` と `ethernet_bus()`
- 1 ステップに `receive_capacity` までのフレームを、送信側を先に実行しても、ステップ幅が違っても捨てず、両方で同じ記録になること
- 超えた分を数えること

### `eth_silkit.*`

- 偽のコントローラーに対するポートのテスト（時刻のないフレーム、あふれた送信の確認を含む）。
- プロセス内の registry で、同じシナリオの記録が仮想ネットワークと SIL Kit で一致すること。ステップ幅の組み合わせを変えて確認します（3 つの送信側が同じ時刻に送信する場合、短いフレームの埋め、ハンドラーからの返信、`receive()`、`Accept::Addressed` を含む）。
- ノードのアドレスは登録の順と違う順にしてあり、仮想ネットワークの側だけでも、同じ時刻のフレームがアドレスの順に並んでいることを確認します。

## 関連するドキュメント

- ステップを実行する仕組み（ホスト、ドライバー、時間の領域）: [can.md](can.md)
- `Ecu::ethernet()` の使い方: [execution.md](execution.md)
- 全体像: [architecture.md](../architecture.md)
- どこに何があるか: [code-map.md](../code-map.md)
- ほかの内部の仕組みのドキュメント: architecture.md の「[内部の仕組み](../architecture.md#内部の仕組み)」
