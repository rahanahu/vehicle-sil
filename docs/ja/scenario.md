# シナリオ（`vehicle_sil_launch run --scenario`）

[English](../scenario.md)

シナリオのファイルの形式と、実行したときのルール（時刻、条件が見る値、結果、指摘のコード、出力）のリファレンスです。手順を追って試すときは、ガイド「[シナリオでテストを書く](howto/scenario-tests.md)」から始めます。

| 調べたいこと | 節 |
|---|---|
| キー、値と条件の書き方 | [ファイルの形式](#ファイルの形式) |
| 手順が始まる時刻と終わる時刻 | [時刻](#時刻) |
| 条件が見る値 | [値の見え方](#値の見え方) |
| 書き込みが反映されるステップ | [書き込み](#書き込み) |
| 波形の種類とキー | [波形で動かす（`wave`）](#波形で動かすwave) |
| `ramp` と `wave` の横の `expect` | [動かしている間ずっと確認する](#動かしている間ずっと確認する) |
| パラメーター、機種、手順のスキップ | [機種とパラメーター](#機種とパラメーター) |
| 合否の決まり方、実行したときの指摘のコード | [結果](#結果) |
| 照合の指摘のコード | [照合（`check --scenario`、`run --scenario` の最初）](#照合check---scenariorun---scenario-の最初) |
| 人向けの出力、JSON のキー | [出力](#出力) |
| `--control` と一緒に使う | [実行中に変数を見る（`--control`）](#実行中に変数を見る--control) |
| 使えない引数、できないこと | [一緒に使えない引数と制限](#一緒に使えない引数と制限) |
| ランチャーがどの機能を持つか | [ランチャーの機能の名前（`features`）](#ランチャーの機能の名前features) |

シナリオは、決まった手順で ECU の変数に値を設定し、結果を確認するファイルです。`vehicle_sil_launch run <設定ファイル> --scenario <シナリオ>` は、シミュレーションを起動し、シナリオを最後まで進めて止め、終了コードで合否を返します。そのまま自動テストになります。

同じ設定ファイルと同じシナリオなら、結果（どの手順が、どの仮想時間に、どの値で成功したか、失敗したか）はいつも同じです。PC の速度、最速か等速か（`speed`）、ECU の処理の重さでは変わりません。判定には仮想時間だけを使い、実時間の待ちやタイムアウトは使いません。

シナリオが使う変数は、`ecu_main` で作った ECU が宣言したものと、どの ECU にもある `ecu.connected` です（[ecu-executable.md](ecu-executable.md) の「変数を公開する」）。名前は `vehicle_sil_launch check --json` の `participants[].variables` でわかります。gRPC は使いません（Control Server のないビルドでも動きます）。

このドキュメントでは、シナリオの `steps` に書く 1 項目を「手順」と呼びます。「ステップ」は、ECU や controller が仮想時間を 1 回進める単位のことです。

## 例

[examples/ecu_main/scenarios/throttle_step.yaml](../../examples/ecu_main/scenarios/throttle_step.yaml) は、[examples/ecu_main](../../examples/ecu_main) の Engine と Dashboard のシナリオです。

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

実行したときの標準出力です。participant の行と、手順ごとの行（`[vehicle_sil_launch] scenario: line 10: ...: ok at 330 ms`）は、標準エラーに出力されます。

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

`at` は、その手順の結果が決まった仮想時間です（下の「結果」）。設定ファイルの `stop_at_ms` は 1000 ms ですが、シナリオが終わった 705 ms で止まります。

### ECU をバスから切断する

`<participant>.ecu.connected` に `false` を書くと、その ECU は、動いたまま、すべてのバスから切断されます。送信したフレームはバスに出ず、バスのフレームも受信しません。`true` を書くと戻ります（[internals/execution.md](internals/execution.md) の「`Ecu` クラス」の「バスからの切断」）。通信の相手がいなくなったことを、ほかの ECU が正しく検出するかをテストできます。

[examples/ecu_main/scenarios/engine_disconnect.yaml](../../examples/ecu_main/scenarios/engine_disconnect.yaml) は、300 ms に Engine を切断し、Dashboard が回転数の途絶を検出することを確認します（`dashboard.engine_lost`。回転数を 300 ms 受信しなければ true）。続けて Engine を戻し、回復することを確認します。

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

YAML です（`{` で始まれば JSON）。設定ファイルとは別のファイルにします。形式はまだ変わることがあります。

### 最上位のキー

| キー | 必須 | 意味 |
|---|---|---|
| `vehicle_sil_scenario` | はい | 形式のバージョンで、`1` です。 |
| `name` | いいえ | シナリオの名前です（出力に表示されます）。 |
| `only` | いいえ | このシナリオを実行する機種の名前の並びです（`only: [small, medium]`。書かなければ、どの機種でも実行します。[機種を限定する](#機種を限定するonly)）。 |
| `params` | いいえ | パラメーターの既定の値（名前 → 値）です（手順の値の位置に `${name}` と書いて使います。[パラメーター](#パラメーターparamsname)）。 |
| `inputs` | いいえ | 入力ごとの宣言（変数 → `{ dither: <波形> }`）です。シナリオの始めから、その入力に細かい揺れを重ねます（[細かい揺れを重ねる](#細かい揺れを重ねるdither)）。 |
| `steps` | はい | 手順の並び（1 つ以上）で、上から順に進みます。 |

### 手順のキー

1 つの手順には、動作を 1 つと、時刻を書きます。`ramp` と `wave` にだけは、`expect` を並べて書けます（[動かしている間ずっと確認する](#動かしている間ずっと確認する)）。

| 動作 | 書き方 | すること |
|---|---|---|
| `set` | `set: { <変数>: <値>, ... }` | 値を設定します。 |
| `ramp` | `ramp: { <変数>: { to: <値>, over_ms: <ms>, from: <値> }, ... }` | `over_ms` の間、値を直線で動かします（`from` は省けます）。 |
| `wave` | `wave: { <変数>: { <波形>: { ... }, around: <値>, for_ms: <ms> }, ... }` | 決まった波形（`sine`、`square` など）で値を動かします（[波形で動かす](#波形で動かすwave)）。 |
| `dither` | `dither: { <変数>: { <波形>: { ... } }, ... }`、`dither: { <変数>: none }` | その時刻から、入力に重ねる揺れを替えます。`none` は外します（[細かい揺れを重ねる](#細かい揺れを重ねるdither)）。 |
| `wait_until` | `wait_until: { <変数>: <条件>, ... }` | 条件が成り立つまで待ちます。 |
| `expect` | `expect: { <変数>: <条件>, ... }` | 手順の時刻に、条件が成り立つことを確認します。 |

| 動作と一緒に書くキー | 書ける動作 | 意味 |
|---|---|---|
| `at_ms` | どれでも | 手順の時刻です（仮想時間、ミリ秒、0 以上）。 |
| `after_ms` | どれでも | 手順の時刻を、前の手順が終わった時刻からのミリ秒（0 以上）で書きます。 |
| `timeout_ms: <ms>` | `wait_until`（必須） | この間に条件が成り立たなければ、失敗です。 |
| `within_ms: <ms>` | `expect` | この間に 1 度でも成り立てば成功で、成り立った時点で次の手順へ進みます。 |
| `hold_for_ms: <ms>` | `expect` | この間ずっと成り立つことを確認します。 |
| `expect: { <変数>: <条件>, ... }` | `ramp`、`wave` | 動かしている間ずっと（手順の時刻から終わりまで）、条件が成り立つことを確認します。 |
| `undersampled: ok` | `wave`、`dither` | その手順の波形が ECU のステップより細かくても、照合で指摘しません。 |

`at_ms` も `after_ms` もない手順は、`after_ms: 0` と同じです。

### 変数と値

変数は `<participant>.<変数名>` と書きます。最初の `.` までが participant の名前で、残りが ECU の中の変数名です。変数名に `.` があってもかまいません（participant `Engine` の `engine.speed` は `Engine.engine.speed`）。

値は、変数の型に合わせて書きます。

| 変数の型 | 値の書き方 |
|---|---|
| bool | `true` か `false` |
| integer | 整数（16 進でも書けます） |
| real | 数（10 進だけ） |
| enum | 項目の名前か値（値は 16 進でも書けます） |

16 進（`0x1F`、`0X1F`）のルールは次のとおりです。

| 項目 | ルール |
|---|---|
| 書ける位置 | integer と enum の、`set` の値、`ramp` の `from`・`to`、`wave` の値、条件の値と、その位置に入るパラメーター（`params`、`--param`）です。 |
| 書けない位置 | real の値、時間、`amplitude`、`tolerance` は 10 進だけです。 |
| 引用符 | 囲みません（`"0x1F"` は文字列で、数を引用符で囲んだときと同じエラーです）。 |
| 符号 | 付けません（負の値は 10 進で書きます）。 |
| 出力 | 手順の文と結果は 10 進で表示します（`0x1F` を `set` した手順は `= 31`）。 |
| 範囲の外のエラー | 16 進と 10 進の両方を表示します（`is 0x65 (101), outside its range`）。 |

書き込める変数は、宣言の種類で決まります。

| 変数の種類 | 書き込み | 読み出し（条件） |
|---|---|---|
| 入力 | `set`、`ramp`、`wave` で書き込めます。 | できます。 |
| Parameter | 時刻 0 の `set` でだけ書き込めます（最初のステップの前に反映されます）。 | できます。 |
| 出力 | 書き込めません。 | できます。 |

`set`、`ramp`、`wave` の値は、変数の範囲（`min`、`max`）の中にします。`ramp` を使えるのは integer と real だけです（integer は四捨五入します）。`wave` は、波形ごとに使える型が決まっています。

### 条件

条件は比較だけです。値をそのまま書けば「等しい」で、マッピング（`{ ge: 1700 }` の形）で次の演算子を書けます。

| 演算子 | 成り立つとき | 使える型 |
|---|---|---|
| `eq` | 等しい | どの型でも |
| `ne` | 等しくない | どの型でも |
| `ge` | 以上 | integer、real |
| `le` | 以下 | integer、real |
| `gt` | より大きい | integer、real |
| `lt` | より小さい | integer、real |
| `near` と `tolerance` | 差が `tolerance` 以下 | integer、real |

- 1 つの変数に複数の演算子を書くと、全部が成り立つことが条件です（`{ ge: 1700, le: 1900 }`）。
- 1 つの手順に複数の変数を書くと、全部の変数の条件が成り立つことが条件です。

### スキーマ

JSON Schema は [schema/scenario.schema.json](../../schema/scenario.schema.json) です。YAML の言語サーバーを使うエディターでは、ファイルの先頭に `# yaml-language-server: $schema=<このファイルへのパス>` と書くと、キーの補完と説明、形の検査が使えます。[例のファイル](../../examples/ecu_main/scenarios/throttle_step.yaml)の 1 行目がその例で、パスは、書いているファイルのディレクトリからの相対パスです。

| 項目 | 内容 |
|---|---|
| スキーマが検査するもの | キーと値の形だけです。 |
| スキーマが検査しないもの | 変数があるか、値が型と範囲に合うか、時刻の順序は、`check --scenario` が確認します（下の「照合」）。 |
| JSON で書いたシナリオ | スキーマに結び付ける方法は、設定ファイルと同じです（[simulation-config.md](simulation-config.md) の「エディターの補完」）。 |
| テンプレート | `schema/` にコピーが入っていて、`get_tools.py` が、ダウンロードしたツールのバージョンのスキーマで上書きします。 |
| ツールのアーカイブ | `schema/` にあります。 |

## 時刻

手順の時刻 T は、`at_ms` か、前の手順が終わった時刻 + `after_ms` です。最初の手順の前の「終わった時刻」は 0 です。

| 手順 | 終わる時刻 | 実行の前にわかるか |
|---|---|---|
| `set`、`dither`、`expect`（その時点） | T | わかります。 |
| `ramp` | T + 最も長い `over_ms` | わかります。 |
| `wave` | T + 最も長い波形の長さ | わかります。 |
| `expect` と `hold_for_ms` | T + `hold_for_ms` | わかります。 |
| `wait_until`、`expect` と `within_ms`（待つ手順） | 成り立ったサンプルの時刻 t の、次の controller のステップ P の、さらに 1 タイムステップ後（P + タイムステップ） | 実行するまで決まりません。 |

波形の長さは、`for_ms` です。`pulse` は `width_ms`、`points` は最後の点の時刻、`bounces` を書いた `chatter` は 2 × `bounces` × `interval_ms` です。

終わる時刻が実行の前にわかる手順は、前の手順の結果によらず、書いた時刻どおりに進みます。

待つ手順が P + タイムステップで終わるのは、次の理由です。ランチャーは、時刻 t のサンプルを、その次の controller のステップ P で確実に持っています。次の手順の書き込みが間に合う最初の時刻が P + タイムステップなので、そこを終わりとします。例では、回転数が 1700 になったサンプルが 330 ms、P が 335 ms、終わりが 340 ms です。

そのため、待つ手順より後ろには `at_ms` を書けません（`after_ms` で書きます）。時刻が戻る `at_ms` もエラーです。

controller のタイムステップは、設定ファイルの `time_step_ms` です。書いていなければ、10 ms、`stop_at_ms`、ECU のステップ幅の最大公約数で、どの ECU のステップも controller のステップの境目で始まります（結果の `timing.time_step_ms`。速度を指定したときの既定と同じです。[simulation-run.md](simulation-run.md#タイムステップと-ecu-のステップ) の「タイムステップと ECU のステップ」）。シナリオのある実行では、速度によらずこのタイムステップです。

## 値の見え方

ECU は、Agent の `sample_period`（既定は仮想時間の 10 ms）ごとに、ステップの終わりの全変数の値を 1 組（サンプル）送信します。サンプルの時刻は、そのステップが始まった時刻です。条件が見るのは、このサンプルです。

| 条件 | 見るサンプル |
|---|---|
| その時点の `expect`（時刻 T） | 変数ごとに、その ECU の、T 以前に始まったステップの最後のサンプルです（「時刻 T の値」）。 |
| `wait_until`、`within_ms` | 時刻 T の値と、その後に受信するサンプルの全部です（1 サンプルだけ成り立つ変化も見逃しません）。 |
| `hold_for_ms` | 同じサンプルの全部で、1 つでも成り立たなければ、そのサンプルの時刻で失敗です。 |
| `ramp`、`wave` の横の `expect` | [動かしている間ずっと確認する](#動かしている間ずっと確認する)にあります。 |

- `sample_period` より細かい変化は見えません。サンプルとサンプルの間に変わって戻った値は、条件に現れません。
- サンプルが 1 つもない変数は、どの条件も成り立ちません（指摘の文は `it had no value yet (no sample of its ECU had arrived)`）。
- 時刻 0 の `expect` は、サンプルがないことでは失敗しません。0 ms に始まるステップのサンプルを見ます（`its sample of 0 ms`）。このサンプルには、時刻 0 の書き込みがもう反映されています。
- サンプルがなくて失敗するのは、時刻 0 に、シナリオがまだ書いていない変数を、`from` のない `ramp` か `around` のない `wave` で動かすときです（`scenario-write`）。時刻 0 の書き込みは最初のステップの前に渡すので、始まりの値にするサンプルがまだありません（[書き込み](#書き込み)）。

### `set` と同じ時刻の `expect`

時刻 T の `set` の値が T のサンプルに入っているかは、T に始まるステップがサンプルを取るかで決まります。サンプルを取るのは、`sample_period` の倍数の時刻以後の、最初のステップです。既定では、10 ms の倍数の時刻に始まるステップです。

| T に始まるステップ | `set` の直後の、同じ時刻の `expect` |
|---|---|
| サンプルを取る（例: `at_ms: 100`） | `set` の後の値を見ます。 |
| サンプルを取らない（例: ステップが 5 ms の ECU の 105 ms のステップ） | 前のサンプル（`set` の前の値）を見て、失敗します。 |

値が反映されたことを確認するには、`within_ms` を付けた `expect` を使います（次のサンプルで成り立ちます）。ランチャーは ECU の `sample_period` を知らないので、この失敗は照合ではわかりません。

### 対応表（`variables`）のある participant

設定ファイルの participant に `variables`（対応表）があれば、シナリオに書く変数の名前と値は、対応の後のものです（[simulation-config.md](simulation-config.md#変数の名前と値を合わせるvariables)）。実行ファイルごとの名前や単位の違いは設定ファイルが吸収するので、シナリオは 1 つで済みます。一次式で変換した変数は実数になり、書いた値と読んだ値が丸めの分だけ違うことがあるので、`near` と `tolerance` で比べます。

## 書き込み

`set` は、時刻 T を付けた書き込みです。各 ECU の、T 以降に始まる最初のステップの先頭で反映され、そのステップのアプリケーションが新しい値を見ます。T が ECU のステップの間なら、次のステップで反映されます。

| 項目 | ルール |
|---|---|
| 1 つの `set` に、同じ participant の変数が複数ある | 同じステップでまとめて反映されます。 |
| `ramp` が書く時刻 | 変数ごとに、T、T + ステップ、T + 2 ステップ、…（その ECU のステップ）に、`from` から `to` への直線の上の値を書き、T + `over_ms` に `to` を書きます。 |
| `ramp` の始まりの値（`from` を省いたとき） | シナリオがその変数に最後に書いた値です。 |
| シナリオがまだ書いていない変数の始まりの値 | 最新のサンプルの値です。どのサンプルになるか（1 つもないか）は、controller のタイムステップによります（時刻 T の書き込みは、T より前のタイムステップで送るため。[simulation-run.md](simulation-run.md) の「記録の終わり（`recorded_to_ms`）」）。タイムステップによらない結果にするには、`from` を書きます。 |
| 書いた値もサンプルもない | 失敗です（`from` を書きます）。 |
| シナリオが書いた後に、ほかの書き手（ECU 自身など）が値を変えた | 始まりの値は変わりません。 |
| 書き込みの結果を確認する時点 | 反映する ECU のステップの、次の controller のステップです。 |
| 反映されなかった書き込み（ECU の setter が拒否した、相手が接続していない、など） | その手順の失敗です。 |
| いくつもの変数を書く 1 つの `set` で、ECU の setter がそのうちの 1 つを拒否した | その手順の失敗です。**その `set` の中で、拒否された変数より前の変数は、もう書き込まれていて、そのまま残ります。** 拒否された変数とその後ろは書き込まれていません。メッセージがその変数を挙げ、時間の軸のファイルは書き込みごとに印を付けます（[simulation-run.md](simulation-run.md#ファイルの形) の「ファイルの形」）。 |
| 動きのない間の `dither` の書き込み | 手順のものでない書き込みが、ECU のステップごとに 1 回出ます（[細かい揺れを重ねる](#細かい揺れを重ねるdither)）。同じ時刻・同じ participant の分は、1 つの書き込みにまとめます。断られると、その時点でまだ決まっていない最初の手順の失敗（`scenario-write`）です。1 つの `set` と同じで、その participant の入力のうち、断られたものより前の入力は、もう書き込まれています。 |

書き込みが、書いた時刻のステップに間に合わないことはありません。ランチャーは、時間ゲートで controller の各ステップを止め、次のタイムステップまでの書き込みを ECU に渡し終えてから進めます。時刻 0 の分は、全員が最初のステップの前で待っている間に渡します。

手順のものでない `dither` の書き込みが断られたときの文は、どの入力の揺れが、いつ、どの値を書いたかを言います。範囲の外だと言うのは、エージェントがそう言って断ったときだけで、ほかの拒否（つながっていない、コマンドが多すぎる、setter の例外など）は、状態と理由を言います。まとめた書き込みの入力の 1 つを ECU の setter が断ったときは、その入力を名指しし、それより前の入力（ECU がもう書き込んだもの）を並べます。まとめた書き込みのどの入力のせいかがわからないとき（コマンドが多すぎる、理由に変数の名前を言わない ECU など）は、participant を主語にして、その書き込みの入力と値を全部並べます。

## 波形で動かす（`wave`）

`wave` は、変数を決まった波形で動かします。次の例は、スロットルを、いまの値を中心に ±5 %、1 秒に 10 回の正弦波で 200 ms 動かします（[examples/ecu_main/scenarios/throttle_wave.yaml](../../examples/ecu_main/scenarios/throttle_wave.yaml) にある手順です）。

```yaml
  - wave: { Engine.engine.throttle: { sine: { amplitude: 5, period_ms: 100 }, for_ms: 200 } }
```

変数ごとに、波形を 1 つと、その横に長さ（`for_ms`）と中心（`around`）を書きます。1 つの手順に複数の変数を書けます（同じ変数は 1 回）。手順は、いちばん長い波形が終わると終わります。

### 波形の種類

| 波形 | 値の動き | 使える型 |
|---|---|---|
| `sine` | 中心 ± `amplitude` の正弦波で、中心から始まり、1/4 周期で最大になります。 | integer、real |
| `triangle` | 三角波で、山と谷の位置は `sine` と同じです。 | integer、real |
| `sawtooth` | のこぎり波で、周期の始めが最小で、直線で最大まで上がって戻ります。 | integer、real |
| `square` | 矩形波で、周期の始めから `duty` % の間が高い側です。 | integer、real、bool |
| `pulse` | `width_ms` の間だけ `value` になります。終わると、数は始まる時点の値に戻り、bool は `value` の反対になります（始まる時点の値によりません）。 | integer、real、bool |
| `noise` | 乱数で、1 つの値を `hold_ms` の間保ちます。 | integer、real |
| `chirp` | 周波数が `for_ms` の間に `from_hz` から `to_hz` へ変わる正弦波です。 | integer、real |
| `lag` | 一次遅れの形で `to` に近づきます（`tau_ms` で 63 %、その 5 倍で 99 %）。 | integer、real |
| `chatter` | スイッチのチャタリングで、`to`、元の値、`to`、元の値、… と `interval_ms` ごとに変わり、最後は `to` です。 | integer、real、bool |
| `points` | 時刻と値の表のとおりに動きます。 | どの型でも |

### 波形のキー

| 波形 | キー | 必須 | 意味 |
|---|---|---|---|
| `sine`、`triangle`、`sawtooth` | `amplitude` | はい | 中心からの振れ幅です。 |
| | `period_ms` か `frequency_hz` | どちらか | 周期です（下の「周期」）。 |
| | `phase_deg` | いいえ | 波形を始める位置で、90 なら 1/4 周期の所から始めます。 |
| `sawtooth` | `falling: true` | いいえ | 向きを逆にします。 |
| `square` | `low` と `high` | `amplitude` とどちらか | 低い側と高い側の値です。 |
| | `amplitude` | `low`・`high` とどちらか | 中心 ± `amplitude` の 2 つの値にします。 |
| | `period_ms` か `frequency_hz` | どちらか | 周期です。 |
| | `duty` | いいえ | 高い側の割合（%）で、既定は 50 です。 |
| | `phase_deg` | いいえ | 波形を始める位置です。 |
| `pulse` | `value` | はい（bool は省けます） | パルスの間の値です（bool で省くと `true`）。 |
| | `width_ms` | はい | パルスの長さです。 |
| `noise` | `amplitude` | はい | 乱数の幅です（`distribution` の行）。 |
| | `hold_ms` | はい | 1 つの値を保つ時間です。 |
| | `seed` | はい | 乱数の種です（下の「値の決まり方」）。 |
| | `distribution` | いいえ | `uniform`（既定）は中心 ± `amplitude` の一様分布で、`gaussian` は標準偏差が `amplitude` の正規分布です（標準偏差の 6 倍を超えません）。 |
| `chirp` | `amplitude` | はい | 中心からの振れ幅です。 |
| | `from_hz`、`to_hz` | はい | 始まりと終わりの周波数です（0 以上）。 |
| | `sweep` | いいえ | `linear`（既定）は毎秒同じ Hz ずつ、`log` は毎秒同じ倍率ずつ変わります。 |
| `lag` | `to` | はい | 近づく先の値です。 |
| | `tau_ms` | はい | 時定数です。 |
| | `from` | いいえ | 始まりの値です（省くと、いまの値から）。 |
| | `settle: true` | いいえ | 終わった後の値を `to` にします。 |
| `chatter` | `to` | はい | 最後に落ち着く値です。 |
| | `interval_ms` | はい | 値が変わる間隔です。 |
| | `bounces` | 横の `for_ms` とどちらか | 元の値に戻る回数です。 |
| | `seed` | いいえ | 書くと、変わる時刻が `interval_ms` の 1/4 まで前後にずれます。 |
| | `from` | いいえ | 元の値です（bool には書きません。`to` の反対です）。 |
| `points` | `[[<ms>, <値>], ...]` か CSV ファイル（`{ file: <パス>, value: <列> }`。[点を CSV ファイルから読む](#点を-csv-ファイルから読む)） | はい | 時刻（手順の始めからの ms、昇順）と値の表です。 |

bool の `square` には、`low`・`high` も `amplitude` も書きません（`false` と `true` です）。

`chirp` の周波数のルールは、`sweep` で違います。

| `sweep` | `from_hz` と `to_hz` |
|---|---|
| `linear` | 0 Hz から始めることも、0 Hz で終わることもできます（両方 0 はエラー）。 |
| `log` | どちらも 0 より大きい数です。 |

波形の横（波形と同じ階層）に書くキーは、次の 3 つです。

| キー | 意味 | 書ける波形 |
|---|---|---|
| `for_ms` | 長さです（周期の倍数でなくてもかまいません）。 | `pulse` と `points` には書きません（自分で長さを持ちます）。`chatter` は `bounces` か `for_ms` のどちらかです。 |
| `around` | 中心です。 | 中心のある波形（`sine`、`triangle`、`sawtooth`、`amplitude` の `square`、`noise`、`chirp`）だけです。 |
| `interpolate` | `points` の点の間のつなぎ方です。 | `points` だけです。 |

- **周期**は `period_ms`（1 以上の整数）か `frequency_hz`（0 より大きい数）のどちらかです。1 ms より短い周期は `frequency_hz` で書きます。
- **中心**（`around`）を省くと、手順が始まる時点のその変数の値になります。決め方は `ramp` の `from` と同じです（[書き込み](#書き込み)）。値がなければ失敗するので、`around` を書きます。
- **`pulse`** には `around` を書けません。終わると、数は始まる時点の値に戻り、bool は `value` の反対になります（別の値にするには、次の手順で `set` します）。数の `pulse` は、始まる時点の値がなければ失敗するので、前の手順で `set` しておきます。
- **始まりの値**は、`lag` と `chatter` は `from`、`points` は最初の点で書きます。
- `${name}` は、波形の中の数の位置にも書けます（`amplitude: "${amp}"`）。

`points` の `interpolate` は、次のとおりです。

| `interpolate` | 点の間 | 使える型 |
|---|---|---|
| `linear`（数の既定） | 直線でつなぎます。 | integer、real |
| `hold` | 次の点まで値を保ちます。 | どの型でも（bool と enum は `hold` だけ） |

- `interpolate` は、`points` の中ではなく横に書きます（`{ points: [...], interpolate: hold }`）。点の与え方が増えても、書く場所はここです。
- 値を不連続に変える（段差）ときは `hold` で書きます（`linear` では、同じ時刻の 2 点はエラーです）。
- 最初の点が 0 より後なら、そこまでは、いまの値から最初の点への直線です（`hold` なら、いまの値のまま）。

### 終わった後の値

| 波形 | 終わった後の値 |
|---|---|
| 中心のある波形 | 中心（`around`、省いたときは始まる時点の値）に戻します。 |
| `low`・`high` を書いた `square`、bool の `square` | 低い側で終わります。 |
| `pulse` | 数は始まる時点の値に戻ります。bool は `value` の反対です。 |
| `points` | 最後の点の値のままです。 |
| `chatter` | `to` のままです。 |
| `lag` | `for_ms` の時点で達していた値のままです（`settle: true` なら `to`）。 |

続けて `around` のない `wave` や `from` のない `ramp` を書くと、この値から始まります。

### 値の決まり方

値は、手順が始まった仮想時間からの経過と、書いた数だけで決まります。ECU のステップ幅、controller のタイムステップ、実行の速度によりません。

`noise` と `chatter` の乱数も、`seed`、変数の名前（`<participant>.<変数>`）、時刻だけから作ります。同じシナリオは、どの OS でも毎回同じ値になり、失敗した実行をそのまま再現できます。変数の名前が入るので、2 つの変数に同じ `seed` を書いても、同じ列にはなりません（別々の入力に、そろった乱れは入りません）。

書く時刻は `ramp` と同じです。変数ごとに、T、T + ステップ、T + 2 ステップ、…（その ECU のステップ）にその時刻の値を書き、終わりの時刻に、終わった後の値を書きます。

integer は四捨五入します（ちょうど .5 は 0 から遠い側。計算の最後の桁だけ .5 からずれた値も、.5 として丸めます）。それでも、丸めの境界のごく近くの値は、OS やコンパイラーによって 1 違うことがあります（`sine`、`chirp`、`lag` は C ライブラリの関数を使い、最後の桁が同じとは限りません）。integer の入力に掛けた波形の結果を確認するときは、`ge` と `le` で幅を持たせて書きます。

範囲（`min`、`max`）は、次の時点で確認します。

| 値 | 確認する時点 |
|---|---|
| `low`、`high`、`value`、`to`、`from`、`around`、`points` の値、`around` を書いたときの `around` ± `amplitude` | 照合です。 |
| `around` を省いた波形、`gaussian` の `noise` | 実行中です（実行しないと値がわからないため）。範囲の外の値を書こうとした時点で、その手順が失敗します（`scenario-write`。値を黙って丸めません）。 |

`square` は、値そのものを高い側と低い側に切り替えます（ECU がピンのエッジを見る入力）。ECU がデューティーや周波数を数の変数として受ける PWM の入力には、その変数に `set` や `ramp`、`sine` を使います。

`vehicle_sil::signal`（[`signal/waveform.hpp`](../../libs/signal/include/vehicle_sil/signal/waveform.hpp)）に同じ計算の関数があり、自分の C++ のコードからも使えます。

### 点を CSV ファイルから読む

`points` には、点の表の代わりに CSV ファイルとその列を書けます。記録した値の列を、そのまま入力に流すときに使います。
次の例は、[examples/ecu_main/scenarios/throttle_trace.yaml](../../examples/ecu_main/scenarios/throttle_trace.yaml) の 15 行で、同じディレクトリの `throttle_trace.csv` の列 `throttle` を、最初の列の時刻（ms）で流します。

```yaml
  - wave: { Engine.engine.throttle: { points: { file: throttle_trace.csv, value: throttle } } }
```

| キー | 必須 | 意味 |
|---|---|---|
| `file` | はい | CSV ファイルです。シナリオのファイルがあるディレクトリからのパスで、ディレクトリの区切りは `/` です。絶対パス、`..` を含むパス、`\` を含むパスはエラーです（シナリオは、自分のディレクトリとその下のファイルだけを読みます）。 |
| `value` | はい | 値の列の名前です。 |
| `time` | いいえ | 時刻の列の名前です。省くと最初の列です。 |
| `time_unit` | いいえ | 時刻の単位で、`ms`（既定）か `s` です。どちらも手順の始めからの時刻で、整数の ms になる値を書きます（`s` の `0.25` は 250 ms）。 |

- CSV の 1 行目は列の名前で、その下の 1 行が 1 点です。区切りは `,` で、セルを `"` で囲めます（中の `"` は `""`）。UTF-8 で、先頭の BOM、CRLF の改行、空行はかまいません。どの行も、1 行目と同じ数のセルを持ちます。
- 読んだ点は、`[[<ms>, <値>], ...]` に書いたときと同じ決まりで照合し、同じ値を書きます（時刻は昇順、値は型と範囲、`interpolate` は横に書く、ECU のステップより細かい点の警告）。値のセルは数です。bool は `true`、`false`、`1`、`0`、enum は項目の名前か値です。integer の値には 16 進（`0x3C`）も書けます。
- 1 つのファイルの別の列を、別の変数に使えます。ファイルは照合のときに 1 回だけ読みます。値の行は 100000 行まで、ファイルは 16 MiB までです。
- `file`、`value`、`time`、`time_unit` には `${名前}` を書けます（同じシナリオに、別の記録を流す）。
- ファイル、パス、列のエラーは `scenario-points-file` です。セルのエラーは、`[[<ms>, <値>], ...]` と同じ `value`（時刻）と `scenario-value`（値）です。指摘の行はシナリオの `file` の行で、メッセージが CSV の何行目のどの列かを言います（セルは 20 文字で切ります）。セルのエラーは、1 つの `points` につき最初の 1 つだけを言います。

### ECU のステップより細かい波形

ECU は、ステップごとに 1 回だけ入力を受け取ります。それより細かい波形は、書いたとおりには ECU に見えません。照合（`check --scenario`、`run --scenario` の最初）は、相手の ECU のステップ幅と比べて、ECU に実際に見える形を警告で示します。警告なので、実行はできます。`--strict` では照合で失敗します。

| 指摘のコード | 表示されるとき |
|---|---|
| `scenario-wave-undersampled`（警告） | 周期がステップ幅の 2 倍より短い（`sine`、`triangle`、`sawtooth`、`square`。`chirp` は高い側の周波数）。 |
| | `sine`、`triangle` が、ECU が値を受け取るどの時刻にも同じ値になる（周期がステップ幅の 2 倍で、中心から始まるとき。ECU には一定の値に見える）。 |
| | `pulse` の `width_ms`、`noise` の `hold_ms`、`linear` の `points` の点の間隔、`chatter` の `interval_ms`（`seed` があればその半分）がステップ幅より短い。 |
| | `hold` の `points` に、ECU に反映されない点がある（点の間隔は見ない）。 |
| `scenario-wave-duty`（警告） | `square` の高い側の長さ（周期 × `duty`）がステップ幅の倍数でない（ECU に見えるデューティーを示す）。 |
| | 周期がステップ幅の倍数でない。 |
| `scenario-wave-coarse`（注記） | `sine`、`triangle`、`sawtooth`、`chirp` の 1 周期が 8 ステップより少ない（山と谷を取りこぼすことがある）。 |
| `scenario-wave-not-checked`（注記） | 相手の participant のステップ幅がわからないので、比べていない。 |

ステップ幅 10 ms の ECU に、周期 30 ms、デューティー 25 % の矩形波を書いたときの例です。

```text
warning: pwm.yaml:5: the square of Door.lamp is high for 7.5 ms of each period of 30 ms (duty 25 %), but participant 'Door' takes its inputs once per step of 10 ms: the ECU sees it high for 10 ms of each period (33.3 %). Make the high time (period x duty / 100) a multiple of 10 ms; to run it as it is without this message, add undersampled: ok to the step [scenario-wave-duty]
```

意図したとおりで、そのまま実行するときは、その手順に `undersampled: ok` を書きます。その手順には、上の 4 つの指摘が表示されなくなります。

```yaml
  - wave: { Door.lamp: { pulse: { width_ms: 3 } } }
    undersampled: ok
```

波形ごとの補足です。

| 波形 | 補足 |
|---|---|
| なめらかな波形（`sine`、`triangle`、`sawtooth`） | 警告は、形が見える周期（ステップ幅の 8 倍以上）を勧めます。 |
| ステップ幅より短い `pulse` | パルスの間（始まりの時刻を含み、終わりの時刻を含まない）に ECU のステップの時刻が 1 つあれば 1 ステップの間だけ見え、なければ見えません。 |
| `hold` の `points` | 段差を書くためのものなので、点の間隔が短くても警告しません。 |

ECU に見える値の並びは、手順がいつ始まるかによりません（書き込みは T、T + ステップ、… で、ECU は 1 つずつ別のステップで受け取ります）。

短い `pulse` が見えないのは、始まりと終わりの書き込みが同じステップで反映され、後の方が残るためです。ステップ幅 10 ms の ECU なら、107 ms に始まる 5 ms のパルスは 110 ms のステップで見え、101 ms に始まるものは見えません。手順の時刻が実行の前にわかるとき（`at_ms`、長さの決まった手順の後）は、どちらになるかを警告が示します。わからないとき（`wait_until` の後など）は、わからないことを示します。

`hold` の `points` で警告するのは、値が ECU に 1 度も反映されない点があるときで、次の 2 つです。

- 手順の始めからステップ幅ごとの書き込みが、その点の区間に 1 つもない（点をステップ幅の倍数の時刻に置けば起きません）。
- 最後の点の直前の点の書き込みが、終わりの書き込みと同じステップで反映される（短い `pulse` と同じルールです）。

動かしようのないものは、警告ではなくエラーです。周期や幅が 0 以下、`noise` に `seed` がない、`points` の時刻が昇順でない、型に合わない波形（bool に `sine` など）、範囲の外が、これに当たります。

## 細かい揺れを重ねる（`dither`）

`dither` は、入力に、中心が 0 で終わりのない波形を重ねます。
ECU に書く値は、**手順が入力に置いた値（基準）に、その時刻の揺れを足して、変数の型に丸めたもの**です。
基準は、`set` の値か、`ramp` や `wave` のその時刻の値です。揺れは、基準を動かしても乗ったままです。

```yaml
vehicle_sil_scenario: 1
inputs:
  Engine.engine.throttle:
    dither: { triangle: { amplitude: 2, period_ms: 40 } }   # シナリオの終わりまで乗る
steps:
  - set: { Engine.engine.throttle: 50 }                     # 48〜52 で揺れる
  - ramp: { Engine.engine.throttle: { to: 80, over_ms: 1000 } }   # 基準が動く。揺れは続く
  - after_ms: 500
    dither: { Engine.engine.throttle: none }                # 外す。基準（80）を 1 回書く
  - after_ms: 500
    dither: { Engine.engine.throttle: { noise: { amplitude: 1, hold_ms: 10, seed: 7 } } }
```

書く場所は 2 つです（動かせる例は [examples/ecu_main/scenarios/throttle_dither.yaml](../../examples/ecu_main/scenarios/throttle_dither.yaml)）。

- **最上位の `inputs`**: `inputs: { <変数>: { dither: <波形> } }`。シナリオの始めから乗っています。
- **手順 `dither`**: `dither: { <変数>: <波形> }` はその時刻から揺れを替え、`dither: { <変数>: none }` は外します。手順は時間を取りません（終わりは T）。外す語は `none` です（`off` は、YAML の道具によっては `false` に読まれるので使いません）。

波形は、`wave` と同じ書き方の `sine`、`triangle`、`sawtooth`、`amplitude` の `square`、`noise` です。キーも `wave` の同じ波形と同じで、数の位置に `${name}` を書けます。`for_ms`（終わりがない）と `around`（中心は基準）は書けません。長さのある波形（`pulse`、`lag`、`chatter`、`points`、`chirp`）と、`low`・`high` の `square` はエラーです。使える変数は integer と real の入力だけです。

- **基準**: 揺れが乗るのは、手順がその入力に値を置いてからです。宣言しただけの入力には、最初の `set`、`ramp`、`wave` まで何も書きません。基準のない入力への手順 `dither` も、書かずに通ります（結果の時刻は T）。
- **揺れは仮想時刻そのものの関数**です。手順 `dither` を置いた時刻からではなく、仮想時刻 0 から数えます（周期 40 ms の `triangle` は、いつ置いても 10、50、90 ms、… が山）。ずらすには `phase_deg` を書きます。`noise` は、`wave` と同じく `seed`、変数の名前、時刻から作ります。
- **足してから丸める**: integer の入力では、丸める前の基準に足します。10 から 11 への `ramp` の途中（10.4）に +0.2 の揺れが乗ると、10.6 で 11 を書きます。`from` や `around` を省いた次の動きが始まる値は、揺れを含まない基準（型に丸めた値）です。
- **書く時刻**:
  - `set`、`ramp`、`wave` の書き込みは、今までどおりの時刻に、揺れを足した値を書きます。動きの間（T から終わりまで）に書くのは、動きの書き込みだけです。
  - 動きのない間は、その ECU のステップの時刻（0、ステップ、2 × ステップ、…）ごとに 1 回、基準 + 揺れを書きます。その時刻に手順の書き込みがあれば、それが揺れを運ぶので、足しません。
  - 手順 `dither` は、基準のある入力に、T に 1 回書きます（外すときは基準、替えるときは基準 + 新しい揺れ）。同じ時刻のほかの手順の書き込みも、その時刻に最後に置いた揺れで書きます。
  - ステップの間の時刻の `set` と、ステップの間で終わる動きの終わりの値は、次のステップで、そのステップの時刻の揺れの書き込みより先に入ります（そのステップでは、揺れの書き込みが残ります）。
- **揺れのない入力は変わりません**。書く回数も値も、ほかの入力に揺れがあるかどうかによりません。書くたびに 1 回働く入力（要求の入力など）に `dither` を重ねると、ステップごとに書くことになります。
- **範囲**: 基準 + 揺れが変数の範囲を出ると、ECU がその書き込みを断り、シナリオは失敗します（値を黙って丸めません）。照合は、文面の基準 ± 振幅が範囲を出うるときに警告します（`scenario-dither-range`。下の「照合」）。
- **機種**: 手順 `dither` の変数がその機種にないときは、ほかの手順と同じく、手順の全体をスキップします。`inputs` に宣言した入力がないときは、その宣言だけを使わず、注記（`scenario-input-skipped`）で言います。
- 揺れが ECU のステップより細かいときの警告（`scenario-wave-*`）は、`wave` と同じです。黙らせるには、`inputs` のその入力か、手順 `dither` に `undersampled: ok` を書きます。

## 動かしている間ずっと確認する

`ramp` と `wave` の横に `expect` を書くと、動かしている間ずっと、条件が成り立つことを確認します。

```yaml
  - ramp: { Engine.engine.throttle: { to: 80, over_ms: 1000 } }
    expect: { Engine.engine.speed: { le: 3000 } }
```

見るのは、条件の変数ごとに、時刻が T から U までのサンプルの全部です。

| 記号 | 意味 |
|---|---|
| T | 手順の時刻です。 |
| E | 手順の終わりの時刻です。 |
| U | 終わりの値（`to`、波形の終わった後の値）を書いた変数の ECU が、それを反映するステップの時刻です。 |

U は、E が ECU のステップの上なら E、間なら次のステップです（変数が複数の ECU にあれば、いちばん遅いもの）。T より前のサンプル（動かす前の値）と U より後のサンプルは見ません。こうすることで、T と E が ECU のステップの上か間かで、合否が変わりません。ほかの ECU の変数も、同じ T から U までのサンプルを見ます。

| 項目 | ルール |
|---|---|
| 成功する条件 | 書き込みが全部反映され、かつ条件が U まで成り立ったときです。 |
| 結果の `at` と、次の手順が数える終わり | E のままです（`expect` を書いても、手順の時刻と終わりは変わりません）。 |
| 結果が決まる順 | 書き込みか条件のどちらかが残っている間は、後ろの手順の結果が先に決まっても、この手順の結果は決まりません。 |
| 区間に 1 つもサンプルのない変数 | 確認しなかったことになり、U で失敗です（`no sample of <変数> in that time`）。 |
| `within_ms`、`hold_for_ms` | 書けません（エラー）。 |
| 条件が成り立たなかった | そのサンプルの時刻で失敗です（`scenario-failed`）。 |
| 書き込みが反映されなかった | `scenario-write` です。 |
| 両方が起きた | `code` は、ランチャーが先に知ったほうです（[結果](#結果)）。 |

- サンプルのない変数の扱いは、`hold_for_ms` の `expect` と違います。T より前のサンプルでは判定しません（[値の見え方](#値の見え方)）。
- ほかの区間で確認するときは、`expect` を別の手順にして、`ramp` や `wave` の後に置きます。
- 失敗の文は `hold_for_ms` と同じ形で、区間と、見たサンプルを示します: `not true during the ramp (from 1000 ms to 2000 ms): at 1340 ms Engine.engine.speed le 3000, but it was 3050 (its sample of 1340 ms)`。U が E と違うときは、区間に `, where its end (2000 ms) takes effect` が付きます。

### `expect` の変数がないとき

設定ファイルで `expect` の変数がない（`absent: true`、機種が無効にした participant）ときは、`expect` だけをスキップし、`ramp` や `wave` は書いたとおりに動かします。ある変数の条件も一緒に見ません（手順ごとスキップするときと同じ考え方です）。動かす変数がないときは、手順の全体をスキップします（[ない変数を使う手順のスキップ](#ない変数を使う手順のスキップ)）。

| 表示される所 | 内容 |
|---|---|
| 手順の文 | `ramp ... over 100 ms, expect not checked: <条件>` |
| 照合の注記 `scenario-step-skipped` | `the ramp runs; its expect is not checked (...)` の形です。 |
| 数 | 照合と実行の合否の行（`1 expect not checked`）、JSON の `unchecked_expects`、`vehicle_sil_launch test` の表（`1 expect was not checked`）です。 |
| 理由 | 人向けの出力は表の下の `line <行> expect not checked: <理由>`、JSON は手順の `expect_skipped` です。 |

## 機種とパラメーター

設定ファイルに機種（`variants`）があるとき、同じシナリオをどの機種にも使えるように、次の 3 つがあります。

| 仕組み | すること |
|---|---|
| [パラメーター](#パラメーターparamsname) | 手順の値を、機種や引数で替えます。 |
| [手順のスキップ](#ない変数を使う手順のスキップ) | その機種にない変数を使う手順を、実行しません。 |
| [`only`](#機種を限定するonly) | シナリオを実行する機種を限定します。 |

機種の違いは設定ファイルの側に書き、シナリオには、機種の名前で分かれる手順を書きません（設定ファイルの側の書き方は [simulation-config.md](simulation-config.md) の「機種」と「シナリオのパラメーター」）。

```yaml
vehicle_sil_scenario: 1
name: idle
params: { idle_rpm: 800, settle_ms: 500 }      # 既定の値
steps:
  - set: { Engine.engine.throttle: 0 }
  - after_ms: ${settle_ms}
    expect: { Engine.engine.speed: { near: "${idle_rpm}", tolerance: 50 } }
  - expect: { Engine.engine.turbo: { le: 0.1 } }    # turbo のない機種では飛ばす
```

### パラメーター（`params`、`${name}`）

`${name}` は、手順の中の値の位置に、値の全体として書きます。その位置に、パラメーターの値を書いたのと同じに読みます。型と範囲の照合も同じで、エラーの文には、どのパラメーターのどこから来た値かが表示されます。

| 項目 | ルール |
|---|---|
| 書ける位置 | `set` の値、条件の値と `tolerance`、`ramp` の `to`・`from`・`over_ms`、`wave` の数と長さ、`at_ms`、`after_ms`、`timeout_ms`、`within_ms`、`hold_for_ms` です。 |
| 書けない位置 | キー、変数名、値の一部（`"${a} rpm"`）です（エラー `scenario-param`）。 |
| 式 | ありません（`${a+1}` はエラー `scenario-param`）。計算した値が必要なときは、その値を別のパラメーターにします。 |
| `{ ... }` や `[ ... ]` の中 | 引用符で囲みます（`{ near: "${idle_rpm}" }`）。YAML が、引用符のない `${` をそこでは読めないためです。 |
| JSON | 文字列で書きます。 |
| 引用符で囲んだときの型 | 値はパラメーターに書いたとおりに読みます（数は数です）。 |
| `params` の値 | 1 つの値（数、`true`・`false`、文字列）で、ほかのパラメーターは使えません。 |
| 名前 | 英数字と `_`（数字で始めない）です。 |

値は、次の順で、後のものが前のものを置き換えます。シナリオの `params` に書いていない名前も、ほかから値が来れば使えます。

| 順 | 値を書く所 |
|---|---|
| 1 | シナリオの `params` |
| 2 | 設定ファイルの `params` |
| 3 | 選んだ機種の `params` |
| 4 | 引数の `--param <名前>=<値>` |

| エラーになるもの | 結果 |
|---|---|
| どこからも値が来ない `${name}` | 照合のエラーです（実行の前。`scenario-param`）。 |
| シナリオが使っても宣言してもいない名前の `--param` | 引数のエラーです（終了コード 4。シナリオのパラメーターの名前を表示します）。 |

パラメーターにエラーがある間は、手順の中身を照合しません（まずパラメーターを直します）。

### ない変数を使う手順のスキップ

次の変数を使う手順は、実行でスキップします。照合では、手順ごとに注記 `scenario-step-skipped` が表示されます（エラーではありません）。

| 変数 | 扱い |
|---|---|
| 設定ファイルの対応表で `absent: true` とした変数（その実行ファイルにはない、と書いた変数） | その変数を使う手順をスキップします。 |
| 選んだ機種が無効にした participant（機種の `enabled: false`）の変数 | 同じくスキップします（その participant は照合していないので、変数の名前は確認しません）。 |
| ほかの理由で無効にした participant（共通の部分の `enabled: false`、`--disable`）の変数 | 照合のエラー（`scenario-variable`）です。 |
| 機種が無効にして、`--enable` で戻した participant の変数 | ほかの participant と同じに照合します。 |
| 対応も `absent` も書いていない変数が、実行ファイルにない | 照合のエラー（`scenario-variable`）です（黙ってスキップしません）。 |

機種が無効にした participant のときの理由は、`participant 'Door' is not in the variant medium (enabled: false in that variant)` の形です。

スキップするのは、その変数を使う手順だけです。

| 場合 | スキップするもの |
|---|---|
| 1 つの手順に、ある変数とない変数の両方がある | 手順の全体です（ある変数にも書きません）。 |
| `ramp` と `wave` の横の `expect` だけが、ない変数を使う | その `expect` だけです（[`expect` の変数がないとき](#expect-の変数がないとき)）。 |
| スキップした `set` に頼る後ろの手順 | スキップせずに実行します。 |

機種によって成り立たない手順は、ない変数を使う形にするか、シナリオを分けて `only` を書きます。

スキップした手順も、書いてある内容からわかる長さは、使ったことにします。次の手順の `after_ms` は、その終わりから数えます。こうして、スキップする機種としない機種で、後ろの手順の時刻が同じになります。

| スキップした手順 | 使ったことにする長さ |
|---|---|
| `set`、長さのない `expect` | 0 |
| `ramp` | いちばん長い `over_ms`（ない変数の `ramp` も `over_ms` だけは読み、`to` と `from` は読みません） |
| `wave` | いちばん長い波形の長さ（ない変数の `wave` も、`for_ms`、`width_ms`、最後の点の時刻、`bounces` と `interval_ms` だけは読みます） |
| `hold_for_ms` のある `expect` | その長さ |
| 待つ手順（`wait_until`、`within_ms` のある `expect`） | 0（手順の時刻で終わったことにします） |

待つ手順だけは例外で、いつ終わるかは実行しないとわからないので、0 にします。その後ろの `after_ms` の手順は、スキップしない機種より早く始まります。待った後の時刻が必要な手順は、待つ手順と同じ変数を使う形にする（一緒にスキップする）か、シナリオを分けて `only` を書きます。

| 項目 | ルール |
|---|---|
| ファイルの読み方 | 機種で変わりません。`at_ms` が前の手順の終わりより前、というエラーは、手順をスキップする機種でも同じに表示されます。 |
| スキップした待つ手順の後ろの `at_ms` | 書けません（スキップしないときと同じです）。 |
| スキップした手順のうち最後のものの長さ | 実行を延ばしません（シナリオは、スキップしない手順の結果が全部決まった所で終わります）。 |
| 手順の結果 | `status` が `skipped`（人向けの出力も `skipped`）で、理由が付きます。 |
| シナリオの結果 | ほかの手順が全部成功すれば `passed` のままで、スキップした数が表示されます（人向けは `passed, 3 of 4 steps passed, 1 skipped`、JSON は `skipped_steps`）。 |

確認する手順（`expect`、`wait_until`、`ramp` と `wave` の横の `expect`）を全部スキップすると、シナリオは何も確認せずに `passed` になります。このとき、照合で警告 `scenario-nothing-checked` を 1 つ表示します。警告は、確認する手順の行と、次にすることを示します（その機種のシナリオでなければ `only` を書く、その機種のものなら対応表を追加する）。既定では実行は成功し、`--strict` では照合で失敗して何も起動しません。もとから確認する手順のないシナリオ（`set`、`ramp`、`wave` だけ）には表示しません。

### 機種を限定する（`only`）

`only: [small, medium]` と書いたシナリオは、その機種のためのものです。

| 場合 | 結果 |
|---|---|
| ほかの機種（`--variant large`）で `check --scenario` か `run --scenario` に渡す | エラー `scenario-variant` で、何も起動しません。 |
| 全部の機種を実行する `vehicle_sil_launch test` | その組を実行せずに `skipped` にします（[simulation-run.md](simulation-run.md#シナリオをまとめて実行するvehicle_sil_launch-test)「シナリオをまとめて実行する」）。 |
| `only` に、設定ファイルの `variants` にない名前がある | エラーです。 |
| 機種のない設定ファイルに、`only` のあるシナリオを渡す | エラーです。 |

機種が合わないときは、手順の中身を照合しません（その機種には合わないかもしれないため）。

## 結果

シナリオの結果（JSON の `status`）は、次のように決まります。

| 起きたこと | シナリオ | 実行 |
|---|---|---|
| 全部の手順が成功した | `passed` | ランチャーがその controller のステップでシミュレーションを止め、成功します（ほかにエラーがなければ終了コード 0）。 |
| 手順が 1 つ失敗した | `failed` | ランチャーがシミュレーションを止め、失敗します（終了コード 1）。 |
| `stop_at_ms` に先に達した | `failed` | 結果が決まっていない最初の手順が失敗です（`scenario-out-of-time`）。 |
| 停止の要求（Ctrl+C、`stop` のコマンド）、participant の失敗、`--timeout-ms` | `incomplete` | シナリオより優先で、[simulation-run.md](simulation-run.md) のルールのままです。 |

- 全部の手順が成功すればランチャーが止めるので、設定ファイルに `stop_at_ms` がなくても実行できます。
- `stop_at_ms` に先に達したときは、決められる所まで結果を決めます（`stop_at_ms` を大きくするか、消します）。
- `incomplete` のときは、エラー `scenario-incomplete` が、どこまで進んだかを示します。
- 異常終了した ECU への書き込みが先に失敗しても、実行の結果は participant の失敗で、シナリオは `incomplete` です。

### 手順が失敗したとき

手順が 1 つ失敗すると、シナリオはそこで終わりです。始まっていた手順は `unfinished`、その後ろは `not_run` になります。後ろの手順の書き込みのうち、次のタイムステップまでの分は、すでに ECU に渡っていることがあります。

失敗として報告するのは、ランチャーが最初に知った失敗で、仮想時間の順とは限りません。ランチャーは、controller のステップ P ごとに、前の書き込みの結果、P より前のサンプル、次のタイムステップまでの書き込みの順に見ます。すぐに拒否される書き込み（範囲の外など）は、1 タイムステップ先の時刻のものを送信するときにわかります。そのため、それより前の時刻のサンプルで成り立たない条件より先に報告されることがあり、タイムステップを変えると入れ替わります。

`dither` が動きのない間に出す書き込み（手順のものでない書き込み）が断られたときは、その時点で**まだ決まっていない最初の手順**が失敗します（`scenario-write`）。次の 2 つは controller のタイムステップによります。タイムステップによらない結果にするには、範囲を出ない振幅にします（照合の警告 `scenario-dither-range` が、出うる所を言います）。

- **どの手順が失敗するか**: ランチャーが、断られる書き込みを送る時に、どの手順まで決めていたかで決まります。例: 30 ms と 60 ms の `expect` があり、50 ms の書き込みが断られるとき、タイムステップが 10 ms なら 30 ms の `expect` は決まっていて 60 ms の手順が失敗し、タイムステップが 30 ms なら 30 ms の手順が失敗します。失敗の時刻（`at`）と文は同じです。
- **最後の手順が決まるタイムステップの書き込み**: シナリオは、手順が全部決まった時に終わり、その後の書き込みは送りません。上の例で手順が 30 ms の `expect` だけなら、タイムステップが 10 ms では 50 ms の書き込みを送る前に通り、タイムステップが 30 ms では送って失敗します。手順のものでない書き込みの結果を、シナリオの終わりで待つこともしません。

### 手順の `at`

手順ごとの `at`（JSON の `at_ms`）は、その手順の結果が決まった仮想時間です。

| 手順 | `at` |
|---|---|
| `set`、書き込みのあった `dither` | 値を反映した ECU のステップ |
| 書き込みのない `dither` | T |
| `ramp`、`wave` | 終わり |
| その時点の `expect` | T |
| `wait_until`、`within_ms` | 成り立ったサンプルの時刻 |
| `hold_for_ms` | 終わり（失敗なら、成り立たなかったサンプルの時刻） |
| 横に `expect` のある `ramp` と `wave` で、条件が成り立たなかった | 成り立たなかったサンプルの時刻 |
| タイムアウト | タイムアウトした時刻 |

### 失敗する例

失敗の指摘は、シナリオのファイルと行、手順、時刻、そのときの値を示します。次の例は、スロットルを設定してから 200 ms のうちに回転数が 1700 rpm に達することを確認しますが、達するのは 330 ms です（上の例の `wait_until`）。

<!-- throttle_response.yaml -->
```yaml
vehicle_sil_scenario: 1
name: throttle response
steps:
  - at_ms: 0
    set: { Engine.engine.throttle: 50 }
  - expect: { Engine.engine.speed: { ge: 1700 } }
    within_ms: 200
  - expect: { Dashboard.dashboard.engine_speed: { ge: 1700 } }
    within_ms: 200
```

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

終了コードは 1 です。失敗した手順の後ろは実行されません（`not run`）。`wait_until` のタイムアウトは、次の形です。

```text
error: s.yaml:4: wait_until Body.ticks ge 10; Body.level eq 6 (timeout 100 ms): timed out at 100 ms (100 ms after 0 ms): Body.level eq 6, but it was 5 (its sample of 100 ms) [scenario-timeout]
```

### 実行の指摘のコード

どれもエラーです。

| `code` | 意味 |
|---|---|
| `scenario-failed` | `expect`（`ramp` と `wave` の横の `expect` も）が成り立たなかった。または、読む変数の participant がこの実行で接続していない。 |
| `scenario-timeout` | `wait_until` が `timeout_ms` の間に成り立たなかった。 |
| `scenario-write` | `set`、`ramp`、`wave` の書き込みが反映されなかった（`wave` が範囲の外の値を書こうとした、など）。または、`ramp` の始まりの値や `wave` の中心の値がない。 |
| `scenario-out-of-time` | `stop_at_ms` で止まったとき、まだ結果が決まっていなかった。 |
| `scenario-incomplete` | シナリオが終わる前に、ほかの理由でシミュレーションが終わった。 |

## 照合（`check --scenario`、`run --scenario` の最初）

シナリオは、実行の前に全部照合します。`vehicle_sil_launch check <設定ファイル> --scenario <シナリオ>` は照合だけをします。エラーがあれば、`run` は何も起動しません。指摘は、シナリオのファイル名と行を付けて、設定ファイルの指摘と一緒に表示されます（JSON では指摘の `file`）。

エラーのある例です。変数の名前を打ち間違え、出力に書き込み、範囲の外の値を設定し、待つ手順の後ろに `at_ms` を書き、`near` に `tolerance` を付けていません。

<!-- throttle_mistakes.yaml -->
```yaml
vehicle_sil_scenario: 1
name: throttle mistakes
steps:
  - at_ms: 0
    set: { Engine.engine.throttel: 50 }
  - set: { Engine.engine.speed: 1000 }
  - set: { Engine.engine.throttle: 150 }
  - wait_until: { Engine.engine.speed: { ge: 1700 } }
    timeout_ms: 500
  - at_ms: 600
    expect: { Engine.engine.speed: { near: 1800 } }
```

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

照合はエラーを全部挙げます（最初の 1 つで止まりません）。終了コードは 1 です。シナリオのファイルがないとき（`file`）も、終了コードは 1 です（設定ファイルがないときの 3 とは違います）。

| `code` | 種類 | 意味 |
|---|---|---|
| `file`、`encoding`、`syntax`、`not-a-scenario`、`format-version`、`unsupported-yaml` | エラー | ファイルが読めない、シナリオでない、バージョンが違う、アンカーや 2 つ目のドキュメントがある。 |
| `unknown-key`、`duplicate-key`、`missing-key`、`value` | エラー | キーと値の形のエラー（1 つの手順に 2 つの動作、`wait_until` に `timeout_ms` がない、`near` に `tolerance` がない、など）。 |
| `scenario-variable` | エラー | 変数が `<participant>.<変数名>` でない、participant か変数がない、participant を使えない（無効にしている、`describe: false`、実時間で動く、変数を出していない）。 |
| `scenario-not-writable` | エラー | 出力に書いている。Parameter を時刻 0 の `set` 以外で書いている。 |
| `scenario-value` | エラー | 値が型、範囲、enum の項目に合わない。`ramp` や大小の比較を、数でない変数に使っている。`wave` の波形が変数の型に合わない、`around` ± `amplitude` が範囲の外。`dither` を bool か enum の変数に書いている。 |
| `scenario-param` | エラー | `${name}` に値がない。`${...}` がキーや変数名、値の一部にある（シナリオにない名前の `--param` は、引数のエラー `usage`）。 |
| `scenario-variant` | エラー | シナリオの `only` に、選んだ機種がない。`only` の名前が設定ファイルの機種にない。 |
| `scenario-time` | エラー | `at_ms` が前の手順の終わりより前。待つ手順より後ろに `at_ms` がある。手順の時間を合計すると、時刻の上限（1000000000000 ms）を超える。 |
| `scenario-wave-undersampled`、`scenario-wave-duty` | 警告 | `wave` か `dither` が相手の ECU のステップより細かい（[ECU のステップより細かい波形](#ecu-のステップより細かい波形)）。integer の入力に、振幅が 0.5 より小さい `dither` も `scenario-wave-undersampled` です（下）。 |
| `scenario-wave-coarse`、`scenario-wave-not-checked` | 注記 | 同じ節にあります。 |
| `scenario-dither-range` | 警告 | `dither` が入力を範囲の外へ出すかもしれない（文面からの見積もり。下）。 |
| `scenario-dither-unused` | 警告 | `inputs` に `dither` を宣言した入力に、実行する手順が 1 つも値を置かない（`set`、`ramp`、`wave` がない。スキップする手順は数えない）。揺れは 1 度も書かれない。 |
| `scenario-input-skipped` | 注記 | `inputs` に宣言した入力が、`absent: true` の変数か、機種が外した participant の変数なので、その宣言を使わない。 |
| `scenario-points-file` | エラー | `points` の CSV ファイル（[点を CSV ファイルから読む](#点を-csv-ファイルから読む)）のパスが、シナリオのディレクトリの下を指していない。ファイルがない、読めない、空、UTF-8 でない、行のセルの数が列の数と違う、行が多すぎる。`value` や `time` の列がない、名前が 2 つある。 |
| `scenario-nothing-checked` | 警告 | 確認する手順（`expect`、`wait_until`、`ramp` と `wave` の横の `expect`）を全部スキップするので、シナリオが何も確認しない。 |
| `scenario-step-skipped` | 注記 | 手順が `absent: true` の変数か、機種が無効にした participant の変数を使うので、実行でスキップする。`ramp` と `wave` の横の `expect` だけが使うときは、その `expect` だけをスキップする。 |

照合が `dither` をどう見るか:

- `scenario-dither-range` は**文面からの見積もり**で、揺れごとに 1 つ出ます。見るのは、その揺れが乗っている間に手順がその入力に置く値（`set` の値、`ramp` の `from` と `to`、`wave` の `around` ± `amplitude` など、文面に書いた値）と、手順 `dither` が置かれた時に入力が留まっている値です。どの揺れが乗っているかは、手順の並び順でたどります（外した後の値、替える前の値とは組み合わせません。スキップする手順は数えません）。値に振幅を足し引きし、integer は実行と同じ丸めをかけて、範囲を出れば警告します。`gaussian` の `noise` は振幅（標準偏差）の 3 倍で見積もります（文に "about"）。
- 時刻は数えないので、**当たらない警告が残ります**: 揺れの山と、下の `wave` の山が同じ時刻に来ないとき、`gaussian` の値が 3 倍まで出ないとき。その実行は通りますが、`--strict` では照合で落ちます。黙らせるキーはありません。振幅を下げるか、値を端から離すか、その値の前に `dither: { <変数>: none }` で外します。
- 逆に、`around` を省いた `wave` のように実行しないと決まらない値は見ないので、警告なしで実行が失敗することもあります。
- integer の入力に、振幅が 0.5 より小さい `dither` は `scenario-wave-undersampled` です（整数の値の上では、丸めると元の値に戻る）。その揺れの下の値が範囲の端にあって、0.5 にすると範囲を出るときは、「0.5 以上に」とは勧めず、値を端から離すよう言います。

## 出力

`run --scenario` に `--timeline <file>` を付けると、シナリオが書いた値と見た値を、仮想時刻の順に JSON のファイルにも書きます（手順の見張りの区間、失敗の値、値の列。[simulation-run.md](simulation-run.md) の「シナリオの実行を時間の軸で記録する（`--timeline`）」）。

### 人向けの出力

| 部分 | 内容 |
|---|---|
| 照合 | `scenario <ファイル> (<name>): <n> steps` の行。 |
| 照合（パラメーターがあるとき） | `parameters: <名前> = <値> (<どこから>), ...` の行。 |
| 実行 | 合否の 1 行と、手順の表（行、手順、結果、結果が決まった仮想時間）。 |
| 実行（スキップした手順ごと） | `line <行> skipped: <理由>`。 |
| 実行（横の `expect` をスキップした手順ごと） | `line <行> expect not checked: <理由>`。 |
| 実行中（標準エラー） | 手順の結果が決まるたびに 1 行。 |

- 照合の行には、スキップする手順があれば ` (<n> skipped)`、確認しない横の `expect` があれば ` (<n> expect not checked)` が付きます（両方なら `, ` でつなぎます）。
- 合否の行には、確認しない横の `expect` があれば、末尾に `, <n> expect not checked` が付きます。
- スキップした手順は、表の時刻が `-` で、実行中の行に ` at <時刻> ms` が付きません。

`dither` のあるシナリオ（`inputs` があるか、実行する手順 `dither` がある）では、照合の部分に、入力に置くものの一覧が続きます（ほかのシナリオの出力は変わりません。`run` の出力でも、最初の照合の部分に出ます）。載るのは、`dither` を置くか外す入力だけです（揺れの下に何が来るかを見るための一覧で、ほかの入力の書き込みは載りません）。`inputs` の宣言、手順の順で、入力ごとに 1 行です。`from` と `to` は文面の時刻で、待つ手順の後ろは `?`、終わりのない `dither` の `to` は `-` です。宣言した揺れの `from` は `first write`（その入力への最初の書き込みから乗る。時刻は文面で決まらない）です。例は [examples/ecu_main/scenarios/throttle_dither.yaml](../../examples/ecu_main/scenarios/throttle_dither.yaml) の一覧です。

<!-- vehicle_sil_launch check simulation.yaml --scenario scenarios/throttle_dither.yaml: the lines -->
```text
input                   from         to    what                                                line
Engine.engine.throttle  first write  -     dither triangle amplitude 2 period 80 ms            10
Engine.engine.throttle  0 ms         0 ms  set = 50                                            14
Engine.engine.throttle  ?            ?     ramp to 80 over 300 ms (after the wait of line 16)  21
Engine.engine.throttle  ?            -     dither none (after the wait of line 16)             26
```

### `check --json` と `run --json` の `check`

`scenario` があるのは、`--scenario` を付けたときだけです。

| `scenario` のキー | あるとき | 意味 |
|---|---|---|
| `file`、`name` | いつも | シナリオのファイルと名前 |
| `steps` | いつも | 手順の数 |
| `only` | あるときだけ | 機種の名前の配列 |
| `params` | あるときだけ | シナリオが宣言したか使ったパラメーターごとに、`name`、`value`、`from` |
| `skipped_steps` | あるときだけ | スキップする手順の数 |
| `unchecked_expects` | あるときだけ | 確認しない横の `expect` の数 |
| `inputs`、`lines` | `dither` のあるシナリオだけ | 次の表。`run --json` の `scenario` にも同じものがあります |

`params` の `value` は有効な値（`true`・`false`、数、文字列）で、`from` は `scenario`、`configuration`、`variant`、`command_line` のどれかです。

`dither` のあるシナリオのキー:

| キー | 意味 |
|---|---|
| `inputs[]` | `inputs` に宣言した入力ごとに `variable`、`line`（その入力の行）、`dither`（`shape` と `text`。下の `lines[]` の同じキーと同じ書き方）です。その機種で使わない宣言は入りません。 |
| `lines[]` | 文面が入力に置くものの一覧です（上の人向けの表と同じ並び。`dither` を置くか外す入力だけ）。`--timeline` のファイルの `lines[]`（[simulation-run.md](simulation-run.md) の「入力の線」。実行で起きたこと）と同じ名前のキーは、同じ意味、同じ書き方です: `variable`、`kind`（`dither`、`set`、`ramp`、`wave`）、`line`、`step`（手順の `index`。`inputs` の宣言は null）、`from_ms`、`to_ms`、`shape`（`set` と `none` は null）、`text`（変数を含む文）。`base` と `base_source` は、実行しないと決まらないのでありません。照合だけのキーは `time_source` と `after_line` です。 |
| `lines[].time_source` | `from_ms` と `to_ms` がどこから決まるか、文面でわかるかです。`text`: 文面の時刻（`to_ms` が null なのは、終わりのない `dither` だけ。タイムラインと同じ）。`after_wait`: 待つ手順の後ろなので、実行の前にはわからない（両方 null で、`after_line` がその待つ手順の行）。`first_write`: `inputs` に宣言した揺れで、その入力への最初の書き込みから乗る（`from_ms` は null）。 |

### `run --json` の `scenario`

最上位に `scenario` があります（`--scenario` を付けたときだけ）。

| キー | 意味 |
|---|---|
| `file`、`name` | シナリオのファイル（引数のとおり）と名前（なければ null）です。 |
| `status` | `passed`、`failed`、`incomplete`（ほかの理由で途中で終わった）、`not_run`（シミュレーションが始まらなかった）のどれかです。 |
| `over_at_ms` | シナリオが終わった controller のステップです（`passed` と `failed`。ほかは null）。 |
| `skipped_steps` | スキップした手順の数です（1 つ以上あるときだけ、このキーがあります）。 |
| `unchecked_expects` | 確認しなかった、`ramp` と `wave` の横の `expect` の数です（1 つ以上あるときだけ、このキーがあります）。 |
| `inputs[]`、`lines[]` | `dither` のあるシナリオでだけあります。`inputs` に宣言した揺れと、文面が入力に置くものの一覧です（上の `check --json` の表と同じ。実行の前にわかること）。 |
| `steps[]` | 手順ごとの結果です（次の表）。 |

`unchecked_expects` に数えた手順は、動いたので `skipped_steps` には入りません。

| `steps[]` のキー | 意味 |
|---|---|
| `index` | 手順の番号です（1 から）。 |
| `line` | シナリオのファイルの行です。 |
| `kind` | `set`、`ramp`、`wave`、`dither`、`expect`、`wait_until` のどれかです。 |
| `text` | 手順の文です。 |
| `status` | 手順の結果です（次の表）。 |
| `start_ms` | 手順の時刻です。 |
| `at_ms` | 結果が決まった時刻です。 |
| `end_ms` | 終わる時刻です。 |
| `code`、`message` | 失敗したときと、スキップしたときにあります（ほかは null）。 |
| `expect_skipped` | 横の `expect` をスキップして動かした `ramp` と `wave` にだけあり、スキップした理由です。 |

| 手順の `status` | 意味 |
|---|---|
| `passed` | 成功しました。 |
| `failed` | 失敗しました。 |
| `unfinished` | 始まっていましたが、結果が決まる前にシナリオが終わりました。 |
| `not_run` | 始まりませんでした。 |
| `skipped` | スキップしました（[ない変数を使う手順のスキップ](#ない変数を使う手順のスキップ)）。 |

スキップした手順のキーは、次のとおりです。

| キー | スキップした手順での値 |
|---|---|
| `code` | `scenario-skipped` |
| `message` | 理由 |
| `at_ms` | null（実行の前に決まっていて、仮想時間のどこかで決まったのではないため） |
| `start_ms`、`end_ms` | スキップしなかったときの手順の時刻と終わり（`start_ms` に、書いてある内容からわかる長さを加えたもの） |

スキップした手順の `start_ms` と `end_ms` は、シミュレーションがそこまで進んだことを意味しません。

- 時刻はミリ秒の数です（ECU のステップが 1 ms より細かいと、小数になることがあります）。わからない時刻は null です。
- シナリオが終わってランチャーが止めた実行の `simulation` は `ended` です。

上の「結果」の失敗する例（`throttle_response.yaml`）を `run --json` で実行したときの `scenario` です（ほかのキーは [simulation-run.md](simulation-run.md) の「JSON（`run --json`）」）。

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

### `run --events`

手順が始まったとき（`status` は `running`）と、結果が決まったときに、イベント `scenario_step`（`steps[]` と同じキー）が出力されます。結果のイベントにも `scenario` があります。

スキップする手順の `scenario_step`（`status` は `skipped`、`at_ms` は null）は 1 回で、その手順の時刻が決まったときに出力されます。実行の前からわかっているので、仮想時間がそこへ進むより前に出力されることがあります。

## 実行中に変数を見る（`--control`）

`--scenario` と `--control` を一緒に付けると、シナリオの実行中も Control Server が起動します（[control-api.md](control-api.md) の「ランチャーに接続する」。Control Server を持つビルドだけ）。クライアントができるのは、読むこと、待つこと、止めることだけです。

| クライアントの呼び出し | シナリオの実行中 |
|---|---|
| 読む、待つ: `Ping`、`ListPeers`、`GetValues`、`GetSnapshot`、`Watch`、`GetTime`、`WaitPaused`、`GetOperation`、`WaitOperation` | いつもどおりです。 |
| 変数を書く、Peer を消す: `Write`、`ForgetPeer` | 拒否します。 |
| 時刻を動かす: `Pause`、`Resume`、`Step`、`RunUntil`、`SetSpeed` | 拒否します。 |
| 止める: `Stop` | 受け付けます。停止の要求の 1 つで、シナリオは `incomplete` になります（`interrupted_by` は `"control server stop"`）。 |

拒否した呼び出しは、次のとおりです。

| 項目 | 内容 |
|---|---|
| 状態コード | `FAILED_PRECONDITION` |
| 理由 | trailing metadata の `vehicle-sil-reason` が `scenario-running`（Python では `ControlError.reason`） |
| メッセージ | 何を拒否したか、なぜか、何ができるかを示します。 |
| ロール（[control-api.md](control-api.md) の「認証」） | ロールによらず拒否します。 |
| 記録 | Hub にも時間ゲートにも渡らず、イベント `command` にもなりません（監査ログの行は残ります）。 |

クライアントがいても、いなくても、シナリオの結果は同じです。読む呼び出しと購読は、シミュレーションの進行を待たせません。ただし、同じロックを短く取り合うので、読むクライアントが多いと、実時間では少し遅くなることがあります。等速の実行の `timing` はその分だけ変わることがありますが、シナリオの結果は変わりません。購読は、読まないクライアントの分を上限つきで保持し、あふれた分を捨てて数えます（値は Peer ごとに最新の 1 つ）。

クライアントからは、次のように見えます。

| 場面 | クライアントに見えるもの |
|---|---|
| シナリオの書き込み | 操作として見えます（`Watch` の操作、`client` は `scenario`）。 |
| シナリオが controller のステップを保留している間 | 一時停止ではありません（`GetTime` の `paused` は偽、`GetSnapshot` の `consistent` も偽）。 |
| 標準入力の `pause` で止めている間 | `paused` が真で、`consistent` もシナリオのない実行と同じです。 |

標準入力のコマンド（`--stdin-commands`）は、Control Server があっても変わりません（`stop`、一時停止、ステップ実行、速度）。

## 一緒に使えない引数と制限

- `--scenario` は `--start-paused` と一緒に使えません（引数のエラー。`--control` があっても同じです）。シナリオは、時刻 0 の書き込みを最初のステップの前に済ませて、自分で進みます。
- `--stdin-commands` は使えます（`stop`、一時停止、ステップ実行、速度）。止めて進めても、シナリオの結果は変わりません。
- シナリオが使えるのは、設定ファイルにあって無効にしていない、`ecu_main` で作った、仮想時間で動く ECU の変数だけです。
- フレームを直接待つ条件、式、機種ごとの切り替え、変数の対応表は、シナリオにはありません。

## ランチャーの機能の名前（`features`）

バージョンの違う `vehicle_sil_launch` を使うときは、`vehicle_sil_launch version --json` の `features` に次の名前があるかで、シナリオの機能を使えるかがわかります。

| `features` の名前 | 使える機能 | ないランチャーでは |
|---|---|---|
| `scenario` | `check --scenario` と `run --scenario` | シナリオを使えません。 |
| `scenario-wave` | 手順 `wave` | `wave` は、知らないキーのエラーです。 |
| `scenario-motion-expect` | `ramp` と `wave` の横の `expect` | 手順が 2 つの動作をする、というエラーです。 |
| `scenario-points-file` | CSV ファイルの `points` | `points` の形のエラーです。 |
| `timeline` | `run --scenario --timeline <file>` | `--timeline` は引数のエラーです。 |
| `scenario-control` | `--scenario` と `--control` を一緒に使う | 一緒に使えません（Control Server を持つビルドだけにあります）。 |

`features` の名前の全部は、[simulation-events.md](simulation-events.md) にあります。

## 関連するドキュメント

| 知りたいこと | ドキュメント |
|---|---|
| 手順を追ってシナリオを書く | [シナリオでテストを書く](howto/scenario-tests.md) |
| 設定ファイル、機種、引数と終了コード | [simulation-config.md](simulation-config.md) |
| 照合（`check`） | [simulation-check.md](simulation-check.md) |
| 実行（`run`）、シナリオをまとめて実行する `test` | [simulation-run.md](simulation-run.md) |
| イベント（`run --events`）と標準入力のコマンド | [simulation-events.md](simulation-events.md) |
| 実行中に外から値を読む | [control-api.md](control-api.md) |
