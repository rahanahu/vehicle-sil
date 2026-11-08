# J1939 のノードの作り方

vehicle-sil の J1939 スタックで ECU のノードを作り、動かし、試験する方法です。API の契約と規則の詳細は [architecture.md](architecture.md) の「J1939」にあります。API はまだ変わることがあります。

完全な例は [examples/j1939/ecu_pair](../examples/j1939/ecu_pair) です。エンジンとキャブの 2 つの ECU を部品だけで作り、SIL Kit の 2 つの participant として動かします。同じコードを試験でも使っています。

## 利用者が書くものと、スタックが受け持つもの

| 利用者が書くもの | スタックが受け持つもの |
|---|---|
| ノードのアドレスと NAME | Address Claim（競合、任意アドレス、Commanded Address の受信、250 ms の待ち） |
| 送る PG と周期、データの作り方 | 周期の管理、アドレスを持つ間だけ送ること、DM13 による停止 |
| 受ける PG と、受けたときの処理 | 宛先の振り分け、トランスポートプロトコル（9〜1785 バイト）の組み立て |
| Request を送る相手と、答えの使い方 | 応答待ち、時間切れ、送り直し、同じ要求の相乗り |
| モデル（ふつうの C++） | 仮想時間、ステップ、スレッド、SIL Kit の participant とコントローラ |

## ノードを作る

ノードは `j1939::Network` の上に作ります。`Network` は 1 つの CAN チャネルで、SIL Kit の participant なら `Peer::j1939(name)`、試験なら `testkit::LocalCan::add_j1939(name, step)` が作ります。ノードの組み立ては `Network&` を受け取る関数に書くと、同じコードがどちらの上でも動きます。

```cpp
j1939::Node& install_engine(j1939::Network& can, EngineModel& engine)
{
    auto& ecu = can.add_node({.address = 0x00, .name = kEngineName});      // Address Claim はスタックが行う
    codec::attach_sender<EngineStatus>(ecu, 20ms, [&engine](EngineStatus& m) { m.speed.set(engine.rpm); });
    codec::on_message<SpeedRequest>(ecu, [&engine](const SpeedRequest& m) {
        if (const auto rpm = m.speed.value()) { engine.requested = static_cast<std::uint16_t>(*rpm); }
    });
    return ecu;
}
```

`EngineStatus` と `SpeedRequest` は、YAML の定義から生成した型付きメッセージです（[j1939-codec.md](j1939-codec.md)）。信号を値として読み書きし、ビット位置やスケーリングを書きません。バイト列を直接扱うこともできます（下の各節）。

- `NodeOptions::name` を付けると、ノードは最初のステップでアドレスを要求します。NAME を付けないノードは、指定したアドレスをすぐに使います（試験用）。
- `node.attach<T>(...)` で付けた部品は `Network` が持ち、正しい順序で破棄します。ハンドラが参照するモデルは、`Peer`（や `LocalCan`）より先に宣言します。
- 部品とハンドラは、ステップを回すスレッドの上で呼ばれます。ほかのスレッドから操作するときは、ホストの `post()` で渡します。

## 受ける

```cpp
codec::on_message<SpeedRequest>(ecu, [&](const SpeedRequest& m) { engine.target_rpm = m.speed.value_or(800.0); });
ecu.on_message(kPgnRaw, [&](const j1939::Message& m) { log(m.data); });   // バイト列のまま
```

- 自分のアドレス宛てとグローバル宛てのメッセージを受けます。9 バイト以上は、トランスポートプロトコルで組み立てたものを 1 回で受けます。
- `codec::on_message<M>` は、decode できたもの（長さが足り、セレクターが合うもの）だけを渡します。
- `Message::data` は呼び出しの間だけ有効です。残すときはコピーします。
- 解除が要る登録は `j1939::Subscription` に持たせます（破棄で解除します）。
- 最新の値と途絶だけが要るなら、`Monitor` を使います（下）。

## 送る

- その場で 1 回送るときは `node.send(pgn, data, destination)` です。送れるのはステップの中（ハンドラ、タイマー、`Peer::every` などアプリの処理）だけです。結果は `SendStatus` で返ります（例外ではありません）。
- 周期的に送るときは `PeriodicSender` です。

```cpp
codec::attach_sender<EngineStatus>(ecu, 20ms, fill);                                   // 送るたびに新しい EngineStatus を fill で埋める
codec::send(ecu, request, 0x80);                                                        // 型付きメッセージを 1 回
auto& status = ecu.attach<j1939::PeriodicSender>(kPgnEngineStatus, 20ms, encoder);   // 送るたびに encoder を呼ぶ
auto& fixed = ecu.attach<j1939::PeriodicSender>(kPgnVersion, 1s, std::span(bytes)); // 決まったバイト列
fixed.set_data(new_bytes);                                                              // 次の送信から
status.trigger();                                                                       // 次の機会にすぐ送る
```

