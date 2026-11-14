# シミュレーションの実行（`vehicle_sil_launch run`）

まとめる人と、シミュレーションで試験をする人向けです。先に [simulation-config.md](simulation-config.md)（構成ファイル）と [simulation-check.md](simulation-check.md)（照合と所見）を読みます。次は、外から操作するなら [control-api.md](control-api.md)、シナリオで試験をするなら [scenario.md](scenario.md)、プログラムから動かすなら [simulation-events.md](simulation-events.md) です。引数と終了コードは [simulation-config.md](simulation-config.md) の「引数と終了コード」にあります。

`vehicle_sil_launch run <file>` は、次の順に動きます。

1. `check` と同じ照合をします。誤りがあれば（`--strict` では警告も）、何も起動せずに照合の結果を書き、終了コード 1 です。機種（`variants`）を書いた構成では、`--variant <名前>` で選んだ機種を照合し、その機種の実行ファイル、変数の対応、participant で動かします（[simulation-config.md](simulation-config.md) の「機種」。`--variant` がなければ引数の誤りで、何も起動しません）。
2. registry をこのプロセスの中で立てます（`registry_uri`、なければ 127.0.0.1 の空いているポート）。標準エラーの最初の行が、その URI です（`[vehicle_sil_launch] registry at silkit://127.0.0.1:<port>`）。`--control` を付けたときは、その前に Control Server を立て、`[vehicle_sil_launch] control server at <address>` の行を書きます。
3. controller（participant 名 `SimulationController`）をこのプロセスの中で動かします。シミュレーションが待つ（必須の）participant は、外していない `required: true` のものです。controller は刻み（`time_step_ms` か、その既定値）で時刻同期に参加し、刻みの終わりを止めて、速さ、一時停止、ステップ送りを決めます（最速のときも同じです。刻みごとに同期が 1 回増えます）。
4. 外していない participant を構成ファイルの順に、それぞれ別のプロセスで起動します（`manual` の participant は起動せず、起動するコマンドを書きます）。作業ディレクトリは構成ファイルのあるディレクトリ、標準入力は空です。
5. 必須の participant がそろったら、変数を出す ECU とつながるのを待ってから（下の「変数を読み書きする口」。ふつうは待ちません）始めます。`--control` と `--start-paused` を付けたときは、どの participant も時刻 0 のステップを始める前で、`resume`、`step` か `run_until` の命令を待ちます（[control-api.md](control-api.md) の「始まる前に書く」）。`stop_at_ms` があれば、仮想時間がそこに達したところで止めます。なければ、participant が止めるか、Ctrl+C まで動きます。
6. 終わったら、すべての participant を終わらせ（下の「止め方」）、結果を書きます。

`--scenario <file>` を付けると、照合でシナリオも確かめ、始まった後はシナリオの手順を進めて、シナリオが終わった所でシミュレーションを止めます（[scenario.md](scenario.md)）。このとき、controller の刻みは、速さを決めたときの既定（`time_step_ms` か、ECU のステップがすべて controller のステップから始まる刻み）です。シナリオのパラメーター（`${名前}`）の値は、構成の `params`、機種の `params`、`--param <名前>=<値>` で替えられます。その機種にない変数（`absent: true`）や、機種が外した participant の変数を使う手順は飛ばし、結果に出します（飛ばした手順の `at_ms` は null。確かめる手順が全部飛ぶと、照合の警告 `scenario-nothing-checked`）（[scenario.md](scenario.md) の「機種とパラメーター」）。

## 見張ること

