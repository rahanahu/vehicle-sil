# Control API の使い方

実行中のシミュレーションを外から操作・監視する人（Python、CLI、試験）と、ECU に変数を宣言する人向けです。前に読む文書はありません。ECU の実行ファイル（`ecu_main`）なら、変数の名前と型は `--describe` の `variables` で調べられます（[ecu-executable.md](ecu-executable.md) の「JSON の形式」）。次に読むのは、契約の詳細が要るときだけ、下の段落にある [internals/control.md](internals/control.md) です。

実行中のシミュレーションを、外から操作・監視します。ECU が外から読み書きする変数を宣言し、クライアント（Python、CLI、GUI）が Control Server の gRPC の API（`proto/vehicle_sil/control/v1/control.proto`）で、その変数と仮想時間を操作します。契約の詳細（反映の時刻、順序、上限、スレッド）は [internals/control.md](internals/control.md) にあります。

ふつうは、シミュレーションを動かす `vehicle_sil_launch run` に `--control` を付けて、ランチャーの中の Control Server につなぎます（下の「ランチャーにつなぐ」）。`ecu_main` で作った ECU は、変数を宣言していれば、何も書かなくてもランチャーから読み書きできます。ECU の実行ファイルは gRPC を持ちません（ECU とランチャーの間は SIL Kit です）。

```mermaid
flowchart LR
    client["client (Python, CLI, GUI)"] -- gRPC --> launch["vehicle_sil_launch run --control<br>(Control Server, hub, time gate)"]
    launch -- SIL Kit --> e1["ECU (ecu_main)"]
    launch -- SIL Kit --> e2["ECU (ecu_main)"]
```

ECU をランチャーに起動させずに、自分で起動する（デバッガーから動かす、別の端末で起動する）ときも、構成ファイルの participant に `manual: true` を付けるか `run --manual <name>` で、同じランチャーにつなぎます（[simulation-config.md](simulation-config.md) の「participant を自分で起動する」）。

## ビルド

```sh
cmake --preset linux-gcc-release -DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON
cmake --build --preset linux-gcc-release
```

gRPC が見つからなければ FetchContent で取り込みます（初回は取得とビルドに数分かかります）。Control Server の試験までまとめて回すときは、プリセット `linux-gcc-control`（GCC の Debug でこの設定）を `scripts/verify.py linux-gcc-control` で使えます（[build.md](build.md) の「Linux」）。Windows（MSYS2 UCRT64）は `pacman -S mingw-w64-ucrt-x86_64-grpc` で入れておきます。詳しくは [dependencies.md](dependencies.md)。このビルドの `vehicle_sil_launch` は `--control` を持ち、`vehicle_sil_launch version --json` の `features` に `control-server` があります。持たないビルドで `--control` を付けると、引数の誤り（終了コード 4）で、作り直し方を標準エラーに書きます。

## ランチャーにつなぐ

```sh
vehicle_sil_launch run --events --stdin-commands --start-paused --control 0 simulation.yaml
```

- `--control <address>` で、ランチャーが Control Server を立てます。`<address>` は、ポート番号だけ（127.0.0.1 で待ち受けます。0 なら空いているポート）、`<host>:<port>`、`unix:<path>` のどれかです。
- 実際のアドレス（ポート 0 なら選ばれたポート）は、事象 `control_server` の `address` と、人向けの出力の `[vehicle_sil_launch] control server at <address>` の行で知らせます。照合が通った後、registry より前に立てます（照合で失敗した実行では立てません）。立てられなければ（ポートが使われているなど）、何も起動せずに設定の誤り（`setup`、終了コード 3）です。
- サーバーが扱うのは、そのランチャーの実行だけです。ECU の変数（Peer の一覧、値、書き込み、操作の結果、購読）と、仮想時間（一時停止、再開、ステップ、指定時刻まで、速さ、停止）です。Peer の一覧には、構成ファイルの必須の participant も、Agent がなくても出ます。
- 実行が終わると（ふつうの終わり、止める要求、`--timeout-ms`、participant の失敗、どれでも）、ランチャーは participant を終わらせてから、結果を書く前にサーバーを閉じます。待っている呼び出し（`Watch`、`WaitOperation`、`WaitPaused`）は、そこで終わります（`Watch` はストリームの終わり、`WaitPaused` は `FAILED_PRECONDITION` か `CANCELLED`）。ランチャーはクライアントを待たないので、クライアントがあっても終わるのは遅れません。シミュレーションが終わってからサーバーを閉じるまでの間は、終わった時点の値を読めます。
- シナリオの実行（`--scenario`）では、クライアントは見るだけです。読む・待つ呼び出しと `Stop` はいつもどおりで、書く呼び出し（`Write`、`ForgetPeer`）と時刻を動かす呼び出し（`Pause`、`Resume`、`Step`、`RunUntil`、`SetSpeed`）は `FAILED_PRECONDITION` で断ります。理由は trailing metadata の `vehicle-sil-reason`（値は `scenario-running`）で見分けます。表と約束は [scenario.md](scenario.md) の「実行中に変数を見る」。
- 例: [examples/launch_control](../examples/launch_control)（Python から起動してつなぎ、始まる前に書いて、読み返して確かめる `write_before_start.py`。動いている途中で止め、時刻を指定して書き、`snapshot()` で時刻と値をまとめて読んで確かめ、速さを変えて最後まで動かす `pause_and_step.py`）。

