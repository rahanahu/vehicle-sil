# シナリオ（`vehicle_sil_launch run --scenario`）

まとめる人と、シミュレーションで試験をする人向けです。先に [simulation-config.md](simulation-config.md)（構成ファイル、照合、実行）を読みます。

シナリオは、決まった手順で ECU の変数に値を入れ、結果を確かめるファイルです。`vehicle_sil_launch run <構成ファイル> --scenario <シナリオ>` は、シミュレーションを起動し、シナリオを最後まで進めて止め、終了コードで合否を返します。そのまま自動試験になります。

同じ構成と同じシナリオなら、結果（どの手順が、どの仮想時刻に、どの値で通ったか、落ちたか）はいつも同じです。機械の速さ、最速か等速か（`speed`）、ECU の処理の重さでは変わりません。判定には仮想時刻だけを使い、実時間の待ちや時間切れは使いません。

変数は、`ecu_main` で作った ECU が宣言したものと、どの ECU にもある `ecu.connected` です（[ecu-executable.md](ecu-executable.md) の「変数を出す」）。名前は `vehicle_sil_launch check --json` の `participants[].variables` でわかります。gRPC は使いません（Control Server のないビルドでも動きます）。

## 例

[examples/ecu_main/scenarios/throttle_step.yaml](../examples/ecu_main/scenarios/throttle_step.yaml) は、[examples/ecu_main](../examples/ecu_main) の Engine と Dashboard のシナリオです。

```yaml
vehicle_sil_scenario: 1
name: throttle step
steps:
  - at_ms: 0
    set: { Engine.engine.throttle: 50 }
  - wait_until: { Engine.engine.speed: { ge: 1700 } }
    timeout_ms: 500
  - after_ms: 50
    expect: { Engine.engine.speed: { near: 1800, tolerance: 10 } }
    hold_for_ms: 100
  - expect: { Dashboard.dashboard.engine_speed: { ge: 1700 } }
    within_ms: 200
  - ramp: { Engine.engine.throttle: { to: 0, over_ms: 200 } }
  - expect: { Engine.engine.speed: { le: 1000 } }
    within_ms: 100
```

```sh
vehicle_sil_launch check simulation.yaml --scenario scenarios/throttle_step.yaml   # 照合だけ
vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml
```

実行の標準出力です（participant の行と、手順ごとの行 `[vehicle_sil_launch] scenario: line 10: ...: ok at 330 ms` は標準エラーに出ます）。

<!-- vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

scenario scenarios/throttle_step.yaml (throttle step): 6 steps

run: the simulation ran until the scenario was over (705 ms) and ended

scenario scenarios/throttle_step.yaml (throttle step): passed, 6 of 6 steps passed

line  step                                                              result  at
8     set Engine.engine.throttle = 50                                   ok      0 ms
10    wait_until Engine.engine.speed ge 1700 (timeout 500 ms)           ok      330 ms
12    expect Engine.engine.speed near 1800 (tolerance 10) (for 100 ms)  ok      490 ms
15    expect Dashboard.dashboard.engine_speed ge 1700 (within 200 ms)   ok      490 ms
17    ramp Engine.engine.throttle to 0 over 200 ms                      ok      700 ms
18    expect Engine.engine.speed le 1000 (within 100 ms)                ok      700 ms

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

passed: 0 errors, 0 warnings, 0 notes
```

`at` は、その手順を決めた仮想時刻です（下の「結果」）。構成ファイルの `stop_at_ms` は 1000 ms ですが、シナリオが終わった 705 ms で止まります。

### ECU をバスから外す

`<participant>.ecu.connected` に `false` を書くと、その ECU は、動いたまま、すべてのバスから外れます（送ったものは出ず、バスのものは届きません。`true` で戻ります。[architecture.md](architecture.md) の「ECU の入口」の「バスからの切り離し」）。ほかの ECU が、相手が落ちたことを正しく検出するかを試せます。[examples/ecu_main/scenarios/engine_disconnect.yaml](../examples/ecu_main/scenarios/engine_disconnect.yaml) は、300 ms に Engine を外し、Dashboard が回転数の途絶を検出し（`dashboard.engine_lost`。回転数が 300 ms 来なければ true）、Engine を戻すと回復することを確かめます。

```yaml
steps:
  - at_ms: 300
    set: { Engine.ecu.connected: false }
  - expect: { Dashboard.dashboard.engine_lost: true }
    within_ms: 400
  - set: { Engine.ecu.connected: true }
  - expect: { Dashboard.dashboard.engine_lost: false }
    within_ms: 200
