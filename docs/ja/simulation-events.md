# プログラムから使う（`serve`、`--stdin-commands`、`version`）

[English](../simulation-events.md)

GUI やスクリプトなどのプログラムから `vehicle_sil_launch` を起動して、経過と結果を読み、止めるためのリファレンスです。実行の流れ、止め方、結果は [simulation-run.md](simulation-run.md) にあります。

| 使うもの | できること | 節 |
|---|---|---|
| `serve [--once]` | 実行の経過を NDJSON イベント（`vehicle_sil_events: 4`）として標準出力に書き、標準入力からコマンドを読みます。 | [イベント](#イベントserve---once)、[常駐ランチャー（`serve`）](#常駐ランチャーserve) |
| `run --stdin-commands` | 単発の `run` で標準入力からコマンドを読みます。 | [標準入力のコマンド](#標準入力のコマンドrun---stdin-commands) |
| `version --json` | この `vehicle_sil_launch` が書く形式のバージョンと、機能の一覧を書きます。 | [バージョンと機能の一覧](#バージョンと機能の一覧vehicle_sil_launch-version) |

```sh
vehicle_sil_launch version --json
vehicle_sil_launch check --json simulation.yaml
vehicle_sil_launch serve --once simulation.yaml   # 標準入力に {"command":"run"} を送って開始
```

## イベント（`serve [--once]`）

`serve` と `serve --once` は標準出力に、1 行に 1 つの JSON のオブジェクトを、起きた順に書きます（NDJSON）。文字コードは UTF-8、改行は `\n` で、行の中に改行はありません。1 行ごとに書き出します（`run --events` は廃止され終了コード 4 になります。1 回だけの実行は `serve --once`、複数回の実行は `serve` を使います。[常駐ランチャー（`serve`）](#常駐ランチャーserve) を参照）。どの行も、次の 3 つのキーで始まります。

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
| `outside` | `--connect` のある実行で、ブリッジの数が変わったとき（ネットワークごとに 1 秒に 1 回まで） | `network`、`to_outside`、`from_outside` |
| `finding` | 実行の指摘が決まったとき | 結果の `findings[]` の 1 つと同じキー |
| `scenario_step` | `--scenario` の手順が始まったとき、決まったとき | 結果の `scenario.steps[]` の 1 つと同じキー |
| `time` | 仮想時間が進んだとき、別の時刻で一時停止したとき、遅れているとき | `sim_ms`、`actual_speed`、`late_steps`、`max_late_ms`、`slipped_ms`、`late_ms` |
| `state` | 実行の状態か、受け付ける操作が変わったとき | `rev`、`phase`、`paused`、`paused_at_ms`、`holding`、`speed`、`pacing`、`stop`、`scenario`、`handed_over`、`scenario_over_at_ms`、`allowed`、`outside` |
| `command` | コマンドを受けたとき | `command`、`source`、`accepted`、`message`、`reason`、`ms`、`at_ms`、`id` |
| `heartbeat` | 1 秒のあいだ何も書かなかったとき | なし（3 つのキーだけ） |
| `result` | 最後の行 | `result` |

### イベントごとのキー

`hello`:

| キー | 値 |
|---|---|
| `vehicle_sil_events` | イベントの形式のバージョン（今は 4。`launcher`、`status`、`run_start`、`prepare` および実行ごとの `run` キーは [常駐ランチャー（`serve`）](#常駐ランチャーserve) を参照） |
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

この順に進み、戻りません。飛ばすことがあります（照合で失敗すれば `checking` の次は `ended`。準備できなかった実行は `starting` の次が `ended`、始まる前に止めた実行は `checking` の次が `ended`）。[実行の終わり方](#実行の終わり方)を見てください。

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
| `actual_speed` | 直近 1 秒ほどの、進んだ仮想時間 ÷ 実時間（小数 3 桁） |
| `late_steps` | それまでに遅れたタイムステップの数 |
| `max_late_ms` | それまでの最大の遅れ |
| `slipped_ms` | それまでのずれの合計 |
| `late_ms` | 期限を過ぎてもまだ始まっていないタイムステップの、いまの遅れ（なければ 0） |

- `actual_speed` は、動いた時間が短いときと、一時停止している間に書く `time` では `null` です。
- 一時停止しているか、その時刻、指定の速度は、`time` にありません。最新の `state` の `paused`、`paused_at_ms`、`speed` を読んでください。
- 一時停止している間、最後の `time` は止まる前のものでありえます。その `sim_ms` は `paused_at_ms` より前のことがあり（ランチャーが次に見るまで。20 ms）、`actual_speed` と `late_ms` は止まる前の値のことがあります（最後の `time` と同じ時刻で止まったときは、新しい `time` を書きません）。
- `late_steps`、`max_late_ms`、`slipped_ms` は、結果の `timing` と同じ意味で、いま実行が終わったとしたときの値です。
- シミュレーションが止まっている間（仮想時間が進まず、`late_ms` が許容を超えて増えている間）も、`time` は 100 ms に 1 回まで書きます。

`command`:

| キー | 値 |
|---|---|
| `command` | コマンドの名前（読めなければ `null`） |
| `source` | `"stdin"`（行で来た）、`"eof"`（標準入力の終わり）、`"control_server"`（クライアントの呼び出し） |
| `accepted` | 受け付けたか |
| `message` | 受け付けなかった理由（受け付けたら `null`） |
| `reason` | 受け付けなかった理由のコード（[理由のコード](#理由のコード)。受け付けたら `null`） |
| `ms` | 受け付けた `step` にだけあります。実際に進む仮想時間（タイムステップの倍数に切り上げたもの） |
| `at_ms` | 受け付けた `step`、`run_until` と、時刻つきの `pause` にだけあります。`step` では、その step の後に止まる仮想時刻で、受け付けた時点で決まります（どこから数えるかは「標準入力のコマンド」を見てください）。ほかの 2 つでは、頼んだ仮想時刻（タイムステップの倍数に切り上げたもの）。その時刻がすでに過ぎている場合を除き、止まる時刻です（「標準入力のコマンド」を見てください） |
| `id` | 標準入力の行の `id` をそのまま返します（行になければ `null`。ほかの `source` でも `null`） |

`command` を書くのは、標準入力からコマンドを受けたとき（`--stdin-commands`）と、Control Server のクライアントが時間の操作か停止を呼んだとき（`--control`）です。時刻つきの `pause` は、クライアントからだけ来ます。

コマンドを送って答えを待つプログラムは、自分の番号を `id` に入れ、同じ `id` の `command` のイベントを待ちます。`id` がないと、同じ名前の `command` のイベントが、ほかのプログラムが送った行への答えかもしれません。

### `state` のイベント

`state` は、実行がいまどこにいるかと、いまどの操作を受け付けるかを伝えます。ほかのイベントから組み立てずに、これを読んでください。ランチャーは、`allowed` を作るのと同じ規則で、それぞれのコマンドを判断します。

ほかのイベントと違い、`state` は履歴ではありません。意味があるのは最新の 1 つだけです。書かれるのを待っている `state` があれば（読む側が遅いとき）、新しいものがそれを置き換えます。読む側は、最後に読んだ `state` だけを持てば足ります。

| キー | 値 |
|---|---|
| `rev` | ランチャーが作った状態の番号: 1、2、3、…（飛んでいれば、書かれる前に置き換えられた状態があります。失われるものはありません） |
| `phase` | いまの段階（`phase` のイベントの値） |
| `paused` | シミュレーションが一時停止しているか。始まる前は、時刻 0 で止まることになっているか（`--start-paused`、`pause`）。シミュレーションが終わった後は `false` |
| `paused_at_ms` | 一時停止している仮想時刻（ミリ秒）。`paused` が `false` の間と、`paused` が `true` でもまだ止まっていない間（下の説明）は `null` |
| `holding` | 時刻 0 のステップの前で待っているか（`--control` と `--start-paused`。[control-api.md](control-api.md) の「始まる前に書く」） |
| `speed` | 指定の速度: 数か `"max"`。まだ分からない間は `null`（ランチャーがシミュレーションを準備する前。`speed` のコマンドが来ていれば、その値） |
| `pacing` | 指定の速度を保てているか: `"not-paced"`、`"on-time"`、`"late"`（下の表） |
| `stop` | 停止の要求: `requests` は 0、1（順に止めている）、2（参加者を待たない）。`by` は最初に求めたもの（結果の `interrupted_by` の値。なければ `null`） |
| `scenario` | シナリオを再生する実行か（`--scenario`） |
| `handed_over` | シナリオが成功して手動制御へ引き継いだか（`after_scenario: "pause"`） |
| `scenario_over_at_ms` | シナリオが引き継いだ仮想時刻（ミリ秒。引き継ぎ前や引き継ぎなしでは `null`） |
| `allowed` | 操作ごとに、いま受け付けるか（下の表） |
| `outside` | 実物のバスにつないだネットワーク（`--connect`）ごとに 1 つの要素です（ブリッジの順）。`--connect` がなければ空の配列です（下記） |

`outside[]`（[simulation-run.md](simulation-run.md#リンクとバスの状態) の「リンクとバスの状態」）:

| キー | 値 |
|---|---|
| `network`、`target`、`participant` | ネットワーク、`--connect` が渡したもの、そのブリッジの名前（`bridge.<network>`） |
| `link` | ブリッジの link の行が言う `"up"`、`"down"`、`"unknown"`。まだ言っていなければ `null` |
| `bus` | `"active"`、`"passive"`、`"bus-off"`、`"unknown"`。バスの状態を持たないブリッジ（Ethernet）や、まだ言っていないときは `null` |
| `nothing_received` | ブリッジがバスへフレームを送り、最初のフレームから 10 秒、そこの機器から何も来ていないとき `true`（注記 `connect-nothing-received` と同じ条件） |
| `condition` | `"ok"`、`"interrupted"`（リンクが落ちているか、バスが bus-off。実行は続く）、`"degraded"`（下のそれ以外）。両方なら `"interrupted"` |
| `reasons` | `condition` が `ok` でない理由（`ok` なら空）。順は `link-down`、`bus-off`、`bus-errors`（バスが error passive）、`frames-dropped`（落としたフレームの数が直近 5 秒に増えた）、`loop-suspected`（止めたループの数が直近 5 秒に増えた）、`device-shared`（この実行のものでないブリッジが同じデバイスを使っている間） |

5 秒の保持は、`state` のちらつきを防ぎます。終わったブリッジはここに出ません（`participant` と、誤り `connect-lost` を見てください）。数そのものは `outside` イベントにあります。

`state` にあるのは、段階的に変わる値だけです。仮想時刻と、増え続ける数は `time` にあります（つないだネットワークの数は `outside`）。

`paused`:

`paused` が `true` の `state` は、ふつう、止まった後に書きます。`--start-paused` の最初のものも、時刻 0 で止まってから書くので、結果の `paused_ms` は、このイベントより前から数えています。例外は次の 2 つです。どちらも、止まる予定の時刻に着いているのに、まだ止まっていない間に `true` を書きます。このときの `paused_ms` は、実際に止まってから数えます。

- 始まった後 2 秒たっても、時刻 0 で止まらなかったとき（待つのをやめて書きます）。
- 動いている間に、時刻を指定した `pause` や `run_until` が今の時刻以下を指していて、次のタイムステップで止まるとき。

止まっている時刻は `paused_at_ms` です。`paused` と同じ `state` にあるので、時刻を知るのに `time` のイベントは要りません。まだ止まっていない間は `null` です: シミュレーションが始まる前（`--start-paused` で、まだ時刻 0 に着いていない実行）と、上の 2 つの例外です。最初の刻みの前で保留している間（`holding` が `true`）は `0` です。止まったときに、時刻の入った `state` が続きます。`paused_at_ms` だけが変わったとき（ステップ実行が別の時刻で終わった）も、`state` を書きます。

`pacing`:

| 値 | 意味 |
|---|---|
| `not-paced` | 保つ速度がありません: 速度が `"max"`、一時停止中、または走っていません |
| `on-time` | 指定の速度で走っています |
| `late` | いまのタイムステップが `late_tolerance_ms` より遅れている（`time` の `late_ms`）か、最後の 5 秒に遅れたタイムステップがありました。遅れのない状態が 5 秒続くと `on-time` に戻ります |

どれだけ遅れているか、実際の速さは、`time` の `late_ms`、`slipped_ms`、`actual_speed` です。

`allowed` には、`pause`、`resume`、`step`、`speed`、`run_until`、`stop`、`write` のそれぞれに 1 つのオブジェクトがあります。

| キー | 値 |
|---|---|
| `ok` | いまその操作を受け付けるか（引数が範囲の中なら） |
| `reason` | 受け付けない理由のコード（[理由のコード](#理由のコード)。`ok` が `true` なら `null`）。いまコマンドを送ると、同じ `reason` で断られます |
| `message` | 受け付けない理由と、代わりにすること。英語の 1 文（`ok` が `true` なら `null`） |
| `caution` | `null` か `{"code", "message"}`: 受け付けるが、先に知っておくことがある操作（[注意のコード](#注意のコード)）。`ok` が `false` の間は、いつも `null` |
| `min_ms`、`max_ms` | `step` にだけあります。`ms` の範囲（`ok` が `false` の間も） |
| `min`、`max` | `speed` にだけあります。値の範囲（`ok` が `false` の間も）。`min` と `max` が同じでなければ、`"max"` も受け付けます |

`ok` は「受け付けるか」の答えで、「何かが変わるか」ではありません。一時停止中も `pause` は `ok` です（受け付けて、何もしません）。`paused` が `false` でも `step` が `ok` のことがあります: `pause` を受け付けた直後でシミュレーションが止まるまでの間と、ステップを進めている間です。

`pause`、`resume`、`step`、`run_until`、`speed`、`stop` は、標準入力のコマンドとして答えます。`write`（変数の書き込み）は Control Server のクライアントにしかないので、クライアントとして答えます。Control Server のない実行（`--control` なし）では、`write` を送る口がないので、実行の間ずっと、`ok` が `false`、理由は `no-control-server` です。クライアントの時間の呼び出しへの答えは、シナリオの間も、標準入力のコマンドへの答えと同じです。`write` はシナリオの実行中は拒否され（`scenario-running`）、`after_scenario: "pause"` でシナリオが成功して引き継いだとき（`handed_over: true`）だけ許可（`ok: true`）になります。

シミュレーションの外につながっていて実時間を保つ実行（`--time-policy realtime`、既定）では、`pause`、`step`、`run_until` は、理由 `outside-real-time` で `ok` が `false` になります。`speed` は `min` と `max` が 1 になります（受け付けるのは速さ 1 だけです）。`--time-policy free` では、`ok` は `true` で、`caution` に `outside-not-real-time` が付きます。実行を外につないでいるものは、`check` イベントの `time_lock.because` にあります。

`state` を書くとき:

| とき | 書くもの |
|---|---|
| 段階が変わった | 新しい段階の `state` を書きます。ふつうは `phase` のイベントの後。 |
| コマンドが状態を変えた（一時停止、速度、標準入力やクライアントからの停止） | その変化を反映した `state` を、そのコマンドの `command` のイベントの後に書きます。何も変えないコマンドでは書きません。`pause` はすぐに効くことがあります（速度を合わせて待っているタイムステップがあるとき）。そのときは、この `state` がもう `paused` `true` と `paused_at_ms` を持っています。 |
| シミュレーションが止まった・動き出した、`pacing` が変わった、シグナルが停止を求めた | ランチャーが次に見たとき（シミュレーションが走っている間は 20 ms ごと）。最後の `time` と別の時刻で止まっているのを見つけたときは、先にその時刻の `time` のイベントを書き、そのすぐ後にこの `state`（`paused_at_ms` つき）を書きます。 |

`state` はランチャーの複数のスレッドが書くので、ほかのイベントとの間の正確な位置には頼らないでください。`phase` のイベントや `time` のイベントの近くでは、そのイベントの直前に作られた `state` が 1 つ、そのイベントの後に来ることがあり、その `state` はまだそのイベントを反映していません。新しい段階の `state` が、その `phase` のイベントの直前に来ることもあります。変化を反映した `state` は必ず書かれ、最後の `state` は正しい内容です。イベントの後の最初の `state` ではなく、最新の `state` を使ってください。

`step` と `run_until` は、その `command` のイベントの `at_ms` でシミュレーションが止まって終わります。その時刻に着いただけでは何も書きません。ランチャーが次に見たときに、その `sim_ms` の `time` のイベントを書き、続けて `paused` が `true` で `paused_at_ms` がその時刻の `state` を書きます。着いてからコマンドを送るには、最新の `state` の `paused` が `true` で、`paused_at_ms` がその `at_ms` 以上になるのを待ってください。その時刻の `time` のイベントより先に、`state` がそう言うこともあります（コマンド自身の `state`。シミュレーションがもう着いていたとき）。`time` のイベントが来てすぐに送ったコマンドは、その `state` が書かれる前に受け付けられることがあります。そのときは、次の `state` がそのコマンドの結果を示します。

シミュレーションが走っていない間（走る前と、終わった後）に来た、シグナル（Ctrl+C）による停止の要求は、次の `state`（次の段階のもの）に現れます。

`--control` つきの実行の `state` の例（読みやすく整形しています。出力では 1 行です）:

```json
{
  "event": "state", "seq": 31, "t_ms": 1204,
  "rev": 9,
  "phase": "running",
  "paused": false,
  "paused_at_ms": null,
  "holding": false,
  "speed": 1,
  "pacing": "on-time",
  "stop": { "requests": 0, "by": null },
  "scenario": false,
  "handed_over": false,
  "scenario_over_at_ms": null,
  "allowed": {
    "pause": { "ok": true, "reason": null, "message": null, "caution": null },
    "resume": { "ok": true, "reason": null, "message": null, "caution": null },
    "step": { "ok": false, "reason": "not-paused", "message": "step works while the simulation is paused: pause it first", "caution": null, "min_ms": 1, "max_ms": 1000000000000 },
    "speed": { "ok": true, "reason": null, "message": null, "caution": null, "min": 1e-06, "max": 1e+06 },
    "run_until": { "ok": true, "reason": null, "message": null, "caution": null },
    "stop": { "ok": true, "reason": null, "message": null, "caution": null },
    "write": { "ok": true, "reason": null, "message": null, "caution": null }
  },
  "outside": []
}
```

### 理由のコード

受け付けなかった `command` のイベントの `reason` と、`state` の `allowed` の `reason` です。コードは増えることがあります。コードの意味は変わりません。

| コード | 意味 |
|---|---|
| `not-set-up` | ランチャーがまだシミュレーションを準備していません（`step`、`run_until`、時刻つきの `pause`）。ランチャーが構成ファイルを読む前に、256 個を超えるコマンドが届いたときも、これです（[時間のコマンドを受け付ける条件](#時間のコマンドを受け付ける条件)） |
| `not-paused` | 一時停止していないときの `step` |
| `not-running` | シミュレーションが終わった、または始まりませんでした |
| `scenario-running` | シナリオを再生する実行です。`after_scenario: "pause"` で引き継ぎに成功する（`handed_over: true`）まで、クライアントは変数を書けません（時間は、標準入力と同じく動かせます） |
| `no-control-server` | `state` の `allowed` にだけ出ます。Control Server のない実行（`--control` なし）で、Control Server のクライアントだけが送れる操作（`write`）です |
| `outside-real-time` | 実行がシミュレーションの外につながっていて（`--connect`、または有効な `role: bridge` の participant）、実時間を保ちます（`--time-policy realtime`、既定）。`pause`、`step`、`run_until`、1 以外の `speed` は断ります。`resume`、`speed` 1、`stop` は受け付けます。時間を動かすには、`--time-policy free` で実行を始めます（[simulation-run.md](simulation-run.md) の「外につながる実行の時間」） |
| `out-of-range` | 引数が範囲の外です（クライアントの速度、ステップ、時刻） |
| `missing-argument` | クライアントの `run_until` に時刻がありません（標準入力では `invalid-argument`） |
| `not-a-time-command` | 時間のコマンドの名前ではありません |
| `invalid-command` | コマンドでない標準入力の行: JSON のオブジェクトでない、`command` がない、64 KiB を超える |
| `unknown-command` | 標準入力が受け付けない名前のコマンド |
| `invalid-argument` | 標準入力の行の引数か `id` がない、形が違う、範囲の外 |
| `unknown-source` | ランチャーは書きません: ランチャーのライブラリが、知らない入口からのコマンドを断るときのコードです |

### 注意のコード

`state` の `allowed` の `caution` の `code` です。コードは増えることがあります。コードの意味は変わりません。

| コード | 意味 |
|---|---|
| `outside-not-real-time` | 実行がシミュレーションの外につながっていて、`--time-policy free` で始めました。`pause`、`step`、`run_until`、`speed` は受け付けますが、つながっている相手は仮想時間を待たず、ブリッジは両方向にフレームを運び続けます（シミュレーションが止まっている間、外から来たフレームは溜まります）。最初のそういうコマンドの後、実行は注記 `outside-time-changed` を付けて終わります |

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
| `--connect` のブリッジ（`bridge.<network>`）の `participant` | その `started` と `joined` は、ファイルのどの participant の `started` よりも前（[simulation-run.md](simulation-run.md#バスを外につなぐ--connect) の「バスを外につなぐ」） |
| participant の `log` | その participant の `started` より後 |
| `outside` | そのネットワークのブリッジの `started` より後、`result` より前（[つないだネットワークの数（`outside`）](#つないだネットワークの数outside)） |
| `participant` の `joined` | SIL Kit のライフサイクルで参加する participant だけ |
| `control_link` | 必須の participant の `joined` がそろった後、`phase` の `running` の前に、設定ファイルの順 |
| `participant` の `failed` | 失敗と決めた時点ですぐ |
| `command` | そのコマンドで起きること（`phase` の `stopping` など）より前 |
| `state` | ふつうは、それを変えたもののイベントの後。古い `state` が 1 つ間に挟まることがあり、最後の `state` は正しい内容です（[`state` のイベント](#state-のイベント)） |
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
| 最後の `time` と別の時刻で一時停止しているとき（一時停止が効いた、ステップ実行が終わった） | すぐに書きます。 |
| 遅れているとき（`late_ms` が許容を超えた、遅れたタイムステップが増えた） | 実時間で 100 ms に 1 回まで書きます。 |
| シミュレーションが終わったとき | 最後の `time` から仮想時間が進んでいるか、遅れていれば、間隔によらず、そのときの時刻を書きます。 |

一時停止に入った・出たことと、速度の変化だけでは、`time` を書きません。`state` を書きます。止まっている時刻は `state` の `paused_at_ms` です。最後の `time` が別の時刻のものなら、ランチャーが次に見たときに、その時刻の `time` が続きます。

最初の `time` は時刻 0 のものです。このイベントを書く時点で 0 より先に進んでいても、そうです。始まる前に受け付けた `step` や `run_until` で進んだときなどです。その後に止まっていれば、次の `time` に、止まった時刻があります。その前に次のコマンドで進むか、シミュレーションが終われば、その `time` はありません。

`sim_ms` は減りません。`vehicle_sil_launch` は状態を 20 ms ごとに読むので、それより短い間の変化（最速でのステップ実行の途中など）は書かないことがあります。最後の時刻は必ず書きます。

`finding` は実行の指摘です。照合の指摘は `check` の中にあります。シミュレーションが終わった（または始まらなかった）ときに、その時点で決まっているものを書き、残りを participant を終了させた後に書きます。一度書いた `finding` は、同じ内容のまま `result` の `findings` に入ります。同じものを 2 回は書きません。`result` だけを読んでも、結果はわかります。

シミュレーションの途中で失敗した participant の `participant-failed` は、プロセスの終わり方を含むので、プロセスが終わってから書きます。失敗に気づいた時点では、`participant` の `failed` が通知します。`vehicle_sil_launch` の `log` の行にも理由があります。

### 実行の終わり方

`phase` の `checking` を書いた実行は、シミュレーションが動いたか、準備できなかったか、照合で止まったかによらず、同じ形で終わります。読む側は、場合ごとに扱いを分ける必要はありません。

1. 時間のコマンドをもう受け付けない `state`（`allowed` の `pause`、`resume`、`step`、`run_until`、`speed` が `ok: false` で、理由が `not-running`）。
2. 構成を待っていた時間のコマンド（[時間のコマンドを受け付ける条件](#時間のコマンドを受け付ける条件)）ごとに、`accepted: false` で理由が `not-running` の `command`。届いた順に書きます。コマンドが待つのは、ランチャーがファイルから構成を作るまでです。そのため、この断りは構成を作れなかったときに出ます: ファイルが読めない（`result` が `"unreadable"`）、構成として読めない（構文の誤り、UTF-8 でない、空のファイルなど。`"check_failed"`）、引数がファイルに合わない（`--disable`、`--enable`、`--manual` の名前がない、`--variant` がない・知らない名前、上書きどうしの矛盾。`"usage"`）。
3. レジストリとコントローラーを準備できたときだけ: その時点で決まった指摘、`phase` の `stopping`、参加者の終了。照合は通ったが、その前に終わった実行は、理由を言う指摘を書いて、`stopping` なしに 4 に進みます: 始まる前に止めた（`starting` もありません。`"interrupted"`）、ログやタイムラインのファイルを開けない、Control Server を始められない、レジストリやコントローラーを準備できない（`"setup_failed"`）。ランチャー自身の内部エラーで終わった実行も同じです（指摘 `internal`）。
4. `phase` の `ended`。続いて最後の `state`（`phase` が `ended` で、時間のコマンドは受け付けないまま）。
5. `result`。

1 の `state` は、読む側が読む前に 4 の `state` に置き換わると書かれません（[`state` のイベント](#state-のイベント)）。実行が終わる間に届いたコマンドは、4 の前か、4 の `state` の後に答えます。`phase` の `ended` と最後の `state` の間には入りません。1 より後の時間のコマンドは `not-running` で断ります。標準入力の `stop` は受け付けます（[`accepted` の意味](#accepted-の意味と止めた実行の結果)）。照合の前に断った実行（コマンドラインの引数のエラー、`--control` の値のエラー）は、コマンドを読まず、`phase` も `state` も書きません。`hello` と `result` だけです。

### 実行に進まないときのイベント

`serve [--once]` は、起動時の引数エラーでも `hello` を最初に、`result` を最後に書きます。表では `state`、`status`、`run_start`、`heartbeat` の行を省きます。照合を始めた実行は、[実行の終わり方](#実行の終わり方)のとおりに終わります。

| 場合 | イベント | `result` の `result` | 終了コード |
|---|---|---|---|
| 引数のエラー（知らないオプション、設定ファイルの指定がない、`--control` の値が誤り、など） | `hello`、`result` | `"usage"` | 4 |
| 設定ファイルを読んでわかる引数のエラー | `hello`、`phase`（`checking`）、`check`、`phase`（`ended`）、`result` | `"usage"` | 4 |
| 設定ファイルを読めない | 同じ 5 つ | `"unreadable"` | 3 |
| 照合でエラーが見つかった | 同じ 5 つ | `"check_failed"` | 1 |
| 標準出力がない（閉じた状態で起動された） | 何も書けません（標準エラーにエラーを書きます）。 | | 4 |

構成ができる前に送った時間のコマンドは、ファイルが読めないとき、ファイルを読んでわかる引数のエラーのとき、ファイルが構成として読めずに照合が失敗したときに、`phase`（`ended`）の前に断ります（[実行の終わり方](#実行の終わり方)の 2）。

設定ファイルを読んでわかる引数のエラーは、次のものです。

- `--disable`、`--enable`、`--manual` の名前が設定ファイルにない。
- 機種を書いた設定ファイルに `--variant` がない。
- `--variant` の名前が設定ファイルにない。

`run` やほかのコマンド（`check`、`version`、`serve`、コマンドなし）に `--events` を付けたときは、イベントを書きません。引数のエラーとして標準エラーに書き、終了コードは 4 です。

### `log` の行

`line` は、participant が標準出力か標準エラーに書いた 1 行（行末の改行なし）、または `vehicle_sil_launch` の経過の 1 行です。`run` が標準エラーに書く行から、前の `[<名前>] ` を除いたものと同じです。

| 行の内容 | 扱い |
|---|---|
| 制御文字（タブを除く）と、UTF-8 として正しくないバイト | `?` に置き換えます。 |
| 4096 バイトを超える行 | 4096 バイト以下の複数の `log` に分けます（UTF-8 の文字の途中では切りません）。 |

分けた `log` に、続きであることを示すキーはありません。

`--log-dir` のファイルには、置き換える前のバイトをそのまま書きます。読む側が遅くて `log` を書けなかったときも、ファイルにはすべての行があります。

### つないだネットワークの数（`outside`）

`--connect` のある実行で、`run` は各ブリッジの数を統計の行から読み（[simulation-run.md](simulation-run.md#外のバスとやりとりしたフレームの数) の「外のバスとやりとりしたフレームの数」）、`outside` イベントに書きます。

| キー | 値 |
|---|---|
| `network` | `--connect` がつないだネットワーク |
| `to_outside` | ブリッジがここまでにバスへ送ったフレームの数（整数） |
| `from_outside` | ここまでにバスの機器から来たフレームの数（整数） |

- ネットワークの数が、そのネットワークの前の `outside` と違うときに書きます。ネットワークごとに 1 秒に 1 回までです。シミュレーションが走っている間は、1 秒の間に変わった数を、1 秒たったときに書きます。走る前と、participant を終わらせている間は、次に違う数が来たときか、終わりに書きます。
- ネットワークの最後の `outside` は、結果の `outside[]` のそのネットワークの数と同じです。`result` より前に来ます。ブリッジの `ended` や `phase` の `stopping` より後に来ることがあります。
- ブリッジが、`run` の読める統計の行を 1 つも書かなかったネットワークには、`outside` を書きません（結果の数は `null`）。
- `outside` は、`log` と違い、読む側が遅くても落としません。
- 数のもとの統計の行は、ブリッジが動いている間は `log` イベントにしません（ブリッジの `--log-dir` のファイルにはあります）。最後に読んだ 1 行は、そのネットワークの最後の `outside` の後、`result` の前に、ブリッジの `log` イベントとして 1 回だけ書きます。

`outside` を知らない読む側は、読み飛ばしてかまいません（最後の数は結果にあります）。

### 読む側が遅いとき、読まないとき

`vehicle_sil_launch` は、イベントを読む側を待ちません。イベントを書くのは専用のスレッドで、実行を進めるスレッド、participant の出力を読むスレッド、標準入力を読むスレッドは、イベントをキューに追加するだけです。読む側が遅くても、止まっていても、次のことは変わりません。

- participant は止まりません（出力を読み続けます）。`--log-dir` のファイルも、今までどおりに書きます。
- `--timeout-ms`、シグナル、標準入力の `stop` は、そのまま使えます。

書けずに待っているイベントが増えたときは、次のようにします。

| イベント | 扱い |
|---|---|
| `log` | 待っている `log` が 4096 行または 1 MiB を超える間は、書かずに数えます。 |
| 仮想時間が進んだことだけを通知する `time` | ほかのイベントが書けずに待っている間は、作りません。 |
| 一時停止している時刻を通知する `time` | 作ります。 |
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

`result` を書けなかったときの終了コードは 3 で、標準エラーに `error: cannot write the result to the standard output (<理由>)` と書きます。`stop` コマンドで止めた実行（書けていれば 0 か 5、エラーがあれば 1）でも同じです。結果を待つ側が、結果を受け取っていないためです。中断された実行（シグナル、標準入力の終わり、閉じた標準出力）だけは、130 で終わり、この行は書きません。

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

`event`、キー、理由のコード、`phase` の値と参加者の `state` の値は、バージョンを上げずに増えることがあります。キーの意味が変わるとき、なくなるときは `vehicle_sil_events` を上げます。形式 2 で、`time` から `paused` と `speed` がなくなりました（`state` にあります）。

`serve [--once]` のとき、標準エラーに書くのは `vehicle_sil_launch` 自身の通知（引数のエラー、イベントを書けなくなった、結果を書けない、想定外の失敗）だけで、participant の行も経過も書きません。`vehicle_sil_launch` の中のライブラリが標準出力に書いたものは、標準エラーに行きます（イベントの行に混ざりません）。

`--stdin-commands` を付けておくと、起動したプログラムが止まるか終わったときに標準入力が閉じるので、`vehicle_sil_launch` は標準出力の状態によらず、すぐに順に止まります。

### そのほかのルール

- 文字列のエスケープは `--json` と同じです（`"`、`\`、制御文字。UTF-8 でないバイトは U+FFFD）。`log` の `line` は、上のとおり先に `?` に置き換えています。
- 行の長さに上限はありません（`check` と `result` は大きくなります）。
- `serve` は `--json`、`--events`、`--stdin-commands` を取りません（指定すると引数のエラーになります。`serve` は常に NDJSON イベントを書き、標準入力のコマンドを読みます）。
- `serve --once` の終了コードは `run` と同じです（複数回実行する `serve` は [常駐ランチャー（`serve`）](#常駐ランチャーserve) を参照）。

### 例

短い実行の全行です（`--disable Dashboard` で、出力の順が決まる Engine だけにしています）。時刻、プロセス ID、registry のポート、例のディレクトリ、バージョン、時間の進み方は、実行ごとに違うので `<t_ms>`、`<pid>`、`<port>`、`<dir>`、`<version>`、`<wall_ms>`、`<actual_speed>` にしています（Windows では、実行ファイルの名前に `.exe` が付きます）。

<!-- vehicle_sil_launch serve --once simulation.yaml -->
```text
{"event":"hello","seq":0,"t_ms":<t_ms>,"vehicle_sil_events":4,"vehicle_sil":"<version>","pid":<pid>,"file":"simulation.yaml","arguments":["serve","--once","simulation.yaml"],"launcher":"<launcher>"}
{"event":"status","seq":1,"t_ms":<t_ms>,"id":null,"instance":"<launcher>","status_rev":0,"launcher_state":"unprepared","config":"simulation.yaml","eligibility":{"eligible":false,"reason":null,"message":null},"launcher_allowed":{"prepare":{"ok":true,"reason":null,"message":null,"caution":null},"run":{"ok":true,"reason":null,"message":null,"caution":null}},"active_run_id":null,"last_run_id":null,"run_state":null}
{"event":"run_start","seq":2,"t_ms":<t_ms>,"run":"<launcher>-1","id":null}
{"event":"status","seq":3,"t_ms":<t_ms>,"id":null,"instance":"<launcher>","status_rev":1,"launcher_state":"running","config":"simulation.yaml","eligibility":{"eligible":false,"reason":null,"message":null},"launcher_allowed":{"prepare":{"ok":false,"reason":"busy","message":"a run or prepare is under way (<launcher>-1): wait for it to finish (nothing is queued)","caution":null},"run":{"ok":false,"reason":"busy","message":"a run or prepare is under way (<launcher>-1): wait for it to finish (nothing is queued)","caution":null}},"active_run_id":"<launcher>-1","last_run_id":null,"run_state":{"rev":0,"phase":"checking","paused":false,"paused_at_ms":null,"holding":false,"speed":null,"pacing":"not-paced","stop":{"requests":0,"by":null},"scenario":false,"handed_over":false,"scenario_over_at_ms":null,"allowed":{"pause":{"ok":true,"reason":null,"message":null,"caution":null},"resume":{"ok":true,"reason":null,"message":null,"caution":null},"step":{"ok":false,"reason":"not-set-up","message":"the simulation has not been set up yet: step works once the simulation stands still","caution":null,"min_ms":1,"max_ms":1000000000000},"speed":{"ok":true,"reason":null,"message":null,"caution":null,"min":1e-06,"max":1e+06},"run_until":{"ok":false,"reason":"not-set-up","message":"the simulation has not been set up yet: run_until at a time works once the participants have been started","caution":null},"stop":{"ok":true,"reason":null,"message":null,"caution":null},"write":{"ok":false,"reason":"no-control-server","message":"this run has no control server, and only a control server's client can write a variable","caution":null}},"outside":[]}}
{"event":"phase","seq":4,"t_ms":<t_ms>,"run":"<launcher>-1","phase":"checking"}
{"event":"check","seq":5,"t_ms":<t_ms>,"run":"<launcher>-1","check":{"vehicle_sil_check":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"stop_at_ms":1000,"time_step_ms":10,"start_timeout_ms":30000,"stop_timeout_ms":10000,"registry_uri":null,"commands":null,"participants":[{"name":"Engine","line":11,"command":["engine/vehicle_sil_engine_ecu"],"describe":true,"required":true,"enabled":true,"manual":false,"role":"ecu","step_ms":10,"status":"described","reported":{"vehicle_sil":"<version>","name":"Engine","step_ns":10000000,"networks":[{"name":"J1939_CAN","kind":"can","use":"j1939","nodes":[{"address":0,"j1939_name":"0x00000000FE000001","arbitrary_address_capable":false}],"mac":null}]},"variables":[{"name":"ecu.connected","type":"bool","causality":"input","unit":"","min":null,"max":null,"step":null,"description":"on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on","items":[],"builtin":true},{"name":"engine.throttle","type":"real","causality":"input","unit":"%","min":0,"max":100,"step":null,"description":"accelerator pedal position","items":[],"builtin":false},{"name":"engine.speed","type":"integer","causality":"output","unit":"rpm","min":null,"max":null,"step":null,"description":"","items":[],"builtin":false}],"network_map":null,"j1939_nodes":null,"control":{"transport":"silkit","wire_version":2,"wire_revision":1}},{"name":"Dashboard","line":14,"command":["dashboard/vehicle_sil_dashboard_ecu"],"describe":true,"required":true,"enabled":false,"manual":false,"role":"ecu","step_ms":5,"status":"disabled","reported":null,"variables":null,"network_map":null,"j1939_nodes":null,"control":null}],"networks":[{"name":"J1939_CAN","kind":"can","line":8,"participants":["Engine"],"outside":null}],"time_lock":{"locked":false,"because":[]},"findings":[{"severity":"warning","code":"single-participant","file":"simulation.yaml","line":8,"participant":null,"network":"J1939_CAN","message":"network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it; if it talks only to equipment outside, connect it outside to run it (external says what passes)","detail":null},{"severity":"note","code":"participant-disabled","file":"simulation.yaml","line":14,"participant":"Dashboard","network":null,"message":"participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it","detail":null}],"file_sha256":"ac4f0ee1fb9346d58d80d2b8a994c4b95b6e8194ecffdb41a06f3acf204d7f1b","hot_eligibility":{"eligible":true,"reason":null,"message":null}}}
{"event":"participant","seq":6,"t_ms":<t_ms>,"run":"<launcher>-1","name":"Dashboard","state":"disabled"}
{"event":"phase","seq":7,"t_ms":<t_ms>,"run":"<launcher>-1","phase":"starting"}
{"event":"registry","seq":8,"t_ms":<t_ms>,"run":"<launcher>-1","uri":"silkit://127.0.0.1:<port>"}
{"event":"log","seq":9,"t_ms":<t_ms>,"run":"<launcher>-1","source":"vehicle_sil_launch","line":"registry at silkit://127.0.0.1:<port>"}
{"event":"log","seq":10,"t_ms":<t_ms>,"run":"<launcher>-1","source":"vehicle_sil_launch","line":"starting Engine: <dir>/engine/vehicle_sil_engine_ecu --name Engine --registry-uri silkit://127.0.0.1:<port> --start-timeout-ms 50000"}
{"event":"participant","seq":11,"t_ms":<t_ms>,"run":"<launcher>-1","name":"Engine","state":"started","pid":<pid>,"command":["<dir>/engine/vehicle_sil_engine_ecu","--name","Engine","--registry-uri","silkit://127.0.0.1:<port>","--start-timeout-ms","50000"],"log":null}
{"event":"log","seq":12,"t_ms":<t_ms>,"run":"<launcher>-1","source":"vehicle_sil_launch","line":"not starting Dashboard: disabled"}
{"event":"phase","seq":13,"t_ms":<t_ms>,"run":"<launcher>-1","phase":"waiting"}
{"event":"participant","seq":14,"t_ms":<t_ms>,"run":"<launcher>-1","name":"Engine","state":"joined"}
{"event":"control_link","seq":15,"t_ms":<t_ms>,"run":"<launcher>-1","name":"Engine","state":"connected","variables":22}
{"event":"phase","seq":16,"t_ms":<t_ms>,"run":"<launcher>-1","phase":"running"}
{"event":"log","seq":17,"t_ms":<t_ms>,"run":"<launcher>-1","source":"vehicle_sil_launch","line":"simulation running"}
{"event":"time","seq":18,"t_ms":<t_ms>,"run":"<launcher>-1","sim_ms":0,"actual_speed":<actual_speed>,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0}
{"event":"time","seq":19,"t_ms":<t_ms>,"run":"<launcher>-1","sim_ms":1000,"actual_speed":<actual_speed>,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0}
{"event":"log","seq":20,"t_ms":<t_ms>,"run":"<launcher>-1","source":"vehicle_sil_launch","line":"the simulation ended"}
{"event":"phase","seq":21,"t_ms":<t_ms>,"run":"<launcher>-1","phase":"stopping"}
{"event":"log","seq":22,"t_ms":<t_ms>,"run":"<launcher>-1","source":"vehicle_sil_launch","line":"Engine exited with code 0"}
{"event":"participant","seq":23,"t_ms":<t_ms>,"run":"<launcher>-1","name":"Engine","state":"ended","exit_code":0,"signal":null}
{"event":"phase","seq":24,"t_ms":<t_ms>,"run":"<launcher>-1","phase":"ended"}
{"event":"result","seq":25,"t_ms":<t_ms>,"run":"<launcher>-1","launcher_ends":true,"status":{"instance":"<launcher>","status_rev":2,"launcher_state":"ending","config":"simulation.yaml","eligibility":{"eligible":false,"reason":null,"message":null},"launcher_allowed":{"prepare":{"ok":false,"reason":"launcher-ending","message":"the launcher has been asked to end and takes no run any more: start a new launcher for the next run","caution":null},"run":{"ok":false,"reason":"launcher-ending","message":"the launcher has been asked to end and takes no run any more: start a new launcher for the next run","caution":null}},"active_run_id":null,"last_run_id":"<launcher>-1","run_state":null},"result":{"vehicle_sil_run":2,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","end":"completed","ok":true,"message":"The run reached its end without errors","strict":false,"errors":0,"warnings":1,"notes":1,"simulation":"reached_stop_at","interrupted_by":null,"registry_uri":"silkit://127.0.0.1:<port>","stop_at_ms":1000,"timing":{"speed":"max","time_step_ms":10,"ecu_step_ms":10,"step_delay_ms":0,"late_tolerance_ms":10,"sim_ms":1000,"wall_ms":<wall_ms>,"actual_speed":<actual_speed>,"paced_steps":0,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0,"paused_ms":0},"participants":[{"name":"Engine","required":true,"enabled":true,"manual":false,"for_network":null,"ended":"exited","exit_code":0,"signal":null,"left_processes":false,"log":null,"control_link":{"state":"connected","variables":22,"version":null,"late_messages":0}},{"name":"Dashboard","required":true,"enabled":false,"manual":false,"for_network":null,"ended":"not_started","exit_code":null,"signal":null,"left_processes":false,"log":null,"control_link":{"state":"none","variables":null,"version":null,"late_messages":0}}],"outside":[],"traffic":{"before_ms":1000,"unplaced":[],"networks":[{"name":"J1939_CAN","kind":"can","counted":1,"senders":1,"receivers":0,"delivered":false,"unplaced":[],"participants":[{"name":"Engine","counted":true,"not_counted":null,"virtual_time":true,"at_ms":990,"frames_sent":11,"frames_received":0}]}]},"findings":[],"check":{"vehicle_sil_check":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"stop_at_ms":1000,"time_step_ms":10,"start_timeout_ms":30000,"stop_timeout_ms":10000,"registry_uri":null,"commands":null,"participants":[{"name":"Engine","line":11,"command":["engine/vehicle_sil_engine_ecu"],"describe":true,"required":true,"enabled":true,"manual":false,"role":"ecu","step_ms":10,"status":"described","reported":{"vehicle_sil":"<version>","name":"Engine","step_ns":10000000,"networks":[{"name":"J1939_CAN","kind":"can","use":"j1939","nodes":[{"address":0,"j1939_name":"0x00000000FE000001","arbitrary_address_capable":false}],"mac":null}]},"variables":[{"name":"ecu.connected","type":"bool","causality":"input","unit":"","min":null,"max":null,"step":null,"description":"on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on","items":[],"builtin":true},{"name":"engine.throttle","type":"real","causality":"input","unit":"%","min":0,"max":100,"step":null,"description":"accelerator pedal position","items":[],"builtin":false},{"name":"engine.speed","type":"integer","causality":"output","unit":"rpm","min":null,"max":null,"step":null,"description":"","items":[],"builtin":false}],"network_map":null,"j1939_nodes":null,"control":{"transport":"silkit","wire_version":2,"wire_revision":1}},{"name":"Dashboard","line":14,"command":["dashboard/vehicle_sil_dashboard_ecu"],"describe":true,"required":true,"enabled":false,"manual":false,"role":"ecu","step_ms":5,"status":"disabled","reported":null,"variables":null,"network_map":null,"j1939_nodes":null,"control":null}],"networks":[{"name":"J1939_CAN","kind":"can","line":8,"participants":["Engine"],"outside":null}],"time_lock":{"locked":false,"because":[]},"findings":[{"severity":"warning","code":"single-participant","file":"simulation.yaml","line":8,"participant":null,"network":"J1939_CAN","message":"network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it; if it talks only to equipment outside, connect it outside to run it (external says what passes)","detail":null},{"severity":"note","code":"participant-disabled","file":"simulation.yaml","line":14,"participant":"Dashboard","network":null,"message":"participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it","detail":null}],"file_sha256":"ac4f0ee1fb9346d58d80d2b8a994c4b95b6e8194ecffdb41a06f3acf204d7f1b","hot_eligibility":{"eligible":true,"reason":null,"message":null}}}}
```

この例は、CTest の `launch_run.events` が、実際の出力と、バイト単位で照合します。照合の前に、実際の出力から `heartbeat` の行、`state` の行、最初と最後のほかの `time` の行を除き（`seq` は詰めます）、上の置き換えをします。遅い PC では、途中の時刻の `time` も書くためです。`state` の行は、いくつ書かれるかが読む速さで変わるためです（[`state` のイベント](#state-のイベント)）。

## 標準入力のコマンド（`run --stdin-commands`）

`run` では `--stdin-commands` を付けたときだけ標準入力を読みます（付けないときは標準入力が閉じていても空でも止まりません）。`serve [--once]` は `--stdin-commands` なしで常に標準入力のコマンドを読みます。1 行に 1 つの JSON のオブジェクトです。機械可読のイベントとあわせて使う場合は `serve [--once]` を使います（[読む側が守ること](#読む側が守ること)、[常駐ランチャー（`serve`）](#常駐ランチャーserve)）。

| 行 | 意味 |
|---|---|
| `{"command":"stop"}` | 順に止めます（1 回目の Ctrl+C と同じ）。 |
| `{"command":"pause"}` | 一時停止します。 |
| `{"command":"resume"}` | 一時停止をやめて進みます。 |
| `{"command":"step","ms":<ms>}` | 一時停止している間だけ: 仮想時間を `<ms>`（1 以上 10^12 以下の整数）だけ進めて、また止まります。 |
| `{"command":"run_until","at_ms":<ms>}` | 進み続けて、仮想時刻 `<ms>`（0 以上 10^12 以下の整数。タイムステップの倍数に切り上げます）で一時停止します。[Control API](control-api.md) の `RunUntil` と同じで、`--control` は要りません。 |
| `{"command":"speed","value":<速度>}` | 速度を変えます（`<速度>` は数か `"max"`。`run --speed` と同じ範囲）。 |
| 上のどれかに `"id":<id>` を付けたもの | 同じです。`<id>`（0 以上 2^53 以下の整数）が、その行の `command` のイベントの `id` に返ります。 |
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
| `step` | `command` のイベントの `at_ms` は、その step の後に止まる仮想時刻です。step が着いたことは、前に読んだ時刻に `ms` を足して判断せず、最新の `state` の `paused` が `true` で、`paused_at_ms` がその時刻（以上）になるのを待ってください。後から来た `pause`、`resume`、`run_until`、`step` と、実行の終わりは、止まる時刻を変えます。 |
| `step` | 前の `step` か `run_until` の途中で送ると、その止まる時刻から数えます。 |
| `step` | `pause` を受け付けてから止まるまでの間に送ると、進めている途中のタイムステップから数えます。進行中だった `run_until` や `step` は、その `pause` で終わっています（その瞬間に、速度を合わせて待っているタイムステップがあってもなくても同じです）。 |
| `step` | クライアントの時刻つきの `pause`（[Control API](control-api.md)、`at_ms`）の後、まだその時刻に着いていないときに送ると、その時刻（その `pause` の `command` のイベントの `at_ms`）から数えます。 |
| `run_until` | 動いている間も、一時停止している間も使えます。止まっている時刻がすでに指定の時刻以上なら、そのまま止まっていて、何も起きません。動いている間に過ぎた時刻なら、次のタイムステップで止まります。`command` のイベントの `at_ms` は、頼んだ時刻をタイムステップの倍数に切り上げたものです。過ぎた時刻では止まる時刻と一致しないので、`at_ms` を待たず、最新の `state` の `paused` が `true` で `paused_at_ms` が入るのを待って、それを読んでください。 |
| `speed` | 一時停止はそのままです。 |
| EOF | すでに止める要求（`stop`、シグナル、閉じた標準出力）を受けていれば、何もしません（2 回目に数えません。`command` のイベントも書きません）。 |
| それ以外 | JSON のオブジェクトでない、`command` がない、知らないコマンド、`id` が 0 以上 2^53 以下の整数でない、64 KiB を超える行がこれに当たります。 |
| それ以外 | `serve [--once]` なら `command` のイベント（`accepted: false`、`message`、`reason`）を、`run --stdin-commands` なら標準エラーに 1 行を書きます。 |

オブジェクトのほかのキーは無視します。`at_ms` を付けた `pause` は、時刻なしの `pause` として扱います（時刻で止めるには `run_until` を使います）。`--stdin-commands` のない実行でも、`state` のイベントは `run_until` を標準入力の答えとして示します。イベントを読む側が読んでいなくても、コマンドは受け付けて実行します。

### 時間のコマンドを受け付ける条件

時間のコマンド（`pause`、`resume`、`step`、`run_until`、`speed`）は、標準入力を読むスレッドが、受け付けるかを決めて `command` のイベントを書き、それから controller に渡します。どれも待ちません（実行を進めるスレッドも、イベントを読む側も待ちません）。controller は、次のタイムステップの境目で適用します。

| 場合 | 扱い |
|---|---|
| `step` を、一時停止していないときに送った（`resume` の後など） | 受け付けません。 |
| `step` か `run_until` を、controller ができる前（照合の間など）に送った | 受け付けません。 |
| シミュレーションが終わった後（始まらなかったときも）の時間のコマンド | 受け付けません。 |
| 一時停止しているときの `pause`、動いているときの `resume` | 受け付けて、何もしません。 |
| controller ができる前（照合の間など）に受けた `pause`、`resume`、`speed` | 覚えておき、controller ができたときに渡します。 |
| ランチャーが構成ファイルを読む前に受けた時間のコマンド | 決めずに待たせます。ファイルを読んだ後に答えます（下）。 |
| 外につながっていて実時間を保つ実行での `pause`、`step`、1 以外の `speed` | 受け付けません（`outside-real-time`）。 |

受け付けなかった理由は `message`（文）と `reason`（コード。[理由のコード](#理由のコード)）に書きます。コマンドを送る前には、`state` のイベントの `allowed` が同じことを伝えます。controller ができる前に受けた `pause` は、`--start-paused` と同じく 0 で止まります。

一時停止しても、`phase` は `running` のままです。一時停止は `state` の `paused` で通知します。

実行が実時間を保つかどうか（`--connect`、`role: bridge` の participant、`--time-policy`）は、ランチャーが構成ファイルを読むまで分かりません。それより前に届いた時間のコマンドは、届いたときには決めません。

- 待たせます。`command` イベントは、ファイルを読んだ後に、そのときの答えで書きます。待たせたコマンドには、届いた順に、後から届くどのコマンドよりも先に答えます。`command` イベントの順は、コマンドが効いた順のままです。
- `stop` は待たせません。待っているものがあっても、すぐ受け付けます。
- 待たせるのは 256 個までです。それを超えたコマンドは、理由 `not-set-up` ですぐ断ります。`check` イベントの後に、送り直してください（そのときにはファイルを読み終えています。最初の `state` イベントは、それより前に出ます）。
- ランチャーがファイルから構成を作れなければ（読めない、構成として読めない、引数がファイルに合わない）、何も始まりません。待たせたコマンドは、`phase` の `ended` の前に、理由 `not-running` で断ります（[実行の終わり方](#実行の終わり方)）。
- ファイルを読む前に書いた `state` イベントは、まだこの規則を知りません。その `allowed` は、外につながっていない実行としての答えです。次の `state` から、この規則が入ります。

`--control` と `--start-paused` で時刻 0 のステップの前に止まる実行では、止まるより前に受け付けた `resume`、`step`、`run_until`、`pause` も、その後の止まり方を決めます（[control-api.md](control-api.md) の「始まる前に書く」）。

### `accepted` の意味と、止めた実行の結果

`command` のイベントの `accepted: true` は、行がコマンドとして正しく、処理に渡したという意味です（時間のコマンドでは、今できるという意味でもあります）。そのコマンドで実行が変わったとは限りません。たとえば、実行が終わった後（`phase` の `ended` の後）に受けた `stop` は、`accepted: true` ですが、結果は変わりません。

`command` のイベントは、そのコマンドで起きること（`phase` の `stopping` など）より前に書きます。

`{"command":"stop"}` か、Control Server のクライアントの `Stop` で止めた実行は、中断ではありません。`result` は `"stopped"`、終了コードは 0 です（注記 `stopped`）。シナリオが終わる前に止めたときは、`"incomplete"` と 5 です。シミュレーションが終わった後（participant が終わる間）に来た停止が `end` と `result` を変えるのは、必須の participant の終わりを打ち切ったときだけです。そうでなければ、シミュレーションの終わり方のとおりです。標準入力の終わりは中断に数えます（`"interrupted"`、130）。コマンドを書いていたプログラムがいなくなったためです。[simulation-run.md](simulation-run.md#実行の終わり方endokmessage) の「実行の終わり方」を見てください。

| 止めたもの | `interrupted_by` | `end` |
|---|---|---|
| `{"command":"stop"}` | `"stop command"` | `"stopped"`（シミュレーションの終わりより前。後は [実行の終わり方](simulation-run.md#実行の終わり方endokmessage) のとおり） |
| 標準入力の終わり | `"end of input"` | `"interrupted"` |
| Control Server のクライアントの `Stop` | `"control server stop"` | `"stopped"` |

### Control Server のクライアントからのコマンド

`--control` の Control Server のクライアントの時間の操作（`Pause`、`Resume`、`Step`、`RunUntil`、`SetSpeed`）と `Stop` も、同じコマンドとして処理します（`source` が `"control_server"`）。規則と順序は [control-api.md](control-api.md) の「標準入力とクライアントのコマンド」にあります。

| 項目 | 標準入力 | クライアント |
|---|---|---|
| `--stdin-commands` がないとき | 読みません。 | 受け付けます。 |
| 時刻つきの `pause` | 送れません。 | 送れます。 |
| シミュレーションが終わった後の停止 | `stop` を受け付けて、何もしません。 | `Stop` を拒否します。 |

シナリオつきの実行（`--scenario`）は、どちらからの時間のコマンドも受け付けます。シナリオの結果は変わりません（[scenario.md](scenario.md) の「実行中に変数を見る」）。

## バージョンと機能の一覧（`vehicle_sil_launch version`）

`vehicle_sil_launch version`（`--version` も同じ）は、`vehicle_sil_launch <version>` の 1 行を書きます。`version --json` は、書く形式のバージョンと、機能の一覧を書きます。どちらもファイルを読まず、終了コードは 0 です。

<!-- vehicle_sil_launch version --json -->
```json
{
  "vehicle_sil_launch": 1,
  "vehicle_sil": "<version>",
  "formats": {
    "check": 1,
    "run": 2,
    "events": 4,
    "describe": 1,
    "test": 1,
    "timeline": 1,
    "connections": 1,
    "control_metrics": 1
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
    "scenario-motion-expect",
    "scenario-points-file",
    "timeline",
    "timeline-lines",
    "scenario-inputs",
    "network-names",
    "run-traffic",
    "j1939-nodes",
    "connect",
    "connections",
    "connect-counts",
    "state",
    "time-policy",
    "stdin-run-until",
    "request",
    "scenario-samples",
    "run-stop-at",
    "serve",
    "check-file-only",
    "serve-prepare",
    "control-metrics",
    "scenario-end-ms",
    "scenario-handover",
    "scenario-cases",
    "scenario-templates",
    "scenario-variable-compare"
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
| `test` | `test --json` の `vehicle_sil_test` |
| `timeline` | `run --timeline` が書くファイルの `vehicle_sil_timeline` |
| `connections` | `connections --json` の `vehicle_sil_connections` |
| `control_metrics` | `run --control-metrics` が書くファイルの `vehicle_sil_control_metrics` |

### JSON の形のキーと値

上のどの JSON の形（`check --json`、`run --json`、`test --json`、`connections --json`、イベント、`version --json`、`--timeline` のファイル、`--control-metrics` のファイル）にも、次の決まりがあります。

- キーは snake_case です（`time_step_ms`、`not_applied_from`）。
- vehicle-sil 0.1.0 の後に足したキーは、いつもあります。当てはまらないときの値は `null` です。0.1.0 にあったキーには、当てはまるときだけ書くものがあり、いつ書くかはそれぞれの説明にあります。たとえば、シナリオの `params`、`skipped_steps`、`unchecked_expects`、手順の `expect_skipped`、participant の `variable_map` です。
- 形そのものの語である値（状態、結果、理由、出どころ）は、下線でつなぎます（`not_run`、`after_wait`）。
- 人が打ったり探したりする名前は、ハイフンのままです。`features` の名前（`run-traffic`）と、指摘の `code`（`single-participant`）です。

### `features` の名前

| `features` の名前 | 機能 |
|---|---|
| `events` | NDJSON イベントストリーム（`serve [--once]`、`vehicle_sil_events: 4`） |
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
| `scenario-points-file` | シナリオの `wave` の `points` を CSV ファイルから読む（`points: { file, value, time, time_unit }`。[scenario.md](scenario.md) の「点を CSV ファイルから読む」） |
| `timeline` | `run --scenario --timeline <file>`: シナリオの実行を時間の軸で記録したファイル（[simulation-run.md](simulation-run.md) の「シナリオの実行を時間の軸で記録する（`--timeline`）」） |
| `timeline-lines` | `--timeline` のファイルの `lines[]`（入力ごとの線: 手順が置いた値と動き、動きの始まりの値とその出どころ）と、手順の `writes[]`（[simulation-run.md](simulation-run.md) の同じ節） |
| `scenario-inputs` | シナリオの最上位の `inputs`（入力に重ねる `dither`）と手順 `dither`（[scenario.md](scenario.md) の「細かい揺れを重ねる（`dither`）」） |
| `network-names` | 設定ファイルの `network_names`（[simulation-config.md](simulation-config.md) の「ネットワークの名前を合わせる（`network_names`）」） |
| `run-traffic` | 実行の結果の `traffic`（ネットワークごとの J1939 の送受信の数と `delivered`。[simulation-run.md](simulation-run.md) の「ネットワークの送受信（`traffic:`）」） |
| `j1939-nodes` | 設定ファイルの `j1939_nodes`（[simulation-config.md](simulation-config.md) の「J1939 のアドレスと NAME を合わせる（`j1939_nodes`）」） |
| `connect` | `check` と `run` の `--connect`。ネットワークをブリッジで実機のバスにつなぐこと（[simulation-run.md](simulation-run.md#バスを外につなぐ--connect) の「バスを外につなぐ（`--connect`）」） |
| `connections` | `vehicle_sil_launch connections`: この PC で各ネットワークをどこにつなげるか（[simulation-run.md](simulation-run.md#つなげる先の一覧vehicle_sil_launch-connections) の「つなげる先の一覧（`vehicle_sil_launch connections`）」） |
| `connect-counts` | つないだネットワークが実機のバスへ送ったフレームと、そこの機器から来たフレームの数（[simulation-run.md](simulation-run.md#外のバスとやりとりしたフレームの数) の「外のバスとやりとりしたフレームの数」） |
| `state` | イベント `state`: 実行がいまどこにいるかと、いま受け付ける操作（[`state` のイベント](#state-のイベント)） |
| `time-policy` | `check`、`run`、`test` の `--time-policy`。シミュレーションの外につながる実行は、`free` を選ばなければ実時間を保ちます（[simulation-run.md](simulation-run.md#外につながる実行の時間--time-policy) の「外につながる実行の時間（`--time-policy`）」） |
| `stdin-run-until` | 標準入力の `run_until`（[標準入力のコマンド](#標準入力のコマンドrun---stdin-commands)を見てください） |
| `request` | `check` と `run` の `--request <file>`。何を走らせるかを要求ファイルから読みます（[simulation-run.md](simulation-run.md#要求ファイル--request) の「要求ファイル（`--request`）」） |
| `scenario-samples` | シナリオの最上位のキー `samples` と、シナリオが値を読む ECU のステップごとのサンプル（[scenario.md](scenario.md#ecu-が値を送る頻度samples) の「ECU が値を送る頻度」） |
| `run-stop-at` | Run 単位の `stop_at_ms` と `late_tolerance_ms` に対応。 |
| `serve` | `vehicle_sil_launch serve`: 1 つのランチャープロセスで Run ID とイベント窓を分離して順次実行する（[常駐ランチャー](#常駐ランチャーserve)） |
| `check-file-only` | `check --file-only --json`: 実行ファイルを起動せず設定テキストのみ検証し、`file_sha256` と `hot_eligibility` を出力する（[JSON（`--json`）](simulation-check.md#json--json)） |
| `serve-prepare` | `serve` の `prepare`、`status`、`stop.force`、インライン `run.intent` コマンド（[常駐ランチャー](#常駐ランチャーserve)） |
| `control-metrics` | `run --control-metrics <file>`: Control Hub の SIL Kit トランスポートの累計カウンターを JSON ファイルに書く（`vehicle_sil_control_metrics: 1`、[simulation-run.md](simulation-run.md#control-hub-トランスポートのメトリクスを記録する--control-metrics) の「Control Hub トランスポートのメトリクスを記録する（`--control-metrics`）」） |
| `scenario-end-ms` | シナリオのトップレベル `end_ms` キーとシナリオ JSON 出力の `end_ms`（[scenario.md](scenario.md)） |
| `scenario-handover` | 実行要求の `after_scenario: "pause"`：シナリオ成功時に一時停止して手動制御へ引き継ぎ、書き込みを動的に許可する（`state` の `handed_over` と `scenario_over_at_ms`） |
| `scenario-cases` | シナリオの名前付き `cases:`、`--case`・要求ファイルの `"case"`、`case_details` と複数ケース `check --json` の `usage` メタデータ保持、`serve` インライン `intent.cli_params` |
| `scenario-templates` | 再利用可能なシナリオテンプレート（`vehicle_sil_scenario_template: 1`）、`call_stack` 出典情報、テンプレート由来のパラメーターメタデータ（`param_meta` と `missing_params`） |
| `scenario-variable-compare` | シナリオ条件での実行時の変数間比較（`{ variable: <participant>.<変数> }`、[scenario.md](scenario.md#条件) の「条件」） |
| `control-server` | `run --control`（Control Server を持つビルドだけ） |
| `scenario-control` | `run --scenario` と `--control` を一緒に使える（Control Server を持つビルドだけ） |
| `scenario-time` | シナリオの実行中に、Control Server のクライアントが時間を動かせる（Control Server を持つビルドだけ） |

名前ごとに、次のものがあることを示します。

| `features` の名前 | 含むもの |
|---|---|
| `speed` | 設定ファイルの `time_step_ms`、実行要求の `speed` と `late_tolerance_ms`、`run --speed`、結果の `timing`、指摘 `behind-real-time`、指摘 `coarse-time-step`、`time` のイベントの `actual_speed`、`late_steps`、`max_late_ms`、`slipped_ms`、`late_ms` |
| `control-links` | イベント `control_link`、結果の `participants[].control_link`、照合の JSON の `participants[].control`、指摘 `control-link-no-hello`、`control-link-version`、`control-link-revision`、`control-link-late-values`、`control-link-dropped`、`control-link-not-offered` |
| `manual-participants` | 設定ファイルの `manual`、`--manual`、照合と結果の JSON の `participants[].manual`、指摘 `participant-manual`、イベント `participant` の `state` の `manual` |
| `scenario` | 結果と照合の JSON の `scenario`、イベント `scenario_step`、指摘 `scenario-*`、指摘の `file` がシナリオのファイルになること |
| `scenario-wave` | その手順の `undersampled`、指摘 `scenario-wave-*`、結果とイベントの手順の `kind` の `"wave"` |
| `scenario-motion-expect` | 結果とイベントの手順の `expect_skipped` |
| `scenario-points-file` | 指摘 `scenario-points-file` |
| `timeline` | 結果の `scenario.timeline`、指摘 `timeline-write` と `timeline-limit` |
| `scenario-inputs` | 指摘 `scenario-dither-*` と `scenario-input-skipped`、結果とイベントの手順の `kind` の `"dither"`、照合と結果の JSON の `scenario.inputs` と `scenario.lines`、`--timeline` のファイルの `lines[]` の `kind` の `"dither"` と、`series` の 3 列目（`columns`） |
| `network-names` | 実行ファイルに渡す `--map-network`、照合の JSON の `participants[].network_map`、指摘 `network-map-*` |
| `run-traffic` | 人向けの結果の `traffic:` の行と表、`run --json` とイベント `result` の `traffic` |
| `j1939-nodes` | 実行ファイルに渡す `--j1939-node`、照合の JSON の `participants[].j1939_nodes`、指摘 `j1939-node-*`、`address-conflict`、`address-shared`、`name-conflict` の終わりの設定ファイルでの直し方、照合の participant の表の `0x01<-0x00` |
| `connect` | 設定ファイルのネットワークの `external`、`check --connect` と `run --connect`、照合の JSON の `networks[].outside`、結果の `outside` と `participants[].for_network`、結果とイベントの participant `bridge.<network>`、人向けの結果の `outside:` の行、指摘 `connect-*` |
| `connections` | `vehicle_sil_launch connections` とその `--json` と `--network`、`formats` の `connections`、指摘 `connect-target` と `connect-failed` が挙げる `vehicle_sil_launch connections` |
| `connect-counts` | イベント `outside`、結果の `outside[].to_outside` と `from_outside` の数（この機能の前は `null`）、人向けの結果の `outside:` の行の末尾の数、指摘 `connect-nothing-received`、`run` が起こすブリッジのコマンドの `--stats-every-ms 1000` |
| `state` | イベント `state`、イベント `command` の `reason` と `id`、標準入力のコマンドの `id` |
| `time-policy` | `--time-policy`、照合の JSON の `time_lock`、指摘 `outside-real-time`、`outside-speed-set`、`outside-not-real-time`、`outside-time-changed`、`unsynchronized-time-changed`、断ったコマンドと `allowed` の理由 `outside-real-time`、注意 `outside-not-real-time`（この機能の前は、警告 `connect-not-real-time`、`connect-start-paused`、`connect-time-changed` で、`--connect` のある実行はファイルの速さで動き、時間のコマンドをすべて受け付けました） |
| `request` | `--request`、指摘 `connect-network-unused` と `request-params-unused`、照合と結果の JSON でパラメーターの `from` が `request_file` になること |
| `scenario-samples` | シナリオの `samples`、実行ファイルに渡す `--sample-every-step`、指摘 `scenario-samples-*` と `scenario-not-observed` |
| `control-server` | イベント `control_server`、`command` の `source` の `"control_server"` と `at_ms`、`--control-credentials`、`--control-tls-cert`、`--control-tls-key`、`--control-audit-log`、`interrupted_by` の `"control server stop"`、`--start-paused` で時刻 0 のステップの前に止まること |
| `scenario-end-ms` | シナリオファイルの最上位 `end_ms`、および `check --json`・`run --json`・`--timeline` の `end_ms` |
| `scenario-handover` | 実行要求の `after_scenario`（`"stop"` \| `"pause"`）、`state` イベントの `handed_over` と `scenario_over_at_ms`、引き継ぎ時の動的な書き込み解放 |
| `scenario-cases` | シナリオの `cases:`、`--case` および要求ファイル・`serve` `intent` の `"case"`、`scenario.case`（未選択・不明時は `null`）、`cases`、`case_details`、インライン `intent.cli_params` |
| `scenario-templates` | シナリオテンプレート（`- template:`）、`steps[].file`・`call_stack`、`scenario.param_meta`、`scenario.missing_params`、`scenario.params[]` のテンプレート由来 `type`・`values`・`description` |
| `scenario-variable-compare` | `expect` と `wait_until` の `{ variable: ... }`、`--timeline` の `watch.conditions[].compare[]` の `variable` と `failed.conditions[]` の `compared`、両辺の値と時刻を示す失敗メッセージ |

`scenario-control` のあるビルドでは、シナリオの実行中、クライアントは書けません。書く呼び出しは、`FAILED_PRECONDITION`（理由 `scenario-running`）で拒否します。`scenario-time` のあるビルドでは、時刻を動かす呼び出し（`Pause`、`Resume`、`Step`、`RunUntil`、`SetSpeed`）を、シナリオの実行中も、標準入力の同じコマンドと同じように受け付けます。シナリオの結果は変わりません。`scenario-control` だけのビルドは、これらも同じ理由で拒否します（[scenario.md](scenario.md) の「実行中に変数を見る」）。

上の例は、Control Server を持たないビルドの出力です。持つビルドは、`features` の最後に `"control-server"`、`"scenario-control"`、`"scenario-time"` を追加します。この例も、CTest の `launch_cli.docs` が照合します（`vehicle_sil` のバージョンと、この 3 つを除いて、バイト単位で）。

## 常駐ランチャー（`serve`）

`vehicle_sil_launch serve [--once] <file>` は、ランチャーの寿命を通じて 1 本の NDJSON ストリームを標準出力に書きます（`hello` の `vehicle_sil_events` は 4）。`seq` はストリーム全体で 0 から欠けなく 1 ずつ増え、`t_ms` は後戻りしません。`--once` を付けると 1 回だけ実行し、`result`（`"launcher_ends": true`）を書いた後に終了します。

### ランチャー ID と Run ID

`hello` には `"launcher": "<instance>"`（ランダムな 64 ビット値の小文字 16 進 16 文字）が入ります。受理された `run` コマンドごとに不透明な Run ID `"<instance>-<n>"`（`n` は 1 から連番。拒否された `run` コマンドは番号を進めません）が採番されます。呼び出し側は一致比較だけを行い、認証情報の代わりにしないでください。

### ストリームの寿命とイベント窓

| `event` | 範囲 | 意味とキー |
|---|---|---|
| `hello` | ランチャー（先頭 1 回） | `"vehicle_sil_events": 4`、`"vehicle_sil"`、`"pid"`、`"file"`、`"arguments"`、`"launcher": "<instance>"` |
| `status` | ランチャー（`run` なし） | ランチャー状態遷移時（`unprepared`、`cold_only`、`preparing`、`ready`、`running`、`ending`、`contaminated`）および `{"command":"status"}` への応答: `"id"`、`"instance"`、`"status_rev"`、`"launcher_state"`、`"config"`、`"eligibility"`、`"launcher_allowed"`、`"active_run_id"`、`"last_run_id"`、`"run_state"` |
| `run_start` | 実行（`"run": "<ID>"`） | 実行のイベント窓を開く。`"id"` に `run` コマンドの `"id"` を返す（省略時は `null`） |
| 実行イベント | 実行（`"run": "<ID>"`） | `run_start` と `result` の間のすべてのイベント（`phase`、`check`、`control_server`、`registry`、`participant`、`control_link`、`log`、`log_dropped`、`outside`、`finding`、`scenario_step`、`time`、`state`、`command`、`heartbeat`）に `"run": "<ID>"` が付く。`state.rev` は実行ごとにリセットされる |
| `result` | 実行（`"run": "<ID>"`） | 実行のイベント窓を閉じる。`"launcher_ends": <bool>`、`"status": {...}`（確定後の `ExecutionStatus`）、`"result": {...}` を持つ。この後に同じ `"run"` のイベントは出ない |
| `command` | ランチャー（`run` なし） | ランチャー直下の拒否（`accepted: false`）。`"asked_run"`（指定がなければ `null`）を追加 |
| `prepare` | ランチャー（`run` なし） | `{"command":"prepare"}` への応答: `"id"`、`"accepted"`、`"reason"`、`"message"`、`"status"`（`ExecutionStatus`）、`"metadata"`（`participants`、`networks`、`variants`、`file_sha256`。`!accepted` 時は `null`）、`"findings"` |
| `heartbeat` | ランチャーまたは実行 | 待機中（`run` なし）も実行中（`"run"` 付き）も出力 |

起動時の使い方の誤り（`--once` なしの `VEHICLE_SIL_TRACE` 設定時を含む）では、`hello` の後にランチャー直下の `result`（`run` なし、`"launcher_ends": true`）を最終行として終了します。

### `serve` の標準入力コマンド

```json
{"command":"prepare","id":1,"variant":"fast","disable":["Logger"]}
{"command":"status","id":2}
{"command":"run","id":3,"request":"bench.json","timeline":"timeline.json"}
{"command":"run","id":4,"intent":{"speed":2,"start_paused":true}}
{"command":"stop","run":"0123456789abcdef-1","force":false,"id":5}
```

- `prepare`: 束縛中の `<file>` を `--file-only` 相当で検証し（任意の `"variant"`、`"disable"`、`"enable"`、`"manual"` 配列を指定可能。`"config"` は拒否）、`status`、`metadata`、`file_sha256`、`findings` を含む `prepare` イベントを返します。実行中や終了処理中は `"busy"`、`"launcher-ending"`、`"contaminated"` で拒否（`accepted: false`）します。
- `status`: 現在のランチャーおよび実行中 Run の状態（待機中の `run_state` は `null`）を `status` イベントで返します。
- `run`: 待機中なら実行を開始します。`"request"`（要求 JSON ファイルのパス）と `"intent"`（64 KiB の行上限以内のインライン実行要求 JSON オブジェクト。`"vehicle_sil_run_request"` は任意で、`"case"` や `"after_scenario"`、さらに `--param` 相当の解釈・優先順位で読まれるインライン専用の `"cli_params": { "<name>": "<value>" }` など各フィールドを指定可能）は排他です。`"timeline"`（タイムライン JSON の出力先パス）と `"id"` は任意です。実行中または検証中は `"busy"`、終了処理中は `"launcher-ending"` で拒否します。
- `stop`、`pause`、`resume`、`step`、`run_until`、`speed`: 複数回実行の `serve` では `"run": "<Run ID>"` が必須です（欄がなければ `"run-required"`、現在の実行と異なれば `"run-changed"` と `"asked_run"` で拒否）。`serve --once` では `"run"` を省略できます。`stop` では任意の真偽値 `"force"` で冪等な通常停止（`false`、停止要求 1 回）か即時強制停止（`true`、2 回へ引き上げ）を指定します。ランチャー直下のコマンド（`run`、`prepare`、`status`）は `"run"` を省略するか妥当な文字列であれば無視します。
- 標準入力の閉鎖（EOF）はランチャー終了を意味します（待機中なら終了コード 0、実行中ならその実行を止めて終了コード 130）。

## 関連するドキュメント

- 実行の流れ、止め方、結果の JSON → [simulation-run.md](simulation-run.md)
- 照合の JSON → [simulation-check.md](simulation-check.md)
- gRPC で変数を読み書きし、時間を操作する → [control-api.md](control-api.md)
- ブラウザの GUI と、その開発 → [gui.md](gui.md)

ブラウザの GUI のテストは、Events v4 のシナリオランチャースタブ（`gui/server/tests/stub_launcher.py`）とインプロセス契約検証（`gui/server/tests/fake_runtime.py`）を使います。ランチャーの出力を変えたら、スタブと検証器を合わせ、`python3 scripts/verify-gui.py --launcher <vehicle_sil_launch>` で実際のランチャーと照合します（[gui.md](gui.md) の「開発」）。
