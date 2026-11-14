# シナリオで試験を書く

シミュレーションで試験をする人向けの手引きです。前に [ECU をまとめて動かす](integrate-ecus.md)（構成ファイル、`check`、`run`）を読んでおきます。次は [scenario.md](../scenario.md)（形式、時刻、値の見え方、結果の規則の正本）で、探りながら値を触るときは [信号を外から操作する](operate-signals.md) です。

決まった手順で ECU の変数に値を入れて確かめるシナリオを書き、照合で誤りを見つけ、回して、終了コードと結果を読むまでの順番です。手引きに書くのは手順とつまずく所だけです。キー、条件、時刻の決め方、所見の `code` の正本は scenario.md です。

例には [examples/ecu_main](../../examples/ecu_main) の 2 つの ECU（Engine と Dashboard）と、[examples/ecu_main/scenarios](../../examples/ecu_main/scenarios) の 2 つのシナリオを使います。CMake が、構成ファイルと一緒に `build/<preset>/examples/ecu_main/` に写します。以下のコマンドは、そのディレクトリで実行します。

```mermaid
flowchart LR
    names["変数の名前"] --> write["シナリオ"]
    write --> check["check --scenario"]
    check --> run["run --scenario"]
    run --> result["終了コードと結果"]
```

## 使える変数を調べる

シナリオが読み書きするのは、`ecu_main` で作った ECU が宣言した変数と、どの ECU にもある `ecu.connected` です。Control Server を持たないビルドのランチャーでも動きます（gRPC は使いません）。

- 名前と型は、`vehicle_sil_launch check --json simulation.yaml` の `participants[].variables` か、ECU の `--describe` の `variables` で調べます（[ecu-executable.md](../ecu-executable.md) の「JSON の形式」）。
- シナリオには `<participant>.<変数名>` で書きます。participant `Engine` の `engine.speed` は `Engine.engine.speed` です。
- 書けるのは入力の変数だけです。出力は読むだけで、Parameter は時刻 0 の `set` でだけ書けます。
- 使えない participant があります（外したもの、`describe: false`、実時間で動くもの）。規則は [scenario.md](../scenario.md) の「一緒に使えない引数と制限」です。

## シナリオを書く

[examples/ecu_main/scenarios/throttle_step.yaml](../../examples/ecu_main/scenarios/throttle_step.yaml) の 5〜19 行です。スロットルを踏み、回転数が上がるのを待ち、表示の ECU に届いたことを確かめ、スロットルを戻します。

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

- 手順は上から順に進みます。1 つの手順に、動作（`set`、`ramp`、`wave`、`wait_until`、`expect`）を 1 つと、時刻を書きます。`ramp` と `wave` にだけは、横に `expect` を並べられます（下の表）。
- 時刻は `at_ms`（仮想時刻）か `after_ms`（前の手順が終わった時刻から）です。どちらもなければ `after_ms: 0` です。
- 条件は比較だけです（`ge`、`le`、`near` と `tolerance` など）。キーと条件の一覧は [scenario.md](../scenario.md) の「ファイルの形式」です。

### 入れる、待つ、確かめるの使い分け

| したいこと | 書くもの | 例の行 |
|---|---|---|
| ある時刻に値を入れる | `set` | 8〜9 |
| 時間をかけて値を動かす（integer と real だけ） | `ramp`（`to`、`over_ms`。`from` は省ける） | 17 |
| 正弦、矩形波、パルス、乱数などの波形で値を動かす | `wave`（下の「波形で動かす」） | — |
| 試験の前提がそろうまで待ち、そこから先の時刻を決める | `wait_until` と `timeout_ms`（必須） | 10〜11 |
| その時刻に成り立つ | `expect` だけ | — |
| 決めた時間のうちに 1 度でも成り立つ（応答の遅れの上限） | `expect` と `within_ms` | 15〜16、18〜19 |
| 決めた時間のあいだずっと成り立つ | `expect` と `hold_for_ms` | 12〜14 |
| 値を動かしている間ずっと成り立つ | `ramp` か `wave` の横に `expect` | — |

