# シミュレーションの構成ファイルと照合（`vehicle_sil_launch check`）

各自が作った ECU の実行ファイル（[ecu-executable.md](ecu-executable.md)）を 1 つのシミュレーションにまとめるときに、まとめる人が書く構成ファイルと、その照合です。構成ファイルには、participant にする実行ファイル、それらがつながるネットワーク、止める時刻を書きます。`vehicle_sil_launch check` は、構成ファイルと、各実行ファイルの自己申告（`--describe`）を突き合わせ、食い違いを 1 回で全部出します。形式と照合の規則はまだ変わることがあります。

`check` はシミュレーションを始めず、registry にも接続しません。ただし、`describe: true` の実行ファイルは、1 つずつ `--describe` で起動します。`--describe` は ECU を組み立てるので、その実行ファイルのコード（ECU のコンストラクターなど）が動きます。信頼できない構成ファイルを `check` に渡さないでください。`describe: false` の実行ファイルは起動せず、あることだけを確かめます。

```sh
vehicle_sil_launch check simulation.yaml
```

`vehicle_sil_launch` は SIL Kit をサブモジュールからビルドしたときだけ作ります（YAML の読み込みに SIL Kit 付属の rapidyaml を使うため）。

## 構成ファイル

YAML で書きます。最初の空白でない文字が `{` なら JSON として読みます（内容は同じです）。文字コードは UTF-8 です（先頭の BOM は読み飛ばします。UTF-16 や NUL を含むファイルは読めません）。1 つのファイルに構成は 1 つです（`---` で続けた 2 つ目の文書は誤りです）。YAML のアンカーとエイリアス（`&name`、`*name`、`<<`）は使えません（ファイルを読めない誤りにします）。値は書き下してください。16 MiB を超えるファイルと、通常のファイルでないもの（デバイスやパイプ）は読みません。

例: [examples/ecu_main/simulation.yaml](../examples/ecu_main/simulation.yaml)（CMake が実行ファイルの横、`build/<preset>/examples/ecu_main/` に写します）。

<!-- examples/ecu_main/simulation.yaml -->
```yaml
# The two ECUs of this directory in one simulation. CMake copies this file next to the
# executables (build/<preset>/examples/ecu_main/); check it there with
#   vehicle_sil_launch check simulation.yaml
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
| `stop_at_ms` | | シミュレーションを止める仮想時間（ミリ秒、1 以上 10^12 以下の整数） |
| `start_timeout_ms` | | participant がそろうまで待つ時間（ミリ秒、1 以上 10^12 以下の整数。書かなければ 30000） |
| `networks` | はい | ネットワークの配列（空でもかまいません）。ECU が使うネットワークは、すべてここに書きます |
| `networks[].name` | はい | ネットワークの名前（ECU の `j1939()`、`can()`、`lin_master()`、`lin_slave()` に渡す名前） |
| `networks[].kind` | はい | `can` か `lin` |
| `participants` | はい | participant の配列（1 つ以上） |
| `participants[].name` | はい | participant 名。照合では、ecu_main の実行ファイルに `--name` で渡します。空でなく、1 行で（制御文字なし）、`-` で始まらないこと。`SimulationController`（vehicle-sil の controller の participant 名）は使えません |
| `participants[].command` | はい | 実行ファイルと、その引数の配列。引数がなければ文字列 1 つでもかまいません（空白で分けません） |
| `participants[].step_ms` | | 実行ファイルが申告するステップ幅（ミリ秒、1 以上 10^12 以下の整数）。書いたときだけ照合します |
| `participants[].describe` | | `true`（既定）: ecu_main の実行ファイル（照合します）。`false`: それ以外の実行ファイル（照合しません） |
| `participants[].required` | | `true`（既定）か `false`。シミュレーションがその participant を待つか |

- 知らないキーはエラーです（打ち間違いを黙って無視しません）。近い名前があれば示します。同じキーを 2 回書くのもエラーです。
- 整数は引用符なしで書きます（`"5000"` は文字列なのでエラーです）。真偽値は `true` か `false` です。
- `stop_at_ms`、`start_timeout_ms`、`required` はシミュレーションを動かすときの値です。`check` は値の形だけを確かめます。

### 実行ファイルのパス

- `command` の最初の要素が実行ファイルです。相対パスは構成ファイルのあるディレクトリから数えます（コマンドを実行したディレクトリからではありません）。`PATH` は探しません（見つからないとき、文言でそう伝えます）。パスは正規化しません（シンボリックリンクをまたぐ `..` は OS の解釈のまま）。
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
- 照合では、`command` の引数（`{{語}}` を `{語}` に戻したもの）に `--describe --name <participant 名>` を足して動かします。

### ほかの実行ファイル（`describe: false`）

- 照合しません。名前、ステップ幅、ネットワークを確かめられないことを注記します。`step_ms` を書いても照合しないので、注記します。

## 照合

`vehicle_sil_launch check <file>` は、次の順に確かめ、見つけたものを全部まとめて出します。見つけたものには 3 つの重さがあります。誤り（error）は照合を失敗にします。警告（warning）は `--strict` のときだけ失敗にします。注記（note）は失敗にしません（照合が確かめられなかったことを伝えます）。

1. 構成ファイルの文面: 形式の版、知らないキーと足りないキー、値の型と範囲、名前、`command` の引数と波括弧。誤りのある participant（名前がない・使えない、`command` が使えない、名前が重複している、`describe` が真偽値でない、`command` の引数の誤り）は、次の段に進めません。知らないキーや `step_ms` の誤りは、照合を止めません。
2. 実行ファイル: あること、ディレクトリでないこと、バッチファイルでないこと、（Linux では）実行の許可があること。
3. `describe: true` の各実行ファイルを `--describe --name <participant 名>` で 1 つずつ順に動かし（並列にしません）、標準出力の JSON を読みます。標準入力は空です。終了コードが 0 でない（シグナルや例外で終わったときは、そう書きます）、時間内に終わらない（`--describe-timeout-ms`、既定 10000 ms。過ぎたら止めます。出力を書き続ける実行ファイルも止めます）、JSON でない、形式の版が違う、は、その participant の誤りです。標準エラーの最後の 10 行を添えます。読むのは標準出力の最初の 1 MiB と標準エラーの最後の 1 MiB です。標準出力を切ったときは自己申告の誤りの添え書きで、標準エラーを切ったときはその末尾を添えるところで、そう書きます。
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
| `executable` | 誤り | 実行ファイルがない、ディレクトリ、バッチファイル、実行の許可がない |
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
error: two-engines.yaml:16: participant 'Dashboard': unknown key 'requried': did you mean 'required'? (the keys here: name, command, step_ms, describe, required)
warning: two-engines.yaml:6: network 'Body_LIN': no participant uses it: remove it, or check the network names in the ECUs

failed: 3 errors, 1 warning, 0 notes
```

