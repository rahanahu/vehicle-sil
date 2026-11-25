# 信号を外から操作する

[English](../../howto/operate-signals.md)

動いているシミュレーションの ECU の変数を、Python やコマンドから読み書きし、仮想時間を一時停止して少しずつ進める手順です。`vehicle_sil_launch run` に `--control` を付けると起動する Control Server に、gRPC で接続します。

## 方法を選ぶ

変数を外から読み書きする方法は 4 つあります。このガイドが扱うのは、2 行目と 3 行目です。

| したいこと | 使うもの | 起動の仕方 | 読むもの |
|---|---|---|---|
| 決まった手順で値を設定して確認し、終了コードで合否を出す | シナリオ（YAML） | `vehicle_sil_launch run simulation.yaml --scenario <シナリオ>` | [シナリオでテストを書く](scenario-tests.md) |
| 値を見て、次に書き込む値をプログラムで決める | Python のクライアント（`vehicle_sil_control`） | スクリプトの中の `Launch(...)`（連続実行なら `Serve(...)`）が `vehicle_sil_launch serve` を `--control` 付きで起動する | このガイドの「最初のスクリプトを実行する」から |
| Python を書かずに、1 つずつ読む、書き込む、進める | CLI（`python -m vehicle_sil_control`） | 自分で `vehicle_sil_launch run --control 8510 simulation.yaml` を起動する | このガイドの「コマンドで 1 つずつ操作する」 |
| ブラウザで値を見ながら、スライダーやスイッチで書き込む | GUI（`vehicle-sil-gui`） | `scripts/run-gui.py`（ビルド済みのツールでは `vehicle-sil-gui simulation.live.yaml`） | [gui.md](../gui.md) の「ECU の変数」 |

シナリオは、同じ設定ファイルと同じシナリオなら、結果がいつも同じです。Python と CLI からの操作は、書き込みを受信した順や指定した時刻によって、結果が変わることがあります。操作した結果をテストとして残すときは、同じ手順をシナリオに書きます。

仮想時間の一時停止と再開だけなら、`vehicle_sil_launch run` の標準入力からもできます（「標準入力から時間を操作する」）。

## 前提