- 始まる前: 必須の participant のプロセスが終わったら、`start_timeout_ms` を待たずにやめます（`ended-before-start`）。`start_timeout_ms` の間に参加しなかった必須の participant は、全部を挙げます（`not-joined`）。
- 実行中: 必須の participant のプロセスが 0 でない終了コードで（またはシグナルで）終わったら、すぐにシミュレーションを中断します。controller から見て、必須の participant がシミュレーションを去ったときも中断します（Error の状態ならすぐに、ほかの状態なら 5 秒たってもシミュレーションが終わらないとき）。この規則は `vehicle_sil_sim_controller` と `vehicle_sil::Simulation` と同じです。中断の理由になった participant は `participant-failed` です。中断されても、ほかの ECU は自分で終わります（終了コード 0）。
- `required: false` の participant は待たず、終わってもシミュレーションを止めません。0 でない終了コードは警告です。
- `--timeout-ms <ms>` を付けると、シミュレーションが始まってから `<ms>` の実時間で終わらなければ中断し、誤り（`timeout`）にします。ステップの中で止まった ECU がいると仮想時間が進まず、`stop_at_ms` があっても終わらないためです（CI などで使います）。一時停止している時間も数えます。既定は上限なしです。manual の必須の participant がいる実行では、participant を待ち始めた時点から数えます（[participant を自分で起動する](simulation-config.md#participant-を自分で起動するmanual--manual)）。
- 実行中に、一時停止していないのに仮想時間が 10 秒進まないと、`[vehicle_sil_launch] virtual time has not advanced for 10 s (at t=<ms> ms, not paused): ...` の行を書きます。考えられる原因（ほかのコンピューターの participant のネットワークが切れた、デバッガーで止めている、ステップの中で固まった）と、`--timeout-ms` を挙げます。同じ止まり方が続けば 60 秒と 10 分にも 1 回ずつ書き、また進んだら `virtual time advances again: it stood still for <s> s at t=<ms> ms` を書きます。`run` は待ち続けます（中断しません）。速さを指定したときは、刻みの期限を過ぎた時間で数えます（遅い速さで期限を待つ間は数えません）。所見にはしません（`--events` では `log` の事象です）。ネットワークが切れても、SIL Kit は接続が切れたことに長い間気づきません（Linux では十数分）。
- 一時停止している間も、上の見張りはどれも同じように効きます（participant の失敗、止める要求、`--timeout-ms`）。`start_timeout_ms` は、そろうまでの時間です。`--start-paused` で 0 に止まっている時間は含みません。
- 始まる前の窓（変数を出す ECU を待つ間と、`--control` と `--start-paused` で時刻 0 のステップの前に止まっている間）に必須の participant が抜けると（プロセスが終わった、接続が切れた）、すぐに開始をやめます（`ended-before-start`。プロセスが動いているまま抜けたときは `simulation-start`）。
- 始まるまでの上限: `start_timeout_ms` は、そろうまでの時間で、そろった後の開始（最初のステップまで）にも、その残り（少なくとも、変数を出す ECU を待つ時間 + 1 秒）が効きます。`--control` と `--start-paused` で時刻 0 のステップの前に止まっている間は数えず、進んだ後に改めて `start_timeout_ms` が効きます。過ぎると `simulation-start` の誤りで、最初のステップに着いていない participant と状態を文言に挙げます。
- 利用者が起動する（manual の）必須の participant がいる実行では、manual の participant が参加するまでと、全員が参加した後、manual の participant が最初のステップの準備を終えるまでは上限がありません。`run` が起動した participant が参加するまでと、manual の participant の準備が終わった後には、`start_timeout_ms` が効きます（[participant を自分で起動する](simulation-config.md#participant-を自分で起動するmanual--manual)）。
- `--timeout-ms` は、シミュレーションが始まってから数えます。`--control` と `--start-paused` では、時刻 0 のステップの前に止まった時点から数えます（止まっている時間も数えます）。止まったまま過ぎると、時刻 0 のステップを走らせてから中断し、`timeout` の文言に、最初のステップの前に止まっていたことを書きます。進めた後に過ぎたときは、`timeout` の文言に、そのうち何 ms 止まっていたかを書きます。
- `stop_at_ms` を書いたのに、participant がそれより前にシミュレーションを止めたときは、注記（`ended-before-stop-at`）を出します（`--strict` でも失敗にしません）。

## 変数を読み書きする口

`vehicle_sil_launch run` は、controller の participant の上に、ECU の変数を読み書きする側（`vehicle_sil::control::Hub` と `vehicle_sil::control_silkit::HubTransport`）を持ちます。`ecu_main` で作った ECU は、自分から SIL Kit の publish/subscribe でつながります（どの ECU にも `ecu.connected` があるので、変数を宣言していない ECU もつながります）（[ecu-executable.md](ecu-executable.md) の「変数を出す」）。構成ファイルに書くことはありません。gRPC は使いません。しくみと約束は [internals/control.md](internals/control.md) の「SIL Kit の上の運び手」にあります。

- 変数を出すかどうかは、ECU の実行ファイルが `--describe` で自己申告します（`control`: 運び手と、メッセージの形式の版。[ecu-executable.md](ecu-executable.md) の「JSON の形式」）。照合の段階で、出さない ECU と版の違う ECU がわかります（下の表の所見は、照合の所見です）。
- 始まる前: 必須の participant がそろうと、全員が最初のステップの前で待ちます。その間に、変数があり、それを `vehicle_sil_launch` と同じ版で出すと申告した、必須で外していない ECU の hello と、「コマンドを受け取れる」という返事を待ちます。SIL Kit が participant をつなぐ順序から、hello は窓が開く前に届いているので、ふつうは待ちません。申告したのに来ない ECU がいるときだけ、最長 5000 ms 待ちます。待っている間に Ctrl+C か止める命令が来れば、待つのをやめます。この待ちは `start_timeout_ms` の後に足されることがあります（そろうのが期限の間際だったとき）。申告していない ECU は待ちません。
- 構成ファイルの `participants[].variables`（対応表）があれば、その participant の変数は、対応の後の名前と値で読み書きします（[変数の名前と値を合わせる](simulation-config.md#変数の名前と値を合わせるvariables)）。シナリオと Control API のクライアントが見るのは、対応の後だけです。
- つながり方は participant ごとに 5 つで、結果の `participants[].control_link` と、事象 `control_link` に出ます。シミュレーションが始まった時点の状態です（後から変わっても書き直しません）。

| `state` | 意味 | 所見 |
|---|---|---|
| `connected` | hello が届き、コマンドを受け取れると返事があった。変数を読み書きできる | なし |
| `no_hello` | 変数を出すと申告したのに、待っても hello（か返事）が来なかった。この実行では変数を読み書きできない | 警告 `control-link-no-hello`（実行の所見） |
| `version_mismatch` | メッセージの形式の版が `vehicle_sil_launch` と違う（申告の版か、届いた hello の版）。この実行では変数を読み書きできない | 警告 `control-link-version`（申告でわかれば照合の所見、hello で初めてわかれば実行の所見） |
| `not_offered` | 変数はあるが、出すと申告していない（`--no-control`、申告のない古い vehicle-sil の実行ファイル）。待ちません。変数は読み書きできない | 注記 `control-link-not-offered`（照合の所見） |
| `none` | つなぐものがない（変数がない古い vehicle-sil の実行ファイル、`describe: false`）。申告していないのに hello が届いた participant は `connected` です | なし |

- どの場合も、シミュレーションは動きます（`--strict` では警告が失敗です）。
- 実行中: 時間を止めたとき（`--start-paused`、`pause`、`step` の後）、止まった仮想時刻 P より前に始まった各 ECU のステップの値と、書き込みの結果は、すべて届いています。ちょうど P に始まるステップの分は、止まっている間に届きます。この約束に反して遅れて届いたメッセージがあれば、警告 `control-link-late-values` を出します（`ecu_main` で作った ECU では起きません。数は結果の `control_link.late_messages`）。
- 始まった後に参加した participant（`required: false`、手で起動した実時間の Peer、同じ名前での起動し直し）は、hello の後、「コマンドを受け取れる」という返事が届いてから書けるようになります（ふつうは一瞬です）。それまでの書き込みと、シミュレーションを去り始めた participant（ライフサイクルが止まり始めた、失敗した）への書き込みは、送らずにすぐ Invalid で確定します。
- 値は間引きません。ECU は Agent の `sample_period`（既定 10 ms の仮想時間）ごとに全変数を送ります。ECU が多く、変数が多く、速く回すほど、量が増えます。重いときは、ECU の `sample_period` を大きくします。
- ECU と `vehicle_sil_launch` の間に認証はありません。registry に参加できる者は、ECU の変数に書けます（バスにフレームを送れるのと同じです）。registry は既定で 127.0.0.1 だけで待ち受けます。
- この口を使うのは、`vehicle_sil_launch` の中の Hub です。`run --control <address>`（Control Server を持つビルドだけ）で、Hub を gRPC の Control API としてクライアント（Python、CLI、GUI）に出します。使い方は [control-api.md](control-api.md) の「ランチャーにつなぐ」です。

## 等速で動かす

`speed`（構成ファイル）か `run --speed` を指定すると、controller は刻み（`time_step_ms`）ごとに、仮想時間を実時間に合わせます。`speed: 1` では、仮想時間 1000 ms に実時間で 1000 ms 以上かかります。実時間で動く機器と一緒に動かすとき、人が画面を見ながら操作するときに使います。

実時間で動く機器は、シミュレーションを待ちません。シミュレーションが実時間に遅れると、機器から見た結果が変わります。そのため、遅れを次の定義で数えて報告します。

遅れの定義:

- **期限**: 刻み（仮想時刻 T に始まる controller のステップ）を始めてよい実時間です。基準の実時間 +（T − 基準の仮想時刻）÷ 速さ、で決まります。基準は最初の刻みでとり、再開のたびにとり直します。途中で速さを（数から数へ）変えても、次の刻みの期限は動きません。どの刻みも、期限より前には始まりません。
- **遅れ**: 刻みが実際に始まった実時間から、期限を引いたものです（負なら 0）。刻みが始まるのは、controller が前の刻みを放し、その刻みより前に始まる participant のステップが全部終わったと controller が知ったときです。controller 自身が起きるのが遅れた分も入ります（機器からは区別がつかないためです）。
- **遅れた刻み**: 遅れが `late_tolerance_ms`（既定は実時間の 10 ms）を超えた刻みです。
- **ずれ**: 遅れた刻みの後、シミュレーションは追いつこうとしません。その刻みを新しい基準にして、そこから指定の速さで進みます。それ以後の仮想時間は、遅れた分だけ後ろにずれたままです。ずれの合計が `slipped_ms` です。許容の内の遅れでは基準を変えないので、次の刻みで自然に取り戻します。
- **数える範囲**: 基準をとった次の刻みから、最後の刻み（`stop_at_ms` の刻みを含みます）までです。期限を過ぎてもまだ始まっていない刻みは、そのときまでの遅れで数えます。participant が止まったまま、時間切れ、失敗、停止で実行が終わっても、その遅れは結果に残ります。一時停止していた時間は数えません。最速（`max`）の間は、どの刻みも遅れません。最速にした時点で期限を過ぎていた刻みは、そのときの遅れを保ち、始まったときに 1 回だけ数えます（最速にしていた間の実時間は遅れに入りません）。始まる前に数の速さに戻すと、その刻みの期限は「戻した時刻 − 保っていた遅れ」です。

遅れたときに、実時間で動く機器から見て起きること:

- ある刻みが 490 ms 遅れると、それ以後のメッセージは、すべて 490 ms 遅れて出ます。周期は変わりません（10 ms 周期のメッセージは、その後も 10 ms おきに出ます）。
- 追いつかないのは、追いつくと、たまった分（この例では約 49 周期分）のメッセージが数 ms の間にまとめて出て、受ける側の周期やタイムアウトの判定を乱すためです。
- 遅れた刻みの数は、遅れが起きた回数です。1 回の遅いステップは、どれだけ長くても遅れた刻み 1 つです。ECU のステップがいつも実時間に間に合わないシミュレーションは、ずれ続けます。全体でどれだけ遅れたかは、`slipped_ms` と `actual_speed` でわかります（かかった実時間は、仮想時間 ÷ 速さ に `slipped_ms` を足したものより短くなりません）。
- 1 つの刻みの遅れ（`max_late_ms`）は、大きめに出ることがあります。participant があるステップを終えてすぐ次のステップに入ると、終わったことは次のステップの後で controller に伝わります。controller はその分（最大で、その participant の 1 ステップにかかる実時間）だけ遅く知ります。小さく出ることはありません。
- そのため、ステップの計算が続けてステップの枠（ステップ幅 ÷ 速さ の実時間）をはみ出す ECU では、許容が、次の 1 ステップの計算時間の分だけ小さく効きます。大きめに測った遅れが許容を超えると、その刻みは遅れた刻みになり、基準をとり直すので、以後は実際にその分だけずれます。こういう ECU があるときは、`late_tolerance_ms` を「機器が待てる時間 + ECU の 1 ステップの計算時間」を目安に決めます。

刻みと ECU のステップ:

- 合わせるのは刻みの境目だけです。仮想時刻 T に始まる ECU のステップは、T 以後の最初の刻みの期限に動きます。刻みの間に始まるステップは、次の刻みの期限に、まとめて動きます。T にあたる実時間より遅くなり、早くはなりません。
- 遅くなる量は、最大で「刻み −（刻みと ECU のステップ幅の最大公約数）」です。刻みが 10 ms なら、ステップ幅 1 ms の ECU で最大 9 ms、15 ms の ECU で最大 5 ms です。ステップ幅が刻みの倍数の ECU では 0 です。この量は遅れの数には入りません。結果の `timing` の `step_delay_ms` に出ます。
- そのため、速さを指定して `time_step_ms` を書かなければ、刻みは、10 ms、`stop_at_ms`、ECU のステップ幅（外していない ECU が照合で自己申告したもの）の最大公約数になります。ECU が 10 ms と 5 ms なら 5 ms、4 ms と 6 ms なら 2 ms、20 ms だけなら 10 ms です。ステップ幅がわからない participant（`describe: false`）があるときと、最大公約数が 1 ms の倍数でないときは、1 ms です。
- `time_step_ms` を書いたときは、その値を使います。速さの指定があり、ECU のステップが刻みの境目で始まらないときは、警告 `coarse-time-step` を出します（最大で何 ms 遅れてまとまるか、何と書けばよいか）。最速で始めて、途中で `speed` の命令を受けたときも同じです（刻みは始めに決まり、途中で変わりません）。ファイルに `speed` も書いてあれば、`check` の時点で同じ警告を出します（`run` ではその 1 つだけ。`run --speed` で速さを変えたときは、`run` が変えた速さで判断します）。
- 刻みを細かくすると、時刻同期の回数が増えます。最速のときの既定が 10 ms のままなのは、そのためです。

そのほか:

- 合わせるために待つだけです。いちばん遅い participant より速くはなりません。
- 遅れた刻みが 1 つでもあれば、警告 `behind-real-time` を出します（`--strict` では失敗）。数は、結果の `timing`、人向けの出力の `time:` の行、`--events` の `time` にあります。
- 遅れたときは、速さを下げるか、遅い原因を探します。1 ステップの計算が重い ECU は、そのステップを軽くします（ステップ幅を大きくしても、1 ステップが重ければ遅れは減りません）。小さいステップの ECU が多くて同期の回数が多いときは、ECU のステップ幅と `time_step_ms` を大きくします。`vehicle_sil_launch` は、どの participant が遅いかを知りません（SIL Kit は各 participant のステップにかかった時間を controller に伝えません）。ECU を 1 つずつ外して（`--disable`）比べます。
- `late_tolerance_ms` の既定の 10 ms は、実時間で動く機器のタイムアウトに比べて十分小さく、OS のタイマーの揺れより十分大きい値として選んでいます。実時間の長さで、刻みと速さによりません。機器が待てる時間に合わせて変えます。
- 最速（`max`）では合わせず、遅れも数えません。
- 一時停止、ステップ送り、途中での速さの変更は、標準入力の命令と、Control Server（`--control`）のクライアントの呼び出しです（[標準入力の命令](simulation-events.md#標準入力の命令run---stdin-commands)）。`run --start-paused` は、全員がそろったら仮想時間 0 で止まり、`resume` か `step` を待ちます（`--stdin-commands` か `--control` と一緒に使います）。`--stdin-commands` だけのときは時刻 0 のステップを走らせてから止まり、`--control` があるときは時刻 0 のステップの前で止まります。

## 止め方

シミュレーションが終わったら（中断と Ctrl+C のときも）、次の順に participant を終わらせます。

1. 必須の participant が自分で終わるのを、`stop_timeout_ms` まで待ちます（ecu_main の実行ファイルは、シミュレーションが終われば自分で終わります）。
2. まだ動いている participant に、止まるよう求めます（Linux: その participant のプロセスグループに SIGTERM。Windows: そのプロセスグループに Ctrl+Break）。`required: false` の participant は、ここで止めます（警告にしません）。必須の participant は誤りです（`did-not-end`）。
3. さらに `stop_timeout_ms` まで、止まるよう求めた participant と、それが起動したプロセス（プロセスグループ、Windows ではジョブオブジェクト）が終わるのを待ちます。残ったものを、それが起動したプロセスごと強制終了します（Linux: プロセスグループに SIGKILL。Windows: ジョブオブジェクトの終了）。
4. 自分で終わった participant でも、それが起動したプロセスが残っていれば終わらせ、警告にします（`left-processes`）。

利用者が起動する（manual の）participant には、どの段も当てません（[participant を自分で起動する](simulation-config.md#participant-を自分で起動するmanual--manual)の「止め方」）。

participant を終わらせた後、`run` は自分の SIL Kit の接続（変数の口、controller、registry）を閉じてから、結果を書きます。閉じるのが期限（5 秒に、`stop_timeout_ms` と 5 秒の長い方を足した時間）のうちに終わらなければ、待つのをやめ、警告 `connection-not-closed` を足して結果を書き、接続を閉じないままプロセスを終えます（結果と終了コードの決まりは変わりません）。この待ちも、Ctrl+C か `stop` の命令をもう 1 回送ると切り上げます。切り上げたときは、そう 1 行書いて、同じように終えます（求められたとおりなので、所見にはしません）。

シミュレーションが始まらなかったときは、1 を飛ばして 2 からです（その participant の誤りにはしません）。ecu_main の実行ファイルは、始まる前に止まるよう求められると、すぐに終わります（[ecu-executable.md](ecu-executable.md)）。

各 participant は、自分のプロセスグループ（Windows ではプロセスグループとジョブオブジェクト）で動きます。端末の Ctrl+C は `vehicle_sil_launch` にだけ届き、participant には届きません。Windows で `vehicle_sil_launch` にコンソールがない（participant と共有するコンソールがない）ときは、Ctrl+Break を送れないので、2 の代わりにすぐ強制終了します（文言にそう書きます）。このとき participant は、コンソールの窓を開かないように起動します。

## 中断（Ctrl+C）

Ctrl+C（SIGINT）、SIGTERM、SIGHUP（Windows: Ctrl+C、Ctrl+Break、コンソールを閉じる）を受けると、`vehicle_sil_launch` はシミュレーションを順に止め（controller の stop）、上の止め方で participant を終わらせ、結果に中断を書いて、終了コード 130 で終わります。始まる前なら、participant を止めます。シミュレーションが終わった後、participant が自分で終わるのを待っている間の中断は、その待ちをやめて、止まるよう求めます（止め方の 2）。2 回目の中断では、待つのをやめて、残っている participant をすぐに強制終了します。照合の間の中断は、照合が終わってから効きます（何も起動しません）。`vehicle_sil_launch` を起動した側が無視するように設定したシグナル（`nohup` の SIGHUP など）は、無視したままです。端末の Ctrl+Z（SIGTSTP）は `vehicle_sil_launch` だけを止めます。participant は別のプロセスグループなので動き続け、registry と controller だけが止まります。

Windows でコンソールを閉じると、Windows は数秒でプロセスを終わらせます。participant はジョブオブジェクトが終わらせますが、結果を書けないことがあります。

プログラムから `vehicle_sil_launch` を動かすときは、`--stdin-commands` を付けて、標準入力に `{"command":"stop"}` を書くか、標準入力を閉じると、Ctrl+C と同じように順に止まります（[機械から使う](simulation-events.md)）。シグナルも Ctrl+Break も送れない、コンソールのない Windows の `vehicle_sil_launch` も、これで順に止められます（シミュレーションを止めると ecu_main の実行ファイルは自分で終わります。Ctrl+Break を送れないので、自分で終わらない participant は、止め方の 2 の代わりにすぐ強制終了します）。

## participant を残さないこと

- `vehicle_sil_launch` は、終わる前に、起動したすべての participant を終わらせます。
- participant は、標準入力（空）、標準出力、標準エラー（`vehicle_sil_launch` が読むパイプ）のほかに、`vehicle_sil_launch` のファイルやハンドルを受け継ぎません。
- Windows: participant はそれぞれジョブオブジェクト（最後のハンドルが閉じたら、中のプロセスを終わらせる）に入ります。`vehicle_sil_launch` がどう終わっても（強制終了を含む）、participant とそれが起動したプロセスは終わります。
- Linux: participant は、`vehicle_sil_launch` が終わると SIGKILL を受けます（親が終わったときのシグナル）。ただし、`vehicle_sil_launch` が強制終了されたとき（SIGKILL、クラッシュ）は、止め方の 3 と 4 が動かないので、participant が起動したプロセスは残ることがあります。participant のプロセスグループを抜けたプロセス（`setsid()` などを呼んだもの）は、どの場合も終わらせられません。
- Linux 以外の POSIX は対象にしていません（親が終わったときのシグナルがないので、`vehicle_sil_launch` が強制終了されると participant が残ります）。

## 待ち受けるアドレス

SIL Kit の participant は、ほかの participant からの接続を待ち受けます。vehicle-sil が SIL Kit の構成を作る participant（ecu_main の実行ファイル、`ParticipantRuntime` や `Simulation` を使うプログラム、`vehicle_sil_can_bus_simulator`、Vector XL のブリッジ、`run` の controller）は、registry の URI のホストがループバックのアドレスなら、ループバックのアドレスだけで待ち受けます（TCP は `127.0.0.1`、URI が `::1` なら `::1`。Windows 以外では、ほかに一時ディレクトリの local-domain ソケット）。ほかのコンピューターからはつなげず、Windows のファイアウォールは待ち受けてよいかを尋ねません。`registry_uri` を書かない実行は、いつもこれです。registry は、自分の URI のアドレスで、TCP だけで待ち受けます（local-domain ソケットを使いません）。そのポートを別のプログラムや別の `run` が使っていれば、何も起動せずに `setup` の誤り（終了コード 3、`the registry cannot listen at <URI>: ...`）です。

- ループバックのアドレスとみなすのは、`127.0.0.0/8` の IPv4 アドレス（`127.0.0.1`、`127.1` など、`getaddrinfo()` が読む数の書き方）、`::1`（`[::1]`、`[0:0:0:0:0:0:0:1]`）、IPv6 に写した `127.x.x.x`（`[::ffff:127.0.0.1]`）、`localhost`（大文字小文字を問わず、`localhost.` も）です。ほかの名前は引かずに、ループバックでないとみなします。
- `registry_uri` がループバックのアドレスでないとき（`silkit://0.0.0.0:8500` など）は、SIL Kit の既定のとおり、全部のアドレスで待ち受けます。SIL Kit には認証がないので、このコンピューターに届く誰でもシミュレーションに参加できます（注記 `registry-on-network`）。Windows のファイアウォールは、registry と participant のプログラムに、待ち受けてよいかを尋ねます。
- `registry_uri` がループバックのアドレスでないとき、`run` は `registry at` の行の次に、ほかのコンピューターから参加する URI の行を書きます（`[vehicle_sil_launch] from other computers, join at silkit://192.0.2.10:8500 (this computer's address): give it as --registry-uri to a participant started by hand on another computer ...`）。`0.0.0.0` では、このコンピューターの、動いているネットワークの IPv4 アドレス（ループバックとリンクローカル `169.254.x.x` を除く）を並べます。既定の経路のあるネットワークのアドレスを先に並べ、ほかにもあれば、先頭がそれだと書き添えます（WSL、Docker、仮想マシンの仮想のネットワークのアドレスは、ほかのコンピューターから届かないことが多い）。`[::]` では IPv6 のアドレス（ループバックとリンクローカルを除く）を、Linux では IPv4 のアドレスも並べます（`net.ipv6.bindv6only` が `1` でなければ、`[::]` は IPv4 も受けるため）。Windows の `[::]` は IPv6 だけを受けるので、IPv4 には `silkit://0.0.0.0:<port>` と書くよう添えます。見つからなければ、`ipconfig`（Windows）か `ip addr` で調べるよう書きます。アドレスを書いた `registry_uri` では、その URI です。
- 環境変数 `VEHICLE_SIL_LISTEN_ON_NETWORK`: `1` なら、registry の URI がループバックのアドレスでも、全部のアドレスで待ち受けます。`0`、空、設定なしは、上の決まりのとおりです。ほかの値は誤りです（participant を作れず、変数の名前と値を挙げた文言で失敗します）。
  - `registry_uri` が `0.0.0.0`（または `[::]`）のとき、`run` はこのコンピューターの participant に `127.0.0.1`（`[::1]`）の URI を渡します。そのままではループバックのアドレスだけで待ち受け、SIL Kit はそのアドレスを、ほかのコンピューターの participant にもそのまま渡します。受け取った側は、それを自分のループバックのアドレスで試します。そこで別のプログラムが同じポートで待ち受けていると、そこにつながって返事を待ち続け、参加に失敗します。そうならないように、全部のアドレスで待ち受けさせます。
  - `run` は、起動する participant に、いつもこの変数を渡します。`registry_uri` がループバックのアドレスでなければ `1`、そうでなければ `0` です。`vehicle_sil_launch` 自身の環境の値は渡さず、controller も使いません。その値が `run` の渡す値と違えば、注記 `listen-variable-ignored` を出します。
  - 自分で起動する participant（manual）には、`run` が書くコマンドの行と事象 `participant`（`manual`）の `environment` に、設定する値を示します（`1` のときだけ）。ほかのコンピューターの registry に、そのコンピューターの上でループバックのアドレスでつなぐ participant にも、自分で `1` を設定します。
- 自分の SIL Kit の構成を渡した participant（`vehicle_sil_can_bus_simulator --participant-configuration` など）は、その構成のとおりに待ち受けます（`Middleware.AcceptorUris` がなければ全部のアドレス）。vehicle-sil を使わない SIL Kit のプログラム（`describe: false` の participant など）も、それぞれの構成のとおりです。

## 出力

- participant の標準出力と標準エラーは、行ごとに `[<participant 名>] ` を付けて、`vehicle_sil_launch` の標準エラーに流します。`vehicle_sil_launch` 自身の経過は `[vehicle_sil_launch] ` で始まります（registry の URI、ほかのコンピューターから参加する URI、起動したコマンド、始まった、仮想時間が進まない、終わった、止めた）。4096 バイトを超える行は、UTF-8 の文字の間で分けて流します。制御文字（タブを除く C0、DEL、U+0080 から U+009F の C1）と、UTF-8 でないバイトは `?` にします。participant は UTF-8 で書いてください（Windows のコンソールにも UTF-8 として表示するので、ほかのコードページの文字は化けます）。標準エラーを読む側が止まる（読まれないパイプ）と、`vehicle_sil_launch` も書くところで止まります。
- `--log-dir <dir>` を付けると、participant ごとに `<dir>/<participant 名>.log` にも書きます（`[名前]` なし。名前の ASCII の英数字、`.`、`-`、`_` 以外のバイトは `_` にし、Windows の装置の名前（`NUL`、`CON`、`PRN`、`AUX`、`COM1`〜`COM9`、`LPT1`〜`LPT9`。`.` の前も）には、どの OS でも後ろに `_` を付けます。大文字と小文字だけが違う名前は重なりとみなし、`-2` などを付けます）。ディレクトリがなければ作ります。前の実行のログは、同じ名前のものだけを書き直します（今の構成にない participant のログは残ります）。JSON の `participants[].log` が、どのファイルかを示します。
- `--events` のときは、ここに書いた行（participant の行と `vehicle_sil_launch` の経過）を標準エラーに書かず、事象として標準出力に書きます（[機械から使う](simulation-events.md)）。
- 標準出力には結果だけを書きます（人向けか `--json`。`--events` では事象）。同じ機械の上の同じ結果からは同じ出力です（時刻、所要時間、プロセス ID、registry の URI を含みません）。ただし、時間の進み方を示す値は実行ごとに違います（人向けの `time:` の行、JSON の `timing` の `wall_ms`、`actual_speed`、遅れの数（`late_steps`、`max_late_ms`、`slipped_ms`、`late_ms`）、`paused_ms`）。改行はどの OS でも LF です。Windows のコンソールに書くときは、`check` も `run` も、コンソールのコードページによらず UTF-8 の文字として表示します。

人向けの結果は、照合の表と照合で見つけたもの、`run:` で始まるシミュレーションの 1 行（速さを指定したか、速さを合わせた刻みがあったときは、続けて `time:` の 1 行）、participant の表（必須か、どう終わったか）、実行で見つけたもの、最後に結果の 1 行（`passed:`、`failed:`、`interrupted:` と、照合と実行を合わせた誤り、警告、注記の数）です。照合に誤りがあるときは、照合の表と見つけたものの後に `nothing was started:` の行を書きます。

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

passed: 0 errors, 0 warnings, 0 notes
```

`time:` の行は、速さ、全体の実際の速さ（一時停止を除く）、速さを合わせた刻みの数と、そのうち許容を超えて遅れた数、最大の遅れ、ずれの合計、一時停止していた時間（あれば）です。次は `--speed 4` で動かしたときの行です（`<` と `>` で囲んだ所は、実行ごとに違う値です。刻みは、Dashboard のステップ幅に合わせて 5 ms になるので、200 個です）。

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
| `not started (disabled)` | 外した participant（`required` の列は `-`） |
| `started by you (manual)` | 利用者が起動する participant で、シミュレーションに参加した（どう終わったかは、`run` にはわからない） |
| `not joined (manual)` | 利用者が起動する participant で、参加しなかった |

外した participant がいるときの例です。照合の部分は `check` と同じです（[participant を外す](simulation-config.md#participant-を外すenabled--disable--enable)）。

<!-- vehicle_sil_launch run simulation.yaml --disable Dashboard -->
```text
simulation.yaml: 2 participants (1 disabled), 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    -      disabled

network    kind  participants
J1939_CAN  can   Engine

warning: simulation.yaml:8: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it
note: simulation.yaml:14: participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it

run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       exit code 0
Dashboard    -         not started (disabled)

passed: 0 errors, 1 warning, 1 note
```

実行で見つけるものの `code`:

| `code` | 重さ | 内容 |
|---|---|---|
| `setup` | 誤り（終了コード 3） | registry を立てられない（`registry_uri` のポートが使われているなど）、controller を作れない、`--log-dir` に書けない |
| `start` | 誤り（`required: false` は警告） | participant を起動できない |
| `ended-before-start` | 誤り | 必須の participant が、シミュレーションが始まる前に終わった |
| `not-joined` | 誤り | 必須の participant が、プロセスは動いているのに `start_timeout_ms` の間に参加しなかった |
| `simulation-start` | 誤り | 必須の participant がそろったのに、シミュレーションを始められなかった |
| `participant-failed` | 誤り | 実行中に必須の participant が失敗し、シミュレーションを中断した |
| `exit-code` | 誤り（`required: false` は警告） | participant の終了コードが 0 でない、またはシグナルで終わった（上のどれでもないとき） |
| `did-not-end` | 誤り | 必須の participant が、シミュレーションの終わりから `stop_timeout_ms` たっても動いていた |
| `left-processes` | 警告 | participant が起動したプロセスが、その participant が終わった後も動いていた |
| `connection-not-closed` | 警告 | `run` 自身の SIL Kit の接続が、participant を終わらせた後、期限（「止め方」）のうちに閉じなかった（待った時間を書きます）。結果はそろっています。どの構成でも起きないはずのことなので、`vehicle_sil_launch` か SIL Kit の不具合です。結果と `run` の出力を添えて報告してください |
| `timeout` | 誤り | シミュレーションが `--timeout-ms` の実時間で終わらなかったので、中断した（manual の必須の participant がいる実行では、始まらなかったときも。待っていたものを書きます） |
| `simulation` | 誤り | シミュレーションが正常に終わらなかった（理由の participant がわからない） |
| `ended-before-stop-at` | 注記 | `stop_at_ms` より前に、participant がシミュレーションを止めた |
| `control-link-no-hello` | 警告 | 変数を出すと申告した ECU の hello が、始まる前に来なかった。または hello は来たが、コマンドを受け取れるという返事が来なかった（文がどちらかを言います。[変数を読み書きする口](#変数を読み書きする口)） |
| `control-link-dropped` | 警告 | `vehicle_sil_launch` が、変数の口のメッセージを捨てた（受け付けなかった hello、読めなかったメッセージ、知っている ECU やその変数に合わなかったメッセージの数を書きます）。考えられる原因: 変数を出す participant が上限（256）を超えた、版の違う vehicle-sil の ECU がいる、別のプログラムが同じトピック（`vehicle_sil.control.*`）に書いている、実行中に参加した participant の hello がまだ届いていなかった。該当する participant の変数は、欠けているか古いことがあります |
| `control-link-version` | 警告 | 届いた hello の、メッセージの形式の版が `vehicle_sil_launch` と違う（申告の版が違うときは、照合の同じ `code` の所見で、実行では繰り返しません）。同じ版の vehicle-sil でビルドする |
| `control-link-late-values` | 警告 | ECU の値か結果が、仮想時刻の約束より遅れて届いた（ステップの外から送っている。自作の通信の側の誤り） |
| `variable-map-not-applied` | 誤り | 構成ファイルの `variables` の対応のうち、ECU の変数の口が知らせた変数に合わないものがあった（どれが、なぜ合わないかを書きます）。その対応は当てず、変数は実行ファイルの名前と値のまま見えていて、その対応の名前への書き込みは断った。同じ内容を、つながった時点で実行の行（`log`）にも 1 回出します。照合が実行の前に見つけるので、出るのは `describe: false` の participant か、自己申告と違う変数を知らせた実行ファイルのときだけ（[変数の名前と値を合わせる](simulation-config.md#変数の名前と値を合わせるvariables)） |
| `before-start` | 誤り | 始まる前の窓でする処理（`run` を関数として呼ぶ側が渡すもの。内部用）が失敗した。シミュレーションは始まっていて、すぐに中断した |
| `behind-real-time` | 警告 | 速さを合わせた刻みのうち、遅れが `late_tolerance_ms` を超えたものがあった（[等速で動かす](#等速で動かす)）。遅れた刻みの数、最大の遅れ、ずれの合計、全体の実際の速さ、止まったまま終わったならそのときの遅れを書きます。どの participant の所見でもありません |
| `coarse-time-step` | 警告 | 速さの指定があり、controller の刻みが ECU のステップより粗い（ECU のステップが刻みの境目で始まらない）。ECU のステップが最大で何 ms 遅れてまとまって動くかと、`time_step_ms` に書く値を書きます（[等速で動かす](#等速で動かす)の「刻みと ECU のステップ」）。どの participant の所見でもありません |
| `interrupted` | 注記 | 中断された |

## JSON（`run --json`）

1 つの JSON のオブジェクトです。決まり（キーの順、`null`、字下げ、改行、版の上げ方と読む側の扱い）は照合の JSON と同じです。

| キー | 値 |
|---|---|
| `vehicle_sil_run` | 形式の版（今は 1） |
| `vehicle_sil` | `vehicle_sil_launch` の vehicle-sil の版 |
| `file` | 構成ファイル（引数に書いたとおり） |
| `result` | `"passed"`、`"failed"`、`"interrupted"`、`"check_failed"`（照合の誤りで何も起動しなかった）、`"unreadable"`、`"setup_failed"`、`"usage"` |
| `strict` | `--strict` を付けたか |
| `errors`、`warnings`、`notes` | 照合と実行を合わせた数 |
| `simulation` | `"reached_stop_at"`（`stop_at_ms` まで動いた）、`"ended"`（participant が止めた。`--scenario` では、シナリオが終わってランチャーが止めたときも）、`"aborted"`（中断した。`--timeout-ms` のときも）、`"interrupted"`、`"not_started"`。何も起動しなかったときは `null` |
| `interrupted_by` | 中断のシグナルかキー（`"SIGINT"`、`"Ctrl+C"` など）。中断でなければ `null` |
| `timing` | 時間の進み方（下の表）。何も起動しなかったときは `null` |
| `participants[]` | 構成ファイルの順（外した participant も）。`name`、`required`、`enabled`（`false` なら外した participant で、`ended` は `"not_started"`、`log` は `null`。シミュレーションが待つのは `required` と `enabled` がどちらも `true` のもの）、`manual`（`true` なら利用者が起動する participant で、`ended` は `"not_started"`、`exit_code` は `null`。[participant を自分で起動する](simulation-config.md#participant-を自分で起動するmanual--manual)）、`ended`（`"exited"`、`"stopped"`、`"killed"`、`"not_started"`）、`exit_code`（シグナルで終わったとき、強制終了したとき、起動しなかったときは `null`）、`signal`（`"SIGSEGV"` など。なければ `null`）、`left_processes`（起動したプロセスを残したか）、`log`（`--log-dir` のファイル。なければ `null`）、`control_link`（始まったときの、変数を読み書きする口のつながり方。下の表。起動しなかった participant は `state` が `"none"`） |
| `participants[].control_link` | `state`（`"connected"`、`"no_hello"`、`"version_mismatch"`、`"not_offered"`、`"none"`。[変数を読み書きする口](#変数を読み書きする口)）、`variables`（`connected` のとき、その ECU が出す変数の数。ECU の実行が足す `peer.` などの出力を含むので、`--describe` の数より多いことがあります。ほかは `null`）、`version`（`version_mismatch` のとき、相手の形式の版。ほかは `null`）、`late_messages`（約束より遅れて届いたメッセージの数。ふつうは 0） |
| `findings[]` | 実行で見つけたもの（照合の `findings[]` と同じ形） |
| `check` | 照合の結果（`check --json` と同じオブジェクト）。選んだ機種（`variant`）と、パラメーター（`params`、`scenario.params`）はここにあります |
| `scenario` | `--scenario` を付けたときだけ。シナリオの結果（[scenario.md](scenario.md) の「出力」）。`check` の中にも `scenario`（`file`、`name`、`steps`）があります。`simulation` の `"ended"` は、シナリオが終わってランチャーが止めた場合を含みます。見分けるには `scenario.status` を見ます（`"passed"` か `"failed"` なら、シナリオが終わって止めた実行です）。飛ばした手順があれば、`scenario.skipped_steps` にその数があります（`status` は `"passed"` のまま）。飛ばした手順の `at_ms` は null です |

`timing` のキー（数の定義は[等速で動かす](#等速で動かす)。時間は、controller の最初の刻み（仮想時間 0）が始まってから、最後の刻みが始まるまでのものです。participant の起動、止める処理、失敗に気づくまでの待ちは入りません。次の刻みが期限を過ぎたまま実行が終わったときと、最速のときは、実行が終わった時点までです）:

| キー | 値 |
|---|---|
| `speed` | 終わったときの速さ（数か `"max"`） |
| `time_step_ms` | controller の刻み（構成ファイルの値か既定値） |
| `ecu_step_ms` | ECU のステップ幅の最小（ミリ秒）。ステップ幅がわからない participant（`describe: false`）があるとき、ステップを持つ ECU がないときは `null` |
| `step_delay_ms` | 刻みが ECU のステップより粗いために、ECU のステップが、その仮想時刻にあたる実時間より遅く動く量の最大（ミリ秒。速さを合わせているときに意味があります。0 なら、どの ECU のステップも刻みの境目で始まります）。ステップ幅がわからない participant があるときは `null` |
| `late_tolerance_ms` | 遅れの許容（構成ファイルの値か既定値） |
| `sim_ms` | 進んだ仮想時間（ミリ秒。最後に始まった刻みの時刻）。participant の失敗や中断で終わった実行では、それに気づく時点が controller の次の刻みが始まる前か後かで、1 刻みの幅で実行ごとに変わります |
| `wall_ms` | かかった実時間（ミリ秒。一時停止していた時間を含みます） |
| `actual_speed` | 全体の実際の速さ（`sim_ms` ÷（`wall_ms` − `paused_ms`）、小数 3 桁）。動いた時間が 1 ms に満たなければ `null` |
| `paced_steps` | 速さを合わせた刻みの数（期限と比べた刻み。期限を過ぎたまま始まらなかった最後の刻みを含みます） |
| `late_steps` | そのうち、遅れが `late_tolerance_ms` を超えた刻みの数（ずれた回数） |
| `max_late_ms` | 速さを合わせた刻みの、最大の遅れ（ミリ秒、切り上げ。許容の内のものも含みます） |
| `slipped_ms` | ずれの合計（遅れた刻みの遅れの合計。ミリ秒、切り上げ）。シミュレーションが、指定の速さで進んだ場合より、全体でこれだけ遅れました |
| `late_ms` | 実行が終わったとき、期限を過ぎたまま始まっていなかった刻みの遅れ（ミリ秒、切り上げ）。なければ 0。0 でなければ、シミュレーションは止まったまま終わりました（`late_steps`、`max_late_ms`、`slipped_ms` に入っています） |
| `paused_ms` | 一時停止していた実時間（ミリ秒） |

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
      "ended": "exited",
      "exit_code": 0,
      "signal": null,
      "left_processes": false,
      "log": null,
      "control_link": {
        "state": "connected",
        "variables": 18,
        "version": null,
        "late_messages": 0
      }
    },
    {
      "name": "Dashboard",
      "required": true,
      "enabled": true,
      "manual": false,
      "ended": "exited",
      "exit_code": 0,
      "signal": null,
      "left_processes": false,
      "log": null,
      "control_link": {
        "state": "connected",
        "variables": 18,
        "version": null,
        "late_messages": 0
      }
    }
  ],
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
              ]
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
            "description": "on its buses; false: off all of them (CAN and LIN), its steps go on",
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
        "control": {
          "transport": "silkit",
          "wire_version": 1
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
              ]
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
            "description": "on its buses; false: off all of them (CAN and LIN), its steps go on",
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
        "control": {
          "transport": "silkit",
          "wire_version": 1
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
        ]
      }
    ],
    "findings": []
  }
}
```

この節の例も、CTest の `launch_run.docs` が実際の実行と照合します（`vehicle_sil` の版と、`<wall_ms>`、`<actual_speed>` にした値を除いて、バイト単位で）。
