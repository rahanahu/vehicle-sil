# シナリオ（`vehicle_sil_launch run --scenario`）

まとめる人と、シミュレーションで試験をする人向けです。先に [simulation-config.md](simulation-config.md)（構成ファイル）、[simulation-check.md](simulation-check.md)（照合）、[simulation-run.md](simulation-run.md)（実行）を読みます。 次に読む文書はありません（手順を追って試すなら、手引き「[シナリオで試験を書く](howto/scenario-tests.md)」）。

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

`<participant>.ecu.connected` に `false` を書くと、その ECU は、動いたまま、すべてのバスから外れます（送ったものは出ず、バスのものは届きません。`true` で戻ります。[internals/execution.md](internals/execution.md) の「ECU の入口」の「バスからの切り離し」）。ほかの ECU が、相手が落ちたことを正しく検出するかを試せます。[examples/ecu_main/scenarios/engine_disconnect.yaml](../examples/ecu_main/scenarios/engine_disconnect.yaml) は、300 ms に Engine を外し、Dashboard が回転数の途絶を検出し（`dashboard.engine_lost`。回転数が 300 ms 来なければ true）、Engine を戻すと回復することを確かめます。

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

JSON Schema は [schema/scenario.schema.json](../schema/scenario.schema.json) です。YAML の言語サーバーを使うエディターでは、ファイルの先頭に `# yaml-language-server: $schema=<このファイルへのパス>` と書くと、キーの補完と説明、形の検査が効きます（[例のファイル](../examples/ecu_main/scenarios/throttle_step.yaml)の 1 行目。パスは、書いているファイルのディレクトリから数えます）。スキーマが見るのは、キーと値の形だけです。変数があるか、値が型と範囲に合うか、時刻の順は、`check --scenario` が確かめます（下の「照合」）。ひな形には `schema/` に写しが入っています。道具の圧縮ファイルでは `schema/` にあります。

