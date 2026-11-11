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
| `registry_uri` | | `run` が registry を立てる場所（`silkit://<host>:<port>`、ポートは 1 以上 65535 以下）。書かなければ 127.0.0.1 の空いているポートです。ほかのコンピューターの participant も参加させるときは `silkit://0.0.0.0:8500` のように書きます（このコンピューターの participant には `127.0.0.1` の URI を渡します） |
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

- 知らないキーはエラーです（打ち間違いを黙って無視しません）。近い名前があれば示します。同じキーを 2 回書くのもエラーです。
- 整数は引用符なしで書きます（`"5000"` は文字列なのでエラーです）。真偽値は `true` か `false` です。
- `stop_at_ms`、`start_timeout_ms`、`stop_timeout_ms`、`registry_uri` はシミュレーションを動かすときの値です。`check` は値の形だけを確かめます。

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
- 照合では、`command` の引数（`{{語}}` を `{語}` に戻したもの）に `--describe --name <participant 名>` を足して動かします。`run` では `--name <participant 名> --registry-uri <URI> --start-timeout-ms <ms>` を足します。`<ms>` は `start_timeout_ms + stop_timeout_ms + 10000` です（開始を待つのはランチャーで、来なかった participant を挙げるのもランチャーです。ECU の制限は、ランチャーがいなくなったときのためのものです）。
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
| `value` | 誤り | 型または範囲が違う値、使えない名前、実行ファイルのパスのプレースホルダー |
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
| `version-mismatch` | 注記 | 実行ファイルをビルドした vehicle-sil の版が、`vehicle_sil_launch` の版と違う |
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
error: two-engines.yaml:16: participant 'Dashboard': unknown key 'requried': did you mean 'required'? (the keys here: name, command, step_ms, describe, required, enabled)
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
| `participants[]` | 構成ファイルの順。`name`（読めなかったときは `null`）、`line`（行）、`command`、`describe`、`required`、`enabled`（`--disable` と `--enable` を当てた後の値）、`step_ms`（なければ `null`）、`status`、`reported` |
| `participants[].status` | `"described"`（自己申告を読んだ）、`"not_checked"`（`describe: false`）、`"failed"`（実行ファイルがない、`--describe` の失敗）、`"skipped"`（構成ファイルの誤りで進めなかった）、`"disabled"`（外した。項目に誤りがあっても） |
| `participants[].reported` | 自己申告（`described` のときだけ。ほかは `null`）: `vehicle_sil`、`name`、`step_ns`、`networks`（`--describe` の JSON と同じ形。変数は含みません） |
| `networks[]` | 構成ファイルの順。`name`、`kind`（読めなかったときは `null`）、`line`、`participants`（使う participant 名。構成ファイルの順） |
| `findings[]` | 人向けと同じ順。`severity`（`"error"`、`"warning"`、`"note"`）、`code`、`file`、`line`（なければ `null`）、`participant`、`network`（関係するもの。なければ `null`）、`message`（1 行）、`detail`（添える文。なければ `null`） |

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
2. registry をこのプロセスの中で立てます（`registry_uri`、なければ 127.0.0.1 の空いているポート）。標準エラーの最初の行が、その URI です（`[vehicle_sil_launch] registry at silkit://127.0.0.1:<port>`）。
3. controller（participant 名 `SimulationController`）をこのプロセスの中で動かします。シミュレーションが待つ（必須の）participant は、外していない `required: true` のものです。
4. 外していない participant を構成ファイルの順に、それぞれ別のプロセスで起動します。作業ディレクトリは構成ファイルのあるディレクトリ、標準入力は空です。
5. 必須の participant がそろったら始めます。`stop_at_ms` があれば、仮想時間がそこに達したところで止めます。なければ、participant が止めるか、Ctrl+C まで動きます。
6. 終わったら、すべての participant を終わらせ（下の「止め方」）、結果を書きます。

### 見張ること