- `wait_until` と、`within_ms` つきの `expect` は、どちらも成り立つまで待ちます。違いは失敗の扱いです。`wait_until` の時間切れは `scenario-timeout`、`expect` が成り立たないのは `scenario-failed` です。前提の待ちには `wait_until`、試験したい応答には `expect` を使うと、結果からどちらで落ちたかがわかります。
- `set`、`ramp`、`wave` の値は、変数の範囲（`min`、`max`）の中にします。文面からわかる分は、外れていれば照合の誤りです。
- 1 つでも失敗すると、シナリオはそこで終わります。後ろの手順は回りません。

### 波形で動かす

[examples/ecu_main/scenarios/throttle_wave.yaml](../../examples/ecu_main/scenarios/throttle_wave.yaml) の 13〜22 行です。
スロットルを正弦で揺らし、回転数が元に戻ることを確かめます。次に、100 ms だけ全開にして、回転数が上がって戻ることを確かめます。

```yaml
  # 5 % up and down around the 50 % it has, ten times a second, for 200 ms. The engine takes its
  # inputs every 10 ms, so it gets 10 values per period. At the end the throttle is at 50 % again.
  - wave: { Engine.engine.throttle: { sine: { amplitude: 5, period_ms: 100 }, for_ms: 200 } }
  - expect: { Engine.engine.speed: { near: 1800, tolerance: 100 } }
    hold_for_ms: 50
  # Full throttle for 100 ms, then back to the 50 %: the speed has risen, and comes back.
  - wave: { Engine.engine.throttle: { pulse: { value: 100, width_ms: 100 } } }
  - expect: { Engine.engine.speed: { ge: 2200 } }
  - expect: { Engine.engine.speed: { near: 1800, tolerance: 50 } }
    within_ms: 200
```

- 波形は `sine`、`triangle`、`sawtooth`、`square`、`pulse`、`noise`、`chirp`、`lag`、`chatter`、`points` です。書くものと使える型は [scenario.md](../scenario.md) の「波形で動かす（`wave`）」です。
- `around` を省くと、いまの値を中心に動かし、終わると元の値に戻します。上の `sine` は 50 % の周り、`pulse` の後は 50 % です（`pulse` は、いつも始まる時点の値に戻ります。`around` は書けません）。
- `wave` は、終わるまで次の手順に進みません。波形を掛けた後の結果は、`wave` の後の手順で確かめます。
- 動かしている間ずっと成り立つことは、`ramp` か `wave` の横に `expect` を書いて確かめます。手順の時刻から終わりまでの全部のサンプルを見て、1 つでも外れればそのサンプルの時刻で失敗です。`within_ms` と `hold_for_ms` は書けません。

  ```yaml
    - ramp: { Engine.engine.throttle: { to: 80, over_ms: 1000 } }
      expect: { Engine.engine.speed: { le: 3000 } }
  ```

  構成に `expect` の変数がないとき（`absent: true`）は、`expect` だけを飛ばして動かします。決まりの全体は [scenario.md](../scenario.md) の「動かしている間ずっと確かめる」です。
- ECU のステップより細かい波形（周期がステップ幅の 2 倍より短い、パルスがステップ幅より短い、など）は、照合が警告を出し、ECU に実際に見える形を言います。わかっていて流すときは、その手順に `undersampled: ok` を書きます。
- `noise` には `seed` が要ります。同じ `seed` なら毎回同じ値なので、落ちた実行をそのまま再現できます。

## つまずく所

### 値を入れた直後に、同じ時刻で確かめる

`set` の値は、その時刻以後に始まる ECU のステップの先頭で入ります。ところが、シナリオが見る値は、ECU が `sample_period`（既定 10 ms）ごとに送るサンプルです。`set` と同じ時刻の `expect` は、そのステップがサンプルを取らないと、`set` の前の値を見て失敗します。

- 値が入ったことを確かめるときは、`within_ms` つきの `expect` にします。次のサンプルで成り立ちます。
- ランチャーは ECU の `sample_period` を知らないので、照合ではこの誤りを見つけられません。
- 詳しい規則は [scenario.md](../scenario.md) の「値の見え方」です。

### 待つ手順の後の時刻

`wait_until` と `within_ms` つきの `expect` は、実行するまで終わる時刻が決まりません。

