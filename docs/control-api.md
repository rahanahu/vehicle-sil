# Control API の使い方

実行中のシミュレーションを外から操作・監視する人（Python、CLI、試験）と、ECU に変数を宣言する人向けです。前に読む文書はありません。ECU の実行ファイル（`ecu_main`）なら、変数の名前と型は `--describe` の `variables` で調べられます（[ecu-executable.md](ecu-executable.md) の「JSON の形式」）。次に読むのは、契約の詳細が要るときだけ、下の段落にある architecture.md の節です。

実行中のシミュレーションを、外から操作・監視します。ECU が外から読み書きする変数を宣言し、その Control Agent を Control Server につなぎます。クライアント（Python、CLI、将来の GUI）は Control Server の gRPC の API（`proto/vehicle_sil/control/v1/control.proto`）を使います。契約の詳細（反映の時刻、順序、上限、スレッド）は [architecture.md](architecture.md) の「外部操作（Control API）と GUI」にあります。

```
クライアント（Python、CLI）──gRPC（TLS、トークン）──┐
                                                  ▼
                      vehicle_sil_control_server（Hub、時間ゲート）
                         ▲ gRPC（Agent が接続する）     │ SIL Kit（開始、停止、時刻同期）
          Peer + Agent ──┘                              ▼
          （どの PC でもよい）           SIL Kit のレジストリと CAN/LIN（データプレーン）
```

## ビルド

```sh
cmake --preset linux-gcc-release -DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON
cmake --build --preset linux-gcc-release
```

gRPC が見つからなければ FetchContent で取り込みます（初回は取得とビルドに数分かかります）。Windows（MSYS2 UCRT64）は `pacman -S mingw-w64-ucrt-x86_64-grpc` で入れておきます。詳しくは [dependencies.md](dependencies.md)。

## ECU の変数

ECU のクラス（`Ecu&` を受け取るクラス）が、コンストラクターで変数を宣言します。宣言は ECU の Control Agent（`Ecu::agent()`。ECU に 1 つ）が持ちます。SIL Kit の `Peer` でも、ローカルの `Simulation()` と `LocalBench` でも、同じコードが同じように動きます。

```cpp
class Engine {
public:
    explicit Engine(vehicle_sil::Ecu& ecu) {
        ecu.input("engine.throttle", &throttle_, {.unit = "%", .min = 0.0, .max = 100.0});
        ecu.output("engine.speed", &rpm_, {.unit = "rpm"});
        ecu.every(10ms, [this](vehicle_sil::SimTime) { rpm_ = 800.0 + 50.0 * throttle_; });
    }
private:
    double throttle_ = 0.0;
    double rpm_ = 800.0;
};
```

- 宣言: `input(name, &value, options)`、`input(name, get, set, options)`（setter が値の保存以外もするとき）、`input_enum(name, &value, items, options)`、`output(name, &value, options)`、`output(name, get, options)`、`output_enum(name, get, items, options)`。型は bool、整数、実数、列挙で、単位、範囲、刻み、説明を付けられます（`VariableOptions`。`parameter` は最初のステップの前だけ書ける変数）。値は ECU のステップのスレッドで読み書きするので、モデルにロックは要りません。
- 宣言は実行の前（ECU の組み立ての中）だけです。開始後は `std::logic_error`、同じ ECU での同じ名前は `std::invalid_argument` です（どちらも文言に ECU の名前があります）。
- ポインターと関数は、ECU が持つもの（`Simulation::add()` が作るクラス、`emplace()` したオブジェクト）を指すようにします。ECU は Agent を、ネットワークの後、`emplace()` したオブジェクトの前に破棄します。
- `set_agent_options()` で、読み出しの周期（`sample_period`、既定 10 ms、0 で毎ステップ）とキューの大きさを選べます。最初の `input()`、`output()`、`agent()` より前だけです。不正な値はその場で `std::invalid_argument` です（文言に ECU の名前）。
- 変数を宣言しない ECU には Agent がなく、ステップの処理も増えません。
- 外からの書き込みは、指定した仮想時刻以後の最初のステップで、アプリの処理の前に反映されます（そのステップの J1939 の受信処理や部品のタイマーは反映より前に動くので、それらが新しい値を使うのは次のステップから）。値は、アプリと LIN の処理の後に読み出します。SIL Kit とローカルで同じ時点です。
- 同じプロセスからは、`ecu.agent().submit()` で書き込み、`latest()` で読めます。`Simulation::add<App>()` で足した ECU は、`sim.ecu(name).agent()` で引けます（例: `examples/vehicle`）。