### JSON（`--json`）

1 つの JSON のオブジェクトです。形は `--describe` の JSON と同じ決まりです（キーの順は固定、値がなくてもキーを省かない、字下げは空白 2 つ、改行は Windows でも LF）。

- 先頭の `vehicle_sil_check` は形式の版です。版が上がるのは、あるキーか `code` の意味が変わるとき、またはなくなるときです。キーと、`code`、`severity`、`result`、`participants[].status` の値は、版を上げずに増えることがあります。読む側は、知らない版を断り、知らないキーは無視し、知らない `code` はその `severity` で扱い、知らない `severity` は `"error"` として、知らない `result` は成功でないものとして（終了コードを見ます）、知らない `status` は照合できなかったものとして扱います。
- 値のないものは `null` です（空の文字列にしません）。配列は空の配列です。
- 構成ファイルを読めないとき（終了コード 3）も JSON を書きます（`result` が `"unreadable"`、`participants` と `networks` は空）。引数の誤り（終了コード 4）でも、`--json` があれば JSON を書きます（`result` が `"usage"`、`file` は `null`、`findings` に `code` が `usage` の誤り）。

| キー | 値 |
|---|---|
| `vehicle_sil_check` | 形式の版（今は 1） |
| `vehicle_sil` | `vehicle_sil_launch` の vehicle-sil の版 |
| `file` | 構成ファイル（引数に書いたとおり） |
| `result` | `"passed"`、`"failed"`、`"unreadable"`、`"usage"` |
| `strict` | `--strict` を付けたか |
| `errors`、`warnings`、`notes` | 誤り、警告、注記の数 |
| `stop_at_ms`、`start_timeout_ms` | 構成ファイルの値（読めないときは `null`。`start_timeout_ms` は既定の値も） |
| `participants[]` | 構成ファイルの順。`name`（読めなかったときは `null`）、`line`（行）、`command`、`describe`、`required`、`step_ms`（なければ `null`）、`status`、`reported` |
| `participants[].status` | `"described"`（自己申告を読んだ）、`"not_checked"`（`describe: false`）、`"failed"`（実行ファイルがない、`--describe` の失敗）、`"skipped"`（構成ファイルの誤りで進めなかった） |
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
  "participants": [
    {
      "name": "Engine",
      "line": 6,
      "command": [
        "engine/vehicle_sil_engine_ecu"
      ],
      "describe": true,
      "required": true,
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

## 引数と終了コード

```text
vehicle_sil_launch check [--json] [--strict] [--describe-timeout-ms <ms>] [--] <file>
```

| 引数 | 意味 |
|---|---|
| `--json` | 結果を JSON で書く |
| `--strict` | 警告があっても失敗にする（注記では失敗にしない） |
| `--describe-timeout-ms <ms>` | 1 つの実行ファイルの `--describe` を待つ時間（1 以上の整数。既定は 10000） |
| `--` | これより後はファイル名（`-` で始まる名前のとき） |
| `--help`、`-h` | 使い方を標準出力に書いて 0 で終わる（`--` より前のどこにあっても） |

| 終了コード | 意味 |
|---|---|
| 0 | 誤りがない（警告と注記だけなら 0。`--strict` では警告もない） |
| 1 | 誤りがある（`--strict` では警告も） |
| 3 | 構成ファイルを読めない（ファイルがない、文法の誤り、知らない形式の版など）。結果を標準出力に書けないときと、`vehicle_sil_launch` 自身の想定外の失敗も 3 です（標準エラーに理由） |
| 4 | 引数の誤り（知らないコマンドや引数、構成ファイルの名前がない、2 つ以上のファイル、同じ引数の 2 回目、値のない `--describe-timeout-ms`）。誤りと使い方を標準エラーに書きます |

- Windows では、引数をワイド文字のコマンドラインから UTF-8 で読みます（ASCII 以外の文字を含むパスを使えます）。