- `codec::attach_sender` は、定義の優先度と長さで送ります。
- `Encoder` は部品のバッファ（`max_length` バイト、既定 8）に書き、書いた長さを返します。9 バイト以上（`max_length` を上げる）はトランスポートプロトコルで送ります。
- ノードが送れるようになるまで（Address Claim の後の 250 ms の待ちの間も）送らず、送れるようになったときに 1 回送ってから周期を数えます。アドレスを失えば止まり、取り直せば再開します。
- `trigger()` をアプリの処理（`every` など）から呼ぶと、送るのは次のステップです。そのステップで送りたいときは `node.send()` を使います。
- 全体宛ての放送は、DM13 で放送を止めている間（他のノードの DM13 か、自分の `BroadcastSuppression::suspend()`）は送りません。1 つのアドレス宛ての送信は、既定では止めません（規格での扱いを確かめていないため）。どちらも `PeriodicSenderOptions::suppressible`（true か false）で選べます。
- PDU1 の PG を 1 つのアドレスへ送るときは `PeriodicSenderOptions{.destination = address}` です。

## Request に答える

```cpp
ecu.on_request(kPgnSomething, [&](j1939::RequestContext& r) { r.respond(encode(model)); });
```

- 応答者のない PGN への宛先指定の Request には、スタックが NACK を返します。
- ソフトウェア ID などの識別情報は `Identification` 部品が答えます（`ecu.attach<j1939::Identification>().set_software_identification(fields)`）。診断（DM1 など）は `Diagnostics` 部品です。
- 何ステップかかけて答えるときは `r.defer()` で要求を値として取っておき、後で `node.respond(pending, data)` を呼びます。

## Request を送って答えを受ける

```cpp
j1939::Requester::of(cab).request(kPgnSoftwareId, 0x00, [&](const j1939::RequestResult& r) {
    if (r.outcome == j1939::RequestOutcome::Response) { dash.software.assign(r.data.begin(), r.data.end()); }
});
```

| `RequestOutcome` | 意味 |
|---|---|
| `Response` | 要求した PG が届いた（`data`、`source`）。長い PG はトランスポートプロトコルで組み立てたもの |
| `Acknowledged` | 相手が Acknowledgement を返した（`ack` は肯定、否定、アクセス拒否、応答不能） |
| `Timeout` | 送り直しても答えがなかった（グローバルの要求では、待ち時間の終わり） |
| `NotSent` | 送れなかった（`send_status`。アドレスを要求できないときは `NoAddress`） |

- `request()` はいつ呼んでもかまいません。ノードがアドレスを持つまで待ってから送ります。
- グローバル（`kGlobalAddress`）へ送ると、待ち時間の間に届いた応答をそのたびに `last == false` で受け、最後に `Timeout` を受けます。
- 同じ PG を同じ相手に重ねて要求しても、Request は 1 つで、全員が同じ答えを受けます。待ち時間、送り直しの回数、間隔の下限は `RequesterOptions` です（`j1939::Requester::attach(node, options)` を、何かが `of()` を使う前に呼びます。`Requester` はノードに 1 つです）。

## 受信を監視する

```cpp
auto& eec = cab.attach<j1939::Monitor>(kPgnEngineStatus,
    j1939::MonitorOptions{.name = kEngineName, .timeout = 100ms,
                          .on_state = [&](const j1939::MonitorChange& c) { dash.silent = c.state == j1939::MonitorState::Lost; }});
...
if (const auto status = EngineStatus::decode(eec.data())) { use(status->speed.value()); }   // まだ受けていなければ空
```

- 送信元を NAME で指定すると、相手のアドレスが変わっても追います。
- 途絶（`Lost`）は最後の受信から `timeout` で知らせ、受信が戻れば `Receiving` に戻ります。最初の受信までは `Waiting` で、途絶にしません。

## ほかのノードのアドレスを変える

```cpp
auto& tool = service.attach<j1939::AddressCommander>();
tool.command(kCabName, 0x81, [](const j1939::CommandResult& r) { /* r.outcome == CommandOutcome::Moved */ });
```

相手が新しいアドレスで Address Claimed を送ると `Moved` です。任意アドレス可能でない相手は従いません（`Timeout`）。

## 周期的な処理の使い分け

| 使うもの | 向いているもの | 時刻 |
|---|---|---|
| `PeriodicSender` | PG を周期的に送る | 期限は仮想時間の格子でずれない。送信はその後の最初のステップ。PG ごとにタイマーを 1 つ使う |
| `Peer::every(period, fn)`、`LocalCan::on_step` | モデルの計算（`tick()`）、アプリの処理 | ステップの時刻。1 ステップに 1 回まで |
| `TickScheduler` | 固定周期の既存モデル（DLL の `tick()`） | ステップ幅の倍数の周期だけ（違えば例外） |
| `j1939::Timer` | 部品の中の時間（待ち時間、遅延、独自の周期） | 事象の時刻から数える（ステップ幅によらない） |

モデルの周期とメッセージの周期は分けて書きます。モデルは `every` で進め、送る PG は `PeriodicSender` の `Encoder` でモデルの今の値から作ります。ステップ幅はメッセージの最短の周期以下にすると、送信の回数がステップ幅によりません（長いと、1 ステップの期限をまとめて 1 回送ります）。

