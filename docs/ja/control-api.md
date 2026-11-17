# Control API の使い方

[English](../control-api.md)

Control API は、実行中のシミュレーションを外から操作・監視する gRPC の API です（`proto/vehicle_sil/control/v1/control.proto`）。このドキュメントは、起動のしかた、認証、ECU の変数の宣言、呼び出しの一覧、Python のクライアントと CLI、時間の操作のルールのリファレンスです。手順を追って試すときは、ガイド「[信号を外から操作する](howto/operate-signals.md)」から始めます。

| 調べたいこと | 節 |
|---|---|
| クライアント、ランチャー、ECU のつながり | [全体の構成](#全体の構成) |
| Control Server を持つランチャーのビルド | [ビルド](#ビルド) |
| `--control` のアドレス、サーバーが起動する時点と終わる時点 | [ランチャーに接続する](#ランチャーに接続する) |
| 最初のステップの前に値を書く | [始まる前に書く（`--start-paused`）](#始まる前に書く--start-paused) |
| 時間のコマンドの順序とルール | [標準入力とクライアントのコマンド](#標準入力とクライアントのコマンド) |
| TLS、トークン、ロール | [認証](#認証) |
| ECU のコードでの変数の宣言 | [ECU の変数](#ecu-の変数) |
| gRPC の呼び出しと、Python、CLI、ロールの対応 | [呼び出しの一覧](#呼び出しの一覧) |
| Python のクライアント | [Python](#python) |
| 時刻と値を同じ瞬間のものとして読む | [時刻と値をまとめて読む（`snapshot()`）](#時刻と値をまとめて読むsnapshot) |
| コマンドラインから使う | [CLI](#cli) |
| 状態コードと例外 | [エラー](#エラー) |
| できないこと、上限 | [制限](#制限) |

## 全体の構成

ECU が、外から読み書きする変数を宣言します。クライアント（Python、CLI、GUI）は、Control Server の API で、その変数と仮想時間を操作します。Control Server は、シミュレーションを実行する `vehicle_sil_launch run` に `--control` を付けると、ランチャーの中で起動します。

| 区間 | 通信 |
|---|---|
| クライアントとランチャー（Control Server） | gRPC |
| ランチャーと ECU | SIL Kit |

- `ecu_main` で作った ECU は、変数を宣言していれば、何も書かなくてもランチャーから読み書きできます。ECU の実行ファイルは gRPC を持ちません。
- ECU の実行ファイルの変数の名前と型は、`--describe` の `variables` で調べられます（[ecu-executable.md](ecu-executable.md) の「JSON の形式」）。
- ECU をランチャーに起動させずに、自分で起動する（デバッガーから動かす、別の端末で起動する）ときも、同じランチャーに接続します。設定ファイルの participant に `manual: true` を付けるか、`run --manual <name>` を使います（[simulation-config.md](simulation-config.md) の「participant を自分で起動する」）。
- 保証の詳細（反映の時刻、順序、上限、スレッド）は [internals/control.md](internals/control.md) にあります。

## ビルド

```sh
cmake --preset linux-gcc-release -DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON
cmake --build --preset linux-gcc-release
```

このビルドの `vehicle_sil_launch` は `--control` を持ち、`vehicle_sil_launch version --json` の `features` に `control-server` があります。持たないビルドで `--control` を付けると、引数のエラー（終了コード 4）で、ビルドし直す方法を標準エラーに表示します。

| 項目 | 内容 |
|---|---|
| gRPC が見つからないとき | FetchContent で取り込みます（初回はダウンロードとビルドに数分かかります）。 |
| Windows（MSYS2 UCRT64）の gRPC | `pacman -S mingw-w64-ucrt-x86_64-grpc` でインストールしておきます。 |
| Control Server のテストまでまとめて実行する | プリセット `linux-gcc-control`（GCC の Debug でこの設定）を `scripts/verify.py linux-gcc-control` で使えます（[build.md](build.md) の「Linux」）。 |
| 依存ライブラリの詳細 | [dependencies.md](dependencies.md) にあります。 |

## ランチャーに接続する

```sh
vehicle_sil_launch run --events --stdin-commands --start-paused --control 0 simulation.yaml
```

`--control <address>` を付けると、ランチャーが Control Server を起動します。`<address>` は次のどれかです。

| `<address>` | 待ち受ける所 |
|---|---|
| ポート番号だけ | 127.0.0.1 のそのポートです（0 なら空いているポート）。 |
| `<host>:<port>` | そのアドレスです。 |
| `unix:<path>` | そのパスのソケットです。 |

`unix:` のパスには OS の上限があります（Linux では 107 バイト。`%20` のような `%XX` は、表す 1 バイトとして数えます）。長すぎると、ランチャーは待ち受ける前に、長さと上限を表示して引数のエラー（終了コード 4）で終わります（`the path of the socket is ... bytes long and this system takes ... at most: give a shorter path, for example unix:/tmp/vehicle-sil-control.sock`）。`/tmp` の下などの短いパスにします。

| 項目 | 内容 |
|---|---|
| 実際のアドレス（ポート 0 なら選ばれたポート） | イベント `control_server` の `address` と、人向けの出力の `[vehicle_sil_launch] control server at <address>` の行で通知します。 |
| 起動する時点 | 照合が成功した後、registry より前です（照合で失敗した実行では起動しません）。 |
| 起動できなかった（ポートが使われているなど） | 何も起動せずに、設定のエラー（`setup`、終了コード 3）です。 |
| サーバーが扱う範囲 | そのランチャーの実行だけです。 |
| 扱うもの（ECU の変数） | Peer の一覧、値、書き込み、操作の結果、購読です。 |
| 扱うもの（仮想時間） | 一時停止、再開、ステップ実行、指定時刻まで、速度、停止です。 |
| Peer の一覧 | 設定ファイルの必須の participant は、Agent がなくても一覧にあります。 |

例は [examples/launch_control](../../examples/launch_control) にあります。

| 例 | 内容 |
|---|---|
| `write_before_start.py` | Python から起動して接続し、始まる前に書いて、読み返して確認します。 |
| `pause_and_step.py` | 動いている途中で止め、時刻を指定して書き、`snapshot()` で時刻と値をまとめて読んで確認し、速度を変えて最後まで動かします。 |

### 実行が終わるとき

実行が終わると（通常の終了、停止の要求、`--timeout-ms`、participant の失敗、どれでも）、ランチャーは participant を終わらせてから、結果を出力する前にサーバーを閉じます。

| 項目 | 内容 |
|---|---|
| 待っている呼び出し（`Watch`、`WaitOperation`、`WaitPaused`） | サーバーを閉じる時点で終わります（`Watch` はストリームの終わり、`WaitPaused` は `FAILED_PRECONDITION` か `CANCELLED`）。 |
| クライアントが接続したままのとき | ランチャーはクライアントを待たないので、終了は遅れません。 |
| シミュレーションが終わってからサーバーを閉じるまでの間 | 終わった時点の値を読めます。 |

### シナリオの実行中

シナリオの実行（`--scenario`）では、クライアントができるのは、読むこと、待つこと、止めることだけです。

| 呼び出し | シナリオの実行中 |
|---|---|
| 読む・待つ呼び出し、`Stop` | いつもどおりです。 |
| 書く呼び出し（`Write`、`ForgetPeer`） | `FAILED_PRECONDITION` で拒否します。 |
| 時刻を動かす呼び出し（`Pause`、`Resume`、`Step`、`RunUntil`、`SetSpeed`） | `FAILED_PRECONDITION` で拒否します。 |

拒否の理由は、trailing metadata の `vehicle-sil-reason`（値は `scenario-running`）で見分けます。呼び出しごとの表と保証は [scenario.md](scenario.md) の「実行中に変数を見る」にあります。

### 外につながる実行

シミュレーションの外につながる実行（`--connect`、または有効な `role: bridge` の participant）は、`--time-policy free` で始めなければ、実時間を保ちます（[simulation-run.md](simulation-run.md) の「外につながる実行の時間」）。

| 呼び出し | 実時間を保つ実行 |
|---|---|
| `Pause`、`Step`、`RunUntil`、1 以外の速度の `SetSpeed` | `FAILED_PRECONDITION` で拒否します。trailing metadata の `vehicle-sil-reason` は `outside-real-time` です。 |
| `Resume`、速度 1 の `SetSpeed`、`Stop` | いつもどおりです。 |
| 読む・待つ・書く呼び出し | いつもどおりです（この規則は時刻だけのものです）。 |

`--time-policy free` では、どの呼び出しもいつもどおりです。止める、ステップで進める、速度を変える最初の呼び出しの後、実行は注記 `outside-time-changed` を付けて終わります。

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

書いたら、`wait(seq, until="queued")` で全員が受け取ったことを確認してから進めます。受け取る前に進めると、時刻 0 に間に合わないことがあります。

止まっている間の状態は、次のとおりです。

| 見る所 | 値 |
|---|---|
| `GetTime` | `now` は 0、`paused` は真、`running` は偽（まだ始まっていない）、`ended` は偽です。 |
| `WaitPaused` | すぐに 0 を返します。 |
| イベントの `phase` | `waiting` のままです。 |
| ランチャーの出力 | `standing still before the first step until a resume, step or run_until command ...` の行を表示します。 |

止まっている間に受け付けたコマンドの動作は、次のとおりです。

| コマンド | 動作 |
|---|---|
| `resume` | 始まります。 |
| `step`、`run_until` | 0 から進んで、その時刻で止まります。 |
| `run_until` 0 | 時刻 0 のステップを実行して、0 で止まります（`--control` なしの `--start-paused` と同じ止まり方）。 |
| 時刻を指定した `pause`（0 より後） | `run_until` と同じく進みます。 |
| `pause`（時刻なし）、`speed` | 進みません（`speed` は、進んだ後の速度になります）。 |
| 停止の要求（Ctrl+C、`stop` のコマンド、標準入力の終わり、クライアントの `Stop`） | 待つのをやめ、時刻 0 のステップを 1 つ実行してから止まります。 |

止まるより前（照合や、participant がそろうのを待つ間）に受け付けたコマンドも有効です。止まるのは、止まる時点で、0 で止まることになっているときだけです。

| 止まるより前に受け付けたコマンド | 動作 |
|---|---|
| `resume` | 止まらずに始まります。 |
| `step`、`run_until` | 止まらずに 0 から進んで、その時刻で止まります（止まってから受け付けたのと同じ結果）。 |
| その後の `pause` | また止まります。 |

標準入力から来たコマンドも同じです。

止まっている時間と、タイムアウトの関係は次のとおりです。

| 設定 | 止まっている時間の扱い |
|---|---|
| なし | 止まっている時間そのものに上限はなく、停止の要求が来るまで待ちます。 |
| `start_timeout_ms` | 止まっている間は数えません。 |
| `--timeout-ms` | 止まっている時間も数えます（一時停止している時間を数えるのと同じです）。 |

- `start_timeout_ms` は、止まるまで（全員がそろい、変数を出す ECU と接続するまで）と、進んだ後（全員が最初のステップに達するまで）に、それぞれ適用されます。
- 止まるまでか進んだ後に participant が応答しなくなると、エラー `simulation-start` になります（達していない participant と状態を、メッセージに挙げます）。
- クライアントが来ないまま `--timeout-ms` が過ぎると、時刻 0 のステップを実行してから中断し、エラー `timeout` になります（メッセージに、最初のステップの前で止まっていたことを書きます）。
- 進めた後に `--timeout-ms` が過ぎたときも、メッセージに、最初のステップの前で止まっていた時間を書きます。
- 待っている間に必須の participant がいなくなると（プロセスが終わったなど）、すぐに待つのをやめて、始まらなかった実行として終わります（`ended-before-start`）。

`--control` なしの `--start-paused`（`--stdin-commands` だけ）は、時刻 0 のステップを実行してから 0 で止まります。この場合と、一時停止してから書く場合は、`at` を止まっている時刻より後にします（下の「[書き込みのルール](#書き込みのルール)」）。

### 標準入力とクライアントのコマンド

時間のコマンドは、標準入力（`--stdin-commands`）とクライアントの両方から受け付けます。どちらも同じ経路を通り、ルールも同じです（`step` は一時停止している間だけ、など）。

| クライアントの呼び出し | コマンドの名前 | 引数 |
|---|---|---|
| `Pause` | `pause` | 時刻つきなら `at_ms` |
| `Resume` | `resume` | なし |
| `Step` | `step` | `ms` |
| `RunUntil` | `run_until` | `at_ms` |
| `SetSpeed` | `speed` | 速度 |
| `Stop` | `stop` | なし |

| 項目 | ルール |
|---|---|
| 処理の順 | 1 つずつ順に処理します。 |
| 1 つのコマンドの処理 | 受け付けるかを決め、イベント `command` を出力し、時間ゲートに渡すまでを、ほかのコマンドをはさまずに行います。 |
| 有効になった順 | イベントの順です。 |
| どちらから来たか | イベントの `source` でわかります（`stdin`、`eof`、`control_server`）。 |
| 標準入力とクライアントが同時にコマンドを送った | どちらが先かは、受け取った順です。 |
| 時刻の丸め | ミリ秒に切り上げ、さらに controller のタイムステップに切り上げます（イベントの値が、実際に止まる時刻です）。 |

たとえば、クライアントの `step` と標準入力の `resume` が重なると、イベントの順で、止まるか進み続けるかが決まります。

クライアントへのレスポンスは、受け付けなければ gRPC のエラーで、メッセージはイベントの `message` と同じです。

| 受け付けない理由 | 状態コード | trailing metadata の `vehicle-sil-reason` |
|---|---|---|
| 引数のエラー（範囲の外の速度、0 以下のステップ） | `INVALID_ARGUMENT` | ありません。 |
| 今はできないコマンド | `FAILED_PRECONDITION` | イベントの `reason` です（`not-paused`、`not-running`、`outside-real-time` など。[simulation-events.md](simulation-events.md) の「理由のコード」）。 |

`Stop` は、標準入力の `{"command":"stop"}` と同じです。

| 項目 | `Stop` の動作 |
|---|---|
| 1 回目と 2 回目 | 1 回目は順に止め、2 回目は待たずに participant を終わらせます。 |
| 結果の `interrupted_by` | `"control server stop"` です。 |
| 理由の文 | 監査ログの行にだけ残ります。 |
| シミュレーションが終わった後（始まらなかったときも） | 時間のコマンドと同じく拒否し（`accepted: false`、`FAILED_PRECONDITION`）、結果は変わりません。 |
| イベントの順 | 時間のコマンドと同じ順に並びます。 |

### 認証

認証が必要かは、待ち受けるアドレスで決まります。

| アドレス | 認証 |
|---|---|
| ループバック（ポート番号だけ、`localhost:<port>`、`[::1]:<port>`、数字の `127.x.y.z:<port>`、`unix:<path>`） | `--control-credentials` を付けなければ、認証はありません。 |
| ほかの PC から接続できるアドレス（`0.0.0.0:<port>` など） | TLS と認証情報が必要です（`--control-tls-cert`、`--control-tls-key`、`--control-credentials`）。 |

- 認証のないループバックには、同じ PC の誰でも接続できます。`unix:` のソケットは、ファイルの権限しだいで、同じ PC のほかの利用者も接続できます。
- ほかの PC から接続できるアドレスで、TLS か認証情報が足りなければ、引数のエラー（終了コード 4）です。

認証情報のファイルには、1 行に 1 つ、`<role> <name> <token>` を書きます。

```text
# role     name     token
viewer     guest    5e07d2...（長い乱数）
operator   tester   0f3c9a...
admin      lead     7b21e4...
```

| 項目 | ルール |
|---|---|
| `role` | `viewer`、`operator`、`admin` のどれかです。 |
| `token` | 16 文字以上の、秘密の文字列です。 |
| コメント | `#` で始まる語から行末までです。 |
| ECU | トークンを使いません。 |
| 前のバージョンの `agent` の行 | 読み込みのエラーです（メッセージのとおり、その行を消します）。 |
| 1 件もないファイル（空、コメントだけ） | 引数のエラーです（「付けたのに誰でも接続できる」を避けるため）。 |
| エラーのメッセージ | 行番号と、何が違うかだけです（行の語は、トークンかもしれないので表示しません）。 |
| トークンを引数に書く | できません（ランチャーの引数は、イベント `hello` の `arguments` や、プロセスの一覧に表示されるため）。 |

呼び出しに必要なロールは、次のとおりです（呼び出しごとの対応は[呼び出しの一覧](#呼び出しの一覧)）。

| ロール | できること |
|---|---|
| `viewer` | 読むだけです。 |
| `operator` | 時間の操作と書き込みです。 |
| `admin` | `Stop` です（`Stop` を呼べるのは `admin` だけです）。 |

`--control-audit-log <file>` を付けると、誰が何を変えたかをファイルに追記します。書き込めなくなっても実行は続き、その行は失われます。

ECU とランチャーの間（SIL Kit）には、認証がありません。registry に参加できる者は、ECU に書き込めます。閉じたネットワークで使います。

## ECU の変数

ECU のクラス（`Ecu&` を受け取るクラス）が、コンストラクターで変数を宣言します。宣言は ECU の Control Agent（`Ecu::agent()`。ECU に 1 つ）が保持します。SIL Kit の `Peer` でも、ローカルの `Simulation()` と `LocalBench` でも、同じコードが同じように動きます。

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

### 宣言の関数

| 関数 | 宣言するもの |
|---|---|
| `input(name, &value, options)` | 外から書き込める変数です。 |
| `input(name, get, set, options)` | 外から書き込める変数で、setter が値の保存以外もするときに使います。 |
| `input_enum(name, &value, items, options)` | 外から書き込める、列挙の変数です。 |
| `output(name, &value, options)` | 外から読むだけの変数です。 |
| `output(name, get, options)` | 外から読むだけの変数で、値を getter で返します。 |
| `output_enum(name, get, items, options)` | 外から読むだけの、列挙の変数です。 |

型は、bool、整数、実数、列挙です。値は ECU のステップのスレッドで読み書きするので、モデルにロックは不要です。

| `VariableOptions` のメンバー | 意味 |
|---|---|
| `unit` | 単位です。 |
| `min`、`max` | 範囲です。 |
| `step` | 値の刻み幅です。 |
| `description` | 説明です。 |
| `parameter` | 真にすると、最初のステップの前だけ書ける変数（Parameter）になります。 |

| 宣言のルール | 内容 |
|---|---|
| 宣言できる時点 | 実行の前（ECU の組み立ての中）だけです。 |
| 開始後の宣言 | `std::logic_error` です（メッセージに ECU の名前があります）。 |
| 同じ ECU での同じ名前 | `std::invalid_argument` です（メッセージに ECU の名前があります）。 |
| ポインターと関数が指すもの | ECU が持つもの（`Simulation::add()` が作るクラス、`emplace()` したオブジェクト）にします。 |
| 破棄の順 | ECU は Agent を、ネットワークの後、`emplace()` したオブジェクトの前に破棄します。 |

### Agent の設定（`set_agent_options()`）

`set_agent_options()` で、読み出しの周期（`sample_period`、既定 10 ms、0 で毎ステップ）とキューの大きさを選べます。呼べるのは、最初の `input()`、`output()`、`agent()` より前だけです。不正な値は、その場で `std::invalid_argument` です（メッセージに ECU の名前があります）。

どの ECU にも Agent があります。変数を宣言しなくても、どの ECU にもある入力 `ecu.connected` を持つためです。そのため、変数を宣言しない ECU でも、ステップごとに Agent の処理（書き込みの反映と、`sample_period` ごとの値の読み出し）が動きます。`ecu_main` の ECU は、`sample_period` ごと（既定 10 ms）に値のメッセージを 1 通送信します。`Peer` では、監視の出力（`PeerOptions::monitor`）も付きます。

### 書き込みが反映される時点

外からの書き込みは、指定した仮想時間以後の最初のステップで、アプリケーションの処理の前に反映されます。そのステップの J1939 の受信処理やコンポーネントのタイマーは反映より前に動くので、それらが新しい値を使うのは次のステップからです。値は、アプリケーションと LIN の処理の後に読み出します。SIL Kit とローカルで同じ時点です。

書き込みは、いまの値と比べません。同じ値の書き込みも 1 つずつ反映し、そのたびに setter を呼びます（同じステップに 2 つあれば、(時刻、`seq`) の順に 2 回）。そのため、「押すたびに 1 回」のような入力は、いつも false と読める getter と、呼ばれた回数を数える setter で書けます。

1 つの ECU へのいくつもの書き込みをまとめたコマンド（シナリオの、同じ participant の変数をいくつも書く 1 つの `set`。または 1 回の `Write`）は、「全部か無か」ではありません。

| 書き込みが失敗する時点 | 反映されるもの |
|---|---|
| Agent がコマンドを受け取ったときの確認（名前、型、範囲、書き込めるか）で失敗: `invalid` | そのコマンドの書き込みは、どれも反映されません。 |
| ECU のステップで setter が例外を投げた: `rejected` | コマンドの中でそれより前の書き込みは、もう反映されていて、そのまま残ります。その書き込みと後ろの書き込みは反映されません。 |

ECU ごとの結果は、どの書き込みが反映されたかを返します。ECU の書き込みは、リクエストのうちその ECU への書き込みを、書いた順に 0 から数えたものです。

| 項目（gRPC の `TargetResult`、Python の `Target`） | 意味 |
|---|---|
| `applied_writes` | 先頭から数えて、反映された書き込みの数。`applied` は全部、`rejected` は setter が例外を投げた書き込みの位置、`invalid`、`late`、`cancelled`、`busy` は 0。 |
| `failed_write` | `reason` が言っている書き込みの位置。確認で失敗した書き込み（`invalid`）か、setter が例外を投げた書き込み（`rejected`）。 |

わからないときは空です（gRPC では設定なし、Python では `None`）。書き込みが確定する前、`unknown` のとき、`control.proto` の `TargetResult` のコメントに挙げた場合です。どの書き込みが反映されたかは、`reason` ではなく、この 2 つで判断します（`reason` は人が読むための文です）。setter は取り消せないので、Agent は前の書き込みを元に戻しません。いくつかの値を「全部か無か」で反映したいときは、1 つの setter でまとめて確かめるか、ECU の側で使う前に確かめます。

| 書き込む方法 | ECU が受け取る数 |
|---|---|
| シナリオの `set`、Control API の `Write`、`agent().submit()` | 書いた数だけです。 |
| GUI のコンポーネント（`client.variables.bind`） | 速く続けた書き込みは、数が減ります（[gui.md](gui.md)）。 |

GUI のコンポーネントは、サーバーが前の書き込みを受け取るまでの間に来た値を、最後の 1 つにまとめます。

### 接続のしかた

| ECU の作り方 | Control Server との接続 |
|---|---|
| `ecu_main` の ECU | 自動で接続します。 |
| 自分で書いた `Peer` | `AgentTransport` を作って接続します（下の文）。 |
| ローカル（`Simulation()`、`LocalBench`）の ECU | 接続しません（同じプロセスから `agent()` で読み書きします）。 |

ランチャーの Control Server に接続するのは、SIL Kit の participant の ECU だけです。自分で書いた `Peer` は、ECU が変数を宣言した後、`start()` の前に `vehicle_sil::control_silkit::AgentTransport transport(peer.agent(), peer.runtime());` を作って接続します（`peer.stop()` の後、`Peer` より先に破棄します）。

同じプロセスからは、`ecu.agent().submit()` で書き込み、`latest()` で読めます。`Simulation::add<App>()` で追加した ECU の Agent は、`sim.ecu(name).agent()` で取得できます（例: `examples/vehicle`）。

## 呼び出しの一覧

gRPC の呼び出しと、Python のクライアント（`Control` のメソッド。`sim.` は `Control.sim` のメソッド）、CLI のコマンド、必要なロールの対応です。

| gRPC | すること | Python | CLI | ロール |
|---|---|---|---|---|
| `Ping` | サーバーのバージョンを返します。 | `ping()` | なし | `viewer` |
| `ListPeers` | Peer と変数の一覧を読みます。 | `peers()`、`peer()` | `peers`、`show` | `viewer` |
| `GetValues` | 1 つの Peer の最後の値を読みます。 | `values()` | `show` | `viewer` |
| `GetSnapshot` | 時刻と値をまとめて読みます。 | `snapshot()` | なし | `viewer` |
| `Watch` | 値、イベント、操作などを購読します。 | `watch()` | `watch` | `viewer` |
| `GetTime` | 時間の状態を読みます。 | `sim.time()` | `time` | `viewer` |
| `WaitPaused` | 止まるのを待ちます。 | `sim.wait_paused()` | `pause --wait`、`step`、`run-until` | `viewer` |
| `GetOperation` | 書き込みの結果を読みます。 | `operation()` | なし | `viewer` |
| `WaitOperation` | 書き込みの結果を待ちます。 | `wait()` | `set`（`--wait`） | `viewer` |
| `Write` | 変数に書き込みます。 | `write()` | `set` | `operator` |
| `ForgetPeer` | 切断された Peer を消します。 | `forget_peer()` | `forget` | `operator` |
| `Pause` | 一時停止します。 | `sim.pause()` | `pause` | `operator` |
| `Resume` | 再開します。 | `sim.resume()` | `resume` | `operator` |
| `Step` | 指定した時間だけ進めて止めます。 | `sim.step()` | `step` | `operator` |
| `RunUntil` | 指定した時刻まで進めて止めます。 | `sim.run_until()` | `run-until` | `operator` |
| `SetSpeed` | 速度を変えます。 | `sim.set_speed()` | `speed` | `operator` |
| `Stop` | シミュレーションを止めます。 | `sim.stop()` | `stop` | `admin` |

## Python

```sh
uv sync                                         # .venv に grpcio、grpcio-tools、protobuf などを入れる
source .venv/bin/activate                       # または、各コマンドの前に uv run を付ける
python -m vehicle_sil_control.generate          # スタブを python/vehicle_sil_control/_generated に作る
export PYTHONPATH=$PWD/python
```

| 項目 | 内容 |
|---|---|
| Python のバージョン | 3.11 以上です。 |
| バージョンと依存の指定 | リポジトリのルートの `pyproject.toml`（`control` グループ）と `uv.lock` です（[build.md](build.md) の「Python（uv）」）。 |
| uv を使わない場合 | `pip install grpcio grpcio-tools protobuf` でインストールします。 |
| uv を使わない場合（MSYS2 UCRT64） | `pacman -S mingw-w64-ucrt-x86_64-python-grpcio mingw-w64-ucrt-x86_64-python-grpcio-tools` でインストールします。 |

### ランチャーを起動する（`Launch`）

ランチャーは、`Launch` で子プロセスとして起動できます。`vehicle_sil_launch run --events --stdin-commands --control <address>` を起動し、イベントを別のスレッドで読みます（標準ライブラリだけで、gRPC は不要です）。

```python
from vehicle_sil_control import Control, Launch, ms

with Launch("build/linux-gcc-release/tools/launch/vehicle_sil_launch", "simulation.yaml", start_paused=True) as launch:
    with Control(launch.address) as c:   # address はイベント control_server を待って返す
        ...
    launch.stop()                        # {"command":"stop"}（with を出るときも、動いていれば止める）
    print(launch.wait()["result"])       # run --json と同じ結果
```

`Launch(launcher, config, address="0", start_paused=False, arguments=(), cwd=None, stderr=None, keep_logs=True)` の `arguments` は、`run` のほかの引数（`--speed`、`--disable`、`--control-credentials` など）です。

| メンバー | 内容 |
|---|---|
| `address` | Control Server のアドレスです（イベント `control_server` を待って返します）。 |
| `events` | 読んだイベントの dict の一覧です。 |
| `result` | 結果です。 |
| `wait_for_event(event, timeout, after=None, **keys)` | キーの値が合う最初のイベントを待ちます。 |
| `send(command)` | 標準入力にコマンドを 1 行送信します。 |
| `stop()` | `{"command":"stop"}` を送信します（`with` を出るときも、動いていれば止めます）。 |
| `wait(timeout)` | ランチャーが終わるのを待って、結果を返します。 |
| `returncode` | ランチャーの終了コードです。 |

ランチャーが結果を出力せずに終わったときと、待つイベントが来ないまま実行が終わったときは、`LaunchError` です（メッセージに、結果とエラーの指摘があります）。

自分でランチャーを起動するときは、`control_address(lines)` が、イベントの行（文字列か dict）から最初の `control_server` の `address` を返します。

### 接続して読み書きする（`Control`）

別の PC から、TLS とトークンを付けたランチャー（上の「認証」）にも、同じように接続します（証明書の作り方と、接続できないときのメッセージは [howto/multi-pc.md](howto/multi-pc.md)）。

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

時刻は、仮想時間のナノ秒です（`ms()`、`s()` で作ります）。

| 起きたこと | Python での現れ方 |
|---|---|
| 呼び出しの失敗 | 例外 `ControlError` です（[エラー](#エラー)）。 |
| Agent が拒否した書き込み | 例外ではなく、操作の結果（`invalid` など）になります。 |

### 書き込みのルール

| 項目 | ルール |
|---|---|
| 止めてから書くとき | `at` を、止まっている時刻（`c.sim.time().now`）より後にします（たとえば 1 ns 後）。 |
| 送り直し | `write()` は、自動では送り直しません。 |
| 複数のクライアントが、同じ変数に同じ時刻で書いた | 残る値は、Server が受け取った順で決まります。 |

- 止まっている時刻のステップは、すでに実行した Peer があります。そのため、その時刻か `at` なしの書き込みは、`late` になるか、反映のステップが実行ごとに変わることがあります。
- 例外は、ランチャーの `--start-paused` で最初のステップの前に止まっているときです（上の「始まる前に書く」）。
- レスポンスを受け取る前に接続が切れた書き込みは、受け付けられているかもしれません（同じ書き込みを送り直すと、別の操作になります）。

### 時間の状態を読む

| したいこと | 使うもの |
|---|---|
| 止まるのを待つ | `c.sim.wait_paused()`（達して止まった状態は、このレスポンスで読みます） |
| 時刻と、複数の Peer の値を、同じ瞬間のものとして読む | `c.snapshot(peers=())`（下の「時刻と値をまとめて読む」） |
| 時間の状態を購読する | `watch(time=True)`（約 100 ms ごとの最新で、短い一時停止は現れないことがあります） |

- `c.sim.time()` と `c.values()` を別々に呼ぶと、その間にシミュレーションが進むことがあり、2 つの間に保証はありません。
- `step()`、`run_until()`、時刻つきの `pause()` のレスポンス（`TimeState`）と、その直後の `time()` は、コマンドを受け付けた後のその時点の状態です。
- 進んでいる途中なら、`paused` は偽で、`now` は進む前か途中の時刻です（速ければ、もう達して止まっていることもあります）。
- `TimeState.running` が偽のとき、`ended` が真なら終わっていて、偽ならまだ始まっていません。

### Peer と変数の情報（`peers()`、`watch()`）

`peers()` と `watch()` は、同じ Peer を同じ内容で返します。

| Peer のフィールド | 意味 |
|---|---|
| `has_agent` | Agent が接続したことがあるかです（既定は偽）。 |
| `simulation_state` | SIL Kit の状態です。 |
| `virtual_time` | Agent がシミュレーションの時刻で動くかです（Agent の hello のとおり。偽は実時間の Peer か、Agent がない）。 |

`virtual_time` を持たない前のバージョンの Server では、どの Peer も偽になります。

| 変数（`schema` の各要素）のフィールド | 意味 |
|---|---|
| `builtin` | vehicle-sil が付けた変数か（真）、アプリケーションが宣言した変数か（偽）です。 |
| `source` | 設定ファイルの対応表（`participants[].variables`）が適用されている変数の、実行ファイルでの名前です（適用されていなければ空）。 |

- `builtin` は、Server が名前で決めます（名前が `ecu.`、`peer.`、`j1939.` で始まれば真）。アプリケーションが `j1939.` で始まる名前の変数を宣言すると、それも真になります。
- `source` を持たない前のバージョンの Server でも、`source` は空です。

対応表のある Peer では、次のものが対応の後のものになります。

| 項目 | 対応表のある Peer |
|---|---|
| `schema` の名前、型、範囲、単位、読む値、書く値 | どれも、対応の後のものです。 |
| `builtin` | 対応の後の名前で決まります。 |
| 書き込みを拒否する理由の文 | 対応の後の名前で書きます。 |
| ECU の側が拒否したときの文 | `engine.speed (the executable's rpm_x10): ...` の形で、値は実行ファイルの値です。 |
| 実行ファイルの変数に合わなかった対応 | その対応の名前（共通の名前と実行ファイルでの名前）への書き込みは `invalid` で拒否され、理由に、合わなかった訳が入ります（値は実行ファイルの名前のまま読めます）。 |

### シミュレーションの状態（`watch(simulation=True)`）

シミュレーションの状態の購読は、`SimulationState` を返します。

| `SimulationState` のフィールド | 意味 |
|---|---|
| `participants` | 必須の participant ごとの SIL Kit の状態です。 |
| `failed_participant`、`failure_reason` | 失敗した participant と理由です。 |
| `ended` | 終わったかです。 |
| `error` | Server が状態を読めなかったときの理由です（状態は前の値のままです）。 |

- クライアントが遅くて Server が途中の状態を保持しきれなかったときは、捨てた数が gRPC の `Update.dropped` に入ります。Python の `watch()` はこの数を返しません。失敗と終わりは後の状態に残るので、失われません。
- `peers()` も、Server が状態を読めなかったときは、呼ぶたびに `ControlWarning` で通知します（同じ箇所からの 2 回目以降も通知します）。表示するか、例外にするか、無視するかは利用者の警告フィルターのとおりで、ライブラリはフィルターを変えません。

### 時刻と値をまとめて読む（`snapshot()`）

```python
c.sim.step(ms(100)); c.sim.wait_paused()
snap = c.snapshot()                       # 全 Peer。c.snapshot(["Engine", "Body"]) で絞る
if snap.consistent:                       # 値は、止まっている時刻 snap.time.now のもの
    print(snap.time.now, snap.values["Engine"].values["engine.speed"])
```

`snapshot()`（gRPC の `GetSnapshot`）は、時間の状態（`sim.time()` と同じ `TimeState`）と、Peer ごとの最後の値（`values()` と同じ `Values`）を、Server が 1 回で読んで返します。Server は、時間の状態を先に読み、その後で全 Peer の値を同じ瞬間に読みます。

保証は、シミュレーションの時刻で動く Peer について、Server が時間を操作できる（`time.available`）ときのものです。

| `consistent` | 保証 |
|---|---|
| 真でも偽でも（動いている間も） | 値は時刻より古くありません。`time.now` より前に始まった各 Peer のステップの値は、すべて入っています。 |
| 真 | 時刻は `time.now` で止まっていて（`time.paused`）、どの値も `time.now` より後のステップのものではありません。 |
| 偽 | 動いている、終わっている、呼び出しの間に進んで後のステップの値が入った、のどれかです（値と時刻は、偽でも入っています）。 |

- 値のほうが時刻より新しいことはあります（読む間にシミュレーションが進んだ分）。
- `consistent` が真のとき、値は、止まっている時刻のものです（`time.now` より前のステップは全部、後のステップは 1 つもない）。
- 止まるのを待って（`sim.wait_paused()`）から呼び、その間にだれもシミュレーションを進めなければ、`consistent` は真です。
- ちょうど `time.now` に始まるステップの値は、入っているかどうか決まりません（止まっている間に受信します）。どちらかは `Values.time` でわかります。
- そのため、止まった直後は、同じ時刻でも、ちょうどその時刻のステップの値を受信する前と後の 2 通りのレスポンスがありえます（どちらも `consistent` は真）。

前提と限界は、次のとおりです。

| 項目 | 内容 |
|---|---|
| 保証の前提 | ECU が値をステップの中から送信していることです（`ecu_main` で作った ECU は、そうします）。 |
| 前提が破れたとき | ランチャーが警告 `control-link-late-values` を表示し、`consistent` が真でも、値が欠けていることがあります。 |
| 「ステップの値」 | その時刻より前に読んだ最後のサンプルです（Agent が読むのは `sample_period` ごとで、ステップより長いことがあります）。 |
| 実時間の Peer（`peers()` の `virtual_time` が偽）の値 | 保証がありません（`consistent` は、その値を見ずに決めます）。 |
| 切断された Peer | 最後の値が、そのまま入っています（接続しているかは `c.peers()` で確認します）。 |

`values` に入る Peer は、次のとおりです。

| 項目 | 内容 |
|---|---|
| Python の `values` | 値を持つ Peer だけが入ります（まだ値のない Peer、Agent のない participant、知らない名前は入りません）。 |
| gRPC のレスポンス（`GetSnapshotReply.values`）で、名前を挙げたとき | 挙げた順に 1 つずつです（同じ名前は 1 回）。 |
| gRPC のレスポンスで、名前を挙げなかったとき | Agent を持つ Peer を、名前の順に並べます。 |
| 値のない Peer | `snapshot` がありません。 |
| Agent のない名前 | `incarnation` も空です。 |
| 値の並び | その `incarnation` のスキーマ（`ListPeers`）の順です。 |

| 項目 | 内容 |
|---|---|
| ロール | `viewer` で呼べます。 |
| 1 回に挙げられる名前 | 1024 個までです（超えると `INVALID_ARGUMENT`）。 |
| レスポンスが 1 つのメッセージの上限（4 MiB）を超える | `RESOURCE_EXHAUSTED` です。メッセージのとおり、Peer を分けて呼びます（分けた呼び出しどうしの間に保証はありません）。 |
| この呼び出しを持たない前のバージョンの Server | `ControlError`（`code` は `UNIMPLEMENTED`）になります。そのときは `sim.time()` と `values()` を使います（2 つの間に保証はありません）。 |

## CLI

`--server`（既定は `VEHICLE_SIL_CONTROL_SERVER`）に、ランチャーのイベント `control_server` の `address`（人向けの出力では `control server at` の行）を渡します。コマンドと gRPC の呼び出しの対応は、[呼び出しの一覧](#呼び出しの一覧)にあります。

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

## エラー

呼び出しの失敗は、gRPC の状態コードで返ります。Python では例外 `ControlError` になります。

| `ControlError` のメンバー | 内容 |
|---|---|
| `code` | gRPC の状態コードです。 |
| `reason` | Server が理由を付けて拒否したときの、その語です（trailing metadata の `vehicle-sil-reason`）。`scenario-running`、`outside-real-time` と、今はできない時刻のコマンドのほかの理由です（なければ空）。 |

| 状態コード | 返るとき |
|---|---|
| `INVALID_ARGUMENT` | 引数のエラーです（範囲の外の速度、0 以下のステップ、`snapshot()` の名前が 1024 個を超える）。 |
| `FAILED_PRECONDITION` | 今はできない呼び出しです（一時停止していないときの `step`、終わった後の `Stop`、シナリオの実行中の書き込みと時刻の操作、外につながっていて実時間を保つ実行での一時停止や速度の変更）。 |
| `CANCELLED` | 待っている間に、サーバーが閉じました（`WaitPaused`。`FAILED_PRECONDITION` のこともあります）。 |
| `RESOURCE_EXHAUSTED` | `snapshot()` のレスポンスが、1 つのメッセージの上限（4 MiB）を超えました。 |
| `UNIMPLEMENTED` | Server が、その呼び出しを持たない前のバージョンです。 |

シナリオの実行中の書き込みと時刻の操作は、`reason` が `scenario-running` です。

| Python の例外と警告 | 出るとき |
|---|---|
| `ControlError` | 呼び出しが失敗しました。 |
| `LaunchError` | ランチャーが結果を出力せずに終わったか、待つイベントが来ないまま実行が終わりました。 |
| `ControlWarning` | `peers()` を呼んだとき、Server がシミュレーションの状態を読めていません。 |

## 制限

| 項目 | 制限 |
|---|---|
| 時間の操作（一時停止、ステップ実行、速度）で止まる Peer | 仮想時間で同期している Peer だけで、実時間の Peer は動き続けます。 |
| 速度を指定したときの遅れ | シミュレーションが実時間に遅れても、後から追いつこうとはしません。 |
| ない操作 | リセット、participant の起動と再起動、CAN/LIN のチャネルの選択はありません。 |
| サーバーが扱う実行 | そのランチャーの 1 回の実行だけで、実行が終わると閉じます。 |
| Peer の `status`（Python の `Peer.failed` と `error`、CLI の `FAILED`） | いつも空です（ECU の Agent はこれを送信しません）。 |
| 値の購読 | 最新値です（読む前に来た新しい値で置き換えます）。 |

- 速度を指定して遅れたときは、遅れた分だけ後ろにずれたまま、指定の速度で進みます（[internals/virtual-time.md](internals/virtual-time.md#遅れの測り方) の「遅れの測り方」）。
- participant の失敗は、シミュレーションの状態（`watch(simulation=True)` の `failed_participant` と `failure_reason`）で読みます。
- すべての値を記録するには、SIL Kit のトレースか `TapPort` を使います。

| 上限（Server の既定） | 値 |
|---|---|
| 1 回の `write()` の書き込み | 4096 個まで |
| 1 回の `snapshot()` の Peer の名前 | 1024 個まで |
| 同時に待つ呼び出しと購読 | 合わせて 256 本まで |

## 関連するドキュメント

| 知りたいこと | ドキュメント |
|---|---|
| 手順を追って、値を読み書きし、時間を止めて進める | [信号を外から操作する](howto/operate-signals.md) |
| ほかの PC から接続する手順（証明書の作り方） | [複数の PC で動かす](howto/multi-pc.md) |
| 保証の詳細（反映の時刻、順序、上限、スレッド） | [internals/control.md](internals/control.md) |
| イベントと標準入力のコマンド | [simulation-events.md](simulation-events.md) |
| シナリオの実行中にクライアントができること | [scenario.md](scenario.md) の「実行中に変数を見る」 |
| ブラウザで値を見て操作する | [gui.md](gui.md) |