## Control Server につなぐ

```cpp
vehicle_sil::Peer peer(options);            // Coordinated、仮想時間
peer.emplace<Engine>(peer);                 // ECU が変数を宣言する
vehicle_sil::control_grpc::AgentClient link(peer.agent(), {   // ECU の後、start() の前に作る
    .server = "ctrl-pc:8510", .participant = options.participant,
    .status = [&] { return vehicle_sil::control::PeerStatus{peer.failed(), peer.error()}; },
    .token = agent_token, .root_certificates_pem = server_certificate,
    .on_replaced = [&](const std::string& why) {  // 同じ名前の別のプロセスに置き換えられた
        std::fprintf(stderr, "%s\n", why.c_str());
        peer.request_stop("replaced at the control server");
    }});
peer.start();
while (!peer.wait(std::chrono::minutes(1))) {} // 止めている間も含めて、終わるまで待つ
peer.stop();                                // ステップが終わり、残っている操作を Cancelled にする
link.stop();                                // その結果を送ってから閉じる
```

- `AgentClient` は、作るときに Agent の変数の一覧（スキーマ）を確定させます。ECU が変数を宣言した後、`start()` の前に作ると、シミュレーションが始まる前に Server につながります。確定した後の宣言は `std::logic_error` です。
- `AgentClient` は ECU（`Peer`、`Simulation`、`LocalBench`）より後に宣言し、先に破棄します。ECU が Agent を破棄するためです。ECU に `emplace()` しません（`emplace()` したオブジェクトは Agent より後に壊れます）。`AgentClient` が残ったまま ECU が Agent を破棄すると、ECU の名前と直し方を標準エラーに出してプロセスを止めます（`std::terminate()`）。
- Peer は、スキーマを確定するときに、ECU の変数の後に監視の出力を足します（`peer.steps`、`peer.speed`、J1939 のネットワークとノードの状態と統計。一覧は [architecture.md](architecture.md) の「Control Agent（`vehicle_sil::control`）」の「Peer の監視」の項）。対象は、そのときにある J1939 のネットワークとノードです。その後に `j1939()` でネットワークを足すと `std::logic_error` になるので、ネットワークは `AgentClient` を作る前（最初の `submit()` の前）に足します。要らなければ `PeerOptions::monitor = false` にします。
- 終わり: `peer.stop()` は、ステップが走らなくなった後に、残っている操作を `Cancelled` にします（破棄でも同じ）。その後で `link.stop()` を呼ぶと、その結果も Server に届きます。`wait()` が時間切れで戻っただけでは、ステップが続いています。
- `on_replaced` は、同じ participant 名の別のプロセスに接続を置き換えられて、`AgentClient` がつなぎ直しをやめたときに 1 回だけ、`AgentClient` のスレッドで呼ばれます。引数は `last_error()` と同じ理由です。指定しなければ、理由を標準エラーに 1 行出します。この中で `link.stop()` を呼んだり `link` を破棄したりしません。`examples/control/control_peer.cpp` は、理由を標準エラーに出し、`peer.request_stop()` で止めて、終了コード 1 で終わります。
- `Simulation` の ECU も同じです（`AgentClient link(sim.ecu("Engine").agent(), ...)`）。SIL Kit の `Simulation` は、実行の終わりに ECU を止めるので、残っている操作はそこで `Cancelled` になります。
- ローカル（`Simulation()`、`LocalBench`）の ECU も、`AgentClient` で既存の Control Server につないで値を読み書きできます。ただし、時間の操作（一時停止、ステップ実行）、`Simulation` から Control Server を起動する口、監視の出力はありません。待っている書き込みの結果は届きません（`AgentClient` は ECU より先に破棄するため）。
- 例: `examples/control/control_peer.cpp`（プロセスごとに 1 つの Peer）、`examples/control/control_demo.cpp`（1 つのプロセスにすべて）。どちらも ECU のクラスは `examples/control/control_apps.hpp` にあります。

## Control Server を起動する

