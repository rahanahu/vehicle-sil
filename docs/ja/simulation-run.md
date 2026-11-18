# シミュレーションの実行（`vehicle_sil_launch run`）

[English](../simulation-run.md)

`vehicle_sil_launch run <file>` が何をどの順にするか、いつ中断するか、何を出力するかのリファレンスです。複数のシナリオをまとめて実行する `vehicle_sil_launch test` も、このドキュメントにあります。引数の一覧は [simulation-config.md](simulation-config.md) の「引数と終了コード」にあります。

| 調べること | 節 |
|---|---|
| `run` が何をどの順にするか | [実行の流れ](#実行の流れ) |
| どんなときに中断するか、タイムアウトの数え方 | [中断する条件とタイムアウト](#中断する条件とタイムアウト) |
| ECU の変数を読み書きできるか（`control_link`） | [ECU の変数との接続](#ecu-の変数との接続control_link) |
| `speed` と、実時間からの遅れの数え方 | [速度を指定して動かす](#速度を指定して動かすspeed) |
| ネットワークを実機のバスにつなぐ、そのブリッジが終わったとき | [バスを外につなぐ](#バスを外につなぐ--connect) |
| この PC で各ネットワークをどこにつなげるか | [`connections`](#つなげる先の一覧vehicle_sil_launch-connections) |
| participant を終了させる順序、Ctrl+C | [止め方](#止め方)、[中断（Ctrl+C）](#中断ctrlc) |
| どのアドレスで待ち受けるか、Windows のファイアウォール | [待ち受けるアドレス](#待ち受けるアドレス) |
| 標準エラー、ログのファイル、結果の表、指摘の `code` | [出力](#出力) |
| 終了コード | [終了コード](#終了コード) |
| `run --json` のキー | [JSON](#jsonrun---json) |
| シナリオが書いた値と見た値を JSON のファイルに記録する | [`--timeline`](#シナリオの実行を時間の軸で記録する--timeline) |
| そのファイルを図で見る、シナリオが失敗した理由を見る | [時間の軸のファイルを見る](#時間の軸のファイルを見る) |
| 全部の機種で、シナリオをまとめて実行する | [`test`](#シナリオをまとめて実行するvehicle_sil_launch-test) |

## 実行の流れ

| 手順 | すること |
|---|---|
| 1 | `check` と同じ照合をします。 |
| 2 | registry を、このプロセスの中で起動します。 |
| 3 | controller（participant 名 `SimulationController`）を、このプロセスの中で動かします。 |
| 4 | 無効にしていない participant を、設定ファイルの順に、それぞれ別のプロセスで起動します。 |
| 5 | 必須の participant がそろったら、シミュレーションを始めます。 |
| 6 | シミュレーションが終わったら、すべての participant を終了させ、結果を書きます。 |

手順 1: 照合でエラーがあれば（`--strict` では警告も）、何も起動せずに照合の結果を書き、終了コード 1 です。機種（`variants`）を書いた設定ファイルでは、`--variant <名前>` で選んだ機種を照合し、その機種の実行ファイル、変数の対応、participant で動かします（[simulation-config.md](simulation-config.md) の「機種」）。`--variant` がなければ引数のエラーで、何も起動しません。

手順 2: registry のアドレスは `registry_uri` で、なければ 127.0.0.1 の空いているポートです。標準エラーの最初の行が、その URI です（`[vehicle_sil_launch] registry at silkit://127.0.0.1:<port>`）。`--control` を付けたときは、registry より前に Control Server を起動し、`[vehicle_sil_launch] control server at <address>` の行を書きます。

手順 3: シミュレーションが待つ participant（このドキュメントでは「必須の participant」）は、無効にしていない `required: true` のものです。controller はタイムステップ（`time_step_ms` か、その既定値）で時刻同期に参加し、タイムステップの終わりを止めて、速度、一時停止、ステップ実行を決めます。最速のときも同じで、タイムステップごとに同期が 1 回増えます。

手順 4: `--connect` があると、先につなぐネットワークごとのブリッジを起動し、動き出すのを待ちます（[バスを外につなぐ](#バスを外につなぐ--connect)）。`manual` の participant は起動せず、起動するコマンドを書きます。起動する participant の作業ディレクトリは設定ファイルのあるディレクトリで、標準入力は空です。

手順 5: 始める前に、変数を公開する ECU との接続を待ちます（[ECU の変数との接続](#ecu-の変数との接続control_link)の「始まる前の待ち」。ふつうは待ちません）。`--control` と `--start-paused` を付けたときは、どの participant も時刻 0 のステップを始める前で止まり、`resume`、`step` か `run_until` のコマンドを待ちます（[control-api.md](control-api.md) の「始まる前に書く」）。`stop_at_ms` があれば、仮想時間がそこに達したところで止めます。なければ、participant が止めるか、Ctrl+C まで動きます。

手順 6: participant を終了させる順序は[止め方](#止め方)にあります。

### シナリオを付けた実行（`--scenario`）

`--scenario <file>` を付けると、照合でシナリオも確認し、始まった後はシナリオの手順を進めて、シナリオが終わった所でシミュレーションを止めます（[scenario.md](scenario.md)）。

| 項目 | `--scenario` を付けたとき |
|---|---|
| controller のタイムステップ | 速度を指定したときの既定と同じです（`time_step_ms` か、ECU のステップがすべて controller のステップから始まるタイムステップ）。 |
| パラメーター（`${name}`）の値 | 設定ファイルの `params`、機種の `params`、`--param <名前>=<値>` で替えられます。 |
| その機種にない変数（`absent: true`）を使う手順 | 飛ばして、結果に表示します。 |
| 機種が無効にした participant の変数を使う手順 | 同じく飛ばして、結果に表示します。 |

飛ばした手順の `at_ms` は null です。確認する手順が全部飛ぶと、照合の警告 `scenario-nothing-checked` です（[scenario.md](scenario.md) の「機種とパラメーター」）。

## 中断する条件とタイムアウト

### 始まる前

| 起きたこと | `run` がすること | 指摘 |
|---|---|---|
| 必須の participant のプロセスが終わった | `start_timeout_ms` を待たずに、開始をやめます。 | `ended-before-start` |
| 必須の participant が、`start_timeout_ms` の間に参加しなかった | 参加しなかったものを全部挙げて、開始をやめます。 | `not-joined` |
| そろった後、開始を待っている間に、必須の participant のプロセスが終わった | すぐに開始をやめます。 | `ended-before-start` |
| そろった後、開始を待っている間に、プロセスは動いたまま接続が切れた | すぐに開始をやめます。 | `simulation-start` |
| そろった後、最初のステップまでの上限を過ぎた | 開始をやめ、最初のステップに着いていない participant と状態をメッセージに挙げます。 | `simulation-start` |
| `--connect` のブリッジを起動できない、`start_timeout_ms` の間に動き出さない、終わった、シミュレーションから抜けた | それ以上は起動しないか、すぐに開始をやめます。 | `connect-failed` |

「開始を待っている間」は、変数を公開する ECU を待つ間と、`--control` と `--start-paused` で時刻 0 のステップの前に止まっている間です。

### 実行中

| 起きたこと | `run` がすること | 指摘 |
|---|---|---|
| 必須の participant のプロセスが、0 でない終了コードかシグナルで終わった | すぐにシミュレーションを中断します。 | `participant-failed` |
| 必須の participant が Error の状態でシミュレーションから抜けた | すぐに中断します。 | `participant-failed` |
| 必須の participant が、ほかの状態でシミュレーションから抜けた | 5 秒たってもシミュレーションが終わらなければ、中断します。 | `participant-failed` |
| `required: false` の participant が終わった | シミュレーションを止めません。 | 0 でない終了コードは警告 |
| `--connect` のブリッジが終わった（終了コードによらない） | すぐに中断します。 | `connect-lost` |
| `--timeout-ms` の実時間で終わらなかった | 中断します。 | `timeout`（エラー） |
| 一時停止していないのに、仮想時間が 10 秒進まない | 標準エラーに 1 行書いて、待ち続けます。 | なし |
| `stop_at_ms` より前に、participant がシミュレーションを止めた | そのまま終わります。 | `ended-before-stop-at`（注記。`--strict` でも失敗にしません） |

participant がシミュレーションから抜けたかどうかは、controller から見て判断します。中断の規則は `vehicle_sil_sim_controller` と `vehicle_sil::Simulation` と同じです。中断されても、ほかの ECU は自分で終わります（終了コード 0）。`required: false` の participant は、始まるときも待ちません。

一時停止している間も、participant の失敗、止める要求、`--timeout-ms` は同じように扱います。

### `start_timeout_ms` の数え方

`start_timeout_ms` は、必須の participant がそろうまでの時間の上限です。

| 場合 | 数え方 |
|---|---|
| そろった後、最初のステップまで | `start_timeout_ms` の残りが上限です（少なくとも、変数を公開する ECU を待つ時間 + 1 秒）。 |
| `--control` と `--start-paused` で、時刻 0 のステップの前に止まっている間 | 数えません。 |
| その停止から進んだ後 | 改めて `start_timeout_ms` が上限です。 |
| manual の必須の participant が参加するまで | 上限がありません。 |
| 全員が参加した後、manual の participant が最初のステップの準備を終えるまで | 上限がありません。 |
| `run` が起動した participant が参加するまで（manual の participant がいる実行） | `start_timeout_ms` が上限です。 |
| manual の participant の準備が終わった後 | `start_timeout_ms` が上限です。 |

manual は、利用者が自分で起動する participant です（[participant を自分で起動する](simulation-config.md#participant-を自分で起動するmanual--manual)）。

### `--timeout-ms` の数え方

`--timeout-ms <ms>` を付けると、シミュレーションが始まってから `<ms>` の実時間で終わらなければ中断し、エラー（`timeout`）にします。既定は上限なしです。ステップの中で止まった ECU がいると仮想時間が進まず、`stop_at_ms` があっても終わらないので、CI などではこの引数を使います。

| 場合 | 数え方 |
|---|---|
| ふつうの実行 | シミュレーションが始まってから数えます。 |
| 一時停止している時間 | 数えます。 |
| `--control` と `--start-paused` | 時刻 0 のステップの前に止まった時点から数えます（止まっている時間も数えます）。 |
| manual の必須の participant がいる実行 | participant を待ち始めた時点から数えます（[participant を自分で起動する](simulation-config.md#participant-を自分で起動するmanual--manual)）。 |

`--control` と `--start-paused` で止まったまま上限を過ぎると、時刻 0 のステップを実行してから中断し、`timeout` のメッセージに、最初のステップの前に止まっていたことを書きます。進めた後に過ぎたときは、そのうち何 ms 止まっていたかを書きます。

### 仮想時間が進まないときの表示

実行中に、一時停止していないのに仮想時間が 10 秒進まないと、次の形の行を書きます。

```text
[vehicle_sil_launch] virtual time has not advanced for 10 s (at t=<ms> ms, not paused): ...
```

行には、考えられる原因（ほかの PC の participant のネットワークが切れた、デバッガーで止めている、ステップの中で固まった）と、`--timeout-ms` を挙げます。同じ状態が続けば、60 秒と 10 分にも 1 回ずつ書きます。また進んだら `virtual time advances again: it stood still for <s> s at t=<ms> ms` を書きます。

`run` は中断せず、待ち続けます。この行は指摘にはしません（`--events` では `log` のイベントです）。速度を指定したときは、タイムステップの期限を過ぎた時間で数えます。遅い速度で期限を待つ間は数えません。

ネットワークが切れても、SIL Kit は接続が切れたことに長い間気づきません（Linux では十数分）。

## ECU の変数との接続（`control_link`）

`vehicle_sil_launch run` は、controller の participant の上に、ECU の変数を読み書きする側（`vehicle_sil::control::Hub` と `vehicle_sil::control_silkit::HubTransport`）を持ちます。`ecu_main` で作った ECU は、自分から SIL Kit の publish/subscribe で接続します（[ecu-executable.md](ecu-executable.md) の「変数を公開する」）。どの ECU にも `ecu.connected` があるので、変数を宣言していない ECU も接続します。設定ファイルに書くことはありません。gRPC は使いません。しくみと保証は [internals/control.md](internals/control.md) の「SIL Kit の上のトランスポート」にあります。

この接続を使うのは、`vehicle_sil_launch` の中の Hub です。`run --control <address>`（Control Server を持つビルドだけ）を付けると、Hub を gRPC の Control API としてクライアント（Python、CLI、GUI）に公開します。使い方は [control-api.md](control-api.md) の「ランチャーに接続する」です。

ECU が変数を公開するかどうかは、実行ファイルの `--describe` の出力でわかります（`control`: 通信の方式と、メッセージの形式のバージョン。[ecu-executable.md](ecu-executable.md) の「JSON の形式」）。そのため、公開しない ECU とバージョンの違う ECU は、照合の段階でわかります。

設定ファイルの `participants[].variables`（対応表）があれば、その participant の変数は、対応の後の名前と値で読み書きします（[変数の名前と値を合わせる](simulation-config.md#変数の名前と値を合わせるvariables)）。シナリオと Control API のクライアントが見るのは、対応の後だけです。

### 接続の状態（`state`）

接続の状態は participant ごとに 5 つで、結果の `participants[].control_link` と、イベント `control_link` に表示します。シミュレーションが始まった時点の状態で、後から変わっても書き直しません。`run` が hello を待たない participant（[始まる前の待ち](#始まる前の待ち)）では、始まった直後（全員が最初のステップに着いた時点）の状態です。

| `state` | 意味 | 変数の読み書き | 指摘 |
|---|---|---|---|
| `connected` | hello を受信し、コマンドを受け取れるという返事があった | できる | なし |
| `no_hello` | `--describe` の出力では公開するのに、待っても hello（か返事）が来なかった | この実行ではできない | 警告 `control-link-no-hello`（実行の指摘） |
| `version_mismatch` | メッセージの形式のバージョンが `vehicle_sil_launch` と違う | この実行ではできない | 警告 `control-link-version` |
| `not_offered` | 変数はあるが、`--describe` の出力では公開しない（`--no-control`、`control` のない古い vehicle-sil の実行ファイル） | できない | 注記 `control-link-not-offered`（照合の指摘） |
| `none` | 接続するものがない（変数がない古い vehicle-sil の実行ファイル、`describe: false`） | できない | なし |

- `version_mismatch` のバージョンは、`--describe` の出力のものか、受信した hello のものです。前者でわかれば照合の指摘、hello で初めてわかれば実行の指摘です。
- `not_offered` の ECU は待ちません。
- `--describe` の出力では公開しないのに hello を受信した participant は、`connected` です。
- どの状態でも、シミュレーションは動きます（`--strict` では警告が失敗です）。
- `vehicle_sil_launch` が変数を読み書きできるのは、メッセージの形式のバージョンが自分と同じ ECU の実行ファイルだけです。vehicle-sil 0.1.0 でビルドした ECU と新しいランチャー、新しい ECU と 0.1.0 のランチャーの組み合わせでは、ECU はシミュレーションに加わりますが、変数を読み書きできません。照合が `control-link-version` を出します。

### 始まる前の待ち

必須の participant がそろうと、全員が最初のステップの前で待ちます。その間に `run` は、次の条件を全部満たす ECU の hello と、「コマンドを受け取れる」という返事を待ちます。

- `--describe` の出力で、`vehicle_sil_launch` と同じバージョンの変数の接続を持つと言っている。変数がない ECU も含みます（`run` は、同じ接続でネットワークの送受信の数を読みます）。
- 必須で、無効にしていない。

`describe: false` の participant は待ちません。その participant が変数の接続を持つかどうかを、`run` は知る手段がないからです。hello が始まった後に届いた場合、`control_link` は `none` のままですが、`variables` の対応のうち合わないものは同じように指摘します（`variable-map-not-applied`）。

SIL Kit が participant を接続する順序から、hello はこの待ちが始まる前に受信しているので、ふつうは待ちません。接続を持つと言っているのに hello が来ない ECU がいるときだけ、最長 5000 ms 待ちます。待っている間に Ctrl+C か止めるコマンドが来れば、待つのをやめます。そろうのが `start_timeout_ms` の期限の間際だったときは、この待ちが `start_timeout_ms` の後に加わることがあります。

### 実行中の保証

時間を止めたとき（`--start-paused`、`pause`、`step` の後）、止まった仮想時刻 P より前に始まった各 ECU のステップの値と、書き込みの結果は、すべて受信しています。ちょうど P に始まるステップの分は、止まっている間に受信します。これに反して遅れて受信したメッセージがあれば、警告 `control-link-late-values` を出します。`ecu_main` で作った ECU では起きません。数は結果の `control_link.late_messages` にあります。

始まった後に参加した participant（`required: false`、自分で起動した実時間の Peer、同じ名前での起動し直し）は、hello の後、「コマンドを受け取れる」という返事を受信してから書けるようになります（ふつうは一瞬です）。それまでの書き込みは、送らずにすぐ Invalid で確定します。シミュレーションから抜け始めた participant（ライフサイクルが止まり始めた、失敗した）への書き込みも同じです。

### 通信量と認証

値は間引きません。ECU は Agent の `sample_period`（既定 10 ms の仮想時間）ごとに全変数を送信します。ECU が多く、変数が多く、速く動かすほど、量が増えます。重いときは、ECU の `sample_period` を大きくします。

ECU と `vehicle_sil_launch` の間に認証はありません。registry に参加できる者は、ECU の変数に書けます（バスにフレームを送信できるのと同じです）。registry は既定で 127.0.0.1 だけで待ち受けます。

## 速度を指定して動かす（`speed`）

`speed`（設定ファイル）か `run --speed` を指定すると、controller はタイムステップ（`time_step_ms`）ごとに、仮想時間を実時間に合わせます。`speed: 1` では、仮想時間 1000 ms に実時間で 1000 ms 以上かかります。実時間で動く機器と一緒に動かすとき、人が画面を見ながら操作するときに使います。

合わせるために待つだけなので、いちばん遅い participant より速くはなりません。最速（`max`）では合わせず、遅れも数えません。

シミュレーションの外につながる実行（`--connect`、または有効な `role: bridge` の participant）は、ここで指定した速さが何であっても、速さ 1 で動きます。`--time-policy free` を付ければ別です（[外につながる実行の時間](#外につながる実行の時間--time-policy)）。

実時間で動く機器は、シミュレーションを待ちません。シミュレーションが実時間に遅れると、機器から見た結果が変わります。そのため、遅れを次の定義で数えて報告します。

### 遅れの定義

| 言葉 | 意味 |
|---|---|
| 期限 | タイムステップ（仮想時刻 T に始まる controller のステップ）を始めてよい実時間 |
| 遅れ | タイムステップが実際に始まった実時間から、期限を引いたもの（負なら 0） |
| 遅れたタイムステップ | 遅れが `late_tolerance_ms`（既定は実時間の 10 ms）を超えたタイムステップ |
| ずれ | 遅れたタイムステップの遅れ（合計が `slipped_ms`） |

期限は、「基準の実時間 +（T − 基準の仮想時刻）÷ 速度」で決まります。基準は最初のタイムステップでとり、再開のたびにとり直します。途中で速度を数から数へ変えても、次のタイムステップの期限は動きません。どのタイムステップも、期限より前には始まりません。

タイムステップが始まるのは、controller が前のタイムステップを放し、そのタイムステップより前に始まる participant のステップが全部終わったと controller が知ったときです。遅れには、controller 自身が起きるのが遅れた分も入ります。機器からは区別がつかないためです。

遅れたタイムステップの後、シミュレーションは追いつこうとしません。そのタイムステップを新しい基準にして、そこから指定の速度で進みます。それ以後の仮想時間は、遅れた分だけ後ろにずれたままです。許容の内の遅れでは基準を変えないので、次のタイムステップで自然に取り戻します。

数える範囲は、基準をとった次のタイムステップから、最後のタイムステップ（`stop_at_ms` のタイムステップを含みます）までです。

| 場合 | 数え方 |
|---|---|
| 期限を過ぎても、まだ始まっていないタイムステップ | そのときまでの遅れで数えます。 |
| participant が止まったまま、時間切れ、失敗、停止で実行が終わった | その遅れは結果に残ります。 |
| 一時停止していた時間 | 数えません。 |
| 最速（`max`）の間 | どのタイムステップも遅れません。 |
| 最速にした時点で期限を過ぎていたタイムステップ | そのときの遅れを保ち、始まったときに 1 回だけ数えます（最速にしていた間の実時間は遅れに入りません）。 |
| そのタイムステップが始まる前に、数の速度に戻した | そのタイムステップの期限は「戻した時刻 − 保っていた遅れ」です。 |

### 遅れたときに、実時間で動く機器から見て起きること

あるタイムステップが 490 ms 遅れると、それ以後のメッセージは、すべて 490 ms 遅れて出ます。周期は変わりません（10 ms 周期のメッセージは、その後も 10 ms おきに出ます）。追いつこうとしないのは、追いつくと、たまった分（この例では約 49 周期分）のメッセージが数 ms の間にまとめて出て、受信する側の周期やタイムアウトの判定を乱すためです。

遅れたタイムステップの数は、遅れが起きた回数です。1 回の遅いステップは、どれだけ長くても遅れたタイムステップ 1 つです。ECU のステップがいつも実時間に間に合わないシミュレーションは、ずれ続けます。全体でどれだけ遅れたかは、`slipped_ms` と `actual_speed` でわかります（かかった実時間は、仮想時間 ÷ 速度 に `slipped_ms` を足したものより短くなりません）。

1 つのタイムステップの遅れ（`max_late_ms`）は、大きめに出ることがあります。participant があるステップを終えてすぐ次のステップに入ると、終わったことは次のステップの後で controller に伝わります。controller はその分（最大で、その participant の 1 ステップにかかる実時間）だけ遅く知ります。小さく出ることはありません。

そのため、ステップの計算が続けてステップの枠（ステップ幅 ÷ 速度 の実時間）をはみ出す ECU では、許容が、次の 1 ステップの計算時間の分だけ小さくなります。大きめに測った遅れが許容を超えると、そのタイムステップは遅れたタイムステップになり、基準をとり直すので、以後は実際にその分だけずれます。こういう ECU があるときは、`late_tolerance_ms` を「機器が待てる時間 + ECU の 1 ステップの計算時間」を目安に決めます。

### タイムステップと ECU のステップ

合わせるのはタイムステップの境目だけです。仮想時刻 T に始まる ECU のステップは、T 以後の最初のタイムステップの期限に動きます。タイムステップの間に始まるステップは、次のタイムステップの期限に、まとめて動きます。T にあたる実時間より遅くなり、早くはなりません。

遅くなる量は、最大で「タイムステップ −（タイムステップと ECU のステップ幅の最大公約数）」です。この量は遅れの数には入りません。結果の `timing` の `step_delay_ms` にあります。

| タイムステップ | ECU のステップ幅 | 遅くなる量の最大 |
|---|---|---|
| 10 ms | 1 ms | 9 ms |
| 10 ms | 15 ms | 5 ms |
| 10 ms | タイムステップの倍数 | 0 |

そのため、速度を指定して `time_step_ms` を書かなければ、タイムステップは、10 ms、`stop_at_ms`、ECU のステップ幅の最大公約数になります。ECU のステップ幅は、無効にしていない ECU の `--describe` の出力を照合で読んだものです。

| ECU のステップ幅 | タイムステップ |
|---|---|
| 10 ms と 5 ms | 5 ms |
| 4 ms と 6 ms | 2 ms |
| 20 ms だけ | 10 ms |
| ステップ幅がわからない participant（`describe: false`）がある | 1 ms |
| 最大公約数が 1 ms の倍数でない | 1 ms |

`time_step_ms` を書いたときは、その値を使います。速度の指定があり、ECU のステップがタイムステップの境目で始まらないときは、警告 `coarse-time-step` を出します。警告には、最大で何 ms 遅れてまとまるかと、何と書けばよいかを書きます。

| 場合 | `coarse-time-step` を出す時点 |
|---|---|
| 設定ファイルに `speed` も書いてある | `check` の時点（`run` ではその 1 つだけ） |
| `run --speed` で速度を変えた | `run` が、変えた速度で判断します。 |
| 最速で始めて、途中で `speed` のコマンドを受けた | そのとき（タイムステップは始めに決まり、途中で変わりません） |

タイムステップを細かくすると、時刻同期の回数が増えます。最速のときの既定が 10 ms のままなのは、そのためです。

### 遅れたときの警告と対処

遅れたタイムステップが 1 つでもあれば、警告 `behind-real-time` を出します（`--strict` では失敗）。数は、結果の `timing`、人向けの出力の `time:` の行、`--events` の `time` にあります。

遅れたときは、速度を下げるか、遅い原因を探します。

| 原因 | 対処 |
|---|---|
| 1 ステップの計算が重い ECU がある | そのステップを軽くします（ステップ幅を大きくしても、1 ステップが重ければ遅れは減りません）。 |
| 小さいステップの ECU が多くて、同期の回数が多い | ECU のステップ幅と `time_step_ms` を大きくします。 |
| どの participant が遅いかわからない | ECU を 1 つずつ無効にして（`--disable`）比べます。 |

`vehicle_sil_launch` は、どの participant が遅いかを知りません。SIL Kit は、各 participant のステップにかかった時間を controller に伝えないためです。

`late_tolerance_ms` の既定の 10 ms は、実時間で動く機器のタイムアウトに比べて十分小さく、OS のタイマーの揺れより十分大きい値として選んでいます。実時間の長さで、タイムステップと速度によりません。機器が待てる時間に合わせて変えます。

### 一時停止、ステップ実行、速度の変更

一時停止、ステップ実行、途中での速度の変更は、標準入力のコマンドと、Control Server（`--control`）のクライアントの呼び出しでします（[標準入力のコマンド](simulation-events.md#標準入力のコマンドrun---stdin-commands)）。

`run --start-paused` は、全員がそろったら仮想時間 0 で止まり、`resume` か `step` を待ちます。`--stdin-commands` か `--control` と一緒に使います。

| 一緒に付けた引数 | 止まる所 |
|---|---|
| `--stdin-commands` だけ | 時刻 0 のステップを実行してから止まります。 |
| `--control` がある | 時刻 0 のステップの前で止まります。 |

`--connect` があると、実機のバスの機器はシミュレーションと一緒に止まらず、速さも変わりません。`--time-policy free` を付けなければ、`--start-paused` と 1 以外の `--speed` は断られます。実行中に止める、ステップで進める、速さを変えるコマンドも同じです（[外につながる実行の時間](#外につながる実行の時間--time-policy)）。

## バスを外につなぐ（`--connect`）

`run --connect <network>=<target>` は、このリポジトリのブリッジで、ネットワークを実機のバス（Ethernet のアダプター、Vector XL Driver Library の CAN チャンネル）につなぎます。まず、この PC で各ネットワークをどこにつなげるかを見て、出力の `--connect` の行を写します（[つなげる先の一覧](#つなげる先の一覧vehicle_sil_launch-connections)）。

```sh
vehicle_sil_launch connections simulation.yaml
vehicle_sil_launch run --speed 1 --connect "ETH_EXT=vEthernet (A)" simulation.yaml
```

照合は、`check --connect` と同じように、ブリッジを探して `--describe` を走らせます（simulation-config.md の「[`--connect`](simulation-config.md#--connect)」）。照合が通ると、`run` は次のように進めます。

| 順 | `run` がすること |
|---|---|
| 1 | つなぐネットワークごとのブリッジを、participant `bridge.<network>` として、ファイルのネットワークの順に起動します。引数は、照合が出すコマンド（`check --json` の `networks[].outside.command`）と、`--name`、`--registry-uri`、`--start-timeout-ms`、`--stats-every-ms 1000` です。 |
| 2 | 各ブリッジが SIL Kit の中で動き出すのを、`start_timeout_ms` まで待ちます。ブリッジは実時間で動き、足並みをそろえた開始には加わりません。 |
| 3 | ファイルの participant を、`--connect` がないときと同じように起動します。 |
| 4 | シミュレーションが終わったら、ほかの `required: false` の participant と一緒に、ブリッジに止まるよう頼みます（[止め方](#止め方)）。 |

ブリッジを先に起動するのは、ブリッジが加わる前に ECU が送ったフレーム（時刻 0 のアドレスの宣言など）が、外のバスに出ないためです。

ブリッジはファイルの participant ではありません。結果（`participants[]` のファイルの participant の後で、`for_network` 付き。`outside[]`）とイベント（`started`、`joined`、`ended`、ブリッジの行）には、ほかの participant と同じように出ます。ただし統計の行は別です（下記）。`--disable`、`--enable`、`--manual` にはその名前を書けません。ネットワークを SIL Kit の中だけで動かすなら、`--connect <network>=none` を渡すか、`--connect` を外します。

| ブリッジに起きたこと | `run` がすること | 指摘 |
|---|---|---|
| シミュレーションが始まる前に、起動できない、終わった、シミュレーションから抜けた（手で起動する participant を待つ間と、`--start-paused` と `--control` で最初のステップの前に止まっている間も） | それ以上は起動せず、起動したものを終わらせます。 | `connect-failed` |
| `start_timeout_ms` の間に SIL Kit の中で動き出さない | 同じです。 | `connect-failed` |
| シミュレーションの実行中に終わった（終了コード 0 でも、一時停止中でも、`--control` のない `--start-paused` の時刻 0 で止まっている間でも） | すぐにシミュレーションを中断します。 | `connect-lost` |
| シミュレーションが終わったとき、`run` が止まるよう頼む前に、もう自分で終わっていた | いつもどおり実行を終えます。 | `connect-lost` |
| ほかの理由でランチャーがシミュレーションを中止した後に、自分で終わった（中止は SIL Kit からブリッジにも届きます） | それ以上は何もしません。理由はほかの指摘が言います。 | なし（`participants[]` に終わり方があります） |

指摘の文は、ブリッジがどう終わったかと、ブリッジが理由を書いた行（`[eth-bridge]` などの札の後が `error:` か `FAIL` で始まる最後の行）を出します。強制終了されたブリッジと、そういう行を書かずに終わったブリッジでは、行を出しません（最後の行は `stopped` など、ほかのことだからです）。`run` はブリッジのプロセスを見ているので、SIL Kit につながる前に終わったブリッジは、`start_timeout_ms` を待たずに、すぐ開始を失敗にします。

実機のバスは仮想時間を待ちません。下の[外につながる実行の時間](#外につながる実行の時間--time-policy)を見てください。

### 外につながる実行の時間（`--time-policy`）

次のどちらかに当てはまる実行は、シミュレーションの外につながっています。

- `--connect` でネットワークをつなぐ（先が `none` でないもの）。
- ファイルに、有効な `role: bridge` の participant がいる（`describe: false` でも）。無効にした participant（`enabled: false`、`--disable`）は数えません。

外につながる相手は仮想時間を待たないので、こういう実行は、既定で実時間を保ちます。`--time-policy` で選びます。

| `--time-policy` | 実行の速さ | 1 以外の `--speed`、`--start-paused` |
|---|---|---|
| `realtime`（既定） | ファイルの `speed` が何であっても 1 です。ファイルの速さが 1 でないとき（速さがないときも）、注記 `outside-speed-set` が知らせます。 | 照合のエラー `outside-real-time` です。何も起動しません。 |
| `free` | ほかの実行と同じです。`--speed`、なければファイルの `speed`、それもなければ最速です。 | 受け付けます。注記 `outside-not-real-time` が、実時間でないことを知らせます。 |

- 外につながっていない実行では、`--time-policy` は何もしません。
- `check`、`run`、`test` で使えます。`check` はファイルの `speed` で、`run` は `--speed` と `--start-paused` も見て判断します。`check --json` の `time_lock` に、この判断があります（[simulation-check.md](simulation-check.md#json--json)）。
- `test` は各シナリオを最速で実行しますが、ファイルに有効な `role: bridge` の participant がいるときは速さ 1 で実行します（注記 `outside-speed-set`）。このとき `test` の表の後に、注記 `test-speed-set` を全体で 1 回書きます。それでも最速で実行するなら、`--time-policy free` を付けるか、ブリッジを `--disable` します。
- `free` が変えるのは、時間を操作できるかどうかだけです。ブリッジは両方向にフレームを運び続けます。シミュレーションが止まっている間、外から来たフレームは溜まり、ECU がそれまでに送ったものは送られたままです。
- 注記は、`--strict` でも実行を失敗にしません。
- 自分で作った実時間の participant（ステップのない ECU や `role: test` の participant）は、実行を実時間に固定しません。

実行中のコマンド（標準入力、Control Server のクライアント、GUI）も、同じ選択に従います。

| コマンド | `realtime`（既定） | `free` |
|---|---|---|
| `pause`、`step`、`run_until`、1 以外の `speed`（`max` も） | 理由 `outside-real-time` で断ります。何も変わりません。 | 受け付けます。実行は、注記 `outside-time-changed` を付けて終わります。 |
| `resume`、`speed` 1 | 受け付けます（何も変わりません）。 | 受け付けます。 |
| `stop` | 受け付けます。 | 受け付けます。 |

- シナリオのない実行では、答えは、どの入口からでも同じです。`command` イベントは `accepted: false` と `reason: "outside-real-time"` になり、Control Server のクライアントには、その理由の付いた `FAILED_PRECONDITION` が返ります（[control-api.md](control-api.md) の「外につながる実行」）。`--events` がなければ、断った標準入力の行は標準エラーの 1 行になります。メッセージに、どうすればよいかがあります（`--time-policy free` で始め直すか、外につながずに実行する）。
- 送る前に、`state` イベントで分かります。`realtime` では、`allowed.pause`、`allowed.step`、`allowed.run_until` が、この理由の `ok: false` になり、`allowed.speed` の `min` と `max` が 1 になります。`free` では `ok: true` で、コード `outside-not-real-time` の `caution` が付きます（[simulation-events.md](simulation-events.md) の「`state` のイベント」）。
- シナリオつきの実行（`--scenario`）では、標準入力のコマンドがこの表に従います。Control Server のクライアントは、シナリオが終わるまで、読むことと `Stop` だけができます。実行はシナリオとともに終わります。時間の呼び出しは、`--time-policy` が何であっても、`outside-real-time` でなく、理由 `scenario-running` で拒否します（[control-api.md](control-api.md) の「シナリオの実行中」）。
- ランチャーが構成ファイルを読む前に届いたコマンドは待たされ、ファイルを読んだ後に答えが出ます。早く送っても、この規則をすり抜けません（[simulation-events.md](simulation-events.md) の「時間のコマンドを受け付ける条件」）。
- `outside-time-changed` は、実行の終わりに 1 回書きます。受け付けたコマンドのうち、シミュレーションを止めるかステップで進めるか、1 以外の速さにした最初の 1 つについて、コマンドの名前と、どこから来たかと、来たときの仮想時間を書きます（コマンドはどれも `command` イベントにあります）。`resume`、速さ 1、`--start-paused` そのものは数えません。

自分で作った実時間の participant も、シミュレーションが止まっている間、止まりません。そういう participant（`--describe` の出力にステップのない ECU や `role: test` の participant）のいる実行では、最初のそういうコマンドについて、実行の終わりに注記 `unsynchronized-time-changed` を書きます。その participant がそれ以後に送ったものと見たものは仮想時間に合わず、同じコマンドでもう一度実行しても、同じ動きになるとは限りません。`describe: false` の participant と、ファイルにないまま参加した participant は、ランチャーには分からないので、注記は出ません。

### 外のバスとやりとりしたフレームの数

`run` は、つないだネットワークごとに、ブリッジがバスへ送ったフレームと、バスの機器から来たフレームを数えます。数はブリッジの統計の行（`stats ...`。[ethernet-bridge.md](ethernet-bridge.md#統計) と [bridges.md](bridges.md#統計) の「統計」）から読みます。`--stats-every-ms 1000` で、ブリッジは動いている間もこの行を書きます（数が変わったときに 1 秒に 1 回と、止まるときに 1 回）。

| ネットワーク | `to_outside` | `from_outside` |
|---|---|---|
| Ethernet | `silkit_to_ext` | `ext_from_devices`: 送り元がこの PC でないフレーム。ブリッジの絞り込みの前に数える |
| CAN | `silkit_to_xl` | `xl_to_silkit` |

- 数は結果（`outside[]`。人向けの結果の `outside:` の行、例えば `outside: ETH_EXT -> vEthernet (A) (bridge.ETH_EXT): 152 frames out, 3 from devices`）と、`--events` なら実行中の `outside` イベントに出ます（[simulation-events.md](simulation-events.md#つないだネットワークの数outside) の「つないだネットワークの数（`outside`）」）。`--events` があってもなくても同じ数です。
- ブリッジが動いている間に `run` が読んだ統計の行は、ブリッジの行（`[bridge.<network>] ...`）にも `log` イベントにも出しません。ブリッジが終わったら、`run` は最後に読んだ 1 行を、ブリッジの行として 1 回だけ出します。全部の数が載ったブリッジの報告です（例えば、絞り込みで ECU に渡さなかったフレーム `filtered_from_ext`、止めるときに捨てたフレーム `xl_tx_unsent`）。ブリッジが止まるときに書いた行か、それを書かずに終わらされたなら、動いている間の最後の行です。ブリッジの `--log-dir` のファイルには全部の行があります。`run` が読めない行（例えば、これらのキーのない別の版のブリッジの行）は、ふつうの行として出します。
- 数は最後に読めた行のものです。最後の行を書かずに終わらされたブリッジ（ランチャーが止まるよう頼めなかった Windows）は、動いている間の最後の行の数になります。1 行も読めなければ、どちらも `null` です。

シミュレーションが動き、ブリッジがバスへフレームを送ったのに、実行が終わるまでにそこの機器から 1 つも来なかったら、実行は注意 `connect-nothing-received` で終わります。機器が答えないのは不具合とは限らないので、注意です（`--strict` でも失敗にしません）。実行を失敗させたブリッジ（`connect-failed`、`connect-lost`）と、数が `null` のときは出しません。Ethernet では、この PC がそのアダプターに出すフレーム（例えば、Hyper-V の `vEthernet` のアダプターに OS が出す IPv6 と ARP のフレーム）は数えないので、ケーブルが抜けていればこの注意が出ます。ブリッジも、動いている間に、最初に送ってから 10 秒たって機器から何も来なければ警告します（[ethernet-bridge.md](ethernet-bridge.md#機器から何も来ないときのヒント) の「機器から何も来ないときのヒント」）。

```text
network 'ETH_EXT': nothing came from a device on the real bus (vEthernet (A)) before the run ended: bridge.ETH_EXT sent 152 frames there and got none from a device (frames that this computer sent do not count). If a device is meant to answer, check that it is on, its cable, and that --connect names the adapter it is on; a short run can end before a device answers
```

バスに機器をつないだ自動の試験は、シナリオ 1 つを、速さ 1 と `--strict` で走らせます。`test` は `--connect` を受けません。`test` は、実行を 1 つずつ続けて最速で走らせるためのもので、実際のバスは仮想時間を待たないためです。

```sh
vehicle_sil_launch run --scenario <scenario> --speed 1 --strict --connect <network>=<target> <file>
```

### リンクとバスの状態

走っている間、`run` はブリッジのほかの行も読みます。リンクの行（Ethernet のブリッジの `link link=up|down|unknown`、CAN のブリッジの `link bus=active|passive|bus-off|unknown`。[bridges.md](bridges.md#リンクの行) と [ethernet-bridge.md](ethernet-bridge.md#リンクの行) の「リンクの行」）と、統計の行の `key=N` のすべて（上で数える 2 つだけではありません）です。知らないキーと値は読み飛ばします。リンクの行も、ブリッジの行としては出しません。変わったときは、実行の始まりからの時間を付けたランチャーの 1 行にし、`--events` では `log` イベントにします。

```text
outside: ETH_EXT -> enp3s0: link down at 12.40 s
outside: ETH_EXT -> enp3s0: link up at 15.10 s
```

ブリッジの最初の行は、正常（`up`、`active`）なら何も言いません。`run` は、トラブルを 3 つに分けます。

| 種類 | `run` がすること | 終わりに |
|---|---|---|
| リンクやバスがしばらく落ちていて（`link=down`、`bus=bus-off`）、ブリッジは動いている | 続けます。30 秒続いたら（`RunOptions::outside_lost_after`）、ブリッジが終わったときと同じに、シミュレーションを中断します。抜いたケーブルの差し直しや機器の再起動は数秒ですが、つなぐつもりの実行が、バスなしで最後まで走ってはいけないからです。 | 警告（`outside-link-down`、`outside-bus-off`。回数と合計の時間）。30 秒のときは誤り `connect-lost`（「link down for 30 s」「bus off for 30 s」） |
| つながっているが、フレームが失われた | 続けます | 起きた種類ごとの警告（下）。1 フレームでも出します |
| ブリッジが続けられない | シミュレーションを中断します | 誤り `connect-lost`（上） |

失われた種類は、統計の行の数とリンクの行から見分けます。キーの表はランチャーに 1 つだけあります（`detail::kOutsideDroppedKeys`）。

| 所見 | 元 |
|---|---|
| `outside-frames-dropped` | 次の数のどれかが 0 でない。`ext_overruns`、`ext_queue_full`、`ext_tx_failures`、`silkit_tx_failures`（Ethernet）、`overruns`、`xl_queue_full`、`xl_tx_failures`、`xl_tx_dropped`、`xl_tx_unsent`、`silkit_rejected`、`xl_rejected`、`silkit_tx_failures`（CAN）。文は 0 でない数のそれぞれを挙げます。 |
| `outside-loop-suspected` | `loop_suspected` が 0 でない。Ethernet のブリッジが両側にある送信元 MAC アドレスを見つけ、それ以後、そのフレームを捨てている。 |
| `outside-bus-errors` | CAN のブリッジが `bus=passive`（error passive）を報告した。文に合計の時間と `error_frames` を書きます。 |

ほかの数（`own_tx_dropped`、`filtered_to_ext`、`filtered_from_ext`、`mac_unguarded`、`oversize`、`malformed`、`ext_checksum_incomplete`）はブリッジの失った分ではないので、所見にしません。結果には、ブリッジの書いたとおりに入ります。文は、何が起きたか、結果を信じてよいか、何を確かめるかを書きます。例: `network 'ETH_EXT' -> enp3s0: the link was down once, for 2.70 s in all. The run went on, but frames sent meanwhile did not reach the devices, ...`。この所見は、それまでに読んだものから、終わりに作ります。

### 別の実行も使っているデバイス

ブリッジは、開いたデバイスに印を置き、同じデバイスにいるほかの vehicle-sil のブリッジの数を報告します（`shared`。Linux では `peers` にそのプロセス ID。「[別のブリッジが使っているデバイス](bridges.md#別のブリッジが使っているデバイス)」）。1 つのデバイスに 2 つの実行、あるいは前の実行の残りのブリッジがいると、フレームが混ざります。この実行の機器にもう一方向けのフレームが届き、もう一方にはこの実行のフレームが届きます。`run` は、自分の実行のブリッジを数から引くので、この実行の外のブリッジだけが数に入ります。見つけたら、実行の始まりからの時間と、Linux ではプロセス ID を付けて、1 度だけ知らせます。

```text
outside: CAN1 -> can0: also used by another vehicle-sil bridge (process 4321) at 3.10 s
```

ブリッジ自身の警告（`warning: ... is also used by another vehicle-sil bridge ...`）は、リンクの行と同じく、実行の出力には出しません。ブリッジのログファイル（`--log-dir`）には残ります。

| いつ | `--strict` なし | `--strict` あり |
|---|---|---|
| ブリッジが起きた時点で、デバイスが共有されている | 実行は続きます | `run` は ECU を起こさずに実行を終わらせます。誤り `outside-device-shared`、結果は `failed`。各ブリッジの最初のリンクの行（ブリッジのライフサイクルの前に出ます）を 2 秒まで待ちます（`kOutsideFirstLineWait`）。その間に書かなかったブリッジは、何も共有していないものとして扱います |
| シミュレーションの実行中に始まった | 実行は続きます | 3 秒続いたら（`kOutsideSharedAfter`）、ブリッジが終わったときと同じに、`run` がシミュレーションを中断します。誤り `outside-device-shared`。別の実行が起こして、3 秒以内にまたいなくなったブリッジでは止まりません。ECU の前にやめる実行が、もう一方を巻き添えにしないためです |
| 終わりに | 警告 `outside-device-shared`。プロセス ID と、最初に見つけた時刻 | 同じ警告で、`--strict` はそれを失敗にします。`run` が止めたときは誤り |

文は、何が起きるか（両方のフレームが混ざるので、実行が機器について言うことを信じられない）と、どうするか（実行ごとに別のデバイスにつなぐ、前の実行の残りのブリッジを止める）を言います。

この実行の 2 つのネットワークが、`--connect` の値は違う（それでチェックが見つけられない）のに、ブリッジが同じデバイスを報告したときは、`run` はチェックと同じ形の `connect-target-shared`（「the same device under two names」）で警告します。同じ先を 2 度書いたときはチェックの警告です（[simulation-check.md](simulation-check.md)）。`run` は繰り返しません。

### つなげる先の一覧（`vehicle_sil_launch connections`）

```text
vehicle_sil_launch connections [--json] [--network <name>] [--] <file>
```

`connections` は、ファイルの `can` と `ethernet` のネットワークごとに、この PC でどこにつなげるかを出します。ネットワークのブリッジに一覧を聞き（`vehicle_sil_ethernet_bridge --list-interfaces --json`、`vehicle_sil_vector_xl_bridge --list-channels --json`）、ブリッジは `--connect` と同じように探します（`vehicle_sil_launch` の隣、次に `PATH` の絶対パスのディレクトリ）。どこにもつなぎません。SIL Kit も participant も起動せず、ブリッジはアダプターもチャンネルも開きません。同じ種類のネットワークが 2 つあっても、各ブリッジに聞くのは 1 回です。

- `--network <name>` は、そのネットワークだけを出します。ファイルにない名前は使い方の誤りです。
- ファイルは機種なしで読みます。どの機種もファイルのネットワークを持つからです。`--variant` は使い方の誤りです。
- ファイルの誤りでは、一覧は止まりません。読めたネットワークを出し、その後に、`check` と同じファイルの誤りを出します（終了コード 1）。
- ランチャーは、各ブリッジを、`--connect` と同じく、ブリッジ自身のディレクトリで起動します（simulation-config.md の「[`--connect`](simulation-config.md#--connect)」）。

Windows で、Vector XL のブリッジが入っていないときの例です。

```text
simulation.yaml: where each network can be connected on this computer
ETH_EXT (ethernet; vehicle_sil_ethernet_bridge, npcap: Npcap version 1.80, based on libpcap version 1.10.5):
  --connect "ETH_EXT=vEthernet (A)"  up, connected; Hyper-V Virtual Ethernet Adapter; \Device\NPF_{0A1B2C3D-0000-1111-2222-333344445555}
  --connect "ETH_EXT=Ethernet 2"     up, disconnected; Intel(R) Ethernet; \Device\NPF_{4E5F6A7B-0000-1111-2222-333344445555}
  cannot be used: \Device\NPF_Loopback  (loopback: not an Ethernet adapter)
J1939_CAN1 (can; vehicle_sil_vector_xl_bridge):
  cannot list: vehicle_sil_vector_xl_bridge is neither next to vehicle_sil_launch (C:/vsil/bin) nor on PATH: build it with the Vector XL Driver Library (docs/bridges.md) and put it next to vehicle_sil_launch, or its directory on PATH [tool-missing]
BODY_LIN (lin): SIL Kit only (there is no bridge for LIN)
```

| 行 | 意味 |
|---|---|
| `<network> (<kind>; <bridge>, <backend>: <library>):` | ネットワークと、そのブリッジ、ブリッジが言う backend とライブラリーの版です。 |
| `--connect ...` | ブリッジが使える先です。引数を `check` か `run` に写します。後ろは、名前が違えば先の名前と、ブリッジがその先について言うことです。 |
| `cannot be used: <target>  (<reason>)` | ブリッジが一覧に出すが使えない先（ループバック）か、`--connect` が受けない先です。SIL Kit だけを意味する `none`（理由は、代わりに書く pcap の名前か、ほかの名前のないインターフェースでは名前を変えることを言います）、`-` で始まる名前、そのほか `connect-target` が断る値です。 |
| `cannot be connected with --connect: ...` | ネットワークの名前に `=` があるか、`--` で始まるので、`--connect` でそのネットワークを指せません。ネットワークの名前を変えます。 |
| `note: ...` | ブリッジが足すこと（ネットワークのアダプターを出さない Npcap）か、sh 向けにだけ引用した先（下）です。 |
| `cannot list: <message> [<code>]` | 一覧を作れませんでした（下の code）。 |
| `nothing listed: ...` | ブリッジがこの PC で何も出しません。 |
| `<network> (<kind>): SIL Kit only (<why>)` | この PC にはそのネットワークのブリッジがありません。LIN のブリッジはなく、`can` のネットワークの Vector XL のブリッジは Windows でだけ動きます。動かない OS でも、見つかったブリッジ（試験用のビルド）は使います。 |
| `<network>: its kind in the file has an error` | 何が誤りかは `check` が言います。 |

`--connect` の行の引数は、sh（bash、zsh）、PowerShell、cmd のどれでも同じに読める書き方です。ASCII の英字、数字、`_ - . : / = +` だけならそのまま、それ以外は二重引用符で囲みます。`"`、`$`、`` ` ``、`%`、`!`、引用符 `“` `”` `„`（U+201C、U+201D、U+201E。PowerShell は `"` と同じに読みます）、制御文字を含む先と、`\` が最後にあるか `"`、`$`、`` ` ``、`\` の前にある先は、全部に通じる書き方がありません。その行は sh 向け（`'...'`）に書き、`note:` の行でそう言います。

#### `connections --json`

```json
{
  "vehicle_sil_connections": 1,
  "vehicle_sil": "<version>",
  "file": "simulation.yaml",
  "result": "listed",
  "networks": [
    {
      "name": "ETH_EXT",
      "kind": "ethernet",
      "tool": "vehicle_sil_ethernet_bridge",
      "tool_path": "C:/vsil/bin/vehicle_sil_ethernet_bridge.exe",
      "list": {
        "vehicle_sil_targets": 1,
        "tool": "vehicle_sil_ethernet_bridge",
        "backend": "npcap",
        "source": "Npcap version 1.80, based on libpcap version 1.10.5",
        "targets": [
          {
            "value": "vEthernet (A)",
            "label": "vEthernet (A)",
            "detail": "up, connected; Hyper-V Virtual Ethernet Adapter; \\Device\\NPF_{0A1B2C3D-0000-1111-2222-333344445555}",
            "usable": true,
            "reason": null,
            "in_use": false,
            "in_use_by": [],
            "in_use_count": 0,
            "ethernet": {
              "pcap_name": "\\Device\\NPF_{0A1B2C3D-0000-1111-2222-333344445555}",
              "connection": "vEthernet (A)",
              "description": "Hyper-V Virtual Ethernet Adapter",
              "up": true,
              "connected": true,
              "loopback": false,
              "wireless": false
            }
          }
        ],
        "notes": [],
        "error": null
      }
    },
    {
      "name": "J1939_CAN1",
      "kind": "can",
      "tool": "vehicle_sil_vector_xl_bridge",
      "tool_path": null,
      "list": {
        "vehicle_sil_targets": 1,
        "tool": "vehicle_sil_vector_xl_bridge",
        "backend": null,
        "source": null,
        "targets": [],
        "notes": [],
        "error": {
          "code": "tool-missing",
          "message": "vehicle_sil_vector_xl_bridge is neither next to vehicle_sil_launch (C:/vsil/bin) nor on PATH: build it with the Vector XL Driver Library (docs/bridges.md) and put it next to vehicle_sil_launch, or its directory on PATH"
        }
      }
    },
    {
      "name": "BODY_LIN",
      "kind": "lin",
      "tool": null,
      "tool_path": null,
      "list": null
    }
  ],
  "findings": []
}
```

| キー | 内容 |
|---|---|
| `vehicle_sil_connections` | この形の版です（1）。 |
| `vehicle_sil` | vehicle-sil の版です。 |
| `file` | 渡されたとおりのファイルです。 |
| `result` | `listed`: この PC にあるブリッジは、すべて一覧を作れました（ないブリッジ `tool-missing` は答えの一部です）。`failed`: あるブリッジが一覧を作れなかったか、ファイルに誤りがあります（ネットワークの種類の誤りも）。`unreadable`: ファイルを読めません。`usage`: コマンドラインの誤りです。 |
| `networks[]` | ファイルの順のネットワークです。`--network` があれば、その 1 つです。 |
| `networks[].name` | ネットワークの名前です。 |
| `networks[].kind` | `can`、`ethernet`、`lin`。ファイルの種類に誤りがあれば `null` です。 |
| `networks[].tool` | ブリッジの実行ファイルです。この PC にそのネットワークのブリッジがないもの（`lin`、Windows 以外の `can`）と、種類の分からないものは `null` です。 |
| `networks[].tool_path` | ランチャーが見つけた場所です。見つからないとき、ブリッジがないときは `null` です。 |
| `networks[].list` | ブリッジが書いたとおりの一覧で、キーはすべてそのまま渡します。形は「[一覧の JSON](ethernet-bridge.md#一覧の-json--json)」と「[チャネルの一覧の JSON](bridges.md#チャネルの一覧の-json--json)」です。一覧を出さないブリッジには、ランチャーが同じ形の一覧を作ります（下）。`tool` が `null` のものは `null` です。 |
| `findings` | ファイルを読めない理由、使い方の誤り、ファイルの誤りです（`check --json` と同じ形）。ほかは空です。 |

`usable: true` の先の `value` は、`--connect <network>=<value>` がそのまま受ける値です（プログラムは、シェルを通さずに 1 つの引数として渡します）。`--connect` が受けない先（上の `cannot be used` と `cannot be connected` の行）は、ランチャーが `usable: false` にして `reason` を入れるので、文と JSON は同じことを言います。ブリッジから一覧を受け取れないときは、ランチャーが、先なしで誤りを入れた一覧を作ります。ブリッジ自身の code（`library-missing`、`permission-denied`、`backend-unavailable`、`driver-missing`、`failed`）は、ブリッジの一覧に入って来ます。

| `error.code` | いつ |
|---|---|
| `tool-missing` | ブリッジが `vehicle_sil_launch` の隣にも `PATH` にもありません。これは答えで、失敗ではありません（終了コードは 0 のまま）。 |
| `tool-cannot-start` | ブリッジを起動できません。Windows では、起動の前に終了状態 `0xC0000135` か `0xC0000139` で終わったときも（DLL がないか、版が違います。Vector XL のブリッジには Vector XL Driver Library の `vxlapi64.dll` が要ります）。 |
| `tool-timeout` | ブリッジが 30 秒の間に終わりませんでした。 |
| `tool-bad-output` | ブリッジが、ランチャーの読める一覧を書きませんでした（ほかの vehicle-sil のブリッジ）。64 段より深い入れ子も。文は、ブリッジの標準エラーの、理由を書いた行を引きます。 |

| 終了コード | 場合 |
|---|---|
| 0 | この PC にあるブリッジは、すべて一覧を作れました。ないブリッジ（`tool-missing`）と、この PC にブリッジのないネットワーク（LIN、Windows 以外の CAN）は、答えの一部です。 |
| 1 | あるブリッジが一覧を作れなかった（`tool-cannot-start`、`tool-timeout`、`tool-bad-output`、ブリッジ自身の誤り）か、ファイルに誤りがあります。 |
| 3 | ファイルを読めません。 |
| 4 | 使い方の誤りです。 |

## 止め方

シミュレーションが終わったら（中断と Ctrl+C のときも）、次の順に participant を終了させます。

1. 必須の participant が自分で終わるのを、`stop_timeout_ms` まで待ちます（ecu_main の実行ファイルは、シミュレーションが終われば自分で終わります）。
2. まだ動いている participant に、止まるよう求めます（Linux: その participant のプロセスグループに SIGTERM。Windows: そのプロセスグループに Ctrl+Break）。`required: false` の participant は、ここで止めます（警告にしません）。必須の participant はエラーです（`did-not-end`）。
3. さらに `stop_timeout_ms` まで、止まるよう求めた participant と、それが起動したプロセス（プロセスグループ、Windows ではジョブオブジェクト）が終わるのを待ちます。残ったものを、それが起動したプロセスごと強制終了します（Linux: プロセスグループに SIGKILL。Windows: ジョブオブジェクトの終了）。
4. 自分で終わった participant でも、それが起動したプロセスが残っていれば終了させ、警告にします（`left-processes`）。

| 場合 | 違い |
|---|---|
| 利用者が起動する（manual の）participant | どの手順も適用しません（[participant を自分で起動する](simulation-config.md#participant-を自分で起動するmanual--manual)の「止め方」）。 |
| シミュレーションが始まらなかった | 1 を飛ばして 2 からです（その participant のエラーにはしません）。 |
| Windows で `vehicle_sil_launch` にコンソールがない | Ctrl+Break を送信できないので、2 の代わりにすぐ強制終了します（メッセージにそう書きます）。 |

ecu_main の実行ファイルは、始まる前に止まるよう求められると、すぐに終わります（[ecu-executable.md](ecu-executable.md)）。

各 participant は、自分のプロセスグループ（Windows ではプロセスグループとジョブオブジェクト）で動きます。端末の Ctrl+C は `vehicle_sil_launch` だけが受け取り、participant は受け取りません。Windows で `vehicle_sil_launch` にコンソールがないとき（participant と共有するコンソールがないとき）、participant は、コンソールの窓を開かないように起動します。

### `run` 自身の接続を閉じる

participant を終了させた後、`run` は自分の SIL Kit の接続（ECU の変数との接続、controller、registry）を閉じてから、結果を書きます。期限は、5 秒に、`stop_timeout_ms` と 5 秒の長い方を足した時間です。期限のうちに閉じなければ、待つのをやめ、警告 `connection-not-closed` を追加して結果を書き、接続を閉じないままプロセスを終えます。結果と終了コードのルールは変わりません。

この待ちも、Ctrl+C か `stop` のコマンドをもう 1 回送ると打ち切ります。打ち切ったときは、そう 1 行書いて、同じように終えます。求められたとおりなので、指摘にはしません。

## 中断（Ctrl+C）

Ctrl+C（SIGINT）、SIGTERM、SIGHUP（Windows: Ctrl+C、Ctrl+Break、コンソールを閉じる）を受けると、`vehicle_sil_launch` はシミュレーションを順に止め（controller の stop）、上の止め方で participant を終了させ、結果に中断を書いて、終了コード 130 で終わります。

| 受けた時点 | すること |
|---|---|
| 照合の間 | 照合が終わってから中断します（何も起動しません）。 |
| 始まる前 | participant を止めます。 |
| シミュレーションが終わった後、participant が自分で終わるのを待っている間 | その待ちをやめて、止まるよう求めます（止め方の 2）。 |
| 2 回目 | 待つのをやめて、残っている participant をすぐに強制終了します。 |

| シグナルなど | 扱い |
|---|---|
| `vehicle_sil_launch` を起動した側が無視するように設定したシグナル（`nohup` の SIGHUP など） | 無視したままです。 |
| 端末の Ctrl+Z（SIGTSTP） | `vehicle_sil_launch` だけを止めます。 |
| Windows でコンソールを閉じる | Windows が数秒でプロセスを終了させます。 |

Ctrl+Z では、participant は別のプロセスグループなので動き続け、registry と controller だけが止まります。Windows でコンソールを閉じたとき、participant はジョブオブジェクトが終了させますが、結果を書けないことがあります。

プログラムから `vehicle_sil_launch` を動かすときは、`--stdin-commands` を付けて、標準入力に `{"command":"stop"}` を書くか、標準入力を閉じると、Ctrl+C と同じように順に止まります（[プログラムから使う](simulation-events.md)）。シグナルも Ctrl+Break も送信できない、コンソールのない Windows の `vehicle_sil_launch` も、これで順に止められます。シミュレーションを止めると、ecu_main の実行ファイルは自分で終わります。自分で終わらない participant は、Ctrl+Break を送信できないので、止め方の 2 の代わりにすぐ強制終了します。

## participant を残さないこと

`vehicle_sil_launch` は、終わる前に、起動したすべての participant を終了させます。participant は、標準入力（空）、標準出力、標準エラー（`vehicle_sil_launch` が読むパイプ）のほかに、`vehicle_sil_launch` のファイルやハンドルを継承しません。

| OS | `vehicle_sil_launch` が強制終了されたとき（SIGKILL、クラッシュを含む） |
|---|---|
| Windows | participant とそれが起動したプロセスは終わります。 |
| Linux | participant は SIGKILL を受けますが、participant が起動したプロセスは残ることがあります。 |
| Linux 以外の POSIX | 対象にしていません（participant が残ります）。 |

- Windows: participant はそれぞれジョブオブジェクト（最後のハンドルが閉じたら、中のプロセスを終了させる）に入ります。`vehicle_sil_launch` がどう終わっても同じです。
- Linux: participant は、`vehicle_sil_launch` が終わると SIGKILL を受けます（親が終わったときのシグナル）。強制終了されたときに participant が起動したプロセスが残るのは、止め方の 3 と 4 が動かないためです。
- Linux: participant のプロセスグループを抜けたプロセス（`setsid()` などを呼んだもの）は、どの場合も終了させられません。
- Linux 以外の POSIX には、親が終わったときのシグナルがありません。

## 待ち受けるアドレス

SIL Kit の participant は、ほかの participant からの接続を待ち受けます。どのアドレスで待ち受けるかは、registry の URI のホストがループバックのアドレスかどうかで決まります。

| registry の URI のホスト | participant が待ち受けるアドレス | ほかの PC からの接続 | Windows のファイアウォール |
|---|---|---|---|
| ループバックのアドレス（`registry_uri` を書かない実行は、いつもこれ） | ループバックのアドレスだけ | できません。 | 待ち受けてよいかを尋ねません。 |
| ループバックでないアドレス（`silkit://0.0.0.0:8500` など） | SIL Kit の既定のとおり、全部のアドレス | できます。 | 下の「Windows のファイアウォール」 |

この表が当てはまるのは、vehicle-sil が SIL Kit の設定を作る participant です。ecu_main の実行ファイル、`ParticipantRuntime` や `Simulation` を使うプログラム、`vehicle_sil_can_bus_simulator`、Vector XL のブリッジ、`run` の controller がそうです。

ループバックのアドレスだけで待ち受けるとき、TCP は `127.0.0.1` です（URI が `::1` なら `::1`）。Windows 以外では、ほかに一時ディレクトリの local-domain ソケットでも待ち受けます。

ループバックでないアドレスでは、SIL Kit には認証がないので、この PC に接続できる誰でもシミュレーションに参加できます（注記 `registry-on-network`）。

registry は、自分の URI のアドレスで、TCP だけで待ち受けます（local-domain ソケットを使いません）。そのポートを別のプログラムや別の `run` が使っているとき、または `registry_uri` のホストがこの PC のアドレスでないときは、何も起動せずに `setup` のエラー（終了コード 3、`the registry cannot listen at <URI>: ...`）です。エラーの括弧の中に、どちらの見込みが高いかと、`registry_uri` に代わりに書くものが出ます。

### ループバックのアドレスとみなすもの

| 書き方 | 例 |
|---|---|
| `127.0.0.0/8` の IPv4 アドレス（`getaddrinfo()` が読む数の書き方） | `127.0.0.1`、`127.1` |
| `::1` | `[::1]`、`[0:0:0:0:0:0:0:1]` |
| IPv6 にマップした `127.x.x.x` | `[::ffff:127.0.0.1]` |
| `localhost`（大文字小文字を問わず） | `localhost`、`localhost.` |

ほかの名前は、名前解決をせずに、ループバックでないとみなします。

### Windows のファイアウォール

`registry_uri` がループバックのアドレスでないとき、Windows のファイアウォールは、registry（`vehicle_sil_launch`）と participant のプログラムに、待ち受けてよいかを尋ねます。どちらも全部のアドレスで待ち受けるためです。

許可が必要なのは、registry を持つ `vehicle_sil_launch` だけです。participant の実行ファイルは、許可しなくても参加できます。participant のほうから、ランチャーを動かす PC に接続するためです。直接接続できない participant どうしは、registry の中継で接続します（[dependencies.md](dependencies.md) の「混ぜてよい SIL Kit のバージョン」）。

| 許可の状態 | 起きること |
|---|---|
| `vehicle_sil_launch` の許可がない | ほかの PC から参加する ECU が、約 5 秒で失敗します。 |
| ランチャーを動かす PC の participant の許可がない | ほかの PC からの参加のたびに、5 秒ほどかかります。 |
| 参加する PC の participant の許可がない | 確認している構成では、そのまま参加できます。 |

確認している構成と、許可するコマンドは、[複数の PC で動かす](howto/multi-pc.md) の「ファイアウォールの許可」と「確認している構成」にあります。

### ほかの PC から参加する URI の表示

`registry_uri` がループバックのアドレスでないとき、`run` は `registry at` の行の次に、ほかの PC から参加する URI の行を書きます。

```text
[vehicle_sil_launch] from other computers, join at silkit://192.0.2.10:8500 (this computer's address): give it as --registry-uri to a participant started by hand on another computer ...
```

| `registry_uri` のホスト | 表示するアドレス |
|---|---|
| `0.0.0.0` | この PC の、動いているネットワークの IPv4 アドレス（ループバックとリンクローカル `169.254.x.x` を除く） |
| `[::]` | IPv6 のアドレス（ループバックとリンクローカルを除く）と、Linux では IPv4 のアドレス |
| アドレスを書いたもの | その URI |

- 既定の経路のあるネットワークのアドレスを先に並べ、ほかにもあれば、先頭がそれだと書き添えます。WSL、Docker、仮想マシンの仮想のネットワークのアドレスは、ほかの PC から接続できないことが多いためです。
- Linux の `[::]` で IPv4 のアドレスも並べるのは、`net.ipv6.bindv6only` が `1` でなければ、`[::]` が IPv4 も受けるためです。
- Windows の `[::]` は IPv6 だけを受けるので、IPv4 には `silkit://0.0.0.0:<port>` と書くよう添えます。
- アドレスが見つからなければ、`ipconfig`（Windows）か `ip addr` で調べるよう書きます。

### 環境変数 `VEHICLE_SIL_LISTEN_ON_NETWORK`

| 値 | 意味 |
|---|---|
| `1` | registry の URI がループバックのアドレスでも、全部のアドレスで待ち受けます。 |
| `0`、空、設定なし | 上のルールのとおりです。 |
| ほかの値 | エラーです（participant を作れず、変数の名前と値を挙げたメッセージで失敗します）。 |

`run` は、起動する participant に、いつもこの変数を渡します。`registry_uri` がループバックのアドレスでなければ `1`、そうでなければ `0` です。`vehicle_sil_launch` 自身の環境の値は渡さず、controller も使いません。その値が `run` の渡す値と違えば、注記 `listen-variable-ignored` を出します。

`1` を渡す理由は次のとおりです。`registry_uri` が `0.0.0.0`（または `[::]`）のとき、`run` はこの PC の participant に `127.0.0.1`（`[::1]`）の URI を渡します。そのままではループバックのアドレスだけで待ち受け、SIL Kit はそのアドレスを、ほかの PC の participant にもそのまま渡します。受け取った側は、それを自分のループバックのアドレスで試します。そこで別のプログラムが同じポートで待ち受けていると、そこに接続して返事を待ち続け、参加に失敗します。そうならないように、全部のアドレスで待ち受けさせます。

自分で起動する participant（manual）には、`run` が書くコマンドの行とイベント `participant`（`manual`）の `environment` に、設定する値を示します（`1` のときだけ）。ほかの participant も、registry と同じ PC の上からループバックのアドレス（`127.0.0.1` など）で接続し、その registry にほかの PC からも participant が参加するときは、自分で `1` を設定します。

### 自分の SIL Kit の設定を渡した participant

| participant | 待ち受けるアドレス |
|---|---|
| SIL Kit の設定を渡し、そこに `Middleware.AcceptorUris` がある | そのとおりに待ち受けます。 |
| SIL Kit の設定を渡し、そこに `Middleware.AcceptorUris` がない | 上のルールのとおりです（vehicle-sil が、設定に追加します）。 |
| vehicle-sil を使わない SIL Kit のプログラム（`describe: false` の participant など） | それぞれの設定のとおりです（`Middleware.AcceptorUris` がなければ全部のアドレス）。 |

SIL Kit の設定は、ecu_main の `--silkit-config`、`vehicle_sil_can_bus_simulator --participant-configuration` などで渡します（[ecu-executable.md](ecu-executable.md) の「SIL Kit の設定ファイルを渡す」）。

## 出力

| 出力先 | 内容 |
|---|---|
| 標準エラー | participant の出力と、`vehicle_sil_launch` 自身の経過 |
| `--log-dir` のファイル | participant ごとの出力 |
| 標準出力 | 結果だけ（人向けか `--json`。`--events` ではイベント） |

`--events` のときは、標準エラーに書く行（participant の行と `vehicle_sil_launch` の経過）を標準エラーに書かず、イベントとして標準出力に書きます（[プログラムから使う](simulation-events.md)）。

### 標準エラー

participant の標準出力と標準エラーは、行ごとに `[<participant 名>] ` を付けて、`vehicle_sil_launch` の標準エラーに書きます。`vehicle_sil_launch` 自身の経過は `[vehicle_sil_launch] ` で始まります。経過として書くのは、registry の URI、ほかの PC から参加する URI、起動したコマンド、始まった、仮想時間が進まない、終わった、止めた、です。

| 行の内容 | 扱い |
|---|---|
| 4096 バイトを超える行 | UTF-8 の文字の間で分けて書きます。 |
| 制御文字（タブを除く C0、DEL、U+0080 から U+009F の C1） | `?` にします。 |
| UTF-8 でないバイト | `?` にします。 |

participant は UTF-8 で書いてください。Windows のコンソールにも UTF-8 として表示するので、ほかのコードページの文字は文字化けします。

標準エラーを読む側が止まる（読まれないパイプ）と、`vehicle_sil_launch` も書くところで止まります。

### ログのファイル（`--log-dir`）

`--log-dir <dir>` を付けると、participant ごとに `<dir>/<participant 名>.log` にも書きます（`[名前]` なし）。ディレクトリがなければ作ります。JSON の `participants[].log` が、どのファイルかを示します。

前の実行のログは、同じ名前のものだけを書き直します。今の設定にない participant のログは残ります。

| participant の名前 | ファイルの名前 |
|---|---|
| ASCII の英数字、`.`、`-`、`_` 以外のバイト | `_` にします。 |
| Windows のデバイスの名前（`NUL`、`CON`、`PRN`、`AUX`、`COM1`〜`COM9`、`LPT1`〜`LPT9`。`.` の前も） | どの OS でも、後ろに `_` を付けます。 |
| 大文字と小文字だけが違う名前 | 重なりとみなし、`-2` などを付けます。 |

### 標準出力

標準出力には結果だけを書きます。同じ PC の上の同じ結果からは同じ出力です（時刻、所要時間、プロセス ID、registry の URI を含みません）。ただし、時間の進み方を示す次の値は、実行ごとに違います。

- 人向けの `time:` の行
- JSON の `timing` の `wall_ms`、`actual_speed`、`paused_ms`
- JSON の `timing` の遅れの数（`late_steps`、`max_late_ms`、`slipped_ms`、`late_ms`）

ネットワークの送受信の数（人向けの `traffic:` の行と表、JSON の `traffic`）は、参加するのが仮想時間で動く ECU だけで、全員が始まる前からいて、`stop_at_ms` かシナリオの終わりで終わった実行では、実行ごとに同じです。次の場合は、実行ごとに違うことがあります。

- 止める要求、`--timeout-ms`、participant の失敗、participant が止めたことで終わった実行（終わった仮想時刻 `sim_ms` が違うので、数も違います）。
- 仮想時間で動かない participant（実時間の `Peer`、ブリッジ）がフレームを出すネットワーク（受けた側の数が違います）。
- 後から参加した participant（`manual`、`required: false`）と、起動し直した participant（数えるかどうかと理由が違います）。
- `describe: false` の participant がいる短い実行（その participant の変数の接続がつながるのを `run` は待たないので、つながるのが遅れると `no_sample` か `no_control_link` になります）。

改行はどの OS でも LF です。Windows のコンソールに書くときは、`check` も `run` も、コンソールのコードページによらず UTF-8 の文字として表示します。

### 人向けの結果

人向けの結果は、次の順に並びます。

| 順 | 内容 |
|---|---|
| 1 | 照合の表と、照合の指摘 |
| 2 | `run:` で始まるシミュレーションの 1 行 |
| 3 | `time:` の 1 行（速度を指定したか、速度を合わせたタイムステップがあったとき） |
| 4 | `--connect` があると、つないだネットワークごとに `outside:` の 1 行（`outside: ETH_EXT -> vEthernet (A) (bridge.ETH_EXT): 152 frames out, 3 from devices`。数が分からなければ数なし） |
| 5 | participant の表（必須か、どう終わったか。`--connect` のブリッジは最後） |
| 6 | ネットワークごとの送受信（`traffic:` の 1 行と表。下の「[ネットワークの送受信](#ネットワークの送受信traffic)」） |
| 7 | 実行の指摘 |
| 8 | 結果の 1 行（`passed:`、`failed:`、`interrupted:` と、照合と実行を合わせたエラー、警告、注記の数） |

照合にエラーがあるときは、照合の表と指摘の後に `nothing was started:` の行を書きます。

<!-- vehicle_sil_launch run simulation.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

traffic: J1939_CAN: 1 participant sent, 1 received (2 counted) (steps before 1000 ms)

network    participant  sent  received
J1939_CAN  Engine         11         0
J1939_CAN  Dashboard       0        11

passed: 0 errors, 0 warnings, 0 notes
```

`time:` の行は、速度、全体の実際の速度（一時停止を除く）、速度を合わせたタイムステップの数と、そのうち許容を超えて遅れた数、最大の遅れ、ずれの合計、一時停止していた時間（あれば）です。次は `--speed 4` で動かしたときの行です。`<` と `>` で囲んだ所は、実行ごとに違う値です。タイムステップは、Dashboard のステップ幅に合わせて 5 ms になるので、200 個です。

<!-- vehicle_sil_launch run simulation.yaml --speed 4: the line of the time -->
```text
time: speed 4, ran at <actual_speed> times real time; <late_steps> of 200 paced steps more than 10 ms late (the latest by <max_late_ms> ms), <slipped_ms> ms behind in all
```

participant の表の `ended` の列:

| 表示 | 意味 |
|---|---|
| `exit code <n>` | 自分で終わった（Windows の NTSTATUS の値は 16 進で、例: `0xC0000005`） |
| `signal <名前>` | シグナルで終わった（Linux。例: `signal SIGSEGV`） |
| `stopped (SIGTERM), exit code <n>` | 止まるよう求めた後に終わった（Windows では `Ctrl+Break`） |
| `killed` | 強制終了した |
| `not started` | 起動できなかった（またはその前に実行をやめた） |
| `not started (disabled)` | 無効にした participant（`required` の列は `-`） |
| `started by you (manual)` | 利用者が起動する participant で、シミュレーションに参加した（どう終わったかは、`run` にはわからない） |
| `not joined (manual)` | 利用者が起動する participant で、参加しなかった |

無効にした participant がいるときの例です。照合の部分は `check` と同じです（[participant を無効にする](simulation-config.md#participant-を無効にするenabled--disable--enable)）。

<!-- vehicle_sil_launch run simulation.yaml --disable Dashboard -->
```text
simulation.yaml: 2 participants (1 disabled), 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    -      disabled

network    kind  participants
J1939_CAN  can   Engine

warning: simulation.yaml:8: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it [single-participant]
note: simulation.yaml:14: participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it [participant-disabled]

run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       exit code 0
Dashboard    -         not started (disabled)

traffic: J1939_CAN: 1 participant sent, 0 received (1 counted) (steps before 1000 ms)

network    participant  sent  received
J1939_CAN  Engine         11         0

passed: 0 errors, 1 warning, 1 note
```

### ネットワークの送受信（`traffic:`）

シミュレーションが始まった実行は、participant の表の後に、ネットワークごとに J1939 のフレームが届いたかを書きます。数は、各 ECU が変数の接続で送ってくる監視用の出力 `j1939.<ネットワーク名>.frames_sent` と `frames_received` です（[ECU の変数との接続](#ecu-の変数との接続control_link)）。接頭辞 `j1939.` は予約してあるので、ECU 自身の変数がこの名前になることはありません（[ecu-executable.md](ecu-executable.md) の「使えない変数名の接頭辞」）。

- `traffic:` の行は、設定ファイルのネットワークごとに、数えた participant のうち、フレームを 1 つ以上送ったものの数と、1 つ以上受けたものの数です（`J1939_CAN: 1 participant sent, 1 received (2 counted)`）。上の最初の例は、Engine が送り、Dashboard が受けています。Dashboard は受けるだけで何も送らないので、送ったのは 1、受けたのも 1 で、これで正常です。割合ではないので、2 つの数が `counted` と同じである必要はありません。届いていないのは、送った participant がいるのに `0 received` で、括弧の中が `counted` だけのネットワークです。
- 数えなかった participant がいれば、括弧の中に `1 not counted` と続けます。誰も送らず誰も受けておらず、全員を数えたネットワークは `nothing sent (2 counted)` です。そうでなければ、誰も送っていないネットワークも数で言います（`0 participants sent, 1 received (1 counted, 1 not counted)`）。数えた participant が 1 つもないネットワーク（LIN、イーサネット、生の CAN として使うネットワークなど）は `not counted (3 participants)`、誰がいるかをランチャーが知らないネットワークは `not counted (no participant known to be on it)` です（そのネットワークに居場所がわからない participant がいれば、括弧の中に `, 1 unplaced` と続けます。下）。設定ファイルにネットワークが 1 つもないときは、`traffic: no network in use` の 1 行だけです。
- `describe: false` の participant は、変数の接続がそのネットワークの数を持たないとき、そのネットワークで居場所がわからない（unplaced）participant です。この participant について、ランチャーがいる場所を知る手がかりは数だけで、いない場所を知る手がかりはありません。数のない使い方（生の CAN、LIN、イーサネット）で、ほかのネットワークにいるかもしれません。そのネットワークのフレームを受けたかもしれないので、そのネットワークの括弧に `1 unplaced` と続け、ランチャーは、そこで「届いていない」とは言いません。理由は、その participant の行の右にあります（理由の表は下）。ランチャーがどのネットワークにいるかを 1 つも知らない participant（ロガー、ブリッジ、ほかのツール）は、ネットワークの後に、ネットワークが `-` の 1 行になります。数で 1 つのネットワークにいるとわかった participant は、ほかのネットワークそれぞれの下に 1 行ずつ載ります。
- このため、`describe: false` の participant が実行にいると、それが居場所のわからないネットワークでは、「届いていない」は出ません（JSON の `delivered` は `true` か `null` で、`false` になりません）。ロガーやブリッジのいる構成では、どのネットワークもそうなります。
- 表は、ネットワークと participant ごとの 1 行です。ネットワークの名前は、シミュレーションでの名前です（`network_names` で替えた後の名前）。載る participant は、ランチャーがそのネットワークにいると知っているものだけです（`--describe` がそのネットワークを言うもの。`describe: false` では、変数がそのネットワークの数を持つもの）。変数の接続のない `describe: false` の participant（ブリッジ、ほかのツール）は、フレームを出していても、ネットワークの下に数とともには載りません（上の、居場所がわからない participant です）。誰も載らないネットワークは、participant が `-` の 1 行になります。
- 数え方: 各 participant の「実行が終わった仮想時刻（行の `steps before <ms> ms`、JSON の `before_ms`）より前に始まった最後のステップ」の値です。その時刻以後のステップの値は、実行が終わるまでに届くことも届かないこともあるので、使いません。
- `sent` は、そのステップより前に、ポートが送信として受け付けたフレームの数です（バスに出た数ではありません。Address Claimed も入ります）。`received` は、ポートから受け取った、ほかの participant の J1939 のフレームの数です（同じ participant の別のノードのものと、J1939 でないフレームは入りません）。
- 数えなかった participant は、数が `-` で、行の右に理由があります。

| 行の右の表示 | JSON の `not_counted` | 意味 |
|---|---|---|
| `not counted: no control link` | `"no_control_link"` | 変数の接続がつながらなかった（`--no-control`、変数の接続のない実行ファイル） |
| `not counted: restarted` | `"restarted"` | 同じ名前のプロセスが起動し直された（数が 0 からやり直しになるため） |
| `not counted: real time` | `"real_time"` | 仮想時間で動いていない（値の時刻がシミュレーションの時刻と別のため。仮想時間で動くと申告していても、値の時刻が controller のタイムステップで 64 個より先に進んだものを含みます） |
| `not counted: no J1939 counters` | `"no_counters"` | そのネットワークの数を持たない（生の CAN、LIN、イーサネットとして使っている） |
| `not counted: counters mapped by variables` | `"mapped"` | 設定の `variables` が、その数の出力に対応を当てている（値が実行ファイルのものと違うため） |
| `not counted: no sample before the end` | `"no_sample"` | 終わりより前のステップの値が 1 つも届かなかった |

- 居場所がわからない participant は、数が `-` で、行の右に 2 つの理由のどちらかがあります。

| 行の右の表示 | JSON の `reason` | 意味 |
|---|---|---|
| `unplaced: no control link` | `"no_control_link"` | 実行が終わるまでに、変数の接続がつながらなかった。変数の接続を持たない（ロガー、ブリッジ、ほかのツール、`--no-control`。どの実行でも同じ）か、今回の実行では接続が間に合わなかった（`run` は、`describe: false` の participant の接続を待ちません）かのどちらかです。ランチャーには、この 2 つを区別できません。変数の接続を持つ participant なら、実行を長くすれば居場所がわかります |
| `unplaced: no J1939 counters` | `"no_counters"` | 変数の接続が、そのネットワークの数を持たない（ネットワークが `-` の行では、設定ファイルのどのネットワークの数も持たない）。その構成では、どの実行でも同じです |

### 実行の指摘（`code`）

| `code` | 重大度 | 内容 |
|---|---|---|
| `setup` | エラー（終了コード 3） | registry を起動できない（`registry_uri` のポートが使われているなど）、controller を作れない、`--log-dir` に書けない、`--timeline` のファイルを開けない |
| `start` | エラー（`required: false` は警告） | participant を起動できない |
| `ended-before-start` | エラー | 必須の participant が、シミュレーションが始まる前に終わった |
| `not-joined` | エラー | 必須の participant が、プロセスは動いているのに `start_timeout_ms` の間に参加しなかった |
| `simulation-start` | エラー | 必須の participant がそろったのに、シミュレーションを始められなかった |
| `participant-failed` | エラー | 実行中に必須の participant が失敗し、シミュレーションを中断した |
| `connect-failed` | エラー | シミュレーションが始まる前に、`--connect` のブリッジが動き出せなかったので、シミュレーションを始めなかった。ファイルの participant を起動する前の失敗なら、それらも起動しなかった（[バスを外につなぐ](#バスを外につなぐ--connect)） |
| `connect-lost` | エラー | 実行中に `--connect` のブリッジが終わったので、シミュレーションを中断した。または、シミュレーションが終わったときに、もう終わっていた。または、リンクやバスが 30 秒落ちていた（[リンクとバスの状態](#リンクとバスの状態)） |
| `outside-time-changed` | 注記 | 外につながる実行を `--time-policy free` で動かし、コマンドがシミュレーションを止めたかステップで進めたか、1 以外の速さにした。その間も、つながっている相手は動いていた（実行に 1 つ。最初のそういうコマンドについて。[外につながる実行の時間](#外につながる実行の時間--time-policy)） |
| `unsynchronized-time-changed` | 注記 | 実時間で動く participant（ステップがない）のいる実行で、コマンドがシミュレーションを止めたかステップで進めたか、1 以外の速さにした。その間も、その participant は動いていた（実行に 1 つ。最初のそういうコマンドについて） |
| `connect-nothing-received` | 注意 | `--connect` のある実行で、ネットワークのブリッジが実機のバスへフレームを送ったのに、そこの機器から 1 つも来なかった（そういうネットワークごとに 1 つ。[外のバスとやりとりしたフレームの数](#外のバスとやりとりしたフレームの数)） |
| `outside-link-down`、`outside-bus-off` | 警告 | シミュレーションの実行中に、つないだネットワークのリンクやバスが落ち、ブリッジは動き続けた。回数と合計の時間（[リンクとバスの状態](#リンクとバスの状態)） |
| `outside-bus-errors`、`outside-frames-dropped`、`outside-loop-suspected` | 警告 | つないだネットワークのブリッジが error passive になった、フレームを失った（1 つでも）、MAC アドレスのループを止めた（同じ節） |
| `outside-device-shared` | 警告（`--strict` が止めた、または始めなかったときは誤り） | つないだネットワークのデバイスを、この実行の外の vehicle-sil のブリッジも使っていた。プロセス ID と、最初に見つけた時刻（[別の実行も使っているデバイス](#別の実行も使っているデバイス)） |
| `connect-target-shared` | 警告 | 実行の 2 つのネットワークの `--connect` の値は違うが、ブリッジが同じデバイスを報告した（チェックが見つけられない場合の、チェックと同じ所見。同じ節） |
| `exit-code` | エラー（`required: false` は警告） | participant の終了コードが 0 でない、またはシグナルで終わった（上のどれでもないとき） |
| `did-not-end` | エラー | 必須の participant が、シミュレーションの終わりから `stop_timeout_ms` たっても動いていた |
| `left-processes` | 警告 | participant が起動したプロセスが、その participant が終わった後も動いていた |
| `connection-not-closed` | 警告 | `run` 自身の SIL Kit の接続が、participant を終了させた後、期限（[止め方](#止め方)）のうちに閉じなかった |
| `timeout` | エラー | シミュレーションが `--timeout-ms` の実時間で終わらなかったので、中断した |
| `simulation` | エラー | シミュレーションが正常に終わらなかった（理由の participant がわからない） |
| `ended-before-stop-at` | 注記 | `stop_at_ms` より前に、participant がシミュレーションを止めた |
| `control-link-no-hello` | 警告 | 変数を公開するはずの ECU の hello か、コマンドを受け取れるという返事が、始まる前に来なかった |
| `control-link-dropped` | 警告 | `vehicle_sil_launch` が、ECU の変数との接続のメッセージを捨てた |
| `control-link-version` | 警告 | 受信した hello の、メッセージの形式のバージョンが `vehicle_sil_launch` と違う |
| `control-link-late-values` | 警告 | ECU の値か結果を、仮想時刻の保証より遅れて受信した |
| `variable-map-not-applied` | エラー | 設定ファイルの `variables` の対応のうち、ECU が接続のときに通知した変数に合わないものがあった |
| `before-start` | エラー | 開始を待っている間にする処理が失敗した |
| `behind-real-time` | 警告 | 速度を合わせたタイムステップのうち、遅れが `late_tolerance_ms` を超えたものがあった |
| `coarse-time-step` | 警告 | 速度の指定があり、controller のタイムステップが ECU のステップより粗い |
| `timeline-write` | エラー | `--timeline` のファイルを、実行の後に書けなかった（開けたが、書き込みが失敗した） |
| `timeline-limit` | 注記 | `--timeline` のファイルが、上限で古い側の点を捨てた |
| `interrupted` | 注記 | 中断された |

`code` ごとの補足:

| `code` | 補足 |
|---|---|
| `connection-not-closed` | 待った時間を書きます。結果はそろっています。 |
| `connect-failed`、`connect-lost` | ネットワーク、先、ブリッジがどう終わったかと、ブリッジが理由を書いた行を書きます（[バスを外につなぐ](#バスを外につなぐ--connect)）。ブリッジについては、`start`、`ended-before-start`、`not-joined`、`participant-failed`、`exit-code` の代わりに出ます。`connect-failed` は、各ネットワークのつなげる先を出す `vehicle_sil_launch connections` を挙げます。 |
| `timeout` | manual の必須の participant がいる実行では、始まらなかったときにも出ます（待っていたものを書きます）。 |
| `control-link-no-hello` | hello が来なかったのか、返事が来なかったのかは、メッセージに書きます（[接続の状態](#接続の状態state)）。 |
| `control-link-dropped` | 受け付けなかった hello、読めなかったメッセージ、知っている ECU やその変数に合わなかったメッセージの数を書きます。 |
| `control-link-version` | `--describe` の出力のバージョンが違うときは、照合の同じ `code` の指摘で、実行では繰り返しません。 |
| `control-link-late-values` | ステップの外から送信しています（自作の通信の側のエラー）。 |
| `variable-map-not-applied` | どれが、なぜ合わないかを書きます。 |
| `before-start` | 処理は、`run` を関数として呼ぶ側が渡すものです（内部用）。 |
| `behind-real-time` | 遅れたタイムステップの数、最大の遅れ、ずれの合計、全体の実際の速度、止まったまま終わったならそのときの遅れを書きます。外につながっていて実時間を保つ実行では、つながっている相手は待たないので、遅れた分だけ相手とずれたことも書きます。 |
| `coarse-time-step` | ECU のステップが最大で何 ms 遅れてまとまって動くかと、`time_step_ms` に書く値を書きます。 |
| `timeline-write` | ファイルの中身は途中までなので使いません。シナリオの結果は変わりませんが、この指摘で実行は失敗です（終了コード 1）。空きを作るか、別のファイルを指定して、もう一度実行します（[シナリオの実行を時間の軸で記録する](#シナリオの実行を時間の軸で記録する--timeline)）。 |
| `timeline-limit` | 捨てた点の数を書きます。終わりの側の点は全部あります。どの系列のどの時刻かは、ファイルの `limits.dropped` にあります。全部の点が要るなら、シナリオを短くします。 |

`connection-not-closed` は、どの設定でも起きないはずなので、`vehicle_sil_launch` か SIL Kit の不具合です。結果と `run` の出力を添えて報告してください。

`control-link-dropped` で考えられる原因は、次の 4 つです。該当する participant の変数は、欠けているか古いことがあります。

- 変数を公開する participant が上限（256）を超えた。
- バージョンの違う vehicle-sil の ECU がいる。
- 別のプログラムが同じトピック（`vehicle_sil.control.*`）に書いている。
- 実行中に参加した participant の hello を、まだ受信していなかった。

`control-link-version` が出たら、どちらが古いかが文に出ます。ECU のほうが古ければ `vehicle_sil_launch` の vehicle-sil でビルドし直し、新しければ ECU の vehicle-sil と同じかそれより新しいバージョンの `vehicle_sil_launch` を使います。

`variable-map-not-applied` では、合わなかった対応は適用しません。変数は実行ファイルの名前と値のまま見えていて、その対応の名前への書き込みは拒否しています。同じ内容を、実行の行（`log`）にも 1 回書きます。始まった時点で接続している ECU は始まったときに、その後に接続した ECU は実行の終わりに書きます。照合が実行の前に見つけるので、出るのは `describe: false` の participant か、`--describe` の出力と違う変数を通知した実行ファイルのときだけです（[変数の名前と値を合わせる](simulation-config.md#変数の名前と値を合わせるvariables)）。

`before-start` が出たとき、シミュレーションは始まっていて、すぐに中断しています。

`behind-real-time` と `coarse-time-step` は、どの participant の指摘でもありません（[速度を指定して動かす](#速度を指定して動かすspeed)）。

## 終了コード

`run` の終了コードです。`check` と `test` を含む全体の表と、引数のエラーの一覧は、[simulation-config.md](simulation-config.md) の「引数と終了コード」にあります。`test` の終了コードは[シナリオをまとめて実行する](#シナリオをまとめて実行するvehicle_sil_launch-test)にもあります。

| 終了コード | 意味 | `--json` の `result` |
|---|---|---|
| 0 | シミュレーションが正常に終わり、必須の participant がどれも 0 で終わった（`--strict` では警告もない） | `"passed"` |
| 1 | 照合のエラーで、何も起動しなかった | `"check_failed"` |
| 1 | 実行のエラー（participant が失敗した、そろわなかった、止めても終わらなかったなど） | `"failed"` |
| 3 | 設定ファイルを読めない（ファイルがない、文法のエラー、知らない形式のバージョンなど） | `"unreadable"` |
| 3 | registry や controller を用意できない（指摘 `setup`） | `"setup_failed"` |
| 3 | 結果を標準出力に書けない、`vehicle_sil_launch` 自身の想定外の失敗 | |
| 4 | 引数のエラー | `"usage"` |
| 130 | 中断された（Ctrl+C、`{"command":"stop"}`、標準入力の終わり、閉じた標準出力、Control Server のクライアントの `Stop` など） | `"interrupted"` |

ファイルがないときの終了コードは、ファイルの種類で違います。

| ないファイル | 終了コード | 理由 |
|---|---|---|
| 設定ファイル（`run nofile.yaml`） | 3 | 設定ファイルを読めないので、照合を始められません。 |
| `--scenario` のファイル | 1 | 照合のエラー（`code` は `file`）として数えます。 |
| `test --scenarios` のパス | 4 | 引数のエラーです。 |

## JSON（`run --json`）

1 つの JSON のオブジェクトです。ルール（キーの順、`null`、字下げ、改行、バージョンの上げ方と読む側の扱い）は照合の JSON と同じです。

| キー | 値 |
|---|---|
| `vehicle_sil_run` | 形式のバージョン（今は 1） |
| `vehicle_sil` | `vehicle_sil_launch` の vehicle-sil のバージョン |
| `file` | 設定ファイル（引数に書いたとおり） |
| `result` | `"passed"`、`"failed"`、`"interrupted"`、`"check_failed"`（照合のエラーで何も起動しなかった）、`"unreadable"`、`"setup_failed"`、`"usage"` |
| `strict` | `--strict` を付けたか |
| `errors`、`warnings`、`notes` | 照合と実行を合わせた数 |
| `simulation` | シミュレーションの終わり方（下の表）。何も起動しなかったときは `null` |
| `interrupted_by` | 中断のシグナルかキー（`"SIGINT"`、`"Ctrl+C"` など）。中断でなければ `null` |
| `registry_uri` | 実行の registry が待ち受けた URI で、participant に渡したもの。ファイルのポートが 0 なら、取ったポートです（`check` の `registry_uri` は書いたとおり）。registry を起動しなかったときは `null` |
| `timing` | 時間の進み方（下の表）。何も起動しなかったときは `null` |
| `participants[]` | participant ごとの結果（下の表）。設定ファイルの順で、無効にした participant も含む。その後に `--connect` のブリッジ |
| `outside[]` | `--connect` で実機のバスにつないだネットワーク（下の表）。ファイルのネットワークの順。`--connect` がなければ空 |
| `traffic` | ネットワークごとの送受信（[ネットワークの送受信](#ネットワークの送受信traffic)。下の表）。シミュレーションが始まらなかったときは `null` |
| `findings[]` | 実行の指摘（照合の `findings[]` と同じ形） |
| `check` | 照合の結果（`check --json` と同じオブジェクト） |
| `scenario` | `--scenario` を付けたときだけ。シナリオの結果（[scenario.md](scenario.md) の「出力」） |

選んだ機種（`variant`）と、パラメーター（`params`、`scenario.params`）は、`check` の中にあります。`check` の中にも `scenario`（`file`、`name`、`steps`）があります。どちらの `scenario` にも `inputs[]`（`inputs` に宣言した揺れ）と `lines[]`（文面が入力に置くものの一覧）があります。`dither` のないシナリオでは空で、シナリオを読めなかったときは `null` です。

`simulation` の値:

| 値 | 意味 |
|---|---|
| `"reached_stop_at"` | `stop_at_ms` まで動いた |
| `"ended"` | participant が止めた（`--scenario` では、シナリオが終わってランチャーが止めたときも） |
| `"aborted"` | 中断した（`--timeout-ms` のときも） |
| `"interrupted"` | 止める要求（Ctrl+C など）で止めた |
| `"not_started"` | 始まらなかった |

`--scenario` を付けた実行で、`"ended"` がどちらの意味かを見分けるには、`scenario.status` を見ます。`"passed"` か `"failed"` なら、シナリオが終わって止めた実行です。飛ばした手順があれば、`scenario.skipped_steps` にその数があります（`status` は `"passed"` のまま）。飛ばした手順の `at_ms` は null です。`--timeline` を付けた実行では、`scenario.timeline` に、時間の軸のファイルの名前（引数のとおり）があります。付けなければ `null` です（[シナリオの実行を時間の軸で記録する](#シナリオの実行を時間の軸で記録する--timeline)）。

`participants[]` のキー:

| キー | 値 |
|---|---|
| `name` | participant の名前 |
| `required` | 設定ファイルの `required` |
| `enabled` | `false` なら無効にした participant（`ended` は `"not_started"`、`log` は `null`） |
| `manual` | `true` なら利用者が起動する participant（`ended` は `"not_started"`、`exit_code` は `null`） |
| `for_network` | `--connect` のために `run` が起動したブリッジでは、つなぐネットワーク。ファイルの participant では `null` |
| `ended` | `"exited"`、`"stopped"`、`"killed"`、`"not_started"` |
| `exit_code` | 終了コード（シグナルで終わったとき、強制終了したとき、起動しなかったときは `null`） |
| `signal` | `"SIGSEGV"` など（なければ `null`） |
| `left_processes` | 起動したプロセスを残したか |
| `log` | `--log-dir` のファイル（なければ `null`） |
| `control_link` | 始まったときの、ECU の変数との接続の状態（下の表） |

シミュレーションが待つのは、`required` と `enabled` がどちらも `true` のものです。`manual` は [participant を自分で起動する](simulation-config.md#participant-を自分で起動するmanual--manual)にあります。

`outside[]` のキー（[バスを外につなぐ](#バスを外につなぐ--connect)）:

| キー | 値 |
|---|---|
| `network` | ネットワーク |
| `target` | つないだ先（`--connect` に渡したとおり） |
| `participant` | `participants[]` でのブリッジの名前（`bridge.<network>`） |
| `to_outside` | ブリッジがバスへ送ったフレームの数（Ethernet は `silkit_to_ext`、CAN は `silkit_to_xl`）。ブリッジの統計の行を 1 つも読めなければ `null` |
| `from_outside` | バスの機器から来たフレームの数（Ethernet は `ext_from_devices`、CAN は `xl_to_silkit`）。`null` は `to_outside` と同じ（[外のバスとやりとりしたフレームの数](#外のバスとやりとりしたフレームの数)） |
| `counts` | ブリッジの最後の統計の行の `key=N` のすべて。キーはブリッジのまま（オブジェクト。行を 1 つも読めなければ `{}`） |

`participants[].control_link` のキー（[ECU の変数との接続](#ecu-の変数との接続control_link)）:

| キー | 値 |
|---|---|
| `state` | `"connected"`、`"no_hello"`、`"version_mismatch"`、`"not_offered"`、`"none"`（起動しなかった participant は `"none"`） |
| `variables` | `connected` のとき、その ECU が公開する変数の数（ほかは `null`） |
| `version` | `version_mismatch` のとき、相手の形式のバージョン（ほかは `null`） |
| `late_messages` | 保証より遅れて受信したメッセージの数（ふつうは 0） |

`variables` は、ECU の実行が追加する `peer.` などの出力を含むので、`--describe` の数より多いことがあります。

`timing` のキー（数の定義は[速度を指定して動かす](#速度を指定して動かすspeed)）:

| キー | 値 |
|---|---|
| `speed` | 終わったときの速度（数か `"max"`） |
| `time_step_ms` | controller のタイムステップ（設定ファイルの値か既定値） |
| `ecu_step_ms` | ECU のステップ幅の最小（ミリ秒） |
| `step_delay_ms` | タイムステップが ECU のステップより粗いために、ECU のステップが、その仮想時刻にあたる実時間より遅く動く量の最大（ミリ秒） |
| `late_tolerance_ms` | 遅れの許容（設定ファイルの値か既定値） |
| `sim_ms` | 進んだ仮想時間（ミリ秒。最後に始まったタイムステップの時刻） |
| `wall_ms` | かかった実時間（ミリ秒。一時停止していた時間を含む） |
| `actual_speed` | 全体の実際の速度（`sim_ms` ÷（`wall_ms` − `paused_ms`）、小数 3 桁） |
| `paced_steps` | 速度を合わせたタイムステップの数（期限と比べたタイムステップ） |
| `late_steps` | そのうち、遅れが `late_tolerance_ms` を超えたタイムステップの数（ずれた回数） |
| `max_late_ms` | 速度を合わせたタイムステップの、最大の遅れ（ミリ秒、切り上げ。許容の内のものも含む） |
| `slipped_ms` | ずれの合計（遅れたタイムステップの遅れの合計。ミリ秒、切り上げ） |
| `late_ms` | 実行が終わったとき、期限を過ぎたまま始まっていなかったタイムステップの遅れ（ミリ秒、切り上げ。なければ 0） |
| `paused_ms` | 一時停止していた実時間（ミリ秒） |

`timing` の時間は、controller の最初のタイムステップ（仮想時間 0）が始まってから、最後のタイムステップが始まるまでのものです。participant の起動、止める処理、失敗に気づくまでの待ちは入りません。次のタイムステップが期限を過ぎたまま実行が終わったときと、最速のときは、実行が終わった時点までです。

| キー | 補足 |
|---|---|
| `ecu_step_ms` | ステップ幅がわからない participant（`describe: false`）があるとき、ステップを持つ ECU がないときは `null` です。 |
| `step_delay_ms` | 速度を合わせているときに意味があります。 |
| `step_delay_ms` | 0 なら、どの ECU のステップもタイムステップの境目で始まります。 |
| `step_delay_ms` | ステップ幅がわからない participant があるときは `null` です。 |
| `sim_ms` | participant の失敗や中断で終わった実行では、1 タイムステップの幅で実行ごとに変わります。 |
| `actual_speed` | 動いた時間が 1 ms に満たなければ `null` です。 |
| `paced_steps` | 期限を過ぎたまま始まらなかった最後のタイムステップを含みます。 |
| `slipped_ms` | シミュレーションが、指定の速度で進んだ場合より、全体でこれだけ遅れました。 |
| `late_ms` | 0 でなければ、シミュレーションは止まったまま終わりました（`late_steps`、`max_late_ms`、`slipped_ms` に入っています）。 |

`sim_ms` が実行ごとに変わるのは、失敗や中断に気づく時点が、controller の次のタイムステップが始まる前か後かで違うためです。

`traffic` のキー:

| キー | 値 |
|---|---|
| `before_ms` | 実行が終わった仮想時刻（`timing.sim_ms`）。数は、これより前に始まったステップのものです |
| `unplaced[]` | 実行にいて、ランチャーがどのネットワークにいるかを 1 つも知らない participant（「[ネットワークの送受信](#ネットワークの送受信traffic)」）。設定ファイルの順。`participant`（名前）、`reason`（`"no_control_link"` か `"no_counters"`。理由の表は同じ節）。いなければ `[]`。ここにいる participant は、どのネットワークの `unplaced` にもいます |
| `networks[]` | 設定ファイルのネットワークの全部（その順）。`name`（シミュレーションでの名前）、`kind`、`counted`（数えた participant の数）、`senders`（そのうち 1 つ以上送ったものの数）、`receivers`（そのうち 1 つ以上受けたものの数）、`delivered` と `unplaced`（下） |
| `networks[].delivered` | 送ったフレームが届いたか。`true`: 数えた participant が送り、数えた別の participant が受けた。`false`: 数えた participant が送ったのに、数えた participant が誰も受けていない（そのネットワークにいるとわかっている participant は全部数えていて、そのネットワークの `unplaced` が空）。`null`: わからない（数えた participant がいない、誰も送っていない、ただ 1 つの送り手がただ 1 つの受け手（ランチャーが数えない participant のフレームを受けた）、または、数えた participant が誰も受けていないが、数えなかった participant がそのネットワークにいるか、そのネットワークの `unplaced` が空でない） |
| `networks[].unplaced[]` | 実行にいて、ランチャーが知らないまま、このネットワークにいるかもしれない participant（`describe: false` で、このネットワークの数を持たないもの）。設定ファイルの順。`participant`、`reason`（上の `unplaced` と同じ）。いなければ `[]`。数でほかのネットワークにいるとわかった participant は、ここには載り、`traffic` の `unplaced` には載りません |
| `networks[].participants[]` | 設定ファイルの順（誰がいるかわからないネットワークは `[]`）。`name`、`counted`、`not_counted`（数えたときは `null`。ほかは「[ネットワークの送受信](#ネットワークの送受信traffic)」の表の文字列）、`virtual_time`（仮想時間で動いているか。変数の接続がつながらなかったときは `null`）、`at_ms`（数を読んだステップの時刻）、`frames_sent`、`frames_received`（数えなかったときは、この 3 つは `null`） |

届いたかどうかをプログラムで判断するときは、ネットワークごとの `delivered` を見ます（`false` が「送ったのに届いていない」。`null` は判断できないので、届いたとも届いていないとも読みません）。`describe: false` の participant（ロガー、ブリッジ）が実行にいると、その participant が `unplaced` に載っているネットワークの `delivered` は `false` になりません（その participant がフレームを受けたかもしれないため）。誰が、なぜそうなのかは、そのネットワークの `unplaced` にあります。participant ごとには `not_counted` と `frames_received` を見ます。`delivered` は数だけで決めています。participant は自分が送ったフレームを受信に数えないので、送った本人でない participant の受信が 1 以上なら、誰かのフレームが届いています。ただし、それが誰のフレームかは数からはわかりません（数えなかった participant やブリッジのフレームかもしれません）。

<!-- vehicle_sil_launch run simulation.yaml --json -->
```json
{
  "vehicle_sil_run": 1,
  "vehicle_sil": "<version>",
  "file": "simulation.yaml",
  "result": "passed",
  "strict": false,
  "errors": 0,
  "warnings": 0,
  "notes": 0,
  "simulation": "reached_stop_at",
  "interrupted_by": null,
  "registry_uri": "silkit://127.0.0.1:<port>",
  "timing": {
    "speed": "max",
    "time_step_ms": 10,
    "ecu_step_ms": 5,
    "step_delay_ms": 5,
    "late_tolerance_ms": 10,
    "sim_ms": 1000,
    "wall_ms": <wall_ms>,
    "actual_speed": <actual_speed>,
    "paced_steps": 0,
    "late_steps": 0,
    "max_late_ms": 0,
    "slipped_ms": 0,
    "late_ms": 0,
    "paused_ms": 0
  },
  "participants": [
    {
      "name": "Engine",
      "required": true,
      "enabled": true,
      "manual": false,
      "for_network": null,
      "ended": "exited",
      "exit_code": 0,
      "signal": null,
      "left_processes": false,
      "log": null,
      "control_link": {
        "state": "connected",
        "variables": 22,
        "version": null,
        "late_messages": 0
      }
    },
    {
      "name": "Dashboard",
      "required": true,
      "enabled": true,
      "manual": false,
      "for_network": null,
      "ended": "exited",
      "exit_code": 0,
      "signal": null,
      "left_processes": false,
      "log": null,
      "control_link": {
        "state": "connected",
        "variables": 22,
        "version": null,
        "late_messages": 0
      }
    }
  ],
  "outside": [],
  "traffic": {
    "before_ms": 1000,
    "unplaced": [],
    "networks": [
      {
        "name": "J1939_CAN",
        "kind": "can",
        "counted": 2,
        "senders": 1,
        "receivers": 1,
        "delivered": true,
        "unplaced": [],
        "participants": [
          {
            "name": "Engine",
            "counted": true,
            "not_counted": null,
            "virtual_time": true,
            "at_ms": 990,
            "frames_sent": 11,
            "frames_received": 0
          },
          {
            "name": "Dashboard",
            "counted": true,
            "not_counted": null,
            "virtual_time": true,
            "at_ms": 990,
            "frames_sent": 0,
            "frames_received": 11
          }
        ]
      }
    ]
  },
  "findings": [],
  "check": {
    "vehicle_sil_check": 1,
    "vehicle_sil": "<version>",
    "file": "simulation.yaml",
    "result": "passed",
    "strict": false,
    "errors": 0,
    "warnings": 0,
    "notes": 0,
    "stop_at_ms": 1000,
    "start_timeout_ms": 30000,
    "stop_timeout_ms": 10000,
    "registry_uri": null,
    "participants": [
      {
        "name": "Engine",
        "line": 11,
        "command": [
          "engine/vehicle_sil_engine_ecu"
        ],
        "describe": true,
        "required": true,
        "enabled": true,
        "manual": false,
        "role": "ecu",
        "step_ms": 10,
        "status": "described",
        "reported": {
          "vehicle_sil": "<version>",
          "name": "Engine",
          "step_ns": 10000000,
          "networks": [
            {
              "name": "J1939_CAN",
              "kind": "can",
              "use": "j1939",
              "nodes": [
                {
                  "address": 0,
                  "j1939_name": "0x00000000FE000001",
                  "arbitrary_address_capable": false
                }
              ],
              "mac": null
            }
          ]
        },
        "variables": [
          {
            "name": "ecu.connected",
            "type": "bool",
            "causality": "input",
            "unit": "",
            "min": null,
            "max": null,
            "step": null,
            "description": "on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on",
            "items": [],
            "builtin": true
          },
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
          {
            "name": "engine.speed",
            "type": "integer",
            "causality": "output",
            "unit": "rpm",
            "min": null,
            "max": null,
            "step": null,
            "description": "",
            "items": [],
            "builtin": false
          }
        ],
        "network_map": null,
        "j1939_nodes": null,
        "control": {
          "transport": "silkit",
          "wire_version": 2
        }
      },
      {
        "name": "Dashboard",
        "line": 14,
        "command": [
          "dashboard/vehicle_sil_dashboard_ecu"
        ],
        "describe": true,
        "required": true,
        "enabled": true,
        "manual": false,
        "role": "ecu",
        "step_ms": 5,
        "status": "described",
        "reported": {
          "vehicle_sil": "<version>",
          "name": "Dashboard",
          "step_ns": 5000000,
          "networks": [
            {
              "name": "J1939_CAN",
              "kind": "can",
              "use": "j1939",
              "nodes": [
                {
                  "address": 40,
                  "j1939_name": null,
                  "arbitrary_address_capable": false
                }
              ],
              "mac": null
            }
          ]
        },
        "variables": [
          {
            "name": "ecu.connected",
            "type": "bool",
            "causality": "input",
            "unit": "",
            "min": null,
            "max": null,
            "step": null,
            "description": "on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on",
            "items": [],
            "builtin": true
          },
          {
            "name": "dashboard.engine_speed",
            "type": "integer",
            "causality": "output",
            "unit": "rpm",
            "min": null,
            "max": null,
            "step": null,
            "description": "the speed shown",
            "items": [],
            "builtin": false
          },
          {
            "name": "dashboard.engine_lost",
            "type": "bool",
            "causality": "output",
            "unit": "",
            "min": null,
            "max": null,
            "step": null,
            "description": "no engine speed for more than 300 ms (it comes every 100 ms)",
            "items": [],
            "builtin": false
          }
        ],
        "network_map": null,
        "j1939_nodes": null,
        "control": {
          "transport": "silkit",
          "wire_version": 2
        }
      }
    ],
    "networks": [
      {
        "name": "J1939_CAN",
        "kind": "can",
        "line": 8,
        "participants": [
          "Engine",
          "Dashboard"
        ],
        "outside": null
      }
    ],
    "time_lock": {
      "locked": false,
      "because": []
    },
    "findings": []
  }
}
```

この節の例も、CTest の `launch_run.docs` が実際の実行と照合します（`vehicle_sil` のバージョンと、`<wall_ms>`、`<actual_speed>`、`<port>` にした値を除いて、バイト単位で）。

## シナリオの実行を時間の軸で記録する（`--timeline`）

`run --scenario` に `--timeline <file>` を付けると、シナリオが書いた値と見た値を、仮想時刻の順に 1 つの JSON のファイルに書きます。グラフにする道具や、2 つの実行を比べる道具が読むためのファイルです。`vehicle_sil_launch version --json` の `features` に `timeline` があれば使えます。

```sh
vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml --timeline throttle_step.timeline.json
```

- ファイルは実行の後に書き、標準エラーに `[vehicle_sil_launch] timeline: <file>` の 1 行を出します（`--events` では `log` のイベント）。`run --json` と `--events` の結果では、`scenario.timeline` にファイルの名前（引数のとおり）が入り、`--timeline` がなければ `null` です。**`--timeline` を付けない実行の出力は、ほかに何も変わりません。**
- ファイルは、何かを起動する前に開きます（ディレクトリがなければ作ります）。開けなければエラー `setup`（終了コード 3）で、何も起動しません。実行の後に書けなかったとき（ディスクがいっぱい、など）は、エラー `timeline-write` です。シナリオの結果（`scenario.status`）は変わりませんが、エラーの指摘なので、シナリオが通っていても実行は失敗です（終了コード 1）。
- 上限で点を捨てたときは、`timeline:` の行に捨てた点の数を足し、注記 `timeline-limit` を出します（下の「上限」。終了コードは変わりません）。
- シナリオが通っても、失敗しても、途中で終わっても書きます。照合のエラーで何も起動しなかったときは書きません。
- `--scenario` のない `run` に付けると、引数のエラーです。`check` と `test` には付けられません。
- 形は `schema/timeline.schema.json`（JSON Schema）にあります。改行はどの OS でも LF です。

### 時間の軸のファイルを見る

時間の軸のページは、時間の軸のファイルを図にします。ブラウザの GUI から始めたシナリオの実行なら、「シナリオ」のカードのリンク「時間の軸」で開きます。GUI サーバーが実行の時間の軸を記録し、ページを GUI と同じ言語で配ります（[gui.md](gui.md#シナリオの実行の時間の軸) の「シナリオの実行の時間の軸」）。

`--timeline` で自分で書いたファイルには、vehicle-sil のリポジトリの `gui/timeline/timeline.html`、またはビルド済みの道具の圧縮ファイルの `python/vehicle_sil_gui/timeline/timeline.html` を使います。ブラウザで開き（ローカルのファイルとして開いてかまいません。隣に `vehicle-sil-timeline.js` と `page.js` が要ります）、時間の軸のファイルをページに落とすか、**Open a file**（日本語では **ファイルを開く**。ボタンなので、キーボードでも押せます）で選びます。ページはファイルをブラウザの中で読み、どこにも送りません。

- シナリオが失敗していれば、ページは失敗の所を開きます。失敗した手順とその時刻、ランチャーの文、成り立たなかった条件とそのとき見た値、書き込みの失敗なら、その時刻の書き込みのうち ECU に入ったもの、入らなかったもの、入ったかどうかを言う結果がないもの（不明）を出します。
- 図には、手順の段と、変数ごとの段があります。入力（送った値と、条件が読む入力なら見た値）、その後にほかの変数です。帯は、条件が許す値の範囲です（緑は成り立った、赤は成り立たなかった、灰はその条件では決まらなかった手順）。送った値のうち、ECU に入らなかったものは赤の破線、入ったかどうか不明なものはオレンジの点線で描き、どちらも変数がとった値としては描きません。図の上をドラッグすると拡大し、ダブルクリックで実行の全体に戻ります。
- 図の下の表に、同じ手順、入力の線、指摘があります。
- ページは英語か日本語です。アドレスの `?lang=en` か `?lang=ja`、なければブラウザの言語、それもなければ英語です。ファイルとランチャーの名前、値、文は、そのまま出します。
- Web サーバーから配ったページは、`?src=` でファイルのアドレスも受け取ります。同じサーバーのものだけです（`timeline.html?src=runs/throttle_step.timeline.json`）。
- ページが読むのは、形の版 1 で `stage` が `"run"` のファイルです。ほかの版や `stage` のファイルは、描かずに、表示できないと言います。知らないキーは無視し、知らない値と、変数の型に合わない値は、描かずに段の下に名前を出します。表示できないファイルを開くと、ページは何も出さない状態に戻ります（前のファイルの図を、そのファイルのものと取り違えないため）。表示しているファイルの名前は、上の欄に出ます。

### ファイルの形

時刻は全部、仮想時刻のミリ秒です（小数になりえます）。変数は、どこでも `"<participant>.<名前>"` の文字列で指します。

| キー | 値 |
|---|---|
| `vehicle_sil_timeline` | この形式のバージョン（今は 1） |
| `vehicle_sil`、`stage` | vehicle-sil のバージョン、`"run"`（実際の実行の記録） |
| `file`、`variant`、`params` | 設定ファイル（引数のとおり）、機種（なければ `null`）、シナリオのパラメーターと効いた値（`check --json` の `scenario.params` と同じ形） |
| `time_step_ms` | controller のタイムステップ（`run --json` の `timing.time_step_ms` と同じ） |
| `scenario` | `file`、`name`、`status`、`over_at_ms`（`run --json` と同じ）と、`recorded_to_ms`（下の「記録の終わり」） |
| `variables[]` | シナリオが使う変数（最初に出た順）。`name`、`participant`、`type`、`causality`、`unit`、`min`、`max`、`items`（enum）、`ecu_step_ms`（その ECU のステップ幅）、`written`（シナリオが書く）、`watched`（条件が読む） |
| `steps[]` | 手順。`run --json` の `scenario.steps[]` と同じキーに、`lines`、`watch`、`failed` を足したもの |
| `series[]` | 値の列（下） |
| `lines[]` | 入力ごとの線: 手順が置いた値と動き（下の「入力の線」） |
| `findings[]` | 照合と実行の指摘のうち、`code` が `scenario-` で始まるもの（`run --json` と同じ形）。ECU の値が約束より遅れて届いたことを言う指摘 `control-link-late-values` は、`severity` と `code` だけで、ほかのキーは `null`（この指摘のある実行は、記録が実行ごとに違いえます）。実時間の速さの指摘（`behind-real-time`）など、ほかの指摘は入りません |
| `limits` | 点の数の上限と、捨てた点（下の「上限」） |

読む側は、知らないキーを無視します。知らない値（系列の `source` と `encoding`、`watch` の `mode`、条件の `op`、手順の `kind` と `status`、線の `kind` と `base_source`、`shape` の `type`）は、その部分を描かずに名前と `text` を出します。形式のバージョンを上げずに、キーと値が増えることがあります。

この節のキーは、どのファイルにも全部あります。当てはまらないキーの値は `null` です。例外は手順の `expect_skipped` だけで、`run --json` と同じく、その手順にあるときだけ書きます。

手順の `lines` は、その手順が置いた線の、いちばん上の `lines[]` の中の位置（0 から）の配列です（書かない手順、時刻が `recorded_to_ms` より後の手順、書き込みを 1 つも送らなかった手順では空）。

手順の `watch` は、その手順が見る条件です（条件のない手順では `null`）。

| `mode` | 手順 | 見るもの | `to_ms` |
|---|---|---|---|
| `at` | その時点の `expect` | `from_ms` の時刻の値 | `null` |
| `always` | `hold_for_ms` つきの `expect` | `from_ms` の値と、`to_ms` までの全部のサンプル | 手順の時刻 + `hold_for_ms` |
| `once` | `within_ms` つきの `expect`、`wait_until` | `from_ms` の値と、その後のサンプル。成り立った時刻は手順の `at_ms` | 待つ上限 |
| `during` | `ramp` と `wave` の横の `expect` | 時刻が `from_ms` から `to_ms` までのサンプルだけ | 終わりの値が ECU に入るステップ（手順の `end_ms` より後のことがある。[scenario.md](scenario.md) の「動かしている間ずっと確認する」） |

- `watch.conditions[]` は変数ごとで、`variable` と `compare[]`（`op` は `eq`、`ne`、`ge`、`le`、`gt`、`lt`、`near`。`value`。`near` には `tolerance`。ほかの `op` では `tolerance` は `null`）です。
- シナリオが届かなかった手順（`not_run`）の `from_ms` と `to_ms` は `null` です。

失敗した手順の `failed` は、失敗の中身を数で持ちます（ほかの手順では `null`。人向けの文は `message`）。

```json
"failed": {
  "at_ms": 1340,
  "code": "scenario-failed",
  "conditions": [
    {
      "condition": 0,
      "value": 3050,
      "sample_ms": 1340
    }
  ],
  "writes": [],
  "write": null
}
```

- `conditions[]` は、成り立たなかった条件の全部です。`condition` は `watch.conditions[]` の添字（0 から）、`value` と `sample_ms` は、そのときの値と、その値のサンプルの時刻です。値がまだなかった条件と、`during` の区間にサンプルが 1 つもなかった条件では、どちらも `null` です。
- `writes[]` と `write` は、失敗した書き込みです（下）。書き込みの失敗でない手順では、`writes[]` は空で、`write` は `null` です。

書き込みで失敗した手順では、`writes[]` に、シナリオがその時刻に一緒に送った書き込みが全部入ります。次の例では、1 つの `set` が participant `Body` の 2 つの変数を書き、2 つ目を ECU の setter が断りました。

```json
"failed": {
  "at_ms": 20,
  "code": "scenario-write",
  "conditions": [],
  "writes": [
    {
      "variable": "Body.mode",
      "at_ms": 20,
      "value": 2,
      "point": 0,
      "applied": true
    },
    {
      "variable": "Body.level",
      "at_ms": 20,
      "value": 13,
      "point": 1,
      "applied": false
    }
  ],
  "write": 1
}
```

- `writes[]` は、失敗した要求の書き込みを、送った順に並べたものです。`set` ならその変数の全部（participant をまたいで全部）、`ramp` と `wave` なら 1 つ、`dither` ならその時刻に `dither` が出した書き込みです。書き込みの失敗でなければ空です。
- 動きのない間に `dither` が出す書き込みは、手順のものではありません。同じ時刻・同じ participant の分は、1 つの `set` の変数と同じように、まとめて ECU に届きます。そのため、`applied` の決まりも同じで、断られた入力より前の入力は、もう書き込まれています。
- `applied` は、その書き込みが ECU に入ったかどうかです。

  | `applied` | 意味 |
  |---|---|
  | `true` | ECU に入りました。手順は失敗しましたが、値は ECU に入っています。 |
  | `false` | ECU に入っていません。ECU かそのエージェントが断った、同じ `set` の中で断られた書き込みより後ろにある、送っていない、のどれかです。 |
  | `null` | わかりません。結果が来なかったか、ECU の結果からはどの書き込みが入ったかがわかりません。 |

  ECU は、1 つの `set` の書き込みを順に適用し、setter が断った書き込みで止まります。そのため、断られた書き込みより前のものはもう入っていて（`true`）、断られたものとその後ろは入っていません（`false`）。ECU のエージェントが受け取った時点で断ったとき（名前、型、範囲の誤り）は、その `set` のその participant への書き込みは、どれも入っていません。最初のステップの後に `set` がパラメータを書くときも同じで、ECU はその participant への書き込みをまとめて断ります。
- `point` は、その変数の `written` の系列の `points` の添字（0 から）で、その点がこの書き込みです。何も送っていなければ `null` です。`applied` が `false` か `null` の点は、その系列の `not_applied_from` か、それより後ろにあります。
- `write` は、失敗の原因になった書き込みの、`writes[]` の添字（0 から）です。ECU かそのエージェントが断るときに指した書き込み（名前ではなく、その participant への書き込みの中での位置で指します）か、断った participant への書き込みが 1 つだけならその書き込みです。原因が 1 つに決まらないとき（participant がいくつもの書き込みを、どれとも指さずにまとめて断った、結果が来なかった）と、書き込みの失敗でない手順では `null` です。
- 何も送る前に失敗した手順（変数に書けない、`ramp` や `wave` に始まりの値がない）では、`writes[]` はその 1 つの書き込みで、`point` は `null`、`applied` は `false` です。値を計算する前なら、`value` も `null` です。

### 値の列（`series[]`）

```json
{
  "variable": "Engine.engine.throttle",
  "source": "written",
  "encoding": "all",
  "columns": ["at_ms", "value"],
  "samples": null,
  "first_ms": null,
  "last_ms": null,
  "not_applied_from": null,
  "points": [
    [0, 50],
    [500, 50],
    [510, 47.5]
  ]
}
```

| `source` | 中身 | `encoding` |
|---|---|---|
| `written` | シナリオが送った書き込みの全部。点は `[時刻, 値]` で、時刻は、その書き込みが効く時刻。**送った値で、ECU に入ったとは限りません**（下の `not_applied_from`） | `all` |
| `sampled` | シナリオが見たサンプルの値。条件が読む変数だけ | `changes`: 値が変わったサンプルだけが点になる。`samples`（見たサンプルの数）、`first_ms`、`last_ms`（最初と最後のサンプルの時刻）を別に持つ。`written` の系列では、この 3 つは `null` |

- `columns` は、点の列の名前で、`["at_ms", "value"]` です。**シナリオが `dither` を置く入力の `written` の系列は 3 列**で、`["at_ms", "value", "base"]` です。3 列目の `base`（基準）は、揺れを足して型に丸める前の値で、`set` の値か、`ramp` や `wave` のその時刻の値です（[scenario.md](scenario.md) の「細かい揺れを重ねる」）。3 列になるのは、シナリオの文面がその入力に揺れを置くとき（`inputs` の宣言か、その機種で実行する、波形を書いた手順 `dither`）で、揺れを置く前の点も、外した後の点も、全部の点が 3 列です。実行がその手順まで進んだかどうかにはよりません（列の数は controller のタイムステップで変わりません）。読む側は、`columns` の名前で列を取ります。

  ```json
  {
    "variable": "Engine.engine.throttle",
    "source": "written",
    "encoding": "all",
    "columns": ["at_ms", "value", "base"],
    "samples": null,
    "first_ms": null,
    "last_ms": null,
    "not_applied_from": null,
    "points": [
      [0, 50, 50],
      [10, 52, 50],
      [20, 50, 50]
    ]
  }
  ```

- 記録するのは、シナリオが使う変数だけです。シナリオが書く変数に `written` の系列、条件が読む変数に `sampled` の系列があります（両方の変数には 2 つ）。順は `variables[]` の順で、1 つの変数では `written` が先です。
- 点は 1 行に 1 つです。時刻は戻りません。同じ時刻の点が続くことがあり（`ramp` の終わりと次の `set`）、順は送った順です。
- 値は、bool が `true` と `false`、enum が項目の値（名前は `variables[].items`）、数にならない real が `null` です。
- サンプルは「ステップの終わりの値、時刻はステップの始まり」です（[scenario.md](scenario.md) の「値の見え方」）。`sampled` の値は、点から次の点まで（最後の点は `last_ms` まで）続くものとして読みます。
- `changes` の「変わった」は、書いた文字が違うことです（`0` と `-0` は別、`null` が続けば同じ）。
- `written` の系列の `not_applied_from` が `null` でなければ、その添字（0 から）の点から後ろは、送ったが、ECU に入ったことがわかっていません。値が変わったように描かないでください。次の 2 つです。
  - 失敗した手順の書き込みのうち、`applied` が `false` か `null` のものの点（`failed.writes[]` のその要素の `point`）と、その後ろ。エージェントがすぐ断ったときも、ECU がステップで断ったときも付きます。同じ `set`（または同じ時刻の `dither` の書き込み）の中で ECU に入った書き込み（`applied: true`）には付きません。
  - 時刻が `scenario.over_at_ms` より後の点（どの ECU もその時刻まで進まなかった）。
- 失敗した実行と途中で終わった実行では、`not_applied_from` より前の点にも、入ったことをシナリオが確認する前に実行が終わったものがありえます（結果を見る前の書き込み。時刻が `over_at_ms` と同じ点は、ECU がその時刻のステップを動かしたかどうかが実行ごとに違います）。通った実行では、全部の点が入っています。例外は 1 つで、最後の手順と同じ時刻の `dither` の書き込みは、結果がまだ来ていないことがあります（最後の手順が通った時点で、シナリオが終わるためです）。

### 入力の線（`lines[]`）

シナリオが書く変数には、変数ごとに 1 本の線があります。手順は、時刻が来ると、自分のすることを線に置きます。`set` は値を、`ramp` と `wave` は動きを置きます。`lines[]` は、置いたもののうち、書き込みを 1 つでも送ったものを、置いた順に並べたものです（時刻の順。同じ時刻では手順の順、1 つの手順の中では書いた順）。

```json
{
  "variable": "Engine.engine.throttle",
  "kind": "ramp",
  "line": 9,
  "step": 2,
  "from_ms": 500,
  "to_ms": 1500,
  "base": 50,
  "base_source": "line",
  "shape": {
    "type": "ramp",
    "to": 80,
    "over_ms": 1000
  },
  "text": "Engine.engine.throttle to 80 over 1000 ms"
}
```

| キー | 値 |
|---|---|
| `variable` | 変数 |
| `kind`、`line`、`step` | 置いた手順の種類（`set`、`ramp`、`wave`。`dither` は下）、シナリオのファイルの行、手順の `index` |
| `from_ms`、`to_ms` | 手順の時刻と、動きの終わり（文面のとおり: `from_ms` + 長さ）。`set` は同じ値。実行が先に終わったとき、点があるのは `recorded_to_ms` まで |
| `base` | `set` は書いた値。`ramp` と `wave` は、始まりの値、または振れの中心（[scenario.md](scenario.md) の `from` と `around`）。書き方は系列の点と同じ（手順が書いた数は、そのまま）。値を自分で持つ形（`low` と `high` の `square`、0 ms から始まる `points` など）では `null` |
| `base_source` | `base` の出どころ（下の表）。`base` が `null` なら `null` |
| `shape` | `ramp` と `wave` の形: `type`（`ramp`、または波形の名前）と、手順に書いたキーと値（パラメーターを当てた後）。`set` は `null` |
| `text` | 人向けの 1 行（手順の `text` の、この変数の分） |

| `base_source` | `base` の出どころ |
|---|---|
| `written` | 手順に書いた値（`set` の値、`from`、`around`、bool の「もう一方の値」） |
| `line` | 線の値: その時刻にシナリオがその変数を置いていた値（前の `set` の値、前の動きの終わりの値。整数には丸めた値） |
| `sample` | シナリオがまだ書いていない変数の、その時点でいちばん新しいサンプル。**どのサンプルになるかは controller のタイムステップによります**（下の「記録の終わり」）。この値に戻って終わる動きの後ろの、`line` の `base` も同じです |

- `shape` にあるのは、手順に書いたキーだけです（省いたキーの既定の値は [scenario.md](scenario.md) の波形の表）。`from` と `around` は `shape` に入れず、`base` にします。`wave` には `for_ms` も入ります（`for_ms` を書く波形）。`shape` は、線を表示するためのものです。値は系列にあるので、`shape` から数え直しません。
- `base` の意味は `shape.type` で読みます: `ramp`、`lag`、`chatter` は始まりの値、`sine` などの中心のある波形は振れの中心、`pulse` は終わると戻る値です。
- **`dither` の線**（`kind` が `dither`）は、入力に重ねた揺れです。`inputs` の宣言と、手順 `dither` が置いたものが、それぞれ 1 本です。

  | キー | `dither` の線での値 |
  |---|---|
  | `line`、`step` | 手順 `dither` の行と `index`。`inputs` の宣言は、その入力の行と `null` |
  | `from_ms` | その揺れが乗った最初の書き込みの時刻（基準のない入力に置いた揺れは、最初の `set`、`ramp`、`wave` から） |
  | `to_ms` | `null`（文面に終わりがない）。**同じ変数の次の `dither` の線の `from_ms` が、この揺れの終わり**です |
  | `base`、`base_source` | `null` |
  | `shape` | 揺れの波形。`none`（外す）は `null` |

  並ぶ位置は、その時刻に手順が置いた線の後ろで、最初の書き込みを送った順です。手順 `dither` の `lines` は、その手順が置いた揺れの線の位置です。1 度も書き込みに乗らなかった揺れ（基準のないまま終わった入力、書き込みの前に次の手順が替えた揺れ）は、線になりません。
- `points` の `shape` は、点の数（`points`）と `interpolate` です。CSV ファイルから読んだ点では、`file`、`value`、`time`、`time_unit` も、書いたとおりに入ります（列の名前だけ。点の値は系列にあります）。
- 入るのは、`from_ms` が `recorded_to_ms` 以前で、**書き込みを 1 つでも送った線だけ**です（ファイルは起きたことの記録です）。線の最初の書き込みより前にシナリオが失敗したとき、その線は入らず、手順の `lines` にも入りません。例: 同じ時刻の前の手順の `set` が断られた後ろの `ramp`。1 つの手順がいくつもの変数を動かし、その 1 つの始まりの値が見つからないときの、その変数と、後ろに書いた変数。断られた書き込みは送ったものなので、その線は入ります（系列の点と同じ）。
- シナリオが始まらなかった実行では、`lines` は空です。

### 記録の終わり（`recorded_to_ms`）

`scenario.recorded_to_ms` は、記録の右端の仮想時刻です。系列には、これ以前の点だけが入ります。`lines[]` も、これ以前に置いて、書き込みを送った線だけです。

| シナリオ | `recorded_to_ms` |
|---|---|
| 失敗した（`failed`） | 失敗した手順の `at_ms`（`stop_at_ms` で終わった実行では、止まった時刻） |
| 通った（`passed`） | 手順の `at_ms` のうち、いちばん後のもの |
| 途中で終わった（`incomplete`） | 最後に見たサンプルの時刻（1 つも見ていなければ 0）。書き込みが断られた後で participant の失敗が実行の結果になったときは、その書き込みの時刻（`failed` はどの手順でも `null` ですが、`not_applied_from` は `null` ではありません） |
| シナリオが始まらなかった（`not_run`。シミュレーションは始まったのにシナリオを始められなかったときは `incomplete`） | `null`（系列は空） |

- controller はタイムステップごとにしかサンプルを見ないので、シナリオが終わったとわかる時刻（`over_at_ms`）は、ふつう `recorded_to_ms` より後です。その間のサンプルと書き込みは、ファイルに入れません。エージェントがすぐ断った書き込みで失敗した実行だけ、`over_at_ms`（断られた時点のタイムステップ）が `recorded_to_ms`（その書き込みの時刻）以前です。
- **同じ実行からは、同じファイルができます。** 入っているのは仮想時刻で決まるものだけです（実時間、プロセス ID、遅れの数は入りません）。速さ（`--speed`）を変えても同じです。対象は、通った実行と、サンプルか待ち時間で失敗した実行です。ECU 自身が実時間や乱数で動くなら、値の列も変わります。
- 通った実行と、サンプルか待ち時間で失敗した実行では、`over_at_ms` と `time_step_ms` を除いて、controller のタイムステップ（`time_step_ms`）を変えても同じファイルです。ただし、次のものはタイムステップによります（シナリオの結果がタイムステップによる所と同じです。[scenario.md](scenario.md) の「時刻」と「結果」）。
  - 成り立った待つ手順（`wait_until`、`within_ms` つきの `expect`）の `end_ms` と、その後ろの手順の時刻。
  - ある手順が失敗した実行での、ほかの手順（失敗した手順より後ろの手順と、進行中だった手順）の `status`（`passed`、`unfinished`、`not_run` のどれにもなりえます）と、`start_ms`、`at_ms`、`end_ms`。失敗がわかる前に controller がどこまで先を送り、どこまで結果を見たかが、タイムステップで決まるためです。このとき、**`at_ms` が `recorded_to_ms` より後の `passed` の手順がありえます。その手順の書き込みの点は、系列にありません**（系列と `recorded_to_ms` は、タイムステップによりません）。
  - 書き込みが適用されずに終わった実行の全部。`dither` が動きのない間に出す書き込み（手順のものでない書き込み）が断られたときは、失敗する手順（その時点でまだ決まっていない最初の手順）もタイムステップによります。その書き込みが、最後の手順が決まるタイムステップより後に送る分なら、タイムステップによっては送らずに通ります（[scenario.md](scenario.md) の「手順が失敗したとき」）。
  - シナリオがまだ書いていない変数を、`from` や `around` を省いた `ramp` や `wave` で動かすときの、始まりの値（`lines[]` の `base_source` が `sample` の線の `base`）と、その動きの値。その動きが始まりの値に戻って終わるとき（`around` を省いた `sine` など、中心に戻る波形と、`pulse`）は、後ろに続く、`from` や `around` を省いた動きの `base`（`base_source` は `line`）も、同じサンプルから来るので同じように変わります。controller は、時刻 T の書き込みを、T より前の自分のタイムステップで送ります。始まりの値は、その時までに見たいちばん新しいサンプルなので、タイムステップが粗いと古いサンプルになり、1 つも見る前（T が controller の最初のタイムステップの長さ以下。タイムステップが 30 ms なら、30 ms までの時刻）なら、手順は `scenario-write`（始まりの値がない）で失敗します。タイムステップによらない結果にするには、`from` か `around` を書くか、先に `set` します。
- `incomplete` の実行と、`stop_at_ms` で終わった実行は、どこまで見たかが実時間で決まることがあるので、同じファイルになるとは限りません。

### 上限

系列の点が多すぎるときは、**古い側を捨て、新しい側を残します**（失敗のある終わりの側は、必ず全部残ります）。

- 上限は、系列ごとに 100,000 点、全部の系列を合わせて 1,000,000 点です（残る点の数の上限です）。数えるのは、`sampled` の点と、`written` のうち `ramp` と `wave` の点です。`set` の点は数えず、捨てません。
- 全体の上限を超えたときは、点の多い系列から順に、同じ数になるまで古い側を捨てます。
- `limits` に、上限（`points_per_series`、`points_total`）と、点を捨てた系列（`dropped[]`）があります。捨てていなければ `dropped` は空です。`sampled` の `samples` と `first_ms` は、捨てた点の分も含みます。

  | `dropped[]` のキー | 値 |
  |---|---|
  | `variable`、`source` | どの系列か |
  | `dropped` | 捨てた点の数 |
  | `dropped_from_ms` | 捨てた点のうち、いちばん古いものの時刻 |
  | `dropped_before_ms` | 捨てる対象の点（`sampled` は全部、`written` は `ramp` と `wave` の点）のうち、残ったいちばん古いものの時刻 |

- **`dropped_from_ms` から `dropped_before_ms` の手前までは、値が欠けています。** 描く道具は、この区間をまたいで点をつながないでください。`written` の系列では、`set` の点がこの区間の中や前に残ります（その時刻に送った値としては正しい）が、その点から `dropped_before_ms` までの間の `ramp` と `wave` の値はありません。
- 点を捨てた実行では、標準エラーの行が `[vehicle_sil_launch] timeline: <file> (without <N> older points, dropped at its limits: limits.dropped in the file)` になり、結果に注記 `timeline-limit`（捨てた点の数）が入ります。ファイルを開かなくても、欠けたことがわかります。

## シナリオをまとめて実行する（`vehicle_sil_launch test`）

`vehicle_sil_launch test <file> --scenarios <ディレクトリかファイル>...` は、設定ファイルの機種ごとに、並べたシナリオを 1 つずつ実行し、全部の結果を 1 つの表にします。CI で「全部の機種で、全部のシナリオが成功するか」を 1 つのコマンドと終了コードで確認するためのものです。

```sh
vehicle_sil_launch test simulation.yaml --scenarios scenarios/                      # 全部の機種 × ディレクトリのシナリオ
vehicle_sil_launch test simulation.yaml --variant medium --scenarios a.yaml b.yaml  # 1 つの機種 × 並べたファイル
```

### 実行する組と順序

実行する組は、機種 × シナリオの全部です。

| 項目 | ルール |
|---|---|
| 順序 | 機種が設定ファイルに書いた順、その中でシナリオが引数の順です。 |
| 機種のない設定ファイル | シナリオだけを並べます。 |
| `--variant <名前>` | その機種だけを実行します（繰り返せます。順は設定ファイルの順のままです）。 |
| 1 組の実行 | `run <file> --variant <機種> --scenario <シナリオ> --speed max` と同じです。 |
| 速度 | 設定ファイルの `speed` によらず、最速です。 |
| 組と組の間 | registry、controller、participant のプロセスを作り直します（前の組の状態は残りません）。 |
| 同時の実行 | しません。 |

### シナリオの指定（`--scenarios`）

`--scenarios` には、ディレクトリかファイルを書きます。いくつでも並べられ、`--scenarios` も繰り返せます。`--scenarios` は、次の `--` で始まる引数までを全部シナリオとして読むので、設定ファイルは `--scenarios` より前に書きます。後ろに書くなら `--` の後です（`test --scenarios scenarios/ -- simulation.yaml`）。

| 書いたもの | 実行するシナリオ | 順序 |
|---|---|---|
| ディレクトリ | 直下にある、名前が `.yaml`、`.yml`、`.json` で終わるファイルの全部 | ファイルの名前を UTF-8 のバイト列として比べた順 |
| ファイル | 拡張子によらず、そのファイル | 書いた順 |

- ディレクトリでは、拡張子の大文字と小文字を区別せず、下のディレクトリには入りません。
- ディレクトリの順序は、どの OS でも同じです（大文字は小文字より前）。
- ディレクトリのシナリオは、表に `<書いたディレクトリ>/<名前>` で表示します。
- ディレクトリに、シナリオでない YAML は置きません（置くと、その組は照合で失敗します）。
- ファイルのパスは、設定ファイルと同じく、今のディレクトリからのものです。
- ないパス、シナリオのファイルが 1 つもないディレクトリは、引数のエラーです（何も実行しません）。

### 飛ばす組、失敗した組、中断

| 場合 | `test` がすること |
|---|---|
| シナリオの `only` に機種がない組 | 実行せずに `skipped` にします。 |
| 全部の組が `skipped` | 表の後に警告 `test-nothing-run`（`nothing was run: ...`）を 1 行書きます。 |
| 失敗した組がある | 残りも実行します。 |
| `--fail-fast` を付けて、失敗した組がある | 最初の失敗の後は実行しません（残りは `not run`）。 |
| Ctrl+C（1 回目） | 実行している組を `run` と同じ順で止め、残りを実行せずに、そこまでの表を書きます（終了コード 130）。 |
| Ctrl+C（2 回目） | `run` と同じで、participant が終わるのを待たずに終了させます。 |

`only` に機種がない組は、`run` ではエラー `scenario-variant` ですが、`test` では失敗にしません。その組の設定やシナリオにほかのエラーがあれば、`skipped` ではなく失敗です。

全部の組が `skipped` になるのは、`--variant` で絞った機種が、どのシナリオの `only` にもないときなどです。このときは、何も確認していません。既定では成功で（終了コード 0）、`--strict` では失敗です（終了コード 1）。

### 使える引数

| 引数 | `test` での扱い |
|---|---|
| `--json`、`--strict`、`--describe-timeout-ms`、`--disable`、`--enable` | 使えます（どの組にも同じように適用します）。 |
| `--timeout-ms` | 使えます（1 組ごとの上限）。 |
| `--param <名前>=<値>` | そのパラメーターを宣言しているか使っているシナリオにだけ適用します。 |
| `--speed`、`--log-dir`、`--events`、`--stdin-commands`、`--control`、`--manual` などの `run` だけの引数 | 引数のエラーです。 |
| `--scenario` | 引数のエラーです。 |

`--param` の名前がどのシナリオにもなければ、引数のエラーです（何も実行しません）。打ち間違いを黙って無視しないためです。

### 設定ファイルの扱い

最初の組の前に、設定ファイルを読みます。読めなければ、何も実行せずに終了コード 3 です。設定ファイルに照合のエラーがあるときは、どの組も失敗になります（組ごとに照合するためです）。

`test` は、どの組も自分で起動します。実行する機種の設定に、有効で `manual: true` の participant（人が起動するもの）がいると、組ごとに人を待つことになります。そのため、最初の組の前に引数のエラーにします（終了コード 4。participant の名前をメッセージに書きます）。

| したいこと | 方法 |
|---|---|
| その participant なしで実行する | `--disable <名前>` で無効にします。 |
| その participant と一緒に動かす | `run --scenario` で 1 つずつ実行します。 |

`--disable` で無効にしたとき、その participant の変数を使う手順のあるシナリオは、その組の照合で失敗します。

### 出力

標準エラーには、`run` と同じ行（participant の出力、ランチャーの行）を書きます。組の前後に、次の行が入ります。

```text
[vehicle_sil_launch] test 2 of 4: variant small, scenario scn/turbo.yaml
[vehicle_sil_launch] test 2 of 4: variant small, scenario scn/turbo.yaml: skipped (the scenario is only for medium (its only))
```

標準出力には、次の順に書きます（時刻、所要時間、プロセス ID は入りません）。

| 順 | 内容 |
|---|---|
| 1 | 組ごとに 1 行の表 |
| 2 | 失敗があれば、1 組だけを実行し直すコマンド |
| 3 | `test` 全体の指摘（あれば） |
| 4 | 結果の 1 行 |

```text
variant  scenario        result
small    scn/idle.yaml   passed
small    scn/turbo.yaml  skipped  the scenario is only for medium (its only)
medium   scn/idle.yaml   FAILED   line 4: expect Engine.rpm ge 650: not true at 100 ms: Engine.rpm ge 650, but it was 640 (its sample of 100 ms)
medium   scn/turbo.yaml  passed   1 of 3 steps was skipped, 1 warning

to see a failed run in full, run it alone: vehicle_sil_launch run sim.yaml --speed max --variant medium --scenario scn/idle.yaml

FAILED: 4 runs, 2 passed, 1 failed, 1 skipped
```

| 表の `result` | 意味 | 右の列 |
|---|---|---|
| `passed` | その組の `run` が成功した | 飛ばした手順の数、確認しなかった `expect` の数、警告の数（あるものだけ） |
| `FAILED` | 照合か実行が失敗した（`run` の終了コードが 1 か 3） | 最初のエラー（シナリオのエラーなら行つき）と、残りの数 |
| `skipped` | シナリオの `only` に、この機種がない | `only` の機種 |
| `interrupted` | この組の途中で中断された | 中断のシグナルかキー |
| `not run` | 実行しなかった（中断、`--fail-fast`、または前の組でランチャーの SIL Kit の接続が閉じなかった） | 理由 |

- `passed` の「確認しなかった `expect`」は、`ramp` と `wave` の横の `expect` です（`1 expect was not checked`）。
- `passed` の警告の中身は、その組を 1 つだけ実行するか、`--json` の `runs[].run` で見ます。
- `FAILED` が `--strict` で警告だけのときは、右の列は最初の警告です。
- 機種のない設定ファイルでは、`variant` の列がありません。

1 組だけを実行し直すコマンド（`to see a failed run in full, ...`）は、最初に失敗した組のものです。`test` に付けた引数のうち、その組に適用されるものを並べるので、同じ実行になります。並べるのは、`--param`（その組のシナリオにある名前だけ）、`--disable`、`--enable`、`--strict`、`--timeout-ms`、`--describe-timeout-ms` です。コマンドには、`test` の実行と同じ `--speed max` が付きます。`role: bridge` の participant が外とつないでいるために速さ 1 で実行した組には付きません（[外につながる実行の時間](#外につながる実行の時間--time-policy)）。`test` に `--time-policy free` を付けていれば、それも付きます。空白などを含むパスと値は、POSIX のシェル（sh、bash）に貼れるように `'...'` で囲みます。Windows のコマンドプロンプトでは `'` が引用にならないので、`"..."` に書き換えます。

最後の行は、`passed`、`FAILED`、`interrupted` のどれかと、組の数です。`interrupted` と `not run` の数は、あるときだけ書きます。

| 場合 | 最後の行の末尾 |
|---|---|
| 接続が閉じなかったので、残りを実行しなかった | `(the launcher's connection to the simulation did not close)` |
| `--strict` を付けた | `(--strict)` |

### `test` 全体の指摘

`test` 全体の指摘は、表の後に `<重大度>: <設定ファイル>: <文> [<code>]` の 1 行で書きます（`--json` では `findings[]`）。

| `code` | 重大度 | 意味 |
|---|---|---|
| `test-nothing-run` | 警告 | 実行した組が 1 つもない（全部が `skipped`）ので、何も確認していない（`--strict` では失敗） |
| `test-runs-left` | エラー | 中断されていないのに、実行しなかった組がある |
| `test-speed-set` | 注記 | `role: bridge` の participant が外とつないでいるために、最速でなく速さ 1 で実行した組がある（`test` 全体で 1 回。最初のそういう組の注記 `outside-speed-set` の文を添える） |

`test-runs-left` は、ある組の後でランチャー自身の SIL Kit の接続が閉じなかったときに出ます（その組の警告 `connection-not-closed`。[止め方](#止め方)）。このとき `test` は、同じプロセスで次のシミュレーションを始めずに終わっています。`not run` の組を、`--variant` と `--scenarios` で選んで実行し直します。

### `test` の終了コード

| 終了コード | 意味 |
|---|---|
| 0 | 失敗した組も、実行しなかった組もない |
| 1 | 失敗した組が 1 つ以上ある |
| 1 | 中断されていないのに実行しなかった組がある（`test-runs-left`） |
| 1 | `--strict` で、実行した組が 1 つもない（`test-nothing-run`） |
| 3 | 設定ファイルを読めない（何も実行していません） |
| 3 | 結果を書けない、`vehicle_sil_launch` 自身の想定外の失敗 |
| 4 | 引数のエラー（何も実行していません） |
| 130 | 中断された（その前に失敗した組があっても 130） |

`skipped` は失敗に数えません。全部が `skipped` でも、`--strict` がなければ 0 です。設定ファイルの `manual: true` の participant のエラーも 4 です。

### JSON（`test --json`）

1 つの JSON のオブジェクトです。ルール（キーの順、`null`、字下げ、改行、バージョンの上げ方と読む側の扱い）は照合の JSON と同じです。

| キー | 値 |
|---|---|
| `vehicle_sil_test` | 形式のバージョン（今は 1） |
| `vehicle_sil` | `vehicle_sil_launch` の vehicle-sil のバージョン |
| `file` | 設定ファイル（引数に書いたとおり） |
| `result` | `"passed"`、`"failed"`（終了コード 1 の全部）、`"interrupted"`、`"unreadable"`、`"usage"` |
| `strict` | `--strict` を付けたか |
| `variants` | 実行する機種（設定ファイルの順）。機種のない設定ファイルでは空 |
| `scenarios` | 実行するシナリオ（表に出る名前、実行する順） |
| `passed`、`failed`、`skipped`、`interrupted`、`not_run` | それぞれの結果の組の数 |
| `runs[]` | 組ごとの結果（下の表）。実行する順 |
| `findings[]` | 何も実行しなかった理由（`result` が `"usage"` か `"unreadable"` のとき）と、`test` 全体の指摘（`test-nothing-run`、`test-runs-left`、`test-speed-set`）。なければ空 |

`findings[]` は、照合の `findings[]` と同じ形です。

`runs[]` のキー:

| キー | 値 |
|---|---|
| `variant` | 機種（機種のない設定ファイルでは `null`） |
| `scenario` | シナリオ |
| `result` | `"passed"`、`"failed"`、`"skipped"`、`"interrupted"`、`"not_run"` |
| `reason` | 表の右の列（なければ `null`） |
| `run` | その組の `run --json` と同じオブジェクト（`skipped` と `not_run` では `null`） |

`version --json` の `formats` の `test` が、この JSON のバージョンです。`features` には `test` の項目はありません。

## 関連するドキュメント

- 設定ファイルの書き方と引数の一覧 → [simulation-config.md](simulation-config.md)
- 照合と、照合の指摘 → [simulation-check.md](simulation-check.md)
- `run --events` のイベントと、標準入力のコマンド → [simulation-events.md](simulation-events.md)
- シナリオの書き方 → [scenario.md](scenario.md)
- 実行中の変数をプログラムから読み書きする → [control-api.md](control-api.md)
- 複数の PC で動かす手順 → [howto/multi-pc.md](howto/multi-pc.md)
