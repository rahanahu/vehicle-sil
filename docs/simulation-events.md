# 機械から使う（`--events`、`--stdin-commands`、`version`）

GUI やスクリプトなど、`vehicle_sil_launch` を起動して見届けるプログラムを書く人向けです。先に [simulation-run.md](simulation-run.md)（実行、止め方、結果）を読みます。この後に読む文書はありません。

`run --events` は、実行の経過を事象として標準出力に書き、最後に結果を書きます。`run --stdin-commands` は、標準入力から命令を読みます。`version --json` は、この `vehicle_sil_launch` が書く形式の版と、できることの一覧です。

```sh
vehicle_sil_launch version --json
vehicle_sil_launch check --json simulation.yaml
vehicle_sil_launch run --events --stdin-commands simulation.yaml
```

ブラウザの GUI の試験は、この出力を録ったもの（`gui/server/tests/recordings/`）を流し直します。ランチャーの出力を変えたら、`python3 gui/server/tests/record_launcher.py --launcher <vehicle_sil_launch> --compare` で比べ、違う場面を録り直します（[gui.md](gui.md) の「開発」）。

## 事象（`run --events`）

標準出力に、1 行に 1 つの JSON のオブジェクトを、起きた順に書きます（NDJSON）。文字コードは UTF-8、改行は `\n` で、行の中に改行はありません。1 行ごとに書き出します。事象を書くのは `run` のときだけです。どの行も、次の 3 つのキーで始まります。

| キー | 値 |
|---|---|
| `event` | 事象の種類 |
| `seq` | 0 から 1 ずつ増える整数。書いた行の順と同じで、抜けません。読む側は、行を取りこぼしていないことを確かめられます |
| `t_ms` | 事象が起きたときの、`vehicle_sil_launch` が始まってからの実時間（ミリ秒、整数）。行の順に見て、戻りません |

