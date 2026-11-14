# シミュレーションの構成ファイル（`vehicle_sil_launch`）

まとめる人向けです。先に [ecu-executable.md](ecu-executable.md)（ECU の実行ファイルの引数、`--describe` の JSON、止め方）を読みます。次は [simulation-check.md](simulation-check.md)（照合と所見）と [simulation-run.md](simulation-run.md)（実行、止め方、結果）です。プログラムから動かすなら、その後に [simulation-events.md](simulation-events.md) を読みます。

各自が作った ECU の実行ファイル（[ecu-executable.md](ecu-executable.md)）を 1 つのシミュレーションにまとめるときに、まとめる人が書く構成ファイルです。構成ファイルには、participant にする実行ファイル、それらがつながるネットワーク、止める時刻を書きます。`vehicle_sil_launch check` は、構成ファイルと、各実行ファイルの自己申告（`--describe`）を突き合わせ、食い違いを 1 回で全部出します。`vehicle_sil_launch run` は、同じ照合をしてから、シミュレーション全体を起動し、終わりまで見届けて、各 participant がどう終わったかを出します。形式と規則はまだ変わることがあります。

`check` はシミュレーションを始めず、registry にも接続しません。ただし、`describe: true` の実行ファイルは、1 つずつ `--describe` で起動します。`--describe` は ECU を組み立てるので、その実行ファイルのコード（ECU のコンストラクターなど）が動きます。構成ファイルは、実行するコマンドの一覧です。信頼できない構成ファイルを `check` にも `run` にも渡さないでください。`describe: false` の実行ファイルは起動せず、あることだけを確かめます。