- 始まる前: 必須の participant のプロセスが終わったら、`start_timeout_ms` を待たずにやめます（`ended-before-start`）。`start_timeout_ms` の間に参加しなかった必須の participant は、全部を挙げます（`not-joined`）。
- 実行中: 必須の participant のプロセスが 0 でない終了コードで（またはシグナルで）終わったら、すぐにシミュレーションを中断します。controller から見て、必須の participant がシミュレーションを去ったときも中断します（Error の状態ならすぐに、ほかの状態なら 5 秒たってもシミュレーションが終わらないとき）。この規則は `vehicle_sil_sim_controller` と `vehicle_sil::Simulation` と同じです。中断の理由になった participant は `participant-failed` です。中断されても、ほかの ECU は自分で終わります（終了コード 0）。
- `required: false` の participant は待たず、終わってもシミュレーションを止めません。0 でない終了コードは警告です。
- `--timeout-ms <ms>` を付けると、シミュレーションが始まってから `<ms>` の実時間で終わらなければ中断し、誤り（`timeout`）にします。ステップの中で止まった ECU がいると仮想時間が進まず、`stop_at_ms` があっても終わらないためです（CI などで使います）。既定は上限なしです。
- `stop_at_ms` を書いたのに、participant がそれより前にシミュレーションを止めたときは、注記（`ended-before-stop-at`）を出します（`--strict` でも失敗にしません）。

### 止め方

シミュレーションが終わったら（中断と Ctrl+C のときも）、次の順に participant を終わらせます。

1. 必須の participant が自分で終わるのを、`stop_timeout_ms` まで待ちます（ecu_main の実行ファイルは、シミュレーションが終われば自分で終わります）。
2. まだ動いている participant に、止まるよう求めます（Linux: その participant のプロセスグループに SIGTERM。Windows: そのプロセスグループに Ctrl+Break）。`required: false` の participant は、ここで止めます（警告にしません）。必須の participant は誤りです（`did-not-end`）。
3. さらに `stop_timeout_ms` まで、止まるよう求めた participant と、それが起動したプロセス（プロセスグループ、Windows ではジョブオブジェクト）が終わるのを待ちます。残ったものを、それが起動したプロセスごと強制終了します（Linux: プロセスグループに SIGKILL。Windows: ジョブオブジェクトの終了）。
4. 自分で終わった participant でも、それが起動したプロセスが残っていれば終わらせ、警告にします（`left-processes`）。

シミュレーションが始まらなかったときは、1 を飛ばして 2 からです（その participant の誤りにはしません）。ecu_main の実行ファイルは、始まる前に止まるよう求められると、すぐに終わります（[ecu-executable.md](ecu-executable.md)）。

各 participant は、自分のプロセスグループ（Windows ではプロセスグループとジョブオブジェクト）で動きます。端末の Ctrl+C は `vehicle_sil_launch` にだけ届き、participant には届きません。Windows で `vehicle_sil_launch` にコンソールがない（participant と共有するコンソールがない）ときは、Ctrl+Break を送れないので、2 の代わりにすぐ強制終了します（文言にそう書きます）。このとき participant は、コンソールの窓を開かないように起動します。

### 中断（Ctrl+C）

Ctrl+C（SIGINT）、SIGTERM、SIGHUP（Windows: Ctrl+C、Ctrl+Break、コンソールを閉じる）を受けると、`vehicle_sil_launch` はシミュレーションを順に止め（controller の stop）、上の止め方で participant を終わらせ、結果に中断を書いて、終了コード 130 で終わります。始まる前なら、participant を止めます。シミュレーションが終わった後、participant が自分で終わるのを待っている間の中断は、その待ちをやめて、止まるよう求めます（止め方の 2）。2 回目の中断では、待つのをやめて、残っている participant をすぐに強制終了します。照合の間の中断は、照合が終わってから効きます（何も起動しません）。`vehicle_sil_launch` を起動した側が無視するように設定したシグナル（`nohup` の SIGHUP など）は、無視したままです。端末の Ctrl+Z（SIGTSTP）は `vehicle_sil_launch` だけを止めます。participant は別のプロセスグループなので動き続け、registry と controller だけが止まります。

Windows でコンソールを閉じると、Windows は数秒でプロセスを終わらせます。participant はジョブオブジェクトが終わらせますが、結果を書けないことがあります。

### participant を残さないこと

