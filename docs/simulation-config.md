# シミュレーションの構成ファイル、照合と実行（`vehicle_sil_launch check`、`run`）

まとめる人向けです。先に [ecu-executable.md](ecu-executable.md)（ECU の実行ファイルの引数、`--describe` の JSON、止め方）を読みます。この後に読む文書はありません。

各自が作った ECU の実行ファイル（[ecu-executable.md](ecu-executable.md)）を 1 つのシミュレーションにまとめるときに、まとめる人が書く構成ファイルと、その照合です。構成ファイルには、participant にする実行ファイル、それらがつながるネットワーク、止める時刻を書きます。`vehicle_sil_launch check` は、構成ファイルと、各実行ファイルの自己申告（`--describe`）を突き合わせ、食い違いを 1 回で全部出します。`vehicle_sil_launch run` は、同じ照合をしてから、シミュレーション全体を起動し、終わりまで見届けて、各 participant がどう終わったかを出します。形式と規則はまだ変わることがあります。

`check` はシミュレーションを始めず、registry にも接続しません。ただし、`describe: true` の実行ファイルは、1 つずつ `--describe` で起動します。`--describe` は ECU を組み立てるので、その実行ファイルのコード（ECU のコンストラクターなど）が動きます。構成ファイルは、実行するコマンドの一覧です。信頼できない構成ファイルを `check` にも `run` にも渡さないでください。`describe: false` の実行ファイルは起動せず、あることだけを確かめます。