- これらより後ろの手順には `at_ms` を書けません（照合の誤り `scenario-time`）。`after_ms` で書きます。
- 終わる時刻は、成り立ったサンプルの時刻ではなく、少し後（その次の controller のステップのさらに 1 刻み後）です。結果の表の `at` は成り立った時刻なので、次の手順の `after_ms` は `at` からではなく、その終わりから数えます。例では、回転数が 1700 になったサンプルが 330 ms、終わりが 340 ms です。
- 決め方は [scenario.md](../scenario.md) の「時刻」です。

### サンプルの周期より細かい変化は見えない

- サンプルとサンプルの間で変わって戻った値は、条件に現れません。`sample_period` より短いパルスは、サンプルを取るステップにかからなければ、`within_ms` でも `hold_for_ms` でも見えません。ECU のステップが 5 ms でも、既定ではサンプルは 10 ms ごとです。
- 細かい変化を見たいときは、ECU を書く人に `sample_period` を小さくしてもらいます（[control-api.md](../control-api.md) の「ECU の変数」の `set_agent_options()`）。量が増えるので、必要な ECU だけにします。
- まだサンプルが 1 つもない変数（最初のステップの前）は、どの条件も成り立ちません。

### そのほか

- 構成ファイルの `stop_at_ms` に先に着くと、決まっていない最初の手順が `scenario-out-of-time` で失敗します。`stop_at_ms` を大きくするか、消します。シナリオが終わればシミュレーションも止まるので、`stop_at_ms` がなくても回せます。
- `--scenario` は `--start-paused` と一緒に使えません（引数の誤り）。`--control` とは一緒に使えますが、シナリオの実行中、クライアントは読む・待つ・止めるだけです（[scenario.md](../scenario.md) の「実行中に変数を見る」）。シナリオの実行に外から値を書くと、同じ結果にならなくなるためです。Python から触りながら確かめたいときは、[信号を外から操作する](operate-signals.md) の形にします。

## 照合する（`check --scenario`）

```sh
vehicle_sil_launch check simulation.yaml --scenario scenarios/throttle_step.yaml
```

- 構成ファイルと一緒にシナリオを照合します。シミュレーションは始めません。
- `run --scenario` も最初に同じ照合をし、誤りがあれば何も起動しません。

誤りを 5 つ入れたシナリオ（[scenario.md](../scenario.md) の「照合」の `throttle_mistakes.yaml`。変数の名前の打ち間違い、出力への書き込み、範囲の外の値、待つ手順の後ろの `at_ms`、`tolerance` のない `near`）を照合した出力です。

<!-- vehicle_sil_launch check simulation.yaml --scenario throttle_mistakes.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

scenario throttle_mistakes.yaml (throttle mistakes): 5 steps

error: throttle_mistakes.yaml:5: 'Engine.engine.throttel': participant 'Engine' has no variable 'engine.throttel': did you mean 'Engine.engine.throttle'? (vehicle_sil_launch check --json lists the variables) [scenario-variable]
error: throttle_mistakes.yaml:6: Engine.engine.speed is an output of its ECU: it can be read (expect, wait_until), not written; write one of the ECU's inputs instead [scenario-not-writable]
error: throttle_mistakes.yaml:7: the value of Engine.engine.throttle (real) is 150, outside its range (from 0 to 100 %) [scenario-value]
error: throttle_mistakes.yaml:10: at_ms after a step that waits (line 8): when that step ends is not known before the run; give this step after_ms (from the end of the step before it) instead [scenario-time]
error: throttle_mistakes.yaml:11: Engine.engine.speed: near needs tolerance (how far from the value it may be): add tolerance: <number> [value]