| `event` | いつ | ほかのキー |
|---|---|---|
| `hello` | 最初の行 | `vehicle_sil_events`（事象の形式の版。今は 1）、`vehicle_sil`（vehicle-sil の版）、`pid`（`vehicle_sil_launch` のプロセス ID）、`file`（構成ファイル。引数に書いたとおり。わからなければ `null`）、`arguments`（受け取った引数の配列） |
| `phase` | 段階が変わったとき | `phase`: `checking`（照合）、`starting`（registry、controller、participant の起動）、`waiting`（必須の participant がそろうのを待つ）、`running`（シミュレーションが動いている）、`stopping`（participant を終わらせている）、`ended`（すべて終わった）。この順に進み、戻りません。飛ばすことがあります（照合で失敗すれば `checking` の次は `ended`） |
| `check` | 照合が終わったとき | `check`: `check --json` と同じオブジェクト（[JSON](simulation-check.md#json--json)） |
| `control_server` | `--control` の Control Server が待ち受けを始めたとき（`registry` の前） | `address`: クライアントがつなぐアドレス（ポート 0 なら選ばれたポート）、`tls`: TLS か、`credentials`: 呼び出しにトークンが要るか |
| `registry` | registry が待ち受けを始めたとき | `uri`: participant が接続する URI |
| `participant` | participant の状態が変わったとき | `name`、`state`（下の表） |
| `control_link` | シミュレーションが始まった直後（`phase` の `running` の前）に、起動した participant と、利用者が起動する（manual の）participant ごとに 1 回 | `name`、`state`（結果の `participants[].control_link.state` と同じ 5 つ）。`connected` には `variables`、`version_mismatch` には `version`（結果の同じ名前のキーと同じ意味。ほかの `state` にはこの 2 つのキーがありません） |
| `log` | participant か `vehicle_sil_launch` が 1 行書いたとき | `source`（participant 名か `vehicle_sil_launch`）、`line`（その行。[`log` の行](#log-の行)） |
| `log_dropped` | 読む側が遅くて `log` を書けなかったとき、その場所に（[読む側が遅いとき](#読む側が遅いとき読まないとき)） | `count`（書かなかった `log` の数）、`sources`（`source` ごとの数のオブジェクト。例: `{"Engine": 120, "vehicle_sil_launch": 1}`） |
| `finding` | 実行で見つけたものが決まったとき | 結果の `findings[]` の 1 つと同じキー（`severity`、`code`、`file`、`line`、`participant`、`network`、`message`、`detail`） |
| `scenario_step` | `--scenario` の手順が始まったとき、決まったとき | 結果の `scenario.steps[]` の 1 つと同じキー（[scenario.md](scenario.md) の「出力」）。始まったときの `status` は `running`。飛ばす手順（`status` は `skipped`、`at_ms` は null）は 1 回だけ |
| `time` | 仮想時間が進んだとき、一時停止に入ったとき・出たとき、速さが変わったとき | `sim_ms`: controller が見ている仮想時間（ミリ秒。減りません）、`speed`: 今の速さ（数か `"max"`）、`paused`: 一時停止しているか（`true` の事象は、ふつう、止まった後に出ます。`--start-paused` の最初の `time` も、時刻 0 で止まってから出るので、結果の `paused_ms` は、この事象より前から数えています。例外は 2 つで、どちらも、止まる予定の時刻に着いているのに、まだ止まっていない間に `true` が出ます: 始まった後 2 秒たっても時刻 0 で止まらなかったとき（待つのをやめて出します）と、時刻を指定した `pause` や `run_until` が今の時刻以下を指していて、次の刻みで止まるとき。このときの `paused_ms` は、実際に止まってから数えます）、`actual_speed`: 直近 1 秒ほどの、進んだ仮想時間 ÷ 実時間（小数 3 桁。動いた時間が短いとき、一時停止しているときは `null`）、`late_steps`: それまでに遅れた刻みの数、`max_late_ms`: それまでの最大の遅れ、`slipped_ms`: それまでのずれの合計（どれも結果の `timing` と同じ意味で、いま実行が終わったとしたときの値）、`late_ms`: 期限を過ぎてもまだ始まっていない刻みの、いまの遅れ（なければ 0）。シミュレーションが止まっている間（仮想時間が進まず、`late_ms` が許容を超えて増えている間）も、100 ms に 1 回まで出ます |
| `command` | 標準入力から命令を受けたとき（`--stdin-commands`）、Control Server のクライアントが時間の操作か停止を呼んだとき（`--control`） | `command`（命令の名前。読めなければ `null`）、`source`（`"stdin"`: 行で来た、`"eof"`: 標準入力の終わり、`"control_server"`: クライアントの呼び出し）、`accepted`（受け付けたか）、`message`（受け付けなかった理由。受け付けたら `null`）。受け付けた `step` には `ms`（実際に進む仮想時間。刻みの倍数に切り上げたもの）。クライアントからは `run_until` と、時刻つきの `pause` も来ます。受け付けたものには `at_ms`（止まる仮想時刻。刻みの倍数に切り上げたもの） |
| `heartbeat` | 1 秒のあいだ何も書かなかったとき | なし（3 つのキーだけ） |
| `result` | 最後の行 | `result`: `run --json` と同じオブジェクト（[JSON](simulation-run.md#jsonrun---json)） |

`participant` の `state`:

| `state` | 意味 | ほかのキー |
|---|---|---|
| `disabled` | 外した participant（起動しません） | |
| `not_started` | プロセスを起動できなかった | `message`: 理由 |
| `started` | プロセスを起動した | `pid`（そのプロセスの ID）、`command`（起動した実行ファイルと引数の配列）、`log`（`--log-dir` のファイル。なければ `null`） |
| `manual` | 利用者が起動する participant なので、起動しなかった（`started` の代わり。[participant を自分で起動する](simulation-config.md#participant-を自分で起動するmanual--manual)）。この後、参加すれば `joined` が来ます。`ended`、`stop_requested`、`killed` は来ません | `command`（起動する実行ファイルと引数の配列）、`working_directory`（起動するディレクトリ）、`command_line`（同じものを、この OS のシェルに貼れる 1 行にしたもの。標準エラーに書く行と同じ）、`environment`（起動するときに設定する環境変数の名前と値のオブジェクト。`registry_uri` がループバックのアドレスでないときだけあり、`{"VEHICLE_SIL_LISTEN_ON_NETWORK": "1"}`。[待ち受けるアドレス](simulation-run.md#待ち受けるアドレス)） |
| `joined` | シミュレーションに参加した（controller がその participant のライフサイクルの状態を見た） | |
| `failed` | `vehicle_sil_launch` が、この participant の失敗で実行をやめると決めた | `reason`: `left_simulation`（プロセスは動いているが、シミュレーションから抜けた。プロセスが終わるときは、接続が先に切れるので、終わりより先にこちらで気づくこともあります）、`process_ended`（シミュレーションの途中で、プロセスが失敗として終わった）、`ended_before_start`（シミュレーションが始まる前にプロセスが終わった）。`message`: この失敗の `finding` の `message` と同じ文。その時点で文が決まっているときだけあります（`left_simulation` にはありません） |
| `stop_requested` | 止まるよう求めた（止め方の 2） | |
| `ended` | プロセスが終わった | `exit_code`、`signal`（結果の `participants[]` と同じ意味） |
| `killed` | 強制終了した（それが起動したプロセスも） | |

### 順序の約束

- 最初の行は `hello`、最後の行は `result` です。`result` の後には何も書きません。
- 外した participant の `disabled` は、`check` の直後、`starting` の前に、構成ファイルの順に書きます。
- participant の `log` は、その participant の `started` より後に来ます。終わりの側には約束がありません。participant の出力は別のスレッドが読んでいるので、最後の何行かが、その participant の `ended` の後や、`phase` の `ended` の後に来ることがあります。`result` より後には来ません。
- `joined` は、SIL Kit のライフサイクルで参加する participant だけです（参加しない道具の participant には来ません）。
- `control_link` は、必須の participant の `joined` がそろった後、`phase` の `running` の前に、構成ファイルの順に書きます（起動できなかった participant と、外した participant にはありません）。
- `manual` は、`started` と同じ所に（構成ファイルの順に）書きます。その participant には `log` が来ません（出力は、利用者が起動した所に出ます）。シミュレーションが始まらなかったときは書きません。
- `failed` は、失敗と決めた時点ですぐに書きます。ほかの participant を止める（`phase` の `stopping`）より前で、その失敗の `finding` より前です。`log` と違って、読む側が遅くても捨てません。`reason` が `left_simulation` のときは、その participant の `ended` より前に来ます。`process_ended` と `ended_before_start` は、プロセスの終わりで失敗に気づくので、`ended` の後に来ます。`failed` を知らない読む側は、無視してかまいません（結果と、ほかの事象は変わりません）。
- `time` は、`running` になったときに 0 を書きます。その後は、仮想時間が進んだときに実時間で 100 ms に 1 回まで、一時停止に入ったとき・出たとき・速さが変わったとき・一時停止したまま時刻が変わったとき（ステップ送りの後）はすぐに書きます。シミュレーションが終わったときの状態は、間隔によらず書きます（終わったシミュレーションは一時停止していません）。`sim_ms` は減りません（一時停止や速さの変更では、前と同じ値のことがあります）。見張りが 20 ms ごとに読むので、それより短い間の変化（最速でのステップ送りの途中など）は書かないことがあります。最後の状態は必ず書きます。
- `finding` は実行で見つけたものです。シミュレーションが終わった（または始まらなかった）ときに、その時点で決まっているものを書き、残りを participant を終わらせた後に書きます。一度書いた `finding` は、同じ内容のまま `result` の `findings` に入ります。同じものを 2 回は書きません。シミュレーションの途中で失敗した participant の `participant-failed` は、プロセスの終わり方を含むので、プロセスが終わってから書きます（失敗に気づいた時点では、`participant` の `failed` が知らせます。`vehicle_sil_launch` の `log` の行にも理由があります）。照合で見つけたものは `check` の中にあります。`result` だけを読んでも、結果はわかります。
- `heartbeat` は、`hello` から `result` までのどの段階でも、ほかの行の間に入ります。数は実行にかかった時間で変わります。
- `command` の事象は、その命令で起きること（`phase` の `stopping` など）より前に書きます。標準入力とクライアントの時間の命令は、1 つずつ順に処理するので、`command` の事象の順が、効いた順です。
- `control_server` は、`phase` の `starting` の後、`registry` より前に 1 回だけ書きます（照合で失敗した実行と、引数の誤りでは書きません）。

### 実行に進まないときの事象

`--events` を付けた `run` は、引数が誤っていても `hello` を最初に、`result` を最後に書きます（`heartbeat` が間に入ることがあります）。

| 場合 | 事象 | `result` の `result` | 終了コード |
|---|---|---|---|
| 引数の誤り（知らないオプション、構成ファイルの指定がない、`--json` と一緒に付けた、など） | `hello`、`result` | `"usage"` | 4 |
| 構成ファイルを読んでわかる引数の誤り（`--disable`、`--enable`、`--manual` の名前が構成ファイルにない、機種を書いた構成に `--variant` がない、`--variant` の名前が構成ファイルにない） | `hello`、`phase`（`checking`）、`check`、`phase`（`ended`）、`result` | `"usage"` | 4 |
| 構成ファイルを読めない | 同じ 5 つ | `"unreadable"` | 3 |
| 照合で誤りが見つかった | 同じ 5 つ | `"failed"` | 1 |
| 標準出力がない（閉じた状態で起動された） | 何も書けません。標準エラーに誤りを書きます | | 4 |

`run` でないコマンド（`check`、`version`、コマンドなし）に `--events` を付けたときは、事象を書きません。引数の誤りとして標準エラーに書き、終了コードは 4 です。

### `log` の行

`line` は、participant が標準出力か標準エラーに書いた 1 行（行末の改行なし）、または `vehicle_sil_launch` の経過の 1 行です。`--events` なしで標準エラーに書く行から、前の `[<名前>] ` を除いたものと同じです。

- 制御文字（タブを除く）と、UTF-8 として正しくないバイトは、`?` に置き換えます。
- 4096 バイトを超える行は、4096 バイト以下の複数の `log` に分けます（UTF-8 の文字の途中では切りません）。続きであることを示すキーはありません。
- `--log-dir` のファイルには、置き換える前のバイトをそのまま書きます。読む側が遅くて `log` を書けなかったときも、ファイルにはすべての行があります。

### 読む側が遅いとき、読まないとき

`vehicle_sil_launch` は、事象を読む側を待ちません。事象を書くのは専用のスレッドで、実行を進めるスレッド、participant の出力を読むスレッド、標準入力を読むスレッドは、事象を待ち行列に積むだけです。読む側が遅くても、止まっていても、次のことは変わりません。

- participant は止まりません（出力を読み続けます）。`--log-dir` のファイルも、今までどおりに書きます。
- `--timeout-ms`、シグナル、標準入力の `stop` は効きます。

書けずに待っている事象が増えたときは、次のようにします。

- `log` は、待っている `log` が 4096 行または 1 MiB を超える間は、書かずに数えます。数えた分は、その場所に `log_dropped` を 1 行書いて知らせます（読む側がまた読み始めたときに届きます）。`count` と `sources` は、その行を書く時点までに数えた分です。
- `log` と `log_dropped` 以外の事象は、捨てません。ただし、仮想時間が進んだことだけを知らせる `time` は、ほかの事象が書けずに待っている間は作りません（一時停止と速さの変化を知らせる `time` は作ります）。
- 書き込みが 30 秒のあいだまったく進まないとき、または `log` 以外の事象が 4096 個を超えて待っているときは、読む側がいなくなったものとして扱います（次の項）。

「30 秒進まない」の数え方:

- 数え始めるのは、読む側が読むのをやめたときではなく、書き込みが待ち始めたとき（パイプがいっぱいになったとき）です。書く量が少ない実行（`heartbeat` だけ、など）では、パイプがいっぱいになるまでに長くかかります。読むのをやめて 30 秒で必ず止まるわけではありません。
- `vehicle_sil_launch` は 4096 バイトまでの単位で書き、その単位が 1 つ入りきると「進んだ」と数えます。読む側は、30 秒のうちにおよそ 4096 バイトを読めば、読んでいると見なされます（待たない設定のパイプでは 1 バイトで足ります）。
- 30 秒は `--events-stall-ms <ms>` で変えられます（1000 以上。デバッガーで止めることがある読む側は、長くします）。端末に書いているときも同じです（出力を止めたままにすると、実行が止まります）。

シミュレーションが終わると、`vehicle_sil_launch` は `result` を書き終えるまで待ちます。待つのは、読む側が読んでいても長くて 30 秒（`--events-stall-ms` の値）です。待っている事象が多く、読む側が遅いと、`result` が届かないことがあります。書き込みが 0.5 秒進まない間に、止める要求が 2 回以上になったとき（それまで要求がなければ 1 回目が来たとき）は、そこで待つのをやめます。`result` を書けなかったときの終了コードは 3 で、標準エラーに `error: cannot write the result to the standard output (<理由>)` と書きます（止める要求で止めた実行は 130 で、この行は書きません）。

### 読む側がいなくなったとき

標準出力を読む側がいなくなった（パイプが閉じた）ことには、次に行を書くときに気づきます。`heartbeat` があるので、どの段階でも 1 秒以内です。上の「読まない」の条件に当てはまったときも、同じ扱いです。

- 事象の流れを閉じます。その後は、`result` を含めて何も書きません（書きかけの行が途中で切れることがあります）。
- 止める要求として扱います（1 回目の Ctrl+C と同じ）。すでに止める要求を受けていれば、何もしません（2 回目に数えません）。
- 標準エラーに理由を 1 行書きます（`vehicle_sil_launch: cannot write the events (<理由>): stopping the run`）。すでに止める要求を受けていたときは、行の終わりが `no more events are written` です。`result` を書くのを待っている間に閉じたときは、この行ではなく、上の `error: cannot write the result ...` の行です。標準エラーも読まれていないと、この行は届きません（`vehicle_sil_launch` は、それでも終わります）。
- 結果の `interrupted_by` は `"output closed"`、終了コードは 130 です（SIGPIPE で終わりません）。

標準出力が、書き込みで待たない設定（`O_NONBLOCK` など）のパイプでも同じように動きます。いっぱいのときは、閉じたとは扱わず、空くのを待ちます。

### 読む側が守ること

- 標準出力を読み続けます。`result` の行が来たら、流れは終わりです。
- 改行で終わらない最後の行は捨てます（`vehicle_sil_launch` が強制終了された、流れが閉じた）。
- `result` が来ないまま標準出力が終わったら、`vehicle_sil_launch` が結果を書けずに終わっています（強制終了、クラッシュ、上の「いなくなった」の扱い）。プロセスの終了コードで判断します。
- `heartbeat` を含めて何の行も数秒来なければ、`vehicle_sil_launch` が止まっています（1 秒に 1 行は書きます）。
- 知らない `event` と知らないキーは無視します。`event`、キー、`state` と `phase` の値は、版を上げずに増えることがあります。キーの意味が変わるとき、なくなるときは `vehicle_sil_events` を上げます。
- 標準エラーも読みます（読み捨ててもかまいません）。`--events` のとき、標準エラーに書くのは `vehicle_sil_launch` 自身の知らせ（引数の誤り、事象を書けなくなった、結果を書けない、想定外の失敗）だけで、participant の行も経過も書きません。`vehicle_sil_launch` の中のライブラリが標準出力に書いたものは、標準エラーに行きます（事象の行に混ざりません）。
- `vehicle_sil_launch` を起動して見届けるプログラム（GUI、CI のスクリプト）は、`--stdin-commands` も付けます。そのプログラムが止まるか終わると標準入力が閉じるので、`vehicle_sil_launch` は標準出力の状態によらず、すぐに順に止まります。

### そのほかの決まり

- 文字列のエスケープは `--json` と同じです（`"`、`\`、制御文字。UTF-8 でないバイトは U+FFFD）。`log` の `line` は、上のとおり先に `?` に置き換えています。
- 行の長さに上限はありません（`check` と `result` は大きくなります）。
- `--json` と `--events` を一緒に付けるのは引数の誤りです（事象の最後が結果なので）。
- 終了コードは `--events` なしと同じです。

### 例

短い実行の全行です（`--disable Dashboard` で、出力の順が決まる Engine だけにしています）。時刻、プロセス ID、registry のポート、例のディレクトリ、版、時間の進み方は、実行ごとに違うので `<t_ms>`、`<pid>`、`<port>`、`<dir>`、`<version>`、`<wall_ms>`、`<actual_speed>` にしています（Windows では、実行ファイルの名前に `.exe` が付きます）。

<!-- vehicle_sil_launch run simulation.yaml --events --disable Dashboard -->
```text
{"event":"hello","seq":0,"t_ms":<t_ms>,"vehicle_sil_events":1,"vehicle_sil":"<version>","pid":<pid>,"file":"simulation.yaml","arguments":["run","simulation.yaml","--events","--disable","Dashboard"]}
{"event":"phase","seq":1,"t_ms":<t_ms>,"phase":"checking"}
{"event":"check","seq":2,"t_ms":<t_ms>,"check":{"vehicle_sil_check":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"stop_at_ms":1000,"start_timeout_ms":30000,"stop_timeout_ms":10000,"registry_uri":null,"participants":[{"name":"Engine","line":11,"command":["engine/vehicle_sil_engine_ecu"],"describe":true,"required":true,"enabled":true,"manual":false,"step_ms":10,"status":"described","reported":{"vehicle_sil":"<version>","name":"Engine","step_ns":10000000,"networks":[{"name":"J1939_CAN","kind":"can","use":"j1939","nodes":[{"address":0,"j1939_name":"0x00000000FE000001","arbitrary_address_capable":false}],"mac":null}]},"variables":[{"name":"ecu.connected","type":"bool","causality":"input","unit":"","min":null,"max":null,"step":null,"description":"on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on","items":[],"builtin":true},{"name":"engine.throttle","type":"real","causality":"input","unit":"%","min":0,"max":100,"step":null,"description":"accelerator pedal position","items":[],"builtin":false},{"name":"engine.speed","type":"integer","causality":"output","unit":"rpm","min":null,"max":null,"step":null,"description":"","items":[],"builtin":false}],"control":{"transport":"silkit","wire_version":1}},{"name":"Dashboard","line":14,"command":["dashboard/vehicle_sil_dashboard_ecu"],"describe":true,"required":true,"enabled":false,"manual":false,"step_ms":5,"status":"disabled","reported":null,"variables":null,"control":null}],"networks":[{"name":"J1939_CAN","kind":"can","line":8,"participants":["Engine"]}],"findings":[{"severity":"warning","code":"single-participant","file":"simulation.yaml","line":8,"participant":null,"network":"J1939_CAN","message":"network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it","detail":null},{"severity":"note","code":"participant-disabled","file":"simulation.yaml","line":14,"participant":"Dashboard","network":null,"message":"participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it","detail":null}]}}
{"event":"participant","seq":3,"t_ms":<t_ms>,"name":"Dashboard","state":"disabled"}
{"event":"phase","seq":4,"t_ms":<t_ms>,"phase":"starting"}
{"event":"registry","seq":5,"t_ms":<t_ms>,"uri":"silkit://127.0.0.1:<port>"}
{"event":"log","seq":6,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"registry at silkit://127.0.0.1:<port>"}
{"event":"log","seq":7,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"starting Engine: <dir>/engine/vehicle_sil_engine_ecu --name Engine --registry-uri silkit://127.0.0.1:<port> --start-timeout-ms 50000"}
{"event":"participant","seq":8,"t_ms":<t_ms>,"name":"Engine","state":"started","pid":<pid>,"command":["<dir>/engine/vehicle_sil_engine_ecu","--name","Engine","--registry-uri","silkit://127.0.0.1:<port>","--start-timeout-ms","50000"],"log":null}
{"event":"log","seq":9,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"not starting Dashboard: disabled"}
{"event":"phase","seq":10,"t_ms":<t_ms>,"phase":"waiting"}
{"event":"participant","seq":11,"t_ms":<t_ms>,"name":"Engine","state":"joined"}
{"event":"control_link","seq":12,"t_ms":<t_ms>,"name":"Engine","state":"connected","variables":18}
{"event":"phase","seq":13,"t_ms":<t_ms>,"phase":"running"}
{"event":"log","seq":14,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"simulation running"}
{"event":"time","seq":15,"t_ms":<t_ms>,"sim_ms":0,"speed":"max","paused":false,"actual_speed":<actual_speed>,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0}
{"event":"time","seq":16,"t_ms":<t_ms>,"sim_ms":1000,"speed":"max","paused":false,"actual_speed":<actual_speed>,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0}
{"event":"log","seq":17,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"the simulation ended"}
{"event":"phase","seq":18,"t_ms":<t_ms>,"phase":"stopping"}
{"event":"log","seq":19,"t_ms":<t_ms>,"source":"vehicle_sil_launch","line":"Engine exited with code 0"}
{"event":"participant","seq":20,"t_ms":<t_ms>,"name":"Engine","state":"ended","exit_code":0,"signal":null}
{"event":"phase","seq":21,"t_ms":<t_ms>,"phase":"ended"}
{"event":"result","seq":22,"t_ms":<t_ms>,"result":{"vehicle_sil_run":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"simulation":"reached_stop_at","interrupted_by":null,"timing":{"speed":"max","time_step_ms":10,"ecu_step_ms":10,"step_delay_ms":0,"late_tolerance_ms":10,"sim_ms":1000,"wall_ms":<wall_ms>,"actual_speed":<actual_speed>,"paced_steps":0,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0,"paused_ms":0},"participants":[{"name":"Engine","required":true,"enabled":true,"manual":false,"ended":"exited","exit_code":0,"signal":null,"left_processes":false,"log":null,"control_link":{"state":"connected","variables":18,"version":null,"late_messages":0}},{"name":"Dashboard","required":true,"enabled":false,"manual":false,"ended":"not_started","exit_code":null,"signal":null,"left_processes":false,"log":null,"control_link":{"state":"none","variables":null,"version":null,"late_messages":0}}],"findings":[],"check":{"vehicle_sil_check":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"stop_at_ms":1000,"start_timeout_ms":30000,"stop_timeout_ms":10000,"registry_uri":null,"participants":[{"name":"Engine","line":11,"command":["engine/vehicle_sil_engine_ecu"],"describe":true,"required":true,"enabled":true,"manual":false,"step_ms":10,"status":"described","reported":{"vehicle_sil":"<version>","name":"Engine","step_ns":10000000,"networks":[{"name":"J1939_CAN","kind":"can","use":"j1939","nodes":[{"address":0,"j1939_name":"0x00000000FE000001","arbitrary_address_capable":false}],"mac":null}]},"variables":[{"name":"ecu.connected","type":"bool","causality":"input","unit":"","min":null,"max":null,"step":null,"description":"on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on","items":[],"builtin":true},{"name":"engine.throttle","type":"real","causality":"input","unit":"%","min":0,"max":100,"step":null,"description":"accelerator pedal position","items":[],"builtin":false},{"name":"engine.speed","type":"integer","causality":"output","unit":"rpm","min":null,"max":null,"step":null,"description":"","items":[],"builtin":false}],"control":{"transport":"silkit","wire_version":1}},{"name":"Dashboard","line":14,"command":["dashboard/vehicle_sil_dashboard_ecu"],"describe":true,"required":true,"enabled":false,"manual":false,"step_ms":5,"status":"disabled","reported":null,"variables":null,"control":null}],"networks":[{"name":"J1939_CAN","kind":"can","line":8,"participants":["Engine"]}],"findings":[{"severity":"warning","code":"single-participant","file":"simulation.yaml","line":8,"participant":null,"network":"J1939_CAN","message":"network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it","detail":null},{"severity":"note","code":"participant-disabled","file":"simulation.yaml","line":14,"participant":"Dashboard","network":null,"message":"participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it","detail":null}]}}}
```

この例は、CTest の `launch_run.events` が、実際の出力から `heartbeat` の行と、最初と最後のほかの `time` の行（遅い機械では途中の時刻も書きます）を除き（`seq` は詰めます）、上の置き換えをした後で、バイト単位で照合します。

## 標準入力の命令（`run --stdin-commands`）

`--stdin-commands` を付けたときだけ、標準入力を読みます。付けないときは読みません（標準入力が閉じていても空でも、止まりません）。1 行に 1 つの JSON のオブジェクトです。`--events` と一緒に使います（[読む側が守ること](#読む側が守ること)）。単独でも使えます。

| 行 | 意味 |
|---|---|
| `{"command":"stop"}` | 順に止めます（1 回目の Ctrl+C と同じ）。2 回目は 2 回目の Ctrl+C と同じです（待つのをやめて、残っている participant を強制終了します） |
| `{"command":"pause"}` | 一時停止します。controller の刻みの境目で止まります（速さを合わせて待っている刻みがあれば、そこで。なければ次の刻みで） |
| `{"command":"resume"}` | 一時停止をやめて進みます。速さは、ここから数え直します |
| `{"command":"step","ms":<ms>}` | 一時停止している間だけ: 仮想時間を `<ms>`（1 以上 10^12 以下の整数）だけ進めて、また止まります。刻みの倍数に切り上げます（`command` の事象の `ms` が、実際に進む量です）。前の `step` の途中で送ると、その止まる時刻から数えます |
| `{"command":"speed","value":<速さ>}` | 速さを変えます（`<速さ>` は数か `"max"`。構成ファイルの `speed` と同じ範囲）。一時停止はそのままです |
| 標準入力の終わり（EOF） | `stop` を 1 回受けたのと同じです（起動した側がいなくなった、パイプを閉じた）。すでに止める要求（`stop`、シグナル、閉じた標準出力）を受けていれば、何もしません（2 回目に数えません。`command` の事象も書きません） |
| 空白だけの行 | 読み飛ばします |
| それ以外（JSON のオブジェクトでない、`command` がない、知らない命令、64 KiB を超える行） | 何もしません。`--events` なら `command` の事象（`accepted: false` と理由）、なければ標準エラーに 1 行 |

- オブジェクトのほかのキーは無視します。
- 時間の命令（`pause`、`resume`、`step`、`speed`）は、標準入力を読むスレッドが、受け付けるかを決めて `command` の事象を書き、それから controller の時間のゲートに渡します。どれも待ちません（実行を進めるスレッドも、事象を読む側も待ちません）。効くのは、controller の次の刻みの境目です。
- 時間の命令を受け付けないのは、`step` を一時停止していないときに送ったとき（`resume` の後など）、`step` を controller ができる前（照合の間など）に送ったとき、シミュレーションが終わった後（始まらなかったときも）です。`message` に理由を書きます。一時停止しているときの `pause` と、動いているときの `resume` は、受け付けて何もしません。
- controller ができる前（照合の間など）に受けた `pause`、`resume`、`speed` は覚えておき、controller ができたときに渡します（`pause` は `--start-paused` と同じく 0 で止まります）。
- 一時停止しても、`phase` は `running` のままです。`time` の `paused` で知らせます。
- 命令で止めた実行の結果は、Ctrl+C で止めたときと同じです（`result` は `"interrupted"`、終了コード 130、注記 `interrupted`）。`interrupted_by` は、`{"command":"stop"}` なら `"stop command"`、標準入力の終わりなら `"end of input"`、Control Server のクライアントの `Stop` なら `"control server stop"` です。
- `command` の事象の `accepted: true` は、行が命令として正しく、処理に回したという意味です（時間の命令では、今できるという意味でもあります）。その命令で実行が変わったとは限りません。たとえば、実行が終わった後（`phase` の `ended` の後）に受けた `stop` は、`accepted: true` ですが、結果は変わりません。
- `command` の事象は、その命令で起きること（`phase` の `stopping` など）より前に書きます。
- 照合の間に受けた `stop` は、照合が終わってから効きます（Ctrl+C と同じ）。
- 事象を読む側が読んでいなくても、命令は効きます。
- `--control` の Control Server のクライアントの時間の操作（`Pause`、`Resume`、`Step`、`RunUntil`、`SetSpeed`）と `Stop` も、同じ命令として処理します（`source` が `"control_server"`。規則と順序は [control-api.md](control-api.md) の「2 つの口の命令」）。`--stdin-commands` がなくても受け付けます。`run_until` と時刻つきの `pause` は、標準入力からは送れません。クライアントの `Stop` は、シミュレーションが終わった後は断ります（標準入力の `stop` は、上のとおり受け付けて何もしません）。
- `--control` と `--start-paused` で時刻 0 のステップの前に止まる実行では、止まるより前に受け付けた `resume`、`step`、`pause` も、その後の止まり方を決めます（[control-api.md](control-api.md) の「始まる前に書く」）。

## 版とできること（`vehicle_sil_launch version`）

`vehicle_sil_launch version`（`--version` も同じ）は、`vehicle_sil_launch <版>` の 1 行を書きます。`version --json` は、書く形式の版と、できることの一覧を書きます。どちらもファイルを読まず、終了コードは 0 です。

<!-- vehicle_sil_launch version --json -->
```json
{
  "vehicle_sil_launch": 1,
  "vehicle_sil": "<version>",
  "formats": {
    "check": 1,
    "run": 1,
    "events": 1,
    "describe": 1
  },
  "features": [
    "events",
    "stdin-commands",
    "check-variables",
    "speed",
    "start-paused",
    "time-commands",
    "control-links",
    "scenario",
    "manual-participants"
  ]
}
```

| キー | 値 |
|---|---|
| `vehicle_sil_launch` | この JSON の形式の版（今は 1） |
| `vehicle_sil` | vehicle-sil の版 |
| `formats` | 書く形式と読む形式の版: `check`（`check --json` の `vehicle_sil_check`）、`run`（`run --json` の `vehicle_sil_run`）、`events`（`hello` の `vehicle_sil_events`）、`describe`（読む `--describe` の JSON の `vehicle_sil_ecu`） |
| `features` | できることの名前の配列。読む側は、版を比べずに、名前があるかで判断します。名前は増えるだけです |

| `features` の名前 | できること |
|---|---|
| `events` | `run --events` |
| `stdin-commands` | `run --stdin-commands` と `{"command":"stop"}` |
| `check-variables` | 照合の JSON の `participants[].variables` |
| `speed` | 構成ファイルの `speed`、`time_step_ms`、`late_tolerance_ms`、`run --speed`、結果の `timing`、所見 `behind-real-time`、所見 `coarse-time-step`、`time` の事象の `speed`、`paused`、`actual_speed`、`late_steps`、`max_late_ms`、`slipped_ms`、`late_ms` |
| `start-paused` | `run --start-paused` |
| `time-commands` | 標準入力の `pause`、`resume`、`step`、`speed` |
| `control-links` | ECU の変数を読み書きする口: 事象 `control_link`、結果の `participants[].control_link`、照合の JSON の `participants[].control`、所見 `control-link-no-hello`、`control-link-version`、`control-link-late-values`、`control-link-dropped`、`control-link-not-offered` |
| `manual-participants` | 利用者が起動する participant: 構成ファイルの `manual`、`--manual`、照合と結果の JSON の `participants[].manual`、所見 `participant-manual`、事象 `participant` の `state` の `manual` |
| `scenario` | `check --scenario` と `run --scenario`（[scenario.md](scenario.md)）: 結果と照合の JSON の `scenario`、事象 `scenario_step`、所見 `scenario-*`、所見の `file` がシナリオのファイルになること |
| `control-server` | `run --control`（Control Server を持つビルドだけ）: 事象 `control_server`、`command` の `source` の `"control_server"` と `at_ms`、`--control-credentials`、`--control-tls-cert`、`--control-tls-key`、`--control-audit-log`、`interrupted_by` の `"control server stop"`、`--start-paused` で時刻 0 のステップの前に止まること |
| `scenario-control` | `run --scenario` と `--control` を一緒に使える（Control Server を持つビルドだけ）: シナリオの実行中、クライアントは読む・待つ・止めるだけで、書く呼び出しと時刻を動かす呼び出しは `FAILED_PRECONDITION`（理由 `scenario-running`）で断る（[scenario.md](scenario.md) の「実行中に変数を見る」） |

この例は、Control Server を持たないビルドの出力です。持つビルドは、`features` の最後に `"control-server"` と `"scenario-control"` を足します。この例も、CTest の `launch_cli.docs` が照合します（`vehicle_sil` の版と、この 2 つを除いて、バイト単位で）。