## エラーの扱い

- 送信の結果は `SendStatus` の戻り値です（`QueueFull`、`Busy`、`NoAddress` など）。部品は送信の失敗を数えて（`statistics()`）、次の機会にまた送ります。
- 使い方の誤り（不正な PGN、範囲外の値）は、部品の生成や呼び出しの時点で `std::invalid_argument` などの例外です。タイマーの枠（`NetworkOptions::timer_capacity`、既定 64）が足りないときは部品の生成が `std::length_error` で、メッセージに `timer_capacity` と出ます。周期送信の多いノードでは、`J1939ChannelOptions::network.timer_capacity` などで上げます。
- ハンドラ、`Encoder`、コールバックから投げた例外は `Network` を失敗状態にします。`Peer::failed()` と `error()` で分かります。
- 通信の状態は、`Network::statistics()`、部品の `statistics()`、`Monitor` の状態で見ます。

## 独自のプロトコルを部品として作る

ノードの公開 API と道具箱（`Subscription`、`Timer`、`Node::attach`）と、既存の部品（`Requester` など）だけで作ります。

```cpp
class ItemReader // 「項目 n を読む」コマンドを送り、応答か時間切れを返す
{
public:
    ItemReader(j1939::Node& node, j1939::Address server)
      : node_(node), server_(server),
        replies_(node, node.on_message(kPgnItems, [this](const j1939::Message& m) { on_reply(m); })),
        timeout_(node, j1939::TimerKind::Timeout, [this] { finish(std::nullopt); }) {}

    void read(std::uint8_t item, Done done)  // ステップの中から
    {
        item_ = item;
        done_ = std::move(done);
        node_.send(kPgnItems, encode_read(item), server_);
        timeout_.start_at(*node_.network().now() + 200ms);  // 送ったステップから数える
    }
    ...
};
auto& reader = cab.attach<ItemReader>(0x00);
```

- スレッド、イベントループ、OS の時計を使いません。時間は `Timer`（仮想時間）で数えます。
- 登録は `Subscription`、タイマーは `Timer` に持たせると、部品の破棄で自動で解除されます。部品は `attach` で `Network` に持たせます。
- 標準の Request を使うなら `Requester::of(node)` を使います（自分で応答待ちを作りません）。
- 自分の利用者を呼ぶループでは、通知の前に自分の状態を確定し、通知の中で足されたものを同じループで扱いません（[architecture.md](architecture.md) の「道具箱」）。
- `on_any_request` と `Network::set_observer` は使いません。
- 試験の全文は `tests/testkit/test_ecu_personas.cpp` にあります。

## 動かす

- SIL Kit の participant にするときは `vehicle_sil::Peer` です（[architecture.md](architecture.md) の「Peer」）。1 つの participant に複数のノード（ECU）を置けます。participant を分けるときは `Coordinated` にして、`SimulationController` と一緒に起動します（1 つのプロセスなら `start_together`）。例は [examples/j1939/ecu_pair/main.cpp](../examples/j1939/ecu_pair/main.cpp) です。
- SIL Kit を使わずに 1 つのプロセスで動かすときは `testkit::LocalCan` です（仮想バス、呼んだスレッドで進む）。

## 試験する

```cpp
EngineModel engine;
Dashboard dash;
testkit::LocalCan bus;
install_engine(bus.add_j1939("engine", 10ms), engine);
install_cab(bus.add_j1939("cab", 5ms), dash);
testkit::J1939Events events(bus.network("cab"));          // スタックの出来事（読み取り専用）
testkit::FrameLog wire(bus.tap("engine"));                // バスのフレーム（読み取り専用）
bus.run_for(1s);
bus.endpoint("engine").add_outage(bus.now(), bus.now() + 300ms);  // エンジンがバスから外れる
bus.run_for(1s);
CHECK(bus.network("cab").node(0).find<j1939::Monitor>()->losses() == 1);
```

- 時刻はすべて仮想時間で、`sleep` は要りません。同じ入力からは同じ結果になります。
- 故障注入（フレームの欠落、遅延、通信断）は仮想バスの端点で行います（`testkit::faults`、`add_outage`）。
- `Monitor` は通信の途絶を、`J1939Events` は送信の要求、フレームの送出、転送の終わり、アドレスの変化を、`FrameLog` はバス上のフレームを見せます。

## データの形（Codec）

部品とエンジンはバイト列だけを扱います（`PeriodicSender` の `Encoder` は「バッファに書いて長さを返す」関数、受信は `Message::data` の `std::span`）。信号の配置とスケーリングは、YAML から生成した型付きメッセージ（[j1939-codec.md](j1939-codec.md)）が受け持ち、`codec_node.hpp` の関数が両者をつなぎます。独自プロトコルの部品も、メッセージの形を同じ YAML で定義できます（Proprietary A のように 1 つの PGN に複数のメッセージを載せるときは `selector`）。