```sh
vehicle_sil_control_server --listen 0.0.0.0:8510 \
    --registry-uri silkit://localhost:8500 --required Engine,Body --start-paused 1 \
    --credentials credentials.txt --tls-cert server.crt --tls-key server.key --audit-log audit.log
```

- `--required` を付けると、Server が SimulationController を兼ね、必要な participant がそろったところでシミュレーションを始めます。必要な participant が Error で抜ける（プロセスの異常終了、ステップの失敗、切断）とすぐに、ほかの状態で抜けて 5 秒たってもシミュレーションが終わらなければ、Server はシミュレーションを止めて終わります（終了コード 1。判定は `vehicle_sil_sim_controller` と同じ）。始まる前に `stop` を呼ぶと、始まった直後に止まります（Peer によっては、最初のステップの前に終わりを見て、開始の失敗として終わります）。`--gate-step-ms`（既定 10）は止められる時刻の刻みです。`--start-paused 1` で T = 0 で止めて始めます。
- 認証情報のファイルは 1 行に 1 つで、`<role> <name> <token> [participant,...]` です（role は viewer、operator、admin、agent。トークンは 16 文字以上の秘密の文字列）。agent の行には、接続してよい participant 名を必ず書きます（カンマで区切る）。その名前でしか接続できません。どの名前でもよいときは `*` を書きます。participant 名のない agent の行は、読み込みの誤りです。

```text
# role     name     token                     participants
operator   tester   0f3c9a...（長い乱数）
admin      lead     7b21e4...
agent      engine   c4d8f0...                 Engine
agent      body     91aa3b...                 Body
agent      bench    5e07d2...                 *      # どの名前でもよい（試験台など）
```

- ループバック以外で待ち受けるときは TLS と認証情報が必須です（試すだけなら `--allow-insecure 1`）。

## Python

```sh
pip install grpcio grpcio-tools protobuf
python -m vehicle_sil_control.generate          # スタブを python/vehicle_sil_control/_generated に作る
export PYTHONPATH=$PWD/python
```

```python
from vehicle_sil_control import Control, ms

with Control("ctrl-pc:8510", token=token, root_certificates=open("server.crt", "rb").read()) as c:
    print([p.participant for p in c.peers()])
    c.sim.wait_paused()
    seq = c.write({"Engine": {"engine.throttle": 50.0}, "Body": {"cabin.lamp": 2}}, at=ms(100))
    c.wait(seq, until="queued")         # 全員が受け取ってから進める（at が止まっている時刻より後なので、遅すぎる書き込みが起きない）
    c.sim.run_until(ms(500)); c.sim.wait_paused()
    print(c.wait(seq).target("Engine").applied_at)   # 100 ms（ns で）
    print(c.values("Engine").values["engine.speed"])
    for update in c.watch(peers=["Body"]):           # Peer、Values、EventInfo、Operation
        print(update)
        break
```

時刻は仮想時間のナノ秒です（`ms()`、`s()` で作る）。呼び出しの失敗は `ControlError`（`code` は gRPC の状態コード）で、Agent が断った書き込みは例外ではなく、操作の結果（`invalid` など）になります。

- 止めてから書くときは、`at` を止まっている時刻（`c.sim.time().now`）より後にします（たとえば 1 ns 後）。止まっている時刻のステップは、すでに走った Peer があるので、その時刻か `at` なしの書き込みは `late` になるか、反映のステップが実行ごとに変わることがあります。
- `write()` は自動では送り直しません。応答を受け取る前に接続が切れた書き込みは、受け付けられているかもしれません（同じ書き込みを送り直すと、別の操作になります）。
- 複数のクライアントが同じ変数に同じ時刻で書くと、残る値は Server に届いた順で決まります。
- 時間の状態の購読（`watch(time=True)`）は約 100 ms ごとの最新で、短い一時停止は現れないことがあります。止まるのを待つには `c.sim.wait_paused()` を使います。
- `peers()` と `watch()` は、同じ Peer を同じ内容で返します。`has_agent` は Agent がつながったことがあるか（既定は偽）、`simulation_state` は SIL Kit の状態です。`TimeState.running` が偽のとき、`ended` が真なら終わっていて、偽ならまだ始まっていません。
- シミュレーションの状態の購読（`watch(simulation=True)`）は `SimulationState` を返します。必須の participant ごとの SIL Kit の状態（`participants`）、失敗した participant と理由（`failed_participant`、`failure_reason`）、終わったか（`ended`）が入ります。Server が状態を読めなかったときは、前の値に理由（`error`）が付きます。クライアントが遅くて Server が途中の状態を持ちきれなかったときは、捨てた数が gRPC の `Update.dropped` に入ります（Python の `watch()` はこの数を返しません。失敗と終わりは後の状態に残るので失われません）。`peers()` も、読めなかったときは、呼ぶたびに `ControlWarning` で知らせます（同じ箇所からの 2 回目以降も出します。出すか、例外にするか、無視するかは利用者の警告フィルターのとおりで、ライブラリはフィルターを変えません）。

