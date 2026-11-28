# シミュレーションの設定ファイル（`vehicle_sil_launch`）

[English](../simulation-config.md)

`vehicle_sil_launch` に渡す設定ファイルのリファレンスです。設定ファイルのキーと書き方のルール、`check`、`run`、`test`、`version` の引数と終了コードを載せています。形式とルールはまだ変わることがあります。

設定ファイルには、participant にする実行ファイル、それらが接続するネットワークを書きます。別々に作った ECU の実行ファイル（[ecu-executable.md](ecu-executable.md)）を、1 つのシミュレーションで動かすために使います。

```sh
vehicle_sil_launch check simulation.yaml
vehicle_sil_launch run simulation.yaml --stop-at-ms 1000
```

| 調べること | 節 |
|---|---|
| `check` と `run` が、設定ファイルから何を起動するか | [`check` と `run` が起動するもの](#check-と-run-が起動するもの) |
| キーの一覧と既定の値 | [キー](#キー) |
| `command` の書き方 | [実行ファイルのパス](#実行ファイルのパス)、[波括弧とプレースホルダー](#波括弧とプレースホルダー) |
| ECU を 1 つ除いて動かす | [participant を無効にする](#participant-を無効にするenabled--disable--enable) |
| ECU をデバッガーから動かす | [participant を自分で起動する](#participant-を自分で起動するmanual--manual) |
| 実行ファイルごとに違う変数の名前をそろえる | [変数の名前と値を合わせる](#変数の名前と値を合わせるvariables) |
| 実行ファイルごとに違うネットワークの名前をそろえる | [ネットワークの名前を合わせる](#ネットワークの名前を合わせるnetwork_names) |
| 同じ ECU を 1 つのネットワークに 2 つ載せる（J1939 のアドレスと NAME を替える） | [J1939 のアドレスと NAME を合わせる](#j1939-のアドレスと-name-を合わせるj1939_nodes) |
| 機種ごとに実行ファイルや値を変える | [機種](#機種variants--variant)、[シナリオのパラメーター](#シナリオのパラメーターparams--param) |
| 引数と終了コード | [引数と終了コード](#引数と終了コード) |

## `check` と `run` が起動するもの

| コマンド | すること |
|---|---|
| `vehicle_sil_launch check` | 設定ファイルと、各実行ファイルの `--describe` の出力を照合し、不整合を 1 回で全部表示します（[simulation-check.md](simulation-check.md)）。 |
| `vehicle_sil_launch run` | 同じ照合をしてから、シミュレーション全体を起動し、終わりまで監視して、各 participant がどう終わったかを表示します（[simulation-run.md](simulation-run.md)）。 |

`check` はシミュレーションを始めず、registry にも接続しません。ただし、`describe: true` の実行ファイルは `--describe` で起動します。いくつかを同時に起動します（数に上限があります）が、出力はファイルの参加者の順のままです。`--describe` は ECU を組み立てるので、その実行ファイルのコード（ECU のコンストラクターなど）が動きます。`describe: false` の実行ファイルは起動せず、あることだけを確認します。

`run` は、照合に通ったときだけ、すべての participant を起動します。

> [!WARNING]
> 設定ファイルは、実行するコマンドの一覧です。信頼できない設定ファイルを、`check` にも `run` にも渡さないでください。

`vehicle_sil_launch` は、SIL Kit をサブモジュールからビルドしたときだけビルドされます（YAML の読み込みに SIL Kit 付属の rapidyaml を使うため）。

## 設定ファイル

### ファイルの形式

| 項目 | ルール |
|---|---|
| 書式 | YAML です。 |
| JSON | 最初の空白でない文字が `{` なら、JSON として読みます（内容は同じです）。 |
| 文字コード | UTF-8 です（先頭の BOM は読み飛ばします）。 |
| 読めない文字コード | UTF-16 のファイルと、NUL を含むファイルは読めません。 |
| 1 つのファイルの設定の数 | 1 つです（`---` で続けた 2 つ目の文書はエラーです）。 |
| アンカーとエイリアス（`&name`、`*name`、`<<`） | 使えないので、値は書き下してください（ファイルを読めないエラーにします）。 |
| 大きさ | 16 MiB を超えるファイルは読みません。 |
| ファイルの種類 | 通常のファイルでないもの（デバイスやパイプ）は読みません。例外は `check --text-from-stdin` で、文を標準入力から、この表のほかのルールで読みます（[引数](#引数)）。 |

例: [examples/ecu_main/simulation.yaml](../../examples/ecu_main/simulation.yaml)（CMake が実行ファイルの横、`build/<preset>/examples/ecu_main/` にコピーします）。

<!-- examples/ecu_main/simulation.yaml -->
```yaml
# yaml-language-server: $schema=../../schema/simulation.schema.json
# The two ECUs of this directory in one simulation. CMake copies this file next to the executables
# (build/<preset>/examples/ecu_main/); there, vehicle_sil_launch check simulation.yaml checks it and
# vehicle_sil_launch run simulation.yaml runs it.
vehicle_sil_simulation: 2

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

### キー

全体のキーは次のとおりです。

| キー | 必須 | 値 | 書かないとき |
|---|---|---|---|
| `vehicle_sil_simulation` | はい | 形式のバージョン（今は `1`） | |
| `registry_uri` | | `run` が registry を立てる場所（`silkit://<host>:<port>`、ポートは 0 以上 65535 以下。0 は空いているポート） | 127.0.0.1 の空いているポート |
| `time_step_ms` | | controller のタイムステップ（ミリ秒の仮想時間、1 以上 10^12 以下の整数） | 自動決定 |
| `networks` | はい | ネットワークの配列（空でもかまいません） | |
| `participants` | はい | participant の配列（1 つ以上） | |
| `variants` | | 機種の対応（機種の名前 → 共通の部分との差分。[機種](#機種variants--variant)） | 機種のない設定ファイルです |
| `params` | | シナリオのパラメーターの値（名前 → 値。[シナリオのパラメーター](#シナリオのパラメーターparams--param)） | |

`networks` の要素のキーは次のとおりです。ECU が使うネットワークは、すべて `networks` に書きます。

| キー | 必須 | 値 |
|---|---|---|
| `networks[].name` | はい | ネットワークの名前（ECU の `j1939()`、`can()`、`lin_master()`、`lin_slave()`、`ethernet()` に渡す名前。実行ファイルが別の名前で呼んでいるときは、その participant の `network_names` に書きます） |
| `networks[].kind` | はい | `can`、`lin`、`ethernet` のどれか |
| `networks[].external` | | ネットワークを外の実機のバスにつないだときのふるまい（`can` は `bitrate`、`ethernet` は `ethertypes`、`all_frames`）。[ネットワークを外につなぐ](#ネットワークを外につなぐexternal--connect) |

`participants` の要素のキーは次のとおりです。

| キー | 必須 | 値 | 既定 |
|---|---|---|---|
| `participants[].name` | はい | participant 名 | |
| `participants[].command` | はい（`manual: true` かつ `describe: false` のときは省略可） | 実行ファイルと、その引数の配列 | |
| `participants[].step_ms` | | 実行ファイルが `--describe` に出力するステップ幅（ミリ秒、1 以上 10^12 以下の整数） | 照合しません |
| `participants[].describe` | | `true` は ecu_main の実行ファイル（照合します）、`false` はそれ以外の実行ファイル（照合しません） | `true` |
| `participants[].required` | | シミュレーションがその participant を待つか（`true` か `false`） | `true` |
| `participants[].enabled` | | `false` の participant は無効にします（照合も起動もしません。[participant を無効にする](#participant-を無効にするenabled--disable--enable)） | `true` |
| `participants[].manual` | | `true` の participant は、`run` が起動せず、自分で起動します（[participant を自分で起動する](#participant-を自分で起動するmanual--manual)） | `false` |
| `participants[].role` | | participant の用途（`ecu`、`test`、`bridge`。[テスト用の participant とブリッジ](#テスト用の-participant-とブリッジrole)） | `ecu` |
| `participants[].variables` | | 変数の対応表（[変数の名前と値を合わせる](#変数の名前と値を合わせるvariables)） | 変数は、実行ファイルの名前のまま見えます |
| `participants[].network_names` | | ネットワークの名前の対応。シミュレーションの名前（`networks` の名前）から、この実行ファイルがコードで使っている名前への対応を書きます（[ネットワークの名前を合わせる](#ネットワークの名前を合わせるnetwork_names)） | ネットワークは、実行ファイルの名前のままです |
| `participants[].j1939_nodes` | | この実行ファイルの J1939 のノードの、別のアドレスと NAME の欄。ノードごとに 1 行です（[J1939 のアドレスと NAME を合わせる](#j1939-のアドレスと-name-を合わせるj1939_nodes)） | ノードは、実行ファイルのコードの値のままです |

キーごとの補足です。

**`registry_uri`**

| 場合 | registry の場所と、起きること |
|---|---|
| 書かない | 127.0.0.1 の空いているポートです。 |
| 書かず、自分で起動する必須の participant がいる | `silkit://127.0.0.1:8500` です（[participant を自分で起動する](#participant-を自分で起動するmanual--manual)）。 |
| ポート 0（`silkit://127.0.0.1:0`、`silkit://0.0.0.0:0`） | 実行を始めるときに選ぶ、空いているポートです。`registry at` と `join at` の行、participant のコマンド（自分で起動する participant に出すものも）、`serve [--once]` のイベント `registry`、`run --json` の結果の `registry_uri` は、そのポートです。照合の JSON の `registry_uri` は、書いたとおりです。ポートは実行ごとに変わるので、自分で起動する participant（manual）やほかの PC の participant のコマンドも、実行ごとに変わります。使い回すデバッガーの設定や、ほかの PC には、決まったポートを書きます。 |
| ほかの PC の participant も参加させる | `silkit://0.0.0.0:8500` のように書きます（この PC の participant には `127.0.0.1` の URI を渡します）。 |
| ループバックのアドレス（`127.x.x.x`、`[::1]`、`localhost`）でない | 注記 `registry-on-network` を表示し、`run` が起動する participant に環境変数 `VEHICLE_SIL_LISTEN_ON_NETWORK=1` を渡します（[待ち受けるアドレス](simulation-run.md#待ち受けるアドレス)）。 |

ほかの PC から参加させる手順は、[howto/multi-pc.md](howto/multi-pc.md) にあります。

**`time_step_ms`** は、コントローラーの仮想時間の刻みです。省略すると、ランチャーが稼働する ECU の周期、10 ms、Run の停止時刻（指定時）の最大公約数から求めます。固定値を使う場合は、Run の停止時刻がその倍数でなければ `check` と `run` が `stop-not-on-step` を報告します。

形式 1 から移行する場合は、`vehicle_sil_simulation: 2` に変更し、`stop_at_ms`、`speed`、`late_tolerance_ms` を Run の引数か要求ファイルに移します。`start_timeout_ms` と `stop_timeout_ms` はホストの起動引数 `--participant-start-timeout-ms`、`--participant-stop-timeout-ms` に移します。

**`participants[].name`**: 空でなく、1 行で（制御文字なし）、`-` で始まらない名前にします。`SimulationController`（vehicle-sil の controller の participant 名）は使えません。照合では、ecu_main の実行ファイルに `--name` で渡します。

**`participants[].command`**: 引数がなければ、文字列 1 つでもかまいません（空白で分けません）。`manual: true` かつ `describe: false` のときだけ `command` を省略できます（ほかの PC で起動する外部 SIL Kit participant など）。空の `command: []` や `command: ""` は引き続きエラーです。

**`participants[].step_ms`**: 書いたときだけ照合します。

**`participants[].required`**: `false` は、`describe: false` の participant にだけ書けます。有効な participant のうち、少なくとも 1 つは `true` の participant が必要です（エラー `no-required-participant`）。

**`participants[].role`**: 照合だけが読み、実行の動きは変わりません。

**`variants`**: 機種を書いた設定ファイルは、`check` と `run` に `--variant <名前>` が必要です。

**`params`**: 値は、シナリオの `${name}` に反映されます。

### 書き方のルール

| 項目 | ルール |
|---|---|
| 知らないキー | エラーで、近い名前があれば表示します（打ち間違いを黙って無視しません）。 |
| 同じキーの 2 回目 | エラーです。 |
| 整数 | 引用符なしで書きます（`"5000"` は文字列なのでエラーです）。 |
| 真偽値 | 引用符なしの `true` か `false` です（`True`、`TRUE`、`False`、`FALSE` も読みます）。 |
| `yes`、`on` など | 真偽値としてはエラーです。 |

### エディターの補完

JSON Schema は [schema/simulation.schema.json](../../schema/simulation.schema.json) です。YAML の言語サーバーを使うエディターでは、ファイルの先頭に `# yaml-language-server: $schema=<このファイルへのパス>` と書くと、キーの補完と説明、形の検査が使えます。上の例の 1 行目がこれです。パスは、書いているファイルのディレクトリを基準にします。

| 項目 | 内容 |
|---|---|
| スキーマが確認するもの | キーと値の形だけです。 |
| `check` が確認するもの | 実行ファイルと合っているか、同じキーの 2 回目、キーの組み合わせです。 |
| JSON で書いたファイル（最初の文字が `{`） | コメントを書けないので、エディターの設定で、ファイルとスキーマを結び付けます。 |
| VS Code の設定 | `json.schemas` に、ファイルの名前のパターンとスキーマのパスを書きます。 |
| `"$schema"` のキー | ファイルの中には書けません（ランチャーが、知らないキーのエラーにします）。 |
| テンプレートのスキーマ | `schema/` にコピーが入っていて、`get_tools.py` が、ダウンロードしたツールのバージョンのスキーマで上書きします。 |
| ツールのアーカイブのスキーマ | `schema/` にあります。 |

### 実行ファイルのパス

`command` の最初の要素が実行ファイルです。

| 書き方 | 扱い |
|---|---|
| 相対パス | 設定ファイルのあるディレクトリを基準にします（コマンドを実行したディレクトリではありません）。 |
| `..` を含むパス | 正規化しません（シンボリックリンクをまたぐ `..` は OS の解釈のまま）。 |
| 設定ファイルのディレクトリになく、区切り（`/`、`\`）を含まない名前（`python3` など） | `PATH` から探します（`PATH` の相対のディレクトリは使いません）。 |
| 設定ファイルのディレクトリにあっても実行できないファイル（実行の許可がないもの） | 使わず、`PATH` を探します。 |
| 区切り | Windows でも `/` で書けます。 |
| `\` を使うパス | YAML の単一引用符で書きます（`'C:\ecu\engine.exe'`）。 |
| 空白や ASCII 以外の文字を含むパス | 使えます。 |
| Windows で、パスにファイルがなく、`.exe` で終わらない | `.exe` を付けたファイルがあれば、それを使います（同じ設定ファイルを Linux と Windows で使えます）。 |
| バッチファイル（`.bat`、`.cmd`） | エラーなので、それが起動する実行ファイルを書きます。 |

`PATH` で見つけたときは、注記（`found-on-path`）でその場所を表示します。その PC のパスなので、この注記は PC ごとに違います。バッチファイルがエラーなのは、cmd.exe を通り、引数の引用のルールが違うためです。

### 波括弧とプレースホルダー

`command` の中で特別なのは、英数字と `_` を `{` と `}` で囲んだ `{word}` の形だけです。ほかの波括弧は、そのまま渡します（JSON の引数 `'{"a":{"b":1}}'` は 1 バイトも変わりません）。

| 書いたもの | 扱い |
|---|---|
| `describe: false` の `command` の、実行ファイルより後の引数の `{registry_uri}` | シミュレーションの registry の URI に置き換えます。 |
| `describe: false` の `command` の、実行ファイルより後の引数の `{name}` | participant 名に置き換えます。 |
| `describe: true` の `command` の `{registry_uri}` と `{name}` | エラーです。 |
| 知らない `{word}` | どちらの `command` でもエラーです。 |
| 実行ファイルのパスの `{word}` | すべてエラーです。 |
| `{{word}}` | `{word}` という文字列そのものを渡します（`{{name}}` は `{name}` になります）。 |
| `{registry_uri}` のない `describe: false` の `command` | 注記を表示します（registry の場所を受け取れないかもしれないため）。 |

- プレースホルダーは、長い引数の一部でもかまいません（例: `--uri={registry_uri}`）。
- `{word}` をエラーにするのは、打ち間違いを見つけるためです。
- `{{word}}` は、`describe: true` の引数と実行ファイルのパスでも同じです。
- YAML では、`{...}` を引用符で囲みます（`"{registry_uri}"`）。
- 囲まないと YAML のマッピングになり、エラーになります（指摘の文に直し方があります）。
- `*` か `&` で始まる値も引用符で囲みます（囲まないとエイリアスとアンカーです）。

### ecu_main の実行ファイル（`describe: true`）

`vehicle_sil_launch` は、`command` の引数に、次の引数を追加して起動します。`command` の引数は、`{{word}}` を `{word}` に戻したものです。

| 起動 | 追加する引数 |
|---|---|
| 照合 | `--describe --name <participant 名>` |
| `run` | `--name <participant 名> --registry-uri <URI> --start-timeout-ms <ms>` |

`network_names` があれば、`run` は、その後ろに `--map-network <シミュレーションの名前>=<実行ファイルの名前>` を書いた順に追加し、`j1939_nodes` があれば、何かを変える行ごとに `--j1939-node <ネットワーク>=<設定>` を追加します（照合の `--describe` には、どちらも追加しません）。

`<ms>` は `start_timeout_ms + stop_timeout_ms + 10000` です。開始を待つのはランチャーで、参加しなかった participant を挙げるのもランチャーです。ECU の側の制限は、ランチャーがいなくなったときのためのものです。次の 2 つの実行では、ECU が始まるまで待つ時間に上限がないので、10^12（`1000000000000`）です。この 2 つでも、始まらない participant は、ランチャーが `start_timeout_ms` で見つけます。manual の participant がいる実行では、下の「[待ち方](#待ち方)」の表の区間ごとに数えます。時刻 0 の前に止まる実行では、participant を待ち始めてから止まるまでを数え、止まっている間は数えず、進めた時点から改めて `start_timeout_ms` を数えます。

- 自分で起動する必須の participant がいる実行。
- `--control` と `--start-paused` で、時刻 0 のステップの前に止まる実行。

`command` に書ける引数と書けないものは、次のとおりです。

| 書くもの | 扱い |
|---|---|
| `ecu_main` の引数（`--describe`、`--name`、`--registry-uri`、`--local`、`--run-for-ms`、`--start-timeout-ms`、`--map-network`、`--j1939-node`、`--sample-every-step`、`--help`、`-h`。`--name=X` の形も） | 書けません（エラー `launcher-argument`）。`--map-network` は `network_names` に（[ネットワークの名前を合わせる](#ネットワークの名前を合わせるnetwork_names)）、`--j1939-node` は `j1939_nodes` に（[J1939 のアドレスと NAME を合わせる](#j1939-のアドレスと-name-を合わせるj1939_nodes)）書きます。 |
| `--no-control` | 書けます（その ECU は変数を公開しません）。 |
| `--silkit-config <file>` | 書けます（その ECU に SIL Kit の設定ファイルを渡します）。 |
| `required: false` | 書けません（エラー `not-required-ecu`）。`role: bridge` なら書けます。 |

**`--no-control`**: 照合の `--describe` にも同じ引数が渡るので、`--describe` の出力の `control` が `null` になり、`run` はその ECU の hello を待ちません（[ECU の変数との接続](simulation-run.md#ecu-の変数との接続control_link)）。

**`--silkit-config <file>`**: `command: [engine/vehicle_sil_engine_ecu, --silkit-config, engine.silkit.yaml]` のように書きます。相対パスは、設定ファイルのあるディレクトリを基準にします（participant の作業ディレクトリ）。照合の `--describe` にも渡るので、次のようなエラーは、照合で `describe-failed` として分かります（[ecu-executable.md](ecu-executable.md) の「SIL Kit の設定ファイルを渡す」）。

- ファイルがない。
- SIL Kit が受け付けない。
- participant 名と違う `ParticipantName` がある。

**`required: false`**: 書けないのは、ecu_main の実行ファイルが coordinated の participant として参加し、SIL Kit は必須でない coordinated の participant を始めないためです。ECU を除いて動かすときは、`enabled: false` を書くか、`--disable <participant 名>` を付けます（[participant を無効にする](#participant-を無効にするenabled--disable--enable)）。`role: bridge` の participant には書けます。ブリッジは、自分で、実時間で参加するためです（[テスト用の participant とブリッジ](#テスト用の-participant-とブリッジrole)）。

### ほかの実行ファイル（`describe: false`）

| 項目 | ルール |
|---|---|
| 照合 | 照合しません（名前、ステップ幅、ネットワークを確認できないことを注記します）。 |
| `step_ms` | 書いても照合しないので、注記します。 |
| 起動 | `run` は、書いた引数（プレースホルダーを置き換えたもの）のまま起動します。 |
| `required: true`（既定）の participant | `{registry_uri}` の registry に、participant 名で、coordinated のライフサイクルで参加する必要があります（シミュレーションはそれを待ちます）。 |
| シミュレーションに参加しないツール（ログを取るだけのものなど） | `required: false` にします。 |

### participant を無効にする（`enabled`、`--disable`、`--enable`）

ある ECU を除いて動かすときは、その participant に `enabled: false` を書きます。ファイルを書き換えずに組み合わせを変えるときは、`check` と `run` に次の引数を付けます。

| 引数 | すること |
|---|---|
| `--disable <participant 名>` | 無効にします。 |
| `--enable <participant 名>` | `enabled: false` のものを有効に戻します。 |

どちらも繰り返せます。コマンドラインの指定が、ファイルの `enabled` より優先します。

無効にした participant の扱いは次のとおりです。

| 項目 | 扱い |
|---|---|
| 照合と起動 | 照合も起動もしません（`--describe` も動かしません）。 |
| 実行ファイル | 探しません（まだない実行ファイルでもかまいません）。 |
| シミュレーション | その participant を待ちません。 |
| 項目の書き方 | 有効な participant と同じように確認します（キー、値の型、名前の重複、`command` の引数など）。 |
| 項目のエラー | 残りの照合を止めません。 |
| 同じ名前の項目 | 無効にしていてもエラーです（`--disable <名前>` がどれを指すかわからなくなるため）。 |
| 注記 | participant ごとに `participant-disabled` を表示します（`--strict` でも失敗にしません）。 |

- 書き方を確認するので、有効に戻したときに初めてエラーが出ることはありません。
- `enabled` の値が真偽値でない participant は、無効にするかどうかがわからないので、照合も起動もしません（エラーなので照合は失敗します）。
- 代わりの ECU と入れ替えるときは、別の名前にするか、設定ファイルを分けます。
- 注記を表示するのは、「一部を無効にして通った」を「全部で通った」と読み違えないためです。

残りの participant の照合は、次のようになります。

- 照合は、残りの participant の組み合わせで行います。
- 無効にした結果として出る指摘（participant が 1 つだけになったネットワークなど）は、そのまま表示します。
- 無効にしたことで出る警告（下の例の `single-participant` など）は警告のままなので、`--strict` を付けると失敗します（`run --strict` は何も起動しません）。
- 有効な participant のうち、少なくとも 1 つは必須（`required: true`）でなければなりません（エラー `no-required-participant`）。

`--disable` と `--enable` には、設定ファイルにある participant 名を書きます。

| 場合 | 扱い |
|---|---|
| 同じ名前を `--disable` と `--enable` の両方に書く | いつでも引数のエラーです（終了コード 4）。 |
| 設定ファイルにない名前で、設定ファイルを読めてエラーがない | 引数のエラーです（終了コード 4。使える名前を並べ、合わない名前を全部挙げます）。 |
| 設定ファイルにない名前で、設定ファイルを読めない（終了コード 3）か、エラーがある（終了コード 1） | ファイルのエラーを先に表示し、合わない名前は言いません。 |

合わない名前を言わないのは、名前を読めなかった項目のせいかもしれないためです。その間も、設定ファイルにある名前の指定は反映します。

<!-- vehicle_sil_launch check simulation.yaml --disable Dashboard -->
```text
simulation.yaml: 2 participants (1 disabled), 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    -      disabled

network    kind  participants
J1939_CAN  can   Engine

warning: simulation.yaml:8: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it; if it talks only to equipment outside, connect it outside to run it (external says what passes) [single-participant]
note: simulation.yaml:14: participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it [participant-disabled]

passed: 0 errors, 1 warning, 1 note
```

### participant を自分で起動する（`manual`、`--manual`）

ECU をデバッガーから動かすときなど、ある participant を `run` ではなく自分で起動するときは、その participant に `manual: true` を書きます。ファイルを書き換えずに、その実行だけ自分で起動するときは、`check` と `run` に `--manual <participant 名>` を付けます（繰り返せます）。`run` はその participant を起動せず、起動するコマンドを表示して、参加するのを待ちます。

照合は、ほかの participant と同じです（`describe: true` なら実行ファイルがローカルに必要で、`--describe` を動かします）。設定ファイルに `manual: true` と `describe: false` を両方書いたときは、`command` を省略でき、`check` は実行ファイルがランチャー側の PC に存在することを求めません。`describe: false` の participant にコマンドラインから `--manual` を指定した場合は、ファイル側の `command` は引き続き必須ですが、書いた `command` のローカル実行ファイル確認は省きます（`command` がローカルに存在する場合は通常どおり解決します）。manual の participant ごとに、注記 `participant-manual` を表示します。

引数の組み合わせのルールは次のとおりです。

| 場合 | 扱い |
|---|---|
| `--disable` と `--manual` に同じ名前を書く | 引数のエラーです（終了コード 4）。 |
| 設定ファイルにない名前 | `--disable` と同じ扱いです。 |
| 無効にした participant への `--manual` | `enabled` を変えません（無効のままです）。 |
| 設定ファイルの `manual: true` を、コマンドラインで戻す | そのための引数はありません。 |

#### `run` が表示するコマンド

`run` は、その participant を起動する順番のところで、作業ディレクトリと、起動するコマンドの 1 行を表示します（下の例）。`manual: true` かつ `describe: false` で `command` を省略した場合は、コマンドを表示せず設定した `name` での参加を待ちます。`command` を書いたがローカルに存在しない場合は、プレースホルダーを展開したコマンドをローカルの作業ディレクトリなしで表示します。引数は、`run` が自分で起動するときと同じです（ecu_main の実行ファイルには `--name`、`--registry-uri`、`--start-timeout-ms`。`network_names` があれば `--map-network`、`j1939_nodes` があれば `--j1939-node` も）。そのコマンドを、表示されたディレクトリで実行します。デバッガーでは、実行ファイル、引数、作業ディレクトリに分けて設定します。`serve [--once]` では、イベント `participant` の `state` が `manual` の行に、同じ内容があります。

引数の引用は、貼り付けるシェルに合わせてあります。

- Linux では、POSIX sh（bash、zsh）に貼る形です。
- Windows では、コマンドプロンプト（cmd.exe）に貼る形で、実行ファイルのパスは `\` で区切ります。
- cmd.exe の形では、C ランタイムがコマンドラインを読むルールで引用し、`&` `|` `<` `>` `^` `,` `;` を含む引数も `"` で囲みます（PowerShell は、引用のない `a,b` を 2 つの引数として渡し、`;` を文の区切りとして読みます）。
- cmd.exe は `"` の中でも `%...%` を展開するので、`%` を含む引数は直して貼ります。
- PowerShell に貼るときは、`"` で囲んだ実行ファイルの前に `&` を追加し、`$` と `` ` `` を含む引数を直します。

`registry_uri` を書いていなければ、manual の必須の participant がいる実行は、registry を `silkit://127.0.0.1:8500` で立てます。コマンドが実行ごとに変わらないので、デバッガーの設定を 1 度作れば使い回せます。そのポートを別のプログラムが使っていれば `setup` のエラーです（`registry_uri` に別のポートか、空いているポートを選ぶ 0 を書きます。0 ではコマンドが実行ごとに変わります）。

#### ループバックでない `registry_uri` のとき

`registry_uri` がループバックのアドレスでないとき、`run` は、コマンドの前の行に「with VEHICLE_SIL_LISTEN_ON_NETWORK=1 in its environment」と書きます。注記 `participant-manual` にも、同じ環境変数のことを書きます。

| 起動する PC | `--registry-uri` | `VEHICLE_SIL_LISTEN_ON_NETWORK=1` |
|---|---|---|
| ランチャーと同じ PC | 表示されたコマンドのままです。 | 設定して起動します（[待ち受けるアドレス](simulation-run.md#待ち受けるアドレス)）。 |
| ほかの PC | `registry at` の次の行（`from other computers, join at ...`）の URI に変えます。 | 必要ありません。 |

注記 `participant-manual` の文は、起動する PC を区別せずに、この環境変数を設定するように言います。`run` の `from other computers, join at ...` の行は、ほかの PC では必要ないと言います。ほかの PC で起動するときは、`run` の行のとおり、設定しません（手順は [howto/multi-pc.md](howto/multi-pc.md)）。

#### 待ち方

manual の必須の participant がいる実行では、開始の時間の上限を 3 つの区間に分けます。

| 区間 | 上限 |
|---|---|
| 1. 参加するまで | `run` が起動した participant は `start_timeout_ms` で、manual の participant には上限がありません。 |
| 2. 全員が参加した後、manual の participant がすべて最初のステップの準備を終える（ReadyToRun）まで | 上限はありません（デバッガーで止めていてもかまいません）。 |
| 3. その後、全員が最初のステップに着くまで | `start_timeout_ms` です。 |

- 区間 1 で、`run` が起動した participant が `start_timeout_ms` の間に参加しなければ、manual のいない実行と同じく `not-joined` です。
- manual でない必須の participant がそろうと、`waiting for <名前> to join (manual): ...` の行を 1 回表示します。
- 区間 3 で、`run` が起動した participant が初期化で固まれば、manual のいない実行と同じく `simulation-start` のエラーです。
- どの区間でも、Ctrl+C、`stop` のコマンド、Control Server の `Stop` で止まります（終了コードは、Ctrl+C なら 130、コマンドと `Stop` なら 0）。
- `required: false` の manual の participant は、待ちません（コマンドを表示するだけです）。

`--timeout-ms` は、manual の必須の participant がいる実行では、`run` が participant を待ち始めた時点から数えます（参加を待つ時間、開始を待つ時間、実行の時間の合計です）。始まる前に過ぎると `timeout` のエラーで、何を待っていたか（参加していない participant か、最初のステップに着いていない participant）をメッセージに挙げます。`manual: true` を書いた設定ファイルを CI で動かすときは、`--timeout-ms` を付けると、誰も起動しなくても終わります。

参加した manual の participant が、始まる前に終わる（デバッガーで止めて起動し直すなど）と、開始は失敗します（`simulation-start`）。その participant はこの実行には参加し直せないので、ランチャーも起動し直します。

待っている間に、設定ファイルにない名前の participant がシミュレーションの開始を待っていると、次の行を 1 回表示します。`--name` の打ち間違いなどで起き、2 秒 ServicesCreated のままか、Error の participant が対象です。

`participant '<名前>' joined the simulation, but this run has no participant of that name (did you mean '<近い名前>'?): it waits for the simulation's start, which only the participants of the file get; ...`

その participant は始まりません。`run` は、設定ファイルの名前の participant を待ち続けます。設定ファイルにない participant でも、自分で動く（autonomous の）もの（実時間の Peer、ブリッジ）は、シミュレーションの参加者として扱い、何も表示しません。

#### 実行中

監視することは、ほかの必須の participant と同じです。違いは次のとおりです。

| 項目 | manual の participant |
|---|---|
| 失敗の検出 | `run` はそのプロセスを見ていないので、シミュレーションから抜けたこと（接続が切れた、Error の状態）で検出します（`participant-failed`）。 |
| 出力 | 起動した場所（端末、デバッガー）に出力されます。 |
| デバッガーで止めている間 | 仮想時間が進みません。 |
| 速度を指定しているとき | 止めている間を遅れとして数えます（`behind-real-time`）。 |
| `--timeout-ms` | 止めている間も数えます。 |

#### 止め方

`run` は、manual の participant のプロセスに、停止の要求も強制終了も送信しません。

| 場合 | 起きること |
|---|---|
| シミュレーションが終わった | ecu_main の実行ファイルは、自分で終わります。 |
| シミュレーションが始まらなかった（中断、ほかの participant の失敗） | 参加していた manual の participant に SIL Kit の abort を送信します（ecu_main の実行ファイルは、開始の失敗として終わります）。 |
| どちらの場合も | 参加した manual の participant がシミュレーションを去るのを `stop_timeout_ms` まで待ってから、registry を閉じます。 |
| 去らない（デバッガーで止めているなど） | そう表示して終わります（エラーにはしません）。 |
| `run` が強制終了された（SIGKILL、クラッシュ、Windows でコンソールを閉じた） | 自分で起動した participant は残ることがあるので、自分で終わらせます。 |

- 去るのを待つ間は、Ctrl+C か `stop` のコマンドをもう 1 回送信すると、待つのを切り上げます。
- `run` が強制終了されたのが始まる前なら、残った participant は開始を待ち続けます。
- participant をデバッガーで止めたままでも、`run` は止められます（Ctrl+C、`stop` のコマンド）。
- そのとき `run` は、上の待ちの後、結果を表示して終わります。
- 止めたままの participant は残るので、自分で続行させるか、終わらせます。

#### 結果

| 出力 | manual の participant |
|---|---|
| participant の表の `ended` | `started by you (manual)`（参加した）か `not joined (manual)` です。 |
| JSON の `participants[].manual` | `true` です。 |
| JSON の `ended` | `"not_started"` です。 |
| JSON の `exit_code` | `null` です。 |

#### 例

[examples/ecu_main/simulation.yaml](../../examples/ecu_main/simulation.yaml) の Engine を自分で起動します。`run` は、標準エラーに次の 2 行を書きます（`<dir>` は例のディレクトリです。Windows では、パスが `\` で区切られ、実行ファイルの名前に `.exe` が付きます）。2 行目の `[vehicle_sil_launch]` の後が、実行するコマンドです。

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

traffic: J1939_CAN: 1 participant sent, 1 received (2 counted) (steps before 1000 ms)

network    participant  sent  received
J1939_CAN  Engine         11         0
J1939_CAN  Dashboard       0        11

passed: 0 errors, 0 warnings, 1 note
```

### テスト用の participant とブリッジ（`role`）

`role` は、participant の用途を照合に伝えます。照合のルールと表示だけが使い、`run` は、どの値でも同じに起動し、同じに参加させます（`--describe` の JSON と、シミュレーションの通信には含まれません）。

| 値 | 意味 | 照合で変わること |
|---|---|---|
| `ecu`（既定） | 車両の ECU | なし |
| `test` | テスト用の participant（受信するだけの spy、ECU に送信するツールなど） | ネットワークの ECU として数えません（`single-participant` は、spy を追加しても消えません）。 |
| `bridge` | ネットワークを実機のバスに中継する participant（[bridges.md](bridges.md)） | 下の警告で、ブリッジとして扱います。 |

`role: test` の participant が、J1939 のノードを `--describe` に出力していて、ブリッジがいるかもしれないネットワークにいると、警告 `test-participant-talks-on-bridged-network` です。

照合がブリッジとして扱う participant は、次のとおりです。

| 設定ファイル | ブリッジとして扱うもの |
|---|---|
| `role: bridge` の participant がいる | その participant だけです。 |
| `role: bridge` で `describe: true` | `--describe` に出力したネットワークだけを中継するとみなします。 |
| `role: bridge` で `describe: false` | どのネットワークを中継するかが照合にはわからないので、どのネットワークも中継するかもしれないとみなします。 |
| `role: bridge` の participant が 1 つもない | `role` を書いていない `describe: false` の participant を「ブリッジかもしれない」とみなします（警告の文もそう言い分けます）。 |

ブリッジでないツールには `role: ecu` か `role: test` を書くと、ブリッジかもしれないとはみなしません。

このリポジトリのブリッジ（`vehicle_sil_ethernet_bridge`、`vehicle_sil_vector_xl_bridge`）は `--describe` に答え、`run` が足す引数を受けるので、`describe: true`（既定）と `required: false` で書きます。`--describe` が `--network` のネットワークを報告するので、検査はブリッジが何を中継するかを知っています。

```yaml
  - name: EthBridge
    role: bridge
    required: false
    command: [vehicle_sil_ethernet_bridge, --network, ETH_EXT, --backend, packet, --interface, eth1, --all-frames]
```

ランチャーが引数に足すものと `--describe` が書くべきものは、bridge-contract.md の「[自作のブリッジを起こす](bridge-contract.md#自作のブリッジを起こすrole-bridge)」にあります。ブリッジごとの引数は、[ethernet-bridge.md](ethernet-bridge.md#設定ファイルから起こす) と [bridges.md](bridges.md#設定ファイルから起こす) の「設定ファイルから起こす」にあります。

警告の理由は次のとおりです。ブリッジは、SIL Kit の側で送信されたものを実機のバスに送信します。そのため、J1939 のノードを持つテスト用の participant の送信、とくにアドレスの要求（Address Claimed）も実機に出て、同じアドレスの実機の ECU とアドレスを取り合います。直し方は、その participant をそのネットワークでは受信だけにする（J1939 のノードを作らない）か、その participant を使う実行ではブリッジを無効にする（`--disable <ブリッジ>`、機種の `enabled: false`）かです。生の CAN で送信する participant は、`--describe` の出力からはわからないので、警告しません。

そのほかのルールは次のとおりです。

- 人向けの participant の表では、`ecu` でない participant の名前の後に `(role: test)` のように書きます。
- `check --json` では、`participants[].role` に出力されます。
- 有効な `role: bridge` の participant のいる実行は、`--connect` のある実行と同じく実時間を保ちます。速さ 1 で動き、`test` もシナリオを速さ 1 で実行します（[simulation-run.md](simulation-run.md#外につながる実行の時間--time-policy) の「外につながる実行の時間（`--time-policy`）」）。
- 機種（`variants`）では変えられません（`describe`、`required` と同じです）。

### ネットワークを外につなぐ（`external`、`--connect`）

ネットワークは、外の実機のバスにつなげます。`external` には、つないだときのネットワークのふるまいを書きます。使うのはつないだときだけで、つながない実行では読み飛ばします。書くのは、シミュレーションを走らせるどの PC でも同じことです。つなぐ先（アダプター、チャンネル）は PC ごとに違うので、ファイルには書かず、始めるときに `--connect <network>=<target>`（下）で渡します。`external.connect` は誤りです。

```yaml
networks:
  - name: J1939_CAN1
    kind: can
    external:
      bitrate: 250000
  - name: ETH_EXT
    kind: ethernet
    external:
      ethertypes: [0x88b5, 0x0800]
```

| キー | 種類 | 値 | 省略したとき |
|---|---|---|---|
| `bitrate` | `can` | チャンネルの速さ（bit/s。1 から 1000000 の整数）。つなぐ道具によっては、範囲がもっと狭くなります（Vector XL は 15000 から）。Vector XL はチャンネルに設定します（チャンネルの init access が要ります）。SocketCAN は設定せず、インターフェースが別の速さで動いていると止まります | チャンネルの設定を変えません |
| `ethertypes` | `ethernet` | ネットワークと外の間で通すフレームの EtherType。1 つ以上の一覧で、同じ値は 1 回だけ（`0x88b5`、`34997`、または引用符で囲んだ `"0x88b5"`。JSON には 16 進の数がありません） | `ethertypes` も `all_frames` もなければ、そのネットワークの ECU が報告した MAC アドレスのフレームを通します |
| `all_frames` | `ethernet` | `true` ですべてのフレームを通します。`ethertypes` とは一緒に書けません | `false` |

そのほかのルールは次のとおりです。

- `lin` のネットワークには `external` を書けません。LIN をつなぐブリッジがないので、SIL Kit の中だけで動きます。
- 既定（`ethertypes` も `all_frames` もない）で実機の線に出るのは、ECU（`role` のない participant、または `role: ecu`）のフレームだけで、入ってくるのもその宛てのフレームだけです。`role: test` の participant のフレームも通すなら、`ethertypes` か `all_frames` を書きます。
- ほかの種類のキー（`can` のネットワークの `ethertypes`、`ethernet` のネットワークの `bitrate`）は `unknown-key` の誤りです。
- 機種（`variants`）では変えられません（`networks` のほかのキーと同じです）。

#### `--connect`

`--connect <network>=<target>` は、このリポジトリのブリッジで、ネットワークを実機のバスにつなぎます。`check --connect` は、つないで走らせるときの形でファイルを照合します。`run --connect` は、ほかの participant より先にブリッジを起動し、ブリッジが終わると実行を失敗にします（[simulation-run.md](simulation-run.md#バスを外につなぐ--connect)「バスを外につなぐ（`--connect`）」）。`test` は `--connect` を使い方の誤り（終了コード 4）にします。各シナリオをできるだけ速く走らせるためです。

```sh
vehicle_sil_launch check --connect "ETH_EXT=vEthernet (A)" --connect J1939_CAN1=physical:1 simulation.yaml
vehicle_sil_launch run --speed 1 --connect "ETH_EXT=vEthernet (A)" --connect J1939_CAN1=physical:1 simulation.yaml
vehicle_sil_launch run --speed 1 --connect ETH_EXT=tap:vsil0 --connect J1939_CAN1=can0 simulation.yaml   # Linux
```

先は `none` か `[<backend>:]<selector>` です。backend は、ネットワークをどの方式でつなぐかを言います。付けなければ、OS の既定の方式が selector を受けます。

| 種類 | `<backend>:` | OS | ブリッジ | `<selector>` |
|---|---|---|---|---|
| `ethernet` | `npcap:`（Windows の既定） | Windows | `vehicle_sil_ethernet_bridge`（[ethernet-bridge.md](ethernet-bridge.md)）、`--backend npcap` | ブリッジの `--interface` が受けるアダプターの名前です。接続名（`vEthernet (A)`）、pcap の名前（`\Device\NPF_{...}`）、`{GUID}`。 |
| `ethernet` | `packet:`（Linux の既定） | Linux | 同じ、`--backend packet` | インターフェース名（`enp3s0`）です。 |
| `ethernet` | `tap:` | Linux | 同じ、`--backend tap` | 自分で作った TAP デバイスの名前（`vsil0`）です。 |
| `can` | `xl:`（Windows の既定。Linux では selector が `virtual:<n>` か `physical:<n>` のときの既定） | どの OS でも書ける。動くのは Windows | `vehicle_sil_vector_xl_bridge`（[bridges.md](bridges.md)） | `virtual:<n>` か `physical:<n>`。1 から始まるチャンネル番号です。 |
| `can` | `socketcan:`（Linux の既定。selector が XL のチャンネルでないとき） | Linux | `vehicle_sil_socketcan_bridge`（[socketcan-bridge.md](socketcan-bridge.md)） | インターフェース名（`can0`、`vcan0`）です。 |
| `lin` | なし | | なし | つなげません（`connect-kind`）。 |

- `<backend>:` と読むのは、最初の `:` の前の語が、上の表にあるその種類の backend（小文字）のときだけです。ほかの語の後の `:` は selector の一部です（`virtual:1`、`eth0:1`、`TAP:x`、`pcap:x`）。接頭辞の後の selector にも `:` を書けます（`npcap:tap:x` はアダプター `tap:x` です）。selector が backend の語で始まるか `none` のときは、backend を付けて渡します。`packet:none` は `none` という名前のインターフェースです。
- その OS にない backend（Windows の `tap:x` や `socketcan:can0`、Linux の `npcap:x`）は、誤り `connect-backend` です。その OS にある backend を並べ、接頭辞なしで書けば既定になることを言います。
- ランチャーは、Ethernet のネットワークではいつも backend をブリッジに渡す（`--backend`）ので、ブリッジが自分で選ぶことはありません。

`vehicle_sil_launch connections <file>` は、この PC での各ネットワークの先を、写して使える `--connect` の行で出します（simulation-run.md の「[つなげる先の一覧](simulation-run.md#つなげる先の一覧vehicle_sil_launch-connections)」）。

- 先を `none` にすると SIL Kit の中だけで、そのネットワークに `--connect` を書かないのと同じです（スクリプトで「つながない」と書くため）。`none` という名前のインターフェースは、backend を付けて `packet:none` と書きます。
- 1 台の PC の先を、毎回打たずにファイルに置くには、要求ファイルの `"connect"` に書いて `--request <file>` を渡します。そのとき `--connect` は、1 つのネットワークの先を置き換えるか外します（[simulation-run.md](simulation-run.md#要求ファイル--request) の「要求ファイル（`--request`）」）。
- 1 つのネットワークは 1 回だけ書きます。ネットワークの名前は最初の `=` までなので、名前に `=` を含むネットワークはつなげません（名前を変えます）。
- 先に空白があるときは、引数全体を引用符で囲みます（`--connect "ETH_EXT=vEthernet (A)"`）。
- ランチャーは、ブリッジを `vehicle_sil_launch` の隣で探し、次に `PATH` の絶対パスのディレクトリで探します。作業ディレクトリと設定ファイルのディレクトリは見ません。探すのは `--connect` で渡したネットワークのブリッジだけなので、`--connect` のない照合の出力は、どの PC でも同じです。ブリッジ（`--describe`、`connections` の一覧、run）は、ブリッジ自身のディレクトリで起動します。設定ファイルのディレクトリでは起動しません。人から受け取ったフォルダーに置かれた DLL を読み込ませないためです。ファイルの participant は、これまでどおり設定ファイルのディレクトリで起動します。
- ビルド済みのツールのアーカイブでは、`bin/` の `vehicle_sil_launch` の隣に `vehicle_sil_ethernet_bridge` があり、Linux では `vehicle_sil_socketcan_bridge` もあります。Vector XL の CAN のブリッジは入っていません（ビルドします。[bridges.md](bridges.md) の「ビルド」）。
- Vector XL のブリッジは Windows でだけ動きます。Linux で `xl:virtual:1` と書くと、`connect-tool-missing` が、そのネットワークは SIL Kit の中だけで動くと言います（見つかったブリッジ、つまり試験用のビルドがあれば、それを使います）。
- 照合は、下の引数と `--name bridge.<network>`（ブリッジの participant 名）でブリッジの `--describe` を走らせます。`run` は、同じ引数と `--name bridge.<network> --registry-uri <URI> --start-timeout-ms <start_timeout_ms>` でブリッジを起動します。`bridge.<network>` はファイルの participant ではないので、`--disable`、`--enable`、`--manual` には書けません。`--describe` のブリッジは、アダプターもチャンネルも開きません。引数を受けないブリッジは誤り `connect-tool-failed` で、ブリッジ自身の行を出します。
- つなぐネットワークでは、ブリッジを participant として数えます（ECU が 1 つでも `single-participant` になりません）。同じネットワークに `role: bridge` の participant がいると、輪です（`connect-loop`）。
- 実機のバスは仮想時間を待ちません。`--connect` のある実行は、既定では速さ 1 で動きます（注記 `outside-speed-set`）。1 以外の速さを明示するか `--start-paused` を指定すると、エラー `outside-real-time` です。`--time-policy free` で外せます（[simulation-run.md](simulation-run.md#外につながる実行の時間--time-policy) の「外につながる実行の時間（`--time-policy`）」）。

ランチャーがブリッジに渡す引数は、次のとおりです。

| 種類 | 引数 |
|---|---|
| `ethernet` | `--network <network> --backend <backend> --interface <selector>` と、通すフレーム。`--all-frames`（`all_frames: true`）、`--ethertype`（`ethertypes`）、どちらもなければ `--src-mac` で、そのネットワークの ECU が `--describe` に出力した MAC アドレス。`role: test` と `role: bridge` の participant のものは入れません（出力しない ECU や `describe: false` の participant がいる、または出力した ECU が 1 つもないと、誤り `connect-filter`） |
| `xl` の `can` | `--network <network> --xl-target <virtual か physical> --xl-channel <n>` と、`bitrate` があれば `--bitrate <bitrate>` |
| `socketcan` の `can` | `--network <network> --interface <selector>` と、`bitrate` があれば `--bitrate <bitrate>`（ブリッジはインターフェースがこの速さで動いているかを確かめるだけで、設定はしません） |

指摘は [simulation-check.md](simulation-check.md#指摘のコード) の `connect-*` です。`check --json` は、ブリッジ、見つけた場所、コマンドラインを `networks[].outside` に書き、人向けの出力はネットワークの表に列 `outside` を足します。

### 変数の名前と値を合わせる（`variables`）

同じ役目の ECU でも、実行ファイルによって変数の名前、単位、値の表し方が違うことがあります（他人の ECU、古いバージョン、機種ごとの違い）。`variables` に対応を書くと、シミュレーションの側（シナリオ、`check --json`、Control API とそれを使う GUI）からは、どの実行ファイルも同じ名前と値で見えます。実行ファイルは変えません。名前を ECU の側でそろえられるなら、そのほうが簡単です。対応表は、そろえられないもののためにあります。

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

キーは、シミュレーションで使う名前です。値は次の 4 つの形のどれかです。1 つの変数に書ける形は 1 つで、ここにないキーはエラーです。

| 形 | 意味 | 書ける変数 |
|---|---|---|
| `<実行ファイルの変数名>`（文字列） | 名前だけを替えます（`{ from: <名前> }` と同じです）。 | |
| `{ from, scale, offset }` | 見える値 = 実行ファイルの値 × `scale` + `offset` | 整数か実数 |
| `{ from, map: { <名前>: <値>, ... } }` | 実行ファイルの値（整数）に名前を付けます。 | 整数か列挙 |
| `{ absent: true }` | この実行ファイルには、その変数がありません。 | |

- `scale` は 0 以外の数（書かなければ 1）、`offset` は数（書かなければ 0）です。
- `map` では、1 つの値に付けられる名前は 1 つです。
- `absent: true` は、ほかのキーと一緒には書けません。
- `from` のある形には、`unit`（見える値の単位）も書けます。

#### 変数の見え方

| 項目 | 見え方 |
|---|---|
| 名前 | 対応を書いた変数は、キーの名前でだけ見えます（実行ファイルの名前では読み書きできません）。 |
| 並び | 実行ファイルが宣言した順のままです。 |
| 入力、出力、パラメーターの別 | 変わりません。 |
| `scale`、`offset` の型 | 実数になります。 |
| `scale`、`offset` の範囲（`min`、`max`）と増減の幅（`step`） | 見える値に換算します（`scale` が負なら、範囲の両端が入れ替わります）。 |
| `scale`、`offset` の単位 | `unit` に書いたもので、書かなければ空です（実行ファイルの単位は、換算した後の値には当てはまりません）。 |
| `map` の型 | 列挙になり、項目は `map` に書いたものだけです。 |
| `map` の数 | 実行ファイルの値のままです。 |
| `absent: true` | どこにも見えません。 |

`map` を使うと、実行ファイルによって数が違っても、シナリオに名前で書けば（`engine.gear: D`）同じシナリオが使えます。

`absent: true` の変数を使うシナリオの手順は、飛ばします（[scenario.md](scenario.md) の「機種とパラメーター」）。対応も `absent` も書いていない変数が実行ファイルになければ、シナリオの照合でエラーになります。

#### 書き込み

`scale`、`offset` の変数への書き込みは、逆に換算します（実行ファイルの値 = (書いた値 − `offset`) ÷ `scale`）。

| 項目 | ルール |
|---|---|
| 実行ファイルの変数が整数 | 最も近い整数に丸めます（ちょうど半分は 0 から遠いほうへ: 2.5 は 3、−2.5 は −3）。 |
| 範囲の外の値 | 実行ファイルに渡す前に拒否します。 |
| 範囲の判定 | 換算した後の実行ファイルの値で決めます（シナリオの照合も同じ決め方です）。 |
| 一覧に表示される範囲の両端 | 有効数字 15 桁に丸めた値です。 |
| `map` にない値 | 書けません。 |

丸めるので、書いた値と読み戻した値は、丸めの分だけ違うことがあります。シナリオで比べるときは `near` と `tolerance` を使います。

#### 対応の照合

照合（`check`、`run` の最初）は、対応の全部を、実行ファイルの `--describe` の出力と比べます。比べるのは、`from` の変数があるか、形が型に合うか、`map` の値を変数が取れるか、名前がほかの変数と重ならないかです（指摘は [simulation-check.md](simulation-check.md) の `variable-map-*`）。合わない対応が 1 つでもあれば、`run` は何も起動しません。

`check --json` の `participants[].variables` は対応を反映した後の変数で、`participants[].variable_map` に両方の名前が出力されます。

`describe: false` の participant にも `variables` は書けますが、実行の前には比べられません（注記 `variable-map-not-checked`）。対応は、その participant の Control Agent が接続したときに反映します。合わなかった対応は、次のように扱います。

| 項目 | 扱い |
|---|---|
| 反映 | 合わなかった対応は反映しません。 |
| 通知 | シミュレーションが始まったとき（Control Agent が始まった後に接続した場合は、実行の終わり）に、実行の行（`[vehicle_sil_launch] participant '<name>': variables: <n> of its mappings do not fit ...`、イベントでは `log`）で通知します。 |
| 実行の指摘 | 終わりに、`variable-map-not-applied`（エラー）にします。この指摘のために実行を止めることはしません。Control Agent がいつ接続しても、実行は最後まで進み、その後に失敗になります。 |
| 読み出し | その実行の間、合わなかった対応の変数は、実行ファイルの名前と値のまま読めます。 |
| 書き込み | その対応の名前（キーと `from`）への書き込みは拒否します。 |

書き込みを拒否するのは、共通の名前のつもりで書いた値が、別の変数や換算なしの値として反映されるのを防ぐためです。

### ネットワークの名前を合わせる（`network_names`）

ECU の実行ファイルは、ネットワークの名前をコードに持っています（`j1939("J1939_CAN")` など）。別々の人が作った ECU は、同じバスを違う名前で呼んでいることがあります。`network_names` に対応を書くと、実行ファイルを作り直さずに、そのネットワークをシミュレーションの名前のバスにつなげます。

```yaml
networks:
  - name: VehicleCAN
    kind: can
participants:
  - name: Engine
    command: [engine/engine_ecu]          # コードでは j1939("J1939_CAN")
    network_names:
      VehicleCAN: J1939_CAN               # この実行ファイルの J1939_CAN は、シミュレーションの VehicleCAN
  - name: Dashboard
    command: [dashboard/dashboard_ecu]    # コードでも VehicleCAN。対応は要りません
```

向きは、どこでも「シミュレーションの名前: 実行ファイルの名前」です（`variables` と同じです）。

- キーは `networks` にある名前です（ないとエラー `network-map-undeclared`）。値は、実行ファイルが `j1939()`、`can()`、`lin_master()`、`lin_slave()`、`ethernet()` に渡している名前です。
- 対応は名前ごとに効きます。同じ名前の `j1939()` と `can()` は、両方替わります。種類は替わりません。書いていないネットワークは、実行ファイルの名前のままです。
- 実行ファイルの 1 つのネットワークに付けられる名前は 1 つです。1 つの値を 2 つのキーに書くことも、対応の後で実行ファイルの 2 つのネットワークが同じ名前になることもエラーです（`network-map-collision`）。2 つの名前の入れ替え（`A: B` と `B: A`）は書けます。
- キーと値が同じ行（`VehicleCAN: VehicleCAN`）は、行がないのと同じです。機種で、共通の行を消すのに使います（下の「機種でネットワークを別のバスへ移す」）。
- キーは、空でなく、`=` を含まず、`-` で始まらないこと（エラー `value`。引数 `--map-network <キー>=<値>` で渡すためです）。
- `run` は、対応のある participant の引数の最後に、`--map-network VehicleCAN=J1939_CAN` を書いた順に追加します。起動した実行ファイルは、コードの `J1939_CAN` を SIL Kit の `VehicleCAN` につなぎます（[ecu-executable.md](ecu-executable.md) の「ネットワークの名前を替える」）。ECU のコードから見える名前は変わりません。自分で起動する participant（`manual`）のコマンドと、イベント `participant` の `command` にも、同じ引数が出ます。
- 照合（`check`、`run` の最初）は、自己申告のネットワークを対応の後の名前に直してから、ルールを当てます。表、`check --json` の `networks[].participants`、指摘の `network` は、シミュレーションの名前です。表の participant の行には `VehicleCAN <- J1939_CAN (j1939 0x00)` と両方の名前が出ます。`check --json` では、`participants[].network_map` に書いた行が、`participants[].reported.networks[].name` に実行ファイルの名前が出ます。
- 照合は、対応を自己申告と比べます。値の名前のネットワークを実行ファイルが使っていない（`network-map-unknown`）、実行ファイルが `--map-network` を受け取れない（`network-map-unsupported`。自己申告の `features` に `map-network` がない、前のバージョンの vehicle-sil で作った実行ファイルです。作り直すか、`networks` の名前をその実行ファイルの名前にして、ほかの participant の側に対応を書きます）。どれもエラーで、`run` は何も起動しません。
- `describe: false` の participant にも書けます（OS の違う別のコンピューターで自分で起動する ECU など）。照合できないので、注記 `network-map-not-checked` を出します。注記は、その実行（選んだ機種）で `run` が追加する引数を、そのまま書きます。無効にした participant と、機種が行を全部消した participant には出しません。`--map-network` を受け取れるのは ecu_main の実行ファイルだけです。ブリッジやほかのツールには `network_names` を書かず、その実行ファイルの引数か設定に、シミュレーションの名前を書きます。`describe: false` の `command` に `--map-network` を自分で書くこともできますが、`network_names` と両方は書けません（エラー `launcher-argument`）。

**機種でネットワークを別のバスへ移す**: 機種の行は、共通の行にキーごとに重なります。キーが違う行は追加されるので、実行ファイルの同じネットワークを別のキーに書くだけでは、名前が 2 つになります（エラー `network-map-collision`。メッセージに、追加する行が出ます）。共通の行を、キーと値が同じ行で消します。

```yaml
participants:
  - name: Engine
    command: [engine/engine_ecu]
    network_names:
      VehicleCAN: J1939_CAN             # 共通: J1939_CAN は VehicleCAN
variants:
  standard: {}
  body_bus:
    participants:
      Engine:
        network_names:
          VehicleCAN: VehicleCAN        # 共通の行を消す
          BodyCAN: J1939_CAN            # この機種では、J1939_CAN は BodyCAN
```

`network_names` のエラーは、選んでいない機種のものも出ます（機種の `command` のエラーと同じです。`check` は、ファイルに書いた全部の機種を読みます）。

制限:

- Windows のランチャーでは、対応のあるネットワークの名前（キーと値）は ASCII だけです（エラー `network-map-not-ascii`）。Windows の ECU の実行ファイルは、ASCII でない `--map-network` を拒否します（引数がコードページで届くためです）。Linux のランチャーはこのエラーを出さないので、Linux から起動を指示して Windows のコンピューターで ECU を動かすときも、対応のあるネットワークの名前は ASCII にします。
- 監視用の変数（`j1939.<ネットワーク名>.bus_state` など）の名前は、`run` ではシミュレーションの名前になります（`j1939.VehicleCAN.*`）。`describe: false` の participant の `variables` でこの変数を `from` に書いているときは、その名前を合わせます。
- `--silkit-config` で渡す SIL Kit の設定のコントローラーに `Network` があると、対応と一緒には使えません（起動のときに実行ファイルが終了コード 3 で拒否します）。rapidyaml なしでビルドした ECU は、対応と `--silkit-config` の併用そのものを、同じく起動のときに拒否します。どちらも照合では分かりません。
- 外から渡したポートの J1939（`j1939(name, port)`）の名前に対応を書いても、照合では分かりません（起動のときに終了コード 3）。
- `network_names` は、`describe: false` の participant がどのネットワークにいるかの申告にはなりません（照合は、その participant のネットワークを今までどおり知りません）。

### J1939 のアドレスと NAME を合わせる（`j1939_nodes`）

J1939 のノードは、アドレスと NAME を ECU のコード（`add_node()`）に持っています。同じ ECU の実行ファイルを 1 つのネットワークに 2 つ載せると、2 つが同じ NAME で同じアドレスを求め、どちらもアドレスを得られません（`name-conflict`）。`j1939_nodes` に行を書くと、実行ファイルを作り直さずに、片方に別のアドレスと NAME の欄を与えます。

```yaml
participants:
  - name: Engine
    command: [engine/engine_ecu]          # コードでノード 1 つ、0x00
  - name: Engine2
    command: [engine/engine_ecu]          # 同じ実行ファイル
    j1939_nodes:
      - { network: J1939_CAN, address: 0x01, identity_number: 2 }
```

行のキー:

| キー | 必須 | 値 |
|---|---|---|
| `network` | はい | ノードの CAN のネットワークの、シミュレーションでの名前（`networks` の名前。`network_names` があれば、そのキー。実行ファイルの名前ではありません） |
| `from` | | 実行ファイルのコードでのノードのアドレス。0..254（254: 空いているアドレスを求めるノード）。実行ファイルがそのネットワークにノードを 1 つだけ持つときは省けます |
| `address` | | ノードが求めるアドレス。0..254（254: 空いているアドレス。NAME が arbitrary address capable のノードだけ） |
| `identity_number` | | NAME の identity number の欄。0..2097151 |
| `function_instance` | | NAME の function instance の欄。0..31 |
| `ecu_instance` | | NAME の ECU instance の欄。0..7 |

- 数は引用符なしで、YAML 1.2 の整数の書き方で書きます。10 進（`010` は 10）か、小文字の `0x` を付けた 16 進（`0x01`）です（YAML は `0X01` を文字列として読みます。`1_0` と `0b11` も同じです）。範囲の外はエラー `value` です。欄の幅に切り詰めることはしません。
- `from` は、実行ファイルのコードのアドレスです。`j1939_nodes` のないときに `check` の表がそのノードに出すアドレスで、新しいアドレスではありません。実行ファイルがそのネットワークにノードを 2 つ以上持つときは、どの行にも `from` が要ります（ないとエラー `j1939-node-ambiguous`）。
- `from` のない行は、そこにある実行ファイルのただ 1 つのノードに当たります。ECU を作る人がコードでそのノードのアドレスを変えても、新しいアドレスのノードに当たります。アドレスを決めて指したいときは `from` を書きます。
- ノードごとに 1 行です（同じ `network` と `from` の行が 2 つ、`from` のない行と同じネットワークのほかの行は、エラー `j1939-node-duplicate`）。
- `network` と `from` だけの行は、何も変えません。`run` は渡さず、照合もしません。機種で共通の行を消すのに使います。
- NAME の欄は、NAME のあるノードにだけ書けます（ないとエラー `j1939-node-no-name`）。NAME のほかの欄（manufacturer code、function、arbitrary address capable など）は変えられません。
- `run` は、何かを変える行ごとに、`--map-network` の後ろに `--j1939-node <ネットワーク>=<設定>` を追加します（`--j1939-node J1939_CAN=address:0x01,identity_number:2`）。自分で起動する participant（`manual`）のコマンドと、イベント `participant` の `command` にも、同じ引数が出ます。[ecu-executable.md](ecu-executable.md) の「J1939 のノードのアドレスと NAME を替える」も見てください。
- 照合は、実行ファイルが申告したノードに、実行ファイルと同じ決まりで行を当ててから、アドレスと NAME の規則（`address-conflict`、`address-shared`、`name-conflict`）を見ます。表では、行でアドレスが変わったノードを、シミュレーションでの値を先に、両方のアドレスで書きます: `J1939_CAN (j1939 0x01<-0x00)`（254 は `any`）。注記 `j1939-node-changed` が、何が変わるかを並べます。`check --json` では、`participants[].j1939_nodes` が行と、各行が当たったノードを並べ、`participants[].reported` はコードの値のままです。
- ノードに当たらない行はエラーです（`j1939-node-unknown`、`j1939-node-ambiguous`、`j1939-node-indistinct`、`j1939-node-no-name`、`j1939-node-any-address`。[simulation-check.md](simulation-check.md#指摘のコード)）。前の vehicle-sil で作った実行ファイルは `--j1939-node` を受け取れません（`j1939-node-unsupported`: `features` に `j1939-node` がない）。どのエラーでも、`run` は何も起動しません。
- `describe: false` の participant にも書けます。照合できないので、注記 `j1939-node-not-checked` を出し、`run` が追加する引数を書きます。`--j1939-node` を受け取れるのは ecu_main の実行ファイルだけです。`describe: false` の `command` に `--j1939-node` を自分で書くこともできますが、何かを変える `j1939_nodes` と両方は書けません（エラー `launcher-argument`）。

**機種では**、同じノードの行（同じ `network` と `from`、またはどちらも `from` なし）は、共通の行を丸ごと置き換えます（設定を混ぜません）。機種の行は、共通の行と同じ形で書きます。共通が `from` なしで機種が `from` ありだと、別の 2 行です（`j1939-node-duplicate`）。共通の行を消すには、`network` と `from` だけを書きます:

```yaml
participants:
  - name: Engine2
    command: [engine/engine_ecu]
    j1939_nodes:
      - { network: J1939_CAN, from: 0x00, address: 0x01, identity_number: 2 }
variants:
  standard: {}
  alone:
    participants:
      Engine2:
        j1939_nodes:
          - { network: J1939_CAN, from: 0x00 }   # 共通の行を消す
```

制限:

- ほかの ECU のコードに書かれたこの ECU のこと（宛先や送り手の見分けに使うアドレス、NAME の全体）は変わりません。NAME の全体を比べて `address_of()` で探す ECU は、NAME の欄を変えた 2 台目を見つけられません。ECU 自身のコードのほかの定数（フィルター、送るデータの中の値）も変わりません。注記 `j1939-node-changed` が participant ごとにこれを言います。
- ECU が開始の後に追加するノードは `--describe` に出ず、コードの値のままです。
- ノードを別のアドレスに動かすと、address claim の後に送信を待つ時間が変わることがあります（待ちはアドレスの範囲で決まります）。規格と合っているかは確かめていません。
- Windows のランチャーでは、何かを変える行の `network` は ASCII にします（エラー `j1939-node-not-ascii`）。`network_names` と同じです。

### 機種（`variants`、`--variant`）

実行ファイルや変数の対応が機種ごとに違うときは、共通の部分を `participants` に書き、機種ごとの差分を `variants` に書きます。`check` と `run` は、`--variant <名前>` で選んだ機種の差分を共通の部分に重ねた設定を、照合し、実行します。全部の機種を順に実行するのは `test` です（[simulation-run.md](simulation-run.md#シナリオをまとめて実行するvehicle_sil_launch-test)「シナリオをまとめて実行する」）。シナリオは共通の名前だけを使うので、どの機種にも同じものを使えます。

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
| `variants.<機種>` | その機種の差分 |
| `variants.<機種>.participants.<participant>` | 共通の部分の participant の、この機種での違い |
| `…<participant>.command` | 実行ファイルと引数（共通の `command` の全体を置き換えます） |
| `…<participant>.variables` | 変数の対応表（共通の `variables` に、対応 1 つずつ重ねます） |
| `…<participant>.network_names` | ネットワークの名前の対応（共通の `network_names` に、行ごとに重ねます） |
| `…<participant>.j1939_nodes` | J1939 のノードの行（共通の `j1939_nodes` に、ノードごとに重ねます） |
| `…<participant>.enabled` | `true` か `false`（共通の `enabled` を置き換えます） |
| `variants.<機種>.params` | パラメーター（共通の `params` に、名前ごとに重ねます） |

- `variants.<機種>` が `{}`（か、値なし）なら、共通の部分のままです。
- 機種の名前は、空でなく、1 行で、`-` で始まらない名前にします。
- `variants.<機種>.participants` のキーは participant 名で、`participants` にある名前だけを書けます（エラー `variant-participant`）。
- `…<participant>.command` の書き方とルールは、`participants[].command` と同じです。
- `…<participant>.variables` では、同じ名前（キー）の対応は置き換え、ない名前は追加します。
- `…<participant>.network_names` では、同じキーの行は置き換え（キーと値が同じ行は、共通の行を消します）、ないキーは追加します。
- `…<participant>.j1939_nodes` では、同じ `network` と `from` の行は丸ごと置き換え（`network` と `from` だけの行は、共通の行を消します）、ない行は追加します（[J1939 のアドレスと NAME を合わせる](#j1939-のアドレスと-name-を合わせるj1939_nodes)）。

機種で変えられるのは、この 6 つ（`command`、`variables`、`network_names`、`j1939_nodes`、`enabled`、`params`）だけです。ほかのキーはエラーです（`unknown-key`）。ネットワーク、`step_ms`、`describe`、`required`、`role` などは、どの機種でも同じです。

| したいこと | 書き方 |
|---|---|
| ある機種にだけ participant を置く | `participants` に `enabled: false` で書き、その機種で `enabled: true` にします。 |
| 共通の対応を、ある機種で使わない | 同じ名前に、その機種の対応を書きます（`engine.speed: engine.speed` は、名前も値もそのままです）。 |

下の行の書き方を使うのは、対応を共通の部分より減らすことができないためです。

`--variant` のルールは次のとおりです。

| 場合 | 扱い |
|---|---|
| 機種を書いた設定ファイルで `--variant` を省く | 引数のエラーです（終了コード 4。メッセージに機種の一覧が出ます）。 |
| 機種のどれでもない名前の `--variant` | 引数のエラーです。 |
| 機種のない設定ファイルへの `--variant` | 引数のエラーです。 |

- 既定の機種はありません。
- 上の 2 つのエラーは、設定ファイルの文面にほかのエラーがあっても、先に表示します。
- このとき、共通の部分だけを機種の代わりに照合することはしません（メッセージに、ファイルにもエラーがあることを追加します）。
- 機種のない設定ファイルへの `--variant` のエラーは、`--disable` と同じで、文面にエラーがある間は言いません（`variants` を読めなかったせいかもしれないため）。

そのほかのルールは次のとおりです。

- 読むときには、選んでいない機種も含めて、全部の機種の形を確認します。
- 実行ファイルとの照合は、選んだ機種だけです。
- `--disable`、`--enable`、`--manual` は、機種を重ねた後に反映します。
- `check --json` には、`variant`（選んだ機種）と `variants`（全部の名前）が出力されます。
- `check --json` の `participants` は、機種を重ねた後のものです。

### シナリオのパラメーター（`params`、`--param`）

シナリオの中の数（しきい値、待ち時間、設定する値）を機種ごとに替えるときは、シナリオに `${name}` と書き、値を `params` に書きます。

| 項目 | ルール |
|---|---|
| 値 | 数、`true`・`false`、文字列のどれかです（1 つの値。配列やマッピングは書けません）。 |
| 名前 | 英数字と `_` です（数字で始めない）。 |

値は、後のものが前のものを置き換えます。順は次のとおりです。

| 順 | 値を書く場所 |
|---|---|
| 1 | シナリオの `params`（既定） |
| 2 | 設定ファイルの `params` |
| 3 | 機種の `params` |
| 4 | ケースの `params`（シナリオ内の `cases.<名前>.params`） |
| 5 | 要求ファイル（`--request`）の `params` |
| 6 | 引数の `--param <名前>=<値>` |

- `--param` は、`--scenario` と一緒にだけ使えます。
- シナリオが使っていない名前の `--param` は、引数のエラーです（終了コード 4。打ち間違いを黙って無視しません）。
- いくつものシナリオを実行する `test` では、どのシナリオも使っていない名前だけがエラーです。
- 設定ファイルと機種の `params` には、シナリオが使わない名前があってもかまいません（1 つの設定ファイルで、いくつものシナリオを実行するため）。

書き方とエラーは [scenario.md](scenario.md) の「機種とパラメーター」にあります。

## 引数と終了コード

```text
vehicle_sil_launch check [--json] [--file-only] [--strict] [--variant <name>] [--scenario <file> [--case <name>] [--param <name>=<value>]...]
                         [--describe-timeout-ms <ms>] [--connect <network>=<target>]...
                         [--time-policy realtime|free] [--request <file>] [--speed <speed>]
                         [--stop-at-ms <ms>] [--late-tolerance-ms <ms>]
                         [--participant-start-timeout-ms <ms>] [--participant-stop-timeout-ms <ms>]
                         [--text-from-stdin] [--descriptions <file>] [--write-descriptions <file>]
                         [--compare-commands <file>]
                         [--disable <name>]... [--enable <name>]... [--manual <name>]... [--] <file>
vehicle_sil_launch run [--json] [--stdin-commands] [--start-paused]
                       [--control <address> [--control-credentials <file>] [--control-tls-cert <file> --control-tls-key <file>]
                       [--control-audit-log <file>]]
                       [--variant <name>] [--scenario <file> [--case <name>] [--param <name>=<value>]... [--timeline <file>]]
                       [--speed <speed>] [--strict] [--log-dir <dir>] [--control-metrics <file>]
                       [--external-registry <uri>] [--timeout-ms <ms>]
                       [--stop-at-ms <ms>] [--late-tolerance-ms <ms>]
                       [--participant-start-timeout-ms <ms>] [--participant-stop-timeout-ms <ms>]
                       [--describe-timeout-ms <ms>] [--connect <network>=<target>]...
                       [--time-policy realtime|free] [--request <file>] [--disable <name>]...
                       [--enable <name>]... [--manual <name>]... [--] <file>
vehicle_sil_launch serve [--once] [--external-registry <uri>] [--events-stall-ms <ms>]
                         [--control <address> [--control-credentials <file>] [--control-tls-cert <file> --control-tls-key <file>]
                         [--control-audit-log <file>]] [--] <file>
vehicle_sil_launch test [--json] [--strict] [--fail-fast] [--external-registry <uri>] [--variant <name>]... [--case <name>]... [--param <name>=<value>]...
                        [--timeout-ms <ms>] [--describe-timeout-ms <ms>] [--time-policy realtime|free]
                        [--stop-at-ms <ms>] [--late-tolerance-ms <ms>] [--request <file>]
                        [--participant-start-timeout-ms <ms>] [--participant-stop-timeout-ms <ms>]
                        [--disable <name>]... [--enable <name>]...
                        <file> --scenarios <directory or files>...
vehicle_sil_launch connections [--json] [--network <name>] [--request <file>] [--] <file>
vehicle_sil_launch version [--json]
```

`connections` は、この PC でファイルの各ネットワークをどこにつなげるかを出します（[simulation-run.md](simulation-run.md#つなげる先の一覧vehicle_sil_launch-connections)「つなげる先の一覧（`vehicle_sil_launch connections`）」）。

`test` は、機種ごとにシナリオを 1 つずつ実行して、全部の結果を表にします（[simulation-run.md](simulation-run.md#シナリオをまとめて実行するvehicle_sil_launch-test)「シナリオをまとめて実行する」）。

### 引数

どのコマンドで使えるかは、上の使い方のとおりです。次の表の「条件」には、それ以外の条件を書いています。

| 引数 | 条件 | 意味 |
|---|---|---|
| `--json` | | 結果を JSON で書きます。 |
| `--once` | `serve` だけ | NDJSON の `serve` ストリームで 1 回だけ実行し、`result` を書いて終了します（[プログラムから使う](simulation-events.md)）。 |
| `--events-stall-ms <ms>` | `serve` だけ | イベントの書き込みがこの時間まったく進まなければ、読む側がいなくなったものとして扱います（1000 以上 86400000 以下の整数。既定は 30000）。 |
| `--stdin-commands` | `run` だけ | 標準入力からコマンドを読みます（標準入力の終わりも、停止の要求です）。 |
| `--start-paused` | `--stdin-commands` か `--control` を付けた `run` だけ | 全員がそろったら仮想時間 0 で止まり、`resume` か `step` のコマンドを待ちます。 |
| `--control <address>` | `run` だけ、Control Server を持つビルドだけ | Control API（gRPC）の Server を立てます（[control-api.md](control-api.md) の「ランチャーに接続する」）。 |
| `--control-credentials <file>` | `--control` だけ | トークンと役割のファイル（1 行に `<role> <name> <token>`）で、あれば、どの呼び出しにもトークンが必要です。 |
| `--control-tls-cert <file>`、`--control-tls-key <file>` | `--control` だけ | サーバーの証明書チェーンと秘密鍵（PEM）で、2 つ一緒に付けると TLS になります。 |
| `--control-audit-log <file>` | `--control` だけ | 誰が何を変えたかを追記します。 |
| `--scenario <file>` | | シナリオのファイルです（[scenario.md](scenario.md)）。 |
| `--case <name>` | `check`、`run`（`--scenario` と一緒に）、`test` | シナリオの `cases` から名前付きケースを選びます（[scenario.md](scenario.md#名前付きケースcases--case) の「名前付きケース」）。`test` では繰り返せます。 |
| `--variant <name>` | | 設定ファイルの機種を選びます（[機種](#機種variants--variant)）。 |
| `--param <name>=<value>` | `--scenario` と一緒にだけ | シナリオのパラメーター `${name}` の値です（[シナリオのパラメーター](#シナリオのパラメーターparams--param)）。 |
| `--timeline <file>` | `run` だけ、`--scenario` と一緒にだけ | シナリオが書いた値と見た値を、仮想時刻の順に JSON のファイルに書きます（[simulation-run.md](simulation-run.md) の「シナリオの実行を時間の軸で記録する（`--timeline`）」）。 |
| `--control-metrics <file>` | `run` だけ | 実行の終わりに、Control Hub の SIL Kit トランスポートの累計カウンター（`HubTransport::stats()`）を JSON のファイルに書きます。`--control` は要りません（[simulation-run.md](simulation-run.md#control-hub-トランスポートのメトリクスを記録する--control-metrics) の「Control Hub トランスポートのメトリクスを記録する（`--control-metrics`）」）。 |
| `--external-registry <uri>` | `run`、`serve`、`test` | プロセス内の registry を立てる代わりに、起動済みのループバック SIL Kit registry（`silkit://<loopback-host>:<port>`。ポートは 1 から 65535、パスなし）へ接続し、実行ごとに分離された `/vsil-<id>` のシミュレーションパスを使います（[simulation-run.md](simulation-run.md#vector-sil-kit-dashboard-で実行を観測する--external-registry) の「Vector SIL Kit Dashboard で実行を観測する（`--external-registry`）」）。 |
| `--scenarios <dir or files>` | `test` だけ | 実行するシナリオです。 |
| `--fail-fast` | `test` だけ | 最初に失敗した組の後は実行しません。 |
| `--speed <speed>` | `check`、`run` | Run の速度（数値か `max`）。 |
| `--stop-at-ms <ms>` | `check`、`run`、`test` | 仮想時刻での自動停止。省略時は自動停止なし。 |
| `--late-tolerance-ms <ms>` | `check`、`run`、`test` | 実時間の遅延許容値。既定は 10 ms。 |
| `--participant-start-timeout-ms <ms>`、`--participant-stop-timeout-ms <ms>` | `check`、`run`、`test` | ホストの起動・停止待機上限。既定は 30000 ms、10000 ms。 |
| `--strict` | | 警告があっても失敗にします（注記では失敗にしません）。 |
| `--log-dir <dir>` | `run` だけ | participant ごとの出力のファイルを置くディレクトリです。 |
| `--timeout-ms <ms>` | `run` と `test`（1 組ごと） | シミュレーションの開始から終わりまでの実時間の上限です（1 以上の整数。既定は上限なし）。 |
| `--describe-timeout-ms <ms>` | | 1 つの実行ファイルの `--describe` を待つ時間です（1 以上の整数。既定は 10000）。 |
| `--connect <network>=<target>` | `check` と `run` | ネットワークを、ブリッジで `<target>` の実機のバスにつなぎます。`none` は SIL Kit の中だけです（繰り返せます。1 つのネットワークは 1 回。[ネットワークを外につなぐ](#ネットワークを外につなぐexternal--connect)）。 |
| `--time-policy realtime\|free` | | シミュレーションの外につながる実行の時間です。`realtime`（既定）は速さ 1 に保ち、`free` は速さと時間の操作を利用者に任せます（[simulation-run.md](simulation-run.md#外につながる実行の時間--time-policy) の「外につながる実行の時間（`--time-policy`）」）。 |
| `--request <file>` | `check`、`run`、`connections` | 何を走らせるかを要求ファイル（JSON）から読みます。欄ごとに、引数がファイルに勝ちます。`connections` が読むのは `connect` だけで、その先がこの PC で使えるかを答えます（[simulation-run.md](simulation-run.md#要求ファイル--request) の「要求ファイル（`--request`）」）。 |
| `--network <name>` | `connections` だけ | ネットワーク `<name>` だけを出します。 |
| `--text-from-stdin` | `check` だけ | 構成の文を `<file>` からでなく標準入力から読みます。`<file>` は名前として使います。相対パスと要求ファイルの `scenario` はそのディレクトリから読み、指摘と JSON はその名前を書きます。ファイルはなくてもかまいません。 |
| `--file-only` | `check` だけ | 実行ファイルを探したり起動したりせず、設定テキストだけを照合します（[simulation-check.md](simulation-check.md#ファイルだけを照合する--file-only) の「ファイルだけを照合する（`--file-only`）」）。`--text-from-stdin` とは併用できません。 |
| `--write-descriptions <file>` | `check` だけ | 読めた participant の `--describe` の出力を、`--descriptions` のために `<file>` に書きます（[simulation-check.md](simulation-check.md#保存した説明--write-descriptions--descriptions) の「保存した説明（`--write-descriptions`、`--descriptions`）」）。 |
| `--descriptions <file>` | `check` だけ | プロセスを起動しません。各 participant の `--describe` の出力を、コマンドと実行ファイルが保存したときのままなら `<file>` から取ります。それ以外の participant は照合しません（[simulation-check.md](simulation-check.md#保存した説明--write-descriptions--descriptions) の同じ節）。 |
| `--compare-commands <file>` | `check` だけ | 実行されるコマンド（すべての機種での各 participant の `command`、`describe`、`manual`、`enabled` と、`describe: false` の participant の `network_names`・`j1939_nodes` のために `run` が足すもの。participant、機種、`registry_uri`）を、設定ファイル `<file>` のものと比べ、`--json` の `commands` に `"same"`、`"changed"`、`"not_compared"` を書きます（[simulation-check.md](simulation-check.md#実行されるコマンドを比べる--compare-commands) の「実行されるコマンドを比べる（`--compare-commands`）」）。 |
| `--disable <name>` | | participant `<name>` を無効にします（`enabled: false` と同じ。繰り返せます）。 |
| `--enable <name>` | | participant `<name>` を有効に戻します（`enabled: true` と同じ。繰り返せます）。 |
| `--manual <name>` | | participant `<name>` を自分で起動します（`manual: true` と同じ。繰り返せます）。 |
| `--` | | これより後はファイル名です（`-` で始まる名前のとき）。 |
| `--help`、`-h` | | 使い方を標準出力に書いて 0 で終わります（`--` より前のどこにあっても）。 |
| `--version` | | `version` と同じです（`--` より前のどこにあっても）。 |

引数ごとの補足です。

| 引数 | 補足 |
|---|---|
| `--events-stall-ms` | `result` を書き終えるのを待つ時間の上限でもあります（[読む側が遅いとき](simulation-events.md#読む側が遅いとき読まないとき)）。 |
| `--start-paused` | `--control` があれば、時刻 0 のステップの前で止まります。 |
| `--control` | `<address>` は、ポート番号（127.0.0.1 で待ち受ける。0 なら空いているポート）、`<host>:<port>`、`unix:<path>` のどれかです。 |
| `--control` | ループバックでないアドレスには、TLS と認証情報が必要です。 |
| `--scenario` | `check` では照合だけ、`run` では照合してから実行します。 |
| `--scenario` | シナリオが終わるとシミュレーションを止め、手順が失敗していれば、実行は失敗です。 |
| `--scenario` | `--start-paused` と一緒には使えません。 |
| `--scenario` | `--control` と一緒なら、クライアントは読むことと時刻を動かすことができ、書けません（[scenario.md](scenario.md) の「実行中に変数を見る」）。 |
| `--variant` | 機種を書いた設定ファイルには、必ず必要です。 |
| `--variant` | 機種のない設定ファイルには付けられません。 |
| `--variant` | `test` では、実行する機種を絞ります（繰り返せる。なければ全部の機種）。 |
| `--param` | 繰り返せます（同じ名前は 1 回だけ）。 |
| `--param` | `test` では、その名前のパラメーターがあるシナリオにだけ反映されます（どのシナリオにもなければ引数のエラー）。 |
| `--timeline` | 開けなければ、何も起動せずに終了コード 3 です。 |
| `--control-metrics` | `run` の終わりにアトミックに書きます（親ディレクトリがなければ作ります）。書き込みに失敗しても標準出力と `run --json` は変えず標準エラーに理由を出し、実行が非 0 ならその終了コードを保ち、0 なら 3 にします。 |
| `--external-registry` | 設定ファイルの `registry_uri` とは併用できません（終了コード 4）。開始時に外部 registry が待ち受けていなければ、どの participant も起動せずに準備失敗（終了コード 3）になります。 |
| `--scenarios` | ディレクトリ（直下の `.yaml`、`.yml`、`.json` を名前の順に）か、ファイル（書いた順に）です。 |
| `--scenarios` | 後ろにいくつでも並べられ、繰り返せます。 |
| `--scenarios` | 設定ファイルは、`--scenarios` より前か、`--` の後に書きます。 |
| `--timeout-ms` | 一時停止している時間も数えます。 |
| `--timeout-ms` | `--control` と `--start-paused` では、時刻 0 のステップの前に止まった時点から数えます。 |
| `--timeout-ms` | manual の必須の participant がいる実行では、participant を待ち始めた時点から数えます。 |
| `--timeout-ms` | 超えたら、中断してエラーにします。 |
| `--manual` | `run` は起動せず、コマンドを表示して待ちます。 |
| `--connect` | `run` は、ほかの participant より先にブリッジ `bridge.<network>` を起動します（[simulation-run.md](simulation-run.md#バスを外につなぐ--connect)「バスを外につなぐ」）。`test` では使い方の誤りです。各シナリオをできるだけ速く走らせるためです。 |
| `--request` | 読むのは指定したファイルだけです。ランチャーが探すことはありません。 |
| `--request` | ファイルの `scenario` は、作業ディレクトリからでなく、設定ファイルのディレクトリから読みます。 |
| `--time-policy` | ファイルに有効な `role: bridge` の participant がいるとき、`test` は各シナリオを速さ 1 で実行します。`--time-policy free` を付ければ、それでも最速で実行します。 |
| `--text-from-stdin` | 文には設定ファイルのルールが当たります（[ファイルの形式](#ファイルの形式)）。UTF-8（先頭の BOM は可）で、UTF-16 と NUL は読まず、16 MiB までです。Windows でもバイトのまま読むので、CRLF の改行と行番号はファイルと同じです。 |
| `--descriptions` | `--connect` と要求ファイルの `connect`: ブリッジを探さず、起動もしません。ほかは保存した説明で照合します。 |
| `--descriptions` | 保存したものと比べるのは、participant の名前、`command`、実行ファイルだけです。コマンドで渡すファイル、ライブラリ、環境変数が変わっても気づきません。使える出力がなければ、何も比べずに通ります。 |
| `--descriptions`、`--write-descriptions` | 一緒に使えます（ファイルから取った出力を書き直します）。`--text-from-stdin` とも一緒に使えます。 |
| `--compare-commands` | `<file>` は読むだけで（書いたとおり作業ディレクトリから）、この選択肢はプロセスを起動しません。`--text-from-stdin` なら文とファイルを比べます。`--descriptions` も一緒なら、照合はプロセスを起動しません。 |
| `--compare-commands` | 比べるものに誤りのある構成は、同じとも違うともせず、比べません（`"not_compared"`）。`"same"` は `result` と別です。照合が失敗する構成でも `"same"` になりえます。 |

Windows では、引数をワイド文字のコマンドラインから UTF-8 で読みます（ASCII 以外の文字を含むパスを使えます）。

### 終了コード

| 終了コード | 意味 |
|---|---|
| 0 | エラーがない |
| 1 | エラーがある |
| 3 | 設定ファイルを読めない |
| 4 | 引数のエラー |
| 5 | `run`: 指定した停止時刻かコマンドで、シナリオが終わる前に止めた |
| 130 | `run` か `test` が中断された |

終了コードごとの条件は次のとおりです。

| 終了コード | 場合 |
|---|---|
| 0 | 警告と注記だけなら 0 です（`--strict` では、警告もないこと）。 |
| 0 | `run` では、シミュレーションが正常に終わり、必須の participant がどれも 0 で終わったときです。 |
| 0 | `run` では、指定した停止時刻、`{"command":"stop"}`、または Control Server のクライアントの `Stop` で止め、ほかに問題がなかったときも 0 です。 |
| 1 | `--strict` では、警告があるときも 1 です。 |
| 1 | `run` では、照合のエラー（何も起動しない）か、実行のエラー（participant が失敗した、そろわなかった、止めても終わらなかったなど）です。 |
| 1 | `test` では、失敗した組が 1 つ以上ある、中断されていないのに実行しなかった組がある、`--strict` で実行した組が 1 つもない、のどれかです。 |
| 1 | `--scenario` のシナリオのファイルがないときは、`check` でも `run` でも 1 です（設定ファイルがないときの 3 とは違います）。 |
| 1 | `connections` では、この PC にあるブリッジが一覧を作れなかったか、ファイルに誤りがあるか、`--request` で要求ファイルの先が使えません（ないブリッジは誤りではありません）。 |
| 1 | `check --write-descriptions` では、ファイルを書けないときも 1 です（エラー `descriptions-write`）。 |
| 3 | 設定ファイルがない、文法のエラー、知らない形式のバージョンなどです。 |
| 3 | `run` で registry や controller を用意できないときも 3 です。 |
| 3 | 結果を標準出力に書けないときと、`vehicle_sil_launch` 自身の想定外の失敗も 3 です。 |
| 4 | エラーと使い方を標準エラーに書きます（対象の引数は下の表）。 |
| 5 | `run` では、指定した停止時刻、`{"command":"stop"}`、または Control Server のクライアントの `Stop` で、シナリオが終わる前に止め、ほかに問題がなかったときです。 |
| 130 | Ctrl+C などのシグナル、標準入力の終わり、閉じた標準出力です。 |

想定外の失敗（終了コード 3）では、標準エラーに理由を書きます。`--json` では、そのコマンドの形の JSON に、`code` が `internal` のエラーを書きます。

終了コード 4 になる引数は次のとおりです。

| 対象 | 引数のエラー |
|---|---|
| どのコマンドでも | 知らないコマンドや引数 |
| どのコマンドでも | 設定ファイルの名前がない、2 つ以上のファイル |
| どのコマンドでも | 同じ引数の 2 回目（`--disable` と `--enable` は繰り返せます） |
| どのコマンドでも | 値のない `--describe-timeout-ms` |
| どのコマンドでも | `--events`（廃止。`serve --once` または `serve` を使う） |
| `check` | `--stdin-commands`、`--events-stall-ms` |
| `check` | `--log-dir`、`--timeout-ms`、`--start-paused`、`--control` と `--control-*`、`--external-registry` |
| `run`、`serve`、`test` | 形式不正またはループバックでない `--external-registry`、あるいは設定ファイルに `registry_uri` があるときの `--external-registry` |
| `run` | `--file-only`、`--text-from-stdin`、`--descriptions`、`--write-descriptions`、`--compare-commands`（`check` だけのもの） |
| `run` | `--events-stall-ms`（`serve` だけのもの）、または `serve` で範囲外の `--events-stall-ms` |
| `run` | `--stdin-commands` も `--control` もない `--start-paused` |
| `run` | 数でも `max` でもない `--speed` |
| `run` | `--start-paused` と一緒の `--scenario` |
| `--control` | `--control` なしの `--control-credentials` など |
| `--control` | Control Server を持たないビルドの `--control` |
| `--control` | `--control` の値のエラー |
| `--control` | 読めない `--control-*` のファイル |
| `--control` | 認証情報が 1 件もない `--control-credentials` のファイル |
| `--control` | 片方だけの `--control-tls-cert` と `--control-tls-key` |
| `--control` | TLS と認証情報のない、ループバックでない `--control` |
| `version` | `--json` 以外の引数 |
| participant の指定 | 設定ファイルにない participant 名の `--disable`、`--enable`、`--manual` |
| participant の指定 | 同じ名前の `--disable` と `--enable`、同じ名前の `--disable` と `--manual` |
| 機種とパラメーター | 機種を書いた設定ファイルで `--variant` がない、機種のどれでもない名前の `--variant` |
| 機種とパラメーター | `cases` のあるシナリオを `--case` なしで `check`/`run`、`cases` のないシナリオへの `--case`、`--scenario` なしの `--case`（`check`/`run`）、存在しない `--case` 名 |
| 機種とパラメーター | 形の違う `--param`、`--scenario` なしの `--param`、シナリオにない名前の `--param` |
| `test` | `--scenarios` がない、`--scenarios` のパスがない、シナリオのファイルがないディレクトリ |
| `test` | `test` に付けた `run` だけの引数と `--scenario` |
| `test` | `--file-only`、`--text-from-stdin`、`--descriptions`、`--write-descriptions`、`--compare-commands`（`check` だけのもの） |
| `test` | `test` に渡した設定ファイルの、有効で `manual: true` の participant |

## 関連するドキュメント

| 調べること | ドキュメント |
|---|---|
| ECU の実行ファイルの引数と `--describe` の JSON | [ecu-executable.md](ecu-executable.md) |
| 照合の順序と指摘のコード | [simulation-check.md](simulation-check.md) |
| 実行、止め方、結果 | [simulation-run.md](simulation-run.md) |
| プログラムから `run` を使う（イベント、標準入力のコマンド） | [simulation-events.md](simulation-events.md) |
| 設定ファイルを書いて ECU を動かす手順 | [複数の ECU をつないで動かす](howto/integrate-ecus.md) |