- `vehicle_sil_launch` は、終わる前に、起動したすべての participant を終わらせます。
- participant は、標準入力（空）、標準出力、標準エラー（`vehicle_sil_launch` が読むパイプ）のほかに、`vehicle_sil_launch` のファイルやハンドルを受け継ぎません。
- Windows: participant はそれぞれジョブオブジェクト（最後のハンドルが閉じたら、中のプロセスを終わらせる）に入ります。`vehicle_sil_launch` がどう終わっても（強制終了を含む）、participant とそれが起動したプロセスは終わります。
- Linux: participant は、`vehicle_sil_launch` が終わると SIGKILL を受けます（親が終わったときのシグナル）。ただし、`vehicle_sil_launch` が強制終了されたとき（SIGKILL、クラッシュ）は、止め方の 3 と 4 が動かないので、participant が起動したプロセスは残ることがあります。participant のプロセスグループを抜けたプロセス（`setsid()` などを呼んだもの）は、どの場合も終わらせられません。
- Linux 以外の POSIX は対象にしていません（親が終わったときのシグナルがないので、`vehicle_sil_launch` が強制終了されると participant が残ります）。

### 出力

- participant の標準出力と標準エラーは、行ごとに `[<participant 名>] ` を付けて、`vehicle_sil_launch` の標準エラーに流します。`vehicle_sil_launch` 自身の経過は `[vehicle_sil_launch] ` で始まります（registry の URI、起動したコマンド、始まった、終わった、止めた）。4096 バイトを超える行は、UTF-8 の文字の間で分けて流します。制御文字（タブを除く C0、DEL、U+0080 から U+009F の C1）と、UTF-8 でないバイトは `?` にします。participant は UTF-8 で書いてください（Windows のコンソールにも UTF-8 として表示するので、ほかのコードページの文字は化けます）。標準エラーを読む側が止まる（読まれないパイプ）と、`vehicle_sil_launch` も書くところで止まります。
- `--log-dir <dir>` を付けると、participant ごとに `<dir>/<participant 名>.log` にも書きます（`[名前]` なし。名前の ASCII の英数字、`.`、`-`、`_` 以外のバイトは `_` にし、Windows の装置の名前（`NUL`、`CON`、`PRN`、`AUX`、`COM1`〜`COM9`、`LPT1`〜`LPT9`。`.` の前も）には、どの OS でも後ろに `_` を付けます。大文字と小文字だけが違う名前は重なりとみなし、`-2` などを付けます）。ディレクトリがなければ作ります。前の実行のログは、同じ名前のものだけを書き直します（今の構成にない participant のログは残ります）。JSON の `participants[].log` が、どのファイルかを示します。
- 標準出力には結果だけを書きます（人向けか `--json`）。同じ機械の上の同じ結果からは同じ出力です（時刻、所要時間、プロセス ID、registry の URI を含みません）。改行はどの OS でも LF です。Windows のコンソールに書くときは、`check` も `run` も、コンソールのコードページによらず UTF-8 の文字として表示します。

人向けの結果は、照合の表と照合で見つけたもの、`run:` で始まるシミュレーションの 1 行、participant の表（必須か、どう終わったか）、実行で見つけたもの、最後に結果の 1 行（`passed:`、`failed:`、`interrupted:` と、照合と実行を合わせた誤り、警告、注記の数）です。照合に誤りがあるときは、照合の表と見つけたものの後に `nothing was started:` の行を書きます。

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

participant の表の `ended` の列:

