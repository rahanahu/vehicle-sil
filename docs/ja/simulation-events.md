# プログラムから使う（`--events`、`--stdin-commands`、`version`）

GUI やスクリプトなどのプログラムから `vehicle_sil_launch` を起動して、経過と結果を読み、止めるためのリファレンスです。実行の流れ、止め方、結果は [simulation-run.md](simulation-run.md) にあります。

| 使うもの | できること | 節 |
|---|---|---|
| `run --events` | 実行の経過をイベントとして標準出力に書き、最後に結果を書きます。 | [イベント](#イベントrun---events) |
| `run --stdin-commands` | 標準入力からコマンドを読みます。 | [標準入力のコマンド](#標準入力のコマンドrun---stdin-commands) |
| `version --json` | この `vehicle_sil_launch` が書く形式のバージョンと、機能の一覧を書きます。 | [バージョンと機能の一覧](#バージョンと機能の一覧vehicle_sil_launch-version) |

```sh
vehicle_sil_launch version --json
vehicle_sil_launch check --json simulation.yaml
vehicle_sil_launch run --events --stdin-commands simulation.yaml
```

## イベント（`run --events`）

標準出力に、1 行に 1 つの JSON のオブジェクトを、起きた順に書きます（NDJSON）。文字コードは UTF-8、改行は `\n` で、行の中に改行はありません。1 行ごとに書き出します。イベントを書くのは `run` のときだけです。どの行も、次の 3 つのキーで始まります。

| キー | 値 |
|---|---|
| `event` | イベントの種類 |
| `seq` | 0 から 1 ずつ増える整数（書いた行の順と同じで、抜けません） |
| `t_ms` | イベントが起きたときの、`vehicle_sil_launch` が始まってからの実時間（ミリ秒、整数） |

読む側は、`seq` で、行を取りこぼしていないことを確認できます。`t_ms` は、行の順に見て、戻りません。

### イベントの種類

| `event` | いつ | ほかのキー |
|---|---|---|
| `hello` | 最初の行 | `vehicle_sil_events`、`vehicle_sil`、`pid`、`file`、`arguments` |
| `phase` | 段階が変わったとき | `phase` |
| `check` | 照合が終わったとき | `check` |
| `control_server` | `--control` の Control Server が待ち受けを始めたとき（`registry` の前） | `address`、`tls`、`credentials` |
| `registry` | registry が待ち受けを始めたとき | `uri` |
| `participant` | participant の状態が変わったとき | `name`、`state` |
| `control_link` | シミュレーションが始まった直後（`phase` の `running` の前）に、participant ごとに 1 回 | `name`、`state`、`variables`、`version` |
| `log` | participant か `vehicle_sil_launch` が 1 行書いたとき | `source`、`line` |
| `log_dropped` | 読む側が遅くて `log` を書けなかったとき、その場所に | `count`、`sources` |
| `finding` | 実行の指摘が決まったとき | 結果の `findings[]` の 1 つと同じキー |
| `scenario_step` | `--scenario` の手順が始まったとき、決まったとき | 結果の `scenario.steps[]` の 1 つと同じキー |
| `time` | 仮想時間が進んだとき、一時停止に入ったとき・出たとき、速度が変わったとき | `sim_ms`、`speed`、`paused`、`actual_speed`、`late_steps`、`max_late_ms`、`slipped_ms`、`late_ms` |
| `command` | コマンドを受けたとき | `command`、`source`、`accepted`、`message`、`ms`、`at_ms` |
| `heartbeat` | 1 秒のあいだ何も書かなかったとき | なし（3 つのキーだけ） |
| `result` | 最後の行 | `result` |

### イベントごとのキー

`hello`:

| キー | 値 |
|---|---|
| `vehicle_sil_events` | イベントの形式のバージョン（今は 1） |
| `vehicle_sil` | vehicle-sil のバージョン |
| `pid` | `vehicle_sil_launch` のプロセス ID |
| `file` | 設定ファイル（引数に書いたとおり。わからなければ `null`） |
| `arguments` | 受け取った引数の配列 |

`phase` の `phase`:

| 値 | 段階 |
|---|---|
| `checking` | 照合 |
| `starting` | registry、controller、participant の起動 |
| `waiting` | 必須の participant がそろうのを待つ |
| `running` | シミュレーションが動いている |
| `stopping` | participant を終了させている |
| `ended` | すべて終わった |

この順に進み、戻りません。飛ばすことがあります（照合で失敗すれば `checking` の次は `ended`）。

`check`、`registry`、`result`、`control_server`:

| イベント | キー | 値 |
|---|---|---|
| `check` | `check` | `check --json` と同じオブジェクト（[JSON](simulation-check.md#json--json)） |
| `registry` | `uri` | participant が接続する URI |
| `result` | `result` | `run --json` と同じオブジェクト（[JSON](simulation-run.md#jsonrun---json)） |
| `control_server` | `address` | クライアントが接続するアドレス（ポート 0 なら選ばれたポート） |
| `control_server` | `tls` | TLS か |
| `control_server` | `credentials` | 呼び出しにトークンが必要か |

`control_link`:

| キー | 値 |
|---|---|
| `name` | participant の名前 |
| `state` | 結果の `participants[].control_link.state` と同じ 5 つ |
| `variables` | `state` が `connected` のときだけあります（結果の同じ名前のキーと同じ意味）。 |
| `version` | `state` が `version_mismatch` のときだけあります（結果の同じ名前のキーと同じ意味）。 |

`control_link` を書くのは、起動した participant と、利用者が起動する（manual の）participant です。

`log` と `log_dropped`:

| イベント | キー | 値 |
|---|---|---|
| `log` | `source` | participant 名か `vehicle_sil_launch` |
| `log` | `line` | その行（[`log` の行](#log-の行)） |
| `log_dropped` | `count` | 書かなかった `log` の数 |
| `log_dropped` | `sources` | `source` ごとの数のオブジェクト（例: `{"Engine": 120, "vehicle_sil_launch": 1}`） |

`log_dropped` は[読む側が遅いとき](#読む側が遅いとき読まないとき)にあります。

`finding` のキーは、`severity`、`code`、`file`、`line`、`participant`、`network`、`message`、`detail` です。

`scenario_step` のキーは [scenario.md](scenario.md) の「出力」にあります。始まったときの `status` は `running` です。飛ばす手順（`status` は `skipped`、`at_ms` は null）は 1 回だけ書きます。

`time`:

| キー | 値 |
|---|---|
| `sim_ms` | controller が見ている仮想時間（ミリ秒。減りません） |
| `speed` | 今の速度（数か `"max"`） |
| `paused` | 一時停止しているか |
| `actual_speed` | 直近 1 秒ほどの、進んだ仮想時間 ÷ 実時間（小数 3 桁） |
| `late_steps` | それまでに遅れたタイムステップの数 |
| `max_late_ms` | それまでの最大の遅れ |
| `slipped_ms` | それまでのずれの合計 |
| `late_ms` | 期限を過ぎてもまだ始まっていないタイムステップの、いまの遅れ（なければ 0） |

- `actual_speed` は、動いた時間が短いとき、一時停止しているときは `null` です。
- `late_steps`、`max_late_ms`、`slipped_ms` は、結果の `timing` と同じ意味で、いま実行が終わったとしたときの値です。
- シミュレーションが止まっている間（仮想時間が進まず、`late_ms` が許容を超えて増えている間）も、`time` は 100 ms に 1 回まで書きます。

`paused` が `true` のイベントは、ふつう、止まった後に書きます。`--start-paused` の最初の `time` も、時刻 0 で止まってから書くので、結果の `paused_ms` は、このイベントより前から数えています。例外は次の 2 つです。どちらも、止まる予定の時刻に着いているのに、まだ止まっていない間に `true` を書きます。このときの `paused_ms` は、実際に止まってから数えます。

- 始まった後 2 秒たっても、時刻 0 で止まらなかったとき（待つのをやめて書きます）。
- 時刻を指定した `pause` や `run_until` が今の時刻以下を指していて、次のタイムステップで止まるとき。

`command`:

| キー | 値 |
|---|---|
| `command` | コマンドの名前（読めなければ `null`） |
| `source` | `"stdin"`（行で来た）、`"eof"`（標準入力の終わり）、`"control_server"`（クライアントの呼び出し） |
| `accepted` | 受け付けたか |
| `message` | 受け付けなかった理由（受け付けたら `null`） |
| `ms` | 受け付けた `step` にだけあります。実際に進む仮想時間（タイムステップの倍数に切り上げたもの） |
| `at_ms` | 受け付けた `run_until` と、時刻つきの `pause` にだけあります。止まる仮想時刻（タイムステップの倍数に切り上げたもの） |

`command` を書くのは、標準入力からコマンドを受けたとき（`--stdin-commands`）と、Control Server のクライアントが時間の操作か停止を呼んだとき（`--control`）です。`run_until` と、時刻つきの `pause` は、クライアントからだけ来ます。

### `participant` の `state`

| `state` | 意味 | ほかのキー |
|---|---|---|
| `disabled` | 無効にした participant（起動しません） | |
| `not_started` | プロセスを起動できなかった | `message` |
| `started` | プロセスを起動した | `pid`、`command`、`log` |
| `manual` | 利用者が起動する participant なので、起動しなかった（`started` の代わり） | `command`、`working_directory`、`command_line`、`environment` |
| `joined` | シミュレーションに参加した（controller がその participant のライフサイクルの状態を見た） | |
| `failed` | `vehicle_sil_launch` が、この participant の失敗で実行をやめると決めた | `reason`、`message` |
| `stop_requested` | 止まるよう求めた（止め方の 2） | |
| `ended` | プロセスが終わった | `exit_code`、`signal` |
| `killed` | 強制終了した（それが起動したプロセスも） | |

| `state` | キー | 値 |
|---|---|---|
| `not_started` | `message` | 理由 |
| `started` | `pid` | そのプロセスの ID |
| `started` | `command` | 起動した実行ファイルと引数の配列 |
| `started` | `log` | `--log-dir` のファイル（なければ `null`） |
| `manual` | `command` | 起動する実行ファイルと引数の配列 |
| `manual` | `working_directory` | 起動するディレクトリ |
| `manual` | `command_line` | 同じものを、この OS のシェルに貼れる 1 行にしたもの（標準エラーに書く行と同じ） |
| `manual` | `environment` | 起動するときに設定する環境変数の名前と値のオブジェクト |
| `failed` | `reason` | `left_simulation`、`process_ended`、`ended_before_start`（下の表） |
| `failed` | `message` | この失敗の `finding` の `message` と同じ文 |
| `ended` | `exit_code`、`signal` | 結果の `participants[]` と同じ意味 |

`manual` の participant には、この後、参加すれば `joined` が来ます。`ended`、`stop_requested`、`killed` は来ません（[participant を自分で起動する](simulation-config.md#participant-を自分で起動するmanual--manual)）。`environment` は、`registry_uri` がループバックのアドレスでないときだけあり、`{"VEHICLE_SIL_LISTEN_ON_NETWORK": "1"}` です（[待ち受けるアドレス](simulation-run.md#待ち受けるアドレス)）。

`failed` の `reason`:

| `reason` | 意味 |
|---|---|
| `left_simulation` | プロセスは動いているが、シミュレーションから抜けた |
| `process_ended` | シミュレーションの途中で、プロセスが失敗として終わった |
| `ended_before_start` | シミュレーションが始まる前にプロセスが終わった |

プロセスが終わるときは、接続が先に切れるので、プロセスの終わりより先に `left_simulation` で気づくこともあります。`failed` の `message` は、その時点で文が決まっているときだけあります（`left_simulation` にはありません）。

### イベントの順序

最初の行は `hello`、最後の行は `result` です。`result` の後には何も書きません。

| イベント | 書く位置 |
|---|---|
| `participant` の `disabled` | `check` の直後、`starting` の前に、設定ファイルの順 |
| `control_server` | `phase` の `starting` の後、`registry` より前に 1 回だけ |
| `participant` の `manual` | `started` と同じ所に、設定ファイルの順 |
| participant の `log` | その participant の `started` より後 |
| `participant` の `joined` | SIL Kit のライフサイクルで参加する participant だけ |
| `control_link` | 必須の participant の `joined` がそろった後、`phase` の `running` の前に、設定ファイルの順 |
| `participant` の `failed` | 失敗と決めた時点ですぐ |
| `command` | そのコマンドで起きること（`phase` の `stopping` など）より前 |
| `heartbeat` | `hello` から `result` までのどの段階でも、ほかの行の間 |

- `control_server` は、照合で失敗した実行と、引数のエラーでは書きません。
- `manual` は、シミュレーションが始まらなかったときは書きません。
- `manual` の participant には `log` が来ません（出力は、利用者が起動した所に出ます）。
- `joined` は、ライフサイクルで参加しないツールの participant には来ません。
- `control_link` は、起動できなかった participant と、無効にした participant にはありません。
- `heartbeat` の数は、実行にかかった時間で変わります。
- 標準入力とクライアントの時間のコマンドは、1 つずつ順に処理するので、`command` のイベントの順が、適用された順です。

participant の `log` の終わりの側には、保証がありません。participant の出力は別のスレッドが読んでいるので、最後の何行かが、その participant の `ended` の後や、`phase` の `ended` の後に来ることがあります。`result` より後には来ません。

`failed` を書くのは、ほかの participant を止める（`phase` の `stopping`）より前で、その失敗の `finding` より前です。`log` と違って、読む側が遅くても捨てません。`reason` が `left_simulation` のときは、その participant の `ended` より前に来ます。`process_ended` と `ended_before_start` は、プロセスの終わりで失敗に気づくので、`ended` の後に来ます。`failed` を知らない読む側は、無視してかまいません（結果と、ほかのイベントは変わりません）。

`time` を書く時点:

| 時点 | 書き方 |
|---|---|
| `running` になったとき | 時刻 0 の `time` を書きます。 |
| 仮想時間が進んだとき | 実時間で 100 ms に 1 回まで書きます。 |
| 一時停止に入ったとき・出たとき、速度が変わったとき | すぐに書きます。 |
| 一時停止したまま時刻が変わったとき（ステップ実行の後） | すぐに書きます。 |
| シミュレーションが終わったとき | 間隔によらず、そのときの状態を書きます（終わったシミュレーションは一時停止していません）。 |

最初の `time` は時刻 0 のもので、`paused` が `true` になるのは、時刻 0 で止まっている（止まることになっている）ときだけです。このイベントを書く時点で 0 より先に進んでいれば、先の時刻で止まっていても `false` を書きます。始まる前に受け付けた `step` や `run_until` で進んだときなどがそうです。その後も止まっていれば、次の `time`（`paused` が `true`）に、止まった時刻があります。その前に次のコマンドで進むか、シミュレーションが終われば、その `time` はありません。

`sim_ms` は減りません（一時停止や速度の変更では、前と同じ値のことがあります）。`vehicle_sil_launch` は状態を 20 ms ごとに読むので、それより短い間の変化（最速でのステップ実行の途中など）は書かないことがあります。最後の状態は必ず書きます。

`finding` は実行の指摘です。照合の指摘は `check` の中にあります。シミュレーションが終わった（または始まらなかった）ときに、その時点で決まっているものを書き、残りを participant を終了させた後に書きます。一度書いた `finding` は、同じ内容のまま `result` の `findings` に入ります。同じものを 2 回は書きません。`result` だけを読んでも、結果はわかります。

シミュレーションの途中で失敗した participant の `participant-failed` は、プロセスの終わり方を含むので、プロセスが終わってから書きます。失敗に気づいた時点では、`participant` の `failed` が通知します。`vehicle_sil_launch` の `log` の行にも理由があります。

### 実行に進まないときのイベント

`--events` を付けた `run` は、引数がエラーでも `hello` を最初に、`result` を最後に書きます（`heartbeat` が間に入ることがあります）。

| 場合 | イベント | `result` の `result` | 終了コード |
|---|---|---|---|
| 引数のエラー（知らないオプション、設定ファイルの指定がない、`--json` と一緒に付けた、など） | `hello`、`result` | `"usage"` | 4 |
| 設定ファイルを読んでわかる引数のエラー | `hello`、`phase`（`checking`）、`check`、`phase`（`ended`）、`result` | `"usage"` | 4 |
| 設定ファイルを読めない | 同じ 5 つ | `"unreadable"` | 3 |
| 照合でエラーが見つかった | 同じ 5 つ | `"check_failed"` | 1 |
| 標準出力がない（閉じた状態で起動された） | 何も書けません（標準エラーにエラーを書きます）。 | | 4 |

設定ファイルを読んでわかる引数のエラーは、次のものです。

- `--disable`、`--enable`、`--manual` の名前が設定ファイルにない。
- 機種を書いた設定ファイルに `--variant` がない。
- `--variant` の名前が設定ファイルにない。

`run` でないコマンド（`check`、`version`、コマンドなし）に `--events` を付けたときは、イベントを書きません。引数のエラーとして標準エラーに書き、終了コードは 4 です。

### `log` の行

`line` は、participant が標準出力か標準エラーに書いた 1 行（行末の改行なし）、または `vehicle_sil_launch` の経過の 1 行です。`--events` なしで標準エラーに書く行から、前の `[<名前>] ` を除いたものと同じです。

| 行の内容 | 扱い |
|---|---|
| 制御文字（タブを除く）と、UTF-8 として正しくないバイト | `?` に置き換えます。 |
| 4096 バイトを超える行 | 4096 バイト以下の複数の `log` に分けます（UTF-8 の文字の途中では切りません）。 |

分けた `log` に、続きであることを示すキーはありません。

`--log-dir` のファイルには、置き換える前のバイトをそのまま書きます。読む側が遅くて `log` を書けなかったときも、ファイルにはすべての行があります。

### 読む側が遅いとき、読まないとき

`vehicle_sil_launch` は、イベントを読む側を待ちません。イベントを書くのは専用のスレッドで、実行を進めるスレッド、participant の出力を読むスレッド、標準入力を読むスレッドは、イベントをキューに追加するだけです。読む側が遅くても、止まっていても、次のことは変わりません。

- participant は止まりません（出力を読み続けます）。`--log-dir` のファイルも、今までどおりに書きます。
- `--timeout-ms`、シグナル、標準入力の `stop` は、そのまま使えます。

書けずに待っているイベントが増えたときは、次のようにします。

| イベント | 扱い |
|---|---|
| `log` | 待っている `log` が 4096 行または 1 MiB を超える間は、書かずに数えます。 |
| 仮想時間が進んだことだけを通知する `time` | ほかのイベントが書けずに待っている間は、作りません。 |
| 一時停止と速度の変化を通知する `time` | 作ります。 |
| そのほかのイベント | 捨てません。 |

数えた `log` は、その場所に `log_dropped` を 1 行書いて通知します（読む側がまた読み始めたときに受け取ります）。`count` と `sources` は、その行を書く時点までに数えた分です。

次のどちらかのときは、読む側がいなくなったものとして扱います（[読む側がいなくなったとき](#読む側がいなくなったとき)）。

- 書き込みが 30 秒のあいだまったく進まない。
- `log` 以外のイベントが 4096 個を超えて待っている。

「30 秒進まない」の数え方:

- 数え始めるのは、読む側が読むのをやめたときではなく、書き込みが待ち始めたとき（パイプがいっぱいになったとき）です。書く量が少ない実行（`heartbeat` だけ、など）では、パイプがいっぱいになるまでに長くかかります。読むのをやめて 30 秒で必ず止まるわけではありません。
- `vehicle_sil_launch` は 4096 バイトまでの単位で書き、その単位が 1 つ入りきると「進んだ」と数えます。読む側は、30 秒のうちにおよそ 4096 バイトを読めば、読んでいると見なされます（待たない設定のパイプでは 1 バイトで足ります）。
- 30 秒は `--events-stall-ms <ms>` で変えられます（1000 以上。デバッガーで止めることがある読む側は、長くします）。端末に書いているときも同じです（出力を止めたままにすると、実行が止まります）。

シミュレーションが終わると、`vehicle_sil_launch` は `result` を書き終えるまで待ちます。待つのは、読む側が読んでいても長くて 30 秒（`--events-stall-ms` の値）です。待っているイベントが多く、読む側が遅いと、読む側が `result` を受け取れないことがあります。書き込みが 0.5 秒進まない間に、止める要求が 2 回以上になったとき（それまで要求がなければ 1 回目が来たとき）は、そこで待つのをやめます。

`result` を書けなかったときの終了コードは 3 で、標準エラーに `error: cannot write the result to the standard output (<理由>)` と書きます。止める要求で止めた実行は 130 で、この行は書きません。

### 読む側がいなくなったとき

標準出力を読む側がいなくなった（パイプが閉じた）ことには、次に行を書くときに気づきます。`heartbeat` があるので、どの段階でも 1 秒以内です。上の「読まない」の条件に当てはまったときも、同じ扱いです。

| すること | 内容 |
|---|---|
| イベントの出力を閉じる | その後は、`result` を含めて何も書きません（書きかけの行が途中で切れることがあります）。 |
| 止める要求として扱う | 1 回目の Ctrl+C と同じです。 |
| 標準エラーに理由を 1 行書く | `vehicle_sil_launch: cannot write the events (<理由>): stopping the run` |
| 結果と終了コード | 結果の `interrupted_by` は `"output closed"`、終了コードは 130 です（SIGPIPE で終わりません）。 |

- すでに止める要求を受けていれば、止める要求としては何もしません（2 回目に数えません）。このとき、標準エラーの行の終わりは `no more events are written` です。
- `result` を書くのを待っている間に閉じたときは、この行ではなく、上の `error: cannot write the result ...` の行です。
- 標準エラーも読まれていないと、この行は読まれません（`vehicle_sil_launch` は、それでも終わります）。

標準出力が、書き込みで待たない設定（`O_NONBLOCK` など）のパイプでも同じように動きます。いっぱいのときは、閉じたとは扱わず、空くのを待ちます。

### 読む側が守ること

- 標準出力を読み続けます。`result` の行が来たら、出力は終わりです。
- 改行で終わらない最後の行は捨てます（`vehicle_sil_launch` が強制終了された、出力が閉じた）。
- 知らない `event` と知らないキーは無視します。
- 標準エラーも読みます（読み捨ててもかまいません）。
- `vehicle_sil_launch` を起動して終わりまで読むプログラム（GUI、CI のスクリプト）は、`--stdin-commands` も付けます。

| 見えたこと | 意味 |
|---|---|
| `result` が来ないまま標準出力が終わった | `vehicle_sil_launch` が結果を書けずに終わっています（強制終了、クラッシュ、上の「いなくなった」の扱い）。 |
| `heartbeat` を含めて何の行も数秒来ない | `vehicle_sil_launch` が止まっています（1 秒に 1 行は書きます）。 |

`result` が来なかったときは、プロセスの終了コードで判断します。

`event`、キー、`state` と `phase` の値は、バージョンを上げずに増えることがあります。キーの意味が変わるとき、なくなるときは `vehicle_sil_events` を上げます。

`--events` のとき、標準エラーに書くのは `vehicle_sil_launch` 自身の通知（引数のエラー、イベントを書けなくなった、結果を書けない、想定外の失敗）だけで、participant の行も経過も書きません。`vehicle_sil_launch` の中のライブラリが標準出力に書いたものは、標準エラーに行きます（イベントの行に混ざりません）。

`--stdin-commands` を付けておくと、起動したプログラムが止まるか終わったときに標準入力が閉じるので、`vehicle_sil_launch` は標準出力の状態によらず、すぐに順に止まります。

### そのほかのルール

- 文字列のエスケープは `--json` と同じです（`"`、`\`、制御文字。UTF-8 でないバイトは U+FFFD）。`log` の `line` は、上のとおり先に `?` に置き換えています。
- 行の長さに上限はありません（`check` と `result` は大きくなります）。
- `--json` と `--events` を一緒に付けるのは引数のエラーです（イベントの最後が結果なので）。
- 終了コードは `--events` なしと同じです。

### 例

短い実行の全行です（`--disable Dashboard` で、出力の順が決まる Engine だけにしています）。時刻、プロセス ID、registry のポート、例のディレクトリ、バージョン、時間の進み方は、実行ごとに違うので `<t_ms>`、`<pid>`、`<port>`、`<dir>`、`<version>`、`<wall_ms>`、`<actual_speed>` にしています（Windows では、実行ファイルの名前に `.exe` が付きます）。

<!-- vehicle_sil_launch run simulation.yaml --events --disable Dashboard -->
```text
{"event":"hello","seq":0,"t_ms":<t_ms>,"vehicle_sil_events":1,"vehicle_sil":"<version>","pid":<pid>,"file":"simulation.yaml","arguments":["run","simulation.yaml","--events","--disable","Dashboard"]}
{"event":"phase","seq":1,"t_ms":<t_ms>,"phase":"checking"}
{"event":"check","seq":2,"t_ms":<t_ms>,"check":{"vehicle_sil_check":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"stop_at_ms":1000,"start_timeout_ms":30000,"stop_timeout_ms":10000,"registry_uri":null,"participants":[{"name":"Engine","line":11,"command":["engine/vehicle_sil_engine_ecu"],"describe":true,"required":true,"enabled":true,"manual":false,"role":"ecu","step_ms":10,"status":"described","reported":{"vehicle_sil":"<version>","name":"Engine","step_ns":10000000,"networks":[{"name":"J1939_CAN","kind":"can","use":"j1939","nodes":[{"address":0,"j1939_name":"0x00000000FE000001","arbitrary_address_capable":false}],"mac":null}]},"variables":[{"name":"ecu.connected","type":"bool","causality":"input","unit":"","min":null,"max":null,"step":null,"description":"on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on","items":[],"builtin":true},{"name":"engine.throttle","type":"real","causality":"input","unit":"%","min":0,"max":100,"step":null,"description":"accelerator pedal position","items":[],"builtin":false},{"name":"engine.speed","type":"integer","causality":"output","unit":"rpm","min":null,"max":null,"step":null,"description":"","items":[],"builtin":false}],"control":{"transport":"silkit","wire_version":1}},{"name":"Dashboard","line":14,"command":["dashboard/vehicle_sil_dashboard_ecu"],"describe":true,"required":true,"enabled":false,"manual":false,"role":"ecu","step_ms":5,"status":"disabled","reported":null,"variables":null,"control":null}],"networks":[{"name":"J1939_CAN","kind":"can","line":8,"participants":["Engine"]}],"findings":[{"severity":"warning","code":"single-participant","file":"simulation.yaml","line":8,"participant":null,"network":"J1939_CAN","message":"network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it","detail":null},{"severity":"note","code":"participant-disabled","file":"simulation.yaml","line":14,"participant":"Dashboard","network":null,"message":"participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it","detail":null}]}}
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
{"event":"result","seq":22,"t_ms":<t_ms>,"result":{"vehicle_sil_run":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"simulation":"reached_stop_at","interrupted_by":null,"timing":{"speed":"max","time_step_ms":10,"ecu_step_ms":10,"step_delay_ms":0,"late_tolerance_ms":10,"sim_ms":1000,"wall_ms":<wall_ms>,"actual_speed":<actual_speed>,"paced_steps":0,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0,"paused_ms":0},"participants":[{"name":"Engine","required":true,"enabled":true,"manual":false,"ended":"exited","exit_code":0,"signal":null,"left_processes":false,"log":null,"control_link":{"state":"connected","variables":18,"version":null,"late_messages":0}},{"name":"Dashboard","required":true,"enabled":false,"manual":false,"ended":"not_started","exit_code":null,"signal":null,"left_processes":false,"log":null,"control_link":{"state":"none","variables":null,"version":null,"late_messages":0}}],"findings":[],"check":{"vehicle_sil_check":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"stop_at_ms":1000,"start_timeout_ms":30000,"stop_timeout_ms":10000,"registry_uri":null,"participants":[{"name":"Engine","line":11,"command":["engine/vehicle_sil_engine_ecu"],"describe":true,"required":true,"enabled":true,"manual":false,"role":"ecu","step_ms":10,"status":"described","reported":{"vehicle_sil":"<version>","name":"Engine","step_ns":10000000,"networks":[{"name":"J1939_CAN","kind":"can","use":"j1939","nodes":[{"address":0,"j1939_name":"0x00000000FE000001","arbitrary_address_capable":false}],"mac":null}]},"variables":[{"name":"ecu.connected","type":"bool","causality":"input","unit":"","min":null,"max":null,"step":null,"description":"on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on","items":[],"builtin":true},{"name":"engine.throttle","type":"real","causality":"input","unit":"%","min":0,"max":100,"step":null,"description":"accelerator pedal position","items":[],"builtin":false},{"name":"engine.speed","type":"integer","causality":"output","unit":"rpm","min":null,"max":null,"step":null,"description":"","items":[],"builtin":false}],"control":{"transport":"silkit","wire_version":1}},{"name":"Dashboard","line":14,"command":["dashboard/vehicle_sil_dashboard_ecu"],"describe":true,"required":true,"enabled":false,"manual":false,"role":"ecu","step_ms":5,"status":"disabled","reported":null,"variables":null,"control":null}],"networks":[{"name":"J1939_CAN","kind":"can","line":8,"participants":["Engine"]}],"findings":[{"severity":"warning","code":"single-participant","file":"simulation.yaml","line":8,"participant":null,"network":"J1939_CAN","message":"network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it","detail":null},{"severity":"note","code":"participant-disabled","file":"simulation.yaml","line":14,"participant":"Dashboard","network":null,"message":"participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it","detail":null}]}}}
```

この例は、CTest の `launch_run.events` が、実際の出力と、バイト単位で照合します。照合の前に、実際の出力から `heartbeat` の行と、最初と最後のほかの `time` の行を除き（`seq` は詰めます）、上の置き換えをします。遅い PC では、途中の時刻の `time` も書くためです。

## 標準入力のコマンド（`run --stdin-commands`）

`--stdin-commands` を付けたときだけ、標準入力を読みます。付けないときは読みません（標準入力が閉じていても空でも、止まりません）。1 行に 1 つの JSON のオブジェクトです。`--events` と一緒に使います（[読む側が守ること](#読む側が守ること)）。単独でも使えます。

| 行 | 意味 |
|---|---|
| `{"command":"stop"}` | 順に止めます（1 回目の Ctrl+C と同じ）。 |
| `{"command":"pause"}` | 一時停止します。 |
| `{"command":"resume"}` | 一時停止をやめて進みます。 |
| `{"command":"step","ms":<ms>}` | 一時停止している間だけ: 仮想時間を `<ms>`（1 以上 10^12 以下の整数）だけ進めて、また止まります。 |
| `{"command":"speed","value":<速度>}` | 速度を変えます（`<速度>` は数か `"max"`。設定ファイルの `speed` と同じ範囲）。 |
| 標準入力の終わり（EOF） | `stop` を 1 回受けたのと同じです（起動した側がいなくなった、パイプを閉じた）。 |
| 空白だけの行 | 読み飛ばします。 |
| それ以外 | 何もしません。 |

| コマンド | 補足 |
|---|---|
| `stop` | 2 回目は 2 回目の Ctrl+C と同じです（待つのをやめて、残っている participant を強制終了します）。 |
| `stop` | 照合の間に受けたものは、照合が終わってから止めます（Ctrl+C と同じ）。 |
| `pause` | controller のタイムステップの境目で止まります（速度を合わせて待っているタイムステップがあれば、そこで。なければ次のタイムステップで）。 |
| `resume` | 速度は、ここから数え直します。 |
| `step` | タイムステップの倍数に切り上げます（`command` のイベントの `ms` が、実際に進む量です）。 |
| `step` | 前の `step` の途中で送ると、その止まる時刻から数えます。 |
| `speed` | 一時停止はそのままです。 |
| EOF | すでに止める要求（`stop`、シグナル、閉じた標準出力）を受けていれば、何もしません（2 回目に数えません。`command` のイベントも書きません）。 |
| それ以外 | JSON のオブジェクトでない、`command` がない、知らないコマンド、64 KiB を超える行がこれに当たります。 |
| それ以外 | `--events` なら `command` のイベント（`accepted: false` と理由）を、なければ標準エラーに 1 行を書きます。 |

オブジェクトのほかのキーは無視します。イベントを読む側が読んでいなくても、コマンドは受け付けて実行します。

### 時間のコマンドを受け付ける条件

時間のコマンド（`pause`、`resume`、`step`、`speed`）は、標準入力を読むスレッドが、受け付けるかを決めて `command` のイベントを書き、それから controller に渡します。どれも待ちません（実行を進めるスレッドも、イベントを読む側も待ちません）。controller は、次のタイムステップの境目で適用します。

| 場合 | 扱い |
|---|---|
| `step` を、一時停止していないときに送った（`resume` の後など） | 受け付けません。 |
| `step` を、controller ができる前（照合の間など）に送った | 受け付けません。 |
| シミュレーションが終わった後（始まらなかったときも）の時間のコマンド | 受け付けません。 |
| 一時停止しているときの `pause`、動いているときの `resume` | 受け付けて、何もしません。 |
| controller ができる前（照合の間など）に受けた `pause`、`resume`、`speed` | 覚えておき、controller ができたときに渡します。 |

受け付けなかった理由は `message` に書きます。controller ができる前に受けた `pause` は、`--start-paused` と同じく 0 で止まります。

一時停止しても、`phase` は `running` のままです。一時停止は `time` の `paused` で通知します。

`--control` と `--start-paused` で時刻 0 のステップの前に止まる実行では、止まるより前に受け付けた `resume`、`step`、`pause` も、その後の止まり方を決めます（[control-api.md](control-api.md) の「始まる前に書く」）。

### `accepted` の意味と、止めた実行の結果

`command` のイベントの `accepted: true` は、行がコマンドとして正しく、処理に渡したという意味です（時間のコマンドでは、今できるという意味でもあります）。そのコマンドで実行が変わったとは限りません。たとえば、実行が終わった後（`phase` の `ended` の後）に受けた `stop` は、`accepted: true` ですが、結果は変わりません。

`command` のイベントは、そのコマンドで起きること（`phase` の `stopping` など）より前に書きます。

コマンドで止めた実行の結果は、Ctrl+C で止めたときと同じです（`result` は `"interrupted"`、終了コード 130、注記 `interrupted`）。

| 止めたもの | `interrupted_by` |
|---|---|
| `{"command":"stop"}` | `"stop command"` |
| 標準入力の終わり | `"end of input"` |
| Control Server のクライアントの `Stop` | `"control server stop"` |

### Control Server のクライアントからのコマンド

`--control` の Control Server のクライアントの時間の操作（`Pause`、`Resume`、`Step`、`RunUntil`、`SetSpeed`）と `Stop` も、同じコマンドとして処理します（`source` が `"control_server"`）。規則と順序は [control-api.md](control-api.md) の「標準入力とクライアントのコマンド」にあります。

| 項目 | 標準入力 | クライアント |
|---|---|---|
| `--stdin-commands` がないとき | 読みません。 | 受け付けます。 |
| `run_until` と時刻つきの `pause` | 送れません。 | 送れます。 |
| シミュレーションが終わった後の停止 | `stop` を受け付けて、何もしません。 | `Stop` を拒否します。 |

## バージョンと機能の一覧（`vehicle_sil_launch version`）

`vehicle_sil_launch version`（`--version` も同じ）は、`vehicle_sil_launch <バージョン>` の 1 行を書きます。`version --json` は、書く形式のバージョンと、機能の一覧を書きます。どちらもファイルを読まず、終了コードは 0 です。

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
    "manual-participants",
    "scenario-wave",
    "scenario-motion-expect"
  ]
}
```

| キー | 値 |
|---|---|
| `vehicle_sil_launch` | この JSON の形式のバージョン（今は 1） |
| `vehicle_sil` | vehicle-sil のバージョン |
| `formats` | 書く形式と読む形式のバージョン（下の表） |
| `features` | 機能の名前の配列（下の表） |

読む側は、バージョンを比べずに、`features` に名前があるかで判断します。名前は増えるだけです。

| `formats` のキー | 何のバージョンか |
|---|---|
| `check` | `check --json` の `vehicle_sil_check` |
| `run` | `run --json` の `vehicle_sil_run` |
| `events` | `hello` の `vehicle_sil_events` |
| `describe` | 読む `--describe` の JSON の `vehicle_sil_ecu` |

### `features` の名前

| `features` の名前 | 機能 |
|---|---|
| `events` | `run --events` |
| `stdin-commands` | `run --stdin-commands` と `{"command":"stop"}` |
| `check-variables` | 照合の JSON の `participants[].variables` |
| `speed` | 速度の指定（[速度を指定して動かす](simulation-run.md#速度を指定して動かすspeed)） |
| `start-paused` | `run --start-paused` |
| `time-commands` | 標準入力の `pause`、`resume`、`step`、`speed` |
| `control-links` | ECU の変数との接続（[ECU の変数との接続](simulation-run.md#ecu-の変数との接続control_link)） |
| `manual-participants` | 利用者が起動する participant |
| `scenario` | `check --scenario` と `run --scenario`（[scenario.md](scenario.md)） |
| `scenario-wave` | シナリオの手順 `wave`（[scenario.md](scenario.md) の「波形で動かす（`wave`）」） |
| `scenario-motion-expect` | シナリオの `ramp` と `wave` の横に書く `expect`（[scenario.md](scenario.md) の「動かしている間ずっと確認する」） |
| `control-server` | `run --control`（Control Server を持つビルドだけ） |
| `scenario-control` | `run --scenario` と `--control` を一緒に使える（Control Server を持つビルドだけ） |

名前ごとに、次のものがあることを示します。

| `features` の名前 | 含むもの |
|---|---|
| `speed` | 設定ファイルの `speed`、`time_step_ms`、`late_tolerance_ms`、`run --speed`、結果の `timing`、指摘 `behind-real-time`、指摘 `coarse-time-step`、`time` のイベントの `speed`、`paused`、`actual_speed`、`late_steps`、`max_late_ms`、`slipped_ms`、`late_ms` |
| `control-links` | イベント `control_link`、結果の `participants[].control_link`、照合の JSON の `participants[].control`、指摘 `control-link-no-hello`、`control-link-version`、`control-link-late-values`、`control-link-dropped`、`control-link-not-offered` |
| `manual-participants` | 設定ファイルの `manual`、`--manual`、照合と結果の JSON の `participants[].manual`、指摘 `participant-manual`、イベント `participant` の `state` の `manual` |
| `scenario` | 結果と照合の JSON の `scenario`、イベント `scenario_step`、指摘 `scenario-*`、指摘の `file` がシナリオのファイルになること |
| `scenario-wave` | その手順の `undersampled`、指摘 `scenario-wave-*`、結果とイベントの手順の `kind` の `"wave"` |
| `scenario-motion-expect` | 結果とイベントの手順の `expect_skipped` |
| `control-server` | イベント `control_server`、`command` の `source` の `"control_server"` と `at_ms`、`--control-credentials`、`--control-tls-cert`、`--control-tls-key`、`--control-audit-log`、`interrupted_by` の `"control server stop"`、`--start-paused` で時刻 0 のステップの前に止まること |

`scenario-control` のあるビルドでは、シナリオの実行中、クライアントは読む・待つ・止めるだけです。書く呼び出しと時刻を動かす呼び出しは、`FAILED_PRECONDITION`（理由 `scenario-running`）で拒否します（[scenario.md](scenario.md) の「実行中に変数を見る」）。

上の例は、Control Server を持たないビルドの出力です。持つビルドは、`features` の最後に `"control-server"` と `"scenario-control"` を追加します。この例も、CTest の `launch_cli.docs` が照合します（`vehicle_sil` のバージョンと、この 2 つを除いて、バイト単位で）。

## 関連するドキュメント

- 実行の流れ、止め方、結果の JSON → [simulation-run.md](simulation-run.md)
- 照合の JSON → [simulation-check.md](simulation-check.md)
- gRPC で変数を読み書きし、時間を操作する → [control-api.md](control-api.md)
- ブラウザの GUI と、その開発 → [gui.md](gui.md)

ブラウザの GUI のテストは、この出力を記録したもの（`gui/server/tests/recordings/`）を再生します。ランチャーの出力を変えたら、`python3 gui/server/tests/record_launcher.py --launcher <vehicle_sil_launch> --compare` で比べ、違う場面を記録し直します（[gui.md](gui.md) の「開発」）。
