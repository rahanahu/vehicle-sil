# シミュレーションの設定ファイル（`vehicle_sil_launch`）

[English](../simulation-config.md)

`vehicle_sil_launch` に渡す設定ファイルのリファレンスです。設定ファイルのキーと書き方のルール、`check`、`run`、`test`、`version` の引数と終了コードを載せています。形式とルールはまだ変わることがあります。

設定ファイルには、participant にする実行ファイル、それらが接続するネットワーク、止める時刻を書きます。別々に作った ECU の実行ファイル（[ecu-executable.md](ecu-executable.md)）を、1 つのシミュレーションで動かすために使います。

```sh
vehicle_sil_launch check simulation.yaml
vehicle_sil_launch run simulation.yaml
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
| 機種ごとに実行ファイルや値を変える | [機種](#機種variants--variant)、[シナリオのパラメーター](#シナリオのパラメーターparams--param) |
| 引数と終了コード | [引数と終了コード](#引数と終了コード) |

## `check` と `run` が起動するもの

| コマンド | すること |
|---|---|
| `vehicle_sil_launch check` | 設定ファイルと、各実行ファイルの `--describe` の出力を照合し、不整合を 1 回で全部表示します（[simulation-check.md](simulation-check.md)）。 |
| `vehicle_sil_launch run` | 同じ照合をしてから、シミュレーション全体を起動し、終わりまで監視して、各 participant がどう終わったかを表示します（[simulation-run.md](simulation-run.md)）。 |

`check` はシミュレーションを始めず、registry にも接続しません。ただし、`describe: true` の実行ファイルは、1 つずつ `--describe` で起動します。`--describe` は ECU を組み立てるので、その実行ファイルのコード（ECU のコンストラクターなど）が動きます。`describe: false` の実行ファイルは起動せず、あることだけを確認します。

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
| ファイルの種類 | 通常のファイルでないもの（デバイスやパイプ）は読みません。 |

例: [examples/ecu_main/simulation.yaml](../../examples/ecu_main/simulation.yaml)（CMake が実行ファイルの横、`build/<preset>/examples/ecu_main/` にコピーします）。

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

### キー

全体のキーは次のとおりです。

| キー | 必須 | 値 | 書かないとき |
|---|---|---|---|
| `vehicle_sil_simulation` | はい | 形式のバージョン（今は `1`） | |
| `stop_at_ms` | | シミュレーションを止める仮想時間（ミリ秒、1 以上 10^12 以下の整数） | participant が止めるか、`run` を Ctrl+C で止めるまで動きます |
| `start_timeout_ms` | | 必須の participant がそろうまで待つ時間（ミリ秒、1 以上 10^12 以下の整数） | 30000 |
| `stop_timeout_ms` | | `run` が participant の終了を待つ時間（ミリ秒、1 以上 10^12 以下の整数） | 10000 |
| `registry_uri` | | `run` が registry を立てる場所（`silkit://<host>:<port>`、ポートは 1 以上 65535 以下） | 127.0.0.1 の空いているポート |
| `speed` | | 仮想時間を進める速度（0.000001 以上 1000000 以下の数か、`max`） | 最速（`max` と同じ） |
| `time_step_ms` | | controller のタイムステップ（ミリ秒の仮想時間、1 以上 10^12 以下の整数） | 最速のときは 10 |
| `late_tolerance_ms` | | 遅れの許容（実時間のミリ秒、1 以上 10^12 以下の整数） | 10（タイムステップと速度によりません） |
| `networks` | はい | ネットワークの配列（空でもかまいません） | |
| `participants` | はい | participant の配列（1 つ以上） | |
| `variants` | | 機種の対応（機種の名前 → 共通の部分との差分。[機種](#機種variants--variant)） | 機種のない設定ファイルです |
| `params` | | シナリオのパラメーターの値（名前 → 値。[シナリオのパラメーター](#シナリオのパラメーターparams--param)） | |

`networks` の要素のキーは次のとおりです。ECU が使うネットワークは、すべて `networks` に書きます。

| キー | 必須 | 値 |
|---|---|---|
| `networks[].name` | はい | ネットワークの名前（ECU の `j1939()`、`can()`、`lin_master()`、`lin_slave()`、`ethernet()` に渡す名前。実行ファイルが別の名前で呼んでいるときは、その participant の `network_names` に書きます） |
| `networks[].kind` | はい | `can`、`lin`、`ethernet` のどれか |

`participants` の要素のキーは次のとおりです。

| キー | 必須 | 値 | 既定 |
|---|---|---|---|
| `participants[].name` | はい | participant 名 | |
| `participants[].command` | はい | 実行ファイルと、その引数の配列 | |
| `participants[].step_ms` | | 実行ファイルが `--describe` に出力するステップ幅（ミリ秒、1 以上 10^12 以下の整数） | 照合しません |
| `participants[].describe` | | `true` は ecu_main の実行ファイル（照合します）、`false` はそれ以外の実行ファイル（照合しません） | `true` |
| `participants[].required` | | シミュレーションがその participant を待つか（`true` か `false`） | `true` |
| `participants[].enabled` | | `false` の participant は無効にします（照合も起動もしません。[participant を無効にする](#participant-を無効にするenabled--disable--enable)） | `true` |
| `participants[].manual` | | `true` の participant は、`run` が起動せず、自分で起動します（[participant を自分で起動する](#participant-を自分で起動するmanual--manual)） | `false` |
| `participants[].role` | | participant の用途（`ecu`、`test`、`bridge`。[テスト用の participant とブリッジ](#テスト用の-participant-とブリッジrole)） | `ecu` |
| `participants[].variables` | | 変数の対応表（[変数の名前と値を合わせる](#変数の名前と値を合わせるvariables)） | 変数は、実行ファイルの名前のまま見えます |
| `participants[].network_names` | | ネットワークの名前の対応。シミュレーションの名前（`networks` の名前）から、この実行ファイルがコードで使っている名前への対応を書きます（[ネットワークの名前を合わせる](#ネットワークの名前を合わせるnetwork_names)） | ネットワークは、実行ファイルの名前のままです |

キーごとの補足です。

**`start_timeout_ms`**: `run` は、ecu_main の実行ファイルにも `--start-timeout-ms` で値を渡します（渡す値は [ecu_main の実行ファイル](#ecu_main-の実行ファイルdescribe-true)）。

**`stop_timeout_ms`**: 止め方の各段階で、この時間だけ待ちます（[simulation-run.md](simulation-run.md) の「止め方」）。

**`registry_uri`**

| 場合 | registry の場所と、起きること |
|---|---|
| 書かない | 127.0.0.1 の空いているポートです。 |
| 書かず、自分で起動する必須の participant がいる | `silkit://127.0.0.1:8500` です（[participant を自分で起動する](#participant-を自分で起動するmanual--manual)）。 |
| ほかの PC の participant も参加させる | `silkit://0.0.0.0:8500` のように書きます（この PC の participant には `127.0.0.1` の URI を渡します）。 |
| ループバックのアドレス（`127.x.x.x`、`[::1]`、`localhost`）でない | 注記 `registry-on-network` を表示し、`run` が起動する participant に環境変数 `VEHICLE_SIL_LISTEN_ON_NETWORK=1` を渡します（[待ち受けるアドレス](simulation-run.md#待ち受けるアドレス)）。 |

ほかの PC から参加させる手順は、[howto/multi-pc.md](howto/multi-pc.md) にあります。

**`speed`**: 実時間 1 秒あたりの仮想時間の秒数です。`1` が等速、`2` が 2 倍速、`0.5` が半分の速度です。`run --speed` で上書きできます（[速度を指定して動かす](simulation-run.md#速度を指定して動かすspeed)）。

**`time_step_ms`**: 一時停止とステップ実行の細かさ、速度を合わせる細かさです。

| 場合 | `time_step_ms` の値 |
|---|---|
| 書かず、最速で動かす | 10 です。 |
| 書かず、最速で、`stop_at_ms` が 10 の倍数でない | `stop_at_ms` と 10 の最大公約数です。 |
| 書かず、速度を指定した | さらに、ECU のステップ幅との最大公約数にします（[速度を指定して動かす](simulation-run.md#速度を指定して動かすspeed)の「タイムステップと ECU のステップ」）。 |
| 書く | `stop_at_ms` の約数にします（約数でなければエラー `value`）。 |

**`late_tolerance_ms`**: タイムステップの遅れがこの値を超えたら、遅れたタイムステップとして数えます。

**`participants[].name`**: 空でなく、1 行で（制御文字なし）、`-` で始まらない名前にします。`SimulationController`（vehicle-sil の controller の participant 名）は使えません。照合では、ecu_main の実行ファイルに `--name` で渡します。

**`participants[].command`**: 引数がなければ、文字列 1 つでもかまいません（空白で分けません）。

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

`stop_at_ms`、`start_timeout_ms`、`stop_timeout_ms`、`registry_uri`、`speed`、`time_step_ms`、`late_tolerance_ms` は、シミュレーションを動かすときの値です。`check` は値の形（と `stop_at_ms` と `time_step_ms` の組み合わせ）だけを確認します。

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

`network_names` があれば、`run` は、その後ろに `--map-network <シミュレーションの名前>=<実行ファイルの名前>` を、書いた順に追加します（照合の `--describe` には追加しません）。

`<ms>` は `start_timeout_ms + stop_timeout_ms + 10000` です。開始を待つのはランチャーで、参加しなかった participant を挙げるのもランチャーです。ECU の側の制限は、ランチャーがいなくなったときのためのものです。次の 2 つの実行では、ECU が始まるまで待つ時間に上限がないので、10^12（`1000000000000`）です。この 2 つでも、始まらない participant は、ランチャーが `start_timeout_ms` で見つけます。manual の participant がいる実行では、下の「[待ち方](#待ち方)」の表の区間ごとに数えます。時刻 0 の前に止まる実行では、participant を待ち始めてから止まるまでを数え、止まっている間は数えず、進めた時点から改めて `start_timeout_ms` を数えます。

- 自分で起動する必須の participant がいる実行。
- `--control` と `--start-paused` で、時刻 0 のステップの前に止まる実行。

`command` に書ける引数と書けないものは、次のとおりです。

| 書くもの | 扱い |
|---|---|
| `ecu_main` の引数（`--describe`、`--name`、`--registry-uri`、`--local`、`--run-for-ms`、`--start-timeout-ms`、`--map-network`、`--help`、`-h`。`--name=X` の形も） | 書けません（エラー `launcher-argument`）。`--map-network` は `network_names` に書きます（[ネットワークの名前を合わせる](#ネットワークの名前を合わせるnetwork_names)）。 |
| `--no-control` | 書けます（その ECU は変数を公開しません）。 |
| `--silkit-config <file>` | 書けます（その ECU に SIL Kit の設定ファイルを渡します）。 |
| `required: false` | 書けません（エラー `not-required-ecu`）。 |

**`--no-control`**: 照合の `--describe` にも同じ引数が渡るので、`--describe` の出力の `control` が `null` になり、`run` はその ECU の hello を待ちません（[ECU の変数との接続](simulation-run.md#ecu-の変数との接続control_link)）。

**`--silkit-config <file>`**: `command: [engine/vehicle_sil_engine_ecu, --silkit-config, engine.silkit.yaml]` のように書きます。相対パスは、設定ファイルのあるディレクトリを基準にします（participant の作業ディレクトリ）。照合の `--describe` にも渡るので、次のようなエラーは、照合で `describe-failed` として分かります（[ecu-executable.md](ecu-executable.md) の「SIL Kit の設定ファイルを渡す」）。

- ファイルがない。
- SIL Kit が受け付けない。
- participant 名と違う `ParticipantName` がある。

**`required: false`**: 書けないのは、ecu_main の実行ファイルが coordinated の participant として参加し、SIL Kit は必須でない coordinated の participant を始めないためです。ECU を除いて動かすときは、`enabled: false` を書くか、`--disable <participant 名>` を付けます（[participant を無効にする](#participant-を無効にするenabled--disable--enable)）。

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

warning: simulation.yaml:8: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it [single-participant]
note: simulation.yaml:14: participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it [participant-disabled]

passed: 0 errors, 1 warning, 1 note
```

### participant を自分で起動する（`manual`、`--manual`）

ECU をデバッガーから動かすときなど、ある participant を `run` ではなく自分で起動するときは、その participant に `manual: true` を書きます。ファイルを書き換えずに、その実行だけ自分で起動するときは、`check` と `run` に `--manual <participant 名>` を付けます（繰り返せます）。`run` はその participant を起動せず、起動するコマンドを表示して、参加するのを待ちます。

照合は、ほかの participant と同じです（`describe: true` なら `--describe` を動かします）。manual の participant ごとに、注記 `participant-manual` を表示します。

引数の組み合わせのルールは次のとおりです。

| 場合 | 扱い |
|---|---|
| `--disable` と `--manual` に同じ名前を書く | 引数のエラーです（終了コード 4）。 |
| 設定ファイルにない名前 | `--disable` と同じ扱いです。 |
| 無効にした participant への `--manual` | `enabled` を変えません（無効のままです）。 |
| 設定ファイルの `manual: true` を、コマンドラインで戻す | そのための引数はありません。 |

#### `run` が表示するコマンド

`run` は、その participant を起動する順番のところで、作業ディレクトリと、起動するコマンドの 1 行を表示します（下の例）。引数は、`run` が自分で起動するときと同じです（ecu_main の実行ファイルには `--name`、`--registry-uri`、`--start-timeout-ms`。`network_names` があれば `--map-network` も）。そのコマンドを、表示されたディレクトリで実行します。デバッガーでは、実行ファイル、引数、作業ディレクトリに分けて設定します。`--events` では、イベント `participant` の `state` が `manual` の行に、同じ内容があります。

引数の引用は、貼り付けるシェルに合わせてあります。

- Linux では、POSIX sh（bash、zsh）に貼る形です。
- Windows では、コマンドプロンプト（cmd.exe）に貼る形で、実行ファイルのパスは `\` で区切ります。
- cmd.exe の形では、C ランタイムがコマンドラインを読むルールで引用し、`&` `|` `<` `>` `^` を含む引数も `"` で囲みます。
- cmd.exe は `"` の中でも `%...%` を展開するので、`%` を含む引数は直して貼ります。
- PowerShell に貼るときは、`"` で囲んだ実行ファイルの前に `&` を追加し、`$` と `` ` `` を含む引数を直します。

`registry_uri` を書いていなければ、manual の必須の participant がいる実行は、registry を `silkit://127.0.0.1:8500` で立てます。コマンドが実行ごとに変わらないので、デバッガーの設定を 1 度作れば使い回せます。そのポートを別のプログラムが使っていれば `setup` のエラーです（`registry_uri` に別のポートを書きます）。

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
- どの区間でも、Ctrl+C、`stop` のコマンド、Control Server の `Stop` で止まります（終了コード 130）。
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

警告の理由は次のとおりです。ブリッジは、SIL Kit の側で送信されたものを実機のバスに送信します。そのため、J1939 のノードを持つテスト用の participant の送信、とくにアドレスの要求（Address Claimed）も実機に出て、同じアドレスの実機の ECU とアドレスを取り合います。直し方は、その participant をそのネットワークでは受信だけにする（J1939 のノードを作らない）か、その participant を使う実行ではブリッジを無効にする（`--disable <ブリッジ>`、機種の `enabled: false`）かです。生の CAN で送信する participant は、`--describe` の出力からはわからないので、警告しません。

そのほかのルールは次のとおりです。

- 人向けの participant の表では、`ecu` でない participant の名前の後に `(role: test)` のように書きます。
- `check --json` では、`participants[].role` に出力されます。
- 機種（`variants`）では変えられません（`describe`、`required` と同じです）。

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
| 通知 | そのときに、実行の行（`[vehicle_sil_launch] participant '<name>': variables: <n> of its mappings do not fit ...`、イベントでは `log`）で通知します。 |
| 実行の指摘 | 終わりに、`variable-map-not-applied`（エラー）にします。 |
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
| `…<participant>.enabled` | `true` か `false`（共通の `enabled` を置き換えます） |
| `variants.<機種>.params` | パラメーター（共通の `params` に、名前ごとに重ねます） |

- `variants.<機種>` が `{}`（か、値なし）なら、共通の部分のままです。
- 機種の名前は、空でなく、1 行で、`-` で始まらない名前にします。
- `variants.<機種>.participants` のキーは participant 名で、`participants` にある名前だけを書けます（エラー `variant-participant`）。
- `…<participant>.command` の書き方とルールは、`participants[].command` と同じです。
- `…<participant>.variables` では、同じ名前（キー）の対応は置き換え、ない名前は追加します。
- `…<participant>.network_names` では、同じキーの行は置き換え（キーと値が同じ行は、共通の行を消します）、ないキーは追加します。

機種で変えられるのは、この 5 つ（`command`、`variables`、`network_names`、`enabled`、`params`）だけです。ほかのキーはエラーです（`unknown-key`）。ネットワーク、`step_ms`、`describe`、`required`、`role` などは、どの機種でも同じです。

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
| 4 | 引数の `--param <名前>=<値>` |

- `--param` は、`--scenario` と一緒にだけ使えます。
- シナリオが使っていない名前の `--param` は、引数のエラーです（終了コード 4。打ち間違いを黙って無視しません）。
- いくつものシナリオを実行する `test` では、どのシナリオも使っていない名前だけがエラーです。
- 設定ファイルと機種の `params` には、シナリオが使わない名前があってもかまいません（1 つの設定ファイルで、いくつものシナリオを実行するため）。

書き方とエラーは [scenario.md](scenario.md) の「機種とパラメーター」にあります。

## 引数と終了コード

```text
vehicle_sil_launch check [--json] [--strict] [--variant <name>] [--scenario <file> [--param <name>=<value>]...]
                         [--describe-timeout-ms <ms>] [--disable <name>]...
                         [--enable <name>]... [--manual <name>]... [--] <file>
vehicle_sil_launch run [--json | --events [--events-stall-ms <ms>]] [--stdin-commands] [--start-paused]
                       [--control <address> [--control-credentials <file>] [--control-tls-cert <file> --control-tls-key <file>]
                       [--control-audit-log <file>]]
                       [--variant <name>] [--scenario <file> [--param <name>=<value>]... [--timeline <file>]]
                       [--speed <speed>] [--strict] [--log-dir <dir>] [--timeout-ms <ms>]
                       [--describe-timeout-ms <ms>] [--disable <name>]... [--enable <name>]... [--manual <name>]...
                       [--] <file>
vehicle_sil_launch test [--json] [--strict] [--fail-fast] [--variant <name>]... [--param <name>=<value>]...
                        [--timeout-ms <ms>] [--describe-timeout-ms <ms>] [--disable <name>]... [--enable <name>]...
                        <file> --scenarios <directory or files>...
vehicle_sil_launch version [--json]
```

`test` は、機種ごとにシナリオを 1 つずつ実行して、全部の結果を表にします（[simulation-run.md](simulation-run.md#シナリオをまとめて実行するvehicle_sil_launch-test)「シナリオをまとめて実行する」）。

### 引数

どのコマンドで使えるかは、上の使い方のとおりです。次の表の「条件」には、それ以外の条件を書いています。

| 引数 | 条件 | 意味 |
|---|---|---|
| `--json` | | 結果を JSON で書きます。 |
| `--events` | `run` だけ | 経過と結果をイベントとして書きます（[プログラムから使う](simulation-events.md)。`--json` と一緒には使えません）。 |
| `--events-stall-ms <ms>` | `--events` を付けた `run` だけ | イベントの書き込みがこの時間まったく進まなければ、読む側がいなくなったものとして扱います（1000 以上 86400000 以下の整数。既定は 30000）。 |
| `--stdin-commands` | `run` だけ | 標準入力からコマンドを読みます（標準入力の終わりも、停止の要求です）。 |
| `--start-paused` | `--stdin-commands` か `--control` を付けた `run` だけ | 全員がそろったら仮想時間 0 で止まり、`resume` か `step` のコマンドを待ちます。 |
| `--control <address>` | `run` だけ、Control Server を持つビルドだけ | Control API（gRPC）の Server を立てます（[control-api.md](control-api.md) の「ランチャーに接続する」）。 |
| `--control-credentials <file>` | `--control` だけ | トークンと役割のファイル（1 行に `<role> <name> <token>`）で、あれば、どの呼び出しにもトークンが必要です。 |
| `--control-tls-cert <file>`、`--control-tls-key <file>` | `--control` だけ | サーバーの証明書チェーンと秘密鍵（PEM）で、2 つ一緒に付けると TLS になります。 |
| `--control-audit-log <file>` | `--control` だけ | 誰が何を変えたかを追記します。 |
| `--scenario <file>` | | シナリオのファイルです（[scenario.md](scenario.md)）。 |
| `--variant <name>` | | 設定ファイルの機種を選びます（[機種](#機種variants--variant)）。 |
| `--param <name>=<value>` | `--scenario` と一緒にだけ | シナリオのパラメーター `${name}` の値です（[シナリオのパラメーター](#シナリオのパラメーターparams--param)）。 |
| `--timeline <file>` | `run` だけ、`--scenario` と一緒にだけ | シナリオが書いた値と見た値を、仮想時刻の順に JSON のファイルに書きます（[simulation-run.md](simulation-run.md) の「シナリオの実行を時間の軸で記録する（`--timeline`）」）。 |
| `--scenarios <dir or files>` | `test` だけ | 実行するシナリオです。 |
| `--fail-fast` | `test` だけ | 最初に失敗した組の後は実行しません。 |
| `--speed <speed>` | `run` だけ | 設定ファイルの `speed` を上書きします（数か `max`。[速度を指定して動かす](simulation-run.md#速度を指定して動かすspeed)）。 |
| `--strict` | | 警告があっても失敗にします（注記では失敗にしません）。 |
| `--log-dir <dir>` | `run` だけ | participant ごとの出力のファイルを置くディレクトリです。 |
| `--timeout-ms <ms>` | `run` と `test`（1 組ごと） | シミュレーションの開始から終わりまでの実時間の上限です（1 以上の整数。既定は上限なし）。 |
| `--describe-timeout-ms <ms>` | | 1 つの実行ファイルの `--describe` を待つ時間です（1 以上の整数。既定は 10000）。 |
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
| `--scenario` | `--control` と一緒なら、クライアントは読むだけです（[scenario.md](scenario.md) の「実行中に変数を見る」）。 |
| `--variant` | 機種を書いた設定ファイルには、必ず必要です。 |
| `--variant` | 機種のない設定ファイルには付けられません。 |
| `--variant` | `test` では、実行する機種を絞ります（繰り返せる。なければ全部の機種）。 |
| `--param` | 繰り返せます（同じ名前は 1 回だけ）。 |
| `--param` | `test` では、その名前のパラメーターがあるシナリオにだけ反映されます（どのシナリオにもなければ引数のエラー）。 |
| `--timeline` | 開けなければ、何も起動せずに終了コード 3 です。 |
| `--scenarios` | ディレクトリ（直下の `.yaml`、`.yml`、`.json` を名前の順に）か、ファイル（書いた順に）です。 |
| `--scenarios` | 後ろにいくつでも並べられ、繰り返せます。 |
| `--scenarios` | 設定ファイルは、`--scenarios` より前か、`--` の後に書きます。 |
| `--timeout-ms` | 一時停止している時間も数えます。 |
| `--timeout-ms` | `--control` と `--start-paused` では、時刻 0 のステップの前に止まった時点から数えます。 |
| `--timeout-ms` | manual の必須の participant がいる実行では、participant を待ち始めた時点から数えます。 |
| `--timeout-ms` | 超えたら、中断してエラーにします。 |
| `--manual` | `run` は起動せず、コマンドを表示して待ちます。 |

Windows では、引数をワイド文字のコマンドラインから UTF-8 で読みます（ASCII 以外の文字を含むパスを使えます）。

### 終了コード

| 終了コード | 意味 |
|---|---|
| 0 | エラーがない |
| 1 | エラーがある |
| 3 | 設定ファイルを読めない |
| 4 | 引数のエラー |
| 130 | `run` か `test` が中断された |

終了コードごとの条件は次のとおりです。

| 終了コード | 場合 |
|---|---|
| 0 | 警告と注記だけなら 0 です（`--strict` では、警告もないこと）。 |
| 0 | `run` では、シミュレーションが正常に終わり、必須の participant がどれも 0 で終わったときです。 |
| 1 | `--strict` では、警告があるときも 1 です。 |
| 1 | `run` では、照合のエラー（何も起動しない）か、実行のエラー（participant が失敗した、そろわなかった、止めても終わらなかったなど）です。 |
| 1 | `test` では、失敗した組が 1 つ以上ある、中断されていないのに実行しなかった組がある、`--strict` で実行した組が 1 つもない、のどれかです。 |
| 1 | `--scenario` のシナリオのファイルがないときは、`check` でも `run` でも 1 です（設定ファイルがないときの 3 とは違います）。 |
| 3 | 設定ファイルがない、文法のエラー、知らない形式のバージョンなどです。 |
| 3 | `run` で registry や controller を用意できないときも 3 です。 |
| 3 | 結果を標準出力に書けないときと、`vehicle_sil_launch` 自身の想定外の失敗も 3 です。 |
| 4 | エラーと使い方を標準エラーに書きます（対象の引数は下の表）。 |
| 130 | Ctrl+C、`{"command":"stop"}`、標準入力の終わり、閉じた標準出力、Control Server のクライアントの `Stop` などです。 |

想定外の失敗（終了コード 3）では、標準エラーに理由を書きます。`--json` では、そのコマンドの形の JSON に、`code` が `internal` のエラーを書きます。

終了コード 4 になる引数は次のとおりです。

| 対象 | 引数のエラー |
|---|---|
| どのコマンドでも | 知らないコマンドや引数 |
| どのコマンドでも | 設定ファイルの名前がない、2 つ以上のファイル |
| どのコマンドでも | 同じ引数の 2 回目（`--disable` と `--enable` は繰り返せます） |
| どのコマンドでも | 値のない `--describe-timeout-ms` |
| `check` | `--events`、`--stdin-commands` |
| `check` | `--log-dir`、`--timeout-ms`、`--speed`、`--start-paused`、`--control` と `--control-*` |
| `run` | `--json` と `--events` を一緒に付ける |
| `run` | `--events` なしの `--events-stall-ms` と、範囲の外の値 |
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
| 機種とパラメーター | 形の違う `--param`、`--scenario` なしの `--param`、シナリオにない名前の `--param` |
| `test` | `--scenarios` がない、`--scenarios` のパスがない、シナリオのファイルがないディレクトリ |
| `test` | `test` に付けた `run` だけの引数と `--scenario` |
| `test` | `test` に渡した設定ファイルの、有効で `manual: true` の participant |

## 関連するドキュメント

| 調べること | ドキュメント |
|---|---|
| ECU の実行ファイルの引数と `--describe` の JSON | [ecu-executable.md](ecu-executable.md) |
| 照合の順序と指摘のコード | [simulation-check.md](simulation-check.md) |
| 実行、止め方、結果 | [simulation-run.md](simulation-run.md) |
| プログラムから `run` を使う（イベント、標準入力のコマンド） | [simulation-events.md](simulation-events.md) |
| 設定ファイルを書いて ECU を動かす手順 | [複数の ECU をつないで動かす](howto/integrate-ecus.md) |