`run` は、照合に通ったときだけ、すべての participant を起動します（[シミュレーションの実行](#シミュレーションの実行vehicle_sil_launch-run)）。

```sh
vehicle_sil_launch check simulation.yaml
vehicle_sil_launch run simulation.yaml
```

`vehicle_sil_launch` は SIL Kit をサブモジュールからビルドしたときだけ作ります（YAML の読み込みに SIL Kit 付属の rapidyaml を使うため）。

## 構成ファイル

YAML で書きます。最初の空白でない文字が `{` なら JSON として読みます（内容は同じです）。文字コードは UTF-8 です（先頭の BOM は読み飛ばします。UTF-16 や NUL を含むファイルは読めません）。1 つのファイルに構成は 1 つです（`---` で続けた 2 つ目の文書は誤りです）。YAML のアンカーとエイリアス（`&name`、`*name`、`<<`）は使えません（ファイルを読めない誤りにします）。値は書き下してください。16 MiB を超えるファイルと、通常のファイルでないもの（デバイスやパイプ）は読みません。

例: [examples/ecu_main/simulation.yaml](../examples/ecu_main/simulation.yaml)（CMake が実行ファイルの横、`build/<preset>/examples/ecu_main/` に写します）。

<!-- examples/ecu_main/simulation.yaml -->
```yaml
# The two ECUs of this directory in one simulation. CMake copies this file next to the
# executables (build/<preset>/examples/ecu_main/); check and run it there with
#   vehicle_sil_launch check simulation.yaml
#   vehicle_sil_launch run simulation.yaml
vehicle_sil_simulation: 1
stop_at_ms: 1000
networks:
  - name: J1939_CAN
    kind: can
participants:
  - name: Engine
    command: [engine/vehicle_sil_engine_ecu]
    step_ms: 10
  - name: Dashboard
    command: [dashboard/vehicle_sil_dashboard_ecu]
    step_ms: 5
```

| キー | 必須 | 値 |
|---|---|---|
| `vehicle_sil_simulation` | はい | 形式の版（今は `1`） |
| `stop_at_ms` | | シミュレーションを止める仮想時間（ミリ秒、1 以上 10^12 以下の整数）。書かなければ、participant が止めるか、`run` を Ctrl+C で止めるまで動きます |
| `start_timeout_ms` | | 必須の participant がそろうまで待つ時間（ミリ秒、1 以上 10^12 以下の整数。書かなければ 30000）。`run` は ecu_main の実行ファイルにも `--start-timeout-ms` で同じ値を渡します |
| `stop_timeout_ms` | | `run` が participant の終わりを待つ時間（ミリ秒、1 以上 10^12 以下の整数。書かなければ 10000）。止め方の各段で、この時間だけ待ちます |
| `registry_uri` | | `run` が registry を立てる場所（`silkit://<host>:<port>`、ポートは 1 以上 65535 以下）。書かなければ 127.0.0.1 の空いているポートです（自分で起動する必須の participant がいる実行では `silkit://127.0.0.1:8500`。[participant を自分で起動する](#participant-を自分で起動するmanual--manual)）。ほかのコンピューターの participant も参加させるときは `silkit://0.0.0.0:8500` のように書きます（このコンピューターの participant には `127.0.0.1` の URI を渡します） |
| `speed` | | 仮想時間を進める速さ（実時間 1 秒あたりの仮想時間の秒数）。`1` が等速、`2` が 2 倍速、`0.5` が半分の速さです（0.000001 以上 1000000 以下の数）。`max` か、書かなければ最速です。`run --speed` で上書きできます（[等速で動かす](#等速で動かす)） |
| `time_step_ms` | | controller の刻み（ミリ秒の仮想時間、1 以上 10^12 以下の整数）。一時停止とステップ送りの細かさ、速さを合わせる細かさです。書かなければ、最速のときは 10 です（`stop_at_ms` が 10 の倍数でなければ、`stop_at_ms` と 10 の最大公約数）。速さを指定したときは、さらに ECU のステップ幅との最大公約数にします（[等速で動かす](#等速で動かす)の「刻みと ECU のステップ」）。書いたときは `stop_at_ms` の約数にします（誤り `value`） |
| `late_tolerance_ms` | | 遅れの許容（実時間のミリ秒、1 以上 10^12 以下の整数）。刻みの遅れがこれを超えたら、遅れた刻みに数えます。書かなければ 10 です（刻みと速さによりません） |
| `networks` | はい | ネットワークの配列（空でもかまいません）。ECU が使うネットワークは、すべてここに書きます |
| `networks[].name` | はい | ネットワークの名前（ECU の `j1939()`、`can()`、`lin_master()`、`lin_slave()` に渡す名前） |
| `networks[].kind` | はい | `can` か `lin` |
| `participants` | はい | participant の配列（1 つ以上） |
| `participants[].name` | はい | participant 名。照合では、ecu_main の実行ファイルに `--name` で渡します。空でなく、1 行で（制御文字なし）、`-` で始まらないこと。`SimulationController`（vehicle-sil の controller の participant 名）は使えません |
| `participants[].command` | はい | 実行ファイルと、その引数の配列。引数がなければ文字列 1 つでもかまいません（空白で分けません） |
| `participants[].step_ms` | | 実行ファイルが申告するステップ幅（ミリ秒、1 以上 10^12 以下の整数）。書いたときだけ照合します |
| `participants[].describe` | | `true`（既定）: ecu_main の実行ファイル（照合します）。`false`: それ以外の実行ファイル（照合しません） |
| `participants[].required` | | `true`（既定）か `false`。シミュレーションがその participant を待つか。`false` は `describe: false` の participant にだけ書けます。外していない participant のうち、少なくとも 1 つは `true` の participant が要ります（誤り `no-required-participant`） |
| `participants[].enabled` | | `true`（既定）か `false`。`false` の participant は外します（照合も起動もしません。[participant を外す](#participant-を外すenabled--disable--enable)） |
| `participants[].manual` | | `false`（既定）か `true`。`true` の participant は、`run` が起動せず、利用者が自分で起動します（デバッガーからなど。[participant を自分で起動する](#participant-を自分で起動するmanual--manual)） |

- 知らないキーはエラーです（打ち間違いを黙って無視しません）。近い名前があれば示します。同じキーを 2 回書くのもエラーです。
- 整数は引用符なしで書きます（`"5000"` は文字列なのでエラーです）。真偽値は `true` か `false` です。
- `stop_at_ms`、`start_timeout_ms`、`stop_timeout_ms`、`registry_uri`、`speed`、`time_step_ms`、`late_tolerance_ms` はシミュレーションを動かすときの値です。`check` は値の形（と `stop_at_ms` と `time_step_ms` の組み合わせ）だけを確かめます。

### 実行ファイルのパス

- `command` の最初の要素が実行ファイルです。相対パスは構成ファイルのあるディレクトリから数えます（コマンドを実行したディレクトリからではありません）。パスは正規化しません（シンボリックリンクをまたぐ `..` は OS の解釈のまま）。
- 構成ファイルのディレクトリになく、区切り（`/`、`\`）を含まない名前（`python3` など）は、`PATH` から探します（`PATH` の相対のディレクトリは使いません）。構成ファイルのディレクトリにあっても実行できないファイル（実行の許可がないもの）は使わず、`PATH` を探します。見つけたら、注記（`found-on-path`）でその場所を示します（その機械のパスなので、この注記は機械ごとに違います）。
- 区切りは `/` で書けます（Windows でも）。`\` を使うときは YAML の単一引用符（`'C:\ecu\engine.exe'`）で書きます。空白や ASCII 以外の文字を含むパスも使えます。
- Windows では、パスにファイルがなく、`.exe` で終わらないとき、`.exe` を付けたファイルがあれば、それを使います（同じ構成ファイルを Linux と Windows で使えます）。
- バッチファイル（`.bat`、`.cmd`）は誤りです（cmd.exe を通り、引数の引用の決まりが違うため）。それが起動する実行ファイルを書きます。

### 波括弧とプレースホルダー

`command` の中で特別なのは、英数字と `_` を `{` と `}` で囲んだ `{語}` の形だけです。ほかの波括弧は、そのまま渡します（JSON の引数 `'{"a":{"b":1}}'` は 1 バイトも変わりません）。

- `describe: false` の `command` では、実行ファイルより後の引数に、`{registry_uri}`（シミュレーションの registry の URI）と `{name}`（participant 名）を書けます（長い引数の一部でもかまいません。例: `--uri={registry_uri}`）。
- それ以外の `{語}` は誤りです（`describe: true` の `command` の `{registry_uri}` と `{name}`、どちらの `command` でも知らない `{語}`、実行ファイルのパスのすべての `{語}`）。打ち間違いを見つけるためです。
- `{語}` という文字列そのものを渡すときは、`{{語}}` と書きます（`{{name}}` は `{name}` になります）。`describe: true` の引数と実行ファイルのパスでも同じです。
- `{registry_uri}` のない `describe: false` の `command` には注記を出します（registry の場所を受け取れないかもしれないため）。
- YAML では `{...}` を引用符で囲みます（`"{registry_uri}"`）。囲まないと YAML のマッピングになり、誤りになります（直し方を示します）。`*` か `&` で始まる値も引用符で囲みます（囲まないとエイリアスとアンカーです）。

### ecu_main の実行ファイル（`describe: true`）

- `command` には、`ecu_main` の引数（`--describe`、`--name`、`--registry-uri`、`--local`、`--run-for-ms`、`--start-timeout-ms`、`--help`、`-h`。`--name=X` の形も）を書けません（誤り）。
- `--no-control` は `command` に書けます。その ECU は変数を出しません。照合の `--describe` にも同じ引数が渡るので、自己申告の `control` が `null` になり、`run` はその ECU の hello を待ちません（[変数を読み書きする口](#変数を読み書きする口)）。
- 照合では、`command` の引数（`{{語}}` を `{語}` に戻したもの）に `--describe --name <participant 名>` を足して動かします。`run` では `--name <participant 名> --registry-uri <URI> --start-timeout-ms <ms>` を足します。`<ms>` は `start_timeout_ms + stop_timeout_ms + 10000` です（開始を待つのはランチャーで、来なかった participant を挙げるのもランチャーです。ECU の制限は、ランチャーがいなくなったときのためのものです）。自分で起動する必須の participant がいる実行では、開始に上限がないので、10^12（`1000000000000`）です。
- `required: false` は書けません（誤り `not-required-ecu`）。ecu_main の実行ファイルは coordinated の participant として参加し、SIL Kit は必須でない coordinated の participant を始めないためです。ECU を外して動かすときは、`enabled: false` を書くか、`--disable <participant 名>` を付けます（[participant を外す](#participant-を外すenabled--disable--enable)）。

### ほかの実行ファイル（`describe: false`）

- 照合しません。名前、ステップ幅、ネットワークを確かめられないことを注記します。`step_ms` を書いても照合しないので、注記します。
- `run` は、書いた引数（プレースホルダーを置き換えたもの）のまま起動します。`required: true`（既定）の participant は、`{registry_uri}` の registry に、participant 名で、coordinated のライフサイクルで参加する必要があります（シミュレーションはそれを待ちます）。シミュレーションに参加しない道具（ログを取るだけのものなど）は `required: false` にします。

### participant を外す（`enabled`、`--disable`、`--enable`）

ある ECU を除いて動かすときは、その participant に `enabled: false` を書きます。ファイルを書き換えずに組み合わせを変えるときは、`check` と `run` に `--disable <participant 名>`（外す）か `--enable <participant 名>`（`enabled: false` のものを戻す）を付けます。どちらも繰り返せます。コマンドラインの指定が、ファイルの `enabled` より優先します。

- 外した participant は、照合も起動もしません。`--describe` も動かさず、実行ファイルも探しません（まだない実行ファイルでもかまいません）。シミュレーションはそれを待ちません。
- 外した participant の項目も、書き方は同じように確かめます（キー、値の型、名前の重複、`command` の引数など）。戻したときに初めて誤りが出ることはありません。誤りがあっても、外した participant は残りの照合を止めません。`enabled` の値が真偽値でない participant は、外すかどうかがわからないので、照合も起動もしません（誤りなので照合は失敗します）。
- 同じ名前の項目は、外していても誤りです（`--disable <名前>` がどれを指すかわからなくなるため）。代わりの ECU と入れ替えるときは、別の名前にするか、構成ファイルを分けます。
- 照合は、残りの participant の組み合わせで行います。外した結果として出るもの（participant が 1 つだけになったネットワークなど）は、そのまま出します。
- 外した participant ごとに注記 `participant-disabled` を出します（`--strict` でも失敗にしません）。「一部を外して通った」を「全部で通った」と読み違えないためです。ただし、外したことで出る警告（下の例の `single-participant` など）は警告のままなので、`--strict` を付けると失敗します（`run --strict` は何も起動しません）。
- 外していない participant のうち、少なくとも 1 つは必須（`required: true`）でなければなりません（誤り `no-required-participant`）。
- `--disable` と `--enable` には、構成ファイルにある participant 名を書きます。同じ名前を両方に書くのは、いつでも引数の誤りです（終了コード 4）。構成ファイルにない名前は、構成ファイルを読めて誤りがないときだけ引数の誤りにします（終了コード 4。使える名前を並べ、合わない名前を全部挙げます）。構成ファイルを読めないとき（終了コード 3）と誤りがあるとき（終了コード 1）は、ファイルの誤りを先に出し、合わない名前は言いません（名前を読めなかった項目のせいかもしれないため）。その間も、構成ファイルにある名前の指定は当てます。

<!-- vehicle_sil_launch check simulation.yaml --disable Dashboard -->
```text
simulation.yaml: 2 participants (1 disabled), 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    -      disabled

network    kind  participants
J1939_CAN  can   Engine

warning: simulation.yaml:8: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it
note: simulation.yaml:14: participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it

passed: 0 errors, 1 warning, 1 note
```

### participant を自分で起動する（`manual`、`--manual`）

ECU をデバッガーから動かすときなど、ある participant を `run` ではなく自分で起動するときは、その participant に `manual: true` を書きます。ファイルを書き換えずに、その実行だけ自分で起動するときは、`check` と `run` に `--manual <participant 名>` を付けます（繰り返せます）。`run` はその participant を起動せず、起動するコマンドを書いて、参加してくるのを待ちます。

- 照合は、ほかの participant と同じです（`describe: true` なら `--describe` を動かします）。manual の participant ごとに注記 `participant-manual` を出します。
- `run` は、その participant を起動する順番のところで、作業ディレクトリと、起動するコマンドの 1 行を書きます（下の例）。引数は、`run` が自分で起動するときと同じです（ecu_main の実行ファイルには `--name`、`--registry-uri`、`--start-timeout-ms`）。そのコマンドを、書いてあるディレクトリで実行します（デバッガーでは、実行ファイル、引数、作業ディレクトリに分けて設定します）。引数の引用は、Linux では POSIX sh（bash、zsh）に貼る形です。Windows では、コマンドプロンプト（cmd.exe）に貼る形で、C ランタイムがコマンドラインを読む決まりで引用し、`&` `|` `<` `>` `^` を含む引数も `"` で囲みます。実行ファイルのパスは `\` で区切ります。cmd.exe は `"` の中でも `%...%` を展開するので、`%` を含む引数は直して貼ります。PowerShell に貼るときは、`"` で囲んだ実行ファイルの前に `&` を足し、`$` と `` ` `` を含む引数を直します。`--events` では、事象 `participant` の `state` が `manual` の行が、同じものを持ちます。
- `registry_uri` を書いていなければ、manual の必須の participant がいる実行は、registry を `silkit://127.0.0.1:8500` で立てます。コマンドが実行ごとに変わらないので、デバッガーの設定を 1 度作れば使い回せます。そのポートを別のプログラムが使っていれば `setup` の誤りです（`registry_uri` に別のポートを書きます）。
- 待ち方: manual の必須の participant がいる実行では、開始の時間の上限を 3 つの区間に分けます。(1) 参加するまで: `run` が起動した participant は、今までどおり `start_timeout_ms` の間に参加しなければ `not-joined` です。manual の participant は、参加するまで待ちます。manual でない必須の participant がそろうと、`waiting for <名前> to join (manual): ...` の行を 1 回書きます。(2) 全員が参加した後、manual の participant がすべて最初のステップの準備を終える（ReadyToRun）まで: 上限はありません（デバッガーで止めていてもかまいません）。(3) その後、全員が最初のステップに着くまで: `start_timeout_ms` です（`run` が起動した participant が初期化で固まれば、今までどおり `simulation-start` の誤りです）。どの区間でも、Ctrl+C、`stop` の命令、Control Server の `Stop` で止まります（終了コード 130）。
- `--timeout-ms` は、manual の必須の participant がいる実行では、`run` が participant を待ち始めた時点から数えます（参加を待つ時間、開始を待つ時間、実行の時間の合計です）。始まる前に過ぎると `timeout` の誤りで、何を待っていたか（参加していない participant か、最初のステップに着いていない participant）を文言に挙げます。`manual: true` を書いた構成ファイルを CI で動かすときは、`--timeout-ms` を付けると、誰も起動しなくても終わります。
- 参加した manual の participant が、始まる前に終わる（デバッガーで止めて起動し直すなど）と、開始は失敗します（`simulation-start`。その participant はこの実行には参加し直せないので、ランチャーも起動し直します）。
- `required: false` の manual の participant は、待ちません（コマンドを書くだけです）。
- 実行中: 見張りは、ほかの必須の participant と同じです。ただし、`run` はそのプロセスを見ていないので、失敗は、シミュレーションから抜けたこと（接続が切れた、Error の状態）でわかります（`participant-failed`）。その participant の出力は、起動した所（端末、デバッガー）に出ます。デバッガーで止めている間は、仮想時間が進みません。速さを指定していれば、その間を遅れとして数え（`behind-real-time`）、`--timeout-ms` も数えます。
- 止め方: `run` は manual の participant のプロセスに、止める要求も強制終了も送りません。シミュレーションが終われば、ecu_main の実行ファイルは自分で終わります。シミュレーションが始まらなかったとき（中断、ほかの participant の失敗）は、参加していた manual の participant に SIL Kit の abort を送ります（ecu_main の実行ファイルは、開始の失敗として終わります）。どちらの場合も、参加した manual の participant がシミュレーションを去るのを `stop_timeout_ms` まで待ってから、registry を閉じます。去らなければ（デバッガーで止めているなど）、そう書いて終わります（誤りにはしません）。この待ちは、Ctrl+C か `stop` の命令をもう 1 回送ると切り上げます。`run` が強制終了されたとき（SIGKILL、クラッシュ、Windows でコンソールを閉じた）は、自分で起動した participant は残ることがあります（始まる前なら、開始を待ち続けます）。自分で終わらせます。
- Windows の制限: manual の participant を、シミュレーションが動いている途中でデバッガーで止めたまま、ランチャーを止める（Ctrl+C、`stop` の命令）と、ランチャーが結果を書かずに終わらないことがあります。先にデバッガーで続行させてから、ランチャーを止めます。終わらなくなったときは、その participant を終わらせ、ランチャーのプロセスも終わらせます。
- 結果: participant の表の `ended` は、`started by you (manual)`（参加した）か `not joined (manual)` です。JSON の `participants[].manual` は `true`、`ended` は `"not_started"`、`exit_code` は `null` です。
- `--disable` と `--manual` に同じ名前を書くのは、引数の誤りです（終了コード 4）。構成ファイルにない名前は、`--disable` と同じ扱いです。`--manual` は `enabled` を変えません（外した participant は、外したままです）。構成ファイルの `manual: true` を、コマンドラインで戻す引数はありません。

例: [examples/ecu_main/simulation.yaml](../examples/ecu_main/simulation.yaml) の Engine を自分で起動します。`run` は、標準エラーに次の 2 行を書きます（`<dir>` は例のディレクトリです。Windows では、パスが `\` で区切られ、実行ファイルの名前に `.exe` が付きます）。2 行目の `[vehicle_sil_launch]` の後が、実行するコマンドです。

<!-- vehicle_sil_launch run simulation.yaml --manual Engine: the lines about Engine -->
```text
[vehicle_sil_launch] not starting Engine: start it yourself (manual: true, or --manual); the simulation waits for it to join. In the directory <dir>, run:
[vehicle_sil_launch]   <dir>/engine/vehicle_sil_engine_ecu --name Engine --registry-uri silkit://127.0.0.1:8500 --start-timeout-ms 1000000000000
```

そのコマンドで Engine を起動すると、シミュレーションが始まります。結果は次のとおりです。

<!-- vehicle_sil_launch run simulation.yaml --manual Engine -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

note: simulation.yaml:11: participant 'Engine': you start it yourself (manual: true, or --manual): run does not start it, but prints the command to start it with, and waits for it to join without a time limit (with --timeout-ms, at most that long)

run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       started by you (manual)
Dashboard    yes       exit code 0

passed: 0 errors, 0 warnings, 1 note
```

## 照合

`vehicle_sil_launch check <file>` は、次の順に確かめ、見つけたものを全部まとめて出します。見つけたものには 3 つの重さがあります。誤り（error）は照合を失敗にします。警告（warning）は `--strict` のときだけ失敗にします。注記（note）は失敗にしません（照合が確かめられなかったことを伝えます）。

1. 構成ファイルの文面: 形式の版、知らないキーと足りないキー、値の型と範囲、名前、`command` の引数と波括弧。誤りのある participant（名前がない・使えない、`command` が使えない、名前が重複している、`describe` が真偽値でない、`command` の引数の誤り）は、次の段に進めません。知らないキーや `step_ms` の誤りは、照合を止めません。
2. 実行ファイル: あること、ディレクトリでないこと、バッチファイルでないこと、（Linux では）実行の許可があること。外した participant（`enabled: false`、`--disable`）は、ここから先を飛ばします。
3. `describe: true` の各実行ファイルを `--describe --name <participant 名>` で 1 つずつ順に動かし（並列にしません）、標準出力の JSON を読みます。作業ディレクトリは構成ファイルのあるディレクトリで、標準入力は空です。終了コードが 0 でない（シグナルや例外で終わったときは、そう書きます）、時間内に終わらない（`--describe-timeout-ms`、既定 10000 ms。過ぎたら止めます。出力を書き続ける実行ファイルも止めます）、JSON でない、形式の版が違う、は、その participant の誤りです。標準エラーの最後の 10 行を添えます。読むのは標準出力の最初の 1 MiB と標準エラーの最後の 1 MiB です。標準出力を切ったときは自己申告の誤りの添え書きで、標準エラーを切ったときはその末尾を添えるところで、そう書きます。
4. 構成と自己申告の突き合わせ。

各規則には、出力に出す固定の名前（`code`）があります。

| `code` | 重さ | 内容 |
|---|---|---|
| `syntax` | 誤り（読めない） | YAML または JSON の文法の誤り |
| `not-a-configuration` | 誤り（読めない） | 空のファイル、マッピングでないファイル |
| `encoding` | 誤り（読めない） | UTF-8 でないファイル（UTF-16、NUL を含む） |
| `unsupported-yaml` | 誤り（アンカーとエイリアスは読めない） | アンカーとエイリアス、2 つ目の文書（`---`。最初の文書は読みます） |
| `format-version` | 誤り（知らない版は読めない） | `vehicle_sil_simulation` が 1 でない |
| `file` | 誤り（読めない） | ファイルがない、ディレクトリ、通常のファイルでない、大きすぎる、開けない |
| `missing-key` | 誤り | 必須のキーがない |
| `unknown-key` | 誤り | 知らないキー |
| `duplicate-key` | 誤り | 同じキーが 2 回 |
| `value` | 誤り | 型または範囲が違う値、使えない名前、実行ファイルのパスのプレースホルダー、`stop_at_ms` の約数でない `time_step_ms` |
| `duplicate-name` | 誤り | participant 名、またはネットワークの名前が 2 回 |
| `reserved-name` | 誤り | participant 名が `SimulationController` |
| `launcher-argument` | 誤り | ecu_main の `command` に、`ecu_main` の引数かプレースホルダーがある |
| `unknown-placeholder` | 誤り | `describe: false` の `command` に知らない `{語}` がある |
| `no-registry-uri` | 注記 | `describe: false` の `command` に `{registry_uri}` がない |
| `step-not-checked` | 注記 | `describe: false` の participant に `step_ms` がある |
| `executable` | 誤り | 実行ファイルがない（区切りのない名前は `PATH` にもない）、ディレクトリ、バッチファイル、実行の許可がない |
| `found-on-path` | 注記 | 実行ファイルを `PATH` で見つけた（その場所を示します） |
| `not-required-ecu` | 誤り | `describe: true` の participant に `required: false` がある |
| `no-required-participant` | 誤り | 外していない participant がどれも `required: false`、またはすべてを外した（シミュレーションが待つ participant がない） |
| `describe-start` | 誤り | 実行ファイルを起動できない |
| `describe-timeout` | 誤り | `--describe` が時間内に終わらない |
| `describe-failed` | 誤り | `--describe` の終了コードが 0 でない（4 は、ecu_main でない実行ファイルの見込みが高い） |
| `describe-output` | 誤り | `--describe` の出力が自己申告の JSON でない |
| `describe-version` | 誤り | 自己申告の形式の版（`vehicle_sil_ecu`）を読めない |
| `name-mismatch` | 誤り | 自己申告の `name` が participant 名と違う（`--name` を `ecu_main` に渡していない） |
| `step-mismatch` | 誤り | `step_ms` が自己申告のステップ幅と違う（実時間で、ステップ幅のない ECU も） |
| `unknown-network` | 誤り | ECU が `networks` にないネットワークを使う |
| `kind-mismatch` | 誤り | ECU がネットワークを `networks` と違う種類（CAN か LIN）で使う |
| `address-conflict` | 誤り | 同じネットワークの 2 つ以上の J1939 ノードが同じアドレスを求め、どれかが動けない（下の表） |
| `address-shared` | 警告 | 同じアドレスを求めるノードのうち、NAME の最も小さいノードが取り、ほかはどれも別のアドレスへ移る |
| `name-conflict` | 誤り | 同じネットワークで、2 つ以上の J1939 ノードの NAME が同じ |
| `lin-masters` | 誤り | 1 つの LIN のネットワークに master が 2 つ以上（同じ participant の 2 回も） |
| `lin-no-master` | 誤り（`describe: false` の participant がいるときは注記） | LIN の slave がいて、master がいない |
| `unused-network` | 警告（同上のときは注記） | どの participant も使わないネットワーク |
| `single-participant` | 警告（同上のときは注記） | participant が 1 つだけのネットワーク（送ったものを誰も受けない） |
| `not-checked` | 注記 | `describe: false` の participant（照合していない） |
| `participant-disabled` | 注記 | 外した participant（`enabled: false`、`--disable`。照合も起動もしない） |
| `participant-manual` | 注記 | 利用者が自分で起動する participant（`manual: true`、`--manual`。`run` は起動せず、コマンドを書いて待つ） |
| `version-mismatch` | 注記 | 実行ファイルをビルドした vehicle-sil の版が、`vehicle_sil_launch` の版と違う |
| `coarse-time-step` | 警告 | ファイルに `speed`（数）と `time_step_ms` があり、ECU のステップが刻みの境目で始まらない（`run` の所見と同じ文言。[等速で動かす](#等速で動かす)の「刻みと ECU のステップ」） |
| `control-link-not-offered` | 注記 | 変数のある ECU が、それをシミュレーションへ出すと申告していない（`--no-control`、申告のない古い実行ファイル）。`run` はその変数を読み書きできない（[変数を読み書きする口](#変数を読み書きする口)） |
| `control-link-version` | 警告 | 変数のある ECU が申告した、メッセージの形式の版（か運び手）が、`vehicle_sil_launch` と違う。`run` はその変数を読み書きできない |
| `usage` | 誤り | 引数の誤り（`--json` のときの JSON だけ） |
| `internal` | 誤り | `vehicle_sil_launch` 自身の想定外の失敗（`--json` のときの JSON だけ） |

- アドレスは、ノードを足したときに求めたアドレスです（[ecu-executable.md](ecu-executable.md) の `nodes`）。254（任意アドレスの範囲から空いているものを取る）は重なりとみなしません。同じアドレスを求めるノードについては、vehicle-sil の J1939 スタックの Address Claim で起きることを判定します。

| 同じアドレスを求めるノード | 起きること | 重さ |
|---|---|---|
| NAME のないノードがある | NAME のないノードは Address Claim をせず、そのアドレスから送り続ける | 誤り |
| NAME の最も小さいノードが 2 つ以上ある（同じ NAME） | 同じ NAME は Address Claim に負けるので、どれもアドレスを取れない | 誤り（`name-conflict` が、同じアドレスであることも書きます） |
| どれも NAME があり、NAME の最も小さいノード以外に、任意アドレスを取れない（NAME の任意アドレスの bit がない）ノードがある | NAME の最も小さいノードが取り、取れないノードはアドレスを失う（Cannot Claim） | 誤り |
| どれも NAME があり、NAME の最も小さいノード以外は、どれも任意アドレスを取れる | NAME の最も小さいノードが取り、ほかは空いたアドレスへ移る | 警告 |

- `address-shared` の「別のアドレスへ移る」は、移る先が空いていることを前提にします。NAME のないノードは Address Claim をしないので、任意アドレスの範囲にそのノードがいると、移ったノードと重なることがあります。照合は移る先を決めないので、これは確かめません。
- `unused-network`、`single-participant`、`lin-no-master` は、すべての participant の自己申告がそろったときだけ出します。照合できなかった participant（`failed` と `skipped`）がいる間は出しません（その participant の誤りを先に直します）。`describe: false` の participant がいるときは注記です（その participant が使っているかもしれないため。文言にもそう書きます）。
- ネットワークを使う participant は、名前と種類の両方が合うものです（表と JSON の `networks[].participants` も同じです）。
- `networks` にないネットワークの上の、アドレスと NAME の重なりは確かめません（先に `unknown-network` を直します）。種類を読めなかったネットワークも確かめません。

## 出力

標準出力に、人向けの表（既定）か JSON（`--json`）を書きます。同じ構成ファイルと実行ファイルからは、同じ出力になります。改行はどの OS でも LF です（Windows でも `\r\n` にしません）。構成ファイルの名前は引数に書いたとおりに、照合が組み立てたパス（探した実行ファイルなど）は、Windows でも `/` で区切って書きます。

### 人向け

participant の表（申告されたステップ幅とネットワーク。J1939 はノードの求めたアドレス、`any` は 254）、ネットワークの表（使う participant）、見つけたもの、最後に結果の 1 行（誤り、警告、注記の数）です。見つけたものは 1 つが 1 行で、`error:`、`warning:`、`note:` で始まり、`<ファイル>:<行>: <何が、どの値で、どう直すか>` の形です（行のないものはファイル名だけ）。誤り、警告、注記の順に、それぞれ行の順です。添える文（標準エラーの末尾、文法の誤りの場所など）は、次の行から 4 文字下げて書きます。ファイル名は、引数に書いたとおりです。表の幅は文字の数で揃えます（全角の文字があるとずれます）。

<!-- vehicle_sil_launch check simulation.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

passed: 0 errors, 0 warnings, 0 notes
```

誤りのある例です。同じ ECU を 2 つ載せ、ステップ幅とキーを書き間違えています。

<!-- two-engines.yaml -->
```yaml
vehicle_sil_simulation: 1
stop_at_ms: 1000
networks:
  - name: J1939_CAN
    kind: can
  - name: Body_LIN
    kind: lin
participants:
  - name: Engine
    command: [engine/vehicle_sil_engine_ecu]
    step_ms: 20
  - name: Engine2
    command: [engine/vehicle_sil_engine_ecu]
  - name: Dashboard
    command: [dashboard/vehicle_sil_dashboard_ecu]
    requried: true
```

<!-- vehicle_sil_launch check two-engines.yaml -->
```text
two-engines.yaml: 3 participants, 2 networks

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Engine2      10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Engine2, Dashboard
Body_LIN   lin   -

error: two-engines.yaml:4: network 'J1939_CAN': Engine and Engine2 have the same J1939 NAME 0x00000000FE000001, and they ask for the same address 0x00 (0), which none of them keeps (an equal NAME loses the address claim): each node needs its own NAME (for a second copy of an ECU, change its identity number)
error: two-engines.yaml:11: participant 'Engine': step_ms is 20, but the executable's step is 10 ms: write step_ms: 10, or remove step_ms
error: two-engines.yaml:16: participant 'Dashboard': unknown key 'requried': did you mean 'required'? (the keys here: name, command, step_ms, describe, required, enabled, manual)
warning: two-engines.yaml:6: network 'Body_LIN': no participant uses it: remove it, or check the network names in the ECUs

failed: 3 errors, 1 warning, 0 notes
```

### JSON（`--json`）

1 つの JSON のオブジェクトです。形は `--describe` の JSON と同じ決まりです（キーの順は固定、値がなくてもキーを省かない、字下げは空白 2 つ、改行は Windows でも LF）。

- 先頭の `vehicle_sil_check` は形式の版です。版が上がるのは、あるキーか `code` の意味が変わるとき、またはなくなるときです。キーと、`code`、`severity`、`result`、`participants[].status` の値は、版を上げずに増えることがあります。読む側は、知らない版を断り、知らないキーは無視し、知らない `code` はその `severity` で扱い、知らない `severity` は `"error"` として、知らない `result` は成功でないものとして（終了コードを見ます）、知らない `status` は照合できなかったものとして扱います。
- 値のないものは `null` です（空の文字列にしません）。配列は空の配列です。
- 構成ファイルを読めないとき（終了コード 3）も JSON を書きます（`result` が `"unreadable"`、`participants` と `networks` は空）。引数の誤り（終了コード 4）でも、`--json` があれば JSON を書きます（`result` が `"usage"`、`findings` に `code` が `usage` の誤り。`file` は引数から構成ファイルがわかればその名前、わからないとき（ファイルがない、2 つ以上など）は `null`）。

| キー | 値 |
|---|---|
| `vehicle_sil_check` | 形式の版（今は 1） |
| `vehicle_sil` | `vehicle_sil_launch` の vehicle-sil の版 |
| `file` | 構成ファイル（引数に書いたとおり） |
| `result` | `"passed"`、`"failed"`、`"unreadable"`、`"usage"` |
| `strict` | `--strict` を付けたか |
| `errors`、`warnings`、`notes` | 誤り、警告、注記の数 |
| `stop_at_ms`、`start_timeout_ms`、`stop_timeout_ms`、`registry_uri` | 構成ファイルの値（読めないときと、書いていない `stop_at_ms` と `registry_uri` は `null`。`start_timeout_ms` と `stop_timeout_ms` は既定の値も） |
| `participants[]` | 構成ファイルの順。`name`（読めなかったときは `null`）、`line`（行）、`command`、`describe`、`required`、`enabled`（`--disable` と `--enable` を当てた後の値）、`manual`（`--manual` を当てた後の値）、`step_ms`（なければ `null`）、`status`、`reported`、`variables`、`control` |
| `participants[].status` | `"described"`（自己申告を読んだ）、`"not_checked"`（`describe: false`）、`"failed"`（実行ファイルがない、`--describe` の失敗）、`"skipped"`（構成ファイルの誤りで進めなかった）、`"disabled"`（外した。項目に誤りがあっても） |
| `participants[].reported` | 自己申告（`described` のときだけ。ほかは `null`）: `vehicle_sil`、`name`、`step_ns`、`networks`（`--describe` の JSON と同じ形。変数は次の `variables`） |
| `participants[].variables` | ECU が申告した変数の配列（`ecu.connected`、続いて宣言の順）。要素の形は `--describe` の JSON の `variables[]` と同じです（`name`、`type`、`causality`、`unit`、`min`、`max`、`step`、`description`、`items`、`builtin`。[ecu-executable.md](ecu-executable.md) の「JSON の形式」）。空の配列は「変数が 0 個」です。`null` は「わからない」です: 自己申告のない participant（`describe: false`）、外した participant、自己申告を読めなかった participant、`variables` を書かない古い vehicle-sil の ECU。実行の前に、どの ECU にどの変数があるかを見るのに使います |
| `participants[].control` | ECU が申告した、変数の出し方（`transport`: いまは `"silkit"`、`wire_version`: メッセージの形式の版）。申告がない（`--no-control`、古い実行ファイル）、照合していない、失敗した、外した participant では `null` |
| `networks[]` | 構成ファイルの順。`name`、`kind`（読めなかったときは `null`）、`line`、`participants`（使う participant 名。構成ファイルの順） |
| `findings[]` | 人向けと同じ順。`severity`（`"error"`、`"warning"`、`"note"`）、`code`、`file`（構成ファイル。`--scenario` のシナリオについての所見では、シナリオのファイルで、`line` はその行）、`line`（なければ `null`）、`participant`、`network`（関係するもの。なければ `null`）、`message`（1 行）、`detail`（添える文。なければ `null`） |

<!-- engine-alone.yaml -->
```yaml
vehicle_sil_simulation: 1
networks:
  - name: J1939_CAN
    kind: can
participants:
  - name: Engine
    command: [engine/vehicle_sil_engine_ecu]
    step_ms: 20
```

<!-- vehicle_sil_launch check engine-alone.yaml --json -->
```json
{
  "vehicle_sil_check": 1,
  "vehicle_sil": "<version>",
  "file": "engine-alone.yaml",
  "result": "failed",
  "strict": false,
  "errors": 1,
  "warnings": 1,
  "notes": 0,
  "stop_at_ms": null,
  "start_timeout_ms": 30000,
  "stop_timeout_ms": 10000,
  "registry_uri": null,
  "participants": [
    {
      "name": "Engine",
      "line": 6,
      "command": [
        "engine/vehicle_sil_engine_ecu"
      ],
      "describe": true,
      "required": true,
      "enabled": true,
      "manual": false,
      "step_ms": 20,
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
    }
  ],
  "networks": [
    {
      "name": "J1939_CAN",
      "kind": "can",
      "line": 3,
      "participants": [
        "Engine"
      ]
    }
  ],
  "findings": [
    {
      "severity": "error",
      "code": "step-mismatch",
      "file": "engine-alone.yaml",
      "line": 8,
      "participant": "Engine",
      "network": null,
      "message": "participant 'Engine': step_ms is 20, but the executable's step is 10 ms: write step_ms: 10, or remove step_ms",
      "detail": null
    },
    {
      "severity": "warning",
      "code": "single-participant",
      "file": "engine-alone.yaml",
      "line": 3,
      "participant": null,
      "network": "J1939_CAN",
      "message": "network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it",
      "detail": null
    }
  ]
}
```

この文書の構成ファイルと出力の例は、CTest の `launch_cli.docs` が、実際の実行ファイルと照合します（`vehicle_sil` の版を除いて、バイト単位で）。

## シミュレーションの実行（`vehicle_sil_launch run`）

`vehicle_sil_launch run <file>` は、次の順に動きます。

1. `check` と同じ照合をします。誤りがあれば（`--strict` では警告も）、何も起動せずに照合の結果を書き、終了コード 1 です。
2. registry をこのプロセスの中で立てます（`registry_uri`、なければ 127.0.0.1 の空いているポート）。標準エラーの最初の行が、その URI です（`[vehicle_sil_launch] registry at silkit://127.0.0.1:<port>`）。`--control` を付けたときは、その前に Control Server を立て、`[vehicle_sil_launch] control server at <address>` の行を書きます。
3. controller（participant 名 `SimulationController`）をこのプロセスの中で動かします。シミュレーションが待つ（必須の）participant は、外していない `required: true` のものです。controller は刻み（`time_step_ms` か、その既定値）で時刻同期に参加し、刻みの終わりを止めて、速さ、一時停止、ステップ送りを決めます（最速のときも同じです。刻みごとに同期が 1 回増えます）。
4. 外していない participant を構成ファイルの順に、それぞれ別のプロセスで起動します（`manual` の participant は起動せず、起動するコマンドを書きます）。作業ディレクトリは構成ファイルのあるディレクトリ、標準入力は空です。
5. 必須の participant がそろったら、変数を出す ECU とつながるのを待ってから（下の「変数を読み書きする口」。ふつうは待ちません）始めます。`--control` と `--start-paused` を付けたときは、どの participant も時刻 0 のステップを始める前で、`resume`、`step` か `run_until` の命令を待ちます（[control-api.md](control-api.md) の「始まる前に書く」）。`stop_at_ms` があれば、仮想時間がそこに達したところで止めます。なければ、participant が止めるか、Ctrl+C まで動きます。
6. 終わったら、すべての participant を終わらせ（下の「止め方」）、結果を書きます。

`--scenario <file>` を付けると、照合でシナリオも確かめ、始まった後はシナリオの手順を進めて、シナリオが終わった所でシミュレーションを止めます（[scenario.md](scenario.md)）。このとき、controller の刻みは、速さを決めたときの既定（`time_step_ms` か、ECU のステップがすべて controller のステップから始まる刻み）です。

### 見張ること

- 始まる前: 必須の participant のプロセスが終わったら、`start_timeout_ms` を待たずにやめます（`ended-before-start`）。`start_timeout_ms` の間に参加しなかった必須の participant は、全部を挙げます（`not-joined`）。
- 実行中: 必須の participant のプロセスが 0 でない終了コードで（またはシグナルで）終わったら、すぐにシミュレーションを中断します。controller から見て、必須の participant がシミュレーションを去ったときも中断します（Error の状態ならすぐに、ほかの状態なら 5 秒たってもシミュレーションが終わらないとき）。この規則は `vehicle_sil_sim_controller` と `vehicle_sil::Simulation` と同じです。中断の理由になった participant は `participant-failed` です。中断されても、ほかの ECU は自分で終わります（終了コード 0）。
- `required: false` の participant は待たず、終わってもシミュレーションを止めません。0 でない終了コードは警告です。
- `--timeout-ms <ms>` を付けると、シミュレーションが始まってから `<ms>` の実時間で終わらなければ中断し、誤り（`timeout`）にします。ステップの中で止まった ECU がいると仮想時間が進まず、`stop_at_ms` があっても終わらないためです（CI などで使います）。一時停止している時間も数えます。既定は上限なしです。manual の必須の participant がいる実行では、participant を待ち始めた時点から数えます（[participant を自分で起動する](#participant-を自分で起動するmanual--manual)）。
- 一時停止している間も、上の見張りはどれも同じように効きます（participant の失敗、止める要求、`--timeout-ms`）。`start_timeout_ms` は、そろうまでの時間です。`--start-paused` で 0 に止まっている時間は含みません。
- 始まる前の窓（変数を出す ECU を待つ間と、`--control` と `--start-paused` で時刻 0 のステップの前に止まっている間）に必須の participant が抜けると（プロセスが終わった、接続が切れた）、すぐに開始をやめます（`ended-before-start`。プロセスが動いているまま抜けたときは `simulation-start`）。
- 始まるまでの上限: `start_timeout_ms` は、そろうまでの時間で、そろった後の開始（最初のステップまで）にも、その残り（少なくとも、変数を出す ECU を待つ時間 + 1 秒）が効きます。`--control` と `--start-paused` で時刻 0 のステップの前に止まっている間は数えず、進んだ後に改めて `start_timeout_ms` が効きます。過ぎると `simulation-start` の誤りで、最初のステップに着いていない participant と状態を文言に挙げます。
- 利用者が起動する（manual の）必須の participant がいる実行では、manual の participant が参加するまでと、全員が参加した後、manual の participant が最初のステップの準備を終えるまでは上限がありません。`run` が起動した participant が参加するまでと、manual の participant の準備が終わった後には、`start_timeout_ms` が効きます（[participant を自分で起動する](#participant-を自分で起動するmanual--manual)）。
- `--timeout-ms` は、シミュレーションが始まってから数えます。`--control` と `--start-paused` では、時刻 0 のステップの前に止まった時点から数えます（止まっている時間も数えます）。止まったまま過ぎると、時刻 0 のステップを走らせてから中断し、`timeout` の文言に、最初のステップの前に止まっていたことを書きます。進めた後に過ぎたときは、`timeout` の文言に、そのうち何 ms 止まっていたかを書きます。
- `stop_at_ms` を書いたのに、participant がそれより前にシミュレーションを止めたときは、注記（`ended-before-stop-at`）を出します（`--strict` でも失敗にしません）。

### 変数を読み書きする口

`vehicle_sil_launch run` は、controller の participant の上に、ECU の変数を読み書きする側（`vehicle_sil::control::Hub` と `vehicle_sil::control_silkit::HubTransport`）を持ちます。`ecu_main` で作った ECU は、自分から SIL Kit の publish/subscribe でつながります（どの ECU にも `ecu.connected` があるので、変数を宣言していない ECU もつながります）（[ecu-executable.md](ecu-executable.md) の「変数を出す」）。構成ファイルに書くことはありません。gRPC は使いません。しくみと約束は [architecture.md](architecture.md) の「SIL Kit の上の運び手」にあります。

- 変数を出すかどうかは、ECU の実行ファイルが `--describe` で自己申告します（`control`: 運び手と、メッセージの形式の版。[ecu-executable.md](ecu-executable.md) の「JSON の形式」）。照合の段階で、出さない ECU と版の違う ECU がわかります（下の表の所見は、照合の所見です）。
- 始まる前: 必須の participant がそろうと、全員が最初のステップの前で待ちます。その間に、変数があり、それを `vehicle_sil_launch` と同じ版で出すと申告した、必須で外していない ECU の hello と、「コマンドを受け取れる」という返事を待ちます。SIL Kit が participant をつなぐ順序から、hello は窓が開く前に届いているので、ふつうは待ちません。申告したのに来ない ECU がいるときだけ、最長 5000 ms 待ちます。待っている間に Ctrl+C か止める命令が来れば、待つのをやめます。この待ちは `start_timeout_ms` の後に足されることがあります（そろうのが期限の間際だったとき）。申告していない ECU は待ちません。
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

### 等速で動かす

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
- 一時停止、ステップ送り、途中での速さの変更は、標準入力の命令と、Control Server（`--control`）のクライアントの呼び出しです（[標準入力の命令](#標準入力の命令run---stdin-commands)）。`run --start-paused` は、全員がそろったら仮想時間 0 で止まり、`resume` か `step` を待ちます（`--stdin-commands` か `--control` と一緒に使います）。`--stdin-commands` だけのときは時刻 0 のステップを走らせてから止まり、`--control` があるときは時刻 0 のステップの前で止まります。

### 止め方

シミュレーションが終わったら（中断と Ctrl+C のときも）、次の順に participant を終わらせます。

1. 必須の participant が自分で終わるのを、`stop_timeout_ms` まで待ちます（ecu_main の実行ファイルは、シミュレーションが終われば自分で終わります）。
2. まだ動いている participant に、止まるよう求めます（Linux: その participant のプロセスグループに SIGTERM。Windows: そのプロセスグループに Ctrl+Break）。`required: false` の participant は、ここで止めます（警告にしません）。必須の participant は誤りです（`did-not-end`）。
3. さらに `stop_timeout_ms` まで、止まるよう求めた participant と、それが起動したプロセス（プロセスグループ、Windows ではジョブオブジェクト）が終わるのを待ちます。残ったものを、それが起動したプロセスごと強制終了します（Linux: プロセスグループに SIGKILL。Windows: ジョブオブジェクトの終了）。
4. 自分で終わった participant でも、それが起動したプロセスが残っていれば終わらせ、警告にします（`left-processes`）。

利用者が起動する（manual の）participant には、どの段も当てません（[participant を自分で起動する](#participant-を自分で起動するmanual--manual)の「止め方」）。

シミュレーションが始まらなかったときは、1 を飛ばして 2 からです（その participant の誤りにはしません）。ecu_main の実行ファイルは、始まる前に止まるよう求められると、すぐに終わります（[ecu-executable.md](ecu-executable.md)）。

各 participant は、自分のプロセスグループ（Windows ではプロセスグループとジョブオブジェクト）で動きます。端末の Ctrl+C は `vehicle_sil_launch` にだけ届き、participant には届きません。Windows で `vehicle_sil_launch` にコンソールがない（participant と共有するコンソールがない）ときは、Ctrl+Break を送れないので、2 の代わりにすぐ強制終了します（文言にそう書きます）。このとき participant は、コンソールの窓を開かないように起動します。

### 中断（Ctrl+C）

Ctrl+C（SIGINT）、SIGTERM、SIGHUP（Windows: Ctrl+C、Ctrl+Break、コンソールを閉じる）を受けると、`vehicle_sil_launch` はシミュレーションを順に止め（controller の stop）、上の止め方で participant を終わらせ、結果に中断を書いて、終了コード 130 で終わります。始まる前なら、participant を止めます。シミュレーションが終わった後、participant が自分で終わるのを待っている間の中断は、その待ちをやめて、止まるよう求めます（止め方の 2）。2 回目の中断では、待つのをやめて、残っている participant をすぐに強制終了します。照合の間の中断は、照合が終わってから効きます（何も起動しません）。`vehicle_sil_launch` を起動した側が無視するように設定したシグナル（`nohup` の SIGHUP など）は、無視したままです。端末の Ctrl+Z（SIGTSTP）は `vehicle_sil_launch` だけを止めます。participant は別のプロセスグループなので動き続け、registry と controller だけが止まります。

Windows でコンソールを閉じると、Windows は数秒でプロセスを終わらせます。participant はジョブオブジェクトが終わらせますが、結果を書けないことがあります。

プログラムから `vehicle_sil_launch` を動かすときは、`--stdin-commands` を付けて、標準入力に `{"command":"stop"}` を書くか、標準入力を閉じると、Ctrl+C と同じように順に止まります（[機械から使う](#機械から使う--events--stdin-commandsversion)）。シグナルも Ctrl+Break も送れない、コンソールのない Windows の `vehicle_sil_launch` も、これで順に止められます（シミュレーションを止めると ecu_main の実行ファイルは自分で終わります。Ctrl+Break を送れないので、自分で終わらない participant は、止め方の 2 の代わりにすぐ強制終了します）。

### participant を残さないこと

- `vehicle_sil_launch` は、終わる前に、起動したすべての participant を終わらせます。
- participant は、標準入力（空）、標準出力、標準エラー（`vehicle_sil_launch` が読むパイプ）のほかに、`vehicle_sil_launch` のファイルやハンドルを受け継ぎません。
- Windows: participant はそれぞれジョブオブジェクト（最後のハンドルが閉じたら、中のプロセスを終わらせる）に入ります。`vehicle_sil_launch` がどう終わっても（強制終了を含む）、participant とそれが起動したプロセスは終わります。
- Linux: participant は、`vehicle_sil_launch` が終わると SIGKILL を受けます（親が終わったときのシグナル）。ただし、`vehicle_sil_launch` が強制終了されたとき（SIGKILL、クラッシュ）は、止め方の 3 と 4 が動かないので、participant が起動したプロセスは残ることがあります。participant のプロセスグループを抜けたプロセス（`setsid()` などを呼んだもの）は、どの場合も終わらせられません。
- Linux 以外の POSIX は対象にしていません（親が終わったときのシグナルがないので、`vehicle_sil_launch` が強制終了されると participant が残ります）。

### 出力

- participant の標準出力と標準エラーは、行ごとに `[<participant 名>] ` を付けて、`vehicle_sil_launch` の標準エラーに流します。`vehicle_sil_launch` 自身の経過は `[vehicle_sil_launch] ` で始まります（registry の URI、起動したコマンド、始まった、終わった、止めた）。4096 バイトを超える行は、UTF-8 の文字の間で分けて流します。制御文字（タブを除く C0、DEL、U+0080 から U+009F の C1）と、UTF-8 でないバイトは `?` にします。participant は UTF-8 で書いてください（Windows のコンソールにも UTF-8 として表示するので、ほかのコードページの文字は化けます）。標準エラーを読む側が止まる（読まれないパイプ）と、`vehicle_sil_launch` も書くところで止まります。
- `--log-dir <dir>` を付けると、participant ごとに `<dir>/<participant 名>.log` にも書きます（`[名前]` なし。名前の ASCII の英数字、`.`、`-`、`_` 以外のバイトは `_` にし、Windows の装置の名前（`NUL`、`CON`、`PRN`、`AUX`、`COM1`〜`COM9`、`LPT1`〜`LPT9`。`.` の前も）には、どの OS でも後ろに `_` を付けます。大文字と小文字だけが違う名前は重なりとみなし、`-2` などを付けます）。ディレクトリがなければ作ります。前の実行のログは、同じ名前のものだけを書き直します（今の構成にない participant のログは残ります）。JSON の `participants[].log` が、どのファイルかを示します。
- `--events` のときは、ここに書いた行（participant の行と `vehicle_sil_launch` の経過）を標準エラーに書かず、事象として標準出力に書きます（[機械から使う](#機械から使う--events--stdin-commandsversion)）。
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

外した participant がいるときの例です。照合の部分は `check` と同じです（[participant を外す](#participant-を外すenabled--disable--enable)）。

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
| `timeout` | 誤り | シミュレーションが `--timeout-ms` の実時間で終わらなかったので、中断した（manual の必須の participant がいる実行では、始まらなかったときも。待っていたものを書きます） |
| `simulation` | 誤り | シミュレーションが正常に終わらなかった（理由の participant がわからない） |
| `ended-before-stop-at` | 注記 | `stop_at_ms` より前に、participant がシミュレーションを止めた |
| `control-link-no-hello` | 警告 | 変数を出すと申告した ECU の hello が、始まる前に来なかった。または hello は来たが、コマンドを受け取れるという返事が来なかった（文がどちらかを言います。[変数を読み書きする口](#変数を読み書きする口)） |
| `control-link-dropped` | 警告 | `vehicle_sil_launch` が、変数の口のメッセージを捨てた（受け付けなかった hello、読めなかったメッセージ、知っている ECU やその変数に合わなかったメッセージの数を書きます）。考えられる原因: 変数を出す participant が上限（256）を超えた、版の違う vehicle-sil の ECU がいる、別のプログラムが同じトピック（`vehicle_sil.control.*`）に書いている、実行中に参加した participant の hello がまだ届いていなかった。該当する participant の変数は、欠けているか古いことがあります |
| `control-link-version` | 警告 | 届いた hello の、メッセージの形式の版が `vehicle_sil_launch` と違う（申告の版が違うときは、照合の同じ `code` の所見で、実行では繰り返しません）。同じ版の vehicle-sil でビルドする |
| `control-link-late-values` | 警告 | ECU の値か結果が、仮想時刻の約束より遅れて届いた（ステップの外から送っている。自作の通信の側の誤り） |
| `before-start` | 誤り | 始まる前の窓でする処理（`run` を関数として呼ぶ側が渡すもの。内部用）が失敗した。シミュレーションは始まっていて、すぐに中断した |
| `behind-real-time` | 警告 | 速さを合わせた刻みのうち、遅れが `late_tolerance_ms` を超えたものがあった（[等速で動かす](#等速で動かす)）。遅れた刻みの数、最大の遅れ、ずれの合計、全体の実際の速さ、止まったまま終わったならそのときの遅れを書きます。どの participant の所見でもありません |
| `coarse-time-step` | 警告 | 速さの指定があり、controller の刻みが ECU のステップより粗い（ECU のステップが刻みの境目で始まらない）。ECU のステップが最大で何 ms 遅れてまとまって動くかと、`time_step_ms` に書く値を書きます（[等速で動かす](#等速で動かす)の「刻みと ECU のステップ」）。どの participant の所見でもありません |
| `interrupted` | 注記 | 中断された |

### JSON（`run --json`）

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
| `participants[]` | 構成ファイルの順（外した participant も）。`name`、`required`、`enabled`（`false` なら外した participant で、`ended` は `"not_started"`、`log` は `null`。シミュレーションが待つのは `required` と `enabled` がどちらも `true` のもの）、`manual`（`true` なら利用者が起動する participant で、`ended` は `"not_started"`、`exit_code` は `null`。[participant を自分で起動する](#participant-を自分で起動するmanual--manual)）、`ended`（`"exited"`、`"stopped"`、`"killed"`、`"not_started"`）、`exit_code`（シグナルで終わったとき、強制終了したとき、起動しなかったときは `null`）、`signal`（`"SIGSEGV"` など。なければ `null`）、`left_processes`（起動したプロセスを残したか）、`log`（`--log-dir` のファイル。なければ `null`）、`control_link`（始まったときの、変数を読み書きする口のつながり方。下の表。起動しなかった participant は `state` が `"none"`） |
| `participants[].control_link` | `state`（`"connected"`、`"no_hello"`、`"version_mismatch"`、`"not_offered"`、`"none"`。[変数を読み書きする口](#変数を読み書きする口)）、`variables`（`connected` のとき、その ECU が出す変数の数。ECU の実行が足す `peer.` などの出力を含むので、`--describe` の数より多いことがあります。ほかは `null`）、`version`（`version_mismatch` のとき、相手の形式の版。ほかは `null`）、`late_messages`（約束より遅れて届いたメッセージの数。ふつうは 0） |
| `findings[]` | 実行で見つけたもの（照合の `findings[]` と同じ形） |
| `check` | 照合の結果（`check --json` と同じオブジェクト） |
| `scenario` | `--scenario` を付けたときだけ。シナリオの結果（[scenario.md](scenario.md) の「出力」）。`check` の中にも `scenario`（`file`、`name`、`steps`）があります。`simulation` の `"ended"` は、シナリオが終わってランチャーが止めた場合を含みます。見分けるには `scenario.status` を見ます（`"passed"` か `"failed"` なら、シナリオが終わって止めた実行です） |

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

## 機械から使う（`--events`、`--stdin-commands`、`version`）

GUI やスクリプトなど、`vehicle_sil_launch` を起動して見届けるプログラム向けです。`run --events` は、実行の経過を事象として標準出力に書き、最後に結果を書きます。`run --stdin-commands` は、標準入力から命令を読みます。`version --json` は、この `vehicle_sil_launch` が書く形式の版と、できることの一覧です。

```sh
vehicle_sil_launch version --json
vehicle_sil_launch check --json simulation.yaml
vehicle_sil_launch run --events --stdin-commands simulation.yaml
```

### 事象（`run --events`）

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
| `check` | 照合が終わったとき | `check`: `check --json` と同じオブジェクト（[JSON](#json--json)） |
| `control_server` | `--control` の Control Server が待ち受けを始めたとき（`registry` の前） | `address`: クライアントがつなぐアドレス（ポート 0 なら選ばれたポート）、`tls`: TLS か、`credentials`: 呼び出しにトークンが要るか |
| `registry` | registry が待ち受けを始めたとき | `uri`: participant が接続する URI |
| `participant` | participant の状態が変わったとき | `name`、`state`（下の表） |
| `control_link` | シミュレーションが始まった直後（`phase` の `running` の前）に、起動した participant と、利用者が起動する（manual の）participant ごとに 1 回 | `name`、`state`（結果の `participants[].control_link.state` と同じ 5 つ）。`connected` には `variables`、`version_mismatch` には `version`（結果の同じ名前のキーと同じ意味。ほかの `state` にはこの 2 つのキーがありません） |
| `log` | participant か `vehicle_sil_launch` が 1 行書いたとき | `source`（participant 名か `vehicle_sil_launch`）、`line`（その行。[`log` の行](#log-の行)） |
| `log_dropped` | 読む側が遅くて `log` を書けなかったとき、その場所に（[読む側が遅いとき](#読む側が遅いとき読まないとき)） | `count`（書かなかった `log` の数）、`sources`（`source` ごとの数のオブジェクト。例: `{"Engine": 120, "vehicle_sil_launch": 1}`） |
| `finding` | 実行で見つけたものが決まったとき | 結果の `findings[]` の 1 つと同じキー（`severity`、`code`、`file`、`line`、`participant`、`network`、`message`、`detail`） |
| `scenario_step` | `--scenario` の手順が始まったとき、決まったとき | 結果の `scenario.steps[]` の 1 つと同じキー（[scenario.md](scenario.md) の「出力」）。始まったときの `status` は `running` |
| `time` | 仮想時間が進んだとき、一時停止に入ったとき・出たとき、速さが変わったとき | `sim_ms`: controller が見ている仮想時間（ミリ秒。減りません）、`speed`: 今の速さ（数か `"max"`）、`paused`: 一時停止しているか、`actual_speed`: 直近 1 秒ほどの、進んだ仮想時間 ÷ 実時間（小数 3 桁。動いた時間が短いとき、一時停止しているときは `null`）、`late_steps`: それまでに遅れた刻みの数、`max_late_ms`: それまでの最大の遅れ、`slipped_ms`: それまでのずれの合計（どれも結果の `timing` と同じ意味で、いま実行が終わったとしたときの値）、`late_ms`: 期限を過ぎてもまだ始まっていない刻みの、いまの遅れ（なければ 0）。シミュレーションが止まっている間（仮想時間が進まず、`late_ms` が許容を超えて増えている間）も、100 ms に 1 回まで出ます。`paused` は、止まる予定の時刻に着いた時点で `true` になります（始まった直後の `--start-paused` など。controller が実際に止まるのは、その刻みの participant のステップが終わった後です） |
| `command` | 標準入力から命令を受けたとき（`--stdin-commands`）、Control Server のクライアントが時間の操作か停止を呼んだとき（`--control`） | `command`（命令の名前。読めなければ `null`）、`source`（`"stdin"`: 行で来た、`"eof"`: 標準入力の終わり、`"control_server"`: クライアントの呼び出し）、`accepted`（受け付けたか）、`message`（受け付けなかった理由。受け付けたら `null`）。受け付けた `step` には `ms`（実際に進む仮想時間。刻みの倍数に切り上げたもの）。クライアントからは `run_until` と、時刻つきの `pause` も来ます。受け付けたものには `at_ms`（止まる仮想時刻。刻みの倍数に切り上げたもの） |
| `heartbeat` | 1 秒のあいだ何も書かなかったとき | なし（3 つのキーだけ） |
| `result` | 最後の行 | `result`: `run --json` と同じオブジェクト（[JSON](#jsonrun---json)） |

`participant` の `state`:

| `state` | 意味 | ほかのキー |
|---|---|---|
| `disabled` | 外した participant（起動しません） | |
| `not_started` | プロセスを起動できなかった | `message`: 理由 |
| `started` | プロセスを起動した | `pid`（そのプロセスの ID）、`command`（起動した実行ファイルと引数の配列）、`log`（`--log-dir` のファイル。なければ `null`） |
| `manual` | 利用者が起動する participant なので、起動しなかった（`started` の代わり。[participant を自分で起動する](#participant-を自分で起動するmanual--manual)）。この後、参加すれば `joined` が来ます。`ended`、`stop_requested`、`killed` は来ません | `command`（起動する実行ファイルと引数の配列）、`working_directory`（起動するディレクトリ）、`command_line`（同じものを、この OS のシェルに貼れる 1 行にしたもの。標準エラーに書く行と同じ） |
| `joined` | シミュレーションに参加した（controller がその participant のライフサイクルの状態を見た） | |
| `failed` | `vehicle_sil_launch` が、この participant の失敗で実行をやめると決めた | `reason`: `left_simulation`（プロセスは動いているが、シミュレーションから抜けた。プロセスが終わるときは、接続が先に切れるので、終わりより先にこちらで気づくこともあります）、`process_ended`（シミュレーションの途中で、プロセスが失敗として終わった）、`ended_before_start`（シミュレーションが始まる前にプロセスが終わった）。`message`: この失敗の `finding` の `message` と同じ文。その時点で文が決まっているときだけあります（`left_simulation` にはありません） |
| `stop_requested` | 止まるよう求めた（止め方の 2） | |
| `ended` | プロセスが終わった | `exit_code`、`signal`（結果の `participants[]` と同じ意味） |
| `killed` | 強制終了した（それが起動したプロセスも） | |

#### 順序の約束

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

#### 実行に進まないときの事象

`--events` を付けた `run` は、引数が誤っていても `hello` を最初に、`result` を最後に書きます（`heartbeat` が間に入ることがあります）。

| 場合 | 事象 | `result` の `result` | 終了コード |
|---|---|---|---|
| 引数の誤り（知らないオプション、構成ファイルの指定がない、`--json` と一緒に付けた、など） | `hello`、`result` | `"usage"` | 4 |
| 構成ファイルを読んでわかる引数の誤り（`--disable` と `--enable` の名前が構成ファイルにない） | `hello`、`phase`（`checking`）、`check`、`phase`（`ended`）、`result` | `"usage"` | 4 |
| 構成ファイルを読めない | 同じ 5 つ | `"unreadable"` | 3 |
| 照合で誤りが見つかった | 同じ 5 つ | `"failed"` | 1 |
| 標準出力がない（閉じた状態で起動された） | 何も書けません。標準エラーに誤りを書きます | | 4 |

`run` でないコマンド（`check`、`version`、コマンドなし）に `--events` を付けたときは、事象を書きません。引数の誤りとして標準エラーに書き、終了コードは 4 です。

#### `log` の行

`line` は、participant が標準出力か標準エラーに書いた 1 行（行末の改行なし）、または `vehicle_sil_launch` の経過の 1 行です。`--events` なしで標準エラーに書く行から、前の `[<名前>] ` を除いたものと同じです。

- 制御文字（タブを除く）と、UTF-8 として正しくないバイトは、`?` に置き換えます。
- 4096 バイトを超える行は、4096 バイト以下の複数の `log` に分けます（UTF-8 の文字の途中では切りません）。続きであることを示すキーはありません。
- `--log-dir` のファイルには、置き換える前のバイトをそのまま書きます。読む側が遅くて `log` を書けなかったときも、ファイルにはすべての行があります。

#### 読む側が遅いとき、読まないとき

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

#### 読む側がいなくなったとき

標準出力を読む側がいなくなった（パイプが閉じた）ことには、次に行を書くときに気づきます。`heartbeat` があるので、どの段階でも 1 秒以内です。上の「読まない」の条件に当てはまったときも、同じ扱いです。

- 事象の流れを閉じます。その後は、`result` を含めて何も書きません（書きかけの行が途中で切れることがあります）。
- 止める要求として扱います（1 回目の Ctrl+C と同じ）。すでに止める要求を受けていれば、何もしません（2 回目に数えません）。
- 標準エラーに理由を 1 行書きます（`vehicle_sil_launch: cannot write the events (<理由>): stopping the run`）。すでに止める要求を受けていたときは、行の終わりが `no more events are written` です。`result` を書くのを待っている間に閉じたときは、この行ではなく、上の `error: cannot write the result ...` の行です。標準エラーも読まれていないと、この行は届きません（`vehicle_sil_launch` は、それでも終わります）。
- 結果の `interrupted_by` は `"output closed"`、終了コードは 130 です（SIGPIPE で終わりません）。

標準出力が、書き込みで待たない設定（`O_NONBLOCK` など）のパイプでも同じように動きます。いっぱいのときは、閉じたとは扱わず、空くのを待ちます。

#### 読む側が守ること

- 標準出力を読み続けます。`result` の行が来たら、流れは終わりです。
- 改行で終わらない最後の行は捨てます（`vehicle_sil_launch` が強制終了された、流れが閉じた）。
- `result` が来ないまま標準出力が終わったら、`vehicle_sil_launch` が結果を書けずに終わっています（強制終了、クラッシュ、上の「いなくなった」の扱い）。プロセスの終了コードで判断します。
- `heartbeat` を含めて何の行も数秒来なければ、`vehicle_sil_launch` が止まっています（1 秒に 1 行は書きます）。
- 知らない `event` と知らないキーは無視します。`event`、キー、`state` と `phase` の値は、版を上げずに増えることがあります。キーの意味が変わるとき、なくなるときは `vehicle_sil_events` を上げます。
- 標準エラーも読みます（読み捨ててもかまいません）。`--events` のとき、標準エラーに書くのは `vehicle_sil_launch` 自身の知らせ（引数の誤り、事象を書けなくなった、結果を書けない、想定外の失敗）だけで、participant の行も経過も書きません。`vehicle_sil_launch` の中のライブラリが標準出力に書いたものは、標準エラーに行きます（事象の行に混ざりません）。
- `vehicle_sil_launch` を起動して見届けるプログラム（GUI、CI のスクリプト）は、`--stdin-commands` も付けます。そのプログラムが止まるか終わると標準入力が閉じるので、`vehicle_sil_launch` は標準出力の状態によらず、すぐに順に止まります。

#### そのほかの決まり

- 文字列のエスケープは `--json` と同じです（`"`、`\`、制御文字。UTF-8 でないバイトは U+FFFD）。`log` の `line` は、上のとおり先に `?` に置き換えています。
- 行の長さに上限はありません（`check` と `result` は大きくなります）。
- `--json` と `--events` を一緒に付けるのは引数の誤りです（事象の最後が結果なので）。
- 終了コードは `--events` なしと同じです。

#### 例

短い実行の全行です（`--disable Dashboard` で、出力の順が決まる Engine だけにしています）。時刻、プロセス ID、registry のポート、例のディレクトリ、版、時間の進み方は、実行ごとに違うので `<t_ms>`、`<pid>`、`<port>`、`<dir>`、`<version>`、`<wall_ms>`、`<actual_speed>` にしています（Windows では、実行ファイルの名前に `.exe` が付きます）。

<!-- vehicle_sil_launch run simulation.yaml --events --disable Dashboard -->
```text
{"event":"hello","seq":0,"t_ms":<t_ms>,"vehicle_sil_events":1,"vehicle_sil":"<version>","pid":<pid>,"file":"simulation.yaml","arguments":["run","simulation.yaml","--events","--disable","Dashboard"]}
{"event":"phase","seq":1,"t_ms":<t_ms>,"phase":"checking"}
{"event":"check","seq":2,"t_ms":<t_ms>,"check":{"vehicle_sil_check":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"stop_at_ms":1000,"start_timeout_ms":30000,"stop_timeout_ms":10000,"registry_uri":null,"participants":[{"name":"Engine","line":11,"command":["engine/vehicle_sil_engine_ecu"],"describe":true,"required":true,"enabled":true,"manual":false,"step_ms":10,"status":"described","reported":{"vehicle_sil":"<version>","name":"Engine","step_ns":10000000,"networks":[{"name":"J1939_CAN","kind":"can","use":"j1939","nodes":[{"address":0,"j1939_name":"0x00000000FE000001","arbitrary_address_capable":false}]}]},"variables":[{"name":"ecu.connected","type":"bool","causality":"input","unit":"","min":null,"max":null,"step":null,"description":"on its buses; false: off all of them (CAN and LIN), its steps go on","items":[],"builtin":true},{"name":"engine.throttle","type":"real","causality":"input","unit":"%","min":0,"max":100,"step":null,"description":"accelerator pedal position","items":[],"builtin":false},{"name":"engine.speed","type":"integer","causality":"output","unit":"rpm","min":null,"max":null,"step":null,"description":"","items":[],"builtin":false}],"control":{"transport":"silkit","wire_version":1}},{"name":"Dashboard","line":14,"command":["dashboard/vehicle_sil_dashboard_ecu"],"describe":true,"required":true,"enabled":false,"manual":false,"step_ms":5,"status":"disabled","reported":null,"variables":null,"control":null}],"networks":[{"name":"J1939_CAN","kind":"can","line":8,"participants":["Engine"]}],"findings":[{"severity":"warning","code":"single-participant","file":"simulation.yaml","line":8,"participant":null,"network":"J1939_CAN","message":"network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it","detail":null},{"severity":"note","code":"participant-disabled","file":"simulation.yaml","line":14,"participant":"Dashboard","network":null,"message":"participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it","detail":null}]}}
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
{"event":"result","seq":22,"t_ms":<t_ms>,"result":{"vehicle_sil_run":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"simulation":"reached_stop_at","interrupted_by":null,"timing":{"speed":"max","time_step_ms":10,"ecu_step_ms":10,"step_delay_ms":0,"late_tolerance_ms":10,"sim_ms":1000,"wall_ms":<wall_ms>,"actual_speed":<actual_speed>,"paced_steps":0,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0,"paused_ms":0},"participants":[{"name":"Engine","required":true,"enabled":true,"manual":false,"ended":"exited","exit_code":0,"signal":null,"left_processes":false,"log":null,"control_link":{"state":"connected","variables":18,"version":null,"late_messages":0}},{"name":"Dashboard","required":true,"enabled":false,"manual":false,"ended":"not_started","exit_code":null,"signal":null,"left_processes":false,"log":null,"control_link":{"state":"none","variables":null,"version":null,"late_messages":0}}],"findings":[],"check":{"vehicle_sil_check":1,"vehicle_sil":"<version>","file":"simulation.yaml","result":"passed","strict":false,"errors":0,"warnings":1,"notes":1,"stop_at_ms":1000,"start_timeout_ms":30000,"stop_timeout_ms":10000,"registry_uri":null,"participants":[{"name":"Engine","line":11,"command":["engine/vehicle_sil_engine_ecu"],"describe":true,"required":true,"enabled":true,"manual":false,"step_ms":10,"status":"described","reported":{"vehicle_sil":"<version>","name":"Engine","step_ns":10000000,"networks":[{"name":"J1939_CAN","kind":"can","use":"j1939","nodes":[{"address":0,"j1939_name":"0x00000000FE000001","arbitrary_address_capable":false}]}]},"variables":[{"name":"ecu.connected","type":"bool","causality":"input","unit":"","min":null,"max":null,"step":null,"description":"on its buses; false: off all of them (CAN and LIN), its steps go on","items":[],"builtin":true},{"name":"engine.throttle","type":"real","causality":"input","unit":"%","min":0,"max":100,"step":null,"description":"accelerator pedal position","items":[],"builtin":false},{"name":"engine.speed","type":"integer","causality":"output","unit":"rpm","min":null,"max":null,"step":null,"description":"","items":[],"builtin":false}],"control":{"transport":"silkit","wire_version":1}},{"name":"Dashboard","line":14,"command":["dashboard/vehicle_sil_dashboard_ecu"],"describe":true,"required":true,"enabled":false,"manual":false,"step_ms":5,"status":"disabled","reported":null,"variables":null,"control":null}],"networks":[{"name":"J1939_CAN","kind":"can","line":8,"participants":["Engine"]}],"findings":[{"severity":"warning","code":"single-participant","file":"simulation.yaml","line":8,"participant":null,"network":"J1939_CAN","message":"network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it","detail":null},{"severity":"note","code":"participant-disabled","file":"simulation.yaml","line":14,"participant":"Dashboard","network":null,"message":"participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it","detail":null}]}}}
```

この例は、CTest の `launch_run.events` が、実際の出力から `heartbeat` の行と、最初と最後のほかの `time` の行（遅い機械では途中の時刻も書きます）を除き（`seq` は詰めます）、上の置き換えをした後で、バイト単位で照合します。

### 標準入力の命令（`run --stdin-commands`）

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

### 版とできること（`vehicle_sil_launch version`）

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

## 引数と終了コード

```text
vehicle_sil_launch check [--json] [--strict] [--scenario <file>] [--describe-timeout-ms <ms>] [--disable <name>]...
                         [--enable <name>]... [--manual <name>]... [--] <file>
vehicle_sil_launch run [--json | --events [--events-stall-ms <ms>]] [--stdin-commands] [--start-paused]
                       [--control <address> [--control-credentials <file>] [--control-tls-cert <file> --control-tls-key <file>]
                       [--control-audit-log <file>]]
                       [--scenario <file>] [--speed <speed>] [--strict] [--log-dir <dir>] [--timeout-ms <ms>]
                       [--describe-timeout-ms <ms>] [--disable <name>]... [--enable <name>]... [--manual <name>]...
                       [--] <file>
vehicle_sil_launch version [--json]
```

| 引数 | 意味 |
|---|---|
| `--json` | 結果を JSON で書く |
| `--events` | `run` だけ。経過と結果を事象として書く（[機械から使う](#機械から使う--events--stdin-commandsversion)）。`--json` と一緒には使えない |
| `--events-stall-ms <ms>` | `--events` を付けた `run` だけ。事象の書き込みがこの時間まったく進まなければ、読む側がいなくなったものとして扱う。`result` を書き終えるのを待つ時間の上限でもある（1000 以上 86400000 以下の整数。既定は 30000。[読む側が遅いとき](#読む側が遅いとき読まないとき)） |
| `--stdin-commands` | `run` だけ。標準入力から命令を読む。標準入力の終わりも止める要求 |
| `--start-paused` | `--stdin-commands` か `--control` を付けた `run` だけ。全員がそろったら仮想時間 0 で止まり、`resume` か `step` の命令を待つ（`--control` があれば、時刻 0 のステップの前で止まる） |
| `--control <address>` | `run` だけ、Control Server を持つビルドだけ。Control API（gRPC）の Server を立てる。`<address>` はポート番号（127.0.0.1 で待ち受ける。0 なら空いているポート）、`<host>:<port>`、`unix:<path>`。ループバックでないアドレスには、TLS と認証情報が要る（[control-api.md](control-api.md) の「ランチャーにつなぐ」） |
| `--control-credentials <file>` | `--control` だけ。トークンと役割のファイル（1 行に `<role> <name> <token>`）。あれば、どの呼び出しにもトークンが要る |
| `--control-tls-cert <file>`、`--control-tls-key <file>` | `--control` だけ。サーバーの証明書の鎖と秘密鍵（PEM）。2 つ一緒に付けると TLS |
| `--control-audit-log <file>` | `--control` だけ。誰が何を変えたかを追記する |
| `--scenario <file>` | シナリオのファイル（[scenario.md](scenario.md)）。`check` では照合だけ、`run` では照合してから実行する。シナリオが終わるとシミュレーションを止め、手順が失敗していれば実行は失敗。`--start-paused` と一緒には使えない。`--control` と一緒なら、クライアントは見るだけ（[scenario.md](scenario.md) の「実行中に変数を見る」） |
| `--speed <speed>` | `run` だけ。構成ファイルの `speed` を上書きする（数か `max`。[等速で動かす](#等速で動かす)） |
| `--strict` | 警告があっても失敗にする（注記では失敗にしない） |
| `--log-dir <dir>` | `run` だけ。participant ごとの出力のファイルを置くディレクトリ |
| `--timeout-ms <ms>` | `run` だけ。シミュレーションの開始から終わりまでの実時間の上限（1 以上の整数。一時停止している時間も数える。`--control` と `--start-paused` では、時刻 0 のステップの前に止まった時点から。manual の必須の participant がいる実行では、participant を待ち始めた時点から）。超えたら中断して誤り。既定は上限なし |
| `--describe-timeout-ms <ms>` | 1 つの実行ファイルの `--describe` を待つ時間（1 以上の整数。既定は 10000） |
| `--disable <name>` | participant `<name>` を外す（`enabled: false` と同じ）。繰り返せる |
| `--enable <name>` | participant `<name>` を戻す（`enabled: true` と同じ）。繰り返せる |
| `--manual <name>` | participant `<name>` を自分で起動する（`manual: true` と同じ）。`run` は起動せず、コマンドを書いて待つ。繰り返せる |
| `--` | これより後はファイル名（`-` で始まる名前のとき） |
| `--help`、`-h` | 使い方を標準出力に書いて 0 で終わる（`--` より前のどこにあっても） |
| `--version` | `version` と同じ（`--` より前のどこにあっても） |

| 終了コード | 意味 |
|---|---|
| 0 | 誤りがない（警告と注記だけなら 0。`--strict` では警告もない）。`run` では、シミュレーションが正常に終わり、必須の participant がどれも 0 で終わった |
| 1 | 誤りがある（`--strict` では警告も）。`run` では、照合の誤り（何も起動しない）か、実行の誤り（participant が失敗した、そろわなかった、止めても終わらなかったなど） |
| 3 | 構成ファイルを読めない（ファイルがない、文法の誤り、知らない形式の版など）。`run` で registry や controller を用意できないときも 3 です。結果を標準出力に書けないときと、`vehicle_sil_launch` 自身の想定外の失敗も 3 です（標準エラーに理由。`--json` では、そのコマンドの形の JSON に `code` が `internal` の誤り） |
| 4 | 引数の誤り（知らないコマンドや引数、`check` の `--events` と `--stdin-commands`、`run` の `--json` と `--events`、`--events` なしの `--events-stall-ms` と範囲の外の値、`--stdin-commands` も `--control` もない `--start-paused`、`--control` なしの `--control-credentials` などと Control Server を持たないビルドの `--control`、`--control` の値の誤り、読めない `--control-*` のファイル、認証情報が 1 件もない `--control-credentials` のファイル、片方だけの `--control-tls-cert` と `--control-tls-key`、TLS と認証情報のない、ループバックでない `--control`、数でも `max` でもない `--speed`、`--start-paused` と一緒の `--scenario`、`version` の `--json` 以外の引数、構成ファイルの名前がない、2 つ以上のファイル、同じ引数の 2 回目（`--disable` と `--enable` は繰り返せます）、値のない `--describe-timeout-ms`、`check` の `--log-dir`、`--timeout-ms`、`--speed`、`--start-paused`、`--control` と `--control-*`、構成ファイルにない participant 名の `--disable`、`--enable`、`--manual`、同じ名前の `--disable` と `--enable`、同じ名前の `--disable` と `--manual`）。誤りと使い方を標準エラーに書きます |
| 130 | `run` が中断された（Ctrl+C、`{"command":"stop"}`、標準入力の終わり、閉じた標準出力、Control Server のクライアントの `Stop` など） |

- Windows では、引数をワイド文字のコマンドラインから UTF-8 で読みます（ASCII 以外の文字を含むパスを使えます）。
