# J1939 のノードの作り方

ECU を書く人向けです。先に README の「[最初の ECU を動かす](../README.md#最初の-ecu-を動かす)」を読みます。次は [j1939-codec.md](j1939-codec.md)（信号を値として読み書きする）と [ecu-executable.md](ecu-executable.md)（ECU を実行ファイルにする）です。

vehicle-sil の J1939 スタックで ECU のノードを作り、動かし、試験する方法です。API の契約と規則の詳細は [internals/j1939.md](internals/j1939.md) にあります。API はまだ変わることがあります。

最初に動かすなら [examples/getting_started](../examples/getting_started) です。完全な例は [examples/j1939/ecu_pair](../examples/j1939/ecu_pair) です。エンジンとキャブの 2 つの ECU を部品だけで作り、`Simulation` で仮想バスか SIL Kit の 2 つの participant として動かします。同じコードを試験でも使っています。

## 利用者が書くものと、スタックが受け持つもの

| 利用者が書くもの | スタックが受け持つもの |
|---|---|
| ノードのアドレスと NAME | Address Claim（競合、任意アドレス、Commanded Address の受信、250 ms の待ち） |
| 送る PG と周期、データの作り方 | 周期の管理、アドレスを持つ間だけ送ること、DM13 による停止 |
| 受ける PG と、受けたときの処理 | 宛先の振り分け、トランスポートプロトコル（9〜1785 バイト）の組み立て |
| Request を送る相手と、答えの使い方 | 応答待ち、時間切れ、送り直し、同じ要求の相乗り |
| 独自プロトコルのコマンドと、応答の見分け方 | 送れるようになってから送ること、応答待ち、途中の応答、送り直し、自分と相手のアドレスの変化、長いコマンドの転送 |
| モデル（ふつうの C++） | 仮想時間、ステップ、スレッド、SIL Kit の participant とコントローラ |

## ノードを作る

ノードは `j1939::Network` の上に作ります。`Network` は 1 つの CAN ネットワーク（チャネル）への接続で、`Ecu::j1939(network)` が作ります。`vehicle_sil::Ecu` は ECU のアプリケーションを組み立てる入口です。SIL Kit の participant（`Peer`）と、プロセス内の仮想バスの ECU（`LocalBench::add_ecu()`）が実装するので、`Ecu&` を受け取る関数かクラスに ECU を書くと、同じコードがどちらの上でも動きます。

```cpp
j1939::Node& install_engine(vehicle_sil::Ecu& ecu, EngineModel& engine)
{
    auto& node = ecu.j1939("J1939_CAN").add_node({.address = 0x00, .name = kEngineName});   // Address Claim はスタックが行う
    ecu.every(10ms, [&engine](vehicle_sil::SimTime) { engine.tick(); });                    // モデルを進める
    codec::attach_sender<EngineStatus>(node, 20ms, [&engine](EngineStatus& m) { m.speed.set(engine.rpm); });
    codec::on_message<SpeedRequest>(node, [&engine](const SpeedRequest& m) {
        if (const auto rpm = m.speed.value()) { engine.requested = static_cast<std::uint16_t>(*rpm); }
    });
    return node;
}
```

`EngineStatus` と `SpeedRequest` は、YAML の定義から生成した型付きメッセージです（[j1939-codec.md](j1939-codec.md)）。信号を値として読み書きし、ビット位置やスケーリングを書きません。バイト列を直接扱うこともできます（下の各節）。

- `NodeOptions::name` を付けると、ノードは最初のステップでアドレスを要求します。NAME を付けないノードは、指定したアドレスをすぐに使います（試験用）。
- `node.attach<T>(...)` で付けた部品は `Network` が持ち、正しい順序で破棄します。
- ハンドラが参照するモデルやアプリのオブジェクトは、`ecu.emplace<T>(args...)` で ECU に持たせます。ECU は、それをネットワークの後に（最後に emplace したものから先に）破棄するので、ハンドラや部品は破棄の間も参照してかまいません。`emplace` は実行の開始前だけ使えます。
- 1 ステップの順序は、ネットワークの受信と処理、時刻の来た変数の書き込みの反映、`on_step`、`every`、`at`（足した順）、LIN のノードの処理（足した順）、変数の記録、最後にネットワークの送信です。同じネットワーク名で `j1939()` を呼べるのは、ECU ごとに 1 回だけです。開始後の `j1939()`、`on_step()`、`every()`、`at()`、`emplace()` は `std::logic_error` です（ノードは、`Network::add_node()` の規則でステップの中からも足せます）。
- 部品とハンドラは、ステップを回すスレッドの上で呼ばれます。ほかのスレッドから操作するときは、「周期的な処理の使い分け」の「ステップの外から渡す」を参照してください。

### 複数のネットワークを持つ ECU

ゲートウェイは、`j1939()` を異なるネットワーク名で呼びます。片方のハンドラから他方へ送ると、同じステップで出ます。

例: [examples/vehicle](../examples/vehicle)（2 つの CAN と 1 つの LIN）。

```cpp
class Gateway {
public:
    explicit Gateway(vehicle_sil::Ecu& ecu) {
        auto& a = ecu.j1939("CAN_A").add_node({.address = 0x30});
        auto& b = ecu.j1939("CAN_B").add_node({.address = 0x30});
        a.on_message(kPgn, [&b](const j1939::Message& m) { static_cast<void>(b.send(kPgn, m.data)); });
    }
};
```

### J1939 でないプロトコル（生の CAN）

`ecu.can(network)` は、J1939 を通さない生の CAN チャネル（`can::RawChannel`）を返します。`on_frame(handler)` で受け、`send(frame)` で送ります。タイマーも分割もないので、周期の処理は `every()` や `on_step()` で書きます。

例: [examples/pid_stream](../examples/pid_stream)。

- `send()` を呼べるのは、ステップの中だけです（コンストラクターから呼ぶと `std::logic_error`）。結果は `RawSendStatus`（`Ok`、`QueueFull`、`InvalidFrame`）で返ります。
- 送ったフレームは、そのステップの最後に、ステップの時刻で出ます。
- 同じ ECU が同じネットワーク名で `j1939()` と `can()` の両方を持ってもかまいません。別のコントローラーになるので、互いのフレームを受けます（J1939 のネットワークは 11 ビットのフレームを無視します）。ただし、`Peer` の J1939 のネットワークが利用者のポート（`j1939(name, port)`。Vector XL など）の上にあるときは別のバスになり、互いのフレームは受けません。生の CAN は常に SIL Kit の上にあるためです。
- 違う ECU から同じ時刻に届いたフレームの順は、SIL Kit では決まりません。順に依存する処理を書かないでください。

## 受ける

```cpp
codec::on_message<SpeedRequest>(node, [&](const SpeedRequest& m) { engine.target_rpm = m.speed.value_or(800.0); });
node.on_message(kPgnRaw, [&](const j1939::Message& m) { log(m.data); });   // バイト列のまま
```

- 自分のアドレス宛てとグローバル宛てのメッセージを受けます。9 バイト以上は、トランスポートプロトコルで組み立てたものを 1 回で受けます。その `Message::time` は転送が終わった時刻（最後のパケットの時刻）で、送り手が送り始めた時刻ではありません。相手の送信の周期を受けた時刻から確かめるときは、単一フレームのメッセージで確かめます。
- `codec::on_message<M>` は、decode できたもの（長さが足り、セレクターが合うもの）だけを渡します。
- `Message::data` は呼び出しの間だけ有効です。残すときはコピーします。
- 解除が要る登録は `j1939::Subscription` に持たせます（破棄で解除します）。
- 最新の値と途絶だけが要るなら、`Monitor` を使います（下）。

## 送る

- その場で 1 回送るときは `node.send(pgn, data, destination)` です。送れるのはステップの中（ハンドラ、タイマー、`Ecu::every`、`Ecu::on_step`、`Ecu::at` などアプリの処理）だけです。結果は `SendStatus` で返ります（例外ではありません）。
- シナリオの「ある時刻に 1 回」（要求を送る、購読を始める、止める）は `ecu.at(time, fn)` で書きます。開始時刻が `time` 以後の最初のステップで 1 回だけ呼ぶので、ステップ幅が時刻を割り切らなくても動きます。実行が `time` に届かずに終われば呼ばれません。登録は最初の実行の前だけです。`main` から直接 `send()` や、送る部品の関数（`examples/pid_stream` の `subscribe()`、`unsubscribe()` など）は呼べません（ステップの外のため）。

```cpp
ecu.at(vehicle_sil::SimTime{} + 100ms, [&client](vehicle_sil::SimTime) { client.subscribe(0x00, {kRpm}, 50ms, on_reply); });
ecu.at(vehicle_sil::SimTime{} + 600ms, [&client](vehicle_sil::SimTime) { client.unsubscribe(); });
```

- 周期的に送るときは `PeriodicSender` です。

```cpp
codec::attach_sender<EngineStatus>(node, 20ms, fill);                                   // 送るたびに新しい EngineStatus を fill で埋める
codec::send(node, request, 0x80);                                                        // 型付きメッセージを 1 回
auto& status = node.attach<j1939::PeriodicSender>(kPgnEngineStatus, 20ms, encoder);   // 送るたびに encoder を呼ぶ
auto& fixed = node.attach<j1939::PeriodicSender>(kPgnVersion, 1s, std::span(bytes)); // 決まったバイト列
fixed.set_data(new_bytes);                                                              // 次の送信から
status.trigger();                                                                       // 次の機会にすぐ送る
status.set_period(50ms);                                                                // 今から数え直す
```

- `codec::attach_sender` は、定義の優先度と長さで送ります。
- `Encoder` は部品のバッファ（`max_length` バイト、既定 8）に書き、書いた長さを返します。9 バイト以上（`max_length` を上げる）はトランスポートプロトコルで送ります。
- 長さが送るたびに変わるメッセージは、`std::vector<std::uint8_t>&` に追記する関数（`PeriodicSender::VectorEncoder`）で作ります。ベクターは空で渡され、その大きさが長さです。部品がベクターを持ち回るので、追記だけなら送るたびの確保はありません。引数の型は書きます（`std::vector<std::uint8_t>&` か `auto&`）。span を取る関数は長さを返す `Encoder` として扱います。`max_length` を超えると、span の形と同じく `Network` が失敗します。

```cpp
auto& reply = node.attach<j1939::PeriodicSender>(kPgnReply, 100ms, [&](std::vector<std::uint8_t>& out) {
    for (const auto id : ids) { out.push_back(id); out.push_back(value_of(id)); }
}, j1939::PeriodicSenderOptions{.destination = client, .max_length = 64});
```

- 周期、宛先、優先度は後から変えられます（`set_period()`、`set_destination()`、`set_priority()`）。`set_period()` は呼んだ時点から新しい周期を数え直します（すぐ送るなら `trigger()` も呼ぶ）。宛先と優先度は次の送信から効き、周期の刻みは変わりません。一時停止の間に変えれば、再開したときに新しい値で動きます。不正な値は `std::invalid_argument` で、何も変わりません。同じ相手からの新しい要求は、作り直さずに周期を変えて応えられます（`examples/pid_stream`）。
- ノードが送れるようになるまで（Address Claim の後の 250 ms の待ちの間も）送らず、送れるようになったときに 1 回送ってから周期を数えます。アドレスを失えば止まり、取り直せば再開します。
- `trigger()` をアプリの処理（`Ecu::every` など）から呼ぶと、送るのは次のステップです。そのステップで送りたいときは `node.send()` を使います。
- 全体宛ての放送は、DM13 で放送を止めている間（他のノードの DM13 か、自分の `BroadcastSuppression::suspend()`）は送りません。1 つのアドレス宛ての送信は、既定では止めません（規格での扱いを確かめていないため）。既定では、そのときの宛先で決まります（`set_destination()` で決め直す。1 つのアドレス宛てで作った送信を全体宛てにするには、組み立てで `BroadcastSuppression::of(node)` を付けておく。なければ `std::logic_error`）。どちらも `PeriodicSenderOptions::suppressible`（true か false）で固定できます。
- PDU1 の PG を 1 つのアドレスへ送るときは `PeriodicSenderOptions{.destination = address}` です。

## Request に答える

```cpp
node.on_request(kPgnSomething, [&](j1939::RequestContext& r) { r.respond(encode(model)); });
```

- 応答者のない PGN への宛先指定の Request には、スタックが NACK を返します。
- ソフトウェア ID などの識別情報は `Identification` 部品が答えます（`node.attach<j1939::Identification>().set_software_identification(fields)`）。診断（DM1 など）は `Diagnostics` 部品です。
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
- `MonitorOptions::accept` に関数を渡すと、それが受け入れたメッセージだけを最新の値と受信に数えます（`.accept = [](const j1939::Message& m) { return j1939::codec::decode<EngineStatus>(m).has_value(); }` で 1 つの型だけ、通し番号や検査値の確認も同じ形）。受け入れなかったものは `rejected()` に数え、途絶の判定にも使いません。
- 途絶（`Lost`）は最後の受信から `timeout` で知らせ、受信が戻れば `Receiving` に戻ります。最初の受信までは `Waiting` で、途絶にしません。

## ほかのノードのアドレスを変える

```cpp
auto& tool = service.attach<j1939::AddressCommander>();
tool.command(kCabName, 0x81, [](const j1939::CommandResult& r) { /* r.outcome == CommandOutcome::Moved */ });
```

相手が新しいアドレスで Address Claimed を送ると `Moved` です。任意アドレス可能でない相手は従いません（`Timeout`）。

## 周期的な処理の使い分け

### いつ動かすか

| 使うもの | 向いているもの | 時刻 |
|---|---|---|
| `PeriodicSender` | PG を周期的に送る | 期限は仮想時間の格子でずれない。送信はその後の最初のステップ。PG ごとにタイマーを 1 つ使う |
| `Ecu::every(period, fn)` | モデルの計算（`tick()`）、アプリの周期的な処理 | 0、period、2 * period、… の時刻以後の最初のステップ。1 ステップに 1 回まで。period がステップ幅の倍数でなくてもよい（ステップ幅 10 ms、period 25 ms なら 0、30、50、80、100 ms…） |
| `Ecu::on_step(fn)` | 毎ステップの処理 | ステップの時刻。受信の処理の後、送信の前 |
| `Ecu::at(time, fn)` | シナリオの 1 回きりの操作（要求を送る、購読を始める、止める） | 開始時刻が `time` 以後の最初のステップで 1 回。`time` がステップ幅で割り切れなくてもよい |
| `TickScheduler` | 固定周期の既存モデル（DLL の `tick()`）。`Ecu` の外、`ParticipantRuntime` のステップハンドラで使う | ステップ幅の倍数の周期だけ（違えば例外） |
| `j1939::Timer` | 部品の中の時間（待ち時間、遅延、独自の周期） | 事象の時刻から数える（ステップ幅によらない） |

### ステップの外から渡す

| 使うもの | 向いているもの | 決定性 |
|---|---|---|
| `StepInbox` | ほかのスレッドや SIL Kit のハンドラで受けたデータを、ステップで取り出す | 時刻つきで入れ、そのステップより前の時刻のものを、時刻、タイブレーク、到着の順に出す。決定的になるのは、次の 2 つを守るときだけです。1 つ目は、送り手を区別するタイブレークを選ぶことです（異なる送り手の同じ時刻のものは、選ばないと順が決まらない）。2 つ目は、容量を最悪の場合に合わせて取り、あふれを誤りとして扱うことです（上限の近くのあふれは決定的でない） |
| Control Agent の書き込み | Control API からの変数の書き込み | 仮想時刻（`Command::at`）を指定すれば決定的。指定しなければ次のステップ |
| `Host::post(fn)`（`LocalBench::host(ecu)`、`Peer::host()`） | ほかのスレッドや、ステップの外からの操作 | 時刻はなし。動くステップは post した時刻による。決定性が要るデータは `StepInbox` に入れる |

モデルの周期とメッセージの周期は分けて書きます。モデルは `every` で進め、送る PG は `PeriodicSender` の `Encoder` でモデルの今の値から作ります。ステップ幅はメッセージの最短の周期以下にすると、送信の回数がステップ幅によりません（長いと、1 ステップの期限をまとめて 1 回送ります）。

## エラーの扱い

- 送信の結果は `SendStatus` の戻り値です（`QueueFull`、`Busy`、`NoAddress` など）。部品は送信の失敗を数えて（`statistics()`）、次の機会にまた送ります。
- 使い方の誤り（不正な PGN、範囲外の値）は、部品の生成や呼び出しの時点で `std::invalid_argument` などの例外です。タイマーの枠（`NetworkOptions::timer_capacity`、既定 64）が足りないときは部品の生成が `std::length_error` で、メッセージに `timer_capacity` と出ます。周期送信の多いノードでは、`Ecu::j1939(network, NetworkOptions{.timer_capacity = ...})`（`Peer` では `J1939ChannelOptions::network.timer_capacity`）で上げます。
- 受けた要求ごとに部品（`PeriodicSender`、`Timer` など）をハンドラの中で作るときは、作る前に `node.network().timers_free()`（残りの枠の数）を、作る部品の `kTimers`（`PeriodicSender::kTimers` など）と比べて確かめ、足りなければ要求を断ります。ハンドラの中の `std::length_error` は `Network` を失敗させるので、確かめないと、多数のアドレスから要求を送るだけでその ECU の通信を止められます。各部品が使うタイマーの数は、部品のヘッダーのコメントと [internals/j1939.md](internals/j1939.md) の「プロトコル部品」にあります。
- ハンドラ、`Encoder`、コールバックから投げた例外は `Network` を失敗状態にします。`Simulation` では `SimulationError` になります（「動かす」）。`Peer` を直接使うときは、`Peer::failed()` と `error()` で分かります。
- 通信の状態は、`Network::statistics()`、部品の `statistics()`、`Monitor` の状態で見ます。

## 独自のプロトコルを作る

Proprietary A/B やメーカー独自の通信手順は、vehicle-sil を変更せずに、利用者の部品として作れます。使うのはノードの公開 API、道具箱（`Subscription`、`Timer`、`Node::attach`）、既存の部品、型付きメッセージだけです。

最初の手本は [examples/pid_stream](../examples/pid_stream) です（要求を受けて周期的に返す部品）。

完全な例は [examples/j1939/param_service](../examples/j1939/param_service) です。サービスツールがエンジン ECU のパラメーターとブロックを Proprietary A で読み書きします（セッションの開始と終了、書き込み中の途中の応答、トランスポートプロトコルで送るブロック、エラー応答、無通信でのセッションの終了、相手の消失）。試験は `tests/testkit/test_param_service.cpp` です。

### メッセージを決める

固定長のメッセージは YAML で定義します（[j1939-codec.md](j1939-codec.md)）。1 つの PGN に複数のメッセージを載せるときは、操作のバイトを `selector` にします。

```yaml
  - name: Read
    proprietary_a: {}
    selector: {start_bit: 0, length: 8, value: 0x11}
    signals:
      - {name: session, start_bit: 8, length: 8}
      - {name: parameter, start_bit: 16, length: 16}
```

可変長のデータ（ブロックの中身など）は、バイト列のまま `node.send()` と `Message::data` で扱います。型付きメッセージとバイト列は、同じ PGN で混ぜて使えます（型の `decode()` は、セレクターの違うものを空で返します）。

### 要求する側（`Exchange`）

コマンドを 1 つの相手へ送り、応答を待つのは `j1939::Exchange` です（ターゲット `vehicle_sil::j1939_exchange`）。

```cpp
j1939::Exchange exchange(node, {.timeout = 200ms, .retries = 1});
messages::Read read;
read.session.set_raw(session);
read.parameter.set_raw(0x0100);
exchange.start(kPgn, read.bytes(), kEngineName,          // 相手は NAME（アドレスでもよい）
    [](const j1939::Message& m) {                         // 受けたメッセージが何か（PGN も見る）
        if (const auto ok = j1939::codec::decode<messages::ReadOk>(m); ok && ok->parameter.raw() == 0x0100) {
            return j1939::ReplyMatch::Final;              // この読み出しの答え
        }
        if (const auto e = j1939::codec::decode<messages::Error>(m); e && e->request.raw() == kRead) {
            return j1939::ReplyMatch::Final;              // 読み出しへのエラー
        }
        if (const auto p = j1939::codec::decode<messages::Pending>(m); p && p->request.raw() == kRead) {
            return j1939::ReplyMatch::Pending;
        }
        return j1939::ReplyMatch::Ignore;
    },
    [](const j1939::ExchangeResult& r) {                  // 1 回だけ呼ばれる
        if (r.outcome == j1939::ExchangeOutcome::Reply) { use(j1939::codec::decode<messages::ReadOk>(r.reply)); }
    },
    {.on_address_change = j1939::AddressChangePolicy::SendAgain}); // 読み出しは 2 回実行してよい
```

| `ExchangeOutcome` | 意味 |
|---|---|
| `Reply` | `match` が `Final` を返した（`reply`） |
| `Timeout` | 送り直しても応答がなかった（`Pending` の後は送り直さず、最後の `Pending` から `timeout`） |
| `NotSent` | 相手に届いていない: 一度も送れなかった（`send_status`。アドレスを要求できなくなったときは `NoAddress`）か、長いコマンドの転送が最後のパケットの前に終わった・相手に中断された（`transfer`、`reason`） |
| `Interrupted` | 送ったが、応答が来られなくなった: 自分のアドレスが変わった・失われた、長いコマンドの最後のパケットの後にこのノードが時間切れで転送を中断した、送り直しを送れなかった。相手が実行したかは分からない |
| `PeerLost` | 相手がアドレスを失った、移った、別の NAME に取られた。NAME で指定した相手が `timeout` の間にアドレスを要求しなかった |

#### 応答を見分ける（`match`）

- `match` には、相手のアドレスから来たメッセージが PGN によらずすべて渡ります（このノード宛てとグローバル。相手の周期の放送も来ます）。`M::decode(data)` はバイト列だけを見るので、PGN も確かめる `j1939::codec::decode<M>(message)` を使います。
- 自分のコマンドへの応答かどうかは、操作のバイト、セッション、コマンドが指す項目（パラメーターの番号など）で見分けます。`Pending`（途中の応答）を返すと、待ち時間を数え直し、送り直しをやめます。
- 前のコマンドへの遅れた応答（送り直した元のコマンドへの応答、`cancel()` したコマンドへの応答）も `match` に来ます。応答が項目や番号を持たない手順では、それを次のやり取りの応答と取り違えます。応答が遅れうるなら、コマンドと応答に項目か通し番号を持たせて照合します。
- `cancel()` は送ったコマンドを取り消しません（応答は来ます）。

#### コマンドごとの設定（`CommandOptions`）

`start()` の最後の引数です。指定しない項目は `Exchange` 全体の設定（`ExchangeOptions`）を使います。

| 項目 | 意味 |
|---|---|
| `priority` | 優先度（既定 6） |
| `timeout` | このコマンドの応答の待ち時間。消去のように時間のかかるコマンドだけ長くする |
| `retries` | 時間切れの後に送り直す回数。書き込みや消去は 0 にする |
| `long_answer` | トランスポートプロトコルで来る応答の PGN（下の「長いコマンドと長い応答」） |
| `on_address_change` | 送った後に自分のアドレスが変わったとき: `Stop`（既定。`Interrupted` で終える）か `SendAgain`（新しいアドレスから送り直す） |

待ち時間は、コマンドを送ったステップから数えます。次のときは、そこから数え直します。

- 送り直したとき（その送り直しを送ったステップから）
- 長いコマンドの転送が終わったとき
- `Pending` を受けたとき（その時刻から）
- `long_answer` の転送が終わったとき（その時刻から）

#### 送り直し

- 時間切れの後の送り直し（`retries`）は既定で 0 です。同じコマンドを 2 回受けても困らない手順（読み出し、同じ値の書き込み、番号付きのコマンド）でだけ使います。
- 自分のアドレスが変わったときの送り直しは、時間切れの送り直しとは別に、`on_address_change` で選びます（`retries` には数えません）。既定は送り直さず `Interrupted` です。書き込みや消去は既定のままにし、`Interrupted` を受けたら、読み出して確かめるなど手順の側で判断します。2 回実行してよいコマンドだけ `SendAgain` にします。
- `SendAgain` で送り直すと、相手から見ると送り手が変わるので、相手がセッションを持つ手順では「セッションがない」と答えることがあります（下の例では、ツールがセッションを開き直します）。

#### 長いコマンドと長い応答

- 9 バイト以上のコマンドはトランスポートプロトコルで送り、転送が終わってから待ち時間を数えます（`max_length` を上げます）。
- 長い応答（とくに BAM。パケットの間隔は 50 ms 以上）は、`long_answer` にその PGN を指定すると、受けている間は時間切れになりません。対象は、相手がこのノード宛てかグローバルに送る、その PGN の転送だけです（無関係な放送では止まりません）。
- 止めておけるのは 1 回の試行につき `ExchangeOptions::answer_transfer_limit`（既定 60 秒）までで、終わらない転送や繰り返し始まる転送でも、待ちは必ず終わります。
- `long_answer` を指定しなければ、長い応答の時間を `timeout` に含めます。

#### 相手のアドレスと NAME

- `start()` はいつ呼んでもかまいません（ステップの外、アドレスを得る前も）。ノードが送れるようになってから送ります。
- NAME で指定した相手がまだアドレスを要求していなければ（起動の直後）、その Address Claimed を待って送ります。
- NAME で指定した相手が待ちの間に移ると、送り直しが残っていても `PeerLost` で終わります（相手がやり取りの状態を失ったかもしれないため）。次のやり取りは新しいアドレスへ送ります。

#### 扱わないことと上限

- `Exchange` は 1 つの相手とのやり取りだけを扱います。全体への問い合わせと複数の応答の収集（`Requester` のグローバルの要求を使う）や、相手と別のアドレスから来る応答（ゲートウェイ経由など）は扱いません。
- 同時に動かせるやり取りは `capacity`（既定 4）です。タイマーは `Exchange` ごとに 2 つ使います。
- コールバックと `match` の中から、次のコマンドを `start()` し、`cancel()` し、`Exchange` を破棄できます。破棄すると、残りのやり取りのコールバックは呼ばれません。

### 手順を書く（複数のメッセージ）

手順は、前の応答のコールバックで次のコマンドを `start()` してつなぎます。セッションのように前の応答で次のコマンドが決まる手順や、操作を順に 1 つずつ行う手順は、利用者の部品が操作の待ち行列を持ちます（例の `ParamClient`: セッションがなければ開いてから操作し、「セッションがない」と答えられたら 1 回だけ開き直す。読み出しと Open、Close は `SendAgain`、書き込みは既定の `Stop` で、移ったら `Interrupted` を利用者に返す。ブロックの読み出しは `long_answer` を指定する）。

- 利用者へのコールバックは、自分の状態を確定してから呼びます（例では、次の操作を始めてから前の操作のコールバックを呼ぶので、コールバックの中で部品を破棄しても安全です）。
- `read()` などの呼び出しの中で、その利用者のコールバックを呼ばないようにします（呼び出し側の状態が途中のことがあります）。

### 応答する側

応答側は `on_message` とノードの `send()` で書きます（専用の部品はありません）。

```cpp
ParamServer(j1939::Node& node, ParameterTable& table)
  : node_(node), table_(table),
    commands_(node, node.on_message(kPgn, [this](const j1939::Message& m) { on_command(m); })),
    claims_(node, node.on_message(j1939::kPgnAddressClaimed, [this](const j1939::Message&) { on_claim(); })),
    idle_(node, j1939::TimerKind::Timeout, [this] { end_session(); }) {}

void on_command(const j1939::Message& m) {
    if (m.destination != node_.address()) { return; }              // Proprietary A のグローバル宛ては使わない
    ...
    j1939::codec::send(node_, reply, m.source);                    // 応答は送ってきたアドレスへ
    idle_.start_after(2s);                                          // 受けたメッセージの時刻から数える
}
```

- 9 バイト以上の応答は `node.send(pgn, data, to, priority, j1939::WhenBusy::Queue)` で送ります（同じ相手への前の転送が終わるのを待ちます）。
- 時間のかかる処理（フラッシュへの書き込みなど）は、`Timer` で後から応答します。その間に `Pending` のような途中の応答を周期的に送ると、要求側の `Exchange` は待ち続けます。
- 相手の消失は、セッションを開いたときに `node.network().name_at(m.source)` で相手の NAME を控え、Address Claimed を受けるたびに `node.network().address_of(name)` が同じアドレスかを確かめて見つけます（Cannot Claim と、別の NAME にアドレスを取られた場合を含みます）。NAME の分からない相手は、無通信の時間切れで終えます。
- 自分のアドレスが変わったら（`on_address_change`）、相手のコマンドは古いアドレスへ来ていたので、セッションを終えます。

### 共通の規則

- スレッド、イベントループ、OS の時計を使いません。時間は `Timer`（仮想時間）で数えます。
- 登録は `Subscription`、タイマーは `Timer`、要求は `Exchange` に持たせると、部品の破棄で自動で解除されます。部品は `attach` で `Network` に持たせるか、`Network` より先に破棄します。
- アプリが持つ部品（`std::unique_ptr` など）は、ハンドラやタイマーのコールバックの中で破棄してかまいません（相手の Stop を受けたら止める、など）。破棄した部品のハンドラは、同じ配送の残りでも呼ばれず、タイマーも発火しません。部品が呼ぶ関数（`PeriodicSender` の Encoder、`Monitor` と `Exchange` のコールバック）の中でも、その部品を破棄してかまいません。例外は `AddressCommander` のコールバックと `Diagnostics` の `on_clear` です。
- ハンドラの中で部品を作るときは、`timers_free()` で枠を確かめてから作ります（「エラーの扱い」）。
- 標準の Request（PGN 59904）を使うなら `Requester::of(node)` を使います（`Exchange` で作り直しません）。
- 自分の利用者を呼ぶループでは、通知の前に自分の状態を確定し、通知の中で足されたものを同じループで扱いません（[internals/j1939.md](internals/j1939.md) の「道具箱」）。
- `on_any_request` と `Network::set_observer` は使いません。

### 独自のプロトコルを試験する

例の試験（`tests/testkit/test_param_service.cpp`）は、正常系のほかに次のことを確かめています。どれも仮想時間で、同じ入力からは同じフレームが出ます。

| 起こすこと | 方法 |
|---|---|
| 相手の無応答 | `bus.endpoint("engine").add_outage(from, until)` |
| 相手・自分のアドレスの喪失と移動 | 優先度の高い NAME の Address Claimed を `bus.inject()` で入れる |
| 無通信でのセッションの終了 | `bus.run_for()` で待つ |
| 2 台目のツール | 同じバスに `add_j1939()` でもう 1 つのネットワークを足す |
| 部品の破棄（処理中、コールバックの中） | 部品を `std::unique_ptr` で持ち、途中で `reset()` する |

コールバックの中で時刻を見るときは、そのネットワークの `now()`（ステップの時刻）を使います。`LocalBench::now()` は、最後の `run_until()` の終わりの時刻です。

## 動かす

`vehicle_sil::Simulation`（ターゲット `vehicle_sil::simulation`）が ECU を動かします。動かす場所はコンストラクターで選び、ECU のコードは変わりません。実行時に選ぶときは、`std::optional<SilKitOptions>` を渡します（空ならプロセス内の仮想バス）。

層を自分で組み立てる例は [examples/low_level](../examples/low_level) です。

```cpp
vehicle_sil::Simulation sim;                                   // プロセス内の仮想バス
// vehicle_sil::Simulation sim(vehicle_sil::SilKitOptions{});  // SIL Kit の participant（registry はプロセス内に立つ）
auto& engine_ecu = sim.add_ecu("Engine", 10ms);                // 10 ms ごとに進む ECU
install_engine(engine_ecu, engine_ecu.emplace<EngineModel>());
auto& cab_ecu = sim.add_ecu("Cab", 5ms);
install_cab(cab_ecu, cab_ecu.emplace<Dashboard>());
sim.run_for(3s);                                               // 仮想時間で 3 秒
```

`sim.add<App>("Engine", 10ms, args...)` は、`add_ecu()` と、`App{ecu, args...}` を ECU に持たせる `emplace` の略です。

| | `Simulation()` | `Simulation(SilKitOptions)` |
|---|---|---|
| ECU | 仮想バス（`LocalBench`）のホスト。呼んだスレッドで進む | 仮想時間の Coordinated な participant（`Peer`）。SIL Kit のスレッドで進む |
| `run_for(d)` | 続きから何度でも呼べる | 1 回だけ（2 回目は `std::logic_error`） |
| 戻るとき | `d` だけ進んだとき | 全 participant が止まった後 |
| 低水準 | `local()`: バス（CAN と LIN）、故障注入、生フレーム、tap（`vehicle_sil/local/local_bench.hpp`） | `peer(name)`: tap、Control Agent、SIL Kit のポート（`vehicle_sil/peer/peer.hpp`）。`registry_uri()` |

- `run_for(d)` は、終わりの時刻（仮想バスでは今の時刻から `d` 進めた時刻）より前に始まるステップだけを動かします。SIL Kit では、1 回しか動かせません。止まった時刻のステップがまだ走っていることがあり、メインスレッドからモデルを読むと競合するためです。止めながら確かめるときは、仮想バスを使います。
- ステップの例外は、`SimulationError` として `run_for()` から出ます。`what()` は `<ECU 名>: <理由>` です。仮想バスでは、失敗した後の `run_for()` も `SimulationError` です。SIL Kit では、失敗した participant があっても simulation は終わらず、仮想時間が止まるだけです。そのため `run_for()` は、失敗を見つけると simulation を終えてから投げます。
- SIL Kit では、ステップは SIL Kit のスレッドで走ります。ステップが書いた値は、`run_for()` が戻ってから読んでください。`Simulation` の関数は 1 つのスレッドから呼びます。
- 破棄の順序は、後から足した ECU から先に、ECU の持ち物（`emplace`）まで壊し、次に controller、最後にプロセス内の registry です。
- LIN の Master/Slave は `Ecu::lin_master(network, schedule)` と `Ecu::lin_slave(network)` で作ります。仮想バスでも SIL Kit でも同じ結果になります（[internals/lin.md](internals/lin.md)）。

`SilKitOptions` で、既存の registry（`registry_uri`）や、ほかのプロセスの participant との併用を選べます。

| 項目 | 意味 |
|---|---|
| `registry_uri` | 接続する registry。空ならプロセス内に立てる（`registry_uri()` で場所が分かる） |
| `controller` | `true`（既定）: このプロセスが simulation を始め、終える。`false`: 外の controller（`vehicle_sil_sim_controller`、Control Server）が決める。`run()` で終わりまで待つ |
| `other_participants` | 始める前にそろっていなければならない、ほかのプロセスの Coordinated な participant（`controller` が `true` のときだけ） |
| `start_timeout` | 始まるまでに許す時間（既定 30 秒） |

`run()` は、誰が終えてもよい simulation の終わりまで待ちます（SIL Kit だけ）。`controller` が `true` のときは、Error で離脱した participant を失敗にします。Error 以外で離脱した participant が数秒のうちに simulation を終えなければ、それも失敗です。`controller` が `false` のときに失敗にするのは、このプロセスの ECU の失敗だけです。

`Peer` を直接使うのは、実時間で動かすとき、別のプロセスで 1 つの participant として動かすとき、Control Agent を組み込むときです（[internals/execution.md](internals/execution.md) の「Peer」）。`Peer` も `Ecu` なので、`install_engine(peer, model)` のように同じ関数が使えます。participant を分けるときは `Coordinated` にして、`SimulationController` と一緒に起動します（1 つのプロセスなら `start_together`）。

## 試験する

自分の ECU は、`Simulation` の仮想バスで試験します。時刻は仮想時間で、`sleep` は要りません。同じ入力からは同じ結果になります。

故障の注入と観測の例は [examples/fault_injection](../examples/fault_injection) です。

```cpp
vehicle_sil::Simulation sim;                               // 仮想バス
auto& engine_ecu = sim.add_ecu("engine", 10ms);
install_engine(engine_ecu, engine_ecu.emplace<EngineModel>());
auto& cab_ecu = sim.add_ecu("cab", 5ms);
auto& dash = cab_ecu.emplace<Dashboard>();
install_cab(cab_ecu, dash);
auto& bus = sim.local();                                   // LocalBench（vehicle_sil/local/local_bench.hpp）
testkit::J1939Events events(bus.network("cab"));           // スタックの出来事（読み取り専用）
testkit::FrameLog wire(bus.tap("engine"));                 // バスのフレーム（読み取り専用）
sim.run_for(1s);                                           // 刻んで進め、そのたびに確かめられる
bus.endpoint("engine").add_outage(sim.now(), sim.now() + 300ms);   // エンジンがバスから外れる
sim.run_for(1s);
CHECK(bus.network("cab").node(0).find<j1939::Monitor>()->losses() == 1);
```

- `local()` の `LocalBench` から、故障注入（フレームの欠落、遅延、通信断。`testkit::faults`、`add_outage`）と、生フレームの注入（`inject()`）ができます。バスが 1 つなら `inject(t, frame)` のように名前を省けます。2 つ以上なら、バスの名前を渡します（例: `inject("J1939_CAN", t, frame)`）。記録と故障注入の道具（`testkit::J1939Events`、`FrameLog`、`faults`）は `vehicle_sil::testkit` にあり、使うときはそれをリンクします（`vehicle_sil::simulation` はリンクしません）。
- `Monitor` は通信の途絶を、`J1939Events` は送信の要求、フレームの送出、転送の終わり、アドレスの変化を、`FrameLog` はバス上のフレームを見せます。
- 参照するときは、ECU の名前とネットワークの名前を渡します。`network("cab", "J1939_CAN")` のように 2 つ渡します。ネットワークが 1 つの ECU には 1 つの名前の形も使えます。
- `LocalBench` を直接使うこともできます。`LocalBench::add_ecu(name, step)` は `Ecu&` を返します。`add_j1939(name, step)` は、1 つのネットワークを持つ ECU の略記です。`add_j1939()` と、`bus()`、`injector()`、`inject(time, frame)` のようにバスの名前を省く形は、ただ 1 つのバスを使います。バスがまだなければ作り、そのバスは後から来た `Ecu::j1939(network)` か `Ecu::can(network)` の名前になります。`bus(network)`、`injector(network)`、`inject(network, time, frame)` は名前でバスを引くだけで、名前を付けません（その名前のバスがなければ `std::out_of_range`）。どちらの順で書いても同じバスです。バスが 2 つ以上あるときに名前を省くと `std::logic_error` です。`add_j1939()` で ECU を足した後に 2 つ目のバスの名前を使うのも `std::logic_error` です（その ECU のバスが決まらないためです）。バスを複数使うときは、ECU を `add_ecu(name, step).j1939(network)` で足し、`inject(network, time, frame)` のようにバスの名前を渡します。

### ECU をバスから外す（`ecu.connected`）

どの ECU にも、bool の入力 `ecu.connected` があります。false を書くと、その ECU は動いたまま、すべてのバスから外れます。`add_outage` と違って、`Simulation()`（仮想バス）でも SIL Kit でも、シナリオ（[scenario.md](scenario.md) の「ECU をバスから外す」）、Control API、`agent()` のどれからでも同じように使えます。

```cpp
sim.ecu("engine").agent().submit({1, {{"ecu.connected", false}}, SimTime{} + 300ms});
sim.ecu("engine").agent().submit({2, {{"ecu.connected", true}}, SimTime{} + 600ms});
```

J1939 では次のように見えます（契約は [internals/execution.md](internals/execution.md) の「ECU の入口」の「バスからの切り離し」）。

- 外れた ECU: `send()` は `Ok` を返しますが、フレームはバスに出ず、`Network` の `send_failures` に数えて捨てます（溜めないので、戻ったときにまとめて出ることはありません）。受信はありません。周期送信、タイマー、アドレスの状態はそのままです。トランスポートプロトコルは、相手の応答が来ないので時間切れで終わります。
- ほかの ECU: 外れた ECU のメッセージが来なくなります。`Monitor` の `Lost` など、受信の途絶で検出します。アドレス表はそのままです。
- 戻ったとき: ネットワークの `NetworkOptions::on_reconnect` で選びます。どちらも規格と照らして確かめていないので、模擬する ECU のふるまいに合わせて選んでください。
  - `j1939::OnReconnect::Announce`（既定）: NAME を持つ各ノードが、Request for Address Claimed を全体へ送り、自分の Address Claimed も送ります。ほかのノードの答えで、外れている間に同じアドレスを取ったノードとの競合が解けます。ノードは Claim の後の 250 ms の待ちをしません。
  - `j1939::OnReconnect::Nothing`: 何も送りません。外れていたことに気づかない ECU を模擬します。
  - Address Claim を最初からやり直す（待ちを含む）選択肢はまだありません。

```cpp
j1939::NetworkOptions options;
options.on_reconnect = j1939::OnReconnect::Nothing;
auto& node = ecu.j1939("J1939_CAN", options).add_node({.address = 0x00, .name = kName});
```

## 外から信号を操作する・読む（UI、試験）

UI や試験から ECU の値を読み書きするときは、ECU に変数を宣言し、Control API を使います。Control API が扱うのは宣言した変数と時間の操作で、J1939 のフレームは扱いません。受けた信号を外に見せるときは、その値を `output` に、外から決める値は `input` にします（例: [examples/ecu_main/engine/main.cpp](../examples/ecu_main/engine/main.cpp)）。

```cpp
ecu.input("engine.throttle", &throttle_,
          {.unit = "%", .min = 0.0, .max = 100.0, .description = "accelerator pedal position"});
ecu.output("engine.speed", &rpm_, {.unit = "rpm"});   // 送る PG の元にする値
```

宣言の規則、同じプロセスからの読み書き、Control Server へのつなぎ方は [control-api.md](control-api.md) にあります。

## データの形（Codec）

部品とエンジンはバイト列だけを扱います（`PeriodicSender` の `Encoder` は「バッファに書いて長さを返す」関数、受信は `Message::data` の `std::span`）。信号の配置とスケーリングは、YAML から生成した型付きメッセージ（[j1939-codec.md](j1939-codec.md)）が受け持ち、`codec_node.hpp` の関数が両者をつなぎます。独自プロトコルの部品も、メッセージの形を同じ YAML で定義できます（Proprietary A のように 1 つの PGN に複数のメッセージを載せるときは `selector`）。

YAML で定義して使う例は [examples/typed_messages](../examples/typed_messages) です。