### 始まる前に書く（`--start-paused`）

`--control` と `--start-paused` を一緒に付けると、全員がそろった後、どの participant も時刻 0 のステップを始める前で止まり、`resume`、`step`、`run_until` のどれかを待ちます（標準入力からでも、クライアントからでもかまいません）。この間に書いた値は、各 ECU の時刻 0 のステップで反映されます（Parameter も書けます）。

```python
from vehicle_sil_control import Control, Launch, ms

with Launch("vehicle_sil_launch", "simulation.yaml", start_paused=True) as launch, Control(launch.address) as c:
    c.sim.wait_paused()                               # 0 で止まっている（time().running は偽: まだ始まっていない）
    seq = c.write({"Engine": {"engine.throttle": 50.0}})
    c.wait(seq, until="queued")                       # 全員が受け取ってから進める
    c.sim.step(ms(500)); c.sim.wait_paused()
    print(c.wait(seq).target("Engine").applied_at)    # 0
```

- 止まっている間、`GetTime` は `now` 0、`paused` 真、`running` 偽（まだ始まっていない）、`ended` 偽です。`WaitPaused` はすぐに 0 を返します。事象の `phase` は `waiting` のままで、ランチャーは `standing still before the first step until a resume, step or run_until command ...` の行を書きます。
- 書いたら、`wait(seq, until="queued")` で全員が受け取ったことを確かめてから進めます。受け取る前に進めると、時刻 0 に間に合わないことがあります。
- 止まっている時間に、それだけの上限はありません。`start_timeout_ms` は、止まるまで（全員がそろい、変数を出す ECU とつながるまで）と、進んだ後（全員が最初のステップに着くまで）にそれぞれ効き、止まっている間は数えません。止まるまでか進んだ後に participant が固まると、`simulation-start` の誤りになります（着いていない participant と状態を文言に挙げます）。
- `--timeout-ms` は、止まっている時間も数えます（一時停止している時間を数えるのと同じです）。クライアントが来ないまま `--timeout-ms` が過ぎると、時刻 0 のステップを走らせてから中断し、誤り `timeout` になります（文言に、最初のステップの前に止まっていたことを書きます）。進めた後に過ぎたときも、文言に、最初のステップの前に止まっていた時間を書きます。`--timeout-ms` がなければ、止める要求が来るまで待ちます。
- 止める要求（Ctrl+C、`stop` の命令、標準入力の終わり、クライアントの `Stop`）でも止まるのをやめます。そのときは、時刻 0 のステップを 1 つ走らせてから止まります。待っている間に必須の participant が抜けると（プロセスが終わったなど）、すぐに待つのをやめて、始まらなかった実行として終わります（`ended-before-start`）。
- `pause` と `speed` では進みません（`speed` は、進んだ後の速さになります）。時刻を指定した `pause`（0 より後）は、`run_until` と同じく進みます。`run_until` 0 を送ると、時刻 0 のステップを走らせて 0 で止まります（`--control` なしの `--start-paused` と同じ止まり方）。
- 止まるより前（照合や、participant がそろうのを待つ間）に受け付けた命令も効きます。止まるのは、止まる時点で 0 で止まることになっているときだけです。`resume` を受け付けていれば止まらずに始まり、`step` や `run_until` を受け付けていれば、止まらずに 0 から進んでその時刻で止まります（止まってから受けたのと同じ結果）。その後に `pause` を受け付けていれば、また止まります。標準入力から来た命令も同じです。
- `--control` なしの `--start-paused`（`--stdin-commands` だけ）は、時刻 0 のステップを走らせてから 0 で止まります。この場合と、一時停止してから書く場合は、`at` を止まっている時刻より後にします（下の「Python」）。