failed: 5 errors, 0 warnings, 0 notes
```

- 所見は 1 行に 1 つで、`<シナリオのファイル>:<行>:` で始まり、何が違うかと直し方を言います。誤りは最初の 1 つで止めずに全部挙げます。終了コードは 1 です。
- 構成ファイルの所見も同じ形で一緒に出ます。所見の `code` の一覧は [scenario.md](../scenario.md) の「照合」です。

## 回す（`run --scenario`）

```sh
vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml
```

標準出力です（participant の行と、手順が決まるたびの行 `[vehicle_sil_launch] scenario: line 10: ...: ok at 330 ms` は標準エラーに出ます）。

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

- 照合の部分に `scenario ... : 6 steps` の行が増えます。
- `run:` の行のとおり、シナリオが終わった 705 ms で止まります（構成ファイルの `stop_at_ms` は 1000 ms）。
- 手順の表の `line` はシナリオのファイルの行、`at` はその手順を決めた仮想時刻です。`at` が何を指すか（`set` は値が入った ECU のステップ、`ramp` は終わり、など）は [scenario.md](../scenario.md) の「結果」です。

## 結果を読む

- 終了コードは、全部通れば 0、手順が 1 つでも失敗すれば 1 です（ほかの誤りも 1。中断は 130。表は [simulation-config.md](../simulation-config.md) の「引数と終了コード」）。

失敗するシナリオ（[scenario.md](../scenario.md) の「結果」の `throttle_response.yaml`。スロットルを踏んでから 200 ms のうちに回転数が 1700 rpm に届くことを確かめるが、届くのは 330 ms）を回した標準出力です。

<!-- vehicle_sil_launch run simulation.yaml --scenario throttle_response.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

scenario throttle_response.yaml (throttle response): 3 steps

run: the simulation ran until the scenario was over (205 ms) and ended

scenario throttle_response.yaml (throttle response): FAILED, 1 of 3 steps passed

line  step                                                             result   at
4     set Engine.engine.throttle = 50                                  ok       0 ms
6     expect Engine.engine.speed ge 1700 (within 200 ms)               FAILED   200 ms
8     expect Dashboard.dashboard.engine_speed ge 1700 (within 200 ms)  not run  -

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

error: throttle_response.yaml:6: expect Engine.engine.speed ge 1700 (within 200 ms): not true at any time within 200 ms (from 0 ms to 200 ms): at the end Engine.engine.speed ge 1700, but it was 1050 (its sample of 200 ms) [scenario-failed]

failed: 1 error, 0 warnings, 0 notes
```

- 誤りの所見が、シナリオのファイルと行、手順、決めた時刻、そのときの値（どのサンプルの値か）を言います。この例では、待つ時間を延ばすか（`within_ms: 400`）、前提の待ちにするなら `wait_until` にします。
- シミュレーションは、失敗を決めた所で止まります（`run:` の行の 205 ms）。失敗した手順の後ろは回らず、`not_run`（表では `not run`）です。そのとき始まっていた手順は `unfinished` です。
- participant の失敗、Ctrl+C、`--timeout-ms` は、シナリオより先に効きます。そのときシナリオは `incomplete` で、誤り `scenario-incomplete` がどこまで進んだかを言います。
- 同じ構成と同じシナリオなら、結果（どの手順が、どの時刻に、どの値で通ったか）はいつも同じです。機械の速さや `--speed` では変わりません。

## ECU をバスから外して、ほかの ECU の検出を確かめる

どの ECU にもある入力 `ecu.connected` に `false` を書くと、その ECU は動いたまま、すべてのバスから外れます。`true` で戻ります。相手が落ちたことを、ほかの ECU が正しく検出するかを試せます。

[examples/ecu_main/scenarios/engine_disconnect.yaml](../../examples/ecu_main/scenarios/engine_disconnect.yaml) の 6〜15 行です。

```yaml
vehicle_sil_scenario: 1
name: engine disconnect
steps:
  - at_ms: 300
    set: { Engine.ecu.connected: false }
  - expect: { Dashboard.dashboard.engine_lost: true }
    within_ms: 400
  - set: { Engine.ecu.connected: true }
  - expect: { Dashboard.dashboard.engine_lost: false }
    within_ms: 200
```

```sh
vehicle_sil_launch run simulation.yaml --scenario scenarios/engine_disconnect.yaml
```