| 表示 | 意味 |
|---|---|
| `exit code <n>` | 自分で終わった（Windows の NTSTATUS の値は 16 進で、例: `0xC0000005`） |
| `signal <名前>` | シグナルで終わった（Linux。例: `signal SIGSEGV`） |
| `stopped (SIGTERM), exit code <n>` | 止まるよう求めた後に終わった（Windows では `Ctrl+Break`） |
| `killed` | 強制終了した |
| `not started` | 起動できなかった（またはその前に実行をやめた） |
| `not started (disabled)` | 外した participant（`required` の列は `-`） |

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
| `timeout` | 誤り | シミュレーションが `--timeout-ms` の実時間で終わらなかったので、中断した |
| `simulation` | 誤り | シミュレーションが正常に終わらなかった（理由の participant がわからない） |
| `ended-before-stop-at` | 注記 | `stop_at_ms` より前に、participant がシミュレーションを止めた |
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
| `simulation` | `"reached_stop_at"`（`stop_at_ms` まで動いた）、`"ended"`（participant が止めた）、`"aborted"`（中断した。`--timeout-ms` のときも）、`"interrupted"`、`"not_started"`。何も起動しなかったときは `null` |
| `interrupted_by` | 中断のシグナルかキー（`"SIGINT"`、`"Ctrl+C"` など）。中断でなければ `null` |
| `participants[]` | 構成ファイルの順（外した participant も）。`name`、`required`、`enabled`（`false` なら外した participant で、`ended` は `"not_started"`、`log` は `null`。シミュレーションが待つのは `required` と `enabled` がどちらも `true` のもの）、`ended`（`"exited"`、`"stopped"`、`"killed"`、`"not_started"`）、`exit_code`（シグナルで終わったとき、強制終了したとき、起動しなかったときは `null`）、`signal`（`"SIGSEGV"` など。なければ `null`）、`left_processes`（起動したプロセスを残したか）、`log`（`--log-dir` のファイル。なければ `null`） |
| `findings[]` | 実行で見つけたもの（照合の `findings[]` と同じ形） |
| `check` | 照合の結果（`check --json` と同じオブジェクト） |

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
  "participants": [
    {
      "name": "Engine",
      "required": true,
      "enabled": true,
      "ended": "exited",
      "exit_code": 0,
      "signal": null,
      "left_processes": false,
      "log": null
    },
    {
      "name": "Dashboard",
      "required": true,
      "enabled": true,
      "ended": "exited",
      "exit_code": 0,
      "signal": null,
      "left_processes": false,
      "log": null
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

この節の例も、CTest の `launch_run.docs` が実際の実行と照合します（`vehicle_sil` の版を除いて、バイト単位で）。

## 引数と終了コード

```text
vehicle_sil_launch check [--json] [--strict] [--describe-timeout-ms <ms>] [--disable <name>]... [--enable <name>]... [--] <file>
vehicle_sil_launch run [--json] [--strict] [--log-dir <dir>] [--timeout-ms <ms>] [--describe-timeout-ms <ms>]
                       [--disable <name>]... [--enable <name>]... [--] <file>
```

| 引数 | 意味 |
|---|---|
| `--json` | 結果を JSON で書く |
| `--strict` | 警告があっても失敗にする（注記では失敗にしない） |
| `--log-dir <dir>` | `run` だけ。participant ごとの出力のファイルを置くディレクトリ |
| `--timeout-ms <ms>` | `run` だけ。シミュレーションの開始から終わりまでの実時間の上限（1 以上の整数）。超えたら中断して誤り。既定は上限なし |
| `--describe-timeout-ms <ms>` | 1 つの実行ファイルの `--describe` を待つ時間（1 以上の整数。既定は 10000） |
| `--disable <name>` | participant `<name>` を外す（`enabled: false` と同じ）。繰り返せる |
| `--enable <name>` | participant `<name>` を戻す（`enabled: true` と同じ）。繰り返せる |
| `--` | これより後はファイル名（`-` で始まる名前のとき） |
| `--help`、`-h` | 使い方を標準出力に書いて 0 で終わる（`--` より前のどこにあっても） |

| 終了コード | 意味 |
|---|---|
| 0 | 誤りがない（警告と注記だけなら 0。`--strict` では警告もない）。`run` では、シミュレーションが正常に終わり、必須の participant がどれも 0 で終わった |
| 1 | 誤りがある（`--strict` では警告も）。`run` では、照合の誤り（何も起動しない）か、実行の誤り（participant が失敗した、そろわなかった、止めても終わらなかったなど） |
| 3 | 構成ファイルを読めない（ファイルがない、文法の誤り、知らない形式の版など）。`run` で registry や controller を用意できないときも 3 です。結果を標準出力に書けないときと、`vehicle_sil_launch` 自身の想定外の失敗も 3 です（標準エラーに理由。`--json` では、そのコマンドの形の JSON に `code` が `internal` の誤り） |
| 4 | 引数の誤り（知らないコマンドや引数、構成ファイルの名前がない、2 つ以上のファイル、同じ引数の 2 回目（`--disable` と `--enable` は繰り返せます）、値のない `--describe-timeout-ms`、`check` の `--log-dir` と `--timeout-ms`、構成ファイルにない participant 名の `--disable` と `--enable`、同じ名前の `--disable` と `--enable`）。誤りと使い方を標準エラーに書きます |
| 130 | `run` が中断された（Ctrl+C など） |

- Windows では、引数をワイド文字のコマンドラインから UTF-8 で読みます（ASCII 以外の文字を含むパスを使えます）。