### 2 つの口の命令

時間の命令は、標準入力（`--stdin-commands`）とクライアントの両方から受け付けます。どちらも同じ経路を通ります。

- 1 つずつ順に処理します。受け付けるかを決め、`command` の事象を書き、時間のゲートに渡すまでを、ほかの命令をはさまずに行います。事象の順が、効いた順です。どの口から来たかは `source` でわかります（`stdin`、`eof`、`control_server`）。
- 規則は同じです（`step` は一時停止している間だけ、など）。クライアントへの答えは、受け付けなければ gRPC の誤りで、文言は事象の `message` と同じです。引数の誤り（範囲の外の速さ、0 以下のステップ）は `INVALID_ARGUMENT`、今はできない命令は `FAILED_PRECONDITION` です。
- クライアントの命令の名前は、`Pause` が `pause`（時刻つきなら `at_ms`）、`Resume` が `resume`、`Step` が `step`（`ms`）、`RunUntil` が `run_until`（`at_ms`）、`SetSpeed` が `speed`、`Stop` が `stop` です。時刻はミリ秒に切り上げ、さらに controller の刻みに切り上げます（事象の値が、実際に止まる時刻です）。
- `Stop` は、標準入力の `{"command":"stop"}` と同じです（1 回目は順に止め、2 回目は待たずに participant を終わらせます）。結果の `interrupted_by` は `"control server stop"` です。`Stop` の理由の文は、監査の行にだけ残ります。シミュレーションが終わった後（始まらなかったときも）の `Stop` は、時間の命令と同じく断ります（`accepted: false`、`FAILED_PRECONDITION`）。結果は変わりません。`Stop` の事象も、時間の命令と同じ順に並びます。
- 標準入力とクライアントが同時に命令を送ると、どちらが先かは届いた順です。たとえば、クライアントの `step` と標準入力の `resume` が重なると、事象の順で、止まるか進み続けるかが決まります。

### 認証

- ループバック（ポート番号だけ、`localhost:<port>`、`[::1]:<port>`、数字の `127.x.y.z:<port>`、`unix:<path>`）では、`--control-credentials` を付けなければ認証はありません。同じコンピューターの誰でもつなげます。`unix:` のソケットは、ファイルの権限しだいで、同じコンピューターのほかの利用者もつなげます。
- ほかのコンピューターからつながるアドレス（`0.0.0.0:<port>` など）では、TLS と認証情報が要ります（`--control-tls-cert`、`--control-tls-key`、`--control-credentials`）。足りなければ引数の誤り（終了コード 4）です。
- 認証情報のファイルは 1 行に 1 つで、`<role> <name> <token>` です（role は viewer、operator、admin。トークンは 16 文字以上の秘密の文字列）。`#` で始まる語から行末まではコメントです。ECU はトークンを使いません。前の版の `agent` の行は読み込みの誤りです（文言のとおり、その行を消します）。1 件もないファイル（空、コメントだけ）は引数の誤りです（付けたのに誰でもつなげる、を避けるため）。誤りの文は行番号と何が違うかだけで、行の語は出しません（トークンかもしれないため）。トークンは引数に書けません（ランチャーの引数は、事象 `hello` の `arguments` や、プロセスの一覧に出るため）。

```text
# role     name     token
viewer     guest    5e07d2...（長い乱数）
operator   tester   0f3c9a...
admin      lead     7b21e4...
```