## CLI

```sh
export VEHICLE_SIL_CONTROL_SERVER=ctrl-pc:8510 VEHICLE_SIL_CONTROL_TOKEN=... VEHICLE_SIL_CONTROL_ROOT_CERT=server.crt
python -m vehicle_sil_control peers
python -m vehicle_sil_control show Engine
python -m vehicle_sil_control set Engine engine.throttle=50 --at-ms 100   # 既定は全員が積むまで待つ（--wait done で反映まで）
python -m vehicle_sil_control run-until 500
python -m vehicle_sil_control step 10
python -m vehicle_sil_control speed 1        # 実時間。max でできるだけ速く
python -m vehicle_sil_control watch --peer Body --count 10
python -m vehicle_sil_control stop
```

## 複数の PC

- SIL Kit のレジストリと Control Server を 1 台に置き、各 PC の Peer は両方へ外向きにつなぎます（Peer 側で待ち受けるポートは要りません。SIL Kit は participant どうしが直接つながらないとき、レジストリを中継に使います）。
- 発見: Server（`--required` のとき）は、必須の participant を SIL Kit のレジストリを通して見ているので、どの PC の Peer も、Agent がつながる前から一覧に出ます（`peers` の `has_agent` が偽、`simulation_state` に SIL Kit の状態）。Agent がつながると、その変数と値が加わります。
- Peer のプロセスが落ちて起動し直すと、Server は別の incarnation として扱い、前の未完了の操作を Unknown（反映されたかわからない）にします。ただし `--required` の Peer が落ちると、シミュレーションと Server が終わります（Coordinated の participant は、始まった後のシミュレーションに戻れない）。起動し直せるのは、必須でない Peer か、`--required` なしの Server のときです。同じ participant 名の Agent が別のプロセスからつながると、新しい接続が勝ちます。前の Agent はつなぎ直しをやめ、`replaced()` が真になり、`last_error()` に理由（participant 名、新しい接続の incarnation と接続元、同じ名前のプロセスが 2 つ動いていないか確かめること）が入り、`on_replaced` が呼ばれます（指定しなければ標準エラーに 1 行）。Server は置き換えを監査に残し、購読には新しい接続の Peer の出来事（Warning。Python では `level` が `"warning"` の `EventInfo`）として置き換えのたびに流すので、まとめる人が画面で気づけます。回線が切れただけなら、Agent はつなぎ直し、Agent が受け取っていた操作はそのまま続きます（受け取ったと知らされる前に切れた操作は Unknown）。黙って切れた接続は keepalive で数十秒以内に見つけます。
- Server を起動し直すと、前の Server の操作の結果は問い合わせられません（`NOT_FOUND`）。Agent がまだ持っていた時刻つきの書き込みは、そのまま反映されます。
- 試験: `tests/control_server/multi_process.py` は、レジストリ、Server、2 つの Peer をプロセスに分けて動かし、Python から操作します。`--remote-body` で Body を別の PC で動かせます。

## 制限

- 時間の操作（一時停止、ステップ実行、速度）は、Server が `--required` でシミュレーションを調停しているときだけです。止まるのは仮想時間で同期している Peer だけで、実時間の Peer は走り続けます。
- リセット、Peer の起動（Launcher）、CAN/LIN のチャネルの選択はありません。
- 値の購読は最新値です（読む前に来た新しい値で置き換えます）。すべての値の記録は、SIL Kit のトレースか `TapPort` を使います。
- 1 回の `write()` の書き込みは 4096 個まで、同時に待つ呼び出しと購読は合わせて 256 本まで（Server の既定）です。
