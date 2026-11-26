# 実行とテストの基盤

[English](../../internals/execution.md)

ECU のアプリケーションを組み立てる `Ecu`、J1939 と LIN のノードを動かす participant の `Peer`、プロセス内の仮想バスで実行する `LocalBench` とテストのツール、それらを選んで動かす `Simulation` について、仕様と、実装が守る保証を書きます。

| 節 | 書いてあること |
|---|---|
| [`Ecu` クラス](#ecu-クラスvehicle_silecu) | 組み立ての関数、1 ステップの順序、所有と破棄、変数、バスからの切断、`describe()` の出力 |
| [`Simulation`](#simulationvehicle_silsimulation) | 実行する場所の選び方、`run_for()` と `run()`、失敗の報告 |
| [Peer](#peervehicle_silpeer) | participant の組み立て、時間のモード、lifecycle、停止と破棄、スレッド |
| [ローカルの実行とテストキット](#ローカルの実行vehicle_sillocalbenchとテストキットvehicle_siltestkit) | `LocalBench` のバスと ECU、記録と故障注入 |
| [検証の範囲](#検証の範囲) | テストが確認している範囲 |

`Peer` と `LocalBench` は、どちらもエンジン（`vehicle_sil::j1939`）の公開 API だけを使います。CTest の `local.boundary`、`testkit.boundary`、`peer.boundary`、`ecu.boundary`、`simulation.boundary` が、内部ヘッダーやテスト用の API を使っていないことを確認します。区分（[architecture.md](../architecture.md) の「ディレクトリとターゲット」の表）は、`Ecu` と `Simulation` が利用者向け、`Peer` と `LocalBench` が基盤、テストキットがテスト用です。

## `Ecu` クラス（`vehicle_sil::Ecu`）

ECU のアプリケーションを組み立てるクラスです（`vehicle_sil::ecu`、SIL Kit なし）。`Peer`（SIL Kit の participant）と `LocalBench::add_ecu()`（仮想バスのホスト）が実装するので、`Ecu&` を受け取る関数やクラスは、どちらの上でも同じコードで動きます。

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

### 組み立ての関数

ECU のアプリケーションは、ふつうはコンストラクターで、次の `Ecu` の関数を呼んで自分を組み立てます。

| 関数 | 作るもの |
|---|---|
| `j1939(network, NetworkOptions)` | J1939 の `Network` |
| `can(network, RawChannelOptions)` | 生の CAN のチャネル |
| `lin_master(network, schedule, LinMasterOptions)`、`lin_slave(network, LinSlaveOptions)` | LIN のノード |
| `ethernet(network, eth::ChannelOptions)` | 生のイーサネットのフレームのチャネル |
| `on_step(fn)`、`every(period, fn)`、`at(time, fn)` | ステップの中で呼ぶ関数 |
| `emplace<T>(args...)` | ECU が持つオブジェクト |

どの関数も、実行の開始前にだけ呼べます。ネットワーク名のルールと、違反したときの結果は次のとおりです。

| 条件 | 結果 |
|---|---|
| 実行の開始後に、組み立ての関数を呼ぶ | `std::logic_error` |
| 実行の開始後に、`emplace` を呼ぶ | `T` を作らずに `std::logic_error` |
| 同じ ECU で、`j1939()` に同じネットワーク名を 2 回渡す（`can()` も同じ） | 2 回目は `std::invalid_argument` |
| LIN のネットワーク名を CAN と共有する | `std::invalid_argument` |
| ECU が 1 つの LIN のネットワークに LIN のノードを 2 つ持つ | `std::invalid_argument` |
| `j1939("")` か `can("")`（空のネットワーク名）を呼ぶ | どちらの実装でも `std::invalid_argument` |
| `T` のコンストラクターが例外を投げる | その例外が伝わり、`T` は ECU に残らない |

`j1939()` と `can()` は、同じ CAN のネットワーク名を使えます。イーサネットのネットワーク名も CAN、LIN と共有できず、ECU は 1 つのイーサネットのネットワークにチャネルを 1 つだけ持ちます。`T` のコンストラクターが例外を投げたときも、コンストラクターが ECU に追加したもの（ネットワーク、ノード、ハンドラー、関数）は残ります。

### 1 ステップの順序

| 順 | すること |
|---|---|
| 1 | `j1939()`、`can()`、`ethernet()` で作ったものが、追加した順に、受信して処理する |
| 2 | 変数への書き込みのうち、時刻が来たものを反映する（`Agent::on_step_begin()`） |
| 3 | `on_step`、`every`、`at` の関数を、追加した順に呼ぶ |
| 4 | LIN のノードの `step(now)` を、追加した順に呼ぶ |
| 5 | 変数を読み出す（`Agent::on_step_end()`。`sample_period` ごと） |
| 6 | `j1939()`、`can()`、`ethernet()` で作ったものが、1 と同じ順で送信する |

- 片方のネットワークのハンドラーから他方へ送信すると、同じステップで送信されます（ゲートウェイは、ネットワークを 2 つ持つ ECU です）。
- アプリがステップで出した LIN の要求（`set_frame_data()`、`set_response()`、`request_sleep()` など）は、そのステップから有効になります。
- LIN のノードが受信したものは、そのステップの LIN の処理で通知されるので、アプリは次のステップで読みます。
- `Peer`、`LocalBench`、`Ecu` の説明は同じ順序です。

### 所有と破棄

ECU は、LIN のノード、ネットワーク（コンポーネントが先）、生チャネル、イーサネットのチャネル、Control Agent、`emplace` したオブジェクトを持ちます。返す参照は、ECU の寿命まで有効です。

破棄の順は、LIN のノード、ネットワーク、生チャネル、イーサネットのチャネル、Agent、`emplace` したオブジェクト（最後に追加したものから先）です。ハンドラー、コンポーネント、変数のポインターと関数は、`emplace` したオブジェクトを破棄の間も参照できます。

### 変数の宣言

外から読み書きする変数は、次の関数で宣言します。引数、型、検査は `control::Agent` の同名の関数と同じです。

- `input(name, &value, options)`、`input(name, get, set, options)`、`input_enum(name, &value, items, options)`
- `output(name, &value, options)`、`output(name, get, options)`、`output_enum(name, get, items, options)`

宣言は ECU の Agent（`agent()`）が持ちます。Agent は ECU に 1 つで、作る時点は次のとおりです。

| 場合 | Agent を作る時点 |
|---|---|
| 変数を宣言するか、`agent()` を呼ぶ | 最初の宣言か `agent()` のとき |
| どちらもない | 実行の開始時（`Peer::start()`、`LocalBench` の最初の実行。`ecu_main` は組み立ての直後） |

どの ECU にも Agent があります。Agent は作るときに、どの ECU にもある入力 `ecu.connected`（下の「[バスからの切断](#バスからの切断ecuconnected)」）を最初の変数として持ちます。`agent()` は開始後も、ある Agent を返します。

| 条件 | 結果 |
|---|---|
| Agent を作った後に `set_agent_options(AgentOptions)` を呼ぶ | `std::logic_error` |
| `set_agent_options()` に不正なオプション（`Agent::check_options()` が拒否するもの）を渡す | ECU の名前つきの `std::invalid_argument`（オプションは変わらない） |
| 実行の開始後か、スキーマの確定の後に、宣言か `set_agent_options()` を呼ぶ | `std::logic_error` |
| 同じ ECU で同じ名前を宣言する | `std::invalid_argument`（メッセージに ECU の名前） |
| `peer.` か `ecu.` で始まる名前を宣言する | どちらの実装でも、宣言のときに `std::invalid_argument` |
| Peer のネットワークごとの監視の出力（`j1939.N.` で始まる名前）と衝突する名前を宣言する | 宣言のときではなく、スキーマを確定するときに `std::invalid_argument` |

`peer.` で始まる名前は Peer の監視の出力のために、`ecu.` で始まる名前はどの ECU にもある変数のために予約しています。

実装は Agent を `detail::EcuAgent` で持ちます。`detail::EcuAgent` が受け持つのは、遅延の生成、オプション、ステップの前後の呼び出し、終わりの `finish()`、残っているトランスポートの検査です。

### トランスポートの寿命

ECU の外から Agent を使うトランスポート（`control_silkit::AgentTransport`）は、生きている間 Agent に `control::TransportLease` を持ちます。Agent は `Agent::transports()` で数えます。ほかのトランスポートも同じ型を使えます。

- トランスポートは、ECU（`Peer`、`Simulation`、`LocalBench`）より後に宣言して、先に破棄します。
- `emplace()` したオブジェクトは Agent より後に破棄されるので、トランスポートを ECU に `emplace()` しません。
- ECU が Agent を破棄するときに `TransportLease` が残っていれば、ECU の名前と直し方を標準エラーに出力して `std::terminate()` を呼びます。

`std::terminate()` にするのは、破棄した Agent を使わせないためです。デストラクターなので、例外にはしません。

### 変数が反映される時点

書き込みは、`at` 以後の最初のステップで、アプリの処理の前に反映されます。そのステップのネットワークの受信処理とコンポーネントのタイマーは、反映より前に実行されるので、新しい値を使うのは次のステップからです。読み出しは、アプリと LIN の処理の後です。`Peer` と `LocalBench` で同じ時点です。

`Peer` は止まるとき（`stop()` と破棄）、ステップがもう実行されなくなった後に、待っている書き込みを `Cancelled` にします（`Agent::finish()`。2 回目以降は何もしない）。その後で止めたトランスポートは、その結果も送信します。`LocalBench` の ECU では、待っている書き込みの結果は返りません。トランスポートは ECU より先に破棄するためです。利用者は `finish()` を呼びません。

### `every()` と `at()`

| 関数 | 呼ぶステップ | ルールのある所 |
|---|---|---|
| `every(period, fn)` | 0、period、2 period … の時刻以後の最初のステップ（1 ステップに 1 回まで） | `vehicle_sil::periodic()`（`core/periodic.hpp`） |
| `at(time, fn)` | 開始時刻が `time` 以後の最初のステップで、1 回だけ | `vehicle_sil::once_at()`（同じヘッダー） |

どちらのルールも 1 か所にあり、`Peer` と `LocalBench` の ECU が共用します。

- `every` の period はステップ幅の倍数でなくてもよく、`TickScheduler` と違って、周期とステップ幅の関係を検査しません。
- `at` は、ステップ幅が `time` を割り切らなくても呼び、`time` が最初のステップより前なら最初のステップで呼びます。
- `at` の関数は、`on_step` と `every` と同じリストに、追加した順に並びます。
- `at` に空の関数を渡すと `std::invalid_argument` です。
- `at` は、ECU が決まった時刻に 1 回だけ行う操作（購読を始める、止める、`Node::send()` で送信する）に使います。
- `Node::send()` や送信するコンポーネントの関数はステップの中でだけ使えるので、`main` から直接ではなく `at()` で呼びます。

### 生の CAN（`can::RawChannel`）

J1939 でないプロトコルの基盤です。`on_frame(handler)` で受信し、`on_transmit(handler)`（セットアップ時のみ）で送信完了・失敗を受け取り、`send(frame)` または `send(frame, token)` で送信します。`TransmitEvent`、`outstanding` の保存則、`RawChannelOptions` の容量導出の詳細は [can.md](can.md#生チャネルrawchannel) にあります。

| 項目 | 仕様 |
|---|---|
| `send()` を呼べる所 | ステップの中だけ（それ以外は `std::logic_error`） |
| `send()` の結果 | `RawSendStatus`（`Ok`、`QueueFull`、`InvalidFrame`。同期の拒否はイベントを出さず、同じトークンで再試行できる） |
| 送信される時点 | そのステップの最後に、`send()` を呼んだ順に、ステップの時刻で |
| `token` と `transmit_capabilities()` | チャネル単位の不透明な `std::uint64_t`（`0` も有効）。`transmit_capabilities()`（`None` または `Correlated`）は `start()` 前の構築時から参照できる |
| 送信通知（`on_transmit()`） | 送信ステップ `T` では決して呼ばず、`T' > T` の `process()` で `on_frame()` より先に呼ぶ（`time == T` の `flush()` 拒否を含む） |
| 上限（`RawChannelOptions`） | 1 ステップあたり送信 64、受信 1024。加えて任意の `transmit_event_capacity`、`port_inbox_capacity`、`port_event_capacity`、`port_transmit_queue_capacity`、`port_bitrate` |
| 上限を超えた分 | 数えて捨て、再試行はしない（`send()` が受け付けた後に `flush()` で失敗したフレームは次のステップで `TransmitEvent::Kind::Failed` を出す） |
| 自分が送信したフレーム | 受信しない |

同じ ECU が同じネットワーク名で `j1939()` と `can()` を持つと、別のコントローラーとして互いのフレームを受信します。受信するのは次のステップですが、時刻は送信したステップのものです。J1939 のネットワークは 11 ビットのフレームを無視します。ただし、`Peer` の J1939 のネットワークが利用者のポート（`j1939(name, port)`。Vector XL など）の上にあるときは、別のバスになります。生チャネルは常に SIL Kit の上にあるためです。

違う送信元から同じ時刻に受信したフレームの順は、SIL Kit ではほかの participant のスレッド次第で、仮想バスでは `StepDriver` の順です。`RawChannel` は並べ替えません。

### 公開の関数と仮想関数

`j1939()`、`can()`、`lin_master()`、`lin_slave()`、`ethernet()` は非仮想の関数で、空の名前を検査します。実装は private の仮想関数（`add_j1939`、`add_can`、`add_lin_master`、`add_lin_slave`、`add_ethernet`）で行います。

`Peer` には、引数が `J1939ChannelOptions` の `j1939(name, ...)` が別にあります。この宣言が基底クラスの同じ名前の関数を隠すので、`Peer&` から呼べる `j1939` は `Peer` のものだけです。`NetworkOptions` を渡す `j1939(network, NetworkOptions)` は `Ecu&` を通して使います。

### LIN のノード

`lin_master(network, schedule)` は、ECU のステップ幅で動く `lin::LinMaster` を作って返します。`lin_slave(network)` は `lin::LinSlave` を作って返します。ハンドラー（`on_result()`、`on_state()`、`on_frame()`）は実行の前に設定します。ノードは最初のステップの前に初期化されます（`init()`）。

| 実装 | ノードを作る所 | 制限 |
|---|---|---|
| `Peer` | LIN のネットワーク名を名前にした SIL Kit の LIN コントローラーと `SilKitLinPort` | 仮想時間だけ（実時間では `std::logic_error`） |
| `LocalBench` | その名前の `VirtualLinBus` のエンドポイント | 1 つのバスに Master は 1 つで、2 つ目は、両方の ECU の名前をメッセージに含む `std::logic_error` |

スケジュールとオプションのエラーは、エンジンの検査による `std::invalid_argument` です。`vehicle_sil::lin` は SIL Kit を含まないので、`vehicle_sil::ecu` と `vehicle_sil::local` は SIL Kit なしのままです。

### イーサネットのチャネル

`ethernet(network, options)` は、生のフレームを送受信する `eth::Channel` を作って返します。扱うのはフレームの単位です。vehicle-sil は TCP/IP のスタックを持たず、送信元のアドレスより後ろのバイトを解釈しません。チャネル、上限、未確認の値は [ethernet.md](ethernet.md) にあります。

| 項目 | 仕様 |
|---|---|
| 受信（`on_frame(handler)`） | `process()` の中で `on_transmit()` ハンドラーの後に、受信した順に呼ぶ（実行の前に設定する） |
| 送信通知（`on_transmit(handler)`） | `T' > T` の `process()` で `on_frame()` より先に呼ぶ（送信ステップ `T` では呼ばない。`transmit_capabilities()` は仮想時間で `Correlated`） |
| 受信（`receive()`） | そのステップの分を、`on_step()` などから 1 つずつ取る |
| 送信（`send(frame)` / `send(frame, token)`） | ステップの中でだけ呼べ（`0` も有効なトークン）、フレームはそのステップの最後に送信される |
| フレームを受信するチャネル | 同じネットワークのほかの全部のチャネル（宛先のアドレスによらない。ハブと同じ） |
| 受信するステップ | 時刻 t のステップで送信したフレームは、受信する ECU の「t より後に始まる最初のステップ」 |
| 同じ時刻のフレームの順 | 送信元のアドレスの順で、`Peer` と `LocalBench` で同じ順になる |
| `options.mac` | `describe()` の出力に入るアドレス（`mac`）で、`Accept::Addressed` の絞り込みにも使い、フレームには書き込まない |

| 実装 | チャネルを作る所 |
|---|---|
| `Peer` | ネットワーク名を名前にした SIL Kit の Ethernet コントローラーと `eth::SilKitEthPort` |
| `LocalBench` | その名前の `eth::VirtualEthernetBus` のエンドポイント |

`Peer` のチャネルは仮想時間だけです。実時間では、することをメッセージに含む `std::logic_error` です。コントローラーは CommunicationReady で有効にします。

どちらの実装も、`ChannelOptions::port_inbox_capacity`（既定は `receive_capacity` の 2 倍）、`port_event_capacity`（`0` のとき `max(1024, 2 * send_capacity, effective_transmit_event_capacity, port_transmit_queue_capacity)` 以上）、`port_transmit_queue_capacity`（`0` = バックエンド既定）をポートに渡します。ネットワークから 1 ステップに受信するフレームが `receive_capacity` 以下なら、ECU を実行する順やスレッドのタイミングによらず、フレームを捨てません。

オプションのエラー（`eth::Channel::validate()`、および `Peer` では `eth::SilKitEthPort::validate()`）は、コントローラーやネットワークを作る前に `std::invalid_argument` で拒否するので、同じ名前で呼び直せます。

### バスからの切断（`ecu.connected`）

`ecu.connected` は、どの ECU にもある bool の入力で、既定は true です。false の間、その ECU のすべてのバス（J1939、生の CAN、LIN、イーサネット）について、ECU が送信したものはバスに出ず、バスのものを ECU は受信しません。ECU のステップ、タイマー、変数は動き続けます（プロセスは止めません）。

実装は、ECU のポートを `detail::GatedCanPort`、`detail::GatedLinPort`、`detail::GatedEthPort`（`ecu/detail/bus_connection.hpp`）の後ろに置きます。`TapPort` はその上にあるので、スタックが見たものを記録します。

#### 反映される時点

書き込みは、ほかの変数と同じくステップ P の先頭（アプリの前）で反映され、そのステップの送信から有効になります。

受信は、時刻 t のイベント（フレーム、LIN のヘッダーなど）を、t より前のステップで最後に決まった状態で判定します。P の変更は、P より後の時刻のイベントから有効になります。時刻 P のイベントが ECU 自身のステップ P の前に来るか後に来るか（SIL Kit、`LocalBench` の ECU の順）によらず、同じ結果になります。

LIN の Master が受信する、自分のヘッダーへの応答は、ヘッダーを送信したときの状態で判定します。ゲートが、そのスロットのヘッダーを送信したかを覚えます。スロットの途中のステップで切断しても、送信したヘッダーへの応答は、いつ受信しても ECU に渡します。

同じステップで false の後に true を書き込んだときは、切断していなかったものとして扱います。何も遮らず、再接続したときの送信もしません。

#### CAN

`detail::GatedCanPort` は `transmit_capabilities()` と 2 引数版の `send(frame, token)` を配下のポートへ転送します。

| 切断している間のこと | 起きること |
|---|---|
| 送信 | ポートの `SendResult::Failed` になる |
| J1939 の `Network` | `send_failures` に数えて捨てる（ためないので、再接続したときにまとめて送信されない） |
| 生チャネル | `frames_dropped` に数え、トークンを漏らさないよう次のステップの `process()` で `TransmitEvent::Kind::Failed`（`send_time = T`、`time = T`）を出す |
| J1939 のトランスポートプロトコル | 相手の応答が来ないので、タイムアウトで終わる |
| BAM の送信側 | パケットが送信されなくても、送信し終えたことになる |
| バスの状態（`bus_state`） | 変えない |

仮想バスに時間のモデル（`CanBusModel`）があると、切断する前に受け付けられて送信待ちのキューに残ったフレームは、切断した後にバスに出ます。実機でコントローラーを外した場合とは違います。

#### イーサネット

`detail::GatedEthPort` は `transmit_capabilities()` と 2 引数版の `send(frame, token)` を転送します。時点のルールは CAN と同じです。

- 切断している間の送信は、ポートの `SendResult::Failed` で、チャネルは `frames_dropped` に数えて捨て、トークンが漏れないよう次のステップの `process()` で `TransmitEvent::Kind::Failed`（`send_time = T`、`time = T`）を出します。
- 捨てるのはステップの最後なので、`send()` の戻り値は `Ok` のままです。
- 切断している間の時刻のフレームは、チャネルに渡しません。
- 再接続したときに送信するものはありません。

#### J1939 で再接続したとき

ネットワークごとに `NetworkOptions::on_reconnect`（`j1939::OnReconnect`）で選びます。どちらも規格の想定と合うかは確認していないので、模擬する ECU に合わせて選びます。電源の入れ直しではなく、接続し直しとして扱います。

| 値 | 再接続したステップですること | 切断している間に同じアドレスを取ったノードとの競合 |
|---|---|---|
| `Announce`（既定） | NAME を持ち Address Claim を始めた各ノードが、グローバル宛ての Request for Address Claimed と、自分の Address Claimed（アドレスがなければ Cannot Claim）を送信する（`detail::announce_again()`） | ほかのノードの応答で、ふつうの Claim と同じに解決する |
| `Nothing` | 何も送信しない | どちらかが送信するまで見つからない |

`Announce` では、ノードのアドレスと状態はそのままで、Claim の後の 250 ms の待ちはしません。

Address Claim を最初からやり直す（待ちを含む）選択肢はありません。ノードの Claim を始め直す公開の関数がないためです。

#### LIN

| 切断した ECU | 起きること |
|---|---|
| Master | スケジュールを実行し続けるが、ヘッダーも自分のフレームもバスに出ず、すべてのスロットの結果が `NoResponse` になる |
| Slave | ヘッダーも Master のフレームも受信せず、何も応答しないので、Master から見てそのスロットは `NoResponse` になる |

- 切断した ECU の Sleep と Wakeup の要求も、バスに出ません。
- 切断している間に Master が休止に入ると、SIL Kit のコントローラーは動作中のままで、LinMaster の状態と一致しないことがあります（その後の起床は確認していません）。

### `describe()` の出力

`describe()` は、次のものを `EcuDescription` で返します。

- ECU の名前
- ステップ幅
- 接続するネットワーク（呼んだ順。J1939 のノードは追加したときの値、イーサネットは `options.mac` に書いたアドレス）
- 変数（`ecu.connected` が先頭。Agent を作る前でも）

宣言を閉じず、いつでも呼べます。`Peer` と `LocalBench` で同じ内容です。`to_json()` が、決まった形の JSON にします。内容と形式は [ecu-executable.md](../ecu-executable.md) にあります。

### スレッド

組み立ては 1 つのスレッドから行います。ステップは実装のスレッドで実行され、関数、ハンドラー、コンポーネントもそこで呼ばれます。

## `Simulation`（`vehicle_sil::Simulation`）

`Ecu` を、プロセス内の仮想バスか、SIL Kit の participant として仮想時間で動かし、失敗を例外で報告します（`vehicle_sil::simulation`。ヘッダーは SIL Kit のヘッダーを含みません）。実行する場所はコンストラクターで選び、ECU のコードは変わりません。`Simulation` はコピーもムーブもできません。

### コンストラクター

| 書き方 | 実行する場所 |
|---|---|
| `Simulation()` | プロセス内の仮想バス |
| `Simulation(SilKitOptions)` | いつも SIL Kit |
| `Simulation(std::optional<SilKitOptions>)` | 値があれば SIL Kit、空なら `Simulation()` と同じ（実行する場所を実行時に選ぶときに使う） |
| `Simulation sim({.registry_uri = uri})`（型名を書かない） | SIL Kit |
| `Simulation sim{{}}` | どちらのコンストラクターとも取れるので、コンパイルエラーになる（暗黙にプロセス内にはならない） |

### 実行する場所による違い

| | `Simulation()` | `Simulation(SilKitOptions)` |
|---|---|---|
| ECU の実体 | `LocalBench::add_ecu()` のホストで、`run_for()` を呼んだスレッドで進む | `Peer`（Coordinated、仮想時間、ECU の名前の participant）で、SIL Kit のスレッドで進む |
| `run_for(d)` | 何度でも呼べて、続きから進む | 1 回だけ（2 回目と、`controller = false` では `std::logic_error`） |
| `run()` | `std::logic_error` | 誰が終わらせてもよいシミュレーションの終わりまで待つ（1 回だけ） |
| 取り出す | `local()` で `LocalBench`、`ecu(name)` で `Ecu` | `peer(name)` で `Peer`、`registry_uri()`、`ecu(name)` で `Ecu` |

### `run_for()` が実行するステップ

`run_for(d)`（d > 0）は、終わりの時刻より前に始まるステップだけを実行します。仮想バスでの終わりの時刻は、今の時刻から `d` 進めた時刻です。SIL Kit では、controller の停止を受信するまで、ほかの participant が stop_at 以降のステップを実行することがあります。そのため、各 ECU に同じ上限を置き、仮想バスと同じ記録になるようにしています。

SIL Kit の `Simulation` が 1 回しか実行できないのは、すべての participant を時刻の境界で止めても、その境界のステップが実行中のことがあるためです。メインスレッドからモデルを読むと競合し、仮想バスとは境界のステップが 1 つずれます。止めながら確認するテストは、仮想バスで行います。

### 失敗の報告

ECU のステップの例外は `SimulationError` で報告します。`local()` から追加した ECU の失敗も同じです。`what()` は、失敗した ECU ごとの `<ECU 名>: <理由>` を `; ` で区切ったものです。

| 実行する場所 | 失敗したときの動き |
|---|---|
| 仮想バス | 失敗した後の `run_for()` も `SimulationError` を投げる |
| SIL Kit | `run_for()` と `run()` が、短い間隔で失敗を監視し、見つけたらシミュレーションを終えて `SimulationError` を投げる |

SIL Kit で監視が必要なのは、participant が失敗しても SIL Kit はシミュレーションを終えず、仮想時間が止まるだけのためです。全体のタイムアウトはありません。

始めるときの例外（必要な participant がそろわない、`start_timeout`）は、全部を止めてからそのまま伝えます。SIL Kit では、`run_for()` は全 participant が止まった後に戻ります。ステップが書いたものは、それから読めます。

### `run()` が失敗として扱うもの

`controller = true` のときは、次のとおりです。

| participant の状態 | 扱い |
|---|---|
| Error で離脱した | すぐ失敗にする |
| Error 以外で離脱し、数秒のうちにシミュレーションを終えない | 失敗にする |

数秒待つのは、`request_stop()` で終わるシミュレーションを、間違って失敗にしないためです。

`controller = false` では、外の controller の仕事は見ず、このプロセスの ECU の失敗だけを見ます。失敗した ECU があるときの動きは、次のとおりです。ECU が 1 つだけのプロセス（`ecu_main`）でも同じです。

1. このプロセスの ECU は、失敗した ECU も含めて、外の controller がシミュレーションを中断するまで（最大 5 秒）、どれも止めずに待ちます。その間、失敗した ECU は Error のままです。
2. 待っている間に求められた停止（`request_stop()`、`ecu_main` への停止の要求）は、そのまま渡します。
3. 5 秒たっても中断も停止もされなければ、追加した順によらず、失敗した ECU を先に止め、次に残りを止めます。

どれも止めずに待つのは、controller に失敗が見えるようにするためです。失敗した ECU を先に止めると Error を離れ、失敗していない ECU を先に止めるとふつうの停止になり、どちらも controller に失敗が見えません。

失敗した ECU が Shutdown になった後の停止は、ふつうの停止です。そのため、ほかの participant も含めてシミュレーション全体が止まります（Error の participant を残したまま、ほかのプロセスを残して終わることはしません）。

### `SilKitOptions`

| メンバー | 内容 |
|---|---|
| `registry_uri` | 空なら、コンストラクターがプロセス内に registry を立てる |
| `controller` | 既定 `true`（participant 名は `SimulationController`） |
| `other_participants` | 始める前にそろうべき、ほかの Coordinated な participant（`controller` が `true` のときだけ使える） |
| `start_timeout` | 既定 30 秒 |

### 引数と呼び出しの検査

| 条件 | 結果 |
|---|---|
| `other_participants` の名前が空、重複、`SimulationController` | `std::invalid_argument` |
| `add_ecu()` の名前が空、使用済み、controller か `other_participants` の名前 | `std::invalid_argument` |
| `add_ecu()` の名前が `SimulationController`（`controller = false` でも、仮想バスでも） | 拒否する |
| ECU が 1 つもないまま `run_for()` か `run()` を呼ぶ | どちらの実装でも `std::logic_error` |
| ほかの ECU が別の種類（CAN か LIN）で使ったネットワークの名前を使う | どちらの実装でも `std::invalid_argument` |

1 つのネットワークの名前は、`Simulation` のすべての ECU にとって、CAN（`j1939()`、`can()`）か LIN（`lin_master()`、`lin_slave()`）のどちらか一方のネットワークです。違反したときの例外のメッセージには、両方の種類が入ります。SIL Kit では、使っている ECU の名前も入ります。SIL Kit で確認するのは、このプロセスの ECU の間だけです。ほかのプロセスの participant の名前は見えません。

### `add<App>()`

`add<App>(name, step, args...)` は、`add_ecu()` と、`App{ecu, args...}` を ECU に持たせる `emplace` です。

`App` のコンストラクターが例外を投げると、その例外が伝わり、その `Simulation` は使えなくなります。コンストラクターが ECU に追加したものが、破棄された `App` を参照しうるためです。以後の `add_ecu()`、`add()`、`run_for()`、`run()` は、ECU の名前を添えた `std::logic_error` です。

### 所有、破棄、スレッド

`Simulation` が ECU とその持ち物を持ち、返す参照は `Simulation` の寿命まで有効です。破棄は、後から追加した ECU から先に ECU を破棄し、次に controller、最後にプロセス内の registry です。デストラクターは全部を止め、例外を投げません。

`Simulation` の関数は 1 つのスレッドから呼びます。SIL Kit では、ステップは SIL Kit のスレッドで実行されます（`Peer` と同じ）。

## Peer（`vehicle_sil::Peer`）

1 つの SIL Kit participant と、その中で動くものをまとめて作り、動かし、正しい順序で破棄します。中で動くものは、CAN の上の J1939 の `Network` と生の CAN、LIN の Master と Slave、生のイーサネットのフレームのチャネル、ステップごとのアプリの処理です。`Ecu` を実装するので、`Ecu&` を受け取る ECU のコードを、そのまま使えます。

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

複数の ECU の組み立ては、`Ecu&` を受け取る関数やクラスに書くと、同じコードを `Peer` と `LocalBench` の両方で使えます（例: `examples/j1939/ecu_pair`。テストで両者の結果が同じことを確認しています）。`Simulation` は、これを 1 行で選べるようにしたものです。

### Peer の組み立ての関数

`Peer` が実装する組み立ての関数が作るものは、次のとおりです。

| 関数 | 作るもの |
|---|---|
| `j1939(network)` | SIL Kit の CAN コントローラー、`SilKitCanPort`、`TapPort`、`Network` |
| `j1939(name, port)` | 利用者が作ったポート（Vector XL の `ExternalPortAdapter` など。Peer が所有する）の上の `Network` |
| `can(network)` | 生の CAN のチャネルと、SIL Kit の CAN コントローラー 1 つ（名前は `<network>.raw`） |
| `Ecu::lin_master(network, schedule, options)`、`Ecu::lin_slave(network, options)` | LIN のネットワーク名を名前にした SIL Kit の LIN コントローラー、`SilKitLinPort`、LIN のノード |
| `Ecu::ethernet(network)` | ネットワーク名を名前にした SIL Kit の Ethernet コントローラー、`eth::SilKitEthPort`、`eth::Channel` |
| `on_step(fn)` | 毎ステップ呼ぶ関数 |
| `every(period, fn)` | 0、period、2 period … の時刻以後の最初のステップで呼ぶ関数（1 ステップに 1 回まで。`vehicle_sil::periodic()` のルール） |
| `at(time, fn)` | `time` 以後の最初のステップで 1 回だけ呼ぶ関数（`vehicle_sil::once_at()` のルール） |

組み立て（`j1939`、`can`、`lin_*`、`ethernet`、`on_step`、`every`、`emplace`）は `start()` の前だけです（後は `std::logic_error`）。ノード、ハンドラー、コンポーネント、Observer、`TapPort` のリスナーも `start()` の前に付けます。ノードは、`Network::add_node()` のルールどおり、ステップの中からも追加できます。

作ったものは、次の関数で取得します。

| 関数 | 対象 |
|---|---|
| `network()`、`network_names()`、`tap()`、`silkit_port()` | J1939 だけ（`tap(name)` で生フレームを観測でき、`network_names()` は追加した順の名前の一覧を返す） |
| `raw_channel(network)` | 生の CAN のチャネル |

LIN とイーサネットのコントローラーは、communication ready の通知で始めます。

- LIN のノードは、communication ready の通知で `init()` を呼びます。
- SIL Kit のコントローラーは作ると外せないので、オプションを先に検査します（LIN の名前、スケジュール、オプションは `LinMaster::validate()` と `LinSlave::validate()`、生の CAN は `can::RawChannel::validate()` と `can::SilKitCanPort::validate()`〔仮想時間での `transmit_queue_capacity <= kMaxCorrelatedInFlight` を含む〕、イーサネットは `eth::Channel::validate()` と `eth::SilKitEthPort::validate()`）。`can()` は `RawChannelOptions::port_bitrate` を `SilKitCanPortOptions::bitrate`（`CommunicationReady` で `SetBaudRate()`）へ渡します。
- 仮想時間の `SilKitCanPort` と `SilKitEthPort` は、実行時の探索や、シミュレーターの離脱・`userContext` 省略による動的ダウングレードを行わず、静的に `TransmitCapabilities::Correlated` を返します。未確認のままのフレームや非対応の確認は `tx_uncorrelated` と `outstanding > 0` で観測できます。
- 例外を投げた `can()`、`ethernet()`、`lin_master()`、`lin_slave()` は何も残さず、同じ名前で呼び直せます。
- Ethernet のコントローラーは communication ready の通知で有効にし（`SilKitEthPort::start()`）、失敗したら、ネットワークの名前をメッセージに含めて participant を Error にします。

コントローラー名の重なりは、次のとおり拒否します。

| 重なり | 結果 |
|---|---|
| 生の CAN のコントローラー名 `<network>.raw` と、同じ Peer のほかのコントローラー名 | `std::invalid_argument` |
| J1939 のコントローラー名どうし | 拒否する |
| CAN のコントローラー名と LIN のコントローラー名（LIN のネットワーク名） | 拒否する |
| Ethernet のコントローラー名（ネットワーク名）と CAN のコントローラー名 | どちらの順でも拒否する |

### Peer の 1 ステップの順序

| 順 | すること |
|---|---|
| 1 | J1939 の各 `Network`、生の CAN、イーサネットのチャネルが、追加した順に、受信して処理する |
| 2 | Control Agent の書き込みを反映する |
| 3 | アプリの処理（`on_step`、`every`、`at`）を、追加した順に呼ぶ |
| 4 | LIN のノードの `step(now)` を、追加した順に呼ぶ |
| 5 | 監視を記録し、Agent が読み出す |
| 6 | J1939、生の CAN、イーサネットが送信する |

Agent の前後の処理は、`on_step` や変数をいつ追加したかによりません。アプリが出した LIN の要求（`set_frame_data()`、`set_response()`）は、同じステップから有効になります。LIN のノードが受信したものは、そのステップの LIN の処理で通知されるので、アプリは次のステップで読みます（`Ecu` の順序と同じです）。

### 時間のモード

| `virtual_time` | 動かし方 | 使えるバス |
|---|---|---|
| ある | SIL Kit の時間同期で実行する | J1939、LIN、イーサネット |
| ない | 実時間で実行し、Peer が `RealTimeDriver` と時計（`SteadyRealTimeClock`、または `PeerOptions::real_time_clock`）を持って、SIL Kit の CAN ポートを実時間にする | J1939 だけ（LIN とイーサネットは `std::logic_error`） |

外部のポートの時間の領域が Peer と違えば `std::logic_error` です。SIL Kit を持たない構成（XL だけ）はありません。

### lifecycle

| モード | 開始と停止 |
|---|---|
| `Coordinated` | `SimulationController` が開始と停止を決める |
| `Autonomous` | `stop()` か、ステップの中からの `request_stop()` で止まる |

- 1 つのプロセスで複数の Peer と controller を起動するときは、`start_together(controller, {&a, &b})` を使います。
- `Autonomous` の participant どうしは開始の時刻がそろうとは限らないので、複数の participant を同じ時刻から決定的に動かすときは `Coordinated` にします。
- 仮想時間で `virtual_time.end` があれば（0 より後であること）、end の前の最後のステップの後で自分で止まります。
- `wait()` は lifecycle の終わりを待ちます。
- Coordinated では、ほかの participant の `start()` が、自分の最初のステップの前にシミュレーションの終わりを見て、開始の失敗として例外を投げることがあります。

#### Running になる前の `request_stop()`

lifecycle が Running になる前（`start()` の前や、ほかの participant を待っている間）の `request_stop()` は、保持しておきます。SIL Kit が Running の前の停止を無視するためです。保持した要求を渡す時点は、次のとおりです。

| 場合 | 停止を渡す時点 |
|---|---|
| 仮想時間 | 最初のステップ |
| 仮想時間で、Running になった後、最初のステップの前に要求が来た | 最初のステップまで待つ（ふつうはすぐ来る） |
| 仮想時間で、Error のとき | すぐ渡す |
| 実時間 | Running になったとき |
| 実時間で、`start()` がタイムアウトで例外を投げた後に Running になった | 保持した要求は渡らないので、`stop()` で止める |

`stop()` は、最初のステップを待たずに停止を渡します。

仮想時間で最初のステップまで待つのは、最初のステップには、時刻で同期するすべての participant が時刻を始めている必要があるからです。まだ Running でない participant を残して止めると、その participant が止まれなくなることがあります。controller が仮想時間を使わない構成では、controller はこの同期に入らないので、保証しません。

#### `Autonomous` と `Coordinated` を混ぜたとき

`Autonomous` で仮想時間の participant が先に仮想時間を進めていると、後から `Coordinated` の participant が始まった時点で、SIL Kit がシミュレーション全体を abort します。先にいた participant も終わります。SIL Kit のドキュメントの 1 か所は「その participant が Error になる」と書いていますが、v5.0.8 の実装と変更履歴は abort です。途中から参加できるのは `Autonomous` の participant だけです。

#### 失敗

`failed()` と `error()` は、ステップ（ホスト、アプリ、LIN、ポート）、ステップのハンドラー、実時間のドライバーの失敗を返します。ステップが失敗すると、仮想時間でも実時間（ドライバーが止まる）でも participant は Error になり、`wait()` が戻ります。

失敗の後の `request_stop()` は、Error になってから停止を渡します。実時間では、`failed()` が見えてからドライバーが失敗を報告するまでの間の要求も含みます。

### Peer の所有と破棄

Peer が participant、コントローラー、ポート、`Network`、LIN のノード、Control Agent を持ち、返す参照は Peer の寿命まで有効です。

`stop()`（と破棄）は、次の順で止めます。

1. Agent が実時間のドライバーを起こすのをやめさせる。
2. ステップを止める（実時間ではドライバー、続いて lifecycle。仮想時間では lifecycle）。以後、ステップは来ません。
3. SIL Kit のポートを閉じる。
4. Agent の待っている書き込みを `Cancelled` にする。

`stop()` の後も `Network`、チャネル（`RawChannel`、`eth::Channel`）、Agent は残るので、統計（終了時に未確認のまま残った送信中フレームを表す `outstanding` を含む）、記録、最後の値を読めます。

破棄では、続いて次の順に破棄します。

1. LIN のノード（それぞれ、その `SilKitLinPort` より先）
2. `Network`（コンポーネントが先）
3. `TapPort`
4. ポート（外部のポートは、そのデストラクターで閉じる）
5. 生の CAN とそのポート
6. ホスト
7. participant
8. Agent
9. `emplace` したオブジェクト（最後に追加したものから先）

`emplace` したオブジェクトは participant より後に破棄されるので、そのデストラクターは SIL Kit のサービスを使ってはいけません。

### Peer のスレッド

| すること | スレッド |
|---|---|
| 組み立て、`start()`、`wait()` | 1 つのスレッドから呼ぶ |
| ステップ | SIL Kit のスレッド（仮想時間）か、ドライバーのスレッド（実時間）で実行され、ハンドラー、アプリの処理、Observer、`TapPort` のリスナーもそこで呼ばれる |
| `stop()` | ステップと SIL Kit のハンドラー以外の、どのスレッドからでも呼べる（別のスレッドからは `start()` が戻った後） |
| `request_stop()`、`failed()`、`error()` | ステップの中を含めて、どこからでも呼べる |

記録は、`wait()` か `stop()` の後に読みます。`stop()` の同時の呼び出しは、順に処理します。Peer の破棄は、ほかのスレッドの `stop()` が戻った後に行います。

## ローカルの実行（`vehicle_sil::LocalBench`）とテストキット（`vehicle_sil::testkit`）

SIL Kit を使わずに、仮想バスと `StepDriver` の上で ECU を動かし、J1939 のテストを組み立てます。何も時計を読まず、呼んだスレッドで進みます。`LocalBench`（`vehicle_sil::local`、ヘッダーは `vehicle_sil/local/local_bench.hpp`）が仮想バスと ECU を持ち、`vehicle_sil::testkit` はテストのツール（記録と故障注入）を追加します。`testkit::LocalCan` は `LocalBench` の別名です。

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

### バスと ECU

`LocalBench` は、`VirtualCanBus`（`VirtualCanBusOptions` でタイミングも選べる）と `lin::VirtualLinBus` をネットワーク名ごとに持ち、その上の ECU を持ちます。

| 関数 | すること |
|---|---|
| `add_ecu(name, step)` | ECU を作り、`Ecu&` を返す |
| `Ecu::j1939(network)` | 名前 `network` のバスにエンドポイントを追加し、`TapPort` で包んで `Network` を作る |
| `Ecu::can(network)` | オプションを検査し、生チャネルを作り、そのバスに `VirtualCanBusEndpointOptions`（`port_inbox_capacity`、`port_event_capacity`、`port_transmit_queue_capacity` 付き。`port_bitrate` は `LocalBench` では未使用）で自分のエンドポイントを接続する |
| `add_j1939(name, step, options)` | J1939 のネットワークを 1 つ持つ ECU を作る略記 |

- ECU ごとに自分のホストとステップ幅を持ちます（SIL Kit で ECU ごとに participant を分けたときと同じ構成です）。
- `Ecu::j1939(network)` は、名前 `network` のバスがなければ、コンストラクターのオプションで作ります。`VirtualCanBusOptions::inbox_capacity` はチャネル由来の容量を持たないエンドポイント（`Ecu::j1939()`、`injector()`、直接の `bus().attach()`）向けのバス既定値であり、`Ecu::can()` は（`Ecu::ethernet()` や `Peer::can()` と同じく）`RawChannelOptions::port_inbox_capacity` と `RawChannelOptions::port_event_capacity` からエンドポイント容量を導出します（テストで `LocalBench` と `Peer` の受信キュー容量をそろえて絞るには `RawChannelOptions::port_inbox_capacity` を使います）。
- 同じ名前を使う ECU は、同じバスを共有します。
- ECU は、複数のバスに `Network` を持てます（ゲートウェイ）。

### バスの名前を省く形

`add_j1939()`、`inject(time, frame)`、`injector()`、`bus()` は、ただ 1 つのバスを使います。バスに名前を付けるのは ECU だけです。

| 状況 | 起きること |
|---|---|
| バスがまだないときに、名前を省く形を使う | 名前の決まっていないバスを作る（名前の検索では `""`） |
| 名前の決まっていないバスがあるときに、`Ecu::j1939(network)` か `Ecu::can(network)` を呼ぶ | そのバスがその名前になり、新しいバスは作らない |
| 名前つきのバスが 1 つあるときに、名前を省く形を使う | そのバスを使う |
| 2 つ目の名前を使う | 2 つ目のバスを作る |
| 2 つ目の名前を使うまでに、`add_j1939()` で ECU を追加していた | `std::logic_error` |
| バスが 2 つ以上あるときに、名前を省く形を使う | `std::logic_error`（メッセージにバスの名前の一覧が入る） |

- 名前を省く形と名前つきの形は、どちらの順で書いても同じバスになります。
- 2 つ目の名前で `std::logic_error` になるのは、`add_j1939()` で追加した ECU がどのバスのつもりか決まらないためです。
- そのメッセージには、その ECU の名前と、今あるバスの名前が入ります。
- その ECU は、`add_ecu(name, step).j1939(network)` で追加し直します。
- 例外を投げた `add_j1939()`、`j1939()`、`can()` は、バスを作らず、名前も付けません。
- 生チャネルには名前が必要です（`Ecu::can("")` は `std::invalid_argument`）。

### 仮想のイーサネット

`Ecu::ethernet(network)` は、バスを作ったり名前を付けたりする前に `ChannelOptions` を検査し、名前 `network` の仮想のイーサネットのネットワークにエンドポイントごとの容量（`port_inbox_capacity`、`port_event_capacity`、`port_transmit_queue_capacity`）付きでエンドポイントを接続して `eth::Channel` を作ります。ネットワークは `eth::VirtualEthernetBus` で、ECU が最初にその名前を使ったときに、既定のオプションで作ります。

- CAN や LIN のバスと同じ名前にすると、どちらの順でも、両方の種類をメッセージに含む `std::invalid_argument` です。
- `ethernet(ecu, network)` で ECU のチャネルを、`ethernet_bus(network)` でネットワークを取得します。
- `ethernet(ecu, network)` と `ethernet_bus(network)` に、ない名前を渡すと `std::out_of_range` です。
- テストは、最初の実行の前に `attach()` で自分のエンドポイントを付けて、ECU へフレームを送信し、ECU のフレームを見られます。

### 仮想 LIN バス

`Ecu::lin_master(network)` と `Ecu::lin_slave(network)` は、名前 `network` の LIN バスに、ECU の名前のエンドポイントを付けてノードを作り、すぐ初期化します。LIN バスは、ECU が最初にその名前を使ったときに作ります。LIN のノードのステップは、ECU のアプリの処理の後です。

| 条件 | 結果 |
|---|---|
| CAN バスと LIN バスを同じ名前で作る | どちらの順でも、両方の種類をメッセージに含む `std::invalid_argument` |
| 1 つの LIN バスに Master を 2 つ置く | 2 つ目は、両方の ECU の名前をメッセージに含む `std::logic_error` |
| `lin_bus(network)` に知らない名前を渡す | LIN バスの一覧をメッセージに含む `std::out_of_range` |

- 名前のないバスの仕組み（上の「バスの名前を省く形」）は CAN だけのもので、LIN は関わりません。
- 例外を投げた `lin_master()` と `lin_slave()` は、エンドポイントを付けず、バスも作りません。
- `lin_bus(network)`（`VirtualLinBus&`。Observer はここで追加する）、`lin_master(ecu, network)`、`lin_slave(ecu, network)` で取得します。

### 作ったものを取得する関数

| 関数 | 取得するもの |
|---|---|
| `network(ecu, network)`、`endpoint()`、`tap()`、`host(ecu)`、`ecu(name)` | ECU の名前とネットワークの名前で取得するもの（`endpoint()` は故障注入と通信断に使う） |
| `raw_channel(ecu, network)` | 生チャネル（上の関数の対象に入らない） |
| `bus(network)`、`injector(network)`、`inject(network, time, frame)` | その名前のバスと、そのバスへの注入 |
| `ecu_names()` | ECU の名前の一覧 |

- ネットワークが 1 つの ECU には、ECU の名前だけの形も使えます（0 個や 2 個以上は `std::logic_error`）。
- `bus(network)`、`injector(network)`、`inject(network, time, frame)` は名前でバスを検索するだけで、バスを作らず、名前も付けません。
- その名前のバスがなければ `std::out_of_range` で、メッセージに、今あるバスの名前か、直し方が入ります。
- 名前の決まっていないバスには、名前を省く形を使うか、ECU の `j1939(network)` か `can(network)` で先に名前を付けます。
- 名前のないバスを `bus("")` で取得することもできます。
- `inject()` は、どのネットワークも使わない専用のエンドポイントから、生フレーム（11 ビットか 29 ビット）を送信します。
- `inject()` の時刻は `now()`（最後の `run_until()` の end）より前にできません。
- `on_step(name, fn)` は、ECU のアプリの処理を追加します。

### ステップと組み立ての期限

`Ecu::on_step`、`every`、`at` は同じリストに、追加した順に並びます。`every` は `vehicle_sil::periodic()` のルールを、`at` は `vehicle_sil::once_at()` のルールを使います。

組み立ては最初の実行の前だけです（後は `std::logic_error`）。対象は、`add_ecu`、`add_j1939`、ECU の `j1939()`、`can()`、`lin_master()`、`lin_slave()`、`ethernet()`、`input()`、`output()`、`set_agent_options()`、`on_step()`、`every()`、`at()`、`emplace()` です。ホストは最初の実行で、追加した順にドライバーへ登録されます。

### `LocalBench` の変数

ECU の Control Agent は、`Peer` と同じ時点（アプリの処理の前に書き込み、LIN の処理の後に読み出し）で、呼んだスレッドのステップの中で動きます。`Peer` との違いは次のとおりです。

- 仮想時間で呼んだスレッドが進めるので、実時間のドライバーを起こす処理はありません。
- 監視の出力（`PeerOptions::monitor`）はありません。
- トランスポートは ECU より先に破棄するので、待っている書き込みの結果は返りません。
- ローカルの Agent は `submit()` と `latest()` で直接読み書きし、Control Server には接続しません（トランスポートは SIL Kit の上だけです）。
- 時刻の制御（一時停止、ステップ実行）と、Control Server を起動する API はありません。

### 破棄の順

後に追加した ECU から順に、ECU ごとに次の順で破棄します。

1. ホスト
2. LIN のノード（後に追加したものから）
3. `Network`（後に追加したものから。各 `Network` の後にその `TapPort`）
4. 生チャネル
5. イーサネットのチャネル
6. Control Agent
7. `emplace` したオブジェクト（最後に追加したものから先）

その後にドライバー、最後にイーサネットのネットワーク、LIN バス、CAN バスとエンドポイントを破棄します。

### テストキット

`J1939Events`（Observer）と `FrameLog`（TapListener）は、イベントと生フレームをコピーして持ちます。ネットワークごとに `J1939Events`、タップごとに `FrameLog` を 1 つ作れます（2 つ目は `std::logic_error`）。容量（既定 100000 件）を超えた分は持たずに数えます（`dropped()`）。問い合わせは、種類（向き）と述語で数えたり取り出したりします（`count`、`of`、`first`、`last`）。

`faults` は、仮想バスの `FaultInjector` を作る小さな関数です。

- 関数: `when(pred, action)`、`between(from, until, action)`、`nth(n, action)`、`sequence({...})`、`first_of({...})`
- 述語: `j1939_pgn(pgn)`、`j1939_source(sa)`、`can_id(id, format)`

どれも、状態は選んだフレームの数だけなので決定的です。`nth` と `sequence` のコピーは数を共有するので、エンドポイントごとに作ります。故障注入の仕組みそのものは仮想バスにあり（[can.md](can.md) の「仮想バス」）、テストキットは新しい注入の仕組みを持ちません。

LIN のテストは、仮想 LIN バスの上（`LocalBench`）でも、`Peer` と in-process の registry の上でも行えます。結果は同じです（[lin.md](lin.md) の「仮想 LIN バスの決定性」）。テストキットに、LIN の故障注入と記録のツールはありません。記録は仮想 LIN バスの観測（`LocalBench::lin_bus(network).add_observer()`）で、通信断は `ecu.connected` で行います。

## 検証の範囲

- `Peer` とテストキット（`peer.*`、`testkit.*`）: 3 つの participant での J1939 と LIN、ローカルの仮想バスと同じ結果になること、実時間と外部のポート、組み立てのエラー、失敗、破棄の順序

## 関連するドキュメント

- ECU の書き方: [j1939-guide.md](../j1939-guide.md)、[ecu-executable.md](../ecu-executable.md)
- 全体像: [architecture.md](../architecture.md)
- どこに何があるか: [code-map.md](../code-map.md)
- ほかの内部の仕組みのドキュメント: architecture.md の「[内部の仕組み](../architecture.md#内部の仕組み)」