- `--control-audit-log <file>` で、誰が何を変えたかをファイルに追記します（書けなくなっても実行は続き、その行は失われます）。呼び出しに要る役割は、viewer は読むだけ、operator は時間の操作と書き込み、`Stop` は admin だけです。
- ECU とランチャーの間（SIL Kit）には認証がありません。レジストリに参加できる者は、ECU に書き込めます。閉じたネットワークで使います。

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
- どの ECU にも Agent があります（変数を宣言しなくても、どの ECU にもある入力 `ecu.connected` を持ちます）。そのため、変数を宣言しない ECU でも、ステップごとに Agent の処理（書き込みの反映と、`sample_period` ごとの値の読み出し）が走り、`ecu_main` の ECU は `sample_period` ごと（既定 10 ms）に値のメッセージを 1 通出します。`Peer` では監視の出力（`PeerOptions::monitor`）も付きます。
- 外からの書き込みは、指定した仮想時刻以後の最初のステップで、アプリの処理の前に反映されます（そのステップの J1939 の受信処理や部品のタイマーは反映より前に動くので、それらが新しい値を使うのは次のステップから）。値は、アプリと LIN の処理の後に読み出します。SIL Kit とローカルで同じ時点です。
- 同じプロセスからは、`ecu.agent().submit()` で書き込み、`latest()` で読めます。`Simulation::add<App>()` で足した ECU は、`sim.ecu(name).agent()` で引けます（例: `examples/vehicle`）。
- ランチャーの Control Server につながるのは、SIL Kit の participant の ECU だけです。`ecu_main` の ECU は自動でつながります。自分で書いた `Peer` は、ECU が変数を宣言した後、`start()` の前に `vehicle_sil::control_silkit::AgentTransport transport(peer.agent(), peer.runtime());` を作ってつなぎます（`peer.stop()` の後、`Peer` より先に破棄します）。ローカル（`Simulation()`、`LocalBench`）の ECU は Control Server につながりません。同じプロセスから `agent()` で読み書きします。

## Python

```sh
uv sync                                         # .venv に grpcio、grpcio-tools、protobuf などを入れる
source .venv/bin/activate                       # または、各コマンドの前に uv run を付ける
python -m vehicle_sil_control.generate          # スタブを python/vehicle_sil_control/_generated に作る
export PYTHONPATH=$PWD/python
```

- Python は 3.11 以上です。版と依存はリポジトリ直下の `pyproject.toml`（`control` グループ）と `uv.lock` で決めています（[build.md](build.md) の「Python（uv）」）。
- uv を使わない場合は `pip install grpcio grpcio-tools protobuf`、MSYS2 UCRT64 では `pacman -S mingw-w64-ucrt-x86_64-python-grpcio mingw-w64-ucrt-x86_64-python-grpcio-tools` で入れます。

ランチャーは、`Launch` で子プロセスとして起動できます。`vehicle_sil_launch run --events --stdin-commands --control <address>` を起動し、事象を別のスレッドで読みます（標準ライブラリだけで、gRPC は要りません）。

```python
from vehicle_sil_control import Control, Launch, ms

with Launch("build/linux-gcc-release/tools/launch/vehicle_sil_launch", "simulation.yaml", start_paused=True) as launch:
    with Control(launch.address) as c:   # address は事象 control_server を待って返す
        ...
    launch.stop()                        # {"command":"stop"}（with を出るときも、動いていれば止める）
    print(launch.wait()["result"])       # run --json と同じ結果
```

- `Launch(launcher, config, address="0", start_paused=False, arguments=(), cwd=None, stderr=None, keep_logs=True)`: `arguments` は `run` のほかの引数（`--speed`、`--disable`、`--control-credentials` など）です。`events`（読んだ事象の dict の一覧）、`result`、`wait_for_event(event, timeout, after=None, **keys)`（キーの値が合う最初の事象を待つ）、`send(command)`（標準入力に命令を 1 行）、`stop()`、`wait(timeout)`、`returncode`。ランチャーが結果を書かずに終わったときや、待つ事象が来ないまま実行が終わったときは `LaunchError` です（文言に結果と誤りの所見）。
- 自分でランチャーを起動するときは、`control_address(lines)` が、事象の行（文字列か dict）から最初の `control_server` の `address` を返します。

別のコンピューターから、TLS とトークンを付けたランチャー（上の「認証」）にも、同じようにつなぎます（証明書の作り方と、つながらないときの文は [howto/multi-pc.md](howto/multi-pc.md)）。

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

時刻は仮想時間のナノ秒です（`ms()`、`s()` で作る）。呼び出しの失敗は `ControlError`（`code` は gRPC の状態コード。`reason` は、サーバーが理由を付けて断ったときのその語で、なければ空。シナリオの実行中の書き込みと時刻の操作は `scenario-running`）で、Agent が断った書き込みは例外ではなく、操作の結果（`invalid` など）になります。

