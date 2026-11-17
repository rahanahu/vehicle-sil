# シナリオでテストを書く

[English](../../howto/scenario-tests.md)

ECU の変数に値を設定し、結果を確認するテストを、YAML のファイル（シナリオ）に書いて実行する手順です。`vehicle_sil_launch` がシミュレーションを起動してシナリオを最後まで進め、終了コードで合否を返すので、そのまま CI で使えます。

判定に使うのは仮想時間だけです。同じ設定ファイルと同じシナリオなら、PC の速度によらず、どの手順がどの時刻にどの値で成功したかは毎回同じです。

シナリオの `steps` に書く 1 項目も「手順」と呼びます。この文書の見出しの手順は、「手順 1」のように番号を付けて書きます。「ステップ」は、ECU が仮想時間を 1 回進める単位のことです。

## 前提

| 必要なもの | 条件 |
|---|---|
| ビルド | README の「[リポジトリからビルドする](../../../README.ja.md#リポジトリからビルドする)」のとおりに、`linux-gcc-control` でビルドしてあること。 |
| 例の ECU | [examples/ecu_main](../../../examples/ecu_main) の Engine と Dashboard を使います。ビルドすると、設定ファイルと一緒に `build/linux-gcc-control/examples/ecu_main/` に置かれます。 |

リポジトリのルートで次の 2 行を実行します。これ以降のコマンドは、すべてこのディレクトリ（`simulation.yaml` のある所）で実行します。

```sh
export PATH="$PWD/build/linux-gcc-control/tools/launch:$PATH"   # vehicle_sil_launch のある所
cd build/linux-gcc-control/examples/ecu_main
```

ほかの環境では、次の所が変わります。

| 環境 | 変わる所 |
|---|---|
| ほかのプリセットでビルドした | 上の 2 行の `linux-gcc-control` を、そのプリセットの名前にします。シナリオは Control API のないビルドでも動きます。 |
| Windows | MSYS2 UCRT64 のシェルで、同じコマンドを実行します。ビルドの手順は [build.md](../build.md) にあります。 |
| テンプレートのプロジェクト | `simulation.yaml` のあるディレクトリで実行します。ツールの `bin` を、[最初の ECU を作る](first-ecu.md#手順-1-テンプレートをビルドして動かす) の手順 1 のとおりに `PATH` に追加します。`scenarios/` にあるのは `throttle_step.yaml` だけなので、`engine_disconnect.yaml` はこの文書のブロックから作ります。 |

## 全体の流れ

| 手順 | すること | できるもの |
|---|---|---|
| 1 | 付属のシナリオを実行する | 6 つの手順が成功したことを示す表 |
| 2 | 使える変数を調べる | シナリオに書く変数の名前 |
| 3 | 自分のシナリオを書いて、照合して実行する | `my_tests/throttle_up.yaml` |
| 4 | 失敗するシナリオを実行し、指摘を読んで直す | `my_tests/response_time.yaml` |
| 5 | 入力を動かしながら、その間ずっと確認する | `my_tests/throttle_motion.yaml` |
| 6 | パラメーターと機種を使う | `my_tests/dashboard_speed.yaml`、`simulation.variants.yaml` |
| 7 | ディレクトリのシナリオをまとめて実行する | 機種とシナリオの組ごとの結果の表 |
| 8 | CI で実行する | 終了コードと JSON |

手順 1〜8 は順に進めます。手順 3〜6 で作るファイルの完成形は、[examples/scenario_tests](../../../examples/scenario_tests) にあります。その後の「ECU をバスから切断する」と「注意点」は、必要になったときに読みます。

## 手順 1: 付属のシナリオを実行する

```sh
vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml
```

### 確認

標準出力の最後が `passed:` で、終了コード（`echo $?`）が 0 なら成功です。最後の行は、設定ファイルの照合とシナリオの両方の指摘を数えます（シナリオが失敗すると `failed:` になります）。

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

traffic: J1939_CAN: 1 participant sent, 1 received (2 counted) (steps before 705 ms)

network    participant  sent  received
J1939_CAN  Engine          8         0
J1939_CAN  Dashboard       0         8

passed: 0 errors, 0 warnings, 0 notes
```

| 出力の部分 | 読み方 |
|---|---|
| 最初の 2 つの表 | 設定ファイルと各 ECU を照合した結果です。 |
| `run:` の行 | シナリオが終わった 705 ms で、シミュレーションを止めています。 |
| `line` の列 | シナリオのファイルの行番号です。 |
| `at` の列 | その手順の結果が決まった仮想時間です。 |

標準エラーには、ECU の出力と、手順の結果が決まるたびの行（`[vehicle_sil_launch] scenario: line 10: ...: ok at 330 ms`）が表示されます。

### シナリオの内容

`scenarios/throttle_step.yaml` の、コメントを除いた内容です。スロットルを 50 % にして、回転数が上がるのを待ち、表示の ECU がそれを受信したことを確認してから、スロットルを戻します。`expect` の `within_ms: 200` は 200 ms 以内に条件が 1 度でも成り立つこと、`hold_for_ms: 100` は 100 ms の間ずっと成り立つことを求めます（詳しくは手順 3）。

<!-- examples/ecu_main/scenarios/throttle_step.yaml -->
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

手順は上から順に進みます。1 つの手順には、すること（`set`、`wait_until`、`expect`、`ramp`、`wave` のどれか）を 1 つと、時刻を書きます。

| 時刻の書き方 | 意味 |
|---|---|
| `at_ms: 0` | 仮想時間の 0 ms に実行します。 |
| `after_ms: 50` | 前の手順が終わってから 50 ms 後に実行します。 |
| どちらも書かない | 前の手順が終わった時刻に実行します。 |

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| `vehicle_sil_launch` が見つからない | 新しいシェルで、`PATH` への追加を実行していません。 | 「前提」の 2 行を、リポジトリのルートで実行します。 |
| 末尾に `[executable]` の付いたエラーが表示される | ECU の実行ファイルがありません。 | README の手順でビルドします。 |
| `simulation.yaml` を読めない（終了コード 3） | 実行するディレクトリが違います。 | 「前提」の `cd` を実行します。 |

## 手順 2: 使える変数を調べる

シナリオが読み書きするのは、ECU が宣言した変数です。名前と型は、`check --json` の出力の `participants[].variables` にあります。

```sh
vehicle_sil_launch check simulation.yaml --json
```

### 確認

Engine の `variables` に、次の項目があります。

<!-- scenario guide: check simulation.yaml --json, one variable -->
```json
        {
          "name": "engine.throttle",
          "type": "real",
          "causality": "input",
          "unit": "%",
          "min": 0,
          "max": 100,
          "step": null,
          "description": "accelerator pedal position",
          "items": [],
          "builtin": false
        },
```

シナリオには `<participant の名前>.<変数の名前>` と書きます。この変数は `Engine.engine.throttle` です。例の 2 つの ECU の変数は、次のとおりです。

| シナリオに書く名前 | 型 | `causality` | 内容 |
|---|---|---|---|
| `Engine.engine.throttle` | real（0〜100 %） | input | スロットル。回転数は、800 + 20 × スロットル rpm に近づいていきます。 |
| `Engine.engine.speed` | integer（rpm） | output | エンジンの回転数です。 |
| `Dashboard.dashboard.engine_speed` | integer（rpm） | output | 表示の ECU が J1939 で受信した回転数です。 |
| `Dashboard.dashboard.engine_lost` | bool | output | 回転数を 300 ms 受信していなければ true です。 |
| `Engine.ecu.connected`、`Dashboard.ecu.connected` | bool | input | どの ECU にもある変数で、false を書くとバスから切断します。 |

| `causality` | シナリオでできること |
|---|---|
| input | `set`、`ramp`、`wave` で書き込めて、`expect` と `wait_until` で読めます。 |
| output | 読むだけです。 |
| parameter | 時刻 0 の `set` でだけ書き込めます。 |

## 手順 3: シナリオを書く

スロットルを 50 % にすると回転数が 1800 rpm になることを確認するシナリオを書きます。`my_tests` ディレクトリを作り、次の内容で `my_tests/throttle_up.yaml` を作ります。

```sh
mkdir my_tests
```

<!-- examples/scenario_tests/my_tests/throttle_up.yaml -->
```yaml
vehicle_sil_scenario: 1
name: throttle up
steps:
  - set: { Engine.engine.throttle: 50 }
  - wait_until: { Engine.engine.speed: { ge: 1700 } }
    timeout_ms: 500
  - expect: { Engine.engine.speed: { near: 1800, tolerance: 50 } }
    hold_for_ms: 100
```

| 書くもの | すること |
|---|---|
| `set` | 変数に値を設定します。 |
| `wait_until` と `timeout_ms` | 条件が成り立つまで待ちます。`timeout_ms` の間に成り立たなければ失敗です。 |
| `expect` だけ | その時刻に条件が成り立つことを確認します。 |
| `expect` と `within_ms` | その時間のうちに 1 度でも成り立つことを確認します。成り立った時点で次の手順へ進みます。 |
| `expect` と `hold_for_ms` | その時間ずっと成り立つことを確認します。 |

条件には、値そのもの（等しい）か、`ge`、`le`、`gt`、`lt`、`eq`、`ne`、`near` と `tolerance` を書きます。キーと条件の全部は、[scenario.md](../scenario.md) の「ファイルの形式」にあります。

`wait_until` と、`within_ms` を付けた `expect` は、どちらも条件が成り立つまで待ちます。テストの前提がそろうのを待つときは `wait_until` を、テストしたい応答には `expect` を使います。失敗したときの指摘のコードが `scenario-timeout` と `scenario-failed` に分かれるので、どちらで失敗したかが結果からわかります。待つ時間は、設定ファイルの `stop_at_ms`（`simulation.yaml` では 1000 ms）までにシナリオが終わる長さにします。終わらないと、`scenario-out-of-time` で失敗します。

実行する前に、`check --scenario` で照合します。変数の名前、型、値の範囲、時刻の順序のエラーが、シミュレーションを起動せずにわかります。

```sh
vehicle_sil_launch check simulation.yaml --scenario my_tests/throttle_up.yaml
vehicle_sil_launch run simulation.yaml --scenario my_tests/throttle_up.yaml
```

### 確認

`check` の出力は、2 つの表の後が次の 3 行です。

<!-- scenario guide: check simulation.yaml --scenario my_tests/throttle_up.yaml, the end -->
```text
scenario my_tests/throttle_up.yaml (throttle up): 3 steps

passed: 0 errors, 0 warnings, 0 notes
```

`run` の出力は、`run:` の行から後が次のとおりです。終了コードは 0 です。

<!-- scenario guide: run simulation.yaml --scenario my_tests/throttle_up.yaml, from the line run: -->
```text
run: the simulation ran until the scenario was over (445 ms) and ended

scenario my_tests/throttle_up.yaml (throttle up): passed, 3 of 3 steps passed

line  step                                                              result  at
4     set Engine.engine.throttle = 50                                   ok      0 ms
5     wait_until Engine.engine.speed ge 1700 (timeout 500 ms)           ok      330 ms
7     expect Engine.engine.speed near 1800 (tolerance 50) (for 100 ms)  ok      440 ms

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

traffic: J1939_CAN: 1 participant sent, 1 received (2 counted) (steps before 445 ms)

network    participant  sent  received
J1939_CAN  Engine          6         0
J1939_CAN  Dashboard       0         6

passed: 0 errors, 0 warnings, 0 notes
```

### うまくいかないとき

変数の名前を間違えると、`check` が、行番号と直し方を付けて報告します。名前を書き間違えたコピーで確認します。

```sh
sed 's/engine.throttle/engine.throttel/' my_tests/throttle_up.yaml > mistake.yaml
vehicle_sil_launch check simulation.yaml --scenario mistake.yaml
```

終了コードは 1 で、出力の最後は次のとおりです。`run --scenario` も最初に同じ照合をして、エラーがあれば何も起動しません。

<!-- scenario guide: check simulation.yaml --scenario mistake.yaml, the end -->
```text
error: mistake.yaml:4: 'Engine.engine.throttel': participant 'Engine' has no variable 'engine.throttel': did you mean 'Engine.engine.throttle'? (vehicle_sil_launch check --json lists the variables) [scenario-variable]

failed: 1 error, 0 warnings, 0 notes
```

| 指摘のコード | 原因 | 直し方 |
|---|---|---|
| `scenario-variable` | その participant に、その名前の変数がありません。 | 手順 2 の出力にある名前にします。 |
| `scenario-not-writable` | output の変数に書き込んでいます。 | input の変数に書き込みます。 |
| `scenario-value` | 値が、変数の型か範囲（`min`、`max`）に合いません。 | 範囲の中の値にします。 |
| `scenario-time` | 待つ手順（`wait_until`、`within_ms` を付けた `expect`）より後ろに `at_ms` を書いています。 | 待つ手順がいつ終わるかは実行するまで決まらないので、`after_ms` で書きます。 |

指摘のコードの全部は、[scenario.md](../scenario.md) の「照合（`check --scenario`、`run --scenario` の最初）」にあります。

## 手順 4: 失敗を読んで直す

スロットルを設定してから 200 ms のうちに、回転数が 1700 rpm に達することを確認するシナリオを書きます。次の内容で `my_tests/response_time.yaml` を作ります。

<!-- scenario guide: my_tests/response_time.yaml with within_ms: 200 -->
```yaml
vehicle_sil_scenario: 1
name: response time
steps:
  - set: { Engine.engine.throttle: 50 }
  - expect: { Engine.engine.speed: { ge: 1700 } }
    within_ms: 200
  - expect: { Engine.engine.speed: { le: 1800 } }
    hold_for_ms: 100
```

```sh
vehicle_sil_launch run simulation.yaml --scenario my_tests/response_time.yaml
```

### 確認

このシナリオは失敗します。終了コードは 1 で、`run:` の行から後は次のとおりです。

<!-- scenario guide: run simulation.yaml --scenario my_tests/response_time.yaml with within_ms: 200, from the line run: -->
```text
run: the simulation ran until the scenario was over (205 ms) and ended

scenario my_tests/response_time.yaml (response time): FAILED, 1 of 3 steps passed

line  step                                                result   at
4     set Engine.engine.throttle = 50                     ok       0 ms
5     expect Engine.engine.speed ge 1700 (within 200 ms)  FAILED   200 ms
7     expect Engine.engine.speed le 1800 (for 100 ms)     not run  -

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

traffic: J1939_CAN: 1 participant sent, 1 received (2 counted) (steps before 205 ms)

network    participant  sent  received
J1939_CAN  Engine          3         0
J1939_CAN  Dashboard       0         3

error: my_tests/response_time.yaml:5: expect Engine.engine.speed ge 1700 (within 200 ms): not true at any time within 200 ms (from 0 ms to 200 ms): at the end Engine.engine.speed ge 1700, but it was 1050 (its sample of 200 ms) [scenario-failed]

failed: 1 error, 0 warnings, 0 notes
```

`error:` の行に、失敗の内容が全部あります。

| `error:` の行の部分 | この例での意味 |
|---|---|
| `my_tests/response_time.yaml:5:` | 失敗した手順は、ファイルの 5 行目です。 |
| `not true at any time within 200 ms (from 0 ms to 200 ms)` | 0 ms から 200 ms までの間に、条件が 1 度も成り立ちませんでした。 |
| `at the end Engine.engine.speed ge 1700` | その時間の終わり（200 ms）にも成り立っていなかった条件です。 |
| `but it was 1050 (its sample of 200 ms)` | 200 ms の時点のサンプル（ECU が一定の間隔、既定では 10 ms ごとに送信する変数の値）では、回転数は 1050 rpm でした。 |
| `[scenario-failed]` | 指摘のコードです。`expect` の条件が成り立たなかったことを表します。 |

手順が 1 つ失敗すると、シナリオはそこで終わり、シミュレーションも止まります（`run:` の行の 205 ms）。後ろの手順は実行されず、表では `not run` になります。

この例の ECU は、回転数が 1700 rpm になるまでに 330 ms かかります（手順 3 の表の `at`）。ECU の動作が正しいなら、テストのほうを直します。6 行目の `within_ms: 200` を `within_ms: 400` に書き換えて、もう一度実行します。

```sh
sed -i 's/within_ms: 200/within_ms: 400/' my_tests/response_time.yaml
vehicle_sil_launch run simulation.yaml --scenario my_tests/response_time.yaml
```

今度は成功し、終了コードは 0 です。

<!-- scenario guide: run simulation.yaml --scenario my_tests/response_time.yaml, the result -->
```text
scenario my_tests/response_time.yaml (response time): passed, 3 of 3 steps passed

line  step                                                result  at
4     set Engine.engine.throttle = 50                     ok      0 ms
5     expect Engine.engine.speed ge 1700 (within 400 ms)  ok      330 ms
7     expect Engine.engine.speed le 1800 (for 100 ms)     ok      440 ms
```

### うまくいかないとき

実行して初めてわかる失敗は、次の 4 つです。

| 指摘のコード | 原因 | 直し方 |
|---|---|---|
| `scenario-failed` | `expect` の条件が成り立ちませんでした。 | `error:` の行の値と時刻を見て、ECU かシナリオを直します。 |
| `scenario-timeout` | `wait_until` の条件が、`timeout_ms` の間に成り立ちませんでした。 | 前提がそろわない原因を調べるか、`timeout_ms` を大きくします。 |
| `scenario-out-of-time` | シナリオが終わる前に、設定ファイルの `stop_at_ms` に達しました。 | `stop_at_ms` を大きくするか、消します。シナリオが終わればシミュレーションも止まります。 |
| `scenario-incomplete` | ECU の異常終了、Ctrl+C、`--timeout-ms` で、シナリオの途中で実行が終わりました。 | 一緒に表示されるエラーを先に直します。 |

結果のルールは、[scenario.md](../scenario.md) の「結果」にあります。

## 手順 5: 入力を動かしながら確認する

`ramp` は、決めた時間をかけて値を直線で動かします。`wave` は、正弦波やパルスなどの波形で値を動かします。どちらも、同じ手順に `expect` を並べて書くと、動かしている間のすべてのサンプルで条件が成り立つことを確認します。

次の内容で `my_tests/throttle_motion.yaml` を作ります。

<!-- examples/scenario_tests/my_tests/throttle_motion.yaml -->
```yaml
vehicle_sil_scenario: 1
name: throttle motion
steps:
  - set: { Engine.engine.throttle: 50 }
  - wait_until: { Engine.engine.speed: 1800 }
    timeout_ms: 500
  - ramp: { Engine.engine.throttle: { to: 80, over_ms: 200 } }
    expect: { Engine.engine.speed: { ge: 1800, le: 2400 } }
  - wave: { Engine.engine.throttle: { sine: { amplitude: 10, period_ms: 100 }, for_ms: 200 } }
    expect: { Engine.engine.speed: { le: 0xA28 } }
```

| 行 | すること |
|---|---|
| 7〜8 | スロットルを 200 ms かけて 80 % まで上げ、その間ずっと回転数が 1800〜2400 rpm にあることを確認します。 |
| 9〜10 | スロットルを 80 % の上下 10 % の正弦波で 200 ms 動かし、その間ずっと回転数が 2600 rpm 以下であることを確認します。 |
| 10 の `0xA28` | 2600 の 16 進の表記です。integer と enum の値は、16 進でも書けます。 |

```sh
vehicle_sil_launch run simulation.yaml --scenario my_tests/throttle_motion.yaml
```

### 確認

終了コードは 0 で、出力に次の表があります。`ramp` と `wave` の行の `expecting` の後が、動かしている間に確認した条件です。

<!-- scenario guide: run simulation.yaml --scenario my_tests/throttle_motion.yaml, the result -->
```text
scenario my_tests/throttle_motion.yaml (throttle motion): passed, 4 of 4 steps passed

line  step                                                                                                           result  at
4     set Engine.engine.throttle = 50                                                                                ok      0 ms
5     wait_until Engine.engine.speed eq 1800 (timeout 500 ms)                                                        ok      350 ms
7     ramp Engine.engine.throttle to 80 over 200 ms, expecting Engine.engine.speed ge 1800, le 2400                  ok      560 ms
9     wave Engine.engine.throttle sine amplitude 10 period 100 ms for 200 ms, expecting Engine.engine.speed le 2600  ok      760 ms
```

`ramp` と `wave` は、動かし終わるまで次の手順に進みません。動かした後の結果を確認するときは、次の手順に `expect` を書きます。その例が、手順 1 の `throttle_step.yaml` の最後の 2 つの手順と、リポジトリの `scenarios/throttle_wave.yaml`（テンプレートにはありません）です。

記録した値を入力に流すときは、CSV ファイルにして `points` から読みます。1 行目が列の名前、最初の列が時刻（ms）の CSV を、シナリオと同じディレクトリかその下に置き、ファイルと値の列（`value`）を書きます。

```yaml
  - wave: { Engine.engine.throttle: { points: { file: throttle_trace.csv, value: throttle } } }
```

その例が、リポジトリの [examples/ecu_main/scenarios/throttle_trace.yaml](../../../examples/ecu_main/scenarios/throttle_trace.yaml) です（テンプレートにはありません）。決まりは、[scenario.md](../scenario.md#点を-csv-ファイルから読む) の「点を CSV ファイルから読む」にあります。

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| `not true during the ramp (from ... to ...): at 430 ms ...` で失敗する | 動かしている間のサンプルの 1 つで、条件が成り立ちませんでした。 | 表示された時刻と値を見て、ECU か条件を直します。 |
| `ramp` の横の `expect` に `within_ms` か `hold_for_ms` を書いて、照合のエラーになる | 横の `expect` が確認する区間は、動かしている間に決まっています。 | ほかの区間で確認するときは、`expect` を次の手順にします。 |
| `wave` に、照合の警告 `scenario-wave-undersampled` が出る | 波形が、ECU のステップより細かく変化します。 | 周期を長くします。意図したものなら、その手順に `undersampled: ok` を書きます。 |
| `0x1F` を引用符で囲んで、照合のエラーになる | `"0x1F"` は文字列です。 | 引用符を外します。 |

リファレンスは次のとおりです。

| 知りたいこと | 読む所 |
|---|---|
| 波形の種類（`sine`、`square`、`pulse`、`noise` など）と書き方 | [scenario.md](../scenario.md) の「波形で動かす（`wave`）」 |
| 横の `expect` が見るサンプルの範囲 | [scenario.md](../scenario.md) の「動かしている間ずっと確認する」 |
| 16 進で書ける位置 | [scenario.md](../scenario.md) の「ファイルの形式」 |

## 手順 6: パラメーターと機種を使う

シナリオの中の値をパラメーターにすると、同じシナリオを、値を替えて実行できます。機種（`variants`）のある設定ファイルでは、シナリオを実行する機種を `only` で限定できます。

まず、機種のある設定ファイルを作ります。`simulation.yaml` をコピーして、末尾に機種を 2 つ追加します。`full` は 2 つの ECU をそのまま使い、`engine_only` は Dashboard を無効にします。

```sh
cp simulation.yaml simulation.variants.yaml
cat >> simulation.variants.yaml <<'EOF'
variants:
  full: {}
  engine_only:
    participants:
      Dashboard: { enabled: false }
EOF
```

追加した 5 行です。

<!-- examples/scenario_tests/simulation.variants.yaml: the lines added -->
```yaml
variants:
  full: {}
  engine_only:
    participants:
      Dashboard: { enabled: false }
```

次の内容で `my_tests/dashboard_speed.yaml` を作ります。

<!-- examples/scenario_tests/my_tests/dashboard_speed.yaml -->
```yaml
vehicle_sil_scenario: 1
name: dashboard speed
only: [full]
params: { throttle: 50, speed: 1800 }
steps:
  - set: { Engine.engine.throttle: "${throttle}" }
  - expect: { Dashboard.dashboard.engine_speed: { near: "${speed}", tolerance: 50 } }
    within_ms: 600
```

| 行 | 意味 |
|---|---|
| `only: [full]` | このシナリオは、機種 `full` でだけ実行します。Dashboard のない `engine_only` では実行できないためです。 |
| `params: { ... }` | パラメーターと、その既定の値です。 |
| `"${throttle}"` | その位置に、パラメーター `throttle` の値を書いたのと同じになります。`{ }` の中では引用符で囲みます。 |

機種のある設定ファイルでは、`--variant` で機種を選びます。1 つ目は既定の値で、2 つ目は `--param` で値を替えて実行します。

```sh
vehicle_sil_launch run simulation.variants.yaml --variant full --scenario my_tests/dashboard_speed.yaml
vehicle_sil_launch run simulation.variants.yaml --variant full --scenario my_tests/dashboard_speed.yaml --param throttle=30 --param speed=1400
```

### 確認

どちらも終了コードは 0 です。2 つ目の出力には、次の行と表があります。`parameters:` の行で、それぞれの値がどこから来たかがわかります。

<!-- scenario guide: run --variant full --scenario my_tests/dashboard_speed.yaml --param throttle=30 --param speed=1400, the parameters -->
```text
parameters: throttle = 30 (--param), speed = 1400 (--param)
```

<!-- scenario guide: run --variant full --scenario my_tests/dashboard_speed.yaml --param throttle=30 --param speed=1400, the result -->
```text
line  step                                                                              result  at
6     set Engine.engine.throttle = 30                                                   ok      0 ms
7     expect Dashboard.dashboard.engine_speed near 1400 (tolerance 50) (within 600 ms)  ok      310 ms
```

機種 `engine_only` を選ぶと、何も起動せずに終了コード 1 で終わり、次のエラーが表示されます。

```sh
vehicle_sil_launch run simulation.variants.yaml --variant engine_only --scenario my_tests/dashboard_speed.yaml
```

<!-- scenario guide: run --variant engine_only --scenario my_tests/dashboard_speed.yaml, the error -->
```text
error: my_tests/dashboard_speed.yaml:3: the scenario is only for the variant full (its only), not for engine_only: run it with --variant full, or add engine_only to only [scenario-variant]
```

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| `the file defines variants ...: give --variant <name>`（終了コード 4） | 機種のある設定ファイルに、`--variant` を付けていません。 | 表示された機種の名前の 1 つを `--variant` に書きます。 |
| `--param x=1: the scenario has no parameter x`（終了コード 4） | シナリオにない名前のパラメーターを渡しています。 | 名前を直します。 |
| `only` のあるシナリオを `simulation.yaml` で実行して、エラーになる | 機種のない設定ファイルには、`only` のあるシナリオを使えません。 | 機種のある設定ファイルで実行します。 |

パラメーターの値は、設定ファイルの `params` と、機種ごとの `params` にも書けます。機種によって実行ファイルや変数の名前が違うときの書き方も含めて、リファレンスは次のとおりです。

| 知りたいこと | 読む所 |
|---|---|
| `${name}` を書ける位置と、値が決まる順 | [scenario.md](../scenario.md) の「パラメーター（`params`、`${name}`）」 |
| その機種にない変数を使う手順の扱い | [scenario.md](../scenario.md) の「ない変数を使う手順のスキップ」 |
| 設定ファイルの `variants` の書き方 | [simulation-config.md](../simulation-config.md) の「機種（`variants`、`--variant`）」 |

## 手順 7: シナリオをまとめて実行する

`test` は、設定ファイルの機種ごとに、ディレクトリのシナリオを 1 つずつ実行して、全部の結果を 1 つの表にします。

```sh
vehicle_sil_launch test simulation.variants.yaml --scenarios my_tests/
```

### 確認

標準出力は次のとおりで、終了コードは 0 です。

<!-- scenario guide: test simulation.variants.yaml --scenarios my_tests/ -->
```text
variant      scenario                       result
full         my_tests/dashboard_speed.yaml  passed
full         my_tests/response_time.yaml    passed
full         my_tests/throttle_motion.yaml  passed
full         my_tests/throttle_up.yaml      passed
engine_only  my_tests/dashboard_speed.yaml  skipped  the scenario is only for full (its only)
engine_only  my_tests/response_time.yaml    passed   1 warning
engine_only  my_tests/throttle_motion.yaml  passed   1 warning
engine_only  my_tests/throttle_up.yaml      passed   1 warning

passed: 8 runs, 7 passed, 0 failed, 1 skipped
```

| 表の部分 | 読み方 |
|---|---|
| 行の順 | 機種は設定ファイルに書いた順で、シナリオはファイルの名前の順です。 |
| `skipped` | シナリオの `only` にその機種がないので、実行していません。失敗にはなりません。 |
| `1 warning` | 設定ファイルへの警告です。`engine_only` では、`J1939_CAN` に参加する ECU が Engine だけになるためです（`single-participant`）。 |

失敗する組があると、終了コードは 1 になります。パラメーターの `speed` を、達しない値に替えて確認します。

```sh
vehicle_sil_launch test simulation.variants.yaml --scenarios my_tests/ --param speed=3000
```

<!-- scenario guide: test simulation.variants.yaml --scenarios my_tests/ --param speed=3000 -->
```text
variant      scenario                       result
full         my_tests/dashboard_speed.yaml  FAILED   line 7: expect Dashboard.dashboard.engine_speed near 3000 (tolerance 50) (within 600 ms): not true at any time within 600 ms (from 0 ms to 600 ms): at the end Dashboard.dashboard.engine_speed near 3000 (tolerance 50), but it was 1800 (its sample of 600 ms)
full         my_tests/response_time.yaml    passed
full         my_tests/throttle_motion.yaml  passed
full         my_tests/throttle_up.yaml      passed
engine_only  my_tests/dashboard_speed.yaml  skipped  the scenario is only for full (its only)
engine_only  my_tests/response_time.yaml    passed   1 warning
engine_only  my_tests/throttle_motion.yaml  passed   1 warning
engine_only  my_tests/throttle_up.yaml      passed   1 warning

to see a failed run in full, run it alone: vehicle_sil_launch run simulation.variants.yaml --speed max --variant full --scenario my_tests/dashboard_speed.yaml --param speed=3000

FAILED: 8 runs, 6 passed, 1 failed, 1 skipped
```

失敗した組があっても、残りの組は実行されます。`to see a failed run in full` の行のコマンドで、失敗した 1 組だけを、`test` と同じ条件（`--speed max`: 最速）で実行し直せます。ファイルに有効な `role: bridge` の participant がいるときは、`test` は速さ 1 で実行し、コマンドに `--speed max` は付きません（[外につながる実行の時間](../simulation-run.md#外につながる実行の時間--time-policy)）。

| したいこと | 書き方 |
|---|---|
| 1 つの機種だけを実行する | `--variant full` を付けます。 |
| ファイルを選んで実行する | `--scenarios` の後に、ファイルを並べます。 |
| 最初の失敗で止める | `--fail-fast` を付けます。 |
| 機種のない設定ファイルで実行する | `test simulation.yaml --scenarios scenarios/` のように、同じ書き方です。 |

`test` のルールは、[simulation-run.md](../simulation-run.md) の「シナリオをまとめて実行する（`vehicle_sil_launch test`）」にあります。

## 手順 8: CI で実行する

合否は終了コードで判定します。`check`、`run`、`test` で共通です。

| 終了コード | 意味 |
|---|---|
| 0 | 成功しました。 |
| 1 | 手順が失敗したか、照合でエラーが見つかりました。`--strict` を付けたときは、警告でも 1 です。 |
| 3 | 設定ファイルを読めません。 |
| 4 | 引数が正しくありません。 |
| 130 | Ctrl+C などで中断されました。 |

失敗の内容をプログラムで読むときは、`--json` を付けます。結果が、1 つの JSON として標準出力に書かれます。

```sh
vehicle_sil_launch test simulation.variants.yaml --scenarios my_tests/ --param speed=3000 --json > result.json
```

### 確認

終了コードは 1 です。`result.json` の先頭に全体の結果があり、`runs` に、機種とシナリオの組ごとの結果が並びます。次は、そのうち全体の結果と最初の 1 組を抜き出したものです。

<!-- scenario guide: test --json, an excerpt -->
```json
{
  "vehicle_sil_test": 1,
  "file": "simulation.variants.yaml",
  "result": "failed",
  "passed": 6,
  "failed": 1,
  "skipped": 1,
  "runs": [
    {
      "variant": "full",
      "scenario": "my_tests/dashboard_speed.yaml",
      "result": "failed",
      "reason": "line 7: expect Dashboard.dashboard.engine_speed near 3000 (tolerance 50) (within 600 ms): not true at any time within 600 ms (from 0 ms to 600 ms): at the end Dashboard.dashboard.engine_speed near 3000 (tolerance 50), but it was 1800 (its sample of 600 ms)"
    }
  ]
}
```

各組の `run` には、その組を `run --json` で実行したときと同じ JSON が入っています。その中の `scenario.steps[]` に、手順ごとの `line`、`status`、`code`、`message` があります。

CI では、次の引数も使います。

| 引数 | 使う場面 |
|---|---|
| `--timeout-ms <ms>` | 実時間の上限を付けます。ステップの中で止まった ECU がいると仮想時間が進まず、実行が終わらないためです。`test` では 1 組ごとの上限です。 |
| `--strict` | 警告も失敗にします。 |
| `--log-dir <dir>` | `run` で、ECU ごとの出力をファイルに残します。 |
| `--events` | `run` で、経過を 1 行に 1 つの JSON で読みます。手順が始まったときと結果が決まったときに、イベント `scenario_step` が出力されます。 |

| 知りたいこと | 読む所 |
|---|---|
| `run --json` の `scenario` のキー | [scenario.md](../scenario.md) の「出力」 |
| 引数と終了コードの全部 | [simulation-config.md](../simulation-config.md) の「引数と終了コード」 |
| `--events` のイベント | [simulation-events.md](../simulation-events.md) |

## ECU をバスから切断する

どの ECU にもある入力 `ecu.connected` に `false` を書くと、その ECU は動いたまま、すべてのバスから切断されます。`true` を書くと戻ります。通信の相手がいなくなったことを、ほかの ECU が検出するかをテストできます。

`scenarios/engine_disconnect.yaml` の、コメントを除いた内容です。300 ms に Engine を切断し、Dashboard の `dashboard.engine_lost` が 400 ms のうちに true になることを確認します。続けて Engine を戻し、200 ms のうちに false に戻ることを確認します。

<!-- examples/ecu_main/scenarios/engine_disconnect.yaml -->
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

### 確認

終了コードは 0 で、出力に次の表があります。

<!-- scenario guide: run simulation.yaml --scenario scenarios/engine_disconnect.yaml, the result -->
```text
scenario scenarios/engine_disconnect.yaml (engine disconnect): passed, 4 of 4 steps passed

line  step                                                             result  at
9     set Engine.ecu.connected = false                                 ok      300 ms
11    expect Dashboard.dashboard.engine_lost eq true (within 400 ms)   ok      510 ms
13    set Engine.ecu.connected = true                                  ok      520 ms
14    expect Dashboard.dashboard.engine_lost eq false (within 200 ms)  ok      610 ms
```

`line` は、コメントを含むファイルの行番号です。

検出しているのは Dashboard の ECU のコードで、シナリオはその出力を読むだけです。この例の Dashboard は、回転数を 300 ms 受信しなければ `engine_lost` を true にします。`within_ms` は、検出に許容する遅れに合わせて決めます。

| 知りたいこと | 読む所 |
|---|---|
| 切断している間に ECU の中で動き続けるもの | [internals/execution.md](../internals/execution.md) の「バスからの切断（`ecu.connected`）」 |
| 切断したときと戻したときの J1939 の動作 | [j1939-guide.md](../j1939-guide.md) の「ECU をバスから切断する（`ecu.connected`）」 |

## 注意点

シナリオが見る値は、ECU が `sample_period`（既定は仮想時間の 10 ms）ごとに送信するサンプルです。次の表の上の 2 つは、そのために起きます。

| 症状 | 原因 | 直し方 |
|---|---|---|
| `set` と同じ時刻の `expect` が、設定する前の値を見て失敗する | その時刻に始まる ECU のステップが、サンプルを取らないことがあります。照合ではわかりません。 | 値が反映されたことは、`within_ms` を付けた `expect` で確認します。 |
| 短いパルスが、`within_ms` でも `hold_for_ms` でも見えない | サンプルとサンプルの間に変化して戻った値は、条件に現れません。 | ECU の `sample_period` を小さくします（[control-api.md](../control-api.md) の「ECU の変数」の `set_agent_options()`）。 |
| 待つ手順の次の `after_ms` が、表の `at` から数えた時刻とずれる | 待つ手順が終わる時刻は、条件が成り立ったサンプルの時刻（`at`）の少し後です。 | 時刻に余裕を持たせます。決め方は、[scenario.md](../scenario.md) の「時刻」にあります。 |
| `--scenario cannot be used with --start-paused`（終了コード 4） | シナリオは、時刻 0 の値を自分で書き込んで実行します。 | `--start-paused` を外します。 |
| `--control` で接続したクライアントが、シナリオの実行中に値を書き込めない | シナリオの実行中、クライアントができるのは、読むこと、待つこと、止めることだけです。 | 値を変えながら確認するときは、[信号を外から操作する](operate-signals.md) の方法を使います。 |
| ある participant の変数を、シナリオで使えない | 無効にした participant、`describe: false` の participant、実時間で動く participant の変数は使えません。 | [scenario.md](../scenario.md) の「一緒に使えない引数と制限」を確認します。 |

サンプルのルールは、[scenario.md](../scenario.md) の「値の見え方」にあります。

## 補足

バージョンの違う `vehicle_sil_launch` を使うときは、`vehicle_sil_launch version --json` の `features` で、使える機能を確認できます。

| `features` の値 | 使える機能 |
|---|---|
| `scenario` | `--scenario` |
| `scenario-wave` | 手順 `wave` |
| `scenario-motion-expect` | `ramp` と `wave` の横の `expect` |
| `scenario-points-file` | CSV ファイルの `points` |

## 次に読むもの

- シナリオの形式、時刻、結果、指摘のコードを調べる → [scenario.md](../scenario.md)
- 値を変えながら対話的に確認する、時間を止めて進める → [信号を外から操作する](operate-signals.md)
- ほかの人が作った ECU と一緒に動かす → [複数の ECU をつないで動かす](integrate-ecus.md)
