# 照合と指摘（`vehicle_sil_launch check`）

[English](../simulation-check.md)

`vehicle_sil_launch check <file>` が、設定ファイルと ECU の実行ファイルを照合する順序、指摘のコードの一覧、出力の形式（人が読む表と JSON）のリファレンスです。`vehicle_sil_launch run` も、最初に同じ照合をします。

| 調べること | 節 |
|---|---|
| 何を、どの順に照合するか | [照合の順序](#照合の順序) |
| 指摘のコードの意味と直し方 | [指摘のコード](#指摘のコード) |
| J1939 のアドレスが重なったときの重大度 | [J1939 のアドレスの重なり](#j1939-のアドレスの重なり) |
| 人が読む出力の読み方 | [人向け](#人向け) |
| `check --json` のキー | [JSON（`--json`）](#json--json) |
| 引数と終了コード | [simulation-config.md](simulation-config.md) の「引数と終了コード」 |

## 照合

`check` は、見つけたものを全部まとめて、指摘として表示します。指摘には 3 つの重大度があります。

| 重大度 | 行の先頭 | 照合の結果 |
|---|---|---|
| エラー | `error:` | 照合を失敗にします。 |
| 警告 | `warning:` | `--strict` のときだけ失敗にします。 |
| 注記 | `note:` | 失敗にしません（照合が確認できなかったことを伝えます）。 |

### 照合の順序

| 順 | 照合するもの | 内容 |
|---|---|---|
| 1 | 設定ファイルの文面 | 形式のバージョン、知らないキーと足りないキー、値の型と範囲、名前、`command` の引数と波括弧を確認します。 |
| 2 | 実行ファイル | あること、ディレクトリでないこと、バッチファイルでないこと、（Linux では）実行の許可があることを確認します。 |
| 3 | `--describe` の出力 | `describe: true` の各実行ファイルを `--describe --name <participant 名>` で起動し、標準出力の JSON を読みます。 |
| 4 | 設定ファイルと `--describe` の出力 | 2 つを照合します。 |

`--connect` があると、順 3 の後に、つなぐネットワークごとにブリッジを探し、ランチャーが渡す引数でその `--describe` を走らせます（[simulation-config.md](simulation-config.md#ネットワークを外につなぐexternal--connect)「ネットワークを外につなぐ」）。

**順 1 の補足**

- 機種（`variants`）は、選んでいないものも含めて全部の形を確認します。
- その後、`--variant` で選んだ機種の差分を共通の部分に重ね、ここから先は、重ねた後の設定を照合します（[simulation-config.md](simulation-config.md) の「機種」）。
- 機種を選んだときは、人向けの出力の最初の行に `(variant <名前>)` が付きます。
- エラーのある participant は、順 2 に進めません。
- ここでいうエラーは、名前がない・使えない、`command` が使えない、名前が重複している、`describe` が真偽値でない、`command` の引数のエラーです。
- 知らないキーや `step_ms` のエラーは、照合を止めません。

**順 2 の補足**

- 無効にした participant（`enabled: false`、`--disable`）は、順 2 以降を照合しません。

**順 3 の補足**

| 項目 | ルール |
|---|---|
| 起動の順 | 1 つずつ順に起動します（並列にしません）。 |
| 作業ディレクトリと標準入力 | 作業ディレクトリは設定ファイルのあるディレクトリで、標準入力は空です。 |
| 時間の上限 | `--describe-timeout-ms`（既定 10000 ms）を過ぎたら止めます（出力を書き続ける実行ファイルも止めます）。 |
| その participant のエラーになるもの | 終了コードが 0 でない、時間内に終わらない、JSON でない、形式のバージョンが違う、のどれかです。 |
| シグナルや例外で終わったとき | 指摘の文に、そう書きます。 |
| 添えるもの | エラーには、標準エラーの最後の 10 行を添えます。 |
| 読む量 | 標準出力の最初の 1 MiB と、標準エラーの最後の 1 MiB です。 |
| 標準出力を切ったとき | `--describe` の出力のエラーに添える文に、そう書きます。 |
| 標準エラーを切ったとき | 末尾を添えるところに、そう書きます。 |

### 指摘のコード

各ルールには、出力に表示する固定の名前（コード。JSON では `code`）があります。重大度の「（読めない）」は、設定ファイルを読めないエラーで、終了コードが 3 になります。直し方は指摘の文にも入っているので、この表は、コードから意味を調べるのに使います。シナリオ（`--scenario`）の指摘のコードは、下の表の 1 つを除いて、[scenario.md](scenario.md) にあります。

| コード | 重大度 | 意味 | 直し方 |
|---|---|---|---|
| `syntax` | エラー（読めない） | YAML または JSON の文法のエラーです。 | 添えられた場所の文法を直します。 |
| `not-a-configuration` | エラー（読めない） | 空のファイルか、マッピングでないファイルです。 | `vehicle_sil_simulation: 1`、`networks`、`participants` を持つマッピングを書きます。 |
| `encoding` | エラー（読めない） | UTF-8 でないファイルです（UTF-16、NUL を含む）。 | UTF-8 で保存します。 |
| `unsupported-yaml` | エラー（アンカーとエイリアスは読めない） | アンカーとエイリアスか、2 つ目の文書（`---`）があります。 | 値を書き下し、2 つ目以降の文書は削除するか別のファイルにします。 |
| `format-version` | エラー（知らないバージョンは読めない） | `vehicle_sil_simulation` が 1 ではありません。 | `vehicle_sil_simulation: 1` と書くか、そのファイルの形式を読める vehicle-sil を使います。 |
| `file` | エラー（読めない） | ファイルがない、ディレクトリ、通常のファイルでない、大きすぎる、開けない、のどれかです。 | パスとファイルを確認します。 |
| `missing-key` | エラー | 必須のキーがありません。 | 指摘の文にあるキーを追加します。 |
| `unknown-key` | エラー | 知らないキーがあります。 | キーの名前を直します（指摘の文に、近い名前と書けるキーの一覧があります）。 |
| `duplicate-key` | エラー | 同じキーが 2 回あります。 | 片方を削除します。 |
| `value` | エラー | 型または範囲が違う値、使えない名前、実行ファイルのパスのプレースホルダー、`stop_at_ms` の約数でない `time_step_ms` のどれかです。 | 指摘の文に従って、値を直します。 |
| `duplicate-name` | エラー | participant 名、またはネットワークの名前が 2 回あります。 | 片方を削除するか、別の名前にします。 |
| `reserved-name` | エラー | participant 名が `SimulationController` です。 | 別の名前にします。 |
| `launcher-argument` | エラー | ecu_main の `command` に、`ecu_main` の引数かプレースホルダーがあります。 | それを削除するか、`ecu_main` で作っていない実行ファイルなら `describe: false` を書きます。 |
| `unknown-placeholder` | エラー | `describe: false` の `command` に知らない `{word}` があります。 | `{registry_uri}` か `{name}` に直すか、文字列として渡すなら `{{word}}` と書きます。 |
| `no-registry-uri` | 注記 | `describe: false` の `command` に `{registry_uri}` がありません。 | 実行ファイルが registry の URI を必要とするなら、その引数の値に `"{registry_uri}"` を書きます。 |
| `registry-on-network` | 注記 | `registry_uri` がループバックのアドレスではありません。 | この PC だけで動かすなら、`127.0.0.1` にするか、`registry_uri` を書きません。 |
| `listen-variable-ignored` | 注記 | `vehicle_sil_launch` 自身の環境の `VEHICLE_SIL_LISTEN_ON_NETWORK` が、`run` が participant に渡す値と違います（その値は使いません）。 | ほかの PC から参加させたいときだけ、`registry_uri` に、その PC から接続できるアドレスを書きます。 |
| `step-not-checked` | 注記 | `describe: false` の participant に `step_ms` があります。 | `step_ms` を削除するか、`ecu_main` の実行ファイルなら `describe: true` にします。 |
| `executable` | エラー | 実行ファイルがない（区切りのない名前は `PATH` にもない）、ディレクトリ、バッチファイル、実行の許可がない、のどれかです。 | `command` のパスを直すか、実行ファイルを置きます。 |
| `found-on-path` | 注記 | 実行ファイルを `PATH` で見つけました（その場所を表示します）。 | 表示された場所が、意図した実行ファイルかを確認します。 |
| `not-required-ecu` | エラー | `role: bridge` でない `describe: true` の participant に `required: false` があります。 | `required: false` を削除します（その ECU なしで動かすなら、`enabled: false` か `--disable`）。`role: bridge` は、自分で参加するブリッジのためのものです。`ecu_main` で作った ECU は、`role: bridge` を付けても、`required: false` では始まりません。 |
| `no-required-participant` | エラー | 無効にしていない participant がどれも `required: false` か、すべてを無効にしました（シミュレーションが待つ participant がない）。 | 必須（`required: true`）の participant を、1 つ以上有効にします。 |
| `describe-start` | エラー | 実行ファイルを起動できません。 | そのファイルが、この PC で動く実行ファイルかを確認します。 |
| `describe-timeout` | エラー | `--describe` が時間内に終わりません。 | ECU の組み立ての中で待っている処理（接続、入力）を直すか、`--describe-timeout-ms` を長くします。 |
| `describe-failed` | エラー | `--describe` の終了コードが 0 ではありません。4 は、ecu_main でない実行ファイルの見込みが高い。`role: bridge` の participant では、ブリッジが引数を断ったことを表します。Windows の終了状態 `0xC0000135` か `0xC0000139` は、要る DLL がない（または版が合わない）ため、実行ファイルが起動できなかったことを表します。 | 添えられた標準エラーの原因を直すか、`ecu_main` で作っていない実行ファイルなら `describe: false` を書きます。ブリッジなら `command` を直します（ランチャーが `--name` を足すので、`--participant` も `--name` も書きません）。DLL がないなら、実行ファイルの隣か `PATH` のフォルダーに置きます。この PC で使わないブリッジは `--disable <name>` で外せます。 |
| `describe-output` | エラー | `--describe` の出力が、`--describe` の JSON ではありません。 | ほかのものが標準出力に書かないようにするか、`ecu_main` で作っていない実行ファイルなら `describe: false` を書きます。 |
| `describe-version` | エラー | `--describe` の出力の形式のバージョン（`vehicle_sil_ecu`）を読めません。 | 指摘の文が言うほう（ECU か `vehicle_sil_launch`）を、新しくします。 |
| `describe-unknown-value` | エラー | `--describe` の出力の、決まった値をとるキー（`kind`、`use`、`type`、`causality`）に、この `vehicle_sil_launch` が知らない値があります（新しい vehicle-sil の ECU）。 | `vehicle_sil_launch` を、ECU と同じか、それより新しい vehicle-sil のものにします。 |
| `name-mismatch` | エラー | `--describe` の出力の `name` が participant 名と違います（`--name` を `ecu_main` に渡していない）。 | ECU の `main()` を、引数を全部 `ecu_main` に渡すように直します。 |
| `step-mismatch` | エラー | `step_ms` が、`--describe` の出力のステップ幅と違います（実時間で、ステップ幅のない ECU も）。 | `step_ms` を指摘の文の値に直すか、削除します。 |
| `unknown-network` | エラー | ECU が、`networks` にないネットワークを使います。 | 1 つのバスのつもりなら、指摘の文が言う `network_names` の行をその participant に書き、別のバスなら `networks` に追加します。 |
| `kind-mismatch` | エラー | ECU が、ネットワークを `networks` と違う種類（CAN、LIN、イーサネット）で使います。`network_names` で名前を替えたネットワークは、指摘の文に両方の名前を書きます: `'VehicleCAN' ('J1939_CAN' in the executable)`。 | `networks` の `kind` か、ECU の側のネットワークを直します。 |
| `address-conflict` | エラー | 同じネットワークの 2 つ以上の J1939 ノードが同じアドレスを求め、どれかが動けません。 | ノードごとに別のアドレスにします。指摘の文が、書く `j1939_nodes` の行か、このファイルでは変えられないこと（実行ファイルが `--j1939-node` を受け取れない、またはコードが 1 つのアドレスに 2 つのノードを置く）を言います。 |
| `address-shared` | 警告 | 同じアドレスを求めるノードのうち、NAME の最も小さいノードが取り、ほかはどれも別のアドレスへ移ります。 | どのノードがどのアドレスにいるかを決めたいなら、ノードごとに別のアドレスにします（指摘の文が、書く `j1939_nodes` の行を言います）。 |
| `name-conflict` | エラー | 同じネットワークで、2 つ以上の J1939 ノードの NAME が同じです。 | ノードごとに別の NAME にします。`j1939_nodes` で、片方の identity number を変えます（指摘の文が、書く行を言います）。 |
| `lin-masters` | エラー | 1 つの LIN のネットワークに master が 2 つ以上あります（同じ participant の 2 回も）。 | master を 1 つにします（ほかを slave にするか、`lin_master()` を 1 回だけ呼びます）。 |
| `lin-no-master` | エラー（`describe: false` の participant がいるときは注記） | LIN の slave がいて、master がいません。 | master になる participant を追加します。 |
| `ethernet-duplicate-mac` | エラー | 1 つのイーサネットのネットワークで、2 つ以上の participant が同じ MAC アドレスを `--describe` に出力しています。 | participant ごとに別のアドレスにします（`eth::ChannelOptions::mac` と、送信するフレームの送信元のアドレス）。 |
| `ethernet-mac-unknown` | 注記 | イーサネットのネットワークに、MAC アドレスを `--describe` に出力しない participant がいます。 | `ethernet()` にアドレスを渡す（`eth::ChannelOptions::mac`）か、アドレスが重ならないことを自分で確認します。 |
| `unused-network` | 警告（`describe: false` の participant がいるときは注記） | どの participant も使わないネットワークです。 | ネットワークを削除するか、ECU の側のネットワーク名を確認します。 |
| `single-participant` | 警告（`describe: false` の participant がいるときは注記） | participant が 1 つだけのネットワークです（送信したものを誰も受信しない）。 | そのネットワークのほかの participant を追加するか、ネットワークを削除します。 |
| `test-participant-talks-on-bridged-network` | 警告 | `role: test` で J1939 のノードを持つ participant と、ブリッジが、同じネットワークにいるかもしれません。 | その participant をそのネットワークでは受信だけにする（J1939 のノードを作らない）か、その実行ではブリッジを無効にします。 |
| `connect-network` | 誤り | `--connect` が、`networks` にないネットワークを指しています。 | 名前を直します（指摘の文が近い名前を出します）。名前に `=` を含むネットワークはつなげないので、名前を変えます。 |
| `connect-kind` | 誤り | `--connect` が LIN のネットワークを指しています（LIN をつなぐブリッジはありません）。 | その `--connect` を外します。 |
| `connect-target` | 誤り | `--connect` の先が、空、256 バイトより長い、UTF-8 でない、制御文字を含む、`-` で始まる、のどれかです。CAN のネットワークでは、`virtual:<n>` か `physical:<n>`（`<n>` は 1 から）でもありません。 | ブリッジの一覧（`--list-interfaces`、`--list-channels`）にある先か、`none` を書きます。 |
| `connect-name-taken` | 誤り | ファイルの participant が、つなぐネットワークのブリッジの名前（`bridge.<network>`）を持っています。 | その participant の名前を変えます。 |
| `connect-filter` | 誤り | つなぐイーサネットのネットワークに `external.ethertypes` も `external.all_frames` もなく、そこにいる ECU の MAC アドレスをランチャーが並べられません（出力しない ECU がいる、`role: test` でない `describe: false` の participant がいる、出力した ECU が 1 つもない。`role: test` の participant は数えません）。 | そのネットワークに `external.ethertypes` か `external.all_frames: true` を書く（`role: test` の participant のフレームも通すときも）か、そこにいる各 ECU に MAC アドレスを持たせます。 |
| `connect-tool-missing` | 誤り | つなぐネットワークのブリッジが、`vehicle_sil_launch` の隣にも、`PATH` の絶対パスのディレクトリにもありません。 | ブリッジをビルドし、`vehicle_sil_launch` の隣に置くか、そのディレクトリを `PATH` に入れます。ビルド済みのツールのアーカイブには、`bin/` の `vehicle_sil_launch` の隣に Ethernet のブリッジがあります。Vector XL の CAN のブリッジはありません。 |
| `connect-tool-failed` | 誤り | `--connect` の引数でのブリッジの `--describe` が失敗した（終了コード、時間切れ、説明でない出力）か、ブリッジが別の名前やネットワークを出力しました。 | 指摘の文にあるブリッジの行のとおりに直す（先、`external`）か、`vehicle_sil_launch` と同じ vehicle-sil のブリッジを使います。 |
| `connect-loop` | 誤り | 有効な `role: bridge` の participant が、つなぐネットワークを `--describe` に出力しています（2 つのブリッジの間でフレームが回ります）。 | どちらかを外します（`--disable <name>` か、その `--connect` を外す）。 |
| `connect-maybe-loop` | 警告 | 有効な `role: bridge` の participant が `describe: false` で、つなぐネットワークがあります（そのネットワークを中継するかもしれません）。 | そのネットワークを中継するなら、2 つのブリッジのどちらかを外します。 |
| `connect-not-real-time` | 警告 | つなぐネットワークがあり、速さが 1 ではありません（速さがない、つまり最速のときも）。`check` はファイルの `speed` で、`run` は実際に動かす速さ（`--speed`、なければファイルの値）で判断します。 | 速さ 1 で動かします（ファイルに `speed: 1`、または `run --speed 1`）。 |
| `connect-start-paused` | 警告 | `run` だけ: つなぐネットワークがあり、`--start-paused` があります。シミュレーションが止まっていても、バスの機器は動き続けます。 | シミュレーションを止めて調べるなら、`--connect` なしで動かします。 |
| `not-checked` | 注記 | `describe: false` の participant です（照合していない）。 | 対応は不要です。 |
| `participant-disabled` | 注記 | 無効にした participant です（`enabled: false`、`--disable`。照合も起動もしない）。 | 対応は不要です。 |
| `participant-manual` | 注記 | 自分で起動する participant です（`manual: true`、`--manual`。`run` は起動せず、コマンドを表示して待つ）。 | `run` が表示するコマンドで起動します。 |
| `version-mismatch` | 注記 | 実行ファイルをビルドした vehicle-sil のバージョンが、`vehicle_sil_launch` のバージョンと違います。 | 一緒に動かなければ、同じバージョンでビルドし直します。 |
| `coarse-time-step` | 警告 | ファイルに `speed`（数）と `time_step_ms` があり、ECU のステップがタイムステップの境目で始まりません。 | 指摘の文にある値を、`time_step_ms` に書きます。 |
| `control-link-not-offered` | 注記 | 変数のある ECU が、それをシミュレーションに公開すると `--describe` に出力していません（`--no-control`、`control` のない古い実行ファイル）。 | 変数を読み書きしたいなら、`--no-control` を削除するか、新しい vehicle-sil で ECU をビルドし直します。 |
| `control-link-version` | 警告 | 変数のある ECU が出力した、メッセージの形式のバージョンが `vehicle_sil_launch` と違うか、トランスポートが `silkit` ではありません。シミュレーションは動きますが、`run` はその ECU の変数を読み書きできません。どちらが古いかは文に出ます。 | ECU のほうが古ければ、`vehicle_sil_launch` の vehicle-sil でビルドし直します。新しければ、ECU と同じかそれより新しいバージョンの `vehicle_sil_launch` を使います。別のトランスポートなら、実行ファイルが `silkit` で変数を出すようにします。 |
| `variable-map-unknown` | エラー | `variables` の対応の `from` の変数が、実行ファイルにありません。 | `from` の名前を直します（指摘の文に、近い名前か、実行ファイルの変数の一覧があります）。 |
| `variable-map-type` | エラー | 対応の形が、変数の型に合いません。 | `scale`、`offset` は整数か実数の変数に、`map` は整数か列挙の変数に書きます。 |
| `variable-map-value` | エラー | `map` の値を、実行ファイルの変数が取れません（列挙の項目にない、範囲の外）。 | `map` の値を、変数が取れる値にします。 |
| `variable-map-collision` | エラー | 対応の名前が、実行ファイルのほかの変数の見える名前と同じか、同じ変数に対応を 2 つ書いています。 | 対応の名前を変えるか、2 つの対応の片方を削除します。 |
| `variable-map-absent` | エラー | `absent: true` と書いた名前の変数が、実行ファイルにあります。 | その `absent: true` を削除します。 |
| `variable-map-not-checked` | 注記 | `describe: false` の participant に `variables` があります（実行の前には比べられない）。 | 対応は不要です。 |
| `network-map-undeclared` | エラー | `network_names` のキーが `networks` にありません（キーはシミュレーションの名前、値は実行ファイルの名前）。 | `networks` にネットワークを追加するか、キーを直します。値のほうが `networks` にあれば、指摘の文は逆に書いた行を言います（[ネットワークの名前を合わせる](simulation-config.md#ネットワークの名前を合わせるnetwork_names)）。 |
| `network-map-collision` | エラー | `network_names` が、実行ファイルの 1 つのネットワークに 2 つの名前を付けています（1 つの値を 2 つのキーに書いた）。または、対応の後で、実行ファイルの 2 つのネットワークが同じ名前になります（替えた先の名前のネットワークを、実行ファイルが自分でも持っている）。 | 実行ファイルのネットワーク 1 つに名前を 1 つにします。機種でネットワークを別のバスへ移すときは、指摘の文が言う行（`VehicleCAN: VehicleCAN`）を、その機種に追加します。 |
| `network-map-unknown` | エラー | `network_names` の値の名前のネットワークを、実行ファイルが使っていません。 | 値を直します（指摘の文は、実行ファイルのネットワークの一覧と、近い名前を言います）。 |
| `network-map-unsupported` | エラー | `network_names` を書いた participant の実行ファイルが、`--map-network` を受け取れません（`--describe` の出力の `features` に `map-network` がない。前のバージョンの vehicle-sil で作った実行ファイル）。 | 実行ファイルを作り直すか、`networks` の名前をその実行ファイルの名前にして、ほかの participant の側に対応を書きます。 |
| `network-map-not-ascii` | エラー | Windows のランチャーだけ: `network_names` のキーか値に、ASCII でない文字があります（Windows の ECU の実行ファイルは、その `--map-network` を拒否する）。 | 対応を書くネットワークの名前を ASCII にします。 |
| `network-map-not-checked` | 注記 | `describe: false` の participant に `network_names` があります（照合できない）。 | ecu_main の実行ファイルなら対応は不要です。 |
| `j1939-node-undeclared` | エラー | `j1939_nodes` の行の `network` が `networks` にない、CAN のネットワークでない、または `network_names` が名前を替えたネットワークの、実行ファイルでの名前です。 | ノードの CAN のネットワークの、シミュレーションでの名前を書きます（指摘の文が、近い名前か、`network_names` が付けた名前を言います）。 |
| `j1939-node-duplicate` | エラー | 同じノード（同じ `network` と `from`）の `j1939_nodes` の行が 2 つ、または `from` のない行と同じネットワークのほかの行があります。何も変えない行も数えます。 | 1 つのノードの設定は 1 行に書きます。1 つのネットワークに 2 行以上あるときは、どの行にも `from` を書きます。 |
| `j1939-node-not-ascii` | エラー | Windows のランチャーだけ: `j1939_nodes` の行の `network` に ASCII でない文字があります（Windows の ECU の実行ファイルは、そのような `--j1939-node` を断ります）。 | `j1939_nodes` のあるネットワークは ASCII の名前にします。 |
| `j1939-node-not-checked` | 注記 | `describe: false` の participant に、何かを変える `j1939_nodes` があります（照合できない）。 | ecu_main の実行ファイルなら対応は不要です。 |
| `j1939-node-unsupported` | エラー | `j1939_nodes` のある participant の実行ファイルが `--j1939-node` を受け取れません（`--describe` の `features` に `j1939-node` がない: 前の vehicle-sil で作った実行ファイル）。 | この版以降の vehicle-sil で実行ファイルを作り直します。 |
| `j1939-node-unknown` | エラー | `j1939_nodes` の行がどのノードにも当たりません。`from` のアドレスにノードがない、そのネットワークにノードがない、その名前の J1939 のネットワークがない、のどれかです。 | `from`（実行ファイルのコードのアドレス。指摘の文が並べます）か `network` を直すか、行を消します。 |
| `j1939-node-ambiguous` | エラー | `from` のない行で、実行ファイルがそのネットワークに違うアドレスのノードを持っています。 | 実行ファイルのコードのアドレスで `from` を書きます（指摘の文が並べます）。 |
| `j1939-node-indistinct` | エラー | 行が選ぶアドレス（`from` があればそのアドレス、なければ全部のノードの同じアドレス）に、実行ファイルのコードがノードを 2 つ以上置いています。設定ファイルでは見分けられません。 | 行を消し、ECU を作る人に、ノードに別々のアドレスを付けるか、引数で受け取るように頼みます。 |
| `j1939-node-no-name` | エラー | NAME のないノードに、NAME の欄（`identity_number`、`function_instance`、`ecu_instance`）を書いています。 | その設定を消します。そのノードで変えられるのは `address` だけです。 |
| `j1939-node-any-address` | エラー | NAME が arbitrary address capable でないノードか、NAME のないノードに、`address: 254`（空いているアドレス）を書いています。 | 0..253 のアドレスを書きます。 |
| `j1939-node-changed` | 注記 | participant の `j1939_nodes` の行が、ノードのアドレスか NAME を変えます（participant ごとに 1 件）。 | 古い値に頼っている ECU がないかを確かめます（下の補足）。 |
| `variant-participant` | エラー | 機種（`variants`）の `participants` に、設定ファイルの `participants` にない名前があります。 | 名前を直すか、その機種にだけいる participant なら、`participants` に `enabled: false` で書きます。 |
| `scenario-nothing-checked` | 警告 | `--scenario` のシナリオの確認する手順（`expect`、`wait_until`）が、この設定（機種）では全部飛びます。 | 変数の名前が違うだけなら `variables` で対応させ、この機種向けでないシナリオなら、シナリオの `only` に対象の機種を書きます。 |
| `usage` | エラー | 引数のエラーです（`--json` のときの JSON だけ）。 | 標準エラーの使い方に従って、引数を直します。 |
| `internal` | エラー | `vehicle_sil_launch` 自身の想定外の失敗です（`--json` のときの JSON だけ）。 | 標準エラーの理由を確認します。 |

### コードごとの補足

| コード | 補足 |
|---|---|
| `unsupported-yaml` | 2 つ目の文書があるときも、最初の文書は読みます。 |
| `registry-on-network` | registry と participant がネットワークで待ち受け、この PC に接続できる誰でもシミュレーションに参加できます（[待ち受けるアドレス](simulation-run.md#待ち受けるアドレス)）。 |
| `listen-variable-ignored` | どの値を渡すかは、[待ち受けるアドレス](simulation-run.md#待ち受けるアドレス) にあります。 |
| `describe-version` | 指摘の文は、ECU の vehicle-sil のバージョン（出力にあれば）と `vehicle_sil_launch` のバージョン、新しくするのがどちらかを言います。 |
| `describe-unknown-value` | 指摘の文は、キーと値、知っている値、両方の vehicle-sil のバージョンを言います。 |
| `describe-unknown-value` | その participant だけのエラーで、ほかの participant は照合を続けます。 |
| `address-conflict` | 重大度の条件は、下の「J1939 のアドレスの重なり」にあります。 |
| `address-conflict`、`address-shared`、`name-conflict` | 1 つのネットワークの所見の直し方は、互いに違う空きのアドレスと identity number を勧めます。全部を一度に書けます。そのノードの `j1939_nodes` の行がすでにあれば（何も変えない行も）、直し方は、その行のほかの設定を残して値を書く形です。 |
| `test-participant-talks-on-bridged-network` | ブリッジとして扱うのは `role: bridge` の participant で、1 つもなければ、`role` を書いていない `describe: false` の participant です。 |
| `test-participant-talks-on-bridged-network` | その participant の送信とアドレスの要求（Address Claimed）が、実機のバスに出ます。 |
| `test-participant-talks-on-bridged-network` | 指摘の文は、ブリッジが確かにいるのか、いるかもしれないのかと、直し方を言います（[テスト用の participant とブリッジ](simulation-config.md#テスト用の-participant-とブリッジrole)）。 |
| `test-participant-talks-on-bridged-network` | `--connect` でつなぐネットワークにも、ブリッジがいます。そのときの指摘の文は、その participant を使う実行では `--connect` を外すように言います。 |
| `single-participant` | `--connect` でつなぐネットワークでは出ません。ブリッジを participant として数えます。participant のいないつなぐネットワークは、今までどおり `unused-network` です。 |
| `connect-*` | `--connect` のときだけです（[simulation-config.md](simulation-config.md#ネットワークを外につなぐexternal--connect)「ネットワークを外につなぐ」）。照合だけの誤りと警告で、照合はアダプターもチャンネルも開きません。`run` の照合にもあるので、警告は何かを起こす前に `run --strict` を失敗させます。ブリッジを動かしてからの `run` の指摘は `connect-failed` と `connect-lost` です（[simulation-run.md](simulation-run.md#バスを外につなぐ--connect)「バスを外につなぐ」）。 |
| `connect-filter` | 説明を読めない participant（`failed`、`skipped`）がいる間は出しません。先にその誤りを直します。無効の participant と、`role: bridge`、`role: test` の participant は数えません。既定の絞り込みが通すのは ECU のフレームだけです。 |
| `connect-tool-failed` | 指摘の文は、コマンドと、ブリッジの行（`[eth-bridge]` などの札の後が `error:` か `FAIL` で始まる最後の行、なければ最後の行）を出します。照合がブリッジを走らせるのは、そのネットワークの `--connect` にほかの誤りがなく、有効な participant の説明をすべて読めたときだけです（読めないと、通すフレームが決まらないため）。終了状態 `0xC0000135`、`0xC0000139`（DLL のために Windows がブリッジを起動しなかった）は、そう言います。`vehicle_sil_vector_xl_bridge` では、文が `vxlapi64.dll` を名指しします。 |
| `coarse-time-step` | `run` の指摘と同じ文です（[速度を指定して動かす](simulation-run.md#速度を指定して動かすspeed)の「タイムステップと ECU のステップ」）。 |
| `control-link-not-offered`、`control-link-version` | `run` は、その ECU の変数を読み書きできません（[ECU の変数との接続](simulation-run.md#ecu-の変数との接続control_link)）。 |
| `variable-map-*` | 対応の書き方は、[変数の名前と値を合わせる](simulation-config.md#変数の名前と値を合わせるvariables) にあります。 |
| `variable-map-not-checked` | 合わない対応は、実行の指摘 `variable-map-not-applied` になります。 |
| `network-map-unknown` | この行が原因の指摘は出しません。値に近い名前の実行ファイルのネットワーク（近い名前がなければ、キーと同じ種類のもの）の `unknown-network` と、キーのネットワークの `unused-network`、`single-participant`、`lin-no-master` です。ほかのネットワークの指摘は出ます。 |
| `network-map-unsupported` | ほかのルールは、対応を当てた名前で続けます。 |
| `j1939-node-ambiguous`、`j1939-node-indistinct` | 直す人が違います。`j1939-node-ambiguous` は設定ファイルで直せます（`from` を足す）。`j1939-node-indistinct` は ECU のコードでしか直せません。`from` は同じアドレスのノードを見分けられないためです。 |
| `j1939-node-*`（ノードに当たらない行のもの） | その participant のそのネットワークのノードは、`address-conflict`、`address-shared`、`name-conflict` に入れません（ファイルが意図した値がわからないため）。そこにいるほかの participant どうしの競合は、今までどおり出します。participant の `network_names` がそのネットワークから遠ざけている行（`network-map-unknown` が言う）と、実行ファイルの J1939 のネットワークが 1 つだけで、名前を替えておらず、その名前が `networks` にないとき（`unknown-network` が `network_names` の行を足すように言う）には、`j1939-node-unknown` を出しません。 |
| `j1939-node-unsupported` | ほかの規則は、行が与えるはずの値で見ます（照合が自分で当てます）。実行ファイルを作り直した後に残る指摘と同じものが出ます。その participant には `j1939-node-changed` を出しません。 |
| `j1939-node-changed` | 変わるノードごとに、コードの値と新しい値を並べます。照合は、古い値に頼っているものを見られません。古いアドレスをコードに書いているほかの ECU（宛先や、送り手の見分け）、古い NAME の全体で探すほかの ECU、ECU 自身のコードの定数（フィルター、送るデータの中の値）です。全部の行がコードと同じ値になる participant と、無効にした participant には出しません。コードで空いているアドレスを取るノードには、アドレスについての文を付けません。 |
| `network-map-not-checked` | 指摘の文は、その実行で `run` が追加する引数を書きます。引数は、選んだ機種と `--disable` を当てた後の設定で決まり、追加する引数のない participant には出しません。`run` は `--map-network` を追加するので、ecu_main でない実行ファイルは、起動のときに引数のエラーで終わります。 |
| `scenario-nothing-checked` | シナリオは、何も確認せずに通ります（[scenario.md](scenario.md) の「機種とパラメーター」）。 |
| `usage` | 機種を書いた設定ファイルに `--variant` がない、ない機種の名前、シナリオにない名前の `--param`、も含みます。 |

**`unknown-network`**: 指摘の文は、同じ種類のネットワークで、その participant が使わず、ほかの participant が使うものがあれば、それを名指しします（`networks` にあるものも、ないものも）。そのうえで、1 つのバスのつもりなら、その participant に書く `network_names` の行（`network_names: { J1939_CAN: VehicleCAN }`。向きは「シミュレーションの名前: 実行ファイルの名前」。実行ファイルは変えない）を、別のバスなら `networks` に追加することを言います。挙げたネットワークがどれも `networks` にないときは、先に名前を 1 つ決めて `networks` に追加することを言います。名指しするネットワークがなければ、`networks` に追加することを先に言います。`network_names` で名前を替えたネットワークは、替えた後の名前で比べます。

**`single-participant`**: `role: test` の participant は数えません。そのため、ECU が 1 つとテスト用の participant（受信するだけの spy など）のネットワークでも、テスト用の participant だけのネットワークでも出ます。同じ種類のほかのネットワークにも、別の ECU が 1 つだけいるときは、指摘の文はそのネットワークを挙げ、1 つのバスのつもりなら、片方の名前を `networks` から削除して、その ECU に書く `network_names` の行を言います。

**`ethernet-duplicate-mac`**: 指摘の文は、participant とアドレスを言います。同じアドレスをわざと使う設定（待機系と現用系など）では、1 つを残して、ほかの participant の `eth::ChannelOptions::mac` を渡さずにおきます。そうすると、`ethernet-mac-unknown` の注記だけになります。ただし、`mac` のないチャネルは `Accept::Addressed` を使えません。同じステップで送信したフレームの順は、SIL Kit では実行ごとに変わりえます。

**`ethernet-mac-unknown`**: `ethernet()` にアドレスを渡していない ECU がいると出ます。アドレスの重なりを確認できないことを伝える注記です。アドレスを報告しない `role: bridge` の participant（ブリッジはほかの participant のフレームを運ぶ）は、この注記と `ethernet-duplicate-mac` の対象になりません。

**`participant-manual`**: `registry_uri` がループバックのアドレスでないとき、この注記の文は、環境変数 `VEHICLE_SIL_LISTEN_ON_NETWORK=1` を設定して起動するように言います。この変数が必要なのは、ランチャーと同じ PC で、ループバックのアドレスの URI で起動するときだけです。ほかの PC で起動するときは、設定しません（[participant を自分で起動する](simulation-config.md#participant-を自分で起動するmanual--manual)）。注記の文は、どの PC で起動するかを区別しません。

**`unused-network`、`single-participant`、`lin-no-master`**: すべての participant の `--describe` の出力がそろったときだけ出ます。照合できなかった participant（`failed` と `skipped`）がいる間は出ません（その participant のエラーを先に直します）。`describe: false` の participant がいるときは注記です。その participant が使っているかもしれないためで、指摘の文にもそう書きます。

### J1939 のアドレスの重なり

アドレスは、ノードを追加したときに求めたアドレスです（[ecu-executable.md](ecu-executable.md) の `nodes`）。アドレスと NAME は、participant の `j1939_nodes` を当てた後の値です（[simulation-config.md](simulation-config.md) の「J1939 のアドレスと NAME を合わせる（`j1939_nodes`）」）。行から値の来たノードには、指摘の文で名前の後ろに `(set by j1939_nodes, line 14)` が付きます。254（任意アドレスの範囲から空いているものを取る）は、重なりとみなしません。同じアドレスを求めるノードについては、vehicle-sil の J1939 スタックの Address Claim で起きることを判定します。

| 同じアドレスを求めるノード | 起きること | 重大度 |
|---|---|---|
| NAME のないノードがある | NAME のないノードは Address Claim をせず、そのアドレスから送信し続ける | エラー |
| NAME の最も小さいノードが 2 つ以上ある（同じ NAME） | 同じ NAME は Address Claim に負けるので、どれもアドレスを取れない | エラー（`name-conflict` が、同じアドレスであることも書きます） |
| どれも NAME があり、NAME の最も小さいノード以外に、任意アドレスを取れない（NAME の任意アドレスの bit がない）ノードがある | NAME の最も小さいノードが取り、取れないノードはアドレスを失う（Cannot Claim） | エラー |
| どれも NAME があり、NAME の最も小さいノード以外は、どれも任意アドレスを取れる | NAME の最も小さいノードが取り、ほかは空いたアドレスへ移る | 警告 |

`address-shared` の「別のアドレスへ移る」は、移る先が空いていることを前提にします。NAME のないノードは Address Claim をしないので、任意アドレスの範囲にそのノードがいると、移ったノードと重なることがあります。照合は移る先を決めないので、これは確認しません。

`networks` にないネットワークの上の、アドレスと NAME の重なりは確認しません（先に `unknown-network` を直します）。種類を読めなかったネットワークも確認しません。

### ネットワークを使う participant とイーサネットのアドレス

ネットワークを使う participant は、名前と種類の両方が合うものです（表と JSON の `networks[].participants` も同じです）。

名前は、participant の `network_names` を当てた後の名前（シミュレーションの名前）で比べます。アドレスの重なり、LIN の master など、ネットワークのルールは全部この名前で動きます。

`network_names` に設定ファイルだけで分かるエラー（`network-map-undeclared`、1 つの値を 2 つのキーに書いた `network-map-collision`、形のエラー `value`、`network-map-not-ascii`）がある participant は、照合しません（`skipped`。意図と違う名前で比べないためです）。`network_names` のうち、キーと値が同じ行は、どのルールも見ません。

イーサネットの MAC アドレスの扱いは次のとおりです。

| 項目 | ルール |
|---|---|
| 照合する値 | ECU が `ethernet()` に渡して `--describe` に出力した値です（[ecu-executable.md](ecu-executable.md) の `networks[].mac`）。 |
| 実際のフレーム | 実際に送信するフレームの送信元のアドレスは見ません。 |
| 別のネットワーク | 同じアドレスでも、別のネットワークなら重なりとみなしません。 |
| 表での表示 | ネットワークの欄にアドレスを書きます（`ETH (ethernet 02:00:00:00:00:01)`。出力がなければ `ETH (ethernet)`）。 |

## 出力

標準出力に、人向けの表（既定）か JSON（`--json`）を書きます。同じ設定ファイルと実行ファイルからは、同じ出力になります。

| 項目 | ルール |
|---|---|
| 改行 | どの OS でも LF です（Windows でも `\r\n` にしません）。 |
| 設定ファイルの名前 | 引数に書いたとおりに書きます。 |
| 照合が組み立てたパス（探した実行ファイルなど） | Windows でも `/` で区切って書きます。 |

### 人向け

上から順に、次の 4 つを書きます。

| 部分 | 内容 |
|---|---|
| participant の表 | `--describe` に出力されたステップ幅とネットワークです。 |
| ネットワークの表 | ネットワークを使う participant です。`--connect` があると、4 列目 `outside` に、つなぐネットワークの先を出します（ほかは `-`）。 |
| 指摘 | 1 つが 1 行で、エラー、警告、注記の順に、それぞれ行の順に並びます。 |
| 結果の 1 行 | エラー、警告、注記の数です。 |

participant の表では、J1939 のネットワークにノードの求めたアドレスを書きます（`any` は 254）。`network_names` で名前を替えたネットワークは、`VehicleCAN <- J1939_CAN (j1939 0x00)` と、シミュレーションの名前、実行ファイルの名前の順に書きます。`j1939_nodes` でアドレスが変わったノードは、同じ向きで `0x01<-0x00`（矢印の前後に空白なし）と、シミュレーションでのアドレス、コードのアドレスの順に書きます。NAME だけの変化は表に出ません（注記 `j1939-node-changed` が言います）。`role` が `ecu` でない participant は、名前の後に `(role: test)` などと書きます。表の幅は文字の数でそろえるので、全角の文字があるとずれます。

指摘の行は、`error:`、`warning:`、`note:` で始まり、`<ファイル>:<行>: <何が、どの値で、どう直すか> [<コード>]` の形です。

| 行の部分 | 内容 |
|---|---|
| `<ファイル>` | 引数に書いたとおりのファイル名です。 |
| `<行>` | 行のない指摘では、ファイル名だけになります。 |
| `[<コード>]` | 指摘のコードです（上の表と、JSON の `code` と同じ）。 |
| 次の行から 4 文字下げた文 | 添える文です（標準エラーの末尾、文法のエラーの場所など）。 |

コードで、このドキュメントの説明を探せます。コードを行の終わりに置くのは GCC の `[-W...]` と同じ置き方で、行の先頭は `error:` などのままです。

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

エラーのある例です。同じ ECU を 2 つ載せ、ステップ幅とキーを書き間違えています。

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

error: two-engines.yaml:4: network 'J1939_CAN': Engine and Engine2 have the same J1939 NAME 0x00000000FE000001, and they ask for the same address 0x00 (0), which none of them keeps (an equal NAME loses the address claim): each node needs its own NAME; to do it in this file, write under participant 'Engine2': j1939_nodes: [{ network: J1939_CAN, address: 0x01, identity_number: 2 }] [name-conflict]
error: two-engines.yaml:11: participant 'Engine': step_ms is 20, but the executable's step is 10 ms: write step_ms: 10, or remove step_ms [step-mismatch]
error: two-engines.yaml:16: participant 'Dashboard': unknown key 'requried': did you mean 'required'? (the keys here: name, command, step_ms, describe, required, enabled, manual, role, variables, network_names, j1939_nodes) [unknown-key]
warning: two-engines.yaml:6: network 'Body_LIN': no participant uses it: remove it, or check the network names in the ECUs [unused-network]

failed: 3 errors, 1 warning, 0 notes
```

`name-conflict` は、示された `j1939_nodes` の行を `Engine2` に書くとなくなります。手順は [複数の ECU をつないで動かす](howto/integrate-ecus.md#同じ-ecu-を-2-つ載せる) の「同じ ECU を 2 つ載せる」にあります。

### JSON（`--json`）

1 つの JSON のオブジェクトです。形は `--describe` の JSON と同じルールです（キーの順は固定、値がなくてもキーを省かない、字下げは空白 2 つ、改行は Windows でも LF）。値のないものは `null` で（空の文字列にしません）、配列は空の配列です。

先頭の `vehicle_sil_check` は形式のバージョンです。バージョンが上がるのは、あるキーか `code` の意味が変わるとき、またはなくなるときです。キーと、`code`、`severity`、`result`、`participants[].status` の値は、バージョンを上げずに増えることがあります。読む側は、知らないものを次のように扱います。

| 知らないもの | 読む側の扱い |
|---|---|
| バージョン | 拒否します。 |
| キー | 無視します。 |
| `code` | その `severity` で扱います。 |
| `severity` | `"error"` として扱います。 |
| `result` | 成功でないものとして扱います（終了コードを見ます）。 |
| `status` | 照合できなかったものとして扱います。 |

照合に進まないときも、`--json` があれば JSON を書きます。

| 場合 | 終了コード | JSON |
|---|---|---|
| 設定ファイルを読めない | 3 | `result` が `"unreadable"` で、`participants` と `networks` は空です。 |
| 引数のエラー | 4 | `result` が `"usage"` で、`findings` に `code` が `usage` のエラーがあります。 |

引数のエラーのときの `file` は、引数から設定ファイルがわかればその名前で、わからないとき（ファイルがない、2 つ以上など）は `null` です。

全体のキーは次のとおりです。

| キー | 値 |
|---|---|
| `vehicle_sil_check` | 形式のバージョン（今は 1） |
| `vehicle_sil` | `vehicle_sil_launch` の vehicle-sil のバージョン |
| `file` | 設定ファイル（引数に書いたとおり） |
| `result` | `"passed"`、`"failed"`、`"unreadable"`、`"usage"` |
| `strict` | `--strict` を付けたか |
| `errors`、`warnings`、`notes` | エラー、警告、注記の数 |
| `variant` | 選んだ機種（`--variant`）。機種を書いた設定ファイル（`variants`）のときだけあります |
| `variants` | 設定ファイルにある機種の名前の配列。機種を書いた設定ファイルのときだけあります |
| `params` | 共通の `params` に、選んだ機種の `params` を重ねたもの。設定ファイルに `params` があるときだけあります |
| `stop_at_ms`、`start_timeout_ms`、`stop_timeout_ms`、`registry_uri` | 設定ファイルの値 |
| `participants[]` | participant の配列（設定ファイルの順） |
| `networks[]` | ネットワークの配列（設定ファイルの順） |
| `findings[]` | 指摘の配列（人向けと同じ順） |

- `--variant` がない、または名前が違うとき（`result` が `"usage"`）は、`variant` が `null` で、`variants` に選べる名前が出ます。
- `params` の要素は、`name`、`value`（`true`・`false`、数、文字列）、`from`（`configuration` か `variant`）です。
- 設定ファイルを読めないときは、`stop_at_ms`、`start_timeout_ms`、`stop_timeout_ms`、`registry_uri` が `null` です。
- 書いていない `stop_at_ms` と `registry_uri` は `null` で、書いていない `start_timeout_ms` と `stop_timeout_ms` は既定の値です。

`participants[]` の要素のキーは次のとおりです。

| キー | 値 |
|---|---|
| `name` | participant 名（読めなかったときは `null`） |
| `line` | 設定ファイルの行 |
| `command` | 実行ファイルと引数 |
| `describe`、`required` | 設定ファイルの値 |
| `enabled` | `--disable` と `--enable` を反映した後の値 |
| `manual` | `--manual` を反映した後の値 |
| `role` | `"ecu"`、`"test"`、`"bridge"` |
| `step_ms` | 設定ファイルの値（なければ `null`） |
| `status` | 照合の状態（下の表） |
| `reported` | `--describe` の出力（`status` が `described` のときだけ。ほかは `null`） |
| `variables` | シミュレーションから見える ECU の変数の配列（`ecu.connected`、続いて宣言の順） |
| `variable_map` | 設定ファイルの `variables` に書いた対応の配列（書いた順）。`variables` を書いた participant だけにあります |
| `network_map` | 設定ファイルの `network_names` に書いた行の配列（書いた順。機種の行を重ねた後）。`network_names` のない participant では `null` です |
| `j1939_nodes` | 設定ファイルの `j1939_nodes` に書いた行と、各行が当たったノードの配列（書いた順。機種の行を重ねた後）。`j1939_nodes` のない participant では `null` です |
| `control` | ECU が `--describe` に出力した、変数の公開の方法 |

| `status` | 意味 |
|---|---|
| `"described"` | `--describe` の出力を読んだ |
| `"not_checked"` | `describe: false` |
| `"failed"` | 実行ファイルがない、`--describe` の失敗 |
| `"skipped"` | 設定ファイルのエラーで進めなかった |
| `"disabled"` | 無効にした（項目にエラーがあっても） |

**`reported`**: キーは `vehicle_sil`、`name`、`step_ns`、`networks` です（`--describe` の JSON と同じ形）。ネットワークの名前は実行ファイルの名前のままで、`network_names` を当てる前です。ノードのアドレスと NAME も、実行ファイルのコードの値のままで、`j1939_nodes` を当てる前です。変数は、次の `variables` にあります。

**`variables`**: 設定ファイルの `variables`（対応表）を反映した後の名前、型、範囲、単位です。シナリオと Control API が見るものと同じです。対応表がなければ、ECU が出力したままです。要素の形は `--describe` の JSON の `variables[]` と同じです（`name`、`type`、`causality`、`unit`、`min`、`max`、`step`、`description`、`items`、`builtin`。[ecu-executable.md](ecu-executable.md) の「JSON の形式」）。実行の前に、どの ECU にどの変数があるかを見るのに使います。

| `variables` の値 | 意味 |
|---|---|
| 空の配列 | 変数が 0 個です。 |
| `null` | わかりません。 |

`null` になるのは、`--describe` の出力のない participant（`describe: false`）、無効にした participant、`--describe` の出力を読めなかった participant、`variables` を書かない古い vehicle-sil の ECU です。

**`variable_map`**: 読めなかった対応（エラー `value` など）は含みません。要素のキーは次のとおりです。

| キー | 値 |
|---|---|
| `name` | シミュレーションで使う名前 |
| `line` | 設定ファイルの行 |
| `from` | 実行ファイルの変数名（`absent` なら `null`） |
| `absent` | `absent: true` と書いたか |
| `scale`、`offset` | 一次式の値（一次式の形でなければ両方 `null`） |
| `unit` | 単位（書いていなければ `null`） |
| `map` | `name` と `value` の配列（なければ `null`） |

`scale` と `offset` の片方だけを書いたときは、もう片方が既定の 1 か 0 になります。

**`network_map`**: 要素は `name`（シミュレーションの名前）、`line`（設定ファイルの行）、`from`（実行ファイルの名前）です。キーと値が同じ行も含みます。読めなかった行（エラー `value`）は含みません。

**`j1939_nodes`**: 読めなかった行（エラー `value` など）は含みません。要素のキー:

| キー | 値 |
|---|---|
| `network` | ネットワークのシミュレーションでの名前 |
| `line` | 設定ファイルの行 |
| `from`、`address`、`identity_number`、`function_instance`、`ecu_instance` | 書いた値（数。書いていなければ `null`） |
| `node` | 行が当たったノード: `coded_address`（実行ファイルのコードのアドレス。`reported` の `address` と同じ）、行を当てた後の `address`、`j1939_name`、`arbitrary_address_capable`（`reported` のノードと同じ形）。どのノードにも当たらない行、何も変えない行、照合していない participant では `null` |

**`control`**: キーは `transport`（今は `"silkit"`）と `wire_version`（メッセージの形式のバージョン）です。`control` を出力していない（`--no-control`、古い実行ファイル）、照合していない、失敗した、無効にした participant では `null` です。

`networks[]` と `findings[]` の要素のキーは次のとおりです。

| キー | 値 |
|---|---|
| `networks[].name` | ネットワークの名前 |
| `networks[].kind` | 種類（読めなかったときは `null`） |
| `networks[].line` | 設定ファイルの行 |
| `networks[].participants` | ネットワークを使う participant 名（設定ファイルの順。`network_names` を当てた後の名前で数える） |
| `networks[].outside` | `--connect` でつなぐネットワークでは、そのブリッジです（下のキー）。ほかのネットワークでは `null` です（ブリッジを探さないので、出力がその PC の道具で変わりません） |
| `networks[].outside.tool` | ブリッジの実行ファイル（`vehicle_sil_ethernet_bridge`、`vehicle_sil_vector_xl_bridge`） |
| `networks[].outside.tool_path` | 見つけた場所（見つからなければ `null`） |
| `networks[].outside.target` | `--connect` に渡した先 |
| `networks[].outside.participant` | ブリッジの participant 名（`bridge.<network>`） |
| `networks[].outside.command` | ブリッジと、ランチャーが渡す引数（`--describe` と `--name` は除く）。そのネットワークをつなぐことについての誤り（`connect-*`）があるとき、または説明を読めない participant がいる（通すフレームが決まらない）ときは `null` |
| `findings[].severity` | `"error"`、`"warning"`、`"note"` |
| `findings[].code` | 指摘のコード |
| `findings[].file` | 設定ファイル（`--scenario` のシナリオについての指摘では、シナリオのファイル） |
| `findings[].line` | 行（なければ `null`） |
| `findings[].participant`、`findings[].network` | 関係する participant とネットワーク（なければ `null`） |
| `findings[].message` | 指摘の文（1 行） |
| `findings[].detail` | 添える文（なければ `null`） |

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
      "role": "ecu",
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
    }
  ],
  "networks": [
    {
      "name": "J1939_CAN",
      "kind": "can",
      "line": 3,
      "participants": [
        "Engine"
      ],
      "outside": null
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

このドキュメントの設定ファイルと出力の例は、CTest の `launch_cli.docs` が、実際の実行ファイルと照合します（`vehicle_sil` のバージョンを除いて、バイト単位で）。

## 関連するドキュメント

| 調べること | ドキュメント |
|---|---|
| 設定ファイルのキー、引数、終了コード | [simulation-config.md](simulation-config.md) |
| `--describe` の JSON | [ecu-executable.md](ecu-executable.md) |
| 実行と、実行の指摘 | [simulation-run.md](simulation-run.md) |
| 指摘を見つけて直す手順 | [複数の ECU をつないで動かす](howto/integrate-ecus.md) |
