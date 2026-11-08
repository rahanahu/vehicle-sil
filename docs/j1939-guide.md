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
| 独自プロトコルのコマンドと、応答の見分け方 | 送れるようになってから送ること、応答待ち、途中の応答、送り直し、自分と相手のアドレスの変化、長いコマンドの転送 |
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

## 独自のプロトコルを作る

Proprietary A/B やメーカー独自の通信手順は、vehicle-sil を変更せずに、利用者の部品として作れます。使うのはノードの公開 API、道具箱（`Subscription`、`Timer`、`Node::attach`）、既存の部品、型付きメッセージだけです。

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
    });
```

| `ExchangeOutcome` | 意味 |
|---|---|
| `Reply` | `match` が `Final` を返した（`reply`） |
| `Timeout` | 送り直しても応答がなかった（`Pending` の後は送り直さず、最後の `Pending` から `timeout`） |
| `NotSent` | 送れなかった（`send_status`。アドレスを要求できないときは `NoAddress`）か、長いコマンドの転送が最後まで失敗した（`transfer`、`reason`） |
| `PeerLost` | 相手がアドレスを失った、移った、別の NAME に取られた。NAME で指定した相手が `timeout` の間にアドレスを要求しなかった |

- `start()` はいつ呼んでもかまいません（ステップの外、アドレスを得る前も）。ノードが送れるようになってから送ります。NAME で指定した相手がまだアドレスを要求していなければ（起動の直後）、その Address Claimed を待って送ります。
- `match` には、相手のアドレスから来たメッセージが PGN によらずすべて渡ります（このノード宛てとグローバル。相手の周期の放送も来ます）。`M::decode(data)` はバイト列だけを見るので、PGN も確かめる `j1939::codec::decode<M>(message)` を使います。自分のコマンドへの応答かどうかは、操作のバイト、セッション、コマンドが指す項目（パラメーターの番号など）で見分けます。`Pending`（途中の応答）を返すと、待ち時間を数え直し、送り直しをやめます。
- 前のコマンドへの遅れた応答（送り直した元のコマンドへの応答、`cancel()` したコマンドへの応答）も `match` に来ます。応答が項目や番号を持たない手順では、それを次のやり取りの応答と取り違えます。応答が遅れうるなら、コマンドと応答に項目か通し番号を持たせて照合します。`cancel()` は送ったコマンドを取り消しません（応答は来ます）。
- 送り直し（`retries`）は既定で 0 です。同じコマンドを 2 回受けても困らない手順（読み出し、同じ値の書き込み、番号付きのコマンド）でだけ使います。ただし、ノードのアドレスが変わったときは、`retries` によらず送り直します（応答は古いアドレスへ来るため）。
- 待ち時間は、応答をトランスポートプロトコルで受けている途中かどうかを見ません。長い応答（とくに BAM。パケットの間隔は 50 ms 以上）を受けるときは、その時間を `timeout` に含めます。
- NAME で指定した相手が待ちの間に移ると、送り直しが残っていても `PeerLost` で終わります（相手がやり取りの状態を失ったかもしれないため）。次のやり取りは新しいアドレスへ送ります。
- `Exchange` は 1 つの相手とのやり取りだけを扱います。全体への問い合わせと複数の応答の収集（`Requester` のグローバルの要求を使う）や、相手と別のアドレスから来る応答（ゲートウェイ経由など）は扱いません。
- 9 バイト以上のコマンドはトランスポートプロトコルで送り、転送が終わってから待ち時間を数えます（`max_length` を上げます）。
- ノードのアドレスが変わると、送ったコマンドは新しいアドレスから送り直します。相手から見ると送り手が変わるので、相手がセッションを持つ手順では、相手が「セッションがない」と答えることがあります（下の例では、ツールがセッションを開き直します）。
- 同時に動かせるやり取りは `capacity`（既定 4）です。タイマーは `Exchange` ごとに 2 つ使います。
- コールバックと `match` の中から、次のコマンドを `start()` し、`cancel()` し、`Exchange` を破棄できます。破棄すると、残りのやり取りのコールバックは呼ばれません。

### 手順を書く（複数のメッセージ）

手順は、前の応答のコールバックで次のコマンドを `start()` してつなぎます。セッションのように前の応答で次のコマンドが決まる手順や、操作を順に 1 つずつ行う手順は、利用者の部品が操作の待ち行列を持ちます（例の `ParamClient`: セッションがなければ開いてから操作し、「セッションがない」と答えられたら 1 回だけ開き直す）。

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
- 標準の Request（PGN 59904）を使うなら `Requester::of(node)` を使います（`Exchange` で作り直しません）。
- 自分の利用者を呼ぶループでは、通知の前に自分の状態を確定し、通知の中で足されたものを同じループで扱いません（[architecture.md](architecture.md) の「道具箱」）。
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

コールバックの中で時刻を見るときは、そのネットワークの `now()`（ステップの時刻）を使います。`LocalCan::now()` は、最後の `run_until()` の終わりの時刻です。

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