| キー | 必須 | 意味 |
|---|---|---|
| `vehicle_sil_scenario` | はい | 形式の版。`1` |
| `name` | いいえ | シナリオの名前（出力に出る） |
| `only` | いいえ | このシナリオを回す機種の名前の並び（`only: [small, medium]`）。書かなければ、どの機種でも回す（[機種とパラメーター](#機種とパラメーター)） |
| `params` | いいえ | パラメーターの既定の値（名前 → 値）。手順の値の位置に `${名前}` と書いて使う（[機種とパラメーター](#機種とパラメーター)） |
| `steps` | はい | 手順の並び（1 つ以上）。上から順に進む |

1 つの手順は、`set`、`ramp`、`wave`、`wait_until`、`expect` のどれか 1 つと、時刻を持ちます。

| キー | 意味 |
|---|---|
| `at_ms` | 手順の時刻（仮想時刻、ミリ秒、0 以上） |
| `after_ms` | 手順の時刻を、前の手順が終わった時刻からのミリ秒で書く（0 以上）。`at_ms` も `after_ms` もなければ `after_ms: 0` |
| `set: { <変数>: <値>, ... }` | 値を入れる |
| `ramp: { <変数>: { to: <値>, over_ms: <ms>, from: <値> }, ... }` | `over_ms` の間、直線で値を動かす。`from` は省ける |
| `wave: { <変数>: { <波形>: { ... }, around: <値>, for_ms: <ms> }, ... }` | 決まった波形（`sine`、`square` など）で値を動かす（[波形で動かす](#波形で動かすwave)） |
| `wave` と `undersampled: ok` | その手順の波形が ECU のステップより細かくても、照合で何も言わない |
| `wait_until: { <変数>: <条件>, ... }` と `timeout_ms: <ms>` | 条件が成り立つまで待つ。`timeout_ms`（必須）の間に成り立たなければ失敗 |
| `expect: { <変数>: <条件>, ... }` | 手順の時刻に、条件が成り立つことを確かめる |
| `expect` と `within_ms: <ms>` | その間に 1 度でも成り立てばよい。成り立った時点で次へ進む |
| `expect` と `hold_for_ms: <ms>` | その間ずっと成り立つ |

- **変数**は `<participant>.<変数名>` です。最初の `.` までが participant の名前で、残りが ECU の中の変数名です（変数名に `.` があってよい。例: participant `Engine` の `engine.speed` は `Engine.engine.speed`）。
- **値**は、変数の型に合わせて書きます。bool は `true` / `false`、integer は整数、real は数、enum は項目の名前か値です。
- **条件**は比較だけです。値をそのまま書けば「等しい」です。対応付けで `eq`、`ne`、`ge`、`le`、`gt`、`lt`、`near` と `tolerance`（差が `tolerance` 以下）を書けます。1 つの変数に複数書けば、すべてです（`{ ge: 1700, le: 1900 }`）。1 つの手順に複数の変数を書けば、すべての変数です。bool と enum に使えるのは `eq` と `ne` だけです。
- 書けるのは、入力の変数です。Parameter は、時刻 0 の `set` でだけ書けます（最初のステップの前に入ります）。出力は読むだけです。`set`、`ramp`、`wave` の値は、変数の範囲（`min`、`max`）の中にします。`ramp` は integer と real だけです（integer は四捨五入）。`wave` は、波形ごとに使える型が決まっています。

## 時刻

手順の時刻 T は、`at_ms` か、前の手順が終わった時刻 + `after_ms` です。最初の手順の前は 0 です。

| 手順 | 終わる時刻 |
|---|---|
| `set`、`expect`（その時点） | T |
| `ramp` | T + 最も長い `over_ms` |
| `wave` | T + 最も長い波形の長さ（`for_ms`。`pulse` は `width_ms`、`points` は最後の点の時刻、`bounces` を書いた `chatter` は 2 × `bounces` × `interval_ms`） |
| `expect` と `hold_for_ms` | T + `hold_for_ms` |
| `wait_until`、`expect` と `within_ms` | 成り立ったサンプルの時刻 t の、次の controller のステップ P の、さらに 1 刻み後（P + 刻み） |

- 上の 4 つは、実行の前に終わる時刻がわかります。これらの手順は、前の結果によらず、書いた時刻どおりに進みます。
- `wait_until` と `within_ms` つきの `expect` は、終わる時刻が実行するまで決まりません。ランチャーは、時刻 t のサンプルを、その次の controller のステップ P で確かに持っています。次の手順の書き込みが間に合う最初の時刻が P + 刻みなので、そこを終わりとします。例では、回転数が 1700 になったサンプルが 330 ms、P が 335 ms、終わりが 340 ms です。
- そのため、これらの手順より後ろには `at_ms` を書けません（`after_ms` で書きます）。時刻が戻る `at_ms` も誤りです。
- controller の刻みは、構成ファイルの `time_step_ms` です。書いていなければ、ECU のステップがすべて controller のステップから始まる刻み（結果の `timing.time_step_ms`。[simulation-run.md](simulation-run.md) の「等速で動かす」の、速さを決めたときの既定と同じ）です。シナリオのある実行では、速さによらずこの刻みです。

## 値の見え方

ECU は、Agent の `sample_period`（既定 10 ms の仮想時間）ごとに、ステップの終わりの全変数の値を 1 組（サンプル）送ります。サンプルの時刻は、そのステップの始まりの時刻です。

- 「時刻 T の値」は、変数ごとに、その ECU の、T 以前に始まったステップの最後のサンプルの値です。T に始まるステップで入った `set` の値は、そのステップがサンプルを取るときだけ、T のサンプルに入っています。サンプルを取るのは、`sample_period` の倍数の時刻以後の最初のステップです（既定では、10 ms の倍数の時刻に始まるステップ。ステップが 5 ms の ECU の 105 ms のステップは取りません）。その時刻なら、`at_ms: 100` の `set` の直後に `at_ms: 100` の `expect` を書けます。そうでない時刻の `expect` は、前のサンプル（`set` の前の値）を見て失敗します。値が入ったことを確かめるには、`within_ms` つきの `expect` を使います（次のサンプルで成り立ちます）。ランチャーは ECU の `sample_period` を知らないので、照合ではわかりません。
- `wait_until` と `within_ms` は、T の値と、その後に届くサンプルを 1 つも飛ばさずに見ます。1 サンプルだけ成り立つ変化も見逃しません。`hold_for_ms` も同じで、1 サンプルでも外れれば、そのサンプルの時刻で失敗です。
- `sample_period` より細かい変化は見えません。サンプルとサンプルの間に変わって戻った値は、条件に現れません。
- まだサンプルが 1 つもない変数（最初のステップの前）は、どの条件も成り立ちません。
- 構成ファイルの participant に `variables`（対応表）があれば、シナリオに書く変数の名前と値は、対応の後のものです（[simulation-config.md](simulation-config.md#変数の名前と値を合わせるvariables)）。実行ファイルごとの名前や単位の違いは構成ファイルが吸収するので、シナリオは 1 つで済みます。一次式で変えた変数は実数になり、書いた値と読んだ値が丸めの分だけ違うことがあるので、`near` と `tolerance` で比べます。

## 書き込み

- `set` は、時刻 T を付けた書き込みです。各 ECU の、T 以降に始まる最初のステップの先頭で入り、そのステップのアプリケーションが新しい値を見ます。T が ECU のステップの間なら、次のステップで入ります。
- ランチャーは、時間のゲートで controller の各ステップを止め、次の刻みまでの書き込みを ECU に積み終えてから通します。時刻 0 の分は、全員が最初のステップの前で待っている間に積みます。書き込みが、書いた時刻のステップに間に合わないことはありません。
- 1 つの `set` に同じ participant の変数を複数書くと、同じステップでまとめて入ります。
- `ramp` は、変数ごとに、T、T + ステップ、T + 2 ステップ、…（その ECU のステップ）に、`from` から `to` への直線の上の値を書き、T + `over_ms` に `to` を書きます。`from` を省くと、シナリオがその変数に最後に書いた値（それより新しいサンプルがあれば、その値）から始めます。どちらもなければ失敗です（`from` を書きます）。
- 書き込みの結果は、適用する ECU のステップの次の controller のステップで確かめます。適用されなかった書き込み（ECU の setter が拒んだ、相手がつながっていない、など）は、その手順の失敗です。

## 波形で動かす（`wave`）

`wave` は、変数を決まった波形で動かします。
次の例は、スロットルを、いまの値を中心に ±5 %、1 秒に 10 回の正弦で 200 ms 動かします（[examples/ecu_main/scenarios/throttle_wave.yaml](../examples/ecu_main/scenarios/throttle_wave.yaml) の 15 行）。

```yaml
  - wave: { Engine.engine.throttle: { sine: { amplitude: 5, period_ms: 100 }, for_ms: 200 } }
```

変数ごとに、波形を 1 つと、その横に長さ（`for_ms`）と中心（`around`）を書きます。
1 つの手順に複数の変数を書けます（同じ変数は 1 回）。手順は、いちばん長い波形が終わると終わります。

| 波形 | 書くもの | 値 | 使える型 |
|---|---|---|---|
| `sine` | `amplitude`、`period_ms` か `frequency_hz`、`phase_deg` | 中心 ± `amplitude` の正弦。始まりは中心で、1/4 周期で最大 | integer、real |
| `triangle` | 同じ | 三角波。山と谷の位置は `sine` と同じ | integer、real |
| `sawtooth` | 同じ、`falling: true` | のこぎり波。周期の始めが最小で、直線で最大まで上がって戻る。`falling: true` は逆 | integer、real |
| `square` | `low` と `high`、または `amplitude`。`period_ms` か `frequency_hz`、`duty`（%、既定 50）、`phase_deg` | 矩形波。周期の始めから `duty` % の間が高い側。`low`・`high` はその 2 つの値、`amplitude` は中心 ± `amplitude`。bool はどちらも書かない（`false` と `true`） | integer、real、bool |
| `pulse` | `value`、`width_ms` | `width_ms` の間だけ `value`、終わると元の値。bool は `value` を省ける（`true`） | integer、real、bool |
| `noise` | `amplitude`、`hold_ms`、`seed`、`distribution` | 乱数。1 つの値を `hold_ms` の間保つ。`distribution: uniform`（既定）は中心 ± `amplitude` の一様、`gaussian` は標準偏差が `amplitude` の釣り鐘形（標準偏差の 6 倍を超えない） | integer、real |
| `chirp` | `amplitude`、`from_hz`、`to_hz`、`sweep` | 周波数が `for_ms` の間に `from_hz` から `to_hz` へ変わる正弦。`sweep: linear`（既定）は毎秒同じ Hz ずつ、`log` は毎秒同じ倍率ずつ | integer、real |
| `lag` | `to`、`tau_ms`、`from`、`settle: true` | 一次遅れの形で `to` に寄る（`tau_ms` で 63 %、その 5 倍で 99 %）。`from` を省くと、いまの値から | integer、real |
| `chatter` | `to`、`interval_ms`、`bounces` か横の `for_ms`、`seed`、`from` | スイッチの跳ね。`to`、元の値、`to`、元の値、… と `interval_ms` ごとに変わり、最後は `to`。`bounces` は元の値に戻る回数。`seed` を書くと、変わる時刻が `interval_ms` の 1/4 まで前後にずれる。bool は `from` を書かない（`to` の反対） | integer、real、bool |
| `points` | `[[<ms>, <値>], ...]`、横に `interpolate` | 時刻（手順の始めからの ms、昇順）と値の表。`interpolate: linear`（数の既定）は点の間を直線で、`hold` は次の点まで値を保つ。bool と enum は `hold` だけ | どの型でも |

- **周期**は `period_ms`（1 以上の整数）か `frequency_hz`（0 より大きい数）のどちらかです。1 ms より短い周期は `frequency_hz` で書きます。`phase_deg` は波形を始める位置で、90 なら 1/4 周期の所から始めます。
- **長さ**は `for_ms` です。周期の倍数でなくてもかまいません。`pulse` と `points` は自分で長さを持つので `for_ms` を書きません。`chatter` は `bounces` か `for_ms` のどちらかです。
- **中心**は `around` です。省くと、手順が始まる時点のその変数の値です（`ramp` の `from` と同じ決まり: シナリオが最後に書いた値か、それより新しいサンプル。どちらもなければ失敗するので、`around` を書きます）。
- **終わった後の値**: 中心（`around`、省いたときは始まる時点の値）に戻します。`low`・`high` を書いた `square` と bool の `square` は、低い側で終わります。行き先のある波形はそこに留まります: `points` は最後の点の値、`chatter` は `to`、`lag` は `for_ms` の時点で着いていた値（`settle: true` なら `to`）。続けて `around` のない `wave` や `from` のない `ramp` を書くと、この値から始まります。
- `around` を書けるのは、中心のある波形（`sine`、`triangle`、`sawtooth`、`amplitude` の `square`、`noise`、`chirp`）と、数の `pulse`（終わった後に戻る値）です。`lag` と `chatter` の始まりの値は `from`、`points` は最初の点で書きます。`points` の最初の点が 0 より後なら、そこまでは、いまの値から最初の点への直線（`hold` なら、いまの値のまま）です。
- **値の決まり方**: 値は、手順が始まった仮想時刻からの経過と、書いた数だけで決まります。ECU のステップ幅、controller の刻み、実行の速さによりません。`noise` と `chatter` の乱数も、`seed` と時刻だけから作るので、同じシナリオは毎回同じ値になり、失敗した実行をそのまま再現できます。
- **書く時刻**は `ramp` と同じです。変数ごとに、T、T + ステップ、T + 2 ステップ、…（その ECU のステップ）にその時刻の値を書き、終わりの時刻に終わった後の値を書きます。integer は四捨五入です。
- **範囲**: `low`、`high`、`value`、`to`、`from`、`around`、`points` の値と、`around` を書いたときの `around` ± `amplitude` は、照合で範囲を確かめます。`around` を省いた波形と `gaussian` の `noise` は、実行しないと値がわからないので、範囲の外の値を書こうとした時点でその手順が失敗します（`scenario-write`。値を黙って丸めません）。
- `${名前}` は、波形の中の数の位置にも書けます（`amplitude: "${amp}"`）。
- 手順は 1 つずつ進むので、`wave` が動いている間は `expect` できません。波形を掛けた結果は、`wave` の後の手順で確かめます。
- **`square` と PWM**: `square` は、値そのものを高い側と低い側に切り替えます（ECU がピンのエッジを見る入力）。ECU がデューティーや周波数を数の変数として受ける入力には、その変数に `set` や `ramp`、`sine` を使います。

`vehicle_sil::signal`（[`signal/waveform.hpp`](../libs/signal/include/vehicle_sil/signal/waveform.hpp)）に同じ計算の関数があり、自分の C++ のコードからも使えます。

### ECU のステップより細かい波形

ECU は、ステップごとに 1 回だけ入力を受け取ります。
それより細かい波形は、書いたとおりには ECU に見えません。
照合（`check --scenario`、`run --scenario` の最初）は、相手の ECU のステップ幅と比べて、ECU に実際に見える形を警告で言います。
警告なので、実行はできます。`--strict` では照合で落ちます。

| 所見 | 出るとき |
|---|---|
| `scenario-wave-undersampled`（警告） | 周期がステップ幅の 2 倍より短い（`sine`、`triangle`、`sawtooth`、`square`。`chirp` は高い側の周波数）。`pulse` の `width_ms`、`noise` の `hold_ms`、`points` の点の間隔、`chatter` の `interval_ms`（`seed` があればその半分）がステップ幅より短い |
| `scenario-wave-duty`（警告） | `square` の高い側の長さ（周期 × `duty`）がステップ幅の倍数でない（ECU に見えるデューティーを言う）。周期がステップ幅の倍数でない |
| `scenario-wave-coarse`（注記） | `sine`、`triangle`、`sawtooth`、`chirp` の 1 周期が 8 ステップより少ない（山と谷を取りこぼすことがある） |
| `scenario-wave-not-checked`（注記） | 相手の participant のステップ幅がわからないので、比べていない |

例です。ステップ幅 10 ms の ECU に、周期 30 ms、デューティー 25 % の矩形波を書いたとき:

```text
warning: pwm.yaml:5: the square of Door.lamp is high for 7.5 ms of each period of 30 ms (duty 25 %), but participant 'Door' takes its inputs once per step of 10 ms: the ECU sees it high for 10 ms of each period (33.3 %). Make the high time (period x duty / 100) a multiple of 10 ms; to run it as it is without this message, add undersampled: ok to the step
```

- わかっていてそのまま流すときは、その手順に `undersampled: ok` を書きます。その手順の上の 4 つの所見が出なくなります。

  ```yaml
    - wave: { Door.lamp: { pulse: { width_ms: 3 } } }
      undersampled: ok
  ```

- ステップ幅より短い `pulse` は、手順の時刻が ECU のステップの時刻なら 1 ステップの間だけ見え、そうでなければ見えません（始まりと終わりの書き込みが、同じステップに入るためです）。
- 動かしようのないものは誤りです: 周期や幅が 0 以下、`noise` に `seed` がない、`points` の時刻が昇順でない、型に合わない波形（bool に `sine` など）、範囲の外。

## 機種とパラメーター

構成ファイルに機種（`variants`）があるとき、同じシナリオをどの機種にも当てられるように、次の 3 つがあります。機種の違いは構成の側に書き、シナリオには、機種の名前で分かれる手順を書きません（構成の側の書き方は [simulation-config.md](simulation-config.md) の「機種」と「シナリオのパラメーター」）。

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

### パラメーター（`params`、`${名前}`）

- `${名前}` は、手順の中の値の位置（`set` の値、条件の値と `tolerance`、`ramp` の `to`・`from`・`over_ms`、`wave` の数と長さ、`at_ms`、`after_ms`、`timeout_ms`、`within_ms`、`hold_for_ms`）に、値の全体として書きます。その位置に、パラメーターの値を書いたのと同じに読みます（型と範囲の照合も同じで、誤りの文には、どのパラメーターのどこから来た値かが出ます）。
- キー、変数名、値の一部（`"${a} rpm"`）には書けません。式（`${a+1}`）もありません（誤り `scenario-param`）。計算した値が要るときは、その値を別のパラメーターにします。
- `{ ... }` や `[ ... ]` の中では、引用符で囲みます（`{ near: "${idle_rpm}" }`。YAML が、引用符のない `${` をそこでは読めないためです）。JSON でも文字列で書きます。引用符で囲んでも、値はパラメーターに書いたとおりに読みます（数は数です）。
- 値は、後のものが前のものを置き換えます: シナリオの `params`、構成ファイルの `params`、選んだ機種の `params`、引数の `--param <名前>=<値>`。シナリオの `params` に書いていない名前も、ほかから値が来れば使えます。
- どこからも値が来ない `${名前}` は、照合の誤りです（実行の前。`scenario-param`）。シナリオが使っても宣言してもいない名前の `--param` は、引数の誤りです（終了コード 4。シナリオのパラメーターの名前を並べます）。
- パラメーターに誤りがある間は、手順の中身の照合をしません（まずパラメーターを直します）。
- `params` の値は 1 つの値（数、`true`・`false`、文字列）で、ほかのパラメーターは使えません。名前は英数字と `_`（数字で始めない）です。

### ない変数を使う手順は飛ばす

- 構成の対応表で `absent: true` とした変数（その実行ファイルにはない、と書いた変数）を使う手順は、実行で飛ばします。照合では、手順ごとに注記 `scenario-step-skipped` が出ます（誤りではありません）。
- 選んだ機種が外した participant（機種の `enabled: false`）の変数を使う手順も、同じに飛ばします。理由は `participant 'Door' is not in the variant medium (enabled: false in that variant)` の形です。その participant は照合していないので、変数の名前は確かめません。機種のほかの理由で外した participant（共通の部分の `enabled: false`、`--disable`）の変数を使う手順は、今までどおり照合の誤り（`scenario-variable`）です。機種が外した participant を `--enable` で戻したときは、ほかの participant と同じに照合します。
- 飛ばすのは、その変数を使う手順だけです。1 つの手順に、ある変数とない変数の両方があれば、手順の全体を飛ばします（ある変数にも書きません）。飛ばした `set` に頼る後ろの手順は、飛ばさずに回ります。機種によって成り立たない手順は、ない変数を使う形にするか、シナリオを分けて `only` を書きます。
- 飛ばした手順も、文面からわかる長さは使ったことにします: `ramp` はいちばん長い `over_ms`、`wave` はいちばん長い波形の長さ、`hold_for_ms` のある `expect` はその長さ、`set` と長さのない `expect` は 0 です。次の手順の `after_ms` は、その終わりから数えます。飛ばす機種と飛ばさない機種で、後ろの手順の時刻は同じです。ない変数の `ramp` も、`over_ms` だけは読みます（`to` と `from` は読みません）。ない変数の `wave` も、長さ（`for_ms`、`width_ms`、最後の点の時刻、`bounces` と `interval_ms`）だけは読みます。
- 例外は、待つ手順（`wait_until`、`within_ms` のある `expect`）です。いつ終わるかは実行しないとわからないので、飛ばすと 0 です（手順の時刻で終わったことにします）。その後ろの `after_ms` の手順は、飛ばさない機種より早く始まります。待った後の時刻が要る手順は、待つ手順と同じ変数を使う形にする（一緒に飛ばす）か、シナリオを分けて `only` を書きます。
- ファイルの読み方は機種で変わりません。`at_ms` が前の手順の終わりより前、という誤りは、手順を飛ばす機種でも同じに出ます。待つ手順を飛ばしても、その後ろに `at_ms` は書けません。
- 飛ばした手順のうち最後のものの長さは、実行を延ばしません。シナリオは、飛ばさない手順が全部決まった所で終わります。
- 結果では、手順の `status` が `skipped`（人向けは `skipped`）で、理由が付きます。シナリオの結果は、ほかの手順が全部通れば `passed` のままで、飛ばした数が出ます（人向けは `passed, 3 of 4 steps passed, 1 skipped`、JSON は `skipped_steps`）。
- 確かめる手順（`expect`、`wait_until`）を全部飛ばすと、シナリオは何も確かめずに `passed` になります。このとき、照合で警告 `scenario-nothing-checked` を 1 つ出します（確かめる手順の行と、次にすることを言います: その機種のシナリオでなければ `only` を書く、その機種のものなら対応表を足す）。既定では実行は通り、`--strict` では照合で落ちて何も起動しません。もとから確かめる手順のないシナリオ（`set`、`ramp`、`wave` だけ）には出しません。
- 対応も `absent` も書いていない変数が実行ファイルになければ、今までどおり照合の誤り（`scenario-variable`）です。黙って飛ばしません。

### 機種を限る（`only`）

- `only: [small, medium]` と書いたシナリオは、その機種のためのものです。ほかの機種（`--variant large`）で `check --scenario` か `run --scenario` に渡すと、誤り `scenario-variant` で、何も起動しません。そのとき、手順の中身は照合しません（その機種には合わないかもしれないため）。全部の機種を回す `vehicle_sil_launch test` は、その組を回さずに `skipped` にします（[simulation-run.md](simulation-run.md#シナリオをまとめて回すvehicle_sil_launch-test)「シナリオをまとめて回す」）。
- `only` の名前は、構成ファイルの `variants` にある名前だけです。機種のない構成に `only` のあるシナリオを渡すと誤りです。

## 結果

- 手順が 1 つ失敗すると、シナリオはそこで終わりです（仮想時刻の順で最初の失敗）。ランチャーはシミュレーションを止め、実行は失敗します（終了コード 1）。始まっていた手順は `unfinished`、その後ろは `not_run` です。後ろの手順の書き込みのうち、次の刻みまでの分は、すでに ECU に渡っていることがあります。
- 全部通ると、ランチャーはその controller のステップでシミュレーションを止め、実行は成功します（ほかに誤りがなければ終了コード 0）。構成ファイルに `stop_at_ms` がなくても回せます。
- `stop_at_ms` に先に着くと、決められる所まで決めて、決まっていない最初の手順が失敗です（`scenario-out-of-time`。`stop_at_ms` を大きくするか、消します）。
- 止める要求（Ctrl+C、`stop` の命令）、participant の失敗、`--timeout-ms` は、シナリオより優先です（[simulation-run.md](simulation-run.md) の規則のまま）。そのとき、シナリオは `incomplete` で、誤り `scenario-incomplete` が、どこまで進んだかを言います。落ちた ECU への書き込みが先に失敗しても、実行の結果は participant の失敗で、シナリオは `incomplete` です。
- 手順ごとの `at`（JSON の `at_ms`）は、その手順を決めた仮想時刻です。`set` は値を適用した ECU のステップ、`ramp` と `wave` は終わり、その時点の `expect` は T、`wait_until` と `within_ms` は成り立ったサンプルの時刻、`hold_for_ms` は終わり（失敗なら外れたサンプルの時刻）、時間切れは切れた時刻です。

失敗の所見は、シナリオのファイルと行、手順、時刻、そのときの値を言います。失敗する例です。スロットルを踏んでから 200 ms のうちに回転数が 1700 rpm に届くことを確かめますが、届くのは 330 ms です（上の例の `wait_until`）。

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

error: throttle_response.yaml:6: expect Engine.engine.speed ge 1700 (within 200 ms): not true at any time within 200 ms (from 0 ms to 200 ms): at the end Engine.engine.speed ge 1700, but it was 1050 (its sample of 200 ms)

failed: 1 error, 0 warnings, 0 notes
```

終了コードは 1 です。失敗した手順の後ろは回りません（`not run`）。`wait_until` の時間切れは、次の形です。

```text
error: s.yaml:4: wait_until Body.ticks ge 10; Body.level eq 6 (timeout 100 ms): timed out at 100 ms (100 ms after 0 ms): Body.level eq 6, but it was 5 (its sample of 100 ms)
```

| `code`（実行の所見、どれも誤り） | 意味 |
|---|---|
| `scenario-failed` | `expect` が成り立たなかった。または、読む変数の participant がこの実行でつながっていない |
| `scenario-timeout` | `wait_until` が `timeout_ms` の間に成り立たなかった |
| `scenario-write` | `set`、`ramp`、`wave` の書き込みが適用されなかった（`wave` が範囲の外の値を書こうとした、など）。または、`ramp` の始まりの値や `wave` の中心の値がない |
| `scenario-out-of-time` | `stop_at_ms` で止まったとき、まだ決まっていなかった |
| `scenario-incomplete` | シナリオが終わる前に、ほかの理由でシミュレーションが終わった |

## 照合（`check --scenario`、`run --scenario` の最初）

シナリオは、実行の前に全部照合します。`vehicle_sil_launch check <構成ファイル> --scenario <シナリオ>` は照合だけをします。誤りがあれば、`run` は何も起動しません。所見は、シナリオのファイル名と行を付けて、構成ファイルの所見と一緒に出ます（JSON では所見の `file`）。

誤りのある例です。変数の名前を打ち間違え、出力に書き、範囲の外の値を入れ、待つ手順の後ろに `at_ms` を書き、`near` に `tolerance` を付けていません。

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

error: throttle_mistakes.yaml:5: 'Engine.engine.throttel': participant 'Engine' has no variable 'engine.throttel': did you mean 'Engine.engine.throttle'? (vehicle_sil_launch check --json lists the variables)
error: throttle_mistakes.yaml:6: Engine.engine.speed is an output of its ECU: it can be read (expect, wait_until), not written; write one of the ECU's inputs instead
error: throttle_mistakes.yaml:7: the value of Engine.engine.throttle (real) is 150, outside its range (from 0 to 100 %)
error: throttle_mistakes.yaml:10: at_ms after a step that waits (line 8): when that step ends is not known before the run; give this step after_ms (from the end of the step before it) instead
error: throttle_mistakes.yaml:11: Engine.engine.speed: near needs tolerance (how far from the value it may be): add tolerance: <number>

failed: 5 errors, 0 warnings, 0 notes
```

照合は誤りを全部挙げます（最初の 1 つで止まりません）。終了コードは 1 です。

| `code` | 意味 |
|---|---|
| `file`、`encoding`、`syntax`、`not-a-scenario`、`format-version`、`unsupported-yaml` | ファイルが読めない、シナリオでない、版が違う、アンカーや 2 つ目の文書がある |
| `unknown-key`、`duplicate-key`、`missing-key`、`value` | キーと値の形の誤り（1 つの手順に 2 つの動作、`wait_until` に `timeout_ms` がない、`near` に `tolerance` がない、など） |
| `scenario-variable` | 変数が `<participant>.<変数名>` でない、participant か変数がない、participant を使えない（外している、`describe: false`、実時間で動く、変数を出していない） |
| `scenario-not-writable` | 出力に書いている。Parameter を時刻 0 の `set` 以外で書いている |
| `scenario-value` | 値が型、範囲、enum の項目に合わない。`ramp` や大小の比較を、数でない変数に使っている。`wave` の波形が変数の型に合わない、`around` ± `amplitude` が範囲の外 |
| `scenario-wave-undersampled`、`scenario-wave-duty`（警告）、`scenario-wave-coarse`、`scenario-wave-not-checked`（注記） | `wave` が相手の ECU のステップより細かい（[ECU のステップより細かい波形](#ecu-のステップより細かい波形)） |
| `scenario-param` | `${名前}` に値がない。`${...}` がキーや変数名、値の一部にある（シナリオにない名前の `--param` は、引数の誤り `usage`） |
| `scenario-variant` | シナリオの `only` に、選んだ機種がない。`only` の名前が構成の機種にない |
| `scenario-step-skipped`（注記） | 手順が `absent: true` の変数か、機種が外した participant の変数を使うので、実行で飛ばす |
| `scenario-nothing-checked`（警告） | 確かめる手順（`expect`、`wait_until`）を全部飛ばすので、シナリオが何も確かめない |
| `scenario-time` | `at_ms` が前の手順の終わりより前。待つ手順より後ろに `at_ms` がある。手順の時間を足すと、時刻の上限（1000000000000 ms）を超える |

## 出力

- 人向け: 照合の部分に `scenario <ファイル> (<name>): <n> steps`（飛ばす手順があれば ` (<n> skipped)`）と、パラメーターがあれば `parameters: <名前> = <値> (<どこから>), ...` の行。実行の部分に、合否の 1 行と手順の表（行、手順、結果、決めた仮想時刻。飛ばした手順は `-`）、飛ばした手順ごとに `line <行> skipped: <理由>`。実行中は、手順が決まるたびに 1 行（標準エラー。飛ばした手順の行に ` at <時刻> ms` は付きません）。
- `check --json` と `run --json` の `check`: `scenario` に `file`、`name`、`steps`（手順の数）。`--scenario` を付けたときだけあります。シナリオにあるときだけ、次のキーが続きます: `only`（機種の名前の配列）、`params`（シナリオが宣言したか使ったパラメーターごとに `name`、`value`（効いている値。`true`・`false`、数、文字列）、`from`（`scenario`、`configuration`、`variant`、`command_line`））、`skipped_steps`（飛ばす手順の数）。
- `run --json`: 最上位に `scenario`（`--scenario` を付けたときだけ）。

| キー | 意味 |
|---|---|
| `file`、`name` | シナリオのファイル（引数のとおり）と名前（なければ null） |
| `status` | `passed`、`failed`、`incomplete`（ほかの理由で途中で終わった）、`not_run`（シミュレーションが始まらなかった） |
| `over_at_ms` | シナリオが終わった controller のステップ（`passed` と `failed`。ほかは null） |
| `skipped_steps` | 飛ばした手順の数。1 つ以上あるときだけ、このキーがあります |
| `steps[]` | 手順ごとに `index`（1 から）、`line`、`kind`（`set`、`ramp`、`wave`、`expect`、`wait_until`）、`text`、`status`（`passed`、`failed`、`unfinished`、`not_run`、`skipped`）、`start_ms`（手順の時刻）、`at_ms`（決めた時刻）、`end_ms`（終わる時刻）、`code` と `message`（失敗のときと、飛ばしたとき。飛ばした手順の `code` は `scenario-skipped`、`message` は理由）。飛ばした手順の `at_ms` は null です（実行の前に決まっていて、仮想時間のどこかで決めたのではないため）。`start_ms` と `end_ms` は、飛ばさなかったときの手順の時刻と終わり（`start_ms` に文面の長さを足したもの）で、シミュレーションがそこまで進んだことは意味しません |

上の「結果」の失敗する例（`throttle_response.yaml`）を `run --json` で回したときの `scenario` です（ほかのキーは [simulation-run.md](simulation-run.md) の「JSON（`run --json`）」）。

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

- 時刻はミリ秒の数です（ECU のステップが 1 ms より細かいと、小数になることがあります）。わからない時刻は null です。
- シナリオが終わってランチャーが止めた実行の `simulation` は `ended` です。
- `run --events`: 手順が始まったとき（`status` は `running`）と決まったときに、事象 `scenario_step`（`steps[]` と同じキー）。結果の事象にも `scenario` があります。飛ばす手順の `scenario_step`（`status` は `skipped`、`at_ms` は null）は 1 回で、その手順の時刻が決まったときに出ます（実行の前からわかっているので、仮想時間がそこへ進むより前に出ることがあります）。
- `vehicle_sil_launch version --json` の `features` に `scenario` があれば、この機能を使えます。

## 実行中に変数を見る（`--control`）

`--scenario` と `--control` を一緒に付けると、シナリオの実行中も Control Server が立ちます（[control-api.md](control-api.md) の「ランチャーにつなぐ」。Control Server を持つビルドだけ）。クライアントは、見るだけです。

| クライアントの呼び出し | シナリオの実行中 |
|---|---|
| 読む、待つ: `Ping`、`ListPeers`、`GetValues`、`GetSnapshot`、`Watch`、`GetTime`、`WaitPaused`、`GetOperation`、`WaitOperation` | いつもどおり |
| 変数を書く、Peer を消す: `Write`、`ForgetPeer` | 断る |
| 時刻を動かす: `Pause`、`Resume`、`Step`、`RunUntil`、`SetSpeed` | 断る |
| 止める: `Stop` | 受け付ける。止める要求の 1 つで、シナリオは `incomplete`（`interrupted_by` は `"control server stop"`） |

- 断った呼び出しは `FAILED_PRECONDITION` で、trailing metadata の `vehicle-sil-reason` が `scenario-running` です（Python では `ControlError.reason`）。文言は、何を断ったか、なぜか、何ができるかを言います。役割（[control-api.md](control-api.md) の「認証」）によりません。断った呼び出しは、Hub にも時間のゲートにも届かず、事象 `command` にもなりません（監査の行は残ります）。
- クライアントがいても、いなくても、シナリオの結果は同じです。読む呼び出しと購読は、シミュレーションの進みを待たせません（同じロックを短く取り合うので、読むクライアントが多いと、実時間では少し遅くなりえます。等速の実行の `timing` は、その分だけ変わりえます。シナリオの結果は変わりません）。購読は、読まないクライアントの分を上限つきで持ち、あふれた分を捨てて数えます（値は Peer ごとに最新の 1 つ）。
- クライアントに見えるもの: シナリオの書き込みは、操作として見えます（`Watch` の操作、`client` は `scenario`）。シナリオが controller のステップを保留している間は、一時停止ではありません（`GetTime` の `paused` は偽、`GetSnapshot` の `consistent` も偽）。標準入力の `pause` で止めている間は、`paused` が真で、`consistent` もふつうの実行と同じです。
- 標準入力の命令（`--stdin-commands`）は、Control Server があっても今までどおりです（`stop`、一時停止、ステップ送り、速さ）。
- `vehicle_sil_launch version --json` の `features` に `scenario-control` があれば、この組み合わせを使えます。

## 一緒に使えない引数と制限

- `--scenario` は `--start-paused` と一緒に使えません（引数の誤り。`--control` があっても）。シナリオは、時刻 0 の書き込みを最初のステップの前に済ませて、自分で進みます。`--stdin-commands` は使えます（`stop`、一時停止、ステップ送り、速さ。止めて進めても、シナリオの結果は変わりません）。
- シナリオが使えるのは、構成ファイルにあって外していない、`ecu_main` で作った、仮想時間で動く ECU の変数だけです。
- フレームを直に待つ条件、式、機種ごとの切り替え、変数の対応表はありません。