- 300 ms に Engine をバスから外し、Dashboard の出力 `dashboard.engine_lost` が 400 ms のうちに true になることを確かめます。続けて Engine を戻し、200 ms のうちに false に戻ることを確かめます。
- 検出は Dashboard の ECU のコードです（[examples/ecu_main/dashboard/main.cpp](../../examples/ecu_main/dashboard/main.cpp) の 30〜32 行。回転数が 300 ms 来なければ true。回転数は 100 ms ごとに来る）。シナリオは、その出力を読むだけです。`within_ms` は、検出に許す遅れに合わせて決めます。
- 外れた ECU も、ステップ、タイマー、変数は動き続けます。外れている間も変数を読み書きでき、戻すのも変数への書き込みです（契約は [internals/execution.md](../internals/execution.md) の「ECU の入口」の「バスからの切り離し」）。
- 外れている間と戻ったときに J1939 でどう見えるか（送ったものの扱い、戻ったときの Address Claim）は、[j1939-guide.md](../j1939-guide.md) の「ECU をバスから外す（`ecu.connected`）」です。

## CI で回す

```sh
vehicle_sil_launch run simulation.yaml --scenario scenarios/engine_disconnect.yaml --json
```

- 合否は終了コードで判定できます（0 が成功、1 が失敗）。
- `--json` は、結果を 1 つの JSON で標準出力に書きます。シナリオの結果は最上位の `scenario` です。上の失敗する例を `--json` で回したときの `scenario` の部分を下に写します。`status` は `passed`、`failed`、`incomplete`、`not_run` のどれかです。キーの一覧は [scenario.md](../scenario.md) の「出力」です。

<!-- vehicle_sil_launch run simulation.yaml --scenario throttle_response.yaml --json: the scenario -->
```json
  "scenario": {
    "file": "throttle_response.yaml",
    "name": "throttle response",
    "status": "failed",
    "over_at_ms": 205,
    "steps": [
      {
        "index": 1,
        "line": 4,
        "kind": "set",
        "text": "set Engine.engine.throttle = 50",
        "status": "passed",
        "start_ms": 0,
        "at_ms": 0,
        "end_ms": 0,
        "code": null,
        "message": null
      },
      {
        "index": 2,
        "line": 6,
        "kind": "expect",
        "text": "expect Engine.engine.speed ge 1700 (within 200 ms)",
        "status": "failed",
        "start_ms": 0,
        "at_ms": 200,
        "end_ms": null,
        "code": "scenario-failed",
        "message": "not true at any time within 200 ms (from 0 ms to 200 ms): at the end Engine.engine.speed ge 1700, but it was 1050 (its sample of 200 ms)"
      },
      {
        "index": 3,
        "line": 8,
        "kind": "expect",
        "text": "expect Dashboard.dashboard.engine_speed ge 1700 (within 200 ms)",
        "status": "not_run",
        "start_ms": null,
        "at_ms": null,
        "end_ms": null,
        "code": null,
        "message": null
      }
    ]
  }
```

- CI の道具は、`steps[]` の `status` が `failed` の手順の `line`、`code`、`message` を読めば、どこで何が違ったかを人向けの出力と同じだけ知れます。
- 経過を 1 行ずつ読む道具は、`--json` の代わりに `--events` を付けます。手順が始まったときと決まったときに、事象 `scenario_step` が出ます。
- シナリオの判定は仮想時刻だけで決まり、実時間の待ちはありません。ステップの中で止まった ECU がいると、いつまでも終わらないので、CI では `--timeout-ms` で実時間の上限を付けます。
- ECU ごとの出力を残すときは `--log-dir` を付けます。警告も失敗にするときは `--strict` です。
- 版の違うランチャーを使うときは、`vehicle_sil_launch version --json` の `features` に `scenario` があるかを確かめます（手順 `wave` は `scenario-wave`、`ramp` と `wave` の横の `expect` は `scenario-motion-expect`）。

## 次に読むもの

- シナリオの形式、時刻、値の見え方、書き込み、結果、所見の `code`: [scenario.md](../scenario.md)
- ランチャーの引数と終了コード: [simulation-config.md](../simulation-config.md) の「引数と終了コード」。`--json`: [simulation-run.md](../simulation-run.md)。`--events`: [simulation-events.md](../simulation-events.md)
- 探りながら値を書く・読む、時間を止めて進める: [信号を外から操作する](operate-signals.md)
- ECU を書く人に頼むこと（変数の宣言、`sample_period`）: [control-api.md](../control-api.md) の「ECU の変数」
- バスから外したときの J1939 の振る舞い: [j1939-guide.md](../j1939-guide.md) の「ECU をバスから外す（`ecu.connected`）」