| 必要なもの | 条件 |
|---|---|
| vehicle-sil のビルド | README の「[リポジトリからビルドする](../../../README.ja.md#リポジトリからビルドする)」のとおりに、プリセット `linux-gcc-control` でビルドしてあること。 |
| [uv](https://docs.astral.sh/uv/) | Python のクライアントと CLI の実行に使います。 |

コマンドは、すべてリポジトリのルート（`README.md` のある所）で実行します。このガイドに出てくるパスは、次の 3 つです。

| もの | パス |
|---|---|
| `vehicle_sil_launch` | `build/linux-gcc-control/tools/launch/vehicle_sil_launch` |
| 設定ファイル | `build/linux-gcc-control/examples/ecu_main/simulation.yaml` |
| Python のクライアント | `python/`（パッケージ `vehicle_sil_control`） |

テンプレートのプロジェクトとビルド済みのツールを使うときは、次のように読み替えます。表の `$T` は、ツールのディレクトリ `tools/vehicle-sil-tools-<version>-linux-x86_64` です。テンプレートの 2 つの ECU は、このガイドの例と同じ変数を持っています。

| もの | リポジトリでビルドしたとき | テンプレートのプロジェクトのとき |
|---|---|---|
| 実行するディレクトリ | リポジトリのルート | プロジェクトのディレクトリ（`simulation.yaml` のある所） |
| `vehicle_sil_launch` | `build/linux-gcc-control/tools/launch/vehicle_sil_launch` | `$T/bin/vehicle_sil_launch` |
| 設定ファイル | `build/linux-gcc-control/examples/ecu_main/simulation.yaml` | `simulation.yaml` |
| 準備 | 下の「準備」の 3 行 | 不要です。 |
| Python の実行 | `uv run python <引数>` | `PYTHONPATH=$T/python uv run --no-project --no-build --python '>=3.11,<3.15' --with-requirements $T/python/requirements.txt python <引数>` |
| 例のスクリプト | `examples/launch_control/` | 入っていません。「最初のスクリプトを実行する」の全文をファイルに保存します。 |

## 準備

Python のパッケージをインストールし、gRPC のスタブを生成します。

```sh
uv sync                                          # .venv に grpcio などをインストールする（README の手順で実行済みなら、何も変わらない）
export PYTHONPATH="$PWD/python"                  # クライアントのパッケージの場所。シェルを開くたびに実行する
uv run python -m vehicle_sil_control.generate    # スタブを python/vehicle_sil_control/_generated に生成する（最初の 1 回）
```

### 確認

`generate` は、生成先を 1 行で表示します。

```text
generated into <リポジトリのルート>/python/vehicle_sil_control/_generated
```

`vehicle_sil_launch` が Control Server を持っていることは、次のコマンドで確認できます。`control-server` と表示されれば、持っています。

```sh
build/linux-gcc-control/tools/launch/vehicle_sil_launch version --json | grep control-server
```

```text
    "control-server",
```

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| `grep` が何も表示しない | Control Server なしでビルドした `vehicle_sil_launch` です。 | プリセット `linux-gcc-control` でビルドします（ほかのプリセットでは、configure に `-DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON` を付けます）。 |
| `No module named vehicle_sil_control` | `PYTHONPATH` を設定していません。 | `export PYTHONPATH="$PWD/python"` を、リポジトリのルートで実行します。 |
| `No module named 'grpc_tools'` | `.venv` にパッケージがありません。 | `uv sync` を実行します。 |

## 例の変数

例の設定ファイルは、エンジンの ECU（participant 名 `Engine`）と表示の ECU（`Dashboard`）を動かします。このガイドで読み書きする変数は、次の 3 つです。

| participant | 変数 | 入出力 | 型と単位 | 内容 |
|---|---|---|---|---|
| `Engine` | `engine.throttle` | 入力（外から書き込める） | 実数、%、0〜100 | スロットルです。書き込まなければ 20 です。 |
| `Engine` | `engine.speed` | 出力（読むだけ） | 整数、rpm | 回転数です。0 から始まり、10 ms のステップごとに 50 rpm ずつ、800 + 20 × スロットル（rpm）に近づきます。 |
| `Dashboard` | `dashboard.engine_speed` | 出力（読むだけ） | 整数、rpm | `Engine` が 100 ms ごとに J1939 で送信した回転数を、受信した値です。 |

スロットルが 20 % なら回転数は 1200 rpm、50 % なら 1800 rpm、0 % なら 800 rpm で一定になります。

自分の ECU の変数の名前、型、範囲は、「コマンドで 1 つずつ操作する」の `show` か、ECU の実行ファイルの `--describe`（`variables` の配列）で調べます。ECU のコードに追加するものはありません。`ecu_main()` で作った ECU が `ecu.input()` と `ecu.output()` で宣言した変数は、そのまま外から読み書きできます。

## 最初のスクリプトを実行する

シミュレーションを時刻 0 で一時停止した状態で起動し、スロットルに 50 % を書き込み、500 ms 進めてから、回転数を読みます。次が [examples/launch_control/write_and_read.py](../../../examples/launch_control/write_and_read.py) の全文です（1 行目の `# SPDX-License-Identifier: MIT` だけ省いています）。

<!-- examples/launch_control/write_and_read.py -->
```python
import sys

from vehicle_sil_control import Control, Launch, ms

launcher, config = sys.argv[1], sys.argv[2]                   # vehicle_sil_launch and simulation.yaml

# Starts "vehicle_sil_launch serve --once" with its control server, paused before the first step.
with Launch(launcher, config, start_paused=True) as launch:
    with Control(launch.address) as c:
        c.sim.wait_paused()                                   # every ECU stands still at 0 ms
        seq = c.write({"Engine": {"engine.throttle": 50.0}})  # participant name, then variable name
        c.wait(seq, until="queued")                           # the ECU has received the write

        c.sim.step(ms(500))                                   # run 500 ms, then pause again
        now = c.sim.wait_paused().now
        values = c.values("Engine").values                    # the values the ECU sent last
        print(f"at {now / 1e6:g} ms: engine.throttle = {values['engine.throttle']}, "
              f"engine.speed = {values['engine.speed']}")
```

```sh
uv run python examples/launch_control/write_and_read.py \
    build/linux-gcc-control/tools/launch/vehicle_sil_launch \
    build/linux-gcc-control/examples/ecu_main/simulation.yaml
```

### 確認

<!-- examples/launch_control/write_and_read.py: the output -->
```text
at 500 ms: engine.throttle = 50.0, engine.speed = 1800
```

回転数は 0 から 10 ms ごとに 50 rpm ずつ上がり、スロットル 50 % のときの 1800 rpm で一定になっています。`50.0` を `0.0` に変えると `engine.speed = 800` になり、`c.write()` と `c.wait()` の 2 行を消すと `engine.throttle = 20.0, engine.speed = 1200` になります。

スクリプトの各行がすることは、次のとおりです。

| 行 | すること |
|---|---|
| `Launch(..., start_paused=True)` | `vehicle_sil_launch serve --once --control 0` を子プロセスとして起動し、`{"command":"run","intent":{"start_paused":true}}` を送ります（`--control 0` は空いているポートを選びます）。 |
| `launch.address` | `vehicle_sil_launch` が通知した Control Server のアドレスです。 |
| `Control(launch.address)` | Control Server に gRPC で接続し、`with` を抜けると接続を閉じます。 |
| `c.sim.wait_paused()` | シミュレーションが一時停止するまで待ち、時間の状態を返します。 |
| `c.write({"Engine": {"engine.throttle": 50.0}})` | participant 名と変数の名前を指定して書き込み、操作の番号を返します。 |
| `c.wait(seq, until="queued")` | ECU が書き込みを受信するまで待ちます。 |
| `c.sim.step(ms(500))` | 仮想時間を 500 ms 進めて、また一時停止します。 |
| `c.values("Engine").values` | ECU が最後に送信した値を、変数の名前をキーとする dict で返します。 |

外側の `with` を抜けると、シミュレーションを停止して終わります。

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| `ModuleNotFoundError: No module named 'vehicle_sil_control'` | `PYTHONPATH` を設定していません。 | 「準備」の `export` の行を実行します。 |
| `ImportError: the Control API stubs are missing in ...` | スタブを生成していません。 | 「準備」の `generate` の行を実行します。 |
| `LaunchError: ... --control needs a vehicle_sil_launch built with the control server` | Control Server なしでビルドした `vehicle_sil_launch` を指定しています。 | `build/linux-gcc-control/` の下のものを指定します。 |
| `LaunchError: ... the executable engine/vehicle_sil_engine_ecu does not exist` | `examples/ecu_main/simulation.yaml`（ソースの側）を指定しています。 | `build/linux-gcc-control/examples/ecu_main/simulation.yaml` を指定します。 |
| `AttributeError: 'NoneType' object has no attribute 'values'` | ステップを 1 回も進めていない間は値がなく、`c.values()` が `None` を返します。 | `c.sim.step()` で進めてから読みます。 |

## コマンドで 1 つずつ操作する

Python を書かずに、端末から 1 つずつ操作します。端末を 2 つ使い、どちらもリポジトリのルートで実行します。

端末 1 で、Control Server をポート 8510 で開き、時刻 0 で一時停止した状態でシミュレーションを起動します。

```sh
build/linux-gcc-control/tools/launch/vehicle_sil_launch run --start-paused --control 8510 --stop-at-ms 1000 \
    build/linux-gcc-control/examples/ecu_main/simulation.yaml
```

次の 2 行が表示されれば、接続できます。2 行の間には、registry と ECU の起動の行が表示されます。

```text
[vehicle_sil_launch] control server at 127.0.0.1:8510
[vehicle_sil_launch] standing still before the first step until a resume, step or run_until command (--start-paused with a control server)
```

端末が 1 つのとき（スクリプトや CI）は、端末 1 のコマンドの末尾に `> launch.log 2>&1 &` を付けてバックグラウンドで起動し、`launch.log` に上の 2 行が出てから、同じ端末で端末 2 のコマンドを実行します。途中でやめるときは、CLI の `stop` か `kill <PID>`（SIGTERM）で、シミュレーションと ECU が止まります（`stop` では結果は `stopped`、終了コード 0。`kill` では結果は `interrupted`、終了コード 130）。

端末 2 で、`export PYTHONPATH="$PWD/python"` を実行してから、次のコマンドを順に実行します。CLI は、指定しなければ `127.0.0.1:8510` に接続します。

急ぐ必要はありません。端末 1 は、`step` か `resume` を実行するまで時刻 0 で一時停止したまま待ちます。`step` の後も、次のコマンドまで一時停止して待ちます。

| すること | コマンド |
|---|---|
| participant の一覧を表示する | `uv run python -m vehicle_sil_control peers` |
| 時刻を表示する | `uv run python -m vehicle_sil_control time` |
| 書き込む | `uv run python -m vehicle_sil_control set Engine engine.throttle=50` |
| 500 ms 進めて一時停止する | `uv run python -m vehicle_sil_control step 500` |
| 変数と値を表示する | `uv run python -m vehicle_sil_control show Engine` |
| 範囲の外の値を書き込んで、結果を見る | `uv run python -m vehicle_sil_control set Engine engine.throttle=150 --wait done` |
| 2 つの変数を書き、2 つめは出力の変数にする | `uv run python -m vehicle_sil_control set Engine engine.throttle=30 engine.speed=5 --wait done` |
| 再開する | `uv run python -m vehicle_sil_control resume` |

### 確認

それぞれのコマンドの出力です。`seq` の後の番号は、実行ごとに変わります。

<!-- vehicle_sil_control peers -->
```text
Dashboard  connected  22 variables  SIL Kit: ReadyToRun
Engine     connected  22 variables  SIL Kit: ReadyToRun
```

<!-- vehicle_sil_control time -->
```text
paused at 0 ms, speed max
```

<!-- vehicle_sil_control set Engine engine.throttle=50 -->
```text
seq 1794666759502163
seq 1794666759502163 Engine: queued
```

<!-- vehicle_sil_control step 500 -->
```text
paused at 500 ms, speed max
```

`show Engine` の最初の 3 行です。列は、名前、値、単位、入出力と型と範囲です。続きの行は、vehicle-sil が自動で追加する変数（`peer.`、`j1939.` で始まる名前）です。最初の `step` の前に `show` を実行すると、ECU がまだステップを 1 つも実行していないので、値の列は空です。

<!-- vehicle_sil_control show Engine: the first lines -->
```text
ecu.connected    True         input bool
engine.throttle  50.0  %      input real [0..100]
engine.speed     1800  rpm    output integer
```

範囲の外の値を書き込むと、エラーにはならず、結果が `invalid` になります。

<!-- vehicle_sil_control set Engine engine.throttle=150 --wait done -->
```text
seq 1794666759502164
seq 1794666759502164 Engine: invalid (engine.throttle: 150 is out of range)
```

いくつもの変数を書くと、行の終わりに、反映された数と、断られた書き込みが出ます。ここでは 2 つめの書き込み（出力の変数）を確認が断るので、どれも反映されません。

<!-- vehicle_sil_control set Engine engine.throttle=30 engine.speed=5 --wait done -->
```text
seq 1794666759502165
seq 1794666759502165 Engine: invalid (engine.speed: an output is not writable), 0 of 2 writes took effect, refused: engine.speed
```

<!-- vehicle_sil_control resume -->
```text
running at 500 ms, speed max
```

この行の時刻は、シミュレーションが進み始めた後に読むので、500 ms より後になることがあります（[control-api.md](../control-api.md#時間の状態を読む) の「時間の状態を読む」）。

`resume` の後、端末 1 のシミュレーションは指定した停止時刻（1000 ms）まで進み、結果の表を表示して、終了コード 0 で終わります。

CLI のほかのコマンドは、次のとおりです。

| すること | コマンド（`uv run python -m vehicle_sil_control` の後） |
|---|---|
| 時刻を指定して書き込む | `set Engine engine.throttle=50 --at-ms 600` |
| 一時停止する | `pause`（`--wait` を付けると、一時停止するまで待つ） |
| 指定した時刻まで進めて一時停止する | `run-until 700` |
| 速度を変える | `speed 1`（実時間と同じ速度）、`speed max`（最速） |
| 値が変わるたびに表示する | `watch --peer Engine --count 10` |
| シミュレーションを停止する | `stop` |
| 別のアドレスの Control Server に接続する | `--server <アドレス>` をコマンド名の前に付けるか、環境変数 `VEHICLE_SIL_CONTROL_SERVER` に設定する |

`--control 0` で起動すると、空いているポートが選ばれます。そのときは、端末 1 の `control server at` の行のアドレスを `--server` に渡します。

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| 端末 1 が `--control needs a vehicle_sil_launch built with the control server` と表示して、終了コード 4 で終わる | Control Server なしでビルドした `vehicle_sil_launch` です。 | `build/linux-gcc-control/` の下のものを使います。 |
| 端末 1 が、何も起動せずに終了コード 3 で終わる | ポート 8510 を、ほかのプロセスが使っています。 | `--control 0` で起動し、表示されたアドレスを CLI の `--server` に渡します。 |
| 端末 2 に `error: UNAVAILABLE: ... Connection refused` | 端末 1 のシミュレーションが終わっているか、ポートが違います。 | 端末 1 を起動し直し、`control server at` の行のアドレスを確認します。 |
| `step` が `FAILED_PRECONDITION` のエラーになる | 一時停止していません。 | 先に `pause --wait` を実行します。 |
| `watch --count` が終わらない | 一時停止している間は、新しい値が送信されません。 | `resume` か `step` で進めるか、Ctrl+C で止めます。 |

## Python の例を実行する

ここから後の節は、Python のクライアントの呼び出しを、することごとに説明します。コードは、[examples/launch_control](../../../examples/launch_control) の 2 つの例から抜き出したものです。どちらの例も、結果を自分で確認し、期待どおりなら終了コード 0、違えば 1 で終わります。

| 例 | すること |
|---|---|
| [write_before_start.py](../../../examples/launch_control/write_before_start.py) | 最初のステップの前に書き込み、500 ms 進めて、書き込みの結果と値を確認し、最後まで実行します。 |
| [pause_and_step.py](../../../examples/launch_control/pause_and_step.py) | 実時間の 4 分の 1 の速度で動かし、回転数が 1000 rpm に達したら一時停止し、時刻を指定して書き込み、100 ms 進めて確認します。 |

最初のスクリプトと違い、この 2 つの例は、ランチャーを `--launch` で受け取ります。

```sh
uv run python examples/launch_control/write_before_start.py \
    --launch build/linux-gcc-control/tools/launch/vehicle_sil_launch \
    build/linux-gcc-control/examples/ecu_main/simulation.yaml
uv run python examples/launch_control/pause_and_step.py \
    --launch build/linux-gcc-control/tools/launch/vehicle_sil_launch \
    build/linux-gcc-control/examples/ecu_main/simulation.yaml
```

### 確認

`write_before_start.py` の出力です。ポートの番号は、実行ごとに変わります。

<!-- examples/launch_control/write_before_start.py: the output -->
```text
control server at 127.0.0.1:45287
ok   it stands still at 0 before the first step
ok   Engine queued the throttle (queued)
ok   it stands still at 500 ms (500 ms)
ok   the throttle took effect in the step at 0 (applied at 0)
ok   engine.throttle reads 50.0
ok   engine.speed reads 1800 rpm (expected 1800)
ok   the launcher wrote the step and the resume as command events from the control server
ok   the run passed (passed, reached_stop_at)
all as expected
```

`pause_and_step.py` の出力です。実時間に合わせて動かしている途中で一時停止するので、時刻と回転数は、実行によって変わることがあります。

<!-- examples/launch_control/pause_and_step.py: the output -->
```text
engine.speed 1000 rpm at 190 ms
ok   it stands still at 190 ms
ok   it stands still at 290 ms after the step
ok   the throttle took effect in the engine's step at 200 ms (applied at 200000000)
ok   the snapshot is of 290 ms (consistent: True)
ok   engine.throttle reads 50.0 in the sample of 290 ms; engine.speed 1500 rpm
ok   the speed is max, and it still stands still
ok   the launcher wrote the commands of the control server as events (resume, pause, step, speed, resume)
ok   the run passed (passed, reached_stop_at)
all as expected
```

抜き出したコードに出てくる `expect(条件, 文)` は、例の中で定義した関数です。条件が真なら `ok` と文を、偽なら `FAIL` と文を表示します。`c` は `Control`、`launch` は `Launch` のオブジェクトです。時刻は、どの呼び出しでも仮想時間のナノ秒で、`ms(500)` や `s(1)` で作ります。

## 値を読む

| 呼び出し | 返すもの | 使うとき |
|---|---|---|
| `c.values("Engine")` | その ECU が最後に送信した値（`.values` が dict、`.time` がその値の時刻） | 1 つの ECU の今の値を読むとき。 |
| `c.snapshot(["Engine", "Dashboard"])` | 時刻と、複数の ECU の値（1 回の呼び出しで Server が読んだもの） | 時刻と値を、同じ瞬間のものとして読むとき。 |
| `c.watch(peers=["Engine"])` | 値などの更新を、受信した順に返すイテレーター | 値が変わるのを待つとき（「値の変化を待つ」）。 |
| `c.peers()` | participant と、その変数の名前、型、範囲の一覧 | 変数の名前を調べるとき。 |

`pause_and_step.py` は、一時停止してから、時刻と値を `snapshot()` で読みます。

<!-- examples/launch_control/pause_and_step.py: an excerpt -->
```python
snap = c.snapshot(["Engine"])
engine = snap.values["Engine"]
expect(snap.consistent and snap.time.now == stands.now,
       f"the snapshot is of {snap.time.now / 1e6:g} ms (consistent: {snap.consistent})")
```

出力の `ok   the snapshot is of 290 ms (consistent: True)` が、この確認の結果です。

読むときのルールは、次の 4 つです。

- ECU は 10 ms（`sample_period` の既定）ごとに値を送信するので、それより細かい変化は読めません。
- `c.sim.time()` と `c.values()` を別々に呼ぶと、その間にシミュレーションが進むことがあるので、時刻と値をそろえて読むときは `snapshot()` を使います。
- 一時停止するのを待ってから `snapshot()` を呼び、`consistent` が真なら、値は一時停止している時刻のものです。
- dict には、自分で宣言した変数のほかに、vehicle-sil が自動で追加する変数（`ecu.connected`、`peer.`、`j1939.` で始まる名前）も入っています。

`snapshot()` が保証することと制限は、[control-api.md](../control-api.md) の「時刻と値をまとめて読む（`snapshot()`）」にあります。

## 値を書き込む

`c.write()` には、participant ごとに、変数の名前と値を渡します。1 回の呼び出しで、複数の participant に書き込めます。戻り値は操作の番号で、結果を待つときに使います（「書き込みの結果を待つ」）。

値が反映されるステップは、いつ書き込むかで決まります。

| 書き込むとき | 書き方 | 反映されるステップ |
|---|---|---|
| 最初のステップの前（`start_paused=True` で起動して、一時停止している間） | `c.write({...})` | 時刻 0 のステップです。 |
| 動いている途中 | 先に一時停止し、`at` に一時停止している時刻より後の時刻を指定して `c.write({...}, at=...)` | その ECU の、`at` 以後に始まる最初のステップです。 |

最初のステップの前に書き込むコードは、「最初のスクリプトを実行する」のとおりです。動いている途中で書き込むコードは、`pause_and_step.py` の次の部分です。`paused_at` は、一時停止している時刻です。

<!-- examples/launch_control/pause_and_step.py: an excerpt -->
```python
at = paused_at + ms(1)
seq = c.write({"Engine": {"engine.throttle": THROTTLE}}, at=at)
c.wait(seq, until="queued")
c.sim.step(ms(100))
stands = c.sim.wait_paused()
```

190 ms で一時停止したので、`at` は 191 ms です。エンジンのステップは 10 ms ごとなので、値は 200 ms のステップで反映されます。出力の `ok   the throttle took effect in the engine's step at 200 ms` が、その確認の結果です。

書き込むときのルールは、次の 5 つです。

- どちらの場合も、`c.wait(seq, until="queued")` で ECU が受信したのを待ってから、時間を進めます。
- 動いている途中で、`at` を指定しないか、一時停止している時刻を指定すると、結果が `late` になるか、反映されるステップが実行ごとに変わることがあります。
- 範囲の外の値と、出力の変数への書き込みは、例外にならず、結果が `invalid` になります。
- Parameter（`show` が `input` ではなく `parameter` と表示する変数）に書き込めるのは、最初のステップの前だけです。
- 同じ変数に、複数のクライアントが同じ時刻を指定して書き込むと、残る値は Server が受信した順で決まります。

ECU をバスから切断するのも、変数への書き込みです。どの ECU にもある `ecu.connected` に `False` を書き込みます。J1939 でどう見えるかは、[j1939-guide.md](../j1939-guide.md) の「ECU をバスから切断する（`ecu.connected`）」にあります。

反映される時刻のルールの全体は、[control-api.md](../control-api.md) の「始まる前に書く（`--start-paused`）」と「Python」にあります。

## 書き込みの結果を待つ

| 呼び出し | 待つもの |
|---|---|
| `c.wait(seq, until="queued")` | 宛先のすべての ECU が、書き込みを受信するまで。 |
| `c.wait(seq)` | 宛先のすべての ECU で、結果が決まるまで。 |

どちらも操作の結果を返し、`.target("Engine")` で宛先ごとの結果を読みます。`write_before_start.py` は、500 ms 進めた後で、結果を確認します。

<!-- examples/launch_control/write_before_start.py: an excerpt -->
```python
done = c.wait(seq)
expect(done.target("Engine").status == "applied" and done.target("Engine").applied_at == 0,
       f"the throttle took effect in the step at 0 ({done.target('Engine').status} at "
       f"{done.target('Engine').applied_at})")
```

出力の `ok   the throttle took effect in the step at 0 (applied at 0)` が、この確認の結果です。

`status` の主な値は、次の 6 つです。

| `status` | 意味 |
|---|---|
| `queued` | ECU が受信しました。 |
| `applied` | 反映されました（`applied_at` が、反映されたステップの時刻です）。 |
| `late` | 指定した時刻のステップに間に合いませんでした。 |
| `invalid` | 名前、型、範囲、書き込めるかの検査で拒否されました（接続していない宛先への書き込みも、この値です）。 |
| `rejected` | ECU の setter が拒否しました。いくつもの変数の書き込みでは、それより前の変数は、もう反映されています。`applied_writes` がその数、`failed_write` が拒否された書き込みの位置です（[control-api.md](../control-api.md#書き込みが反映される時点) の「書き込みが反映される時点」）。 |
| `unknown` | 反映されたかどうかが、わかりません。 |

待つときのルールは、次の 3 つです。

- 拒否された書き込みは例外になりません。`status` を確認します。例外（`ControlError`）になるのは、呼び出しそのものが失敗したときです。
- ECU が受信した書き込みでは、`c.wait(seq)` は、値が反映される時刻まで時間が進まないと戻りません。一時停止している間に呼ぶと、タイムアウト（既定 30 秒）で `ControlError` になります。
- `write()` は、自動では送信し直しません。レスポンスを受信する前に接続が切れた書き込みは、受け付けられているかもしれません。

`status` の値ごとの定義は、[internals/control.md](../internals/control.md) の「Control Agent」と「Control Server の中核」にあります。

## 値の変化を待つ

`c.watch()` は、値などの更新を、受信した順に返します。`pause_and_step.py` は、動いているシミュレーションの回転数を見て、1000 rpm に達したら一時停止します。

<!-- examples/launch_control/pause_and_step.py: an excerpt -->
```python
for update in c.watch(peers=["Engine"], events=False, operations=False):
    if isinstance(update, Values) and update.values.get("engine.speed", 0) >= 1000:
        print(f"engine.speed {update.values['engine.speed']} rpm at {update.time / 1e6:g} ms")
        break
else:
    print("FAIL the simulation ended before the engine passed 1000 rpm")
    return 1
c.sim.pause()
stands = c.sim.wait_paused()
paused_at = stands.now
```

出力の最初の 2 行（`engine.speed 1000 rpm at 190 ms` と `ok   it stands still at 190 ms`）が、ここで表示されます。`Values` は、`from vehicle_sil_control import Values` で import します。

- `watch()` が返すのは、participant の情報、値、イベント、操作の結果です。
- 値は `Values` のオブジェクトで、`.values` が dict、`.time` がその値の時刻です。
- 受信するのは最新の値で、すべての値の記録ではありません。
- シミュレーションが終わると、`for` も終わります。
- 最速（既定）で動かすと、一時停止する前にシミュレーションが終わることがあります。途中で一時停止するときは、速度を下げる（例は `Launch(..., arguments=["--speed", "0.25"])`）か、時刻 0 で一時停止した状態で起動して `c.sim.run_until()` で進めます。

`watch()` の引数と、返すものの一覧は、[control-api.md](../control-api.md) の「Python」にあります。

## 時間を止めて進める

時間の操作は `c.sim` にあります。

| すること | Python | CLI |
|---|---|---|
| 今の時間の状態を読む | `c.sim.time()` | `time` |
| 一時停止する | `c.sim.pause()` | `pause` |
| 再開する | `c.sim.resume()` | `resume` |
| 指定した長さだけ進めて、一時停止する | `c.sim.step(ms(100))` | `step 100` |
| 指定した時刻まで進めて、一時停止する | `c.sim.run_until(ms(700))` | `run-until 700` |
| 一時停止するまで待つ | `c.sim.wait_paused()` | `pause --wait` |
| 速度を変える | `c.sim.set_speed(1.0)`、最速は `c.sim.set_speed(None)` | `speed 1`、`speed max` |
| シミュレーションを停止する | `c.sim.stop()` | `stop` |

どの呼び出しも、時間の状態（`now` が時刻、`paused` が一時停止しているか、`speed` が速度）を返します。`pause_and_step.py` は、確認が終わった後、速度を最速に変えて再開し、最後まで実行します。

<!-- examples/launch_control/pause_and_step.py: an excerpt -->
```python
speed = c.sim.set_speed(None)
expect(speed.speed is None and speed.paused, "the speed is max, and it still stands still")
c.sim.resume()
```

時間を操作するときのルールは、次の 6 つです。

- `step()` と `run_until()` の後は、`c.sim.wait_paused()` で一時停止するのを待ってから、値を読みます。
- `step()` が使えるのは、一時停止している間だけです。
- `pause()` で一時停止する時刻と、`step()` で進む長さは、`vehicle_sil_launch` が決めたタイムステップの倍数に切り上げられます。そのため、一時停止した時刻は、`wait_paused()` が返す `now` で読みます。
- 一時停止している間に速度を変えても、一時停止したままです。
- 今はできない操作は `ControlError`（`code` が `FAILED_PRECONDITION`）に、引数のエラーは `INVALID_ARGUMENT` になります。
- 一時停止するのは、仮想時間で動く ECU だけです。

`--speed` を指定したときの速度と遅れの見方は、[複数の ECU をつないで動かす](integrate-ecus.md) の「等速で動かす（`--speed`）と遅れの見方」にあります。

## 終了を待つ

`launch.wait()` は、`vehicle_sil_launch` が終わるのを待ち、`run --json` と同じ結果を dict で返します。`write_before_start.py` は、再開してから、結果を確認します。

<!-- examples/launch_control/write_before_start.py: an excerpt -->
```python
expect(result["result"] == "passed" and result["simulation"] == "reached_stop_at",
       f"the run passed ({result['result']}, {result['simulation']})")
```

- 途中で停止するときは、`launch.stop()` を呼びます。
- `with Launch(...)` を抜けるときも、動いていれば停止します。
- 実行が終わると、`vehicle_sil_launch` は Control Server を閉じ、待っている呼び出し（`watch()`、`wait()`、`wait_paused()`）は、そこで終わります。
- `run` のほかの引数（`--speed`、`--disable` など）は、`Launch(..., arguments=[...])` で渡します。

結果の dict の読み方は、[simulation-run.md](../simulation-run.md) の「JSON（`run --json`）」にあります。

## 標準入力から時間を操作する

仮想時間の操作だけなら、Control Server も Python も使わずに、`vehicle_sil_launch run` の標準入力からできます。変数の読み書きはできません。`--stdin-commands` を付けて起動し、1 行に 1 つの JSON を入力します。

```sh
build/linux-gcc-control/tools/launch/vehicle_sil_launch run --start-paused --stdin-commands --stop-at-ms 1000 \
    build/linux-gcc-control/examples/ecu_main/simulation.yaml
```

`[vehicle_sil_launch] simulation running` の行が表示された後、時刻 0 で一時停止します。次の 2 行を、1 行ずつ入力します。

<!-- standard input of run --start-paused --stdin-commands: the input -->
```json
{"command":"step","ms":500}
{"command":"resume"}
```

### 確認

表示の ECU が、500 ms ごとに 1 行を表示します。1 行目の入力の後に `t=500 ms` の行が、2 行目の入力の後に `t=1000 ms` の行と結果の表が表示され、終了コード 0 で終わります。

<!-- standard input of run --start-paused --stdin-commands: the lines of the Dashboard -->
```text
[Dashboard] t=0 ms engine speed 0 rpm (0 messages)
[Dashboard] t=500 ms engine speed 1200 rpm (5 messages)
[Dashboard] t=1000 ms engine speed 1200 rpm (10 messages)
```

入力できる行は、次の 5 つです。

| 行 | すること |
|---|---|
| `{"command":"pause"}` | 一時停止します。 |
| `{"command":"resume"}` | 再開します。 |
| `{"command":"step","ms":500}` | 一時停止している間だけ使えて、500 ms 進めて、また一時停止します。 |
| `{"command":"speed","value":1}` | 速度を変えます（`"max"` で最速）。 |
| `{"command":"stop"}` | シミュレーションを停止します（結果は `stopped`、終了コード 0）。 |

- 標準入力を閉じる（端末では Ctrl+D）と、`stop` と同じです。
- JSON として読めない行は無視され、理由が標準エラーに 1 行表示されます。
- `--control` と一緒に使うと、標準入力と Control Server のどちらからも時間を操作できます。

行の形式の全体と、受け付けない場合は、[simulation-events.md](../simulation-events.md) の「標準入力のコマンド（`run --stdin-commands`）」にあります。標準入力と Control Server の両方から操作したときの順序は、[control-api.md](../control-api.md) の「標準入力とクライアントのコマンド」にあります。

## 別の PC から接続する

ここまでの手順は、`vehicle_sil_launch` と同じ PC から接続します。別の PC から接続するには、TLS の証明書とトークンが必要です。手順は、[複数の PC で動かす](multi-pc.md) の「ほかの PC から Control API を使う」にあります。

## J1939 の信号を変数として読む

Control API が扱うのは、ECU が宣言した変数と時間だけで、J1939 のフレームは扱いません。受信した信号を外から読むには、ECU が、受信した値を出力の変数にします。外から決める値は、入力の変数にして、ECU が送信する値に使います。

例では、表示の ECU が、受信した回転数を出力の変数 `dashboard.engine_speed` にしています。次は、[examples/ecu_main/dashboard/main.cpp](../../../examples/ecu_main/dashboard/main.cpp) の `Dashboard` のコンストラクターの一部です。

<!-- examples/ecu_main/dashboard/main.cpp: an excerpt -->
```cpp
node.on_message(ecu_main_example::kEngineSpeed, [this](const j1939::Message& m) {
    rpm_ = ecu_main_example::decode_speed(m.data);
    ++received_;
    last_ = m.time;
});
ecu.output("dashboard.engine_speed", &rpm_, {.unit = "rpm", .description = "the speed shown"});
```

変数を追加するのは、ECU のコードを書く人の作業です。必要な信号があれば、その ECU を書いた人に伝えます。変数の宣言の仕方は、[j1939-guide.md](../j1939-guide.md) の「外部から信号を読み書きする（UI、テスト）」と [control-api.md](../control-api.md) の「ECU の変数」にあります。

## 補足

- シナリオの実行（`--scenario`）にも `--control` で接続できますが、その間にできるのは、読む、待つ、停止するだけです。外から値を書き込むと、シナリオが同じ結果にならなくなるためです。詳細は [scenario.md](../scenario.md) の「実行中に変数を見る（`--control`）」にあります。
- GUI も、内部で `vehicle_sil_launch` の Control Server を使うので、反映される時刻と書き込みの結果のルールは Python と同じです。
- `--timeout-ms` は、一時停止している時間も数えます。
- 時間の状態を `c.watch(time=True)` で受信すると、間引かれるので、短い一時停止は現れないことがあります。一時停止を待つには、`c.sim.wait_paused()` を使います。
- 自分で起動した `vehicle_sil_launch` のアドレスをプログラムで読むときは、`serve --once` または `serve`（Events v4）のイベント `control_server` の `address` を使います。

## 次に読むもの

- 呼び出しの引数、反映される時刻、`snapshot()` のルールを確認する → [control-api.md](../control-api.md)
- 1 つの常駐ランチャーで複数のシミュレーションを順に動かす（`Serve` と `RunHandle`） → [control-api.md](../control-api.md#1-つのランチャーで順に実行するserverunhandle) の「1 つのランチャーで順に実行する（`Serve`、`RunHandle`）」と [serve_runs.py](../../../examples/launch_control/serve_runs.py)
- 同じ手順を、終了コードで合否の出るテストにする → [シナリオでテストを書く](scenario-tests.md)
- `serve` / `serve --once` の Events v4 ストリームと `run --json` の結果を、プログラムから読む → [simulation-events.md](../simulation-events.md)