- 止めてから書くときは、`at` を止まっている時刻（`c.sim.time().now`）より後にします（たとえば 1 ns 後）。止まっている時刻のステップは、すでに走った Peer があるので、その時刻か `at` なしの書き込みは `late` になるか、反映のステップが実行ごとに変わることがあります。例外は、ランチャーの `--start-paused` で最初のステップの前に止まっているときです（上の「始まる前に書く」）。
- 時刻と、複数の Peer の値を、同じ瞬間のものとして読むには `c.snapshot(peers=())` を使います（下の「時刻と値をまとめて読む」）。`c.sim.time()` と `c.values()` を別々に呼ぶと、その間にシミュレーションが進むことがあり、2 つの間に約束はありません。
- `write()` は自動では送り直しません。応答を受け取る前に接続が切れた書き込みは、受け付けられているかもしれません（同じ書き込みを送り直すと、別の操作になります）。
- 複数のクライアントが同じ変数に同じ時刻で書くと、残る値は Server に届いた順で決まります。
- 時間の状態の購読（`watch(time=True)`）は約 100 ms ごとの最新で、短い一時停止は現れないことがあります。止まるのを待つには `c.sim.wait_paused()` を使います。`step()`、`run_until()`、時刻つきの `pause()` の応答（`TimeState`）と、その直後の `time()` は、命令を受け付けた後のその時点の状態です。進んでいる途中なら `paused` は偽で、`now` は進む前か途中の時刻です（速ければ、もう着いて止まっていることもあります）。着いて止まった状態は `wait_paused()` の応答で読みます。
- `peers()` と `watch()` は、同じ Peer を同じ内容で返します。`has_agent` は Agent がつながったことがあるか（既定は偽）、`simulation_state` は SIL Kit の状態、`virtual_time` は Agent がシミュレーションの時刻で動くか（Agent の hello のとおり。偽は実時間の Peer か、Agent がない）です。この欄を持たない前の版の Server では、どの Peer も偽になります。変数（`schema` の各要素）の `builtin` は、vehicle-sil が付けた変数か（名前が `ecu.`、`peer.`、`j1939.` で始まる。Server が名前で決めます）で、偽はアプリが宣言した変数です。アプリが `j1939.` で始まる名前の変数を宣言すると、それも真になります。`source` は、ランチャーの構成ファイルの対応表（`participants[].variables`）が掛かっている変数の、実行ファイルでの名前です（掛かっていなければ空。この欄を持たない前の版の Server でも空）。対応表のある Peer では、`schema` の名前、型、範囲、単位、読む値、書く値のどれも、対応の後のものです。`builtin` も対応の後の名前で決まります。実行ファイルの変数に合わず当たらなかった対応があると、その対応の名前（共通の名前と実行ファイルでの名前）への書き込みは `invalid` で断られ、理由に当たらなかった訳が入ります（値は実行ファイルの名前のまま読めます）。書き込みを断る理由の文も対応の後の名前で書きます（ECU の側が断ったときは `engine.speed (the executable's rpm_x10): ...` の形で、値は実行ファイルの値です）。`TimeState.running` が偽のとき、`ended` が真なら終わっていて、偽ならまだ始まっていません。
- シミュレーションの状態の購読（`watch(simulation=True)`）は `SimulationState` を返します。必須の participant ごとの SIL Kit の状態（`participants`）、失敗した participant と理由（`failed_participant`、`failure_reason`）、終わったか（`ended`）が入ります。Server が状態を読めなかったときは、前の値に理由（`error`）が付きます。クライアントが遅くて Server が途中の状態を持ちきれなかったときは、捨てた数が gRPC の `Update.dropped` に入ります（Python の `watch()` はこの数を返しません。失敗と終わりは後の状態に残るので失われません）。`peers()` も、読めなかったときは、呼ぶたびに `ControlWarning` で知らせます（同じ箇所からの 2 回目以降も出します。出すか、例外にするか、無視するかは利用者の警告フィルターのとおりで、ライブラリはフィルターを変えません）。

### 時刻と値をまとめて読む（`snapshot()`）

```python
c.sim.step(ms(100)); c.sim.wait_paused()
snap = c.snapshot()                       # 全 Peer。c.snapshot(["Engine", "Body"]) で絞る
if snap.consistent:                       # 値は、止まっている時刻 snap.time.now のもの
    print(snap.time.now, snap.values["Engine"].values["engine.speed"])
```

`snapshot()`（gRPC の `GetSnapshot`）は、時間の状態（`sim.time()` と同じ `TimeState`）と、Peer ごとの最後の値（`values()` と同じ `Values`）を、Server が 1 回で読んで返します。Server は、時間の状態を先に読み、その後で全 Peer の値を同じ瞬間に読みます。約束は、シミュレーションの時刻で動く Peer について、Server が時間を操作できる（`time.available`）ときのものです。