`run` は、照合に通ったときだけ、すべての participant を起動します（[simulation-run.md](simulation-run.md)）。

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
# yaml-language-server: $schema=../../schema/simulation.schema.json
# The two ECUs of this directory in one simulation. CMake copies this file next to the executables
# (build/<preset>/examples/ecu_main/); there, vehicle_sil_launch check simulation.yaml checks it and
# vehicle_sil_launch run simulation.yaml runs it.
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
| `start_timeout_ms` | | 必須の participant がそろうまで待つ時間（ミリ秒、1 以上 10^12 以下の整数。書かなければ 30000）。`run` が ecu_main の実行ファイルに渡す `--start-timeout-ms` は、これより長い値です（[ecu_main の実行ファイル](#ecu_main-の実行ファイルdescribe-true)） |
| `stop_timeout_ms` | | `run` が participant の終わりを待つ時間（ミリ秒、1 以上 10^12 以下の整数。書かなければ 10000）。止め方の各段で、この時間だけ待ちます |
| `registry_uri` | | `run` が registry を立てる場所（`silkit://<host>:<port>`、ポートは 1 以上 65535 以下）。書かなければ 127.0.0.1 の空いているポートです（自分で起動する必須の participant がいる実行では `silkit://127.0.0.1:8500`。[participant を自分で起動する](#participant-を自分で起動するmanual--manual)）。ほかのコンピューターの participant も参加させるときは `silkit://0.0.0.0:8500` のように書きます（このコンピューターの participant には `127.0.0.1` の URI を渡します）。ループバックのアドレス（`127.x.x.x`、`[::1]`、`localhost`）でなければ、注記 `registry-on-network` を出し、`run` が起動する participant に環境変数 `VEHICLE_SIL_LISTEN_ON_NETWORK=1` を渡します（[待ち受けるアドレス](simulation-run.md#待ち受けるアドレス)）。手順は [howto/multi-pc.md](howto/multi-pc.md) です |
| `speed` | | 仮想時間を進める速さ（実時間 1 秒あたりの仮想時間の秒数）。`1` が等速、`2` が 2 倍速、`0.5` が半分の速さです（0.000001 以上 1000000 以下の数）。`max` か、書かなければ最速です。`run --speed` で上書きできます（[等速で動かす](simulation-run.md#等速で動かす)） |
| `time_step_ms` | | controller の刻み（ミリ秒の仮想時間、1 以上 10^12 以下の整数）。一時停止とステップ送りの細かさ、速さを合わせる細かさです。書かなければ、最速のときは 10 です（`stop_at_ms` が 10 の倍数でなければ、`stop_at_ms` と 10 の最大公約数）。速さを指定したときは、さらに ECU のステップ幅との最大公約数にします（[等速で動かす](simulation-run.md#等速で動かす)の「刻みと ECU のステップ」）。書いたときは `stop_at_ms` の約数にします（誤り `value`） |
| `late_tolerance_ms` | | 遅れの許容（実時間のミリ秒、1 以上 10^12 以下の整数）。刻みの遅れがこれを超えたら、遅れた刻みに数えます。書かなければ 10 です（刻みと速さによりません） |
| `networks` | はい | ネットワークの配列（空でもかまいません）。ECU が使うネットワークは、すべてここに書きます |
| `networks[].name` | はい | ネットワークの名前（ECU の `j1939()`、`can()`、`lin_master()`、`lin_slave()`、`ethernet()` に渡す名前） |
| `networks[].kind` | はい | `can`、`lin`、`ethernet` のどれか |
| `participants` | はい | participant の配列（1 つ以上） |
| `participants[].name` | はい | participant 名。照合では、ecu_main の実行ファイルに `--name` で渡します。空でなく、1 行で（制御文字なし）、`-` で始まらないこと。`SimulationController`（vehicle-sil の controller の participant 名）は使えません |
| `participants[].command` | はい | 実行ファイルと、その引数の配列。引数がなければ文字列 1 つでもかまいません（空白で分けません） |
| `participants[].step_ms` | | 実行ファイルが申告するステップ幅（ミリ秒、1 以上 10^12 以下の整数）。書いたときだけ照合します |
| `participants[].describe` | | `true`（既定）: ecu_main の実行ファイル（照合します）。`false`: それ以外の実行ファイル（照合しません） |
| `participants[].required` | | `true`（既定）か `false`。シミュレーションがその participant を待つか。`false` は `describe: false` の participant にだけ書けます。外していない participant のうち、少なくとも 1 つは `true` の participant が要ります（誤り `no-required-participant`） |
| `participants[].enabled` | | `true`（既定）か `false`。`false` の participant は外します（照合も起動もしません。[participant を外す](#participant-を外すenabled--disable--enable)） |
| `participants[].manual` | | `false`（既定）か `true`。`true` の participant は、`run` が起動せず、利用者が自分で起動します（デバッガーからなど。[participant を自分で起動する](#participant-を自分で起動するmanual--manual)） |
| `participants[].role` | | `ecu`（既定）、`test`、`bridge`。その participant が何のためのものか。照合だけが読み、実行の動きは変わりません（[試験のための participant とブリッジ](#試験のための-participant-とブリッジrole)） |
| `variants` | | 機種の対応（機種の名前 → 共通の部分との差分）。書いた構成は、`check` と `run` に `--variant <名前>` が要ります（[機種](#機種variants--variant)） |
| `params` | | シナリオのパラメーターの値（名前 → 値）。シナリオの `${名前}` に入ります（[シナリオのパラメーター](#シナリオのパラメーターparams--param)） |
| `participants[].variables` | | 変数の対応表。シミュレーションで使う名前から、この実行ファイルの変数への対応を書きます（[変数の名前と値を合わせる](#変数の名前と値を合わせるvariables)）。書いていない変数は、実行ファイルの名前のまま見えます |

- 知らないキーはエラーです（打ち間違いを黙って無視しません）。近い名前があれば示します。同じキーを 2 回書くのもエラーです。
- **エディターの補完**: JSON Schema は [schema/simulation.schema.json](../schema/simulation.schema.json) です。YAML の言語サーバーを使うエディターでは、ファイルの先頭に `# yaml-language-server: $schema=<このファイルへのパス>` と書くと、キーの補完と説明、形の検査が効きます（上の例の 1 行目。パスは、書いているファイルのディレクトリから数えます）。スキーマが見るのは、キーと値の形だけです。実行ファイルと合っているか、同じキーの 2 回目、キーの組み合わせは、`check` が確かめます。JSON で書いたファイル（最初の文字が `{`）にはコメントを書けないので、エディターの設定で、ファイルとスキーマを結び付けます（VS Code なら `json.schemas` に、ファイルの名前のパターンとスキーマのパスを書く）。ファイルの中に `"$schema"` のキーは書けません（ランチャーが、知らないキーの誤りにします）。ひな形には `schema/` に写しが入っていて、`get_tools.py` が、取ってきた道具の版のスキーマで上書きします。道具の圧縮ファイルでは `schema/` にあります。
- 整数は引用符なしで書きます（`"5000"` は文字列なのでエラーです）。真偽値は `true` か `false` です（引用符なし。`True`、`TRUE`、`False`、`FALSE` も読みます。`yes`、`on` などは誤りです）。
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
- `--no-control` は `command` に書けます。その ECU は変数を出しません。照合の `--describe` にも同じ引数が渡るので、自己申告の `control` が `null` になり、`run` はその ECU の hello を待ちません（[変数を読み書きする口](simulation-run.md#変数を読み書きする口)）。
- `--silkit-config <file>` も `command` に書けます（`command: [engine/vehicle_sil_engine_ecu, --silkit-config, engine.silkit.yaml]`）。その ECU に SIL Kit の構成ファイルを渡します。相対パスは、構成ファイルのあるディレクトリから数えます（participant の作業ディレクトリ）。照合の `--describe` にも渡るので、ファイルがない、SIL Kit が受け付けない、participant 名と違う `ParticipantName` がある、といった誤りは、照合で `describe-failed` として分かります（[ecu-executable.md](ecu-executable.md) の「SIL Kit の構成を渡す」）。
- 照合では、`command` の引数（`{{語}}` を `{語}` に戻したもの）に `--describe --name <participant 名>` を足して動かします。`run` では `--name <participant 名> --registry-uri <URI> --start-timeout-ms <ms>` を足します。`<ms>` は `start_timeout_ms + stop_timeout_ms + 10000` です（開始を待つのはランチャーで、来なかった participant を挙げるのもランチャーです。ECU の制限は、ランチャーがいなくなったときのためのものです）。自分で起動する必須の participant がいる実行と、`--control` と `--start-paused` で時刻 0 のステップの前に止まる実行では、ECU が始まるまで待つ時間に上限がないので、10^12（`1000000000000`）です（始まらない participant は、ランチャーの `start_timeout_ms` が、止まる前と進んだ後に見つけます）。
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

warning: simulation.yaml:8: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it [single-participant]
note: simulation.yaml:14: participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it [participant-disabled]

passed: 0 errors, 1 warning, 1 note
```

### participant を自分で起動する（`manual`、`--manual`）

ECU をデバッガーから動かすときなど、ある participant を `run` ではなく自分で起動するときは、その participant に `manual: true` を書きます。ファイルを書き換えずに、その実行だけ自分で起動するときは、`check` と `run` に `--manual <participant 名>` を付けます（繰り返せます）。`run` はその participant を起動せず、起動するコマンドを書いて、参加してくるのを待ちます。

- 照合は、ほかの participant と同じです（`describe: true` なら `--describe` を動かします）。manual の participant ごとに注記 `participant-manual` を出します。
- `run` は、その participant を起動する順番のところで、作業ディレクトリと、起動するコマンドの 1 行を書きます（下の例）。引数は、`run` が自分で起動するときと同じです（ecu_main の実行ファイルには `--name`、`--registry-uri`、`--start-timeout-ms`）。そのコマンドを、書いてあるディレクトリで実行します（デバッガーでは、実行ファイル、引数、作業ディレクトリに分けて設定します）。引数の引用は、Linux では POSIX sh（bash、zsh）に貼る形です。Windows では、コマンドプロンプト（cmd.exe）に貼る形で、C ランタイムがコマンドラインを読む決まりで引用し、`&` `|` `<` `>` `^` を含む引数も `"` で囲みます。実行ファイルのパスは `\` で区切ります。cmd.exe は `"` の中でも `%...%` を展開するので、`%` を含む引数は直して貼ります。PowerShell に貼るときは、`"` で囲んだ実行ファイルの前に `&` を足し、`$` と `` ` `` を含む引数を直します。`--events` では、事象 `participant` の `state` が `manual` の行が、同じものを持ちます。
- `registry_uri` がループバックのアドレスでないときは、コマンドの前の行に「with VEHICLE_SIL_LISTEN_ON_NETWORK=1 in its environment」と書き、注記 `participant-manual` にも書きます。その変数を設定して起動します（[待ち受けるアドレス](simulation-run.md#待ち受けるアドレス)）。ほかのコンピューターで起動するときは、`--registry-uri` を、`registry at` の次の行（`from other computers, join at ...`）の URI に変えます。そこでは変数は要りません（[howto/multi-pc.md](howto/multi-pc.md)）。
- 待っている間に、構成ファイルにない名前の participant がシミュレーションの開始を待っていると（`--name` の打ち間違いなど。2 秒 ServicesCreated のまま、または Error）、`participant '<名前>' joined the simulation, but this run has no participant of that name (did you mean '<近い名前>'?): it waits for the simulation's start, which only the participants of the file get; ...` の行を 1 回書きます。その participant は始まりません。`run` は、構成ファイルの名前の participant を待ち続けます。構成ファイルにない participant でも、自分で動く（autonomous の）もの（実時間の Peer、ブリッジ）は、シミュレーションの参加者として扱い、何も書きません。
- `registry_uri` を書いていなければ、manual の必須の participant がいる実行は、registry を `silkit://127.0.0.1:8500` で立てます。コマンドが実行ごとに変わらないので、デバッガーの設定を 1 度作れば使い回せます。そのポートを別のプログラムが使っていれば `setup` の誤りです（`registry_uri` に別のポートを書きます）。
- 待ち方: manual の必須の participant がいる実行では、開始の時間の上限を 3 つの区間に分けます。(1) 参加するまで: `run` が起動した participant は、今までどおり `start_timeout_ms` の間に参加しなければ `not-joined` です。manual の participant は、参加するまで待ちます。manual でない必須の participant がそろうと、`waiting for <名前> to join (manual): ...` の行を 1 回書きます。(2) 全員が参加した後、manual の participant がすべて最初のステップの準備を終える（ReadyToRun）まで: 上限はありません（デバッガーで止めていてもかまいません）。(3) その後、全員が最初のステップに着くまで: `start_timeout_ms` です（`run` が起動した participant が初期化で固まれば、今までどおり `simulation-start` の誤りです）。どの区間でも、Ctrl+C、`stop` の命令、Control Server の `Stop` で止まります（終了コード 130）。
- `--timeout-ms` は、manual の必須の participant がいる実行では、`run` が participant を待ち始めた時点から数えます（参加を待つ時間、開始を待つ時間、実行の時間の合計です）。始まる前に過ぎると `timeout` の誤りで、何を待っていたか（参加していない participant か、最初のステップに着いていない participant）を文言に挙げます。`manual: true` を書いた構成ファイルを CI で動かすときは、`--timeout-ms` を付けると、誰も起動しなくても終わります。
- 参加した manual の participant が、始まる前に終わる（デバッガーで止めて起動し直すなど）と、開始は失敗します（`simulation-start`。その participant はこの実行には参加し直せないので、ランチャーも起動し直します）。
- `required: false` の manual の participant は、待ちません（コマンドを書くだけです）。
- 実行中: 見張りは、ほかの必須の participant と同じです。ただし、`run` はそのプロセスを見ていないので、失敗は、シミュレーションから抜けたこと（接続が切れた、Error の状態）でわかります（`participant-failed`）。その participant の出力は、起動した所（端末、デバッガー）に出ます。デバッガーで止めている間は、仮想時間が進みません。速さを指定していれば、その間を遅れとして数え（`behind-real-time`）、`--timeout-ms` も数えます。
- 止め方: `run` は manual の participant のプロセスに、止める要求も強制終了も送りません。シミュレーションが終われば、ecu_main の実行ファイルは自分で終わります。シミュレーションが始まらなかったとき（中断、ほかの participant の失敗）は、参加していた manual の participant に SIL Kit の abort を送ります（ecu_main の実行ファイルは、開始の失敗として終わります）。どちらの場合も、参加した manual の participant がシミュレーションを去るのを `stop_timeout_ms` まで待ってから、registry を閉じます。去らなければ（デバッガーで止めているなど）、そう書いて終わります（誤りにはしません）。この待ちは、Ctrl+C か `stop` の命令をもう 1 回送ると切り上げます。`run` が強制終了されたとき（SIGKILL、クラッシュ、Windows でコンソールを閉じた）は、自分で起動した participant は残ることがあります（始まる前なら、開始を待ち続けます）。自分で終わらせます。
- participant をデバッガーで止めたままでも、`run` は止められます（Ctrl+C、`stop` の命令）。上の待ちの後、結果を書いて終わります。止めたままの participant は残るので、自分で続行させるか、終わらせます。
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

note: simulation.yaml:11: participant 'Engine': you start it yourself (manual: true, or --manual): run does not start it, but prints the command to start it with, and waits for it to join without a time limit (with --timeout-ms, at most that long) [participant-manual]

run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       started by you (manual)
Dashboard    yes       exit code 0

passed: 0 errors, 0 warnings, 1 note
```

### 試験のための participant とブリッジ（`role`）

`role` は、participant が何のためのものかを照合に伝えます。照合の規則と表示だけが使い、`run` は、どの値でも同じに起動し、同じに参加させます（`--describe` の JSON と、シミュレーションの通信には入りません）。

| 値 | 意味 | 照合で変わること |
|---|---|---|
| `ecu`（既定） | 車両の ECU | なし |
| `test` | 試験のための participant（聞くだけの spy、ECU に送る道具など） | ネットワークの ECU として数えません（`single-participant` は、spy を足しても消えません）。J1939 のノードを申告していて、ブリッジがいるかもしれないネットワークにいると、警告 `test-participant-talks-on-bridged-network` です |
| `bridge` | ネットワークを実機のバスへ運ぶ participant（[bridges.md](bridges.md)） | 上の警告で、ブリッジとして扱います |

- ブリッジ: `role: bridge` の participant がいれば、それだけをブリッジとして扱います。自己申告した（`describe: true`）ブリッジは、使うと申告したネットワークだけを運びます。`describe: false` のブリッジは、どのネットワークを運ぶかが照合にはわからないので、どのネットワークも運ぶかもしれないとみなします。`role: bridge` の participant が 1 つもなければ、`role` を書いていない `describe: false` の participant を「ブリッジかもしれない」とみなします（警告の文もそう言い分けます）。ブリッジでない道具には `role: ecu` か `role: test` を書くと、そうみなしません。
- 警告の理由: ブリッジは、SIL Kit の側で送られたものを実機のバスへ出します。J1939 のノードを持つ試験用の participant の送信、とくにアドレスの主張（Address Claimed）も実機に出て、同じアドレスの実機の ECU とアドレスを取り合います。直し方は、その participant をそのネットワークでは聞くだけにする（J1939 のノードを作らない）か、その participant を使う実行ではブリッジを外す（`--disable <ブリッジ>`、機種の `enabled: false`）かです。生の CAN で送る participant は、自己申告からはわからないので、警告しません。
- 人向けの participant の表では、`ecu` でない participant の名前の後に `(role: test)` のように書きます。`check --json` の `participants[].role` に出ます。
- 機種（`variants`）では変えられません（`describe`、`required` と同じです）。

### 変数の名前と値を合わせる（`variables`）

同じ役目の ECU でも、実行ファイルによって変数の名前、単位、値の表し方が違うことがあります（他人の ECU、古い版、機種ごとの違い）。`variables` に対応を書くと、シミュレーションの側（シナリオ、`check --json`、Control API とその先の GUI）からは、どの実行ファイルも同じ名前と値で見えます。実行ファイルは変えません。名前を ECU の側でそろえられるなら、そのほうが簡単です。対応表は、そろえられないもののためにあります。

```yaml
participants:
  - name: Engine
    command: [engine/engine_small]
    variables:
      engine.throttle: throttle_pct                        # 名前を替える
      engine.speed: { from: rpm_x10, scale: 0.1, unit: rpm }   # 値を一次式で変える
      engine.gear: { from: gear_raw, map: { P: 0, R: 1, N: 2, D: 3 } }   # 値に名前を付ける
      engine.turbo: { absent: true }                       # この実行ファイルにはない
```

キーは、シミュレーションで使う名前です。値は次の 4 つの形のどれかです。1 つの変数に書ける形は 1 つで、ここにないキーは誤りです。

| 形 | 意味 |
|---|---|
| `<実行ファイルの変数名>`（文字列） | 名前だけを替えます。`{ from: <名前> }` と同じです |
| `{ from, scale, offset }` | 見える値 = 実行ファイルの値 × `scale` + `offset`。`scale` は 0 以外の数（書かなければ 1）、`offset` は数（書かなければ 0）。整数か実数の変数に書けます |
| `{ from, map: { <名前>: <値>, ... } }` | 実行ファイルの値（整数）に名前を付けます。整数か列挙の変数に書けます。1 つの値に付けられる名前は 1 つです |
| `{ absent: true }` | この実行ファイルには、その変数がありません。ほかのキーと一緒には書けません |

`from` のある形には、`unit`（見える値の単位）も書けます。

見え方は次のとおりです。

- 対応を書いた変数は、キーの名前でだけ見えます。実行ファイルの名前では読み書きできません。変数の並びは、実行ファイルが宣言した順のままです。
- `scale`、`offset`: 型は実数になります。範囲（`min`、`max`）と刻み（`step`）は見える値に直ります（`scale` が負なら、範囲の両端が入れ替わります）。単位は `unit` に書いたもので、書かなければ空です（実行ファイルの単位は、変えた後の値には当てはまりません）。
- 書き込みは逆に直します（実行ファイルの値 = (書いた値 − `offset`) ÷ `scale`）。実行ファイルの変数が整数なら、最も近い整数に丸めます（ちょうど半分は 0 から遠いほうへ: 2.5 は 3、−2.5 は −3）。そのため、書いた値と読み戻した値は、丸めの分だけ違うことがあります。シナリオで比べるときは `near` と `tolerance` を使います。範囲の外の値は、実行ファイルに届く前に断ります。範囲に入っているかは、直した後の実行ファイルの値で決めます（シナリオの照合も同じ決め方です）。一覧に出る範囲の両端は、有効数字 15 桁に丸めた値です。
- `map`: 型は列挙になり、項目は `map` に書いたものだけです。数は実行ファイルの値のままです。実行ファイルによって数が違っても、シナリオに名前で書けば（`engine.gear: D`）同じシナリオが使えます。`map` にない値は書けません。
- 入力、出力、パラメーターの別は変わりません。
- `absent: true` の変数は、どこにも見えません。その変数を使うシナリオの手順は飛ばします（[scenario.md](scenario.md) の「機種とパラメーター」）。対応も `absent` も書いていない変数が実行ファイルになければ、シナリオの照合で誤りになります。

照合（`check`、`run` の最初）は、対応の全部を実行ファイルの自己申告と比べます（`from` の変数があるか、形が型に合うか、`map` の値を変数が取れるか、名前がほかの変数と重ならないか。所見は [simulation-check.md](simulation-check.md) の `variable-map-*`）。合わない対応が 1 つでもあれば、`run` は何も起動しません。`check --json` の `participants[].variables` は対応の後の変数で、`participants[].variable_map` に両方の名前が出ます。

`describe: false` の participant にも `variables` は書けますが、実行の前には比べられません（注記 `variable-map-not-checked`）。対応は、その participant の変数の口がつながったときに当てます。合わなかった対応は当てず、そのときに実行の行（`[vehicle_sil_launch] participant ...: variables: ...`、事象では `log`）で知らせ、終わりに実行の所見 `variable-map-not-applied`（誤り）にします。その実行の間、合わなかった対応の変数は実行ファイルの名前と値のまま読めますが、その対応の名前（キーと `from`）への書き込みは断ります（共通の名前のつもりで書いた値が、別の変数や変換なしの値として入るのを防ぐためです）。

### 機種（`variants`、`--variant`）

実行ファイルや変数の対応が機種ごとに違うときは、共通の部分を `participants` に書き、機種ごとの差分を `variants` に書きます。`check` と `run` は、`--variant <名前>` で選んだ機種の差分を共通の部分に重ねた構成を、照合し、実行します（全部の機種を順に回すのは `test` です。[simulation-run.md](simulation-run.md#シナリオをまとめて回すvehicle_sil_launch-test)「シナリオをまとめて回す」）。シナリオは共通の名前だけを使うので、どの機種にも同じものを当てられます。

```yaml
participants:
  - name: Engine
    command: [./engine_small]
    variables:
      engine.turbo: { absent: true }
  - name: Dashboard
    command: [./dashboard]
params: { idle_rpm: 800 }
variants:
  small: {}                           # 共通の部分のまま
  medium:
    participants:
      Engine:
        command: [./engine_medium]                   # 実行ファイルを替える
        variables: { engine.turbo: turbo_bar }       # 対応を重ねる
      Dashboard: { enabled: false }                  # この機種にはいない
    params: { idle_rpm: 650 }                        # パラメーターを替える
```

| キー | 値 |
|---|---|
| `variants.<機種>` | その機種の差分。`{}`（か、値なし）は共通の部分のままです。機種の名前は、空でなく、1 行で、`-` で始まらないこと |
| `variants.<機種>.participants.<participant>` | 共通の部分の participant の、この機種での違い。キーは participant 名で、`participants` にある名前だけを書けます（誤り `variant-participant`） |
| `…<participant>.command` | 実行ファイルと引数。共通の `command` の全体を置き換えます。書き方と決まりは `participants[].command` と同じです |
| `…<participant>.variables` | 変数の対応表。共通の `variables` に、対応 1 つずつ重ねます: 同じ名前（キー）の対応は置き換え、ない名前は足します |
| `…<participant>.enabled` | `true` か `false`。共通の `enabled` を置き換えます |
| `variants.<機種>.params` | パラメーター。共通の `params` に、名前ごとに重ねます |

- 機種で変えられるのは、この 4 つ（`command`、`variables`、`enabled`、`params`）だけです。ほかのキーは誤りです（`unknown-key`）。ネットワーク、`step_ms`、`describe`、`required`、`role` などは、どの機種でも同じです。
- 機種を書いた構成で `--variant` を省くと、引数の誤りです（終了コード 4。文に機種の一覧が出ます）。既定の機種はありません。機種のどれでもない名前の `--variant` も、引数の誤りです。構成ファイルの文面にほかの誤りがあっても、先にこれを出します（共通の部分だけを、機種の代わりに照合することはしません。文には、ファイルにも誤りがあることを足します）。機種のない構成への `--variant` も引数の誤りですが、こちらは `--disable` と同じで、文面に誤りがある間は言いません（`variants` を読めなかったせいかもしれないため）。
- ある機種にだけいる participant は、`participants` に `enabled: false` で書き、その機種で `enabled: true` にします。
- 対応を共通の部分より減らすことはできません。共通の対応が要らない機種では、同じ名前に、その機種の対応を書きます（`engine.speed: engine.speed` は、名前も値もそのままです）。
- 読むときには、選んでいない機種も含めて、全部の機種の形を確かめます。実行ファイルとの照合は、選んだ機種だけです。
- `--disable`、`--enable`、`--manual` は、機種を重ねた後に効きます。
- `check --json` には、`variant`（選んだ機種）と `variants`（全部の名前）が出ます。`participants` は、機種を重ねた後のものです。

### シナリオのパラメーター（`params`、`--param`）

シナリオの中の数（しきい値、待ち時間、入れる値）を機種ごとに替えるときは、シナリオに `${名前}` と書き、値を `params` に書きます。値は、数、`true`・`false`、文字列のどれかです（1 つの値。配列や対応は書けません）。名前は、英数字と `_`（数字で始めない）です。

値は、後のものが前のものを置き換えます: シナリオの `params`（既定）、構成の `params`、機種の `params`、引数の `--param <名前>=<値>`。`--param` は `--scenario` と一緒にだけ使えます。シナリオが使っていない名前の `--param` は引数の誤りです（終了コード 4。打ち間違いを黙って無視しません。いくつものシナリオを回す `test` では、どのシナリオも使っていない名前だけが誤りです）。構成と機種の `params` には、シナリオが使わない名前があってもかまいません（1 つの構成で、いくつものシナリオを回すため）。書き方と誤りは [scenario.md](scenario.md) の「機種とパラメーター」にあります。

## 引数と終了コード

```text
vehicle_sil_launch check [--json] [--strict] [--variant <name>] [--scenario <file> [--param <name>=<value>]...]
                         [--describe-timeout-ms <ms>] [--disable <name>]...
                         [--enable <name>]... [--manual <name>]... [--] <file>
vehicle_sil_launch run [--json | --events [--events-stall-ms <ms>]] [--stdin-commands] [--start-paused]
                       [--control <address> [--control-credentials <file>] [--control-tls-cert <file> --control-tls-key <file>]
                       [--control-audit-log <file>]]
                       [--variant <name>] [--scenario <file> [--param <name>=<value>]...]
                       [--speed <speed>] [--strict] [--log-dir <dir>] [--timeout-ms <ms>]
                       [--describe-timeout-ms <ms>] [--disable <name>]... [--enable <name>]... [--manual <name>]...
                       [--] <file>
vehicle_sil_launch test [--json] [--strict] [--fail-fast] [--variant <name>]... [--param <name>=<value>]...
                        [--timeout-ms <ms>] [--describe-timeout-ms <ms>] [--disable <name>]... [--enable <name>]...
                        <file> --scenarios <directory or files>...
vehicle_sil_launch version [--json]
```

`test` は、機種ごとにシナリオを 1 つずつ回して、全部の結果を表にします（[simulation-run.md](simulation-run.md#シナリオをまとめて回すvehicle_sil_launch-test)「シナリオをまとめて回す」）。

| 引数 | 意味 |
|---|---|
| `--json` | 結果を JSON で書く |
| `--events` | `run` だけ。経過と結果を事象として書く（[機械から使う](simulation-events.md)）。`--json` と一緒には使えない |
| `--events-stall-ms <ms>` | `--events` を付けた `run` だけ。事象の書き込みがこの時間まったく進まなければ、読む側がいなくなったものとして扱う。`result` を書き終えるのを待つ時間の上限でもある（1000 以上 86400000 以下の整数。既定は 30000。[読む側が遅いとき](simulation-events.md#読む側が遅いとき読まないとき)） |
| `--stdin-commands` | `run` だけ。標準入力から命令を読む。標準入力の終わりも止める要求 |
| `--start-paused` | `--stdin-commands` か `--control` を付けた `run` だけ。全員がそろったら仮想時間 0 で止まり、`resume` か `step` の命令を待つ（`--control` があれば、時刻 0 のステップの前で止まる） |
| `--control <address>` | `run` だけ、Control Server を持つビルドだけ。Control API（gRPC）の Server を立てる。`<address>` はポート番号（127.0.0.1 で待ち受ける。0 なら空いているポート）、`<host>:<port>`、`unix:<path>`。ループバックでないアドレスには、TLS と認証情報が要る（[control-api.md](control-api.md) の「ランチャーにつなぐ」） |
| `--control-credentials <file>` | `--control` だけ。トークンと役割のファイル（1 行に `<role> <name> <token>`）。あれば、どの呼び出しにもトークンが要る |
| `--control-tls-cert <file>`、`--control-tls-key <file>` | `--control` だけ。サーバーの証明書の鎖と秘密鍵（PEM）。2 つ一緒に付けると TLS |
| `--control-audit-log <file>` | `--control` だけ。誰が何を変えたかを追記する |
| `--scenario <file>` | シナリオのファイル（[scenario.md](scenario.md)）。`check` では照合だけ、`run` では照合してから実行する。シナリオが終わるとシミュレーションを止め、手順が失敗していれば実行は失敗。`--start-paused` と一緒には使えない。`--control` と一緒なら、クライアントは見るだけ（[scenario.md](scenario.md) の「実行中に変数を見る」） |
| `--variant <name>` | 構成の機種を選ぶ（[機種](#機種variants--variant)）。機種を書いた構成には必ず要る。機種のない構成には付けられない。`test` では、回す機種を絞る（繰り返せる。なければ全部の機種） |
| `--param <name>=<value>` | `--scenario` と一緒にだけ。シナリオのパラメーター `${name}` の値（[シナリオのパラメーター](#シナリオのパラメーターparams--param)）。繰り返せる（同じ名前は 1 回だけ）。`test` では、その名前のパラメーターがあるシナリオにだけ効く（どのシナリオにもなければ引数の誤り） |
| `--scenarios <dir or files>` | `test` だけ。回すシナリオ。ディレクトリ（直下の `.yaml`、`.yml`、`.json` を名前の順に）かファイル（書いた順に）。後ろにいくつでも並べられ、繰り返せる。構成ファイルは `--scenarios` より前か、`--` の後に書く |
| `--fail-fast` | `test` だけ。最初に失敗した組の後は回さない |
| `--speed <speed>` | `run` だけ。構成ファイルの `speed` を上書きする（数か `max`。[等速で動かす](simulation-run.md#等速で動かす)） |
| `--strict` | 警告があっても失敗にする（注記では失敗にしない） |
| `--log-dir <dir>` | `run` だけ。participant ごとの出力のファイルを置くディレクトリ |
| `--timeout-ms <ms>` | `run` と `test`（1 組ごと）。シミュレーションの開始から終わりまでの実時間の上限（1 以上の整数。一時停止している時間も数える。`--control` と `--start-paused` では、時刻 0 のステップの前に止まった時点から。manual の必須の participant がいる実行では、participant を待ち始めた時点から）。超えたら中断して誤り。既定は上限なし |
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
| 1 | 誤りがある（`--strict` では警告も）。`run` では、照合の誤り（何も起動しない）か、実行の誤り（participant が失敗した、そろわなかった、止めても終わらなかったなど）。`test` では、失敗した組が 1 つ以上ある、中断されていないのに回さなかった組がある、`--strict` で回した組が 1 つもない |
| 3 | 構成ファイルを読めない（ファイルがない、文法の誤り、知らない形式の版など）。`run` で registry や controller を用意できないときも 3 です。結果を標準出力に書けないときと、`vehicle_sil_launch` 自身の想定外の失敗も 3 です（標準エラーに理由。`--json` では、そのコマンドの形の JSON に `code` が `internal` の誤り） |
| 4 | 引数の誤り（知らないコマンドや引数、`check` の `--events` と `--stdin-commands`、`run` の `--json` と `--events`、`--events` なしの `--events-stall-ms` と範囲の外の値、`--stdin-commands` も `--control` もない `--start-paused`、`--control` なしの `--control-credentials` などと Control Server を持たないビルドの `--control`、`--control` の値の誤り、読めない `--control-*` のファイル、認証情報が 1 件もない `--control-credentials` のファイル、片方だけの `--control-tls-cert` と `--control-tls-key`、TLS と認証情報のない、ループバックでない `--control`、数でも `max` でもない `--speed`、`--start-paused` と一緒の `--scenario`、`version` の `--json` 以外の引数、構成ファイルの名前がない、2 つ以上のファイル、同じ引数の 2 回目（`--disable` と `--enable` は繰り返せます）、値のない `--describe-timeout-ms`、`check` の `--log-dir`、`--timeout-ms`、`--speed`、`--start-paused`、`--control` と `--control-*`、構成ファイルにない participant 名の `--disable`、`--enable`、`--manual`、同じ名前の `--disable` と `--enable`、同じ名前の `--disable` と `--manual`、機種を書いた構成で `--variant` がない、機種のどれでもない名前の `--variant`、形の違う `--param`、`--scenario` なしの `--param`、シナリオにない名前の `--param`、`test` の `--scenarios` がない、`--scenarios` のパスがない、シナリオのファイルがないディレクトリ、`test` に付けた `run` だけの引数と `--scenario`、`test` に渡した構成の、有効で `manual: true` の participant）。誤りと使い方を標準エラーに書きます |
| 130 | `run` か `test` が中断された（Ctrl+C、`{"command":"stop"}`、標準入力の終わり、閉じた標準出力、Control Server のクライアントの `Stop` など） |

- Windows では、引数をワイド文字のコマンドラインから UTF-8 で読みます（ASCII 以外の文字を含むパスを使えます）。
