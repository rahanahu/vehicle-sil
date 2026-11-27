# Control API の使い方

[English](../control-api.md)

Control API は、実行中のシミュレーションを外から操作・監視する gRPC の API です（`proto/vehicle_sil/control/v1/control.proto`）。このドキュメントは、起動のしかた、認証、ECU の変数の宣言、呼び出しの一覧、Python のクライアントと CLI、時間の操作のルールのリファレンスです。手順を追って試すときは、ガイド「[信号を外から操作する](howto/operate-signals.md)」から始めます。

| 調べたいこと | 節 |
|---|---|
| クライアント、ランチャー、ECU のつながり | [全体の構成](#全体の構成) |
| Control Server を持つランチャーのビルド | [ビルド](#ビルド) |
| `--control` のアドレス、サーバーが起動する時点と終わる時点 | [ランチャーに接続する](#ランチャーに接続する) |
| クライアントを 1 つの実行に結びつける（`vehicle-sil-run`、`PingReply.run`） | [宛先の実行](#宛先の実行) |
| 最初のステップの前に値を書く | [始まる前に書く（`--start-paused`）](#始まる前に書く--start-paused) |
| 時間のコマンドの順序とルール | [標準入力とクライアントのコマンド](#標準入力とクライアントのコマンド) |
| TLS、トークン、ロール | [認証](#認証) |
| ECU のコードでの変数の宣言 | [ECU の変数](#ecu-の変数) |
| gRPC の呼び出しと、Python、CLI、ロールの対応 | [呼び出しの一覧](#呼び出しの一覧) |
| Python のクライアント | [Python](#python) |
| 1 つのランチャーで順に実行する（`Serve`、`RunHandle`） | [1 つのランチャーで順に実行する（`Serve`、`RunHandle`）](#1-つのランチャーで順に実行するserverunhandle) |
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

ビルド済みのツールアーカイブ（`vehicle-sil-tools-*`）には、`--control`（Control Server）付きの `vehicle_sil_launch` と生成済み gRPC スタブ付きの Python クライアントが同梱されているため、ソースからビルドする場合だけビルドが必要です。

```sh
cmake --preset linux-gcc-release -DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON
cmake --build --preset linux-gcc-release
```

このビルドの `vehicle_sil_launch` は `--control` を持ち、`vehicle_sil_launch version --json` の `features` に `control-server` があります。持たないビルドで `--control` を付けると、引数のエラー（終了コード 4）で、ビルドし直す方法を標準エラーに表示します。

| 項目 | 内容 |
|---|---|
| gRPC が見つからないとき | FetchContent で取り込みます（初回はダウンロードとビルドに数分かかります）。 |
| Windows（MSYS2 UCRT64）の gRPC | `pacman -S mingw-w64-ucrt-x86_64-grpc` でインストールしておきます。 |
| Control Server のテストまでまとめて実行する | Linux では `linux-gcc-control`、Windows では `windows-mingw-control`（GCC の Debug でこの設定）を `scripts/verify.py <preset>` で使えます（[build.md](build.md) の「vehicle-sil 自体をビルドする」）。 |
| 依存ライブラリの詳細 | [dependencies.md](dependencies.md) にあります。 |

## ランチャーに接続する

```sh
vehicle_sil_launch run --stdin-commands --start-paused --control 0 simulation.yaml
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
| `write_and_read.py` | 最短の `Launch` の例です。時刻 0 の前で一時停止してスロットルを書き込み、500 ms 進めて回転数を表示します。 |
| `write_before_start.py` | Python から起動して接続し、始まる前に書いて、読み返して確認します。 |
| `pause_and_step.py` | 動いている途中で止め、時刻を指定して書き、`snapshot()` で時刻と値をまとめて読んで確認し、速度を変えて最後まで動かします。 |
| `serve_runs.py` | 常駐ランチャー（`Serve`）を 1 つ起動して `prepare()` し、独立した 2 回の実行を `RunHandle` と `run.control()` で順に動かします。 |

### 実行が終わるとき

実行が終わると（通常の終了、停止の要求、`--timeout-ms`、participant の失敗、どれでも）、ランチャーは participant を終わらせてから、結果を出力する前にサーバーを閉じます。

| 項目 | 内容 |
|---|---|
| 待っている呼び出し（`Watch`、`WaitOperation`、`WaitPaused`） | サーバーを閉じる時点で終わります（`Watch` はストリームの終わり、`WaitPaused` は `FAILED_PRECONDITION` か `CANCELLED`）。 |
| クライアントが接続したままのとき | ランチャーはクライアントを待たないので、終了は遅れません。 |
| シミュレーションが終わってからサーバーを閉じるまでの間 | 終わった時点の値を読めます。 |

### 宛先の実行

実行には、ランチャーが付ける ID があります。中身に意味のない文字列で、等しいかどうかだけを比べます（分解しません）。Control Server は実行ごとにあり、自分の実行の ID を知っています。gRPC のチャネルは、そのアドレスで待ち受けているサーバーに自分でつなぎ直すので、固定のアドレスでは、前の実行のクライアントが次の実行のサーバーに届いてしまいます。そこで、クライアントは宛先の実行を名乗り、サーバーは別の実行宛ての呼び出しを断ります。

| 項目 | 内容 |
|---|---|
| クライアントが ID を知る方法 | `PingReply` の `run`（`Ping`）。`serve` では、ランチャーのイベントにもあります（[simulation-events.md](simulation-events.md)）。 |
| 呼び出しで実行を名乗る方法 | メタデータ `vehicle-sil-run: <ID>`。Python では `Control(address, run=<ID>)`、`Control.for_current_run(address)`、または `Serve` の `run.control()` です（[Python](#python)）。 |
| 別の実行を名乗る呼び出し | `FAILED_PRECONDITION` で断ります。trailing metadata の `vehicle-sil-reason` は `run-changed` です。呼び出しの中身は ECU にも時刻にも届きません。 |
| 実行を名乗らない呼び出し（`vehicle_sil_launch run`） | 受けます（実行に ID がなかったころと同じです）。 |
| 実行を名乗らない呼び出し（`vehicle_sil_launch serve`。1 つのランチャーが実行を 1 つずつ順に行う。[simulation-run.md](simulation-run.md)） | `FAILED_PRECONDITION` で断ります。`vehicle-sil-reason` は `run-required` です。Python では、`ControlError` に `Control.for_current_run()` または `Control(..., run=...)` を案内するヒントが付きます。 |
| `Ping` | 実行を理由に断ることはありません。クライアントがいつでも今の実行を知れるようにするためです。 |
| 確かめる順序 | トークン（`UNAUTHENTICATED`）、ロール（`PERMISSION_DENIED`）、実行の順です。正しいトークンのないクライアントは実行について何も知れず、トークンのあるクライアントも `Ping` で分かること以上は知れません。 |
| 監査ログ | 各行の先頭に `[run <ID>] ` が付きます。1 つのファイル（`--control-audit-log`）に複数の実行の行が入っても区別できます。断った呼び出しの行には、名乗った実行も書きます。 |

実行の ID は資格情報ではありません。`Ping` を呼べる人なら誰でも読めます。クライアントを自分の実行にとどめるだけです。クライアントに何ができるかは、トークンとロールが決めます（[認証](#認証)）。ある実行のクライアントを次の実行から完全に締め出すには、実行ごとに別の資格情報ファイルを使います。

### シナリオの実行中

シナリオの実行（`--scenario`）では、クライアントは、読む、待つ、時刻を動かす、止めることができます。シナリオの再生中（`end_ms` までを含む）は変数を書けません。実行要求に `"after_scenario": "pause"` を指定した場合（ランチャーの feature `scenario-handover`）は、シナリオが成功すると同じ Run のまま一時停止して手動制御へ引き継ぎ、書き込みが解放されます。

| 呼び出し | シナリオの再生中 | `after_scenario: "pause"` の引き継ぎ後 |
|---|---|---|
| 読む・待つ呼び出し、`Stop` | いつもどおりです。 | いつもどおりです。 |
| 書く呼び出し（`Write`、`ForgetPeer`） | `FAILED_PRECONDITION`（`vehicle-sil-reason` は `scenario-running`）で拒否します。 | 同じ Run のまま解放され（`state` の `handed_over: true`）、シナリオのない実行と同じく使えます。 |
| 時刻を動かす呼び出し（`Pause`、`Resume`、`Step`、`RunUntil`、`SetSpeed`） | シナリオのない実行と同じように使えます（ランチャーの feature `scenario-time`）。シナリオの結果は変わりません。 | 引き継いだ一時停止位置からいつもどおり使えます（`Step`、`RunUntil`、`Resume` でその仮想時刻から進みます）。 |

引き継ぎ前の書き込みの拒否は、trailing metadata の `vehicle-sil-reason`（値は `scenario-running`）で見分けます。呼び出しごとの表と保証は [scenario.md](scenario.md) の「実行中に変数を見る」にあります。

引き継ぎの規則:

- `"after_scenario": "pause"` を指定して、停止要求や中断と競合せずにシナリオが成功すると、ランチャーはシナリオ終了の controller ステップ（`over_at_ms`）でアトミックに一時停止し、同じ Run（`PingReply.run` / `RunHandle`）と ECU の状態を保ったまま、`state` イベントで `handed_over: true` と `scenario_over_at_ms` を通知し、Run が開いている間 `Write` と `ForgetPeer` を解放します。`speed: "max"` でも `Step`、`RunUntil`、`Resume` までその位置で一時停止し続けます。
- `"after_scenario": "stop"`（既定）のときや、シナリオが失敗（`scenario-failed`、`scenario-timeout`、`scenario-write`、`scenario-not-observed`）、`incomplete`（`stop_at_ms`、`Stop`、`stop`、中断、`--timeout-ms`、participant の失敗）、または停止要求との競合で終わったときは、引き継ぎは起きません（`handed_over` は `false` のまま、`Write` と `ForgetPeer` は `scenario-running` で拒否され続け、Run は終了します）。

シナリオは、クライアント `scenario` として書き込みます。その操作の `client` は `scenario` です（`Watch`、`GetOperation`、`WaitOperation` の `Operation.client`）。この名前は変わりません。シナリオ自身の操作（ステップごとに 1 件になりうる）が要らない watch は、`WatchRequest.operation_clients_excluded` にこの名前を入れます。この一覧のクライアントの操作は送られず、`Update.dropped` にも数えられません。空の一覧なら、操作は全部送られます。watch の初めの更新には、操作は入りません。名前は `Operation.client` と完全に一致したものだけが当たります。gRPC のクライアントの `client` は `<名前>@<接続のアドレス>` で、接続ごとに変わるので、一覧に入れて役に立つのは固定の名前だけです。いまの固定の名前は `scenario` だけです。GUI サーバーの watch は `scenario` を除きます。Python のクライアントの `Control.watch()` には一覧の引数がないので、Python の watch にはシナリオの操作も届きます。

### 外につながる実行

シミュレーションの外につながる実行（`--connect`、または有効な `role: bridge` の participant）は、`--time-policy free` で始めなければ、実時間を保ちます（[simulation-run.md](simulation-run.md) の「外につながる実行の時間」）。

| 呼び出し | 実時間を保つ実行 |
|---|---|
| `Pause`、`Step`、`RunUntil`、1 以外の速度の `SetSpeed` | `FAILED_PRECONDITION` で拒否します。trailing metadata の `vehicle-sil-reason` は `outside-real-time` です。引数の誤り（0 のステップ、範囲外の速度）も、`INVALID_ARGUMENT` でなく、この答えになります。 |
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

時刻を指定した `pause` が進むのは、このステップ 0 より前の待機からだけです。シミュレーションが始まって一時停止した後は、`pause(at=T)` は再開せずに `pause_target` を設定します（どちらの状態からでも `T` まで進めて止めるには `run_until(T)` を使います）。

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
| `Pause` | `pause` | 時刻つきなら `at_ms`（`pause_target` を更新。開始後の一時停止中に呼んでも再開しません） |
| `Resume` | `resume` | なし（予定されていた `pause_target` を消します） |
| `Step` | `step` | `ms` と `at_ms`（その step の後に止まる時刻。一時停止中だけ有効） |
| `RunUntil` | `run_until` | `at_ms`（一時停止中でも実行中でも `at_ms` まで進めて止めます） |
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

たとえば、クライアントの `step` と標準入力の `resume` が重なると、イベントの順で、止まるか進み続けるかが決まります。時刻コマンドは「後の呼び出しが勝つ」規則に従います。`resume` は予定されていた `pause_target` を消し、開始後の一時停止中に `at_ms` 付きの `pause` を呼ぶと再開せずに `pause_target` だけを更新し、`run_until` は現在が一時停止中か実行中かにかかわらず `at_ms` まで進めて一時停止します。

クライアントへのレスポンスは、受け付けなければ gRPC のエラーで、メッセージはイベントの `message` と同じです。

| 受け付けない理由 | 状態コード | trailing metadata の `vehicle-sil-reason` |
|---|---|---|
| 引数のエラー（範囲の外の速度、0 以下のステップ） | `INVALID_ARGUMENT` | ありません。 |
| 今はできないコマンド | `FAILED_PRECONDITION` | イベントの `reason` です（`not-paused`、`not-running`、`outside-real-time` など。[simulation-events.md](simulation-events.md) の「理由のコード」）。 |

`Stop` は、標準入力の `{"command":"stop"}` と同じです。

| 項目 | `Stop` の動作 |
|---|---|
| 1 回目と 2 回目 | 1 回目は順に止め、2 回目は待たずに participant を終わらせます。 |
| 結果 | `interrupted_by` は `"control server stop"`、`end` は `"stopped"` です。終了コードは 0、シナリオが終わる前に止めたときは 5 です（エラーがあれば 1）。 |
| 理由の文 | 監査ログの行にだけ残ります。 |
| シミュレーションが終わった後（始まらなかったときも） | ランチャーが終わりを受け取った後は、時間のコマンドと同じく拒否し（`accepted: false`、`FAILED_PRECONDITION`）、結果は変わりません。その直前のわずかな時間に来た `Stop` は受け付けますが、何も変えません。 |
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

`--control-audit-log <file>` を付けると、誰が何を変えたかを、呼び出しごとに 1 行、先頭に `[run <ID>] ` を付けてファイルに追記します（[宛先の実行](#宛先の実行)）。書き込めなくなっても実行は続き、その行は失われます。

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

型は、bool、整数、実数、列挙です（`vehicle_sil::control::Value` は `std::variant<bool, std::int64_t, double>`）。値は ECU のステップのスレッドで読み書きするので、モデルにロックは不要です。

| C++ の型 | 変数の型（`ValueType`） | `input()` / `output()` | `input_enum()` / `output_enum()` | 補足 |
|---|---|---|---|---|
| `bool` | `bool`（`Bool`） | 対応 | コンパイルエラー（`static_assert`） | — |
| 符号付き整数（`std::int8_t` .. `std::int64_t`、`int` など） | `integer`（`Integer`） / `enum`（`Enum`） | 対応（`Integer`） | 対応（`Enum`） | `std::int64_t` として保持し、書き込みは C++ の型の範囲と `min` / `max` で確認します。 |
| 32 ビットまでの符号なし整数（`std::uint8_t`、`std::uint16_t`、`std::uint32_t`） | `integer`（`Integer`） / `enum`（`Enum`） | 対応（`Integer`） | 対応（`Enum`） | 範囲は `0`..その型の最大値で、`std::int64_t` として保持します。 |
| 64 ビットの符号なし整数（`std::uint64_t`） | `integer`（`Integer`） | `input()`: **コンパイルエラー（`static_assert`）**<br>`output()`（`min`/`max` なし）: 対応（飽和） | `input_enum()`: **コンパイルエラー（`static_assert`）**<br>`output_enum()`: **実行時に `std::invalid_argument`** | 制御の値は `std::int64_t` で運ぶため `uint64_t` の全範囲を表せず、書き込める入力や列挙の値には `std::int64_t` を使います。`min`/`max` なしの `output()` だけは登録でき、読み出し時に `INT64_MAX` を超える値を `INT64_MAX` に飽和させます（`min`/`max` 指定の `output()` や `output_enum()` は範囲検査で `std::invalid_argument` になります）。 |
| 浮動小数点数（`float`、`double`、`long double`） | `real`（`Real`） | 対応 | コンパイルエラー（`static_assert`） | 読み出し時は `static_cast<double>(v)` で `double` に変換し（丸め・飽和はしません）、書き込みは有限な `float` または `double` の範囲（`real_max`）と `min`/`max` で拒否判定します（クランプしません）。 |
| `enum` 型 | `enum`（`Enum`） | コンパイルエラー（`static_assert`） | `input_enum()`: 対応（64 ビット符号なしの基底型を除く）<br>`output_enum()`: 対応（64 ビット符号なしの基底型は実行時に例外） | 値は `std::int64_t` で運びます。通常の `input()` / `output()` が受け付けるのは `bool`、整数、浮動小数点数だけです。 |
| そのほかの型（`std::string`、構造体、ポインター） | — | コンパイルエラー（`static_assert`） | コンパイルエラー（`static_assert`） | `bool`、整数、浮動小数点数、`enum` だけに対応しています。 |

| `VariableOptions` のメンバー | 意味 |
|---|---|
| `unit` | 単位です。 |
| `min`、`max` | 範囲です。 |
| `step` | 値の刻み幅です。 |
| `description` | 説明です。 |
| `parameter` | 真にすると、最初のステップの前だけ書ける変数（Parameter）になります。 |
| `availability` | 設定すると、変数が使われているかを報告する変数になり、この値が最初の状態です（ふつうは `Availability::Unknown`）。下の「変数の利用状態」を参照してください。 |

| 宣言のルール | 内容 |
|---|---|
| 宣言できる時点 | 実行の前（ECU の組み立ての中）だけです。 |
| 開始後の宣言 | `std::logic_error` です（メッセージに ECU の名前があります）。 |
| 同じ ECU での同じ名前 | `std::invalid_argument` です（メッセージに ECU の名前があります）。 |
| ポインターと関数が指すもの | ECU が持つもの（`Simulation::add()` が作るクラス、`emplace()` したオブジェクト）にします。 |
| 破棄の順 | ECU は Agent を、ネットワークの後、`emplace()` したオブジェクトの前に破棄します。 |

### Agent の設定（`set_agent_options()`）

`set_agent_options(vehicle_sil::control::AgentOptions)` で、ECU の Control Agent の読み出し周期と有界キューの大きさを設定します。呼べるのは、最初の `input()`、`output()`、`agent()` より前だけです。その後に呼ぶと `std::logic_error`、不正な値を渡すとその場で `std::invalid_argument` になります（メッセージに ECU の名前があります）。

| `AgentOptions` のメンバー | 既定値 | 制約 | 意味 |
|---|---|---|---|
| `max_pending` | `256` | `> 0` | 処理中のコマンドの最大数（送信済みで `drain_results()` で回収前のもの）と、コマンド・結果キューの容量。 |
| `max_events` | `1024` | `> 0` | `Agent::log()` のイベントキューの容量。これを超えたイベントは捨てられ、`AgentStats::events_dropped` に数えられます。 |
| `sample_period` | `10ms` | `>= 0` | `0`、`sample_period`、`2 * sample_period`、… 以後の最初のステップで変数を読み出します。`0ms`（`SimDuration::zero()`）で毎ステップ読み出します。 |

```cpp
class Engine {
public:
    explicit Engine(vehicle_sil::Ecu& ecu) {
        ecu.set_agent_options({
            .max_pending = 512,
            .max_events = 2048,
            .sample_period = 20ms,   // or 0ms to sample every step
        });
        ecu.input("engine.throttle", &throttle_, {.unit = "%", .min = 0.0, .max = 100.0});
        ecu.output("engine.speed", &rpm_, {.unit = "rpm"});
    }
private:
    double throttle_ = 0.0;
    double rpm_ = 800.0;
};
```

`vehicle_sil_launch` がシナリオを実行するときは（`samples: as_sent` でない限り）シナリオが変数を読み取る `ecu_main()` の ECU に `--sample-every-step` を渡し、内部で `ecu.sample_every_step()` が呼ばれて、ほかの `AgentOptions` の設定を保ったまま `sample_period` だけが `0` に上書きされます。

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

### 変数の利用状態

どの入出力を使うかを実行中に決める ECU（たとえば最初の数ステップで読む設定で決めるもの）は、変数ごとにそれを知らせられます。画面は使わない変数を灰色にできます。利用状態は情報だけです。書き込み、読み取り、標本、シナリオは見ません。`Inactive` の変数への書き込みも、ほかと同じく検査して反映します。

| 状態（`vehicle_sil::control::Availability`） | 意味 |
|---|---|
| `Unknown` | アプリケーションがまだ決めていない。 |
| `Active` | 使っている。利用状態を報告しない変数は、すべて `Active` とみなします。 |
| `Inactive` | ECU の今の構成では使っていない。 |

| 決まり | 内容 |
|---|---|
| 報告する変数 | `VariableOptions::availability` を付けて宣言した変数です。この値が最初の状態です。どの変数が報告するかは宣言で決まり、実行中は変わりません。 |
| 状態を変える | ECU のステップ（`on_step()`、`every()`、`at()`、ハンドラー）から `ecu.set_availability(name, state)` を呼びます。同じ状態をもう一度渡しても何も起きません。 |
| エラー | ECU が宣言していない名前、`availability` なしで宣言した変数、vehicle-sil がどの ECU にも足す変数（`ecu.connected`、監視の出力）は `std::invalid_argument` です。組み立ての間（実行の前）は `std::logic_error` です。最初の状態は宣言で渡してください。 |
| 状態の行き先 | 変わったときと、シミュレーション時間の 1 秒ごとに、Control Server へ送ります（後からつながったサーバーにも届きます）。Hub の購読は、頼んだとき（`SubscriptionOptions::availability`）だけ受け取ります。gRPC のサービスは下のように渡し、GUI サーバーは画面へ渡します（[gui/protocol](../../gui/protocol/README.ja.md#ecu-の変数) の「ECU の変数」）。標準のブラウザー GUI は、使っていない変数を薄く表示し、その旨を示します（[gui.md](gui.md#コンポーネントに共通の動作) の「コンポーネントに共通の動作」）。 |
| 古い vehicle-sil で作った ECU | 何も報告しません。変数はすべて `Active` とみなします。check が注記します（`control-link-revision`）。 |

```cpp
ecu.input("io.relay", &relay_, {.availability = vehicle_sil::control::Availability::Unknown});
ecu.every(10ms, [this, &ecu](vehicle_sil::SimTime) {
    if (config_read_) {
        ecu.set_availability("io.relay", relay_fitted_ ? vehicle_sil::control::Availability::Active
                                                       : vehicle_sil::control::Availability::Inactive);
    }
});
```

gRPC では、`ListPeers` と `Watch` の `PeerInfo` の `VariableInfo.availability` が宣言です。報告しない変数は `AVAILABILITY_UNSPECIFIED`、報告する変数は最初の状態です。`Watch` に `availability = true` を付けると、報告する変数のある参加者ごとに `PeerAvailability`（`Update` の `availability`）を送ります。`schema` の順で全変数の状態を持ち、報告しない変数は `AVAILABILITY_ACTIVE` です。`PeerInfo` の後に、最初の更新（`initial_done` の前）として、その後は状態が変わったときに届きます。新しい incarnation は宣言の状態から始まります。同じ incarnation の `PeerInfo` がまた届いても（つなぎ直し、SIL Kit の状態の変化）状態は戻らないので、別の incarnation が来るまで状態を持ち続け、今の incarnation と違う `PeerAvailability` は捨てます。購読は参加者ごとに最新だけを持つので、途中の変化を落とすことがあります。`peers` で絞れ、頼まない購読には届きません。利用状態は情報だけで、`Write`、`GetValues`、`GetSnapshot`、`Watch` の値は変わりません。Python の client は頼みません。

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
| `Ping` | サーバーのバージョン（`server_version`）、サーバーを作った vehicle-sil のリリース（`vehicle_sil`）、サーバーの実行の ID（`run`）を返します。 | `ping()`（`server_version` を返す）、`current_run()`（`run` を返す） | なし | `viewer` |
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
| `Pause` | 一時停止します（`at` で時刻指定の停止を予定。開始後の一時停止中は再開しません）。 | `sim.pause()` | `pause` | `operator` |
| `Resume` | 再開します（予定されていた `pause_target` も消します）。 | `sim.resume()` | `resume` | `operator` |
| `Step` | 指定した時間だけ進めて止めます（一時停止中のみ）。 | `sim.step()` | `step` | `operator` |
| `RunUntil` | 指定した時刻まで進めて止めます（一時停止中・実行中のどちらでも可）。 | `sim.run_until()` | `run-until` | `operator` |
| `SetSpeed` | 速度を変えます。 | `sim.set_speed()` | `speed` | `operator` |
| `Stop` | シミュレーションを止めます。 | `sim.stop()` | `stop` | `admin` |

## Python

```sh
uv sync                                         # .venv に vehicle-sil-control（編集可能）、grpcio、grpcio-tools、protobuf を入れる
source .venv/bin/activate                       # または、各コマンドの前に uv run を付ける
python -m vehicle_sil_control.generate          # スタブを python/vehicle_sil_control/_generated に作る
```

| 項目 | 内容 |
|---|---|
| Python のバージョン | 3.11 以上です。 |
| バージョンと依存の指定 | `python/pyproject.toml`（`dependencies = []`、`python -S` でも動作し、`gui/server` と純粋な Events v4 プロトコル定数・NDJSON コマンド構築の `protocol.py` と遅延 gRPC ヘルパー `grpc_helpers.py` を共有します）、リポジトリのルートの `pyproject.toml`（`uv sync`、または Control API のみなら `uv sync --only-group control`）と `uv.lock` です（[build.md](build.md) の「Python（uv）」）。 |
| 標準ライブラリのみと gRPC | `Launch`、`Serve`、`RunHandle`、`LaunchError`、`ServeError`、`control_address` は標準ライブラリだけを使い（`python -S` 対応）、gRPC のモジュールを読み込みません。`Control`、`Control.for_current_run()`、`run.control()`、CLI には `grpcio`、`protobuf`、生成されたスタブが必要です（別の場所に出すときは `generate --out DIR` と `VEHICLE_SIL_CONTROL_STUBS=DIR` を使います）。 |
| ビルド済みツールアーカイブ（`Control` / gRPC） | `<tools>/python` に生成済みスタブが含まれています。`PYTHONPATH=<tools>/python uv run --no-project --no-build --python ">=3.11,<3.15" --with-requirements <tools>/python/requirements-control.txt python <script.py>` で実行します。 |
| ビルド済みツールアーカイブ（`Launch` / `Serve` のみ） | 追加の依存はありません。`PYTHONPATH=<tools>/python python -S <script.py>`（または Python 3.11 以上）で実行できます。 |
| `.venv` の外・`uv sync` なしの場合 | `python -m vehicle_sil_control.generate` やスクリプトを実行する前に `export PYTHONPATH=$PWD/python` を設定し、`pip install grpcio grpcio-tools protobuf` でインストールします。 |
| uv を使わない場合（MSYS2 UCRT64） | `export PYTHONPATH=$PWD/python` を設定し、`pacman -S mingw-w64-ucrt-x86_64-python-grpcio mingw-w64-ucrt-x86_64-python-grpcio-tools` でインストールします。 |

`Launch` と `Serve` の使い分け:

| ヘルパー | ランチャーのモード | 選ぶとき |
|---|---|---|
| `Launch` | `vehicle_sil_launch serve --once` | 1 回だけ実行するとき、または `VEHICLE_SIL_TRACE`、固定の `registry_uri`、`manual` の participant を使うとき（`--once` でのみ許可されます）。 |
| `Serve` / `RunHandle` | `vehicle_sil_launch serve`（または `once=True`） | 1 つの設定ファイルでランチャーを起動し直さずに複数のシミュレーションを順に実行するとき、実行前に `prepare()` / `status()` を使いたいとき、実行ごとの `RunHandle`（`run.id`、`run.control()`、確定済み `status`、`launcher_ends`）が必要なとき。 |

### ランチャーを起動する（`Launch`）

ランチャーは、`Launch` で 1 回実行の子プロセスとして起動できます。`vehicle_sil_launch serve --once --control <address>` を起動し、最初の `{"command":"run", ...}` を標準入力に送り、Events v4（`vehicle_sil_events: 4`）ストリームを別のスレッドで読みます（標準ライブラリだけで、gRPC は不要です）。

```python
from vehicle_sil_control import Control, Launch, ms

with Launch("build/linux-gcc-release/tools/launch/vehicle_sil_launch", "simulation.yaml", start_paused=True) as launch:
    with Control(launch.address) as c:   # address はイベント control_server を待って返す
        ...
    launch.stop()                        # {"command":"stop"}（with を出るときも、動いていれば止める）
    print(launch.wait()["result"])       # run --json と同じ結果
```

`Launch(launcher, config, *, address="0", start_paused=False, case=None, params=None, after_scenario=None, arguments=(), cwd=None, stderr=None, keep_logs=True)` の `config` より後の引数はすべてキーワード専用です。`case`、`params`（型付き JSON パラメーターの対応）、`after_scenario` は最初の標準入力 `run` コマンドの `intent` へそのまま入ります。`arguments` には `run` の引数を渡せます。実行ごとの引数（`--speed`、`--stop-at-ms`、`--scenario`、`--case`、`--after-scenario`、`--variant`、`--disable`、`--enable`、`--manual`、`--param`、`--connect`、`--time-policy`、`--strict`、`--late-tolerance-ms`、`--request`、`--timeline`、`--start-paused`）は最初の標準入力 `run` コマンドに変換され（`--param <k>=<v>` は Python 側で型推測せず `intent.cli_params` へそのまま転送され、型付き `params` よりコマンドライン優先順位で優先します）、ホスト側の引数（`--control-credentials`、`--control-tls-cert`、`--control-tls-key`、`--control-audit-log`、`--log-dir`、`--timeout-ms`、`--describe-timeout-ms`、`--participant-start-timeout-ms`、`--participant-stop-timeout-ms`、`--events-stall-ms`）は `serve --once` に渡されます。

| メンバー | 内容 |
|---|---|
| `address` | Control Server のアドレスです（イベント `control_server` を待って返します）。 |
| `pid`、`participant_pids` | ランチャーの子プロセスの PID（`int`）と、これまで読んだ Events v4 の `participant`/`started` イベントから起動済み participant 名を PID に写す `dict[str, int]` です。 |
| `events` | 読んだイベントの dict の一覧です（`bad_lines` は不正な標準出力行、`stderr_text` は `stderr=subprocess.PIPE` 時の標準エラー出力）。 |
| `result` | 結果です。 |
| `wait_for_event(event, timeout=60.0, after=None, **keys)` | キーの値が合う最初のイベントを待ちます。 |
| `send(command)` | 標準入力にコマンドを 1 行送信します。 |
| `stop()` | `{"command":"stop"}` を送信します（`with` を出るときも、動いていれば止めます）。 |
| `wait(timeout=120.0)` | ランチャーが終わるのを待って、結果を返します。 |
| `close()` | 動いていればランチャーを停止し、パイプを閉じます（`__exit__` が呼びます）。 |
| `returncode` | ランチャーの終了コードです。 |

ランチャーが結果を出力せずに終わったときと、待つイベントが来ないまま実行が終わったときは、`LaunchError` です（メッセージに、結果、エラーの指摘とその `detail`（`--describe` の標準エラーなど、上限 500 文字）、結果なしの異常終了時にはコマンド・終了コード・標準エラー末尾を含みます）。

自分でランチャーを起動するときは、`control_address(lines)` が、イベントの行（文字列か dict）から最初の `control_server` の `address` を返します。

### 1 つのランチャーで順に実行する（`Serve`、`RunHandle`）

`Serve` は、`vehicle_sil_launch serve <file>`（`once=True` のときは `serve --once <file>`）を Events v4（`vehicle_sil_events: 4`）の子プロセスとして起動し、1 つの設定ファイルで実行を順に行います（[simulation-run.md](simulation-run.md#1-つのランチャーで順に実行するvehicle_sil_launch-serve) の「1 つのランチャーで順に実行する（`vehicle_sil_launch serve`）」と [simulation-events.md](simulation-events.md#常駐ランチャーserve) の「常駐ランチャー（`serve`）」）。`Launch` と同じく、`Serve` と `RunHandle` は標準ライブラリだけを使い、gRPC は `run.control()` を呼んだときにだけ読み込みます。実行できる全体の例は [examples/launch_control/serve_runs.py](../../examples/launch_control/serve_runs.py) にあります。

`Launch` と違い、`runner.start(...)` は実行ごとの実行 ID（`run.id`）を持つ `RunHandle` を返し、前の実行の状態を持ち越さず仮想時刻 0 から新しい実行を始めます。`run.control()` は、あらかじめ `run.id` に結びつけた `Control` を返します。複数回実行の `serve` の制限がそのまま当てはまります（`once=True` でない限り、`VEHICLE_SIL_TRACE`、固定の `registry_uri`、`manual` の participant は拒否されます。[simulation-run.md](simulation-run.md#serve-の制限) の「`serve` の制限」）。

```python
from vehicle_sil_control import Serve

with Serve("build/linux-gcc-release/tools/launch/vehicle_sil_launch", "simulation.yaml", control=True) as runner:
    prep = runner.prepare()
    with runner.start(start_paused=True, stop_at_ms=500) as run:
        with run.control() as c:   # bound to run.id; an earlier Control raises ControlError ("run-changed")
            c.sim.wait_paused()
            c.sim.resume()
        print(run.wait()["result"])
    with runner.start(scenario="scenarios/smoke.yaml") as run2:
        print(run2.wait()["result"])
```

`Serve(launcher, config, *, once=False, control=False, address="0", credentials=None, tls_cert=None, tls_key=None, audit_log=None, log_dir=None, timeout_ms=None, describe_timeout_ms=None, participant_start_timeout_ms=None, participant_stop_timeout_ms=None, events_stall_ms=None, arguments=(), cwd=None, env=None, stdout=subprocess.PIPE, stderr=None, keep_logs=True, on_event=None, start_new_session=False, wait_ready=True, startup_timeout=30.0)` では、`address`、`credentials`、`tls_cert`、`tls_key`、`audit_log` を指定するには `control=True` が必要です。

| `Serve` のメンバー | 内容 |
|---|---|
| `hello`、`instance`、`latest_status` | 初めの `hello` イベント、その `launcher` インスタンス ID、最新の `ExecutionStatus` の dict（`status`、`prepare`、`result` イベントから更新、最初のステータス前は `None`）です。 |
| `pid`、`returncode`、`stderr_text` | 子プロセスの PID、終了コード（実行中は `None`）、`stderr=subprocess.PIPE` 時の標準エラー出力末尾（上限 64 KiB）です。 |
| `prepare(*, variant=None, disable=(), enable=(), manual=(), id=None, timeout=30.0)` | `prepare` を送り、`prepare` の応答（`accepted`、`status`、`metadata`、`findings`）を返します。 |
| `status(timeout=30.0, *, id=None)` | `status` を送り、現在の `status` イベントを返します。 |
| `start(*, request=None, intent=None, ..., timeline=None, id=None, timeout=60.0)` | 実行を 1 つ始めて `RunHandle` を返します。`request`、`intent`、実行ごとのキーワード引数（`variant`、`params`、`scenario`、`case`、`disable`、`enable`、`manual`、`connect`、`time_policy`、`speed`、`start_paused`、`strict`、`stop_at_ms`、`late_tolerance_ms`、`after_scenario`）は最大 1 つだけ渡せます。大きすぎるインラインのインテントは自動で一時要求ファイルに切り替え、後で削除します。 |
| `wait(timeout=60.0)` | ランチャーの子プロセスが終了するのを待ち、終了コードを返します（`RunHandle.wait()` とは異なります）。 |
| `close_input()`、`close(timeout=60.0)` | `close_input()` は待たずに標準入力を閉じて（EOF 終了）終了を求めます。`close()` は標準入力を閉じ、子プロセスと読み取りスレッドの終了を待ち、残った一時要求ファイルを削除します。 |

| `RunHandle` のメンバー | 内容 |
|---|---|
| `id`、`run_id` | ランチャーが付けた不透明な実行 ID です。 |
| `address` | この実行の Control Server のアドレスです（この実行の `control_server` イベントを待って返します）。 |
| `start_event`、`result_event`、`events`、`participant_pids`、`result`、`status`、`launcher_ends` | `run_start` イベント、終了後の `result` イベント、この実行のイベント一覧、この実行の `participant`/`started` イベントから participant 名を PID に写す `dict[str, int]`、`result` の dict と内包された確定済み `status`（`ExecutionStatus`）、この実行でランチャーが終わるかどうかです。 |
| `wait_for_event(event, timeout=60.0, after=None, **keys)` | この実行のイベントから、キーの値が合う最初のイベントを待ちます。 |
| `control(**options)` | `Control(run.address, run=run.id, **options)` を返します。`run=...` を渡すと `TypeError` になります。 |
| `send(command)` | この実行宛ての標準入力コマンド（`"run": run.id`）を送ります。 |
| `stop(force=None, *, id=None)` | この実行の `stop` を送ります（`False` で順序どおり、`True` で強制、`None` で既定の順序どおり）。 |
| `wait(timeout=120.0)` | この実行の Events v4 `result` イベントを待ち、内側の `result` dict を返します（`result_event` がイベント全体を保持し、確定済みの `status` を内包して別個の `idle` イベントはありません）。 |

`Serve` は終わった実行の履歴を持たず、ストリーム全体の `events` リストも溜めません。実行の `result` イベントが届くと、`Serve` はその `RunHandle` への参照を外すため、呼び出し側がハンドルを手放した時点でその実行のイベントと結果が解放されます（`keep_logs=False` にすると `RunHandle.events` に `"log"` イベントを含めません）。ストリーム全体のイベントを監視したいとき（`keep_logs=False` でも `"log"` イベントを受け取れます）は `on_event=callback` を渡します。`on_event` は読み取りスレッド上で `Serve` のロックの外から順に呼ばれ、`Serve` が状態を更新して待機中の呼び出しを起こす前に完了します（そのため `Serve()`、`prepare()`、`start()`、`run.wait()`、`close()` が返った時点で対応するイベントは配信済みです）。各呼び出しにはイベントの浅いコピー（`dict`）を渡します。入れ子の値は読み取り専用として扱い、コールバック内では `close()`、`wait()`、およびイベントを待つブロッキング呼び出し・プロパティ（`prepare()`、`status()`、`start()`、`run.wait()`、`run.wait_for_event()`、および該当イベントの処理前に `hello`、`instance`、`run.address`、`run.control()` を読む操作）を行わないでください（呼び出すと `ServeError` になります）。`on_event` が例外を上げても読み取りスレッドは止めず、`sys.excepthook` へ渡します。

`with runner.start(...)` を抜けると、その実行だけを止めて（`force=False`）確定済みの `status` を持つ `result` を待ち、`once=True` や `launcher_ends` が真でない限り `Serve` は次の実行のために生かしたままにします。`launcher_ends: true`（たとえば汚染時の終了コード 6 や、`once=True` の 1 回実行）で終わった後は、次の実行を始められません。`with Serve(...)` を抜けると常駐ランチャーを閉じます。コマンドが拒否されたとき、時間切れのとき、または起動失敗・予期せぬ終了のとき（`once=True` なしの `VEHICLE_SIL_TRACE` を含み、`reason="trace-enabled"` で報告されます）は、`ServeError`（`LaunchError` のサブクラス）を上げ、理由があれば `reason` に設定します。メッセージには指摘の `detail`（`--describe` の標準エラーなど）を含み、プロセスの異常終了経路ではコマンド・終了コード・標準エラー末尾を含みます。

### 接続して読み書きする（`Control`）

別の PC から、TLS とトークンを付けたランチャー（上の「認証」）に接続するときは、`token` と `root_certificates` を渡します（`root_certificates` なしでシステムのルート証明書を使うときは `tls=True`。証明書の作り方と、接続できないときのメッセージは [howto/multi-pc.md](howto/multi-pc.md)）。

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

時刻は、仮想時間のナノ秒です（`ms()`、`s()` で作ります）。指定時刻がすでに過ぎていた場合に次のステップで反映するには `c.write(..., at=..., late="apply_next")` を渡します（既定は `"reject"`）。`c.watch(..., timeout=5.0)` のように `timeout`（実時間の秒数）を渡すとストリームに期限を付けられます（`None` は無期限、期限切れは `ControlError` の `DEADLINE_EXCEEDED`）。

値の読み取り（`c.values()`、`c.snapshot()`、`c.watch()`）は `Values` を返し、`.values` dict に変数名と Python の値（`bool`、`int`、`float`）が入ります。列挙変数の整数値から宣言済みの項目名を得るには `vals.text(name)` を使います（変数が存在しない・列挙でない・一致する項目がないときは `None`）。`c.values(peer, builtin=False)` や `c.snapshot(peers=(), builtin=False)` は、アプリケーションが宣言した変数だけ（`not v.builtin`）に絞るフィルタです。`builtin=False` はアプリ宣言変数フィルタであり（`ecu.connected`、`peer.steps`、`j1939.*` などの決定的な組み込み変数も除かれます）、実行間の決定性を比較するときは `builtin=True` のまま、壁時計に依存する 2 変数（`peer.speed` と `peer.app_time_max_us`）だけを除外してください。

| 起きたこと | Python での現れ方 |
|---|---|
| 呼び出しの失敗 | 例外 `ControlError` です（[エラー](#エラー)）。 |
| Agent が拒否した書き込み | 例外ではなく、操作の結果（`invalid` など）になります。 |

`Control` を 1 つの実行にとどめるには（[宛先の実行](#宛先の実行)）、実行に結びつけます。結びつけた `Control` はすべての呼び出しで実行を名乗り、アドレスのサーバーが別の実行のものになると、`ping()` と `current_run()` 以外の呼び出しは `ControlError`（`reason` は `run-changed`）になります。新しい実行には新しい `Control` を作ります。複数回実行の `vehicle_sil_launch serve` に対して実行未指定の `Control(address)` を使うと `reason` `run-required` で拒否され、`ControlError` のメッセージに `Control.for_current_run()` / `Control(..., run=...)` を案内するヒントが付きます（`Serve` を使う場合は `run.control()` を使ってください）。

```python
from vehicle_sil_control import Control

with Control.for_current_run("127.0.0.1:8510", token=token) as c:   # the run the server belongs to now
    print(c.run)                                                     # its ID
    c.sim.stop()                                                     # stops that run, never a later one
```

| メンバー | 内容 |
|---|---|
| `Control(address, ..., run=<ID>)` | すべての呼び出しで実行 `<ID>` を名乗ります（メタデータ `vehicle-sil-run`）。`run=None`（既定）では名乗りません。 |
| `Control.for_current_run(address, ...)` | サーバーの今の実行に結びつけた `Control` です（`Ping` を 1 回送ります）。引数は `run` を除いて `Control()` と同じです。実行を持たないサーバーでは `ControlError`（`FAILED_PRECONDITION`）です。 |
| `run` | 結びつけた ID か `None` です。 |
| `current_run()` | サーバーの今の実行の ID です（`PingReply.run`。なければ空）。実行を理由に断られることはありません。`run` と比べると、`Control` がまだ自分の実行に届くかが分かります。 |

`vehicle_sil_control` は、ランチャーと同じ vehicle-sil のリリースのものを使います。`Control` を作っただけでは接続しません。最初の呼び出しか watch のときに、`Control` は `Ping` を 1 回送ってサーバーのリリースを確かめます。別のリリースのサーバーは、`ControlError`（`code` は `FAILED_PRECONDITION`）で断ります。メッセージは `the control server is vehicle-sil <X>; this vehicle_sil_control is <Y>; use the vehicle_sil_control of the same release as the launcher` です。`Ping` が失敗したとき（サーバーがまだない）は、次の呼び出しでもう一度確かめます。

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
- `step()` の止まる時刻は、そのレスポンスにあります: 進んでいる途中なら `pause_target`、そこで止まった後なら `now`（`paused` は真）です。標準入力やほかのクライアントが同時に時間のコマンドを送ると、レスポンスはそのコマンドの後の状態のことがあります。ランチャーの `command` のイベントには、step ごとにその step の時刻（`at_ms`）があります。
- 開始後の一時停止中に `sim.pause(at=T)` を呼ぶと、再開せずに `pause_target` だけを更新し、`sim.resume()` は予定されていた `pause_target` を消します（「後の呼び出しが勝つ」規則）。一時停止中・実行中のどちらからでも `T` まで進めて止めるには `sim.run_until(T)` を使います。
- `TimeState.running` が偽のとき、`ended` が真なら終わっていて、偽ならまだ始まっていません。

### Peer と変数の情報（`peers()`、`watch()`）

`peers()` と `watch()` は、同じ Peer を同じ内容で返します。

| Peer のフィールド | 意味 |
|---|---|
| `has_agent` | Agent が接続したことがあるかです（既定は偽）。 |
| `simulation_state` | SIL Kit の状態です。 |
| `virtual_time` | Agent がシミュレーションの時刻で動くかです（Agent の hello のとおり。偽は実時間の Peer か、Agent がない）。 |

| 変数（`schema` の各要素）のフィールド | 意味 |
|---|---|
| `builtin` | vehicle-sil が付けた変数か（真）、アプリケーションが宣言した変数か（偽）です。 |
| `source` | 設定ファイルの対応表（`participants[].variables`）が適用されている変数の、実行ファイルでの名前です（適用されていなければ空）。 |

- `builtin` は、Server が名前で決めます（名前が `ecu.`、`peer.`、`j1939.`、`can.`、`lin.`、`eth.` で始まれば真）。アプリケーションが `j1939.` で始まる名前の変数を宣言すると、それも真になります。

`PeerOptions::monitor`（既定で有効）がオンのとき、SIL Kit の各 `Peer` は実行時に以下の読み取り専用の監視変数（`builtin: true`）を追加します。Control API とブラウザの GUI から参照でき、`--describe` やシナリオでは使えません。

| 変数 | 型 / 単位 | 意味 |
|---|---|---|
| `peer.steps` | int | その Peer が実行したステップ数です。 |
| `peer.speed` | float（`s/s`） | 前回の読み取りからの、実時間 1 秒あたりのシミュレーション秒数です（初回は `0`。一時停止中も含みます）。 |
| `peer.app_time_max_us` | float（`us`） | 前回の読み取りからの、1 ステップ内のアプリケーションと LIN の処理の最長所要時間です（J1939 処理とコマンド適用を除きます）。 |
| `j1939.<net>.bus_state` | enum | CAN コントローラーのエラー状態（`unknown`、`error_active`、`error_passive`、`bus_off`）です。 |
| `j1939.<net>.<counter>` | int | ネットワーク統計カウンター（`frames_received`、`frames_sent`、`send_failures`、`send_queue_full`、`receive_overflows`、`held_dropped`、`address_violations`、`transport_aborts_sent`、`transport_aborts_received`、`transport_timeouts`、`transport_rejected`、`transport_bam_dropped`、`transport_bam_replaced`）です。 |
| `j1939.<net>.node<i>.address` | int | J1939 ノード `i` の現在のアドレスです。 |
| `j1939.<net>.node<i>.address_state` | enum | Address Claim の状態（`fixed`、`not_started`、`claiming`、`claimed`、`cannot_claim`）です。 |

`--map-network` を指定した場合、`<net>` には対応付け後のシミュレーションネットワーク名が入ります（詳しくは [internals/control.md](internals/control.md) の「Peer の監視」）。

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

参加者の値の時刻（`Snapshot.time_ns`、Python の client の `Values.time`）は、その値を終わりに読んだ ECU のステップが始まった時刻です。[シナリオの「値の見え方」](scenario.md#値の見え方)と同じです。

```python
c.sim.step(ms(100)); c.sim.wait_paused()
snap = c.snapshot()                       # 全 Peer。c.snapshot(["Engine", "Body"]) で絞る
if snap.consistent:                       # 値は、止まっている時刻 snap.time.now のもの
    print(snap.time.now, snap.values["Engine"].values["engine.speed"])
```

`snapshot(peers=(), builtin=True)`（gRPC の `GetSnapshot`）は、時間の状態（`sim.time()` と同じ `TimeState`）と、Peer ごとの最後の値（`values()` と同じ `Values`。列挙名逆引きの `Values.text()` も利用可能）を、Server が 1 回で読んで返します。`builtin=False` を渡すとアプリケーション宣言変数のみ（`not v.builtin`）に絞れます。実行間の決定性を比較する場合は `builtin=True` のまま、壁時計の 2 変数（`peer.speed` と `peer.app_time_max_us`）だけ除外してください。Server は、時間の状態を先に読み、その後で全 Peer の値を同じ瞬間に読みます。

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

## CLI

`--server`（既定は `VEHICLE_SIL_CONTROL_SERVER`、未設定時は `127.0.0.1:8510`）に、ランチャーのイベント `control_server` の `address`（人向けの出力では `control server at` の行）を渡します。`--token`（既定は `VEHICLE_SIL_CONTROL_TOKEN`）、`--root-cert`（既定は `VEHICLE_SIL_CONTROL_ROOT_CERT`）、`--run <ID>`（既定は `VEHICLE_SIL_CONTROL_RUN`）で、認証、TLS、コマンドの宛先の実行を指定します（[宛先の実行](#宛先の実行)）。`serve` のランチャーでは `--run` が必須です。コマンドと gRPC の呼び出しの対応は、[呼び出しの一覧](#呼び出しの一覧)にあります（`set` は `--apply-late` と `--wait none|queued|done`、`pause` は `--at-ms` と `--wait`、`watch` は `--no-values`、`stop` は `--reason` も取ります）。

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
| `reason` | Server が理由を付けて拒否したときの、その語です（trailing metadata の `vehicle-sil-reason`）。`run-changed`、`run-required`、`scenario-running`、`outside-real-time` と、今はできない時刻のコマンドのほかの理由です（なければ空）。 |

| 状態コード | 返るとき |
|---|---|
| `INVALID_ARGUMENT` | 引数のエラーです（範囲の外の速度、0 以下のステップ、`snapshot()` の名前が 1024 個を超える）。 |
| `FAILED_PRECONDITION` | 今はできない呼び出しです（一時停止していないときの `step`、終わった後の `Stop`、引き継ぎ前のシナリオの実行中の書き込み、外につながっていて実時間を保つ実行での一時停止や速度の変更）か、サーバーと別の実行宛ての呼び出し（`run-changed`）や、実行が必須のサーバーへの実行を名乗らない呼び出し（`run-required`）です。Python のクライアントは、別の vehicle-sil のリリースのサーバーにも出します。 |
| `UNAUTHENTICATED` / `PERMISSION_DENIED` | `--control-credentials` が有効なときに、Bearer トークンが欠落・不正（`UNAUTHENTICATED`）またはロールが不足（`PERMISSION_DENIED`）しています。 |
| `CANCELLED` | 待っている間に、サーバーが閉じました（`WaitPaused`。`FAILED_PRECONDITION` のこともあります）。 |
| `DEADLINE_EXCEEDED` | 呼び出しの実時間期限が切れました（`Control.watch(..., timeout=...)` や応答遅延など）。 |
| `RESOURCE_EXHAUSTED` | `snapshot()` のレスポンスが、1 つのメッセージの上限（4 MiB）を超えました。 |
| `UNIMPLEMENTED` | そのアドレスは gRPC に答えますが、vehicle-sil の Control Server ではありません（その呼び出しがない）。 |
| `UNAVAILABLE` / `NOT_FOUND` | サーバーへ接続できないか、`values()` / `snapshot()` 中に Peer が再起動し続けているとき（`UNAVAILABLE`）、または `peer(name)` で指定した名前の Peer がないとき（`NOT_FOUND`）。 |

シナリオの実行中（`after_scenario: "pause"` で引き継ぐ前）の書き込みは、`reason` が `scenario-running` です。

| Python の例外と警告 | 出るとき |
|---|---|
| `ControlError` | 呼び出しが失敗しました（`code` と `reason` に失敗の内容が入ります）。 |
| `LaunchError` | ランチャーが結果を出力せずに終わったか、待つイベントが来ないまま実行が終わりました。 |
| `ServeError` | `LaunchError` のサブクラスです。常駐 `serve` のコマンド（`prepare`、`start`、`status`）が拒否・時間切れになったか、常駐ランチャーが起動失敗または予期せず終了しました（`reason` に `"trace-enabled"` や `"busy"` などの拒否・失敗コードが入ることがあります）。 |
| `ControlWarning` | `peers()` を呼んだとき、Server がシミュレーションの状態を読めていません。 |

## 制限

| 項目 | 制限 |
|---|---|
| 時間の操作（一時停止、ステップ実行、速度）で止まる Peer | 仮想時間で同期している Peer だけで、実時間の Peer は動き続けます。 |
| 速度を指定したときの遅れ | シミュレーションが実時間に遅れても、後から追いつこうとはしません。 |
| ない操作 | リセット、participant の起動と再起動、CAN/LIN のチャネルの選択はありません。 |
| サーバーが扱う実行 | 1 回の実行だけで、実行が終わると閉じます。`serve` では、実行ごとに別のサーバーがあります。 |
| 値の購読 | 最新値です（読む前に来た新しい値で置き換えます）。 |

- 速度を指定して遅れたときは、遅れた分だけ後ろにずれたまま、指定の速度で進みます（[internals/virtual-time.md](internals/virtual-time.md#遅れの測り方) の「遅れの測り方」）。
- participant の失敗は、シミュレーションの状態（`watch(simulation=True)` の `failed_participant` と `failure_reason`）で読みます。
- CAN / J1939 の生フレームを取りこぼしなく記録するには、`ecu_main` の ECU では `spy::Scope::on_frame` または `ecu.can().on_frame()` を使います（同一プロセス内で `Peer` や `LocalBench` を直接使う場合は `Peer::tap()` / `TapPort` も利用できますが、`Ecu` からは呼べません）。SIL Kit の `TraceSinks` は CAN ではそのまま使えません。同梱の `PcapFile` / `PcapPipe` は Ethernet 専用で CAN フレームに対して例外を投げ、`Mdf4File` には vehicle-sil に同梱されていない Vector 外部拡張 `SilKitExtension_Mdf` が必要です。

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
