# J1939 のノードの作り方

[English](../j1939-guide.md)

vehicle-sil の J1939 スタックで ECU のノードを作り、メッセージを送受信し、シミュレーションで実行してテストする方法を、C++ のコードで説明します。API の保証とルールの詳細は [internals/j1939.md](internals/j1939.md) にあります。API はまだ変わることがあります。

| したいこと | 使うもの | 節 |
|---|---|---|
| ノードを作る | `Ecu::j1939()`、`Network::add_node()` | [ノードを作る](#ノードを作る) |
| 2 つのネットワークの間を中継する | ネットワーク名を変えた `Ecu::j1939()` | [複数のネットワークを持つ ECU](#複数のネットワークを持つ-ecu) |
| J1939 でないフレームを送受信する | `Ecu::can()` | [J1939 でないプロトコル（生の CAN）](#j1939-でないプロトコル生の-can) |
| メッセージを受信する | `codec::on_message`、`Node::on_message` | [メッセージを受信する](#メッセージを受信する) |
| メッセージを 1 回、決まった時刻に、または周期的に送信する | `Node::send`、`Ecu::at`、`PeriodicSender` | [メッセージを送信する](#メッセージを送信する) |
| Request に応答する | `Node::on_request` | [Request に応答する](#request-に応答する) |
| Request を送信して応答を受信する | `Requester` | [Request を送信して応答を受信する](#request-を送信して応答を受信する) |
| 最新の値を読み、受信の途絶を検出する | `Monitor` | [受信を監視する](#受信を監視する) |
| ほかのノードの故障コードを読む | `parse_dtc_list()` | [ほかのノードの故障コード（DM1、DM2）を読む](#ほかのノードの故障コードdm1dm2を読む) |
| ほかのノードのアドレスを変える | `AddressCommander` | [ほかのノードのアドレスを変える](#ほかのノードのアドレスを変える) |
| 周期的な処理を書く | `PeriodicSender`、`Ecu::every` など | [周期的な処理の使い分け](#周期的な処理の使い分け) |
| 送信の失敗や使い方のエラーを扱う | `SendStatus`、例外 | [エラーの扱い](#エラーの扱い) |
| メーカー独自の通信手順を作る | `Exchange`、`Timer`、`Subscription` | [独自のプロトコルを作る](#独自のプロトコルを作る) |
| ECU をシミュレーションで実行する | `Simulation` | [シミュレーションで実行する](#シミュレーションで実行する) |
| ECU をテストする | `Simulation::local()`、`vehicle_sil::testkit` | [ECU をテストする](#ecu-をテストする) |
| ECU をバスから切断する | 変数 `ecu.connected` | [ECU をバスから切断する（`ecu.connected`）](#ecu-をバスから切断するecuconnected) |
| 何も送信せずに、バスの通信を受信する | `vehicle_sil::spy::Spy`、`j1939::Listener`、`Ecu::can()` | [受信だけをする participant](#受信だけをする-participant) |
| ECU の値を外部から読み書きする | `Ecu::input`、`Ecu::output` | [外部から信号を読み書きする（UI、テスト）](#外部から信号を読み書きするuiテスト) |
| 信号をビット位置を書かずに扱う | 型付きメッセージ | [データの形（Codec）](#データの形codec) |

最初に動かすなら [examples/getting_started](../../examples/getting_started) です。完全な例は [examples/j1939/ecu_pair](../../examples/j1939/ecu_pair) です。エンジンとキャブの 2 つの ECU をコンポーネントだけで作り、`Simulation` で仮想バスか SIL Kit の 2 つの participant として動かします。同じコードをテストでも使っています。

## コード例の読み方

この文書のコードは抜粋で、そのままではコンパイルできません。`#include`、`using`、変数の宣言を省いています。コンパイルできる全体がどのファイルのどの関数にあるかを、抜粋ごとに示します。リポジトリのファイルを短くしたものには「短くしたもの」と書きます。

抜粋は、[examples/j1939/ecu_pair/ecu_pair.hpp](../../examples/j1939/ecu_pair/ecu_pair.hpp) と同じ別名を使います。

```cpp
using namespace std::chrono_literals;
namespace j1939 = vehicle_sil::j1939;
namespace codec = vehicle_sil::j1939::codec;
```

## 利用者が書くものと、スタックが受け持つもの

| 利用者が書くもの | スタックが受け持つもの |
|---|---|
| ノードのアドレスと NAME | Address Claim（競合、任意アドレス、Commanded Address の受信、Claim の後の待ち） |
| 送信する PG と周期、データの作り方 | 周期の管理、アドレスを持つ間だけ送信すること、DM13 による停止 |
| 受信する PG と、受信したときの処理 | 宛先の振り分け、トランスポートプロトコル（9〜1785 バイト）の組み立て |
| Request を送信する相手と、応答の使い方 | 応答待ち、タイムアウト、再送、同じ要求を 1 つにまとめること |
| 独自プロトコルのコマンドと、応答の見分け方 | 送信できるようになってから送信すること、応答待ち、途中の応答、再送、自分と相手のアドレスの変化、長いコマンドの転送 |
| モデル（ふつうの C++） | 仮想時間、ステップ、スレッド、SIL Kit の participant とコントローラ |

## ノードを作る

ノードは `j1939::Network` の上に作ります。`Network` は 1 つの CAN ネットワーク（チャネル）への接続で、`Ecu::j1939(network)` が作ります。

`vehicle_sil::Ecu` は ECU のアプリケーションを組み立てるためのインターフェースです。SIL Kit の participant（`Peer`）と、プロセス内の仮想バスの ECU（`LocalBench::add_ecu()`）が実装します。そのため、`Ecu&` を受け取る関数かクラスに ECU を書くと、同じコードがどちらの上でも動きます。

[examples/j1939/ecu_pair/ecu_pair.hpp](../../examples/j1939/ecu_pair/ecu_pair.hpp) の `install_engine()` です。

```cpp
inline j1939::Node& install_engine(vehicle_sil::Ecu& ecu, EngineModel& engine)
{
    auto& node = ecu.j1939("J1939_CAN").add_node({.address = kEngineAddress, .name = kEngineName});
    ecu.every(10ms, [&engine](vehicle_sil::SimTime) { engine.tick(); });
    codec::attach_sender<EngineStatus>(node, 20ms, [&engine](EngineStatus& m) {
        m.speed.set(engine.rpm);
        m.state.set(engine.rpm > 0 ? EngineStatus::State::running : EngineStatus::State::stopped);
    });
    codec::on_message<SpeedRequest>(node, [&engine](const SpeedRequest& m) {
        if (const auto rpm = m.speed.value()) // not when the cab sends "not available"
        {
            engine.requested = static_cast<std::uint16_t>(*rpm);
        }
    });
    const std::string software[] = {"ENGINE", "1.0.3"};
    node.attach<j1939::Identification>().set_software_identification(software);
    return node;
}
```

| 行 | すること |
|---|---|
| `add_node()` | ノードを作ります（Address Claim はスタックが行います）。 |
| `ecu.every()` | モデルを 10 ms ごとに進めます。 |
| `codec::attach_sender` | `EngineStatus` を 20 ms ごとに送信します。 |
| `codec::on_message` | `SpeedRequest` を受信して、モデルに渡します。 |

`EngineStatus` と `SpeedRequest` は、YAML の定義から生成した型付きメッセージです（[j1939-codec.md](j1939-codec.md)）。信号を値として読み書きし、ビット位置やスケーリングを書きません。バイト列を直接扱うこともできます（下の各節）。

ノードと ECU の持ち物には、次のルールがあります。

| 項目 | ルール |
|---|---|
| `NodeOptions::name` を付けたノード | 最初のステップでアドレスを要求します。 |
| NAME を付けないノード | 指定したアドレスをすぐに使います（テスト用）。 |
| `node.attach<T>(...)` で付けたコンポーネント | `Network` が持ち、正しい順序で破棄します。 |
| ハンドラが参照するモデルやアプリのオブジェクト | `ecu.emplace<T>(args...)` で ECU に持たせます（`emplace` は実行の開始前だけ使えます）。 |
| `emplace` したものの破棄 | ECU がネットワークの後に、最後に emplace したものから先に破棄するので、ハンドラやコンポーネントは破棄の間も参照してかまいません。 |
| コンポーネントとハンドラを呼ぶスレッド | ステップを実行するスレッドです（ほかのスレッドから操作するときは「周期的な処理の使い分け」の「ステップの外からデータを渡す」）。 |

1 ステップの中では、次の順に処理します。

1. ネットワークの受信と処理
2. 時刻の来た変数の書き込みの反映
3. `on_step`、`every`、`at`（追加した順）
4. LIN のノードの処理（追加した順）
5. 変数の記録
6. ネットワークの送信

呼び出しには、次の制限があります。

| 呼び出し | 制限 |
|---|---|
| 同じネットワーク名の `j1939()` | ECU ごとに 1 回だけ呼べます。 |
| 開始後の `j1939()`、`on_step()`、`every()`、`at()`、`emplace()` | `std::logic_error` です。 |
| ノードの追加 | `Network::add_node()` のルールで、ステップの中からもできます。 |

`j1939::Network` を自分で作るプログラム（`Ecu` を使わないもの）は、`NetworkOptions::node_overrides` で、ノードのコードを変えずにアドレスと NAME の一部（identity number、function instance、ECU instance）を替えられます。同じノードのコードを 1 本のバスに 2 つ載せるための口です。行は、コードのアドレス（`from`。省くと、そのネットワークのただ 1 つのノード）でノードを指します。`add_node()` は、指されたノードを替えた後の値で作ります（`Node::address()`、`requested_address()`、`name()` は替えた後の値、`Node::coded_address()` だけがコードのアドレス）。行が当たるのは、最初の `begin()` より前に追加したノードだけです（`Network::close_node_overrides()` でも閉じられます。使われなかった行の番号を返します）。その後に追加したノードは、コードの値のままです。当てられない行（NAME のないノードに NAME の欄、など）と、1 つの行が 2 つ目のノードに当たる場合は、`add_node()` が `std::invalid_argument` を投げます。`Ecu::j1939()` で作るネットワークは `node_overrides` を受け付けません（`std::invalid_argument`）。ECU のノードの値は、ECU を動かす側が外から替えます（下の「シミュレーションで実行する」の `SilKitOptions::j1939_nodes`、`PeerOptions::j1939_nodes`）。

### 複数のネットワークを持つ ECU

ゲートウェイは、`j1939()` を異なるネットワーク名で呼びます。片方のハンドラから他方へ送信すると、同じステップで送信されます。

[examples/vehicle/main.cpp](../../examples/vehicle/main.cpp)（2 つの CAN と 1 つの LIN）の `Gateway` です。

```cpp
class Gateway // one ECU on two networks: what it receives on one it sends on the other
{
public:
    explicit Gateway(vehicle_sil::Ecu& ecu)
    {
        auto& to_body = ecu.j1939("Body").add_node({.address = 0x31});
        ecu.j1939("Powertrain").add_node({.address = 0x30}).on_message(kSpeed, [&to_body](const j1939::Message& m) {
            static_cast<void>(to_body.send(kSpeed, m.data));
        });
    }
};
```

### J1939 でないプロトコル（生の CAN）

`ecu.can(network)` は、J1939 を通さない生の CAN チャネル（`can::RawChannel`）を返します。`on_frame(handler)` で受信し、`send(frame)` で送信します。タイマーも分割もないので、周期の処理は `every()` や `on_step()` で書きます。

`ecu.can()` を使うコードは、`tests/simulation/test_disconnect.cpp` の `Engine` と `Dashboard` にあります。

| 項目 | ルール |
|---|---|
| `send()` を呼べる所 | ステップの中だけです（コンストラクターから呼ぶと `std::logic_error`）。 |
| `send()` の結果 | `RawSendStatus`（`Ok`、`QueueFull`、`InvalidFrame`）で返ります。 |
| 送信の時刻 | フレームは、そのステップの最後に、ステップの時刻で送信されます。 |
| 違う ECU から同じ時刻に受信したフレームの順 | SIL Kit では決まらないので、順に依存する処理を書かないでください。 |

同じ ECU が同じネットワーク名で `j1939()` と `can()` の両方を持ってもかまいません。別のコントローラーになるので、互いのフレームを受信します（J1939 のネットワークは 11 ビットのフレームを無視します）。

ただし、`Peer` の J1939 のネットワークが利用者のポート（`j1939(name, port)`。Vector XL など）の上にあるときは別のバスになり、互いのフレームは受信しません。生の CAN は常に SIL Kit の上にあるためです。

## メッセージを受信する

断片です（`node` はノード、`engine` はモデル）。`codec::on_message` の全体は上の `install_engine()` に、`Node::on_message` の全体は上の `Gateway` にあります。

```cpp
codec::on_message<SpeedRequest>(node, [&](const SpeedRequest& m) { engine.target_rpm = m.speed.value_or(800.0); });
node.on_message(kPgnRaw, [&](const j1939::Message& m) { log(m.data); });   // バイト列のまま
```

| 項目 | ルール |
|---|---|
| 受信するメッセージ | 自分のアドレス宛てとグローバル宛てです。 |
| 9 バイト以上のメッセージ | トランスポートプロトコルで組み立てたものを 1 回で受信します。 |
| `codec::on_message<M>` | decode できたもの（長さが足り、セレクターが合うもの）だけを渡します。 |
| `Message::data` | 呼び出しの間だけ有効なので、残すときはコピーします。 |
| 解除が必要な登録 | `j1939::Subscription` に持たせます（破棄で解除します）。 |
| 最新の値と途絶だけが必要なとき | `Monitor` を使います（「受信を監視する」）。 |

組み立てたメッセージの `Message::time` は転送が終わった時刻（最後のパケットの時刻）で、相手が送信を始めた時刻ではありません。相手の送信の周期を受信の時刻から確認するときは、単一フレームのメッセージで確認します。

## メッセージを送信する

| 送信のしかた | 使うもの |
|---|---|
| その場で 1 回 | `node.send(pgn, data, destination)` |
| 決まった時刻に 1 回 | `ecu.at(time, fn)` の中の `send()` |
| 周期的に | `PeriodicSender` |

### 1 回送信する

`node.send()` で送信できるのは、ステップの中（ハンドラ、タイマー、`Ecu::every`、`Ecu::on_step`、`Ecu::at` などアプリの処理）だけです。結果は `SendStatus` で返ります（例外ではありません）。

`main` から直接、`send()` や、送信するコンポーネントの関数（`examples/pid_stream` の `subscribe()`、`unsubscribe()` など）は呼べません（ステップの外のため）。

### 決まった時刻に 1 回送信する

ECU が決まった時刻に 1 回だけ行う操作（要求を送信する、購読を始める、止める）は `ecu.at(time, fn)` で書きます。

| 項目 | ルール |
|---|---|
| 呼ばれるステップ | 開始時刻が `time` 以後の最初のステップで、1 回だけ呼ばれます（ステップ幅が時刻を割り切らなくても動きます）。 |
| 実行が `time` に達しないとき | 呼ばれません。 |
| 登録できる時点 | 最初の実行の前だけです。 |

[examples/pid_stream/main.cpp](../../examples/pid_stream/main.cpp) の `Tool` のコンストラクターです。

```cpp
        auto& node = ecu.j1939("J1939_CAN").add_node({.address = 0x80});
        auto& client = node.attach<pid_stream::PidClient>(format);
        auto record = [this](const pid_stream::Reply& r) { replies_.push_back(r); };
        // The tool's script. Each action runs once, in the first step at or after its time (also
        // with a step size that does not divide it), from the step: subscribe() sends.
        ecu.at(SimTime{} + 100ms, [&client, record](SimTime) {
            client.subscribe(0x00, {kRpm, kCoolant, kLoad}, 50ms, record); // 11 bytes: transport protocol
        });
        ecu.at(SimTime{} + 300ms, [&client, record](SimTime) {
            client.subscribe(0x00, {kRpm, 0x7E}, 100ms, record); // renew: new period, 0x7E is unknown
        });
        ecu.at(SimTime{} + 600ms, [&client](SimTime) { client.unsubscribe(); });
```

### 周期的に送信する（`PeriodicSender`）

断片です（`node` はノード、`fill`、`encoder`、`bytes` は利用者のもの）。`codec::attach_sender` の全体は上の `install_engine()` にあります。周期、宛先、送信の停止の変更は `tests/j1939/test_j1939_periodic_sender.cpp` がテストしています。

```cpp
codec::attach_sender<EngineStatus>(node, 20ms, fill);                                   // 送信のたびに新しい EngineStatus を fill で埋める
codec::send(node, request, 0x80);                                                        // 型付きメッセージを 1 回
auto& status = node.attach<j1939::PeriodicSender>(kPgnEngineStatus, 20ms, encoder);   // 送信のたびに encoder を呼ぶ
auto& fixed = node.attach<j1939::PeriodicSender>(kPgnVersion, 1s, std::span(bytes)); // 決まったバイト列
fixed.set_data(new_bytes);                                                              // 次の送信から
status.trigger();                                                                       // 次の機会にすぐ送信する
status.set_period(50ms);                                                                // 今から数え直す
```

データの作り方は 3 つあります。

| 作り方 | 内容 |
|---|---|
| `codec::attach_sender` | 定義の優先度と長さで送信します。 |
| `Encoder`（span を取る関数） | コンポーネントのバッファ（`max_length` バイト、既定 8）に書き、書いた長さを返します。 |
| `PeriodicSender::VectorEncoder`（`std::vector<std::uint8_t>&` に追記する関数） | 長さが送信のたびに変わるメッセージを作ります。 |

9 バイト以上（`max_length` を上げる）は、トランスポートプロトコルで送信します。PDU1 の PG を 1 つのアドレスへ送信するときは `PeriodicSenderOptions{.destination = address}` です。

`VectorEncoder` には、次のルールがあります。

| 項目 | ルール |
|---|---|
| 渡されるベクター | 空で渡され、その大きさがメッセージの長さになります。 |
| メモリの確保 | コンポーネントがベクターを持ち回るので、追記だけなら送信のたびの確保はありません。 |
| 引数の型 | 省略せずに書きます（`std::vector<std::uint8_t>&` か `auto&`）。 |
| span を取る関数 | 長さを返す `Encoder` として扱います。 |
| `max_length` を超えたとき | span の形と同じく、`Network` が失敗します。 |

断片です（`ids`、`value_of`、`client` は利用者のもの）。同じ形のコードは、[examples/pid_stream/pid_stream.hpp](../../examples/pid_stream/pid_stream.hpp) の `PidStreamer` にあります。

```cpp
auto& reply = node.attach<j1939::PeriodicSender>(kPgnReply, 100ms, [&](std::vector<std::uint8_t>& out) {
    for (const auto id : ids) { out.push_back(id); out.push_back(value_of(id)); }
}, j1939::PeriodicSenderOptions{.destination = client, .max_length = 64});
```

周期、宛先、優先度は後から変えられます。

| 関数 | 効く時点 |
|---|---|
| `set_period()` | 呼んだ時点から新しい周期を数え直します（すぐ送信するなら `trigger()` も呼びます）。 |
| `set_destination()`、`set_priority()` | 次の送信から効き、周期を数える起点は変わりません。 |
| `trigger()` | アプリの処理（`Ecu::every` など）から呼ぶと、送信するのは次のステップです。 |

- 一時停止の間に変えれば、再開したときに新しい値で動きます。
- 不正な値は `std::invalid_argument` で、何も変わりません。
- 同じ相手からの新しい要求には、コンポーネントを作り直さずに周期を変えて応えられます（`examples/pid_stream`）。
- 呼んだステップで送信したいときは、`trigger()` ではなく `node.send()` を使います。

送信を始める時点と止める時点は、ノードのアドレスの状態で決まります。ノードが送信できるようになるまで（Address Claim の後の待ち、既定 250 ms の間も）送信せず、送信できるようになったときに 1 回送信してから周期を数えます。アドレスを失えば止まり、取り直せば再開します。

DM13 でブロードキャストを止めている間（ほかのノードの DM13 か、自分の `BroadcastSuppression::suspend()`）の扱いは、宛先で変わります。

| 送信 | DM13 で止めている間 |
|---|---|
| 全体宛てのブロードキャスト | 送信しません。 |
| 1 つのアドレス宛ての送信 | 既定では止めません（規格での扱いを確認していないため）。 |

既定では、止めるかどうかは、そのときの宛先で決まります（`set_destination()` で決め直します）。1 つのアドレス宛てで作った送信を全体宛てにするには、組み立てで `BroadcastSuppression::of(node)` を付けておきます。付けていなければ `std::logic_error` です。どちらの送信も、`PeriodicSenderOptions::suppressible`（true か false）で固定できます。

### トランスポートプロトコルの時間（`NetworkOptions::timing`）

9 バイト以上のメッセージは、トランスポートプロトコルで送ります（全体宛ては BAM、1 つのアドレス宛ては接続 RTS/CTS）。パケットの送信、欠けたパケットの求め直し、止まった転送の打ち切りは `Network` が受け持ちます。そのときに待つ時間と、Address Claim の後の待ちは、`NetworkOptions::timing` の項目です。

| 項目 | 既定 | 決めるもの |
|---|---|---|
| `t1` | 750 ms | T1: 受信側が次のパケットを待つ時間 |
| `t2` | 1250 ms | T2: 受信側が、自分の CTS の後の最初のパケットを待つ時間 |
| `t3` | 1250 ms | T3: 送信側が、受信側の CTS か EndOfMsgACK を待つ時間 |
| `t4` | 1050 ms | T4: 送信側が、接続を保留する CTS の後に待つ時間 |
| `max_retransmit_requests` | 2 | ウィンドウごとの再送の要求の回数。もう 1 回必要なとき、受信側は Abort する |
| `address_claim_wait` | 250 ms | ノードが Address Claimed の後に、送信するものを保留する時間 |

これらの既定は、このプロジェクトでは規格と照らしていません。そのため変えられるようにしてあります。模擬する ECU やつなぐ ECU が別の値を使うときは、`Ecu::j1939(network, NetworkOptions{.timing = {...}})`（`Peer` では `J1939ChannelOptions::network.timing`）で変えます。0 以下の時間は、`Network` のコンストラクターが `std::invalid_argument` を投げます。同じ `Network` の `Listener` も同じ時間を使います（下の「受信だけをする participant」）。詳しくは [internals/j1939.md](internals/j1939.md) の「トランスポートプロトコルのタイマー」にあります。

## Request に応答する

断片です（`node` はノード、`encode(model)` は利用者のもの）。`on_request` と `respond()` は `tests/j1939/test_j1939_request.cpp` がテストしています。

```cpp
node.on_request(kPgnSomething, [&](j1939::RequestContext& r) { r.respond(encode(model)); });
```

| 場合 | 動作と書き方 |
|---|---|
| 応答者のない PGN への、宛先を指定した Request | スタックが NACK を返します。 |
| ソフトウェア ID などの識別情報 | `Identification` コンポーネントが応答します（`node.attach<j1939::Identification>().set_software_identification(fields)`）。 |
| 診断（DM1 など） | `Diagnostics` コンポーネントが応答します。 |
| 何ステップかかけて応答するとき | `r.defer()` で要求を値として取っておき、後で `node.respond(pending, data)` を呼びます。 |

## Request を送信して応答を受信する

[examples/j1939/ecu_pair/ecu_pair.hpp](../../examples/j1939/ecu_pair/ecu_pair.hpp) の `install_cab()` の一部です（`cab` はノード、`dash` は表示のモデル）。

```cpp
    j1939::Requester::of(cab).request(kPgnSoftwareId, kEngineAddress, [&dash](const j1939::RequestResult& r) {
        if (r.outcome == j1939::RequestOutcome::Response)
        {
            dash.engine_software.assign(r.data.begin(), r.data.end());
        }
    });
```

| `RequestOutcome` | 意味 |
|---|---|
| `Response` | 要求した PG を受信した（`data`、`source`）。長い PG はトランスポートプロトコルで組み立てたもの |
| `Acknowledged` | 相手が Acknowledgement を返した（`ack` は肯定、否定、アクセス拒否、応答不能） |
| `Timeout` | 再送しても応答がなかった（グローバルの要求では、待ち時間の終わり） |
| `NotSent` | 送信できなかった（`send_status`。アドレスを要求できないときは `NoAddress`） |

| 項目 | ルール |
|---|---|
| `request()` を呼べる時点 | いつでも呼べます（ノードがアドレスを持つまで待ってから送信します）。 |
| グローバル（`kGlobalAddress`）への要求 | 待ち時間の間に受信した応答をそのたびに `last == false` で受け取り、最後に `Timeout` を受け取ります。 |
| 同じ PG を同じ相手に重ねて要求したとき | Request は 1 つで、全員が同じ応答を受け取ります。 |
| 待ち時間、再送の回数、間隔の下限 | `RequesterOptions` で決めます。 |

`RequesterOptions` は、`j1939::Requester::attach(node, options)` で渡します。何かが `of()` を使う前に呼びます（`Requester` はノードに 1 つです）。

## 受信を監視する

断片です（`cab` はノード、`dash` は表示のモデル）。`Monitor` を付ける全体は [examples/j1939/ecu_pair/ecu_pair.hpp](../../examples/j1939/ecu_pair/ecu_pair.hpp) の `install_cab()` にあります。`Monitor::data()` を型で読む形は `tests/j1939/test_j1939_codec_node.cpp` がテストしています。

```cpp
auto& eec = cab.attach<j1939::Monitor>(kPgnEngineStatus,
    j1939::MonitorOptions{.name = kEngineName, .timeout = 100ms,
                          .on_state = [&](const j1939::MonitorChange& c) { dash.silent = c.state == j1939::MonitorState::Lost; }});
...
if (const auto status = EngineStatus::decode(eec.data())) { use(status->speed.value()); }   // まだ受信していなければ空
```

| `MonitorState` | なるとき |
|---|---|
| `Waiting` | 最初の受信まで（途絶にしません） |
| `Receiving` | 受信している間と、途絶の後に受信が戻ったとき |
| `Lost` | 最後の受信から `timeout` が過ぎたとき |

- 送信元を NAME で指定すると、相手のアドレスが変わっても追います。
- `MonitorOptions::accept` に関数を渡すと、それが受け入れたメッセージだけを、最新の値と受信に数えます。
- 受け入れなかったものは `rejected()` に数え、途絶の判定にも使いません。

`accept` で 1 つの型だけを受け入れる形です。通し番号や検査値の確認も同じ形で書きます。

```cpp
.accept = [](const j1939::Message& m) { return j1939::codec::decode<EngineStatus>(m).has_value(); }
```

## ほかのノードの故障コード（DM1、DM2）を読む

断片です（`node` はノード、`dash` と `log` は利用者のもの）。`parse_dtc_list()` は `tests/j1939/test_j1939_diagnostics.cpp` がテストしています。

```cpp
node.on_message(j1939::kPgnDm1, [&](const j1939::Message& m) {
    const auto list = j1939::parse_dtc_list(m.data);   // 確保しない。m.data を指すだけ
    if (!list.valid()) { return; }                        // 長さが合わない
    dash.amber = list.lamp(j1939::Lamp::AmberWarning).has_value();
    dash.low_oil = list.has(0x7F0, 1);
    for (const j1939::ReadDtc& dtc : list) { log(dtc.spn, dtc.fmi, dtc.occurrences); }
});
```

`parse_dtc_list()`（`j1939/diagnostics.hpp`、CMake のターゲット `vehicle_sil::j1939_diagnostics`）は、`Diagnostics` コンポーネントが送信する形の逆として読みます。ランプの 2 バイトの後に、故障コードごとに 4 バイト（SPN、FMI、発生回数）が続く形です。どんな長さのバイト列を渡しても安全に読めます。

| 渡したバイト列 | 読み方 |
|---|---|
| 8 バイト以下 | 故障コード 1 つです（後ろはパディング）。 |
| 8 バイトより長いもの | 2 + 4 × 件数 のバイトでなければ、`valid()` が偽で `size()` は 0 です。 |
| ただ 1 つの故障コードが 4 バイトの 0（故障コードのない一覧） | `valid()` が真で `size()` が 0 です。 |

| 項目 | 読み方 |
|---|---|
| 戻り値 | ビューです（確保せず、渡したバイト列を指すだけ）。 |
| 使える期間 | 渡したバイト列が有効な間だけです（ハンドラでは呼び出しの間だけ）。 |
| 残すとき | `ReadDtc` をコピーします。 |
| `lamp(Lamp)` | ランプが点いていれば点滅の速度（`LampFlash`）を、消えていれば空を返します。 |
| `Diagnostics` が送信しないランプの値 | 状態なら消灯、点滅なら点灯（`Solid`）として読みます。 |
| 別の読み方が必要なとき | `data()` の生のバイトを読みます。 |
| `ReadDtc::raw` | 1 件の 4 バイトを生のまま持ちます。 |
| `ReadDtc::occurrences` | 4 バイト目の下位 7 ビットです。 |
| `ReadDtc::byte4_high_bit` | 4 バイト目の最上位ビットを、解釈せずにそのまま持ちます。 |

4 バイト目の最上位ビットの意味は、規格と照らして確認していないので、使う人が決めます。`Diagnostics` は 0 で送信します。

## ほかのノードのアドレスを変える

断片です（`service` はノード）。`AddressCommander` は `tests/j1939/test_j1939_monitor.cpp` がテストしています。

```cpp
auto& tool = service.attach<j1939::AddressCommander>();
tool.command(kCabName, 0x81, [](const j1939::CommandResult& r) { /* r.outcome == CommandOutcome::Moved */ });
```

相手が新しいアドレスで Address Claimed を送信すると `Moved` です。任意アドレス可能でない相手は従いません（`Timeout`）。

## 周期的な処理の使い分け

### 周期と時刻で実行する

| 使うもの | 向いているもの | 時刻 |
|---|---|---|
| `PeriodicSender` | PG を周期的に送信する | 期限は仮想時間の格子でずれず、送信はその後の最初のステップ（PG ごとにタイマーを 1 つ使う） |
| `Ecu::every(period, fn)` | モデルの計算（`tick()`）、アプリの周期的な処理 | 0、period、2 * period、… の時刻以後の最初のステップ（1 ステップに 1 回まで） |
| `Ecu::on_step(fn)` | 毎ステップの処理 | ステップの時刻（受信の処理の後、送信の前） |
| `Ecu::at(time, fn)` | ECU が決まった時刻に 1 回だけ行う操作（要求を送信する、購読を始める、止める） | 開始時刻が `time` 以後の最初のステップで 1 回（`time` がステップ幅で割り切れなくてもよい） |
| `TickScheduler` | 固定周期の既存モデル（DLL の `tick()`）。`Ecu` の外、`ParticipantRuntime` のステップハンドラで使う | ステップ幅の倍数の周期だけ（違えば例外） |
| `j1939::Timer` | コンポーネントの中の時間（待ち時間、遅延、独自の周期） | イベントの時刻から数える（ステップ幅によらない） |

`Ecu::every` の period は、ステップ幅の倍数でなくてもかまいません。ステップ幅 10 ms、period 25 ms なら、0、30、50、80、100 ms… に呼ばれます。

モデルの周期とメッセージの周期は分けて書きます。モデルは `every` で進め、送信する PG は `PeriodicSender` の `Encoder` でモデルの今の値から作ります。

ステップ幅はメッセージの最短の周期以下にすると、送信の回数がステップ幅によりません。長いと、1 ステップの中の期限をまとめて 1 回送信します。

### ステップの外からデータを渡す

| 使うもの | 向いているもの | 決定性 |
|---|---|---|
| `StepInbox` | ほかのスレッドや SIL Kit のハンドラで受信したデータを、ステップで取り出す | 下の 2 つを守るときだけ決定的 |
| Control Agent の書き込み | Control API からの変数の書き込み | 仮想時刻（`Command::at`）を指定すれば決定的で、指定しなければ次のステップ |
| `Host::post(fn)`（`LocalBench::host(ecu)`、`Peer::host()`） | ほかのスレッドや、ステップの外からの操作 | 時刻はなく、動くステップは post した時刻による |

`StepInbox` には時刻つきで入れます。ステップでは、そのステップより前の時刻のものを、時刻、タイブレーク、到着の順に取り出します。決定的になるのは、次の 2 つを守るときだけです。

| 守ること | 理由 |
|---|---|
| 送信元を区別するタイブレークを選ぶ | 選ばないと、異なる送信元の同じ時刻のものの順が決まりません。 |
| 容量を最悪の場合に合わせて取り、あふれをエラーとして扱う | 上限の近くのあふれは決定的でありません。 |

決定性が必要なデータは、`Host::post` ではなく `StepInbox` に入れます。

## エラーの扱い

| 起きること | 現れ方 |
|---|---|
| 送信の失敗 | `SendStatus` の戻り値（`QueueFull`、`Busy`、`NoAddress` など） |
| 使い方のエラー（不正な PGN、範囲外の値） | コンポーネントの生成や呼び出しの時点で、`std::invalid_argument` などの例外 |
| タイマーの枠の不足 | コンポーネントの生成が `std::length_error`（メッセージに `timer_capacity` と表示） |
| ハンドラ、`Encoder`、コールバックから投げた例外 | `Network` が失敗状態になる |

- コンポーネントは送信の失敗を数えて（`statistics()`）、次の機会にまた送信します。
- 失敗状態は、`Simulation` では `SimulationError` になります（「シミュレーションで実行する」）。
- `Peer` を直接使うときは、`Peer::failed()` と `error()` で失敗がわかります。
- 通信の状態は、`Network::statistics()`、コンポーネントの `statistics()`、`Monitor` の状態で見ます。

### タイマーの枠（`timer_capacity`）

タイマーの枠は `NetworkOptions::timer_capacity`（既定 64）です。周期送信の多いノードでは、`Ecu::j1939(network, NetworkOptions{.timer_capacity = ...})`（`Peer` では `J1939ChannelOptions::network.timer_capacity`）で上げます。各コンポーネントが使うタイマーの数は、コンポーネントのヘッダーのコメントと [internals/j1939.md](internals/j1939.md) の「プロトコルのコンポーネント」にあります。

受信した要求ごとにコンポーネント（`PeriodicSender`、`Timer` など）をハンドラの中で作るときは、作る前に残りの枠を確認します。`node.network().timers_free()`（残りの枠の数）を、作るコンポーネントの `kTimers`（`PeriodicSender::kTimers` など）と比べ、足りなければ要求を拒否します。

ハンドラの中の `std::length_error` は `Network` を失敗させます。そのため、確認しないと、多数のアドレスから要求を送信するだけでその ECU の通信を止められます。確認するコードは、[examples/pid_stream/pid_stream.hpp](../../examples/pid_stream/pid_stream.hpp) の `PidStreamer` にあります。

## 独自のプロトコルを作る

Proprietary A/B やメーカー独自の通信手順は、vehicle-sil を変更せずに、利用者のコンポーネントとして作れます。使うのはノードの公開 API、ツールキット（`Subscription`、`Timer`、`Node::attach`）、既存のコンポーネント、型付きメッセージだけです。

| 例 | 内容 |
|---|---|
| [examples/pid_stream](../../examples/pid_stream) | 最初の手本です（要求を受信して周期的に返すコンポーネント）。 |
| [examples/j1939/param_service](../../examples/j1939/param_service) | 完全な例です（サービスツールがエンジン ECU のパラメーターとブロックを Proprietary A で読み書きします）。 |

`param_service` が扱うのは、セッションの開始と終了、書き込み中の途中の応答、トランスポートプロトコルで送信するブロック、エラー応答、無通信でのセッションの終了、相手の消失です。テストは `tests/testkit/test_param_service.cpp` です。

### メッセージを定義する

固定長のメッセージは YAML で定義します（[j1939-codec.md](j1939-codec.md)）。1 つの PGN に複数のメッセージを載せるときは、操作のバイトを `selector` にします。

[examples/j1939/param_service/param_service_messages.yaml](../../examples/j1939/param_service/param_service_messages.yaml) の `messages` の 1 つです。

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

コマンドを 1 つの相手へ送信し、応答を待つのは `j1939::Exchange` です（ターゲット `vehicle_sil::j1939_exchange`）。

[examples/j1939/param_service/param_service.cpp](../../examples/j1939/param_service/param_service.cpp) の `ParamClient::run()` と `answer_to()` を、読み出しだけに短くしたものです。この形のままのコードは、リポジトリにはありません。

```cpp
j1939::Exchange exchange(node, {.timeout = 200ms, .retries = 1});
messages::Read read;
read.session.set_raw(session);
read.parameter.set_raw(0x0100);
exchange.start(kPgn, read.bytes(), kEngineName,          // 相手は NAME（アドレスでもよい）
    [](const j1939::Message& m) {                         // 受信したメッセージが何か（PGN も見る）
        if (const auto ok = j1939::codec::decode<messages::ReadOk>(m); ok && ok->parameter.raw() == 0x0100) {
            return j1939::ReplyMatch::Final;              // この読み出しの応答
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
| `Timeout` | 再送しても応答がなかった（`Pending` の後は再送せず、最後の `Pending` から `timeout`） |
| `NotSent` | コマンドが相手に渡っていない（下の表） |
| `Interrupted` | 送信したが、応答を受信できなくなった（下の表）。相手が実行したかはわからない |
| `PeerLost` | 相手がいなくなった（下の表） |

| `ExchangeOutcome` | なるとき |
|---|---|
| `NotSent` | 一度も送信できなかった（`send_status`。アドレスを要求できなくなったときは `NoAddress`） |
| `NotSent` | 長いコマンドの転送が最後のパケットの前に終わったか、相手に中断された（`transfer`、`reason`） |
| `Interrupted` | 自分のアドレスが変わったか、失われた |
| `Interrupted` | 長いコマンドの最後のパケットの後に、このノードがタイムアウトで転送を中断した |
| `Interrupted` | 再送を送信できなかった |
| `PeerLost` | 相手がアドレスを失った、移った、別の NAME に取られた |
| `PeerLost` | NAME で指定した相手が、`timeout` の間にアドレスを要求しなかった |

#### 応答を見分ける（`match`）

`match` には、相手のアドレスから受信したメッセージが、PGN によらずすべて渡ります（このノード宛てとグローバル。相手の周期のブロードキャストも渡ります）。`M::decode(data)` はバイト列だけを見るので、PGN も確認する `j1939::codec::decode<M>(message)` を使います。

自分のコマンドへの応答かどうかは、操作のバイト、セッション、コマンドが指す項目（パラメーターの番号など）で見分けます。

`match` が `Pending`（途中の応答）を返すと、待ち時間を数え直し、再送をやめます。

前のコマンドへの遅れた応答（再送した元のコマンドへの応答、`cancel()` したコマンドへの応答）も `match` に渡ります。応答が項目や番号を持たない手順では、それを次のやり取りの応答と取り違えます。応答が遅れうるなら、コマンドと応答に項目か通し番号を持たせて照合します。

`cancel()` は、送信したコマンドを取り消しません（応答は来ます）。

#### コマンドごとの設定（`CommandOptions`）

`start()` の最後の引数です。指定しない項目は `Exchange` 全体の設定（`ExchangeOptions`）を使います。

| 項目 | 意味 |
|---|---|
| `priority` | 優先度（既定 6） |
| `timeout` | このコマンドの応答の待ち時間（消去のように時間のかかるコマンドだけ長くする） |
| `retries` | タイムアウトの後に再送する回数（書き込みや消去は 0 にする） |
| `long_answer` | トランスポートプロトコルで来る応答の PGN（下の「長いコマンドと長い応答」） |
| `on_address_change` | 送信した後に自分のアドレスが変わったときの動作で、`Stop`（既定。`Interrupted` で終える）か `SendAgain`（新しいアドレスから再送する） |

待ち時間は、コマンドを送信したステップから数えます。次のときは、そこから数え直します。

- 再送したとき（その再送を送信したステップから）
- 長いコマンドの転送が終わったとき
- `Pending` を受信したとき（その時刻から）
- `long_answer` の転送が終わったとき（その時刻から）

#### 再送

再送には 2 種類あり、別々に設定します。

| 再送 | 設定 | 既定 | 使うコマンド |
|---|---|---|---|
| タイムアウトの後 | `retries` | 0 回 | 同じコマンドを 2 回受信しても困らない手順（読み出し、同じ値の書き込み、番号付きのコマンド）だけ |
| 自分のアドレスが変わったとき | `on_address_change`（`retries` には数えない） | 再送せず `Interrupted` | 2 回実行してよいコマンドだけ `SendAgain` にする |

書き込みや消去は `on_address_change` を既定のままにし、`Interrupted` を受け取ったら、読み出して確認するなど手順の側で判断します。

`SendAgain` で再送すると、相手から見ると送信元が変わります。そのため、相手がセッションを持つ手順では「セッションがない」と応答することがあります（例の `param_service` では、ツールがセッションを開き直します）。

#### 長いコマンドと長い応答

| 場合 | 動作 |
|---|---|
| 9 バイト以上のコマンド（`max_length` を上げる） | トランスポートプロトコルで送信し、転送が終わってから待ち時間を数えます。 |
| `long_answer` に PGN を指定した長い応答 | 受信している間はタイムアウトになりません。 |
| `long_answer` を指定しない長い応答 | 応答の転送の時間を `timeout` に含めます。 |

`long_answer` は、とくに BAM の応答（パケットの間隔は 50 ms 以上）で使います。対象は、相手がこのノード宛てかグローバルに送信する、その PGN の転送だけです（無関係なブロードキャストでは止まりません）。

タイムアウトを止めておけるのは、1 回の試行につき `ExchangeOptions::answer_transfer_limit`（既定 60 秒）までです。終わらない転送や繰り返し始まる転送でも、待ちは必ず終わります。

#### 相手のアドレスと NAME

| 場合 | 動作 |
|---|---|
| `start()` を呼ぶ時点 | いつでも呼べます（ステップの外でも、アドレスを得る前でも）。ノードが送信できるようになってから送信します。 |
| NAME で指定した相手が、まだアドレスを要求していない（起動の直後） | その Address Claimed を待って送信します。 |
| NAME で指定した相手が、待ちの間に移った | 再送が残っていても `PeerLost` で終わります（相手がやり取りの状態を失ったかもしれないため）。 |

相手が移った後の次のやり取りは、新しいアドレスへ送信します。

#### 扱わないことと上限

`Exchange` は 1 つの相手とのやり取りだけを扱います。次の 2 つは扱いません。

- 全体への問い合わせと複数の応答の収集（`Requester` のグローバルの要求を使います）
- 相手と別のアドレスから来る応答（ゲートウェイ経由など）

| 上限 | 値 |
|---|---|
| 同時に動かせるやり取り | `capacity`（既定 4） |
| タイマー | `Exchange` ごとに 2 つ |

コールバックと `match` の中から、次のコマンドを `start()` し、`cancel()` し、`Exchange` を破棄できます。破棄すると、残りのやり取りのコールバックは呼ばれません。

### 手順を書く（複数のメッセージ）

手順は、前の応答のコールバックで次のコマンドを `start()` してつなぎます。セッションのように前の応答で次のコマンドが決まる手順や、操作を順に 1 つずつ行う手順は、利用者のコンポーネントが操作の待ち行列を持ちます。

例の `ParamClient`（[examples/j1939/param_service/param_service.cpp](../../examples/j1939/param_service/param_service.cpp)）は、次のようにしています。

| 場面 | `ParamClient` の動作 |
|---|---|
| セッションがない | 開いてから操作します。 |
| 「セッションがない」と応答された | 1 回だけ開き直します。 |
| 読み出し、Open、Close | `SendAgain` にします。 |
| 書き込み | 既定の `Stop` のままにし、アドレスが変わったら `Interrupted` を利用者に返します。 |
| ブロックの読み出し | `long_answer` を指定します。 |

利用者へのコールバックには、2 つのルールがあります。

- 利用者へのコールバックは、自分の状態を確定してから呼びます。
- `read()` などの呼び出しの中で、その利用者のコールバックを呼ばないようにします（呼び出し側の状態が途中のことがあります）。

例では、次の操作を始めてから前の操作のコールバックを呼びます。そのため、コールバックの中でコンポーネントを破棄しても安全です。

### 応答する側

応答側は `on_message` とノードの `send()` で書きます（専用のコンポーネントはありません）。

[examples/j1939/param_service/param_service.cpp](../../examples/j1939/param_service/param_service.cpp) の `ParamServer` のコンストラクターと `ParamServer::on_command()` を短くしたものです。この形のままのコードは、リポジトリにはありません。

```cpp
ParamServer(j1939::Node& node, ParameterTable& table)
  : node_(node), table_(table),
    commands_(node, node.on_message(kPgn, [this](const j1939::Message& m) { on_command(m); })),
    claims_(node, node.on_message(j1939::kPgnAddressClaimed, [this](const j1939::Message&) { on_claim(); })),
    idle_(node, j1939::TimerKind::Timeout, [this] { end_session(); }) {}

void on_command(const j1939::Message& m) {
    if (m.destination != node_.address()) { return; }              // Proprietary A のグローバル宛ては使わない
    ...
    j1939::codec::send(node_, reply, m.source);                    // 応答は送信してきたアドレスへ
    idle_.start_after(2s);                                          // 受信したメッセージの時刻から数える
}
```

| 場面 | 書き方 |
|---|---|
| 9 バイト以上の応答 | `node.send(pgn, data, to, priority, j1939::WhenBusy::Queue)` で送信します（同じ相手への前の転送が終わるのを待ちます）。 |
| 時間のかかる処理（フラッシュへの書き込みなど） | `Timer` で後から応答します。 |
| 自分のアドレスが変わった（`on_address_change`） | 相手のコマンドは古いアドレスへ来ていたので、セッションを終えます。 |
| NAME のわからない相手 | 無通信のタイムアウトで終えます。 |

時間のかかる処理の間に `Pending` のような途中の応答を周期的に送信すると、要求側の `Exchange` は待ち続けます。

相手の消失は、次の手順で見つけます（Cannot Claim と、別の NAME にアドレスを取られた場合を含みます）。

1. セッションを開いたときに、`node.network().name_at(m.source)` で相手の NAME を記録します。
2. Address Claimed を受信するたびに、`node.network().address_of(name)` が同じアドレスかを確認します。

### 共通のルール

| 項目 | ルール |
|---|---|
| 時間 | スレッド、イベントループ、OS の時計を使わず、`Timer`（仮想時間）で数えます。 |
| 解除 | 登録は `Subscription`、タイマーは `Timer`、要求は `Exchange` に持たせると、コンポーネントの破棄で自動で解除されます。 |
| コンポーネントの持ち主 | コンポーネントは `attach` で `Network` に持たせるか、`Network` より先に破棄します。 |
| ハンドラの中でコンポーネントを作るとき | `timers_free()` で枠を確認してから作ります（「エラーの扱い」）。 |
| 標準の Request（PGN 59904） | `Requester::of(node)` を使います（`Exchange` で作り直しません）。 |
| 自分の利用者を呼ぶループ | 通知の前に自分の状態を確定し、通知の中で追加されたものを同じループで扱いません（[internals/j1939.md](internals/j1939.md) の「ツールキット」の「利用者に通知するループ」）。 |
| `on_any_request` と `Network::set_observer` | 使いません。 |

アプリが持つコンポーネント（`std::unique_ptr` など）は、ハンドラやタイマーのコールバックの中で破棄してかまいません（相手の Stop を受信したら止める、など）。破棄したコンポーネントのハンドラは、同じ配送の残りでも呼ばれず、タイマーも発火しません。

コンポーネントが呼ぶ関数（`PeriodicSender` の Encoder、`Monitor` と `Exchange` のコールバック）の中でも、そのコンポーネントを破棄してかまいません。例外は `AddressCommander` のコールバックと `Diagnostics` の `on_clear` です。

### 独自のプロトコルをテストする

例のテスト（`tests/testkit/test_param_service.cpp`）は、正常系のほかに次のことを確認しています。どれも仮想時間で、同じ入力からは同じフレームが送信されます。

| 起こすこと | 方法 |
|---|---|
| 相手の無応答 | `bus.endpoint("engine").add_outage(from, until)` |
| 相手・自分のアドレスの喪失と移動 | 優先度の高い NAME の Address Claimed を `bus.inject()` で入れる |
| 無通信でのセッションの終了 | `bus.run_for()` で待つ |
| 2 台目のツール | 同じバスに `add_j1939()` でもう 1 つのネットワークを追加する |
| コンポーネントの破棄（処理中、コールバックの中） | コンポーネントを `std::unique_ptr` で持ち、途中で `reset()` する |

コールバックの中で時刻を見るときは、そのネットワークの `now()`（ステップの時刻）を使います。`LocalBench::now()` は、最後の `run_until()` の終わりの時刻です。

## シミュレーションで実行する

`vehicle_sil::Simulation`（ターゲット `vehicle_sil::simulation`）が ECU を動かします。動かす場所はコンストラクターで選び、ECU のコードは変わりません。実行時に選ぶときは、`std::optional<SilKitOptions>` を渡します（空ならプロセス内の仮想バス）。

[examples/j1939/ecu_pair/main.cpp](../../examples/j1939/ecu_pair/main.cpp) の `main()` を、引数の処理と表示を除いて短くしたものです。層を自分で組み立てる例は [examples/low_level](../../examples/low_level) です。

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
| ステップの例外の後 | 次の `run_for()` も `SimulationError` | `run_for()` が simulation を終えてから投げる |
| 低水準 | `local()`: バス（CAN と LIN）、故障注入、生フレーム、tap（`vehicle_sil/local/local_bench.hpp`） | `peer(name)`: tap、Control Agent、SIL Kit のポート（`vehicle_sil/peer/peer.hpp`）。`registry_uri()` |

`run_for(d)` は、終わりの時刻（仮想バスでは今の時刻から `d` 進めた時刻）より前に始まるステップだけを動かします。

SIL Kit で 1 回しか動かせないのは、止まった時刻のステップがまだ実行中のことがあり、メインスレッドからモデルを読むと競合するためです。止めながら確認するときは、仮想バスを使います。

ステップの例外は、`SimulationError` として `run_for()` から投げられます。`what()` は `<ECU 名>: <理由>` です。SIL Kit では、失敗した participant があっても simulation は終わらず、仮想時間が止まるだけです。そのため `run_for()` は、失敗を見つけると simulation を終えてから投げます。

| 項目 | ルール |
|---|---|
| SIL Kit でのスレッド | ステップは SIL Kit のスレッドで実行されるので、ステップが書いた値は `run_for()` が戻ってから読んでください。 |
| `Simulation` の関数 | 1 つのスレッドから呼びます。 |
| 破棄の順序 | 後から追加した ECU から先に、ECU の持ち物（`emplace`）まで破棄し、次に controller、最後にプロセス内の registry です。 |
| LIN の Master と Slave | `Ecu::lin_master(network, schedule)` と `Ecu::lin_slave(network)` で作ります（仮想バスでも SIL Kit でも同じ結果になります。[internals/lin.md](internals/lin.md)）。 |

### SIL Kit の設定（`SilKitOptions`）

`SilKitOptions` で、既存の registry（`registry_uri`）や、ほかのプロセスの participant との併用を選べます。

| 項目 | 意味 |
|---|---|
| `registry_uri` | 接続する registry。空ならプロセス内に立てる（`registry_uri()` で場所がわかる） |
| `controller` | このプロセスが simulation を始めて終えるか（既定 `true`）。`false` なら外の controller（`vehicle_sil_sim_controller`、Control Server）が決め、`run()` で終わりまで待つ |
| `other_participants` | 始める前にそろっていなければならない、ほかのプロセスの Coordinated な participant（`controller` が `true` のときだけ） |
| `start_timeout` | 始まるまでに許す時間（既定 30 秒） |
| `network_names` | ネットワークの名前の対応（シミュレーションの名前 → ECU のコードの名前）。このプロセスの全部の ECU に効く |
| `j1939_nodes` | J1939 のノードのアドレスと NAME の一部を、ECU のコードを変えずに替える（ECU の名前 → 行の並び）。下の説明を参照 |

`j1939_nodes` は、同じ ECU のコードを 2 つ追加するためのものです。そのままでは、2 つの ECU が同じアドレスを同じ NAME で要求し、どちらもアドレスを持てません。

```cpp
vehicle_sil::SilKitOptions options;
options.j1939_nodes["Engine2"] = {{"VehicleCAN", {.from = 0x00, .address = 0x01, .identity_number = 2}}};
vehicle_sil::Simulation sim(options);
install_engine(sim.add_ecu("Engine1", 10ms), ...);   // コードのとおり 0x00
install_engine(sim.add_ecu("Engine2", 10ms), ...);   // 0x01、identity number 2
```

- 行は、ネットワーク（シミュレーションでの名前。`network_names` で替えたネットワークは、替えた後の名前）と、ノードのコードのアドレス（`from`。省くと、その ECU のそのネットワークのただ 1 つのノード）で、ノードを指します。替えられるのは `address`（254 は、任意アドレス対応の NAME のノードだけ）、`identity_number`、`function_instance`、`ecu_instance` です。
- `network_names` と違って ECU ごとに書きます。全部の ECU に同じ行を当てると、同じネットワークの 2 つの ECU がまた同じアドレスになるためです。
- 当たるのは、開始までに追加したノードだけです。開始の後に追加したノードは、コードの値のままです。
- 替わるのは、そのノード自身だけです。このノードのアドレスや NAME の全体をコードに持っているほかの ECU は、変わりません。
- `std::invalid_argument` になるもの: 空のネットワーク名、範囲の外の値、同じネットワークと `from` の行が 2 つ、`from` のない行と同じネットワークの別の行（`add_ecu()`）。当てられない行（NAME のないノードに NAME の欄、任意アドレスに対応しない NAME に 254）、1 つの行が 2 つ目のノードに当たる場合（`add_node()`）。この simulation にない ECU の名前、どのノードにも当たらなかった行（`run()`、`run_for()`。何も始めずに投げます）。
- プロセス内の仮想バス（`Simulation()`）には、この口はありません。

`run()` は、誰が終えてもよい simulation の終わりまで待ちます（SIL Kit だけ）。失敗にするものは、`controller` で変わります。

| `controller` | `run()` が失敗にするもの |
|---|---|
| `true` | Error で離脱した participant |
| `true` | Error 以外で離脱し、数秒のうちに simulation を終えなかった participant |
| `false` | このプロセスの ECU の失敗だけ |

### `Peer` を直接使うとき

`Peer` を直接使うのは、次のときです（[internals/execution.md](internals/execution.md) の「Peer」、[peers.md](peers.md)）。

- 実時間で動かすとき
- 別のプロセスで 1 つの participant として動かすとき
- Control Agent を組み込むとき

`Peer` も `Ecu` なので、`install_engine(peer, model)` のように同じ関数が使えます。participant を分けるときは `Coordinated` にして、`SimulationController` と一緒に起動します（1 つのプロセスなら `start_together`）。`Peer` では、同じ行を `PeerOptions::j1939_nodes`（その ECU の行の並び）に書きます。どのノードにも当たらなかった行があると、`Peer::start()` が何も始めずに `std::invalid_argument` を投げます（先に `Peer::unused_j1939_nodes()` で、行ごとの理由を得られます）。`start_together` では、どれかの `Peer` にそういう行があると、どれも始めずに、その `Peer` の `start()` と同じメッセージで投げます。`start()` の前に `host()` から自分でステップを回すプログラムでは、行が当たるのは最初のステップまでに追加したノードだけです。

## ECU をテストする

自分の ECU は、`Simulation` の仮想バスでテストします。時刻は仮想時間で、`sleep` は不要です。同じ入力からは同じ結果になります。

断片です（`CHECK` はテストのマクロ）。同じ 2 つの ECU を同じツールでテストするコードは `tests/testkit/test_ecu_personas.cpp` に、故障の注入と観測の例は [examples/fault_injection](../../examples/fault_injection) にあります。

```cpp
vehicle_sil::Simulation sim;                               // 仮想バス
auto& engine_ecu = sim.add_ecu("engine", 10ms);
install_engine(engine_ecu, engine_ecu.emplace<EngineModel>());
auto& cab_ecu = sim.add_ecu("cab", 5ms);
auto& dash = cab_ecu.emplace<Dashboard>();
install_cab(cab_ecu, dash);
auto& bus = sim.local();                                   // LocalBench（vehicle_sil/local/local_bench.hpp）
testkit::J1939Events events(bus.network("cab"));           // スタックのイベント（読み取り専用）
testkit::FrameLog wire(bus.tap("engine"));                 // バスのフレーム（読み取り専用）
sim.run_for(1s);                                           // 少しずつ進め、そのたびに確認できる
bus.endpoint("engine").add_outage(sim.now(), sim.now() + 300ms);   // エンジンがバスから切断される
sim.run_for(1s);
CHECK(bus.network("cab").node(0).find<j1939::Monitor>()->losses() == 1);
```

| 使うもの | できること |
|---|---|
| `testkit::faults`、`add_outage` | 故障注入（フレームの欠落、遅延、通信断） |
| `inject()` | 生フレームの注入 |
| `Monitor` | 通信の途絶を見る |
| `testkit::J1939Events` | 送信の要求、フレームの送出、転送の終わり、アドレスの変化を見る |
| `testkit::FrameLog` | バス上のフレームを見る |

`J1939Events` はネットワークごとに、`FrameLog` はタップごとに 1 つです。同じネットワークかタップに 2 つ目を作ると `std::logic_error` になるので、1 つ目を要る補助関数と共有します。

記録と故障注入のツール（`testkit::J1939Events`、`FrameLog`、`faults`）は `vehicle_sil::testkit` にあり、使うときはそれをリンクします（`vehicle_sil::simulation` はリンクしません）。

`LocalBench` の関数には、名前を渡します。

| 渡す名前 | ルール |
|---|---|
| ECU とネットワーク | `network("cab", "J1939_CAN")` のように 2 つ渡します（ネットワークが 1 つの ECU には、1 つの名前の形も使えます）。 |
| バス | バスが 1 つなら `inject(t, frame)` のように省けます。 |
| バス（2 つ以上あるとき） | `inject("J1939_CAN", t, frame)` のように渡します。 |

### `LocalBench` を直接使う

`LocalBench::add_ecu(name, step)` は `Ecu&` を返します。`add_j1939(name, step)` は、1 つのネットワークを持つ ECU の略記です。

バスの名前を渡す形と省く形は、次のように違います。どちらの順で書いても同じバスです。

| 形 | 関数 | 動作 |
|---|---|---|
| 名前を省く | `add_j1939()`、`bus()`、`injector()`、`inject(time, frame)` | ただ 1 つのバスを使い、まだなければ作ります。 |
| 名前を渡す | `bus(network)`、`injector(network)`、`inject(network, time, frame)` | 名前でバスを探すだけで、名前を付けません（その名前のバスがなければ `std::out_of_range`）。 |

名前を省く形で作ったバスは、後から来た `Ecu::j1939(network)` か `Ecu::can(network)` の名前になります。

| 使い方 | 結果 |
|---|---|
| バスが 2 つ以上あるときに名前を省く | `std::logic_error` |
| `add_j1939()` で ECU を追加した後に、2 つ目のバスの名前を使う | `std::logic_error`（その ECU のバスが決まらないため） |

バスを複数使うときは、ECU を `add_ecu(name, step).j1939(network)` で追加し、`inject(network, time, frame)` のようにバスの名前を渡します。

### ECU をバスから切断する（`ecu.connected`）

どの ECU にも、bool の入力 `ecu.connected` があります。false を書くと、その ECU は動いたまま、すべてのバスから切断されます。`add_outage` と違って、`Simulation()`（仮想バス）でも SIL Kit でも、シナリオ（[scenario.md](scenario.md) の「ECU をバスから切断する」）、Control API、`agent()` のどれからでも同じように使えます。

断片です（`sim` は `Simulation`）。同じ書き込みを `tests/simulation/test_disconnect.cpp` がテストしています。

```cpp
sim.ecu("engine").agent().submit({1, {{"ecu.connected", false}}, SimTime{} + 300ms});
sim.ecu("engine").agent().submit({2, {{"ecu.connected", true}}, SimTime{} + 600ms});
```

J1939 では次のように見えます（保証は [internals/execution.md](internals/execution.md) の「`Ecu` クラス」の「バスからの切断」）。

| 見る所 | 切断している間の動作 |
|---|---|
| 切断した ECU の `send()` | `Ok` を返しますが、フレームはバスに送信されず、`Network` の `send_failures` に数えて捨てます。 |
| 切断した ECU の受信 | ありません。 |
| 切断した ECU の周期送信、タイマー、アドレスの状態 | そのままです。 |
| 切断した ECU のトランスポートプロトコル | 相手の応答が来ないので、タイムアウトで終わります。 |
| ほかの ECU の受信 | 切断した ECU のメッセージが来なくなります（`Monitor` の `Lost` など、受信の途絶で検出します）。 |
| ほかの ECU のアドレス表 | そのままです。 |

捨てたフレームは保持しないので、接続を戻したときにまとめて送信されることはありません。

接続を戻したときの動作は、ネットワークの `NetworkOptions::on_reconnect` で選びます。どちらも規格と照らして確認していないので、模擬する ECU のふるまいに合わせて選んでください。

| `on_reconnect` | 動作 |
|---|---|
| `j1939::OnReconnect::Announce`（既定） | NAME を持つ各ノードが、Request for Address Claimed を全体へ送信し、自分の Address Claimed も送信します。 |
| `j1939::OnReconnect::Nothing` | 何も送信しません（切断されていたことに気づかない ECU を模擬します）。 |

`Announce` では、ほかのノードの応答で、切断している間に同じアドレスを取ったノードとの競合が解けます。ノードは Claim の後の待ちをしません。Address Claim を最初からやり直す（待ちを含む）選択肢は、まだありません。

断片です（`ecu` は `Ecu&`、`kName` は NAME）。`on_reconnect` の 2 つの値は `tests/simulation/test_disconnect.cpp` がテストしています。

```cpp
j1939::NetworkOptions options;
options.on_reconnect = j1939::OnReconnect::Nothing;
auto& node = ecu.j1939("J1939_CAN", options).add_node({.address = 0x00, .name = kName});
```

### 受信だけをする participant

ほかの ECU どうしのやり取りをシナリオで確認するときは、何も送信しない ECU を設定ファイルに追加して、聞いたことを変数に書きます（「外部から信号を読み書きする」）。送信しなければ、バスには何も出ません。

ほかの ECU が受信するもの、送信するもの、送信確認とそれぞれの時刻は、その ECU がいてもいなくても同じで、その ECU のステップ幅にもよりません（仮想バス、SIL Kit の簡易シミュレーション、簡易 CAN Network Simulator でテストしています）。

聞いたことを変数にするのは `vehicle_sil::spy::Spy` です（ヘッダー `vehicle_sil/spy/spy.hpp`、ターゲット `vehicle_sil::spy` をリンクします）。何を見るかは、見張り（watch）のクラスに書いて `Spy` に追加します。

この見本は、`main()` を除いて `tests/spy/test_spy.cpp` にあり、テストしています。`ecu_main` の `main()` は [examples/ecu_main/engine/main.cpp](../../examples/ecu_main/engine/main.cpp) にあります。受信だけをする participant がほかの ECU の通信を変えないことは、`tests/j1939/test_j1939_silkit.cpp` がテストしています。

```cpp
namespace cal {
inline constexpr j1939::Pgn kPgn{0xEF00}; // Proprietary A
}

class BlockWatch {                                   // a watcher: takes Scope& and its own settings; no base class, creates no Listener
public:
    BlockWatch(vehicle_sil::spy::Scope& s, j1939::Address from) {
        s.on_message(cal::kPgn, [this, from](const j1939::Message& m) {   // the Scope keeps the registration (no removal to write)
            if (m.source != from || m.time < latest_) { return; }        // the order of delivery can vary: compare times to find the newest
            latest_ = m.time; ++count_; bytes_ = static_cast<int>(m.data.size());
        });
        s.output("count", &count_); s.output("bytes", &bytes_, {.unit = "bytes"});   // becomes "block.count"
    }
private: vehicle_sil::SimTime latest_{}; std::int64_t count_ = 0; int bytes_ = 0;
};

class CalSpy {
public:
    explicit CalSpy(vehicle_sil::Ecu& ecu) : spy_(ecu, ecu.j1939("J1939_CAN")) {    // no node: sends nothing
        spy_.add<BlockWatch>("block", j1939::Address{0x21});                        // block.count, block.bytes
    }                                                                               // the Spy writes spy.missed and its breakdown
private: vehicle_sil::spy::Spy spy_;
};

int main(int argc, char** argv) { return vehicle_sil::ecu_main<CalSpy>(argc, argv, {.name = "CalSpy", .step = 5ms}); }
```

| 受信のしかた | 使うもの | 見えるもの |
|---|---|---|
| J1939 で受信する | ノードを追加しない `Network`（`ecu.j1939(network)`）と、それに付けた `vehicle_sil::spy::Spy`（または `j1939::Listener` を直に） | ほかの participant が送信したメッセージとフレームが、宛先が誰でも時刻つきで見えます。 |
| 生の CAN で受信する | `ecu.can(network)` の `on_frame` | 全部のフレーム（11 ビットのフレーム、TP.CM と TP.DT を含む）を受信します。 |

- `Spy(ecu, network)` は、その `Network` に `j1939::Listener`（ヘッダー `vehicle_sil/j1939/listener.hpp`）を 1 つ作ります。ノードを追加しない `Network` は、Address Claim も、Request への応答も、ACK と NACK もしません。`Listener` も聞くだけで、何も送信しません。
- `add<W>(名前, 引数...)` は見張り `W(scope, 引数...)` を作り、`Spy` が持ちます。見張りは `spy::Scope&` を最初に取るコンストラクターを持つクラスで、何も継ぎません。見張りのハンドラーは、追加した順に呼ばれます（見張りをまたいでも）。`Scope` の口:
  - `on_message(pgn, handler)`、`on_any_message`、`on_transfer`、`on_frame`: `Listener` の同じ名前の口にハンドラーを追加します（渡るものは下の「J1939 で聞く」）。登録は `Scope` が持ち、`Spy` が見張りを壊す前に外すので、見張りは `ListenerSubscription` を持ちません。
  - `output(項目, …)`、`output_enum(項目, …)`: 変数 `<名前>.<項目>` を書きます。形は `ecu.output()` と同じです。
  - `on_step(fn)`: ECU のステップごとに、そのステップの時刻で `fn` を呼びます。そのステップのハンドラーの後、変数の標本の前です。時間が過ぎたことで決まること（来なくなった回数を数えるなど）に使います。結果はステップ幅の粒で変わります。ハンドラーと同じく、見張りのメンバーを書き換えるだけにします。全部の見張りの関数は、`Spy` を作った時点で登録される 1 つの `Ecu::on_step` の関数から呼ばれます。アプリが `Spy` を作る前に足した `ecu.on_step()` の関数の後、作った後に足した関数（`Spy` をメンバーに持つクラスのコンストラクターの本体で足した関数など）の前です。`fn` が投げると、その後ろに登録した関数はそのステップでは呼ばれず、`ecu.on_step()` の関数が投げたときと同じにステップが止まります。

    ```cpp
    s.on_step([this](vehicle_sil::SimTime now) {           // count once when nothing came for 100 ms
        if (count_ > 0 && !quiet_ && now - latest_ > 100ms) { quiet_ = true; ++silences_; }
    });                                                     // the handler sets quiet_ = false again when a message arrives
    ```

  - `now()`: その ECU の最後のステップの時刻です。変数の getter の中では標本を取るステップの時刻なので、「最後に聞いてから何 ms」は `now()` と最後の `Message::time` の差で出せます（結果はステップ幅の粒で変わります）。ハンドラーの中では 1 つ前のステップの時刻です。ハンドラーでは `Message::time` を使います。
  - 見張りの追加も、ハンドラーと変数の登録も、ECU のクラスのコンストラクターの中でします。始まった後は `std::logic_error` です（「... after the run started: add the watchers in the ECU class's constructor」）。名前の誤りは `std::invalid_argument` です。同じ名前の 2 回目、空か `.` で始まる・終わる名前、聞きこぼしの前置き（下）と同じ名前が誤りで、文に直し方が出ます。
- 見張りのコンストラクターの中から、受け取った `Spy&` で別の見張りを `add()` できます（見張りを組み合わせた見張り）。追加した見張りは、組み合わせた見張りより後に壊れます（`Spy` は `add()` が返った順の逆に壊します）。コンストラクターが投げると、それまでに登録したハンドラーと `on_step` の関数を外し、追加した見張りも壊します。
- 聞きこぼしの変数: `Spy` は、`Listener` と `Network` の数を、次の変数に書きます（増えるだけの整数）。シナリオで `spy.missed` が 0 のままなのを確認すると、「来なかった」と「聞きこぼした」を見分けられます。

  | 変数 | 数えるもの |
  |---|---|
  | `spy.missed` | 下の 8 つの和。0 のままなら、`Network` は受けたフレームを 1 つも落とさず、`Network` と `Listener` が追い始めた BAM と接続は、途中のパケットの欠け（欠けた後に、後ろのパケットが来た場合）・重なり・順の違いや空きのなさで失われておらず、渡したメッセージは全部受け手と同じバイト列です。どの転送も最後まで来た、とは言えません（見えないものは下） |
  | `spy.no_session` | 空きがなくて追わなかった接続（同時に追える数は `ListenerOptions::sniffer.sessions`、既定 32） |
  | `spy.incomplete` | パケットを見逃した接続 |
  | `spy.replaced` | 終わる前に告知し直された接続 |
  | `spy.lost` | 同じ participant のノードがそのアドレスを取ったので、追うのをやめた接続 |
  | `spy.conflicting_packets` | 同じ番号で中身の違うパケット（下の「受け手と違うかもしれないメッセージ」） |
  | `spy.receive_overflows` | `Network` が受け取れなかったフレーム（`NetworkOptions::receive_capacity`） |
  | `spy.transport_rejected` | `Network` が組み立てを始めなかった BAM（形の正しくない告知、空きのセッションなし）と、同じ participant のノードが断った RTS（別の PGN で話し中、大きすぎ、形の誤り、空きなし） |
  | `spy.transport_bam_dropped` | `Network` が組み立ての途中で捨てた BAM（パケットの欠け、重なり、順の違い） |

  - 見えないもの: スパイの ECU をバスから外した間（`ecu.connected` が false）に来たフレームは、`Network` の手前で捨てられ、どこにも数えられません。`Network::statistics()` の `frames_ignored`（11 ビットのフレームなど。11 ビットのフレームも流れるバスではいつも増えるため）と、告知を聞く前に始まった接続（`sniffer_statistics()` の `orphan_data`）も入れません。
  - 最後まで来なかった転送も見えません。末尾のパケットが来なかった BAM と接続は時間切れになり、送り手が止めたのか、パケットを聞きこぼしたのかは見分けられません。BAM の時間切れは `Network::statistics()` の `transport_timeouts` に数えますが、この participant のノードの時間切れも数えるので、和には入れません。ほかの ECU どうしの接続は `sniffer_statistics()` の `timed_out` に数えます。最後のパケットが来る前に、同じ送り手の次の告知で置き換わった BAM は、どこにも数えません。
  - 前置きは `SpyOptions::statistics_as`（既定 `"spy"`）で変えられます。空にすると書きません。
- 寿命: 書いた変数は取り消せないので、変数を書く `Spy` は ECU のクラスのメンバーにして、ECU と同じだけ生かします（見本の形。`ecu_main` と `Simulation::add()` が作るクラスは、ECU のネットワークと変数より後に壊れます）。変数を書いた `Spy` を ECU より先に壊すと、その ECU の次のステップが、変数を読む前に `std::logic_error`（「vehicle_sil::spy::Spy was destroyed before its ECU, but its variables (...) are still read: make the Spy a member of the ECU class ...」）で止まります。`Spy` は壊れるとき、見張りごとに（`add()` が後に返ったものから）ハンドラーと `on_step` の関数を外してから見張りを壊し、最後に自分で作った `Listener` を壊します。
- 1 つの `Network` に `Listener` は 1 つです（2 つ目は `std::logic_error`）。`Listener` をほかのコードと分けるときは、自分で作った `Listener` を `Spy(ecu, network, listener)` に渡します。その `Listener` に `Spy` を 2 つ置くときは、2 つ目の `SpyOptions::statistics_as` を `""` にします。
- 見張りをテストする: `LocalBench` の ECU に見張りを追加したクラスを置き、フレームを入れて回し、変数を読みます（`tests/spy` の見本のテストがこの形です）。見張りに参照で渡すもの（記録の `std::vector` など）は、`LocalBench` より前に宣言します。見張りは ECU と一緒に、`LocalBench` が壊れるときに壊れるためです。

  ```cpp
  auto& ecu = bench.add_ecu("CalSpy", 5ms); ecu.emplace<CalSpy>(ecu);
  bench.inject("J1939_CAN", t, frame); bench.run_until(end);
  // values: find the position i of the name in ecu.agent().schema(), and read ecu.agent().latest().values[i]
  ```

- `Listener` を直に使う: `Spy` を使わずに、`Network` に `j1939::Listener` を作ってハンドラーを追加し、`ecu.output()` で書くこともできます。見張りのコードは `Listener&` を受け取ってハンドラーを追加し、`Listener` を自分で作りません。追加したハンドラーは、外すか `Listener` を壊すまで呼ばれるので、`Listener` より先に壊れうる見張りは、登録の戻り値（`Listener::HandlerId`）を `j1939::ListenerSubscription` のメンバーに持ちます（ノードのハンドラーの `j1939::Subscription` と同じ形）。`ListenerSubscription` は、壊れるか `reset()` でハンドラーを外します。ステップの中でも、そのハンドラー自身の中からでも外せ、`Listener` が先に壊れた後に壊しても安全です。聞きこぼしの数は `Listener::sniffer_statistics()` と `Network::statistics()` にあります。
- J1939 で聞く（`Listener` が渡すもの）:
  - `on_message(pgn, handler)` と `on_any_message(handler)`: ほかの participant が送信したメッセージが、宛先が誰でも渡ります。単一フレーム、BAM で送信されたもの、2 つの ECU の間の接続（RTS/CTS）で送信されたものです。ハンドラーが受け取る型はノードのハンドラーと同じ `j1939::Message` なので、読む関数をノードと共用できます。同じ PGN に複数のハンドラーを追加できます（登録の順に呼ばれます）。
  - `on_transfer(handler)`: ほかの ECU どうしの接続がどう終わったか（`SniffedTransfer`。完了のほか、Abort、時間切れ、聞きこぼし）。
  - `on_frame(handler)`: フレーム 1 つずつ（TP.CM と TP.DT、ほかのアドレス宛てを含む）。自分で組み立てるときに使います。
- 聞こえないもの:
  - 同じ `Network` のノードが送信したもの。`Listener` が渡すのは「ほかの participant が言ったこと」です。自分のノードの送信は、送信した側の結果（`Node::on_transfer_end`、`Requester` の結果）で数えます。`ListenerOptions::hear_local` を入れると、同じ `Network` のノードどうしで折り返したフレームとメッセージも渡りますが（`ReceivedFrame::local` が立ちます）、折り返しはノードが 2 つ以上のときにしか起きないので、ノードが 1 つの `Network` では何も増えません。`hear_local` を入れても、ノードからほかの participant 宛ての接続（RTS/CTS）はフレームだけが渡り、メッセージは渡りません。
  - `Network` が無視するフレーム（`frames_ignored`。11 ビットのフレーム、送り元が 255 のフレーム、送り元が 254 で Address Claimed と Request 以外のフレーム、形の正しくないフレーム）と、`Network` が受け取らなかったフレーム（`receive_overflows`）。11 ビットを見るときは生の CAN で受信します。
  - 組み立てなかったもの、追えなかった接続: 上の聞きこぼしの変数の内訳です。
- 受け手と違うかもしれないメッセージ: ほかの ECU どうしの接続のメッセージが受け手のノードのものと同じバイト列なのは、その転送の `conflicting_packets`（同じ番号で中身の違うパケットを見た数）が 0 のときです。0 でなければ、受け手がどちらを取ったかはバスからは分かりません（`Listener` は先に見たほうを渡します）。`Message` にはその印がありません。見分け方は 2 つです。
  - `spy.conflicting_packets`（`spy.missed` に含まれます）が 0 のままなら、渡ったメッセージは全部受け手と同じです。
  - `on_message` のハンドラーで値を使い、同じ転送について直後に呼ばれる `on_transfer` のハンドラー（PGN、送信元、宛先が同じもの）が `Completed` で `SniffedTransfer::conflicting_packets` が 0 でないのを見たら、いま取った値を捨てます。ほかのメッセージ（単一フレーム、BAM で送られたもの、この `Network` のノード宛てのもの）には `on_transfer` が来ず、受け手と食い違うこともありません。
- 自分のノードを持つ participant でも使えます。ノード宛ての接続とノードが送信する接続は `Network` が扱い、ノード宛てのメッセージは 1 回だけ渡ります（`on_transfer` には出ません）。
- ハンドラーは読むだけです。ハンドラーの中から送信やハンドラーの登録をすると `std::logic_error` で、その participant は失敗して止まります（例外を握りつぶしても止まります）。ハンドラーでは変数を更新し、送信は `ecu.on_step` からにします。
- 時刻と順: 接続で送信されたメッセージの `Message::time` は、受け手がメッセージを受け取った時点（最後のパケット）の時刻で、受け手のノードが見る時刻と同じです。ただしハンドラーが呼ばれるのは、受け手が「受け取った」と返した後です。そのため、後の時刻のメッセージより後に、前の時刻のメッセージが渡ることがあります。最後の値や周期を出すときは、呼ばれた順ではなく `time` を比べます。フレームから決まること（メッセージ、Abort）は、聞く participant のステップ幅によりません。時間の経過から決まること（`TimedOut` など）は、`time` はステップ幅によりませんが、ハンドラーが呼ばれるステップは変わるので、これも `time` で比べます。
- 接続を追う決まりは、このリポジトリのエンジンの受信側に合わせたもので、規格とは照らしていません。ほかのスタックで違いうる値（時間切れ、完了とする時点など）は `ListenerOptions::sniffer`（`Spy` では `SpyOptions::listener.sniffer`）で変えられます。時間切れは、決めなければ聞く側の `Network` の `NetworkOptions::timing` に従います。聞く相手の ECU がその `Network` と違う時間を使うときは決めます（[internals/j1939.md](internals/j1939.md) の「ほかの participant が言ったことを聞く（`Listener`）」と「ほかの ECU の転送を追う（`TransferSniffer`）」）。実機のブリッジがいるネットワークでは、時刻も順も正確ではありません（[bridges.md](bridges.md) の「物理 CAN と仮想時間」）。
- 記録やテストのために `Network` のイベント（送信、アドレスの変化を含む）を見るなら `j1939::Observer` です（[internals/j1939.md](internals/j1939.md) の「観測（`Observer`）」）。`Listener` と同時に付けられます。
- 生の CAN は、`send()` を呼ばなければ何も送信しません。独自の組み立てをするときに、接続だけを追わせたいなら `j1939::TransferSniffer` にフレームを渡します。

確認していないこと: Vector の Network Simulator の上で、送信しない CAN コントローラーがバスの動き（ACK を返す相手の数など）を変えないかどうか。

## 外部から信号を読み書きする（UI、テスト）

UI やテストから ECU の値を読み書きするときは、ECU に変数を宣言し、Control API を使います。Control API が扱うのは宣言した変数と時間の操作で、J1939 のフレームは扱いません。

| 値 | 宣言 |
|---|---|
| 受信した信号など、外部に見せる値 | `output` |
| 外部から決める値 | `input` |

[examples/ecu_main/engine/main.cpp](../../examples/ecu_main/engine/main.cpp) の `Engine` のコンストラクターの一部です。

```cpp
ecu.input("engine.throttle", &throttle_,
          {.unit = "%", .min = 0.0, .max = 100.0, .description = "accelerator pedal position"});
ecu.output("engine.speed", &rpm_, {.unit = "rpm"});   // 送信する PG の元にする値
```

宣言のルール、同じプロセスからの読み書き、Control Server への接続のしかたは [control-api.md](control-api.md) にあります。

## データの形（Codec）

コンポーネントとエンジンはバイト列だけを扱います（`PeriodicSender` の `Encoder` は「バッファに書いて長さを返す」関数、受信は `Message::data` の `std::span`）。信号の配置とスケーリングは、YAML から生成した型付きメッセージ（[j1939-codec.md](j1939-codec.md)）が受け持ち、`codec_node.hpp` の関数が両者をつなぎます。

独自プロトコルのコンポーネントも、メッセージの形を同じ YAML で定義できます（Proprietary A のように 1 つの PGN に複数のメッセージを載せるときは `selector`）。YAML で定義して使う例は [examples/typed_messages](../../examples/typed_messages) です。

## 関連するドキュメント

| 読むもの | 内容 |
|---|---|
| README の「[ECU の書き方](../../README.ja.md#ecu-の書き方)」 | この文書の前に読む、ECU の最小の形 |
| [j1939-codec.md](j1939-codec.md) | 信号を値として読み書きする型付きメッセージ |
| [ecu-executable.md](ecu-executable.md) | ECU を実行ファイルにする方法 |
| [internals/j1939.md](internals/j1939.md) | API の保証とルールの詳細 |