- いつでも（動いている間も）: 値は時刻より古くありません。`time.now` より前に始まった各 Peer のステップの値は、すべて入っています。値のほうが新しいことはあります（読む間にシミュレーションが進んだ分）。
- `consistent` が真: 時刻は `time.now` で止まっていて（`time.paused`）、どの値も `time.now` より後のステップのものではありません。値は、止まっている時刻のものです（`time.now` より前のステップは全部、後のステップは 1 つもない）。
  - ちょうど `time.now` に始まるステップの値は、入っているかどうか決まりません（止まっている間に届きます）。どちらかは `Values.time` でわかります。
  - 止まるのを待って（`sim.wait_paused()`）から呼び、その間にだれもシミュレーションを進めなければ、`consistent` は真です。
- 前提と限り:
  - この約束は、ECU が値をステップの中から送っているかぎりのものです（`ecu_main` で作った ECU はそうします）。破れると、ランチャーが警告 `control-link-late-values` を出します。そのときは `consistent` が真でも、値が欠けていることがあります。
  - 「ステップの値」は、その時刻より前に読んだ最後のサンプルです（Agent が読むのは `sample_period` ごとで、ステップより長いことがあります）。
  - 実時間の Peer（シミュレーションの時刻で動かない Peer。`peers()` の `virtual_time` が偽）の値には、約束がありません。`consistent` は、その値を見ずに決めます。
  - 切れた Peer の最後の値は、そのまま入っています。つながっているかは `c.peers()` で見ます。
  - 止まった直後は、同じ時刻でも、ちょうどその時刻のステップの値が届く前と後の 2 通りの応答がありえます（どちらも `consistent` は真）。届いたかどうかは `Values.time` でわかります。
- `consistent` が偽: 動いている、終わっている、呼び出しの間に進んで後のステップの値が入った、のどれかです。値と時刻は、偽でも入っています。
- `values` には、値を持つ Peer だけが入ります。まだ値のない Peer、Agent のない participant、知らない名前は入りません。gRPC の応答（`GetSnapshotReply.values`）は、名前を挙げたときは挙げた順に 1 つずつ（同じ名前は 1 回）、挙げなければ Agent を持つ Peer を名前の順に並べ、値のない Peer は `snapshot` なし、Agent のない名前は `incarnation` も空です。値の並びは、その `incarnation` のスキーマ（`ListPeers`）の順です。
- 役割は viewer で呼べます。1 回に挙げられる名前は 1024 個までです（`INVALID_ARGUMENT`）。応答が 1 つのメッセージの上限（4 MiB）を超えるときは `RESOURCE_EXHAUSTED` で、文言のとおり、Peer を分けて呼びます（分けた呼び出しどうしの間に約束はありません）。
- この呼び出しを持たない前の版の Server では、`ControlError`（`code` は `UNIMPLEMENTED`）になります。そのときは `sim.time()` と `values()` を使います（2 つの間に約束はありません）。

## CLI

`--server`（既定は `VEHICLE_SIL_CONTROL_SERVER`）に、ランチャーの事象 `control_server` の `address`（人向けの出力では `control server at` の行）を渡します。

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

## 制限

- 時間の操作（一時停止、ステップ実行、速度）で止まるのは仮想時間で同期している Peer だけで、実時間の Peer は走り続けます。速度を指定したとき、シミュレーションが実時間に遅れても、後から追いつこうとはしません（遅れた分だけ後ろにずれたまま、指定の速度で進みます。[internals/virtual-time.md](internals/virtual-time.md) の時間ゲートの「遅れ」）。
- リセット、participant の起動と再起動、CAN/LIN のチャネルの選択はありません。ランチャーの Control Server は、そのランチャーの 1 回の実行だけを扱い、実行が終わると閉じます。
- Peer の `status`（Python の `Peer.failed` と `error`、CLI の `FAILED`）は、いつも空です（ECU の Agent はこれを送りません）。participant の失敗は、シミュレーションの状態（`watch(simulation=True)` の `failed_participant` と `failure_reason`）で読みます。
- 値の購読は最新値です（読む前に来た新しい値で置き換えます）。すべての値の記録は、SIL Kit のトレースか `TapPort` を使います。
- 1 回の `write()` の書き込みは 4096 個まで、1 回の `snapshot()` の Peer の名前は 1024 個まで、同時に待つ呼び出しと購読は合わせて 256 本まで（Server の既定）です。
