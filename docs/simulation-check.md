# 照合と所見（`vehicle_sil_launch check`）

まとめる人向けです。先に [simulation-config.md](simulation-config.md)（構成ファイル）を読みます。次は [simulation-run.md](simulation-run.md)（実行）です。引数と終了コードは [simulation-config.md](simulation-config.md) の「引数と終了コード」にあります。

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
| `registry-on-network` | 注記 | `registry_uri` がループバックのアドレスでない（registry と participant がネットワークで待ち受け、このコンピューターに届く誰でもシミュレーションに参加できる。[待ち受けるアドレス](simulation-run.md#待ち受けるアドレス)） |
| `listen-variable-ignored` | 注記 | `vehicle_sil_launch` 自身の環境の `VEHICLE_SIL_LISTEN_ON_NETWORK` が、`run` が participant に渡す値と違う（その値は使わない。[待ち受けるアドレス](simulation-run.md#待ち受けるアドレス)） |
| `step-not-checked` | 注記 | `describe: false` の participant に `step_ms` がある |
| `executable` | 誤り | 実行ファイルがない（区切りのない名前は `PATH` にもない）、ディレクトリ、バッチファイル、実行の許可がない |
| `found-on-path` | 注記 | 実行ファイルを `PATH` で見つけた（その場所を示します） |
| `not-required-ecu` | 誤り | `describe: true` の participant に `required: false` がある |
| `no-required-participant` | 誤り | 外していない participant がどれも `required: false`、またはすべてを外した（シミュレーションが待つ participant がない） |
| `describe-start` | 誤り | 実行ファイルを起動できない |
| `describe-timeout` | 誤り | `--describe` が時間内に終わらない |
| `describe-failed` | 誤り | `--describe` の終了コードが 0 でない（4 は、ecu_main でない実行ファイルの見込みが高い） |
| `describe-output` | 誤り | `--describe` の出力が自己申告の JSON でない |
| `describe-version` | 誤り | 自己申告の形式の版（`vehicle_sil_ecu`）を読めない（文は、ECU の vehicle-sil の版（申告があれば）と `vehicle_sil_launch` の版、新しくするのがどちらかを言う） |
| `describe-unknown-value` | 誤り | 自己申告の、決まった値をとるキー（`kind`、`use`、`type`、`causality`）に、この `vehicle_sil_launch` が知らない値がある（新しい vehicle-sil の ECU。文は、キーと値、知っている値、両方の vehicle-sil の版を言う）。その participant だけの誤りで、ほかは照合を続ける |
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
| `coarse-time-step` | 警告 | ファイルに `speed`（数）と `time_step_ms` があり、ECU のステップが刻みの境目で始まらない（`run` の所見と同じ文言。[等速で動かす](simulation-run.md#等速で動かす)の「刻みと ECU のステップ」） |
| `control-link-not-offered` | 注記 | 変数のある ECU が、それをシミュレーションへ出すと申告していない（`--no-control`、申告のない古い実行ファイル）。`run` はその変数を読み書きできない（[変数を読み書きする口](simulation-run.md#変数を読み書きする口)） |
| `control-link-version` | 警告 | 変数のある ECU が申告した、メッセージの形式の版（か運び手）が、`vehicle_sil_launch` と違う。`run` はその変数を読み書きできない |
| `variable-map-unknown` | 誤り | `variables` の対応の `from` の変数が、実行ファイルにない（近い名前か、実行ファイルの変数の一覧を書きます。[変数の名前と値を合わせる](simulation-config.md#変数の名前と値を合わせるvariables)） |
| `variable-map-type` | 誤り | 対応の形が変数の型に合わない（`scale`、`offset` は整数か実数、`map` は整数か列挙の変数にだけ書ける） |
| `variable-map-value` | 誤り | `map` の値を、実行ファイルの変数が取れない（列挙の項目にない、範囲の外） |
| `variable-map-collision` | 誤り | 対応の名前が、実行ファイルのほかの変数の見える名前と同じ。または、同じ変数に対応を 2 つ書いた |
| `variable-map-absent` | 誤り | `absent: true` と書いた名前の変数が、実行ファイルにある |
| `variable-map-not-checked` | 注記 | `describe: false` の participant に `variables` がある（実行の前には比べられない。合わない対応は、実行の所見 `variable-map-not-applied` になる） |
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
error: two-engines.yaml:16: participant 'Dashboard': unknown key 'requried': did you mean 'required'? (the keys here: name, command, step_ms, describe, required, enabled, manual, variables)
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
| `participants[]` | 構成ファイルの順。`name`（読めなかったときは `null`）、`line`（行）、`command`、`describe`、`required`、`enabled`（`--disable` と `--enable` を当てた後の値）、`manual`（`--manual` を当てた後の値）、`step_ms`（なければ `null`）、`status`、`reported`、`variables`、`variable_map`（`variables` を書いた participant だけ）、`control` |
| `participants[].status` | `"described"`（自己申告を読んだ）、`"not_checked"`（`describe: false`）、`"failed"`（実行ファイルがない、`--describe` の失敗）、`"skipped"`（構成ファイルの誤りで進めなかった）、`"disabled"`（外した。項目に誤りがあっても） |
| `participants[].reported` | 自己申告（`described` のときだけ。ほかは `null`）: `vehicle_sil`、`name`、`step_ns`、`networks`（`--describe` の JSON と同じ形。変数は次の `variables`） |
| `participants[].variables` | シミュレーションから見える ECU の変数の配列（`ecu.connected`、続いて宣言の順）。構成ファイルの `variables`（対応表）を当てた後の名前、型、範囲、単位です（シナリオと Control API が見るものと同じ）。対応表がなければ、ECU が申告したままです。要素の形は `--describe` の JSON の `variables[]` と同じです（`name`、`type`、`causality`、`unit`、`min`、`max`、`step`、`description`、`items`、`builtin`。[ecu-executable.md](ecu-executable.md) の「JSON の形式」）。空の配列は「変数が 0 個」です。`null` は「わからない」です: 自己申告のない participant（`describe: false`）、外した participant、自己申告を読めなかった participant、`variables` を書かない古い vehicle-sil の ECU。実行の前に、どの ECU にどの変数があるかを見るのに使います |
| `participants[].variable_map` | 構成ファイルの `variables` に書いた対応の配列（書いた順）。`variables` のない participant には、このキーがありません。要素は `name`（シミュレーションで使う名前）、`line`（行）、`from`（実行ファイルの変数名。`absent` なら `null`）、`absent`、`scale` と `offset`（一次式の形でなければ両方 `null`。片方だけ書いたときは、もう片方に既定の 1 か 0 が入ります）、`unit`（書いていなければ `null`）、`map`（`name` と `value` の配列。なければ `null`）。読めなかった対応（誤り `value` など）は入りません |
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