```

## ファイルの形式

YAML です（`{` で始まれば JSON）。構成ファイルとは別のファイルにします。形式はまだ変わることがあります。

| キー | 必須 | 意味 |
|---|---|---|
| `vehicle_sil_scenario` | はい | 形式の版。`1` |
| `name` | いいえ | シナリオの名前（出力に出る） |
| `steps` | はい | 手順の並び（1 つ以上）。上から順に進む |

1 つの手順は、`set`、`ramp`、`wait_until`、`expect` のどれか 1 つと、時刻を持ちます。

| キー | 意味 |
|---|---|
| `at_ms` | 手順の時刻（仮想時刻、ミリ秒、0 以上） |
| `after_ms` | 手順の時刻を、前の手順が終わった時刻からのミリ秒で書く（0 以上）。`at_ms` も `after_ms` もなければ `after_ms: 0` |
| `set: { <変数>: <値>, ... }` | 値を入れる |
| `ramp: { <変数>: { to: <値>, over_ms: <ms>, from: <値> }, ... }` | `over_ms` の間、直線で値を動かす。`from` は省ける |
| `wait_until: { <変数>: <条件>, ... }` と `timeout_ms: <ms>` | 条件が成り立つまで待つ。`timeout_ms`（必須）の間に成り立たなければ失敗 |
| `expect: { <変数>: <条件>, ... }` | 手順の時刻に、条件が成り立つことを確かめる |
| `expect` と `within_ms: <ms>` | その間に 1 度でも成り立てばよい。成り立った時点で次へ進む |
| `expect` と `hold_for_ms: <ms>` | その間ずっと成り立つ |

- **変数**は `<participant>.<変数名>` です。最初の `.` までが participant の名前で、残りが ECU の中の変数名です（変数名に `.` があってよい。例: participant `Engine` の `engine.speed` は `Engine.engine.speed`）。
- **値**は、変数の型に合わせて書きます。bool は `true` / `false`、integer は整数、real は数、enum は項目の名前か値です。
- **条件**は比較だけです。値をそのまま書けば「等しい」です。対応付けで `eq`、`ne`、`ge`、`le`、`gt`、`lt`、`near` と `tolerance`（差が `tolerance` 以下）を書けます。1 つの変数に複数書けば、すべてです（`{ ge: 1700, le: 1900 }`）。1 つの手順に複数の変数を書けば、すべての変数です。bool と enum に使えるのは `eq` と `ne` だけです。
- 書けるのは、入力の変数です。Parameter は、時刻 0 の `set` でだけ書けます（最初のステップの前に入ります）。出力は読むだけです。`set` と `ramp` の値は、変数の範囲（`min`、`max`）の中にします。`ramp` は integer と real だけです（integer は四捨五入）。

## 時刻

手順の時刻 T は、`at_ms` か、前の手順が終わった時刻 + `after_ms` です。最初の手順の前は 0 です。

| 手順 | 終わる時刻 |
|---|---|
| `set`、`expect`（その時点） | T |
| `ramp` | T + 最も長い `over_ms` |
| `expect` と `hold_for_ms` | T + `hold_for_ms` |
| `wait_until`、`expect` と `within_ms` | 成り立ったサンプルの時刻 t の、次の controller のステップ P の、さらに 1 刻み後（P + 刻み） |

- 上の 3 つは、実行の前に終わる時刻がわかります。これらの手順は、前の結果によらず、書いた時刻どおりに進みます。
- `wait_until` と `within_ms` つきの `expect` は、終わる時刻が実行するまで決まりません。ランチャーは、時刻 t のサンプルを、その次の controller のステップ P で確かに持っています。次の手順の書き込みが間に合う最初の時刻が P + 刻みなので、そこを終わりとします。例では、回転数が 1700 になったサンプルが 330 ms、P が 335 ms、終わりが 340 ms です。
- そのため、これらの手順より後ろには `at_ms` を書けません（`after_ms` で書きます）。時刻が戻る `at_ms` も誤りです。
- controller の刻みは、構成ファイルの `time_step_ms` です。書いていなければ、ECU のステップがすべて controller のステップから始まる刻み（結果の `timing.time_step_ms`。[simulation-config.md](simulation-config.md) の「等速で動かす」の、速さを決めたときの既定と同じ）です。シナリオのある実行では、速さによらずこの刻みです。

## 値の見え方

ECU は、Agent の `sample_period`（既定 10 ms の仮想時間）ごとに、ステップの終わりの全変数の値を 1 組（サンプル）送ります。サンプルの時刻は、そのステップの始まりの時刻です。

- 「時刻 T の値」は、変数ごとに、その ECU の、T 以前に始まったステップの最後のサンプルの値です。T に始まるステップで入った `set` の値は、そのステップがサンプルを取るときだけ、T のサンプルに入っています。サンプルを取るのは、`sample_period` の倍数の時刻以後の最初のステップです（既定では、10 ms の倍数の時刻に始まるステップ。ステップが 5 ms の ECU の 105 ms のステップは取りません）。その時刻なら、`at_ms: 100` の `set` の直後に `at_ms: 100` の `expect` を書けます。そうでない時刻の `expect` は、前のサンプル（`set` の前の値）を見て失敗します。値が入ったことを確かめるには、`within_ms` つきの `expect` を使います（次のサンプルで成り立ちます）。ランチャーは ECU の `sample_period` を知らないので、照合ではわかりません。
- `wait_until` と `within_ms` は、T の値と、その後に届くサンプルを 1 つも飛ばさずに見ます。1 サンプルだけ成り立つ変化も見逃しません。`hold_for_ms` も同じで、1 サンプルでも外れれば、そのサンプルの時刻で失敗です。
- `sample_period` より細かい変化は見えません。サンプルとサンプルの間に変わって戻った値は、条件に現れません。
- まだサンプルが 1 つもない変数（最初のステップの前）は、どの条件も成り立ちません。

## 書き込み

- `set` は、時刻 T を付けた書き込みです。各 ECU の、T 以降に始まる最初のステップの先頭で入り、そのステップのアプリケーションが新しい値を見ます。T が ECU のステップの間なら、次のステップで入ります。
- ランチャーは、時間のゲートで controller の各ステップを止め、次の刻みまでの書き込みを ECU に積み終えてから通します。時刻 0 の分は、全員が最初のステップの前で待っている間に積みます。書き込みが、書いた時刻のステップに間に合わないことはありません。
- 1 つの `set` に同じ participant の変数を複数書くと、同じステップでまとめて入ります。
- `ramp` は、変数ごとに、T、T + ステップ、T + 2 ステップ、…（その ECU のステップ）に、`from` から `to` への直線の上の値を書き、T + `over_ms` に `to` を書きます。`from` を省くと、シナリオがその変数に最後に書いた値（それより新しいサンプルがあれば、その値）から始めます。どちらもなければ失敗です（`from` を書きます）。
- 書き込みの結果は、適用する ECU のステップの次の controller のステップで確かめます。適用されなかった書き込み（ECU の setter が拒んだ、相手がつながっていない、など）は、その手順の失敗です。

## 結果

- 手順が 1 つ失敗すると、シナリオはそこで終わりです（仮想時刻の順で最初の失敗）。ランチャーはシミュレーションを止め、実行は失敗します（終了コード 1）。始まっていた手順は `unfinished`、その後ろは `not_run` です。後ろの手順の書き込みのうち、次の刻みまでの分は、すでに ECU に渡っていることがあります。
- 全部通ると、ランチャーはその controller のステップでシミュレーションを止め、実行は成功します（ほかに誤りがなければ終了コード 0）。構成ファイルに `stop_at_ms` がなくても回せます。
- `stop_at_ms` に先に着くと、決められる所まで決めて、決まっていない最初の手順が失敗です（`scenario-out-of-time`。`stop_at_ms` を大きくするか、消します）。
- 止める要求（Ctrl+C、`stop` の命令）、participant の失敗、`--timeout-ms` は、シナリオより優先です（[simulation-config.md](simulation-config.md) の規則のまま）。そのとき、シナリオは `incomplete` で、誤り `scenario-incomplete` が、どこまで進んだかを言います。落ちた ECU への書き込みが先に失敗しても、実行の結果は participant の失敗で、シナリオは `incomplete` です。
- 手順ごとの `at`（JSON の `at_ms`）は、その手順を決めた仮想時刻です。`set` は値を適用した ECU のステップ、`ramp` は終わり、その時点の `expect` は T、`wait_until` と `within_ms` は成り立ったサンプルの時刻、`hold_for_ms` は終わり（失敗なら外れたサンプルの時刻）、時間切れは切れた時刻です。

失敗の所見は、シナリオのファイルと行、手順、時刻、そのときの値を言います。

```text
error: s.yaml:4: wait_until Body.ticks ge 10; Body.level eq 6 (timeout 100 ms): timed out at 100 ms (100 ms after 0 ms): Body.level eq 6, but it was 5 (its sample of 100 ms)
```

| `code`（実行の所見、どれも誤り） | 意味 |
|---|---|
| `scenario-failed` | `expect` が成り立たなかった。または、読む変数の participant がこの実行でつながっていない |
| `scenario-timeout` | `wait_until` が `timeout_ms` の間に成り立たなかった |
| `scenario-write` | `set` か `ramp` の書き込みが適用されなかった。または、`ramp` の始まりの値がない |
| `scenario-out-of-time` | `stop_at_ms` で止まったとき、まだ決まっていなかった |
| `scenario-incomplete` | シナリオが終わる前に、ほかの理由でシミュレーションが終わった |

## 照合（`check --scenario`、`run --scenario` の最初）

シナリオは、実行の前に全部照合します。`vehicle_sil_launch check <構成ファイル> --scenario <シナリオ>` は照合だけをします。誤りがあれば、`run` は何も起動しません。所見は、シナリオのファイル名と行を付けて、構成ファイルの所見と一緒に出ます（JSON では所見の `file`）。

| `code` | 意味 |
|---|---|
| `file`、`encoding`、`syntax`、`not-a-scenario`、`format-version`、`unsupported-yaml` | ファイルが読めない、シナリオでない、版が違う、アンカーや 2 つ目の文書がある |
| `unknown-key`、`duplicate-key`、`missing-key`、`value` | キーと値の形の誤り（1 つの手順に 2 つの動作、`wait_until` に `timeout_ms` がない、`near` に `tolerance` がない、など） |
| `scenario-variable` | 変数が `<participant>.<変数名>` でない、participant か変数がない、participant を使えない（外している、`describe: false`、実時間で動く、変数を出していない） |
| `scenario-not-writable` | 出力に書いている。Parameter を時刻 0 の `set` 以外で書いている |
| `scenario-value` | 値が型、範囲、enum の項目に合わない。`ramp` や大小の比較を、数でない変数に使っている |
| `scenario-time` | `at_ms` が前の手順の終わりより前。待つ手順より後ろに `at_ms` がある。手順の時間を足すと、時刻の上限（1000000000000 ms）を超える |

## 出力

- 人向け: 照合の部分に `scenario <ファイル> (<name>): <n> steps`、実行の部分に、合否の 1 行と手順の表（行、手順、結果、決めた仮想時刻）。実行中は、手順が決まるたびに 1 行（標準エラー）。
- `check --json` と `run --json` の `check`: `scenario` に `file`、`name`、`steps`（手順の数）。`--scenario` を付けたときだけあります。
- `run --json`: 最上位に `scenario`（`--scenario` を付けたときだけ）。

| キー | 意味 |
|---|---|
| `file`、`name` | シナリオのファイル（引数のとおり）と名前（なければ null） |
| `status` | `passed`、`failed`、`incomplete`（ほかの理由で途中で終わった）、`not_run`（シミュレーションが始まらなかった） |
| `over_at_ms` | シナリオが終わった controller のステップ（`passed` と `failed`。ほかは null） |
| `steps[]` | 手順ごとに `index`（1 から）、`line`、`kind`（`set`、`ramp`、`expect`、`wait_until`）、`text`、`status`（`passed`、`failed`、`unfinished`、`not_run`）、`start_ms`（手順の時刻）、`at_ms`（決めた時刻）、`end_ms`（終わる時刻）、`code` と `message`（失敗のとき） |

- 時刻はミリ秒の数です（ECU のステップが 1 ms より細かいと、小数になることがあります）。わからない時刻は null です。
- シナリオが終わってランチャーが止めた実行の `simulation` は `ended` です。
- `run --events`: 手順が始まったとき（`status` は `running`）と決まったときに、事象 `scenario_step`（`steps[]` と同じキー）。結果の事象にも `scenario` があります。
- `vehicle_sil_launch version --json` の `features` に `scenario` があれば、この機能を使えます。

## 一緒に使えない引数と制限

- `--scenario` は `--control` と `--start-paused` と一緒に使えません（引数の誤り）。Control API のクライアントが変数を書いたり、開始を止めたりすると、同じシナリオが同じ結果にならなくなるためです。`--stdin-commands` は使えます（`stop`、一時停止、ステップ送り、速さ。止めて進めても、シナリオの結果は変わりません）。
- シナリオが使えるのは、構成ファイルにあって外していない、`ecu_main` で作った、仮想時間で動く ECU の変数だけです。
- フレームを直に待つ条件、式、機種ごとの切り替え、変数の対応表はありません。
