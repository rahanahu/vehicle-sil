# 複数の ECU をつないで動かす

[English](../../howto/integrate-ecus.md)

何人かが別々に作った ECU の実行ファイルを、1 つの設定ファイルに並べて、1 つのシミュレーションとして動かす手順です。設定ファイルと ECU の不整合を `check` で見つけ、設定ファイルで直せないものは ECU を作った人に依頼し、`run` で実行して、ECU の間で通信できたことを確認します。ECU のソースコードは読みません。

例は 2 つの ECU です。エンジンの ECU は回転数を J1939 で送信し、表示の ECU はそれを受信します。最初に受け取った表示の ECU は、同じバスをエンジンと違う名前で呼んでいます。この不整合を見つけて直すまでを、手順 1〜5 で進めます。例のファイルは [examples/integrate_ecus](../../../examples/integrate_ecus) にあります。

## 前提

| 必要なもの | 条件 |
|---|---|
| ビルド | [README](../../../README.ja.md) の「リポジトリからビルドする」のとおりに、リポジトリをビルドしてあります。 |
| ECU の実行ファイル | `ecu_main()` で作ったものです（作り方は [最初の ECU を作る](first-ecu.md)）。 |

コマンドは、すべて次のディレクトリで実行します。リポジトリのルートで、次の 2 行を実行してください。`linux-gcc-control` は README のプリセットで、ほかのプリセットでビルドしたときは、その名前に読み替えます。

```sh
export PATH="$PWD/build/linux-gcc-control/tools/launch:$PATH"   # vehicle_sil_launch のある場所
cd build/linux-gcc-control/examples/integrate_ecus
```

このディレクトリには、ビルドのときに次のファイルが置かれます。

| ファイル | 内容 | 使う手順 |
|---|---|---|
| `simulation.yaml` | 最初に書く設定ファイルです。 | 2、3 |
| `simulation.two-buses.yaml` | `networks` に、表示の ECU が使うネットワーク名を追加したものです。 | 3、5 |
| `simulation.fixed.yaml` | 表示の ECU を、作り直したものに差し替えたものです。 | 4、5 |
| `scenarios/engine_speed.yaml` | 表示の ECU が回転数を受信することを確認するシナリオです。 | 5 |
| `../ecu_main/engine/vehicle_sil_engine_ecu` | エンジンの ECU です。 | 2〜5 |
| `dashboard_v1/vehicle_sil_dashboard_v1_ecu` | 最初に受け取った表示の ECU です。 | 2、3 |
| `../ecu_main/dashboard/vehicle_sil_dashboard_ecu` | 作り直した表示の ECU です。 | 4、5 |

テンプレートとビルド済みのツールを使うときは、場所が次のように変わります。手順は同じで、自分が受け取った ECU で進めます。

| | リポジトリをビルドしたとき（このガイド） | テンプレートとビルド済みのツール |
|---|---|---|
| 実行するディレクトリ | `build/<プリセット>/examples/integrate_ecus` | 自分の `simulation.yaml` のあるディレクトリ |
| `vehicle_sil_launch` | `build/<プリセット>/tools/launch/` | `get_tools.py` が表示する `export PATH=...` の行で `PATH` に追加します。 |

## 全体の流れ

| 手順 | すること | できるもの |
|---|---|---|
| 1 | ネットワーク名などを決めて、ECU を作る人に伝える | 名前の一覧 |
| 2 | 実行ファイルを集めて、設定ファイルを書く | `simulation.yaml` |
| 3 | `check` で照合して、指摘を読む | 指摘ごとの、直す場所 |
| 4 | 設定ファイルで直せない不整合を、ECU を作った人に依頼する | 作り直した実行ファイルと、指摘のない `check` |
| 5 | `run` で実行して、通信できたことを確認する | 終了コード 0 と、受信した値 |

手順 1〜5 は順に進めます。その後の節は、必要になったときに読みます。

| したいこと | 使うもの | 読む節 |
|---|---|---|
| まだ受け取っていない ECU を除いて動かす | `--disable` | [ECU を無効にして動かす](#ecu-を無効にして動かすenabled--disable) |
| 途中で止める、止まらない実行に上限を付ける | Ctrl+C、`--timeout-ms` | [止める](#止める) |
| スクリプトや CI から結果を読む | `--json`、`--events` | [スクリプトから使う](#スクリプトから使う--json--events) |
| 実時間に合わせて動かす | `--speed` | [等速で動かす](#等速で動かす--speedと遅れの見方) |
| 同じ ECU を 2 つ並べる | `command` の引数 | [同じ ECU を 2 つ載せる](#同じ-ecu-を-2-つ載せる) |
| `ecu_main()` で作っていないツールを一緒に起動する | `describe: false` | [`ecu_main()` で作っていないツールを並べる](#ecu_main-で作っていないツールを並べる) |

## 手順 1: 名前を決めて伝える

ECU を作り始める前に、次の表のものを決めて、ECU を作る人の全員に伝えます。迷ったときの選び方は、[作り始める前に決めること](design-choices.md) にあります。どれも ECU のコードに書く値なので、実行ファイルを受け取った後で合わなければ、作り直しになります。

| 決めるもの | ルール | 合わないときの指摘のコード |
|---|---|---|
| ネットワーク名と種類 | 同じバスに接続する ECU は、全員が同じ名前と種類（`can`、`lin`、`ethernet`）を使います。 | `unknown-network`、`kind-mismatch`、`single-participant` |
| J1939 のアドレス | 同じネットワークの中で重ならないようにします。 | `address-conflict`、`address-shared` |
| J1939 の NAME | ECU ごとに違う値にします。 | `name-conflict` |
| ステップ幅 | ECU を作る人が決めて、設定ファイルを書く人に伝えます。 | `step-mismatch` |

ECU は、コードの中で `ecu.j1939("J1939_CAN")` のように名前を指定してバスに接続します。2 つの ECU が 1 つのバスを違う名前で呼んでいるときは、設定ファイルの `network_names` で、片方の ECU の名前をもう片方の名前に対応させられます（作り直しは要りません。[simulation-config.md](../simulation-config.md) の「ネットワークの名前を合わせる」）。名前を事前に決めておけば、どの設定ファイルにも、この対応は要りません。

例では、次のように決めたとします。

| ECU | ネットワーク | J1939 のアドレス | ステップ幅 |
|---|---|---|---|
| エンジン | `J1939_CAN`（CAN） | 0x00 | 10 ms |
| 表示 | `J1939_CAN`（CAN） | 0x28 | 5 ms |

ECU を作る人には、名前のほかに次のことも依頼します。

| 依頼すること | 守らないと起きること |
|---|---|
| `main()` を `ecu_main()` で作ります。 | `check` がその ECU を照合できません。 |
| 決めた名前を使い、渡す前に `--describe` で確認します（手順 2）。 | 手順 3 の指摘になります。 |
| 設定ファイルを書く人と同じバージョンの vehicle-sil でビルドします。 | `version-mismatch` の注記が出ます。変数を読み書きする形式のバージョンが違うと、警告 `control-link-version` が出て、その ECU の変数を読み書きできません。 |
| ECU が自分で起動したプロセスは、ECU が終わるときに終了させます。 | 警告 `left-processes` が出ます。 |
| 標準出力と標準エラーは UTF-8 で出力します。 | UTF-8 でないバイトは `?` と表示されます。 |

participant 名（シミュレーションの中での ECU の名前）は、設定ファイルに書くだけなので、ECU を作る人と合わせる必要はありません。`vehicle_sil_launch` が、設定ファイルの名前を `--name` で ECU に渡します。

## 手順 2: 設定ファイルを書く

受け取った実行ファイルを 1 つのディレクトリの下に置き、同じ場所に `simulation.yaml` を書きます。リポジトリの例では、ビルドが作業ディレクトリに ECU とこの `simulation.yaml` を置いているので、書かずに読みます。例の `simulation.yaml` は次のとおりです。

<!-- examples/integrate_ecus/simulation.yaml -->
```yaml
# yaml-language-server: $schema=../../schema/simulation.schema.json
# Two ECUs received from their authors, in one simulation (the guide docs/howto/integrate-ecus.md).
vehicle_sil_simulation: 1
stop_at_ms: 1000
networks:
  - name: J1939_CAN
    kind: can
participants:
  - name: Engine
    command: [../ecu_main/engine/vehicle_sil_engine_ecu]
    step_ms: 10
  - name: Dashboard
    command: [dashboard_v1/vehicle_sil_dashboard_v1_ecu]
    step_ms: 5
```

| 行 | 書くもの |
|---|---|
| 1 行目のコメント | エディターが補完と検査に使う JSON Schema の場所で、なくても動きます。 |
| `vehicle_sil_simulation` | 設定ファイルの形式のバージョンで、`1` と書きます。 |
| `stop_at_ms` | シミュレーションを止める仮想時刻で、省略すると Ctrl+C まで動きます。 |
| `networks` | 手順 1 で決めたネットワーク名と種類を、1 つずつ書きます。 |
| `participants[].name` | participant 名で、シミュレーションの中で重ならないようにします。 |
| `participants[].command` | 実行ファイルのパスで、相対パスは設定ファイルのあるディレクトリを基準にします。 |
| `participants[].step_ms` | 手順 1 で伝えてもらったステップ幅で、書くと ECU の値と照合し、省略もできます。 |

`command` に `--name` や `--registry-uri` は書きません。`vehicle_sil_launch` が追加します。Windows では、`.exe` を書かなくても実行ファイルが見つかります。キーの一覧と既定の値は、[simulation-config.md](../simulation-config.md) の「設定ファイル」にあります。

### ECU が使う名前を調べる

ECU がどのネットワーク名を使うかは、ソースコードを読まなくても、実行ファイルに `--describe` を付けて実行すればわかります。ECU は、自身の構成を JSON で標準出力に表示して終了します。

```sh
dashboard_v1/vehicle_sil_dashboard_v1_ecu --describe
```

次は、出力のうち、手順 1 で決めたものに対応する部分です。

<!-- integrate ECUs: vehicle_sil_dashboard_v1_ecu --describe, an excerpt -->
```json
{
  "name": "Dashboard",
  "step_ns": 5000000,
  "networks": [
    {
      "name": "VehicleCAN",
      "kind": "can",
      "use": "j1939",
      "nodes": [
        {
          "address": 40
        }
      ]
    }
  ]
}
```

| キー | 意味 | この例の値 |
|---|---|---|
| `networks[].name`、`kind` | ECU が使うネットワーク名と種類です。 | `VehicleCAN`、CAN |
| `networks[].nodes[].address` | J1939 のアドレスです（10 進）。 | 40（0x28） |
| `step_ns` | ステップ幅です（ナノ秒）。 | 5 ms |

ネットワーク名が `VehicleCAN` で、手順 1 で決めた `J1939_CAN` と違います。この不整合は ECU の作者に直してもらうもので、依頼の書き方は手順 4 にあります。次は、設定ファイルをそのままにして手順 3 に進み、`check` がこの不整合をどう報告するかを見ます。JSON のすべてのキーは、[ecu-executable.md](../ecu-executable.md) の「JSON の形式」にあります。

## 手順 3: 照合して指摘を読む

```sh
vehicle_sil_launch check simulation.yaml
```

`check` は、各 ECU を `--describe` で 1 つずつ起動して、設定ファイルと照合します。シミュレーションは実行しません。ECU のコンストラクターは動くので、信頼できない実行ファイルや設定ファイルは渡さないでください。

### 確認

例では、指摘が 2 件出て、最後の行が `failed:` になります。終了コードは 1 です。

<!-- integrate ECUs: vehicle_sil_launch check simulation.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   VehicleCAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine

error: simulation.yaml:12: participant 'Dashboard': the executable uses the network 'VehicleCAN' (j1939), which is not in networks. Another network of kind can in this simulation is 'J1939_CAN' (line 6, used by Engine): if 'VehicleCAN' is meant to be that bus, say so in this file: add network_names: { J1939_CAN: VehicleCAN } to the participant 'Dashboard' (<simulation's name>: <executable's name>; the executable stays as it is); if 'VehicleCAN' is a bus of its own, add it to networks with kind: can [unknown-network]
warning: simulation.yaml:6: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it [single-participant]

failed: 1 error, 1 warning, 0 notes
```

出力は、上から次の 4 つの部分でできています。

| 部分 | 内容 |
|---|---|
| participant の表 | 各 ECU が `--describe` で出力した、ステップ幅、ネットワーク名、J1939 のアドレスです（設定ファイルの値ではありません）。 |
| network の表 | 設定ファイルの `networks` と、それぞれを使う participant です。 |
| 指摘 | 1 行に 1 件で、`<重大度>: <ファイル>:<行>: <内容と直し方> [<コード>]` の形です。 |
| 最後の行 | 結果（`passed` か `failed`）と、重大度ごとの件数です。 |

指摘の重大度は 3 つです。

| 重大度 | `check` の結果 | `run` |
|---|---|---|
| `error` | 失敗です（終了コード 1）。 | 何も起動しません。 |
| `warning` | 成功のままです（`--strict` を付けると失敗）。 | 実行します。 |
| `note` | 成功のままです。 | 実行します。 |

例の `error` の行は、表示の ECU が使う `VehicleCAN` が `networks` にないことを報告しています。エンジンの回転数を表示の ECU が受信するので、2 つの ECU は同じバスにいなければなりません。

`error` の行が直し方を 2 つ示すのは、`VehicleCAN` がどのバスのつもりかを `check` が知らないからです。

| `VehicleCAN` が | 直し方 | 直す場所 |
|---|---|---|
| `J1939_CAN` と同じバスのつもり | 表示の ECU の名前を `J1939_CAN` に対応させます。participant `Dashboard` に `network_names: { J1939_CAN: VehicleCAN }` を追加します。または、2 つの ECU の名前をそろえます。 | 設定ファイル（[simulation-config.md](../simulation-config.md) の「ネットワークの名前を合わせる」）か、ECU のコード（手順 4） |
| 別のバス | `networks` に `VehicleCAN` を追加します。 | 設定ファイル |

2 つ目の直し方では `check` は通りますが、2 つの ECU は別のバスに分かれ、通信しません（下の「`networks` に名前を追加した場合」）。

このガイドでは、対応を書かずに、決めた名前にそろえます。手順 4 で、表示の ECU を作った人がコードの名前を変えます。対応を書く場合は、いまの実行ファイルのまま `check` が通り、participant の表の `Dashboard` の行は `J1939_CAN <- VehicleCAN (j1939 0x28)` になります。手順 4 は要りません。

### `networks` に名前を追加した場合

`error` を消すだけなら、`networks` に `VehicleCAN` を追加すれば消えます。`simulation.two-buses.yaml` は、`simulation.yaml` の `networks` を次のように変えたものです。

<!-- examples/integrate_ecus/simulation.two-buses.yaml: networks -->
```yaml
networks:
  - name: J1939_CAN
    kind: can
  - name: VehicleCAN
    kind: can
```

```sh
vehicle_sil_launch check simulation.two-buses.yaml
```

結果は `passed:` で、終了コードは 0 です。ただし、network の表では 2 つの ECU が別々のバスに分かれていて、警告が 2 件出ます。

<!-- integrate ECUs: vehicle_sil_launch check simulation.two-buses.yaml -->
```text
simulation.two-buses.yaml: 2 participants, 2 networks

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   VehicleCAN (j1939 0x28)

network     kind  participants
J1939_CAN   can   Engine
VehicleCAN  can   Dashboard

warning: simulation.two-buses.yaml:7: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it; the network 'VehicleCAN' (line 9, only Dashboard) of kind can also has one ECU: if it is meant to be the same bus, keep one of the two names: remove 'J1939_CAN' from networks and add network_names: { VehicleCAN: J1939_CAN } to the participant 'Engine' [single-participant]
warning: simulation.two-buses.yaml:9: network 'VehicleCAN': only Dashboard is on it, so what it sends there reaches no one: add the other participants of this network, or remove it; the network 'J1939_CAN' (line 7, only Engine) of kind can also has one ECU: if it is meant to be the same bus, keep one of the two names: remove 'VehicleCAN' from networks and add network_names: { J1939_CAN: VehicleCAN } to the participant 'Dashboard' [single-participant]

passed: 0 errors, 2 warnings, 0 notes
```

`passed:` でも、警告は読んでください。この設定ファイルは `run` でも終了コード 0 で終わりますが、表示の ECU は回転数を受信できません（手順 5 で確認します）。警告を見逃さないようにするには、`--strict` を付けます。警告があると、結果が `failed: 0 errors, 2 warnings, 0 notes (--strict)` になり、終了コードが 1 になります。

### うまくいかないとき

指摘は、行の末尾のコードで調べます。よく出るコードと、直す場所は次のとおりです。全部のコードは、[simulation-check.md](../simulation-check.md) の「照合」の表にあります。

| 行の末尾のコード | 原因 | 直す場所 |
|---|---|---|
| `[unknown-key]`、`[missing-key]`、`[value]` | 設定ファイルのキーか値が間違っています。 | 設定ファイルを、指摘の文のとおりに直します。 |
| `[executable]` | `command` のパスに実行ファイルがありません。 | 設定ファイルのパスを直すか、実行ファイルを置きます。 |
| `[launcher-argument]` | `command` に、`--name` など `vehicle_sil_launch` が追加する引数を書いています。 | 設定ファイルの `command` から、その引数を削除します。 |
| `[unknown-network]`、`[kind-mismatch]` | ECU が使うネットワークの名前か種類が、`networks` と違います。 | 手順 1 で決めた名前と違うのが設定ファイルなら設定ファイルを直します。ECU の名前が違うなら、指摘の文が言う `network_names` の行を書くか、手順 4 です。種類が違うなら手順 4 です。 |
| `[single-participant]`、`[unused-network]` | ネットワークに ECU が 1 つしかないか、1 つもありません。 | ECU を無効にしたせいなら、そのままで構いません。名前の違いが原因なら、指摘の文が言う `network_names` の行を書くか、手順 4 です。 |
| `[step-mismatch]` | `step_ms` が、ECU のステップ幅と違います。 | 伝えてもらった値と違うのが設定ファイルなら設定ファイル、ECU なら手順 4 です。 |
| `[address-conflict]`、`[address-shared]`、`[name-conflict]` | 同じネットワークで、J1939 のアドレスか NAME が重なっています。 | 手順 4 です。 |
| `[describe-failed]`、`[describe-output]`、`[describe-timeout]` | ECU が `--describe` に正しく応答しません。 | 手順 4 です。`ecu_main()` で作っていないツールなら、`describe: false` を書きます。 |
| `[describe-version]`、`[describe-unknown-value]` | ECU と `vehicle_sil_launch` で、vehicle-sil のバージョンが違います。 | 指摘の文にある古いほうを、新しくします。 |
| `[version-mismatch]`、`[control-link-version]` | ECU をビルドした vehicle-sil のバージョンが、`vehicle_sil_launch` と違います。 | 同じバージョンの vehicle-sil で、ECU をビルドし直してもらいます。 |

## 手順 4: 設定ファイルでは直せない不整合を依頼する

次の値は ECU のビルドに入っていて、設定ファイルでは変えられません（ネットワーク名は例外で、手順 3 のとおり `network_names` で対応させられます）。合わないときは、ECU を作った人に、コードを変えてビルドし直してもらいます。

| 不整合 | 依頼する相手 | 依頼する内容 |
|---|---|---|
| ネットワーク名が ECU の間で違う（`network_names` で対応させない） | 手順 1 で決めた名前と違う ECU を作った人 | `j1939()`、`can()`、`lin_master()`、`lin_slave()`、`ethernet()` に渡す名前を、決めた名前に変えます。 |
| ネットワークの種類が違う | 決めた種類と違う ECU を作った人 | 決めた種類のバスに接続するように変えます。 |
| J1939 のアドレスか NAME が重なる | 手順 1 の一覧と違う ECU を作った人 | ノードのアドレスか NAME を、一覧の値に変えます。 |
| ステップ幅が、伝えてもらった値と違う | その ECU を作った人 | どちらの値が正しいかを確認し、ECU が違うなら直します。 |
| `--describe` が失敗する | その ECU を作った人 | 指摘に付いている標準エラーの末尾を渡して、原因を調べてもらいます。 |

名前を事前に決めていなかったときは、どちらの ECU が正しいかを `check` からは判断できません。ここで名前を決めて全員に伝えてから、違う ECU を作った人に依頼します。

依頼には、次の 4 つを書きます。

| 書くこと | 例での内容 |
|---|---|
| `check` の指摘の行 | 手順 3 の `error:` の行を、そのまま貼ります。 |
| いまの値と、変えた後の値 | ネットワーク名を `VehicleCAN` から `J1939_CAN` に変えます。 |
| 渡す前に確認する方法 | `--describe` の `networks[].name` が `J1939_CAN` になっていることを見ます。 |
| ビルドに使う vehicle-sil のバージョン | `vehicle_sil_launch version` が表示するバージョンです。 |

例の依頼は、次のようになります。

> 表示の ECU は、J1939 のネットワークに `VehicleCAN` という名前で接続しています。決めた名前は `J1939_CAN` で、エンジンの ECU はこの名前を使っています。名前が違うと、どの設定ファイルにも名前の対応を書かない限り、2 つの ECU は別のバスに分かれ、エンジンの回転数を受信できません。コードの `ecu.j1939("VehicleCAN")` を `ecu.j1939("J1939_CAN")` に変えて、vehicle-sil 0.1.0 でビルドし直してください。渡す前に、`--describe` の `networks[].name` が `J1939_CAN` になっていることを確認してください。`check` の指摘は次のとおりです。（手順 3 の `error:` の行）

作り直した実行ファイルを受け取ったら、置き換えて、もう一度 `check` を実行します。例では、作り直した表示の ECU が `../ecu_main/dashboard/` にあるので、`simulation.fixed.yaml` は `Dashboard` の `command` を次の行に変えてあります。

<!-- examples/integrate_ecus/simulation.fixed.yaml: the command of the Dashboard -->
```yaml
    command: [../ecu_main/dashboard/vehicle_sil_dashboard_ecu]
```

```sh
vehicle_sil_launch check simulation.fixed.yaml
```

### 確認

2 つの ECU が同じ `J1939_CAN` に並び、指摘がなくなります。終了コードは 0 です。

<!-- integrate ECUs: vehicle_sil_launch check simulation.fixed.yaml -->
```text
simulation.fixed.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

passed: 0 errors, 0 warnings, 0 notes
```

作り直しを待つ間は、その ECU を無効にして、ほかの ECU だけで先に進められます（[ECU を無効にして動かす](#ecu-を無効にして動かすenabled--disable)）。

## 手順 5: 実行して通信を確認する

```sh
vehicle_sil_launch run simulation.fixed.yaml
```

`run` は、まず `check` と同じ照合をします。`error` がなければ、SIL Kit の registry とすべての ECU を起動し、`stop_at_ms` まで実行して、ECU を終了させます。

### 確認

標準出力の最後に、次の結果が表示されます。`ended` の列がすべて `exit code 0` で、最後の行が `passed:` なら、終了コードは 0 です。

<!-- integrate ECUs: vehicle_sil_launch run simulation.fixed.yaml, from the line "run:" -->
```text
run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

passed: 0 errors, 0 warnings, 0 notes
```

終了コード 0 からわかるのは、すべての ECU が起動し、`stop_at_ms` まで動いて、終了コード 0 で終わったことだけです。ECU がメッセージを受信できたかどうかは、わかりません。手順 3 の `simulation.two-buses.yaml` を実行すると、2 つの ECU が別のバスにいるのに、同じ表が表示されて、終了コード 0 で終わります。

```sh
vehicle_sil_launch run simulation.two-buses.yaml
```

<!-- integrate ECUs: vehicle_sil_launch run simulation.two-buses.yaml, from the line "run:" -->
```text
run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

passed: 0 errors, 2 warnings, 0 notes
```

通信できたことは、次のどれかで確認します。

| 方法 | 見るもの | 使える条件 |
|---|---|---|
| ECU 自身の出力 | 標準エラーの `[<participant 名>]` で始まる行 | ECU が、受信した値を出力するように作ってあります。 |
| `expect` のあるシナリオ | `run --scenario` の結果と終了コード | 受信した値を、ECU が変数として宣言しています。 |
| GUI か Control API | 実行中の変数の値 | 受信した値を、ECU が変数として宣言しています。 |

受信した値を出力も宣言もしない ECU では、受信したことを外から確認できません。確認が必要なら、ECU を作った人に、受信した値を変数として宣言してもらいます（[最初の ECU を作る](first-ecu.md) の「J1939 で通信させる」）。

### ECU の出力で確認する

`run` は、ECU の標準出力と標準エラーを、行の先頭に `[<participant 名>] ` を付けて、標準エラーに表示します。例の表示の ECU は、500 ms ごとに、受信した回転数とメッセージの数を出力します。`simulation.fixed.yaml` では、次の 3 行が表示されます。

<!-- integrate ECUs: vehicle_sil_launch run simulation.fixed.yaml, the lines of the Dashboard -->
```text
[Dashboard] t=0 ms engine speed 0 rpm (0 messages)
[Dashboard] t=500 ms engine speed 1200 rpm (5 messages)
[Dashboard] t=1000 ms engine speed 1200 rpm (10 messages)
```

`simulation.two-buses.yaml` では、最後まで 0 件のままです。

<!-- integrate ECUs: vehicle_sil_launch run simulation.two-buses.yaml, the lines of the Dashboard -->
```text
[Dashboard] t=0 ms engine speed 0 rpm (0 messages)
[Dashboard] t=500 ms engine speed 0 rpm (0 messages)
[Dashboard] t=1000 ms engine speed 0 rpm (0 messages)
```

`--log-dir logs` を付けると、ECU ごとの出力が `logs/<participant 名>.log` にも保存されます（先頭の `[<participant 名>] ` は付きません）。

### シナリオで確認する

シナリオは、ECU の変数に値を設定し、変数の値を確認する手順を書いた YAML です。`expect` の手順が成り立たなければ、`run` が終了コード 1 で終わるので、CI でも通信を確認できます。例の `scenarios/engine_speed.yaml` は、エンジンのスロットルを 50 % にして、表示の ECU の回転数が 600 ms 以内に 1700 rpm 以上になることを確認します。上の `run` の 1200 rpm はスロットルの初期値 20 % のときの値で、50 % では 1800 rpm に上がります。

<!-- examples/integrate_ecus/scenarios/engine_speed.yaml -->
```yaml
# yaml-language-server: $schema=../../../schema/scenario.schema.json
# The dashboard shows the speed the engine sends (the guide docs/howto/integrate-ecus.md).
vehicle_sil_scenario: 1
name: the dashboard receives the engine speed
steps:
  - at_ms: 0
    set: { Engine.engine.throttle: 50 }
  - expect: { Dashboard.dashboard.engine_speed: { ge: 1700 } }
    within_ms: 600
```

```sh
vehicle_sil_launch run simulation.fixed.yaml --scenario scenarios/engine_speed.yaml
```

2 つの手順が `ok` になり、終了コード 0 で終わります。次は、出力の途中からシナリオの結果を抜き出したものです。

<!-- integrate ECUs: vehicle_sil_launch run simulation.fixed.yaml --scenario scenarios/engine_speed.yaml, the result of the scenario -->
```text
scenario scenarios/engine_speed.yaml (the dashboard receives the engine speed): passed, 2 of 2 steps passed

line  step                                                             result  at
6     set Engine.engine.throttle = 50                                  ok      0 ms
8     expect Dashboard.dashboard.engine_speed ge 1700 (within 600 ms)  ok      410 ms
```

同じシナリオを `simulation.two-buses.yaml` で実行すると、`expect` が失敗して、終了コード 1 で終わります。次も、出力の同じところを抜き出したものです。

```sh
vehicle_sil_launch run simulation.two-buses.yaml --scenario scenarios/engine_speed.yaml
```

<!-- integrate ECUs: vehicle_sil_launch run simulation.two-buses.yaml --scenario scenarios/engine_speed.yaml, from the result of the scenario -->
```text
scenario scenarios/engine_speed.yaml (the dashboard receives the engine speed): FAILED, 1 of 2 steps passed

line  step                                                             result  at
6     set Engine.engine.throttle = 50                                  ok      0 ms
8     expect Dashboard.dashboard.engine_speed ge 1700 (within 600 ms)  FAILED  600 ms

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

error: scenarios/engine_speed.yaml:8: expect Dashboard.dashboard.engine_speed ge 1700 (within 600 ms): not true at any time within 600 ms (from 0 ms to 600 ms): at the end Dashboard.dashboard.engine_speed ge 1700, but it was 0 (its sample of 600 ms) [scenario-failed]

failed: 1 error, 2 warnings, 0 notes
```

シナリオの書き方は、[シナリオでテストを書く](scenario-tests.md) にあります。

### GUI か Control API で確認する

実行中の変数の値は、ブラウザの GUI（[gui.md](../gui.md)）か、Control API の Python のクライアント（[信号を外から操作する](operate-signals.md) の「値を読む」）で読めます。例では、`Dashboard` の変数 `dashboard.engine_speed` が、`Engine` の変数 `engine.speed` に追従していれば、通信できています。

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| `nothing was started: the check found errors` と表示される | 照合で `error` が出ています。 | 手順 3 に戻って、`error` を直します。 |
| 終了コード 0 なのに、受信した値が 0 のまま | ECU が別のバスにいます。 | `check` の network の表と警告を読みます（手順 3）。 |
| いつまでも終わらない | ステップの途中で止まった ECU がいて、仮想時間が進みません。 | Ctrl+C で止めます。CI では `--timeout-ms` で実時間の上限を付けます。 |
| `[participant-failed]`、`[not-joined]`、`[did-not-end]` などの指摘が出る | 実行中に、ECU が失敗したか、参加しなかったか、終わりませんでした。 | 指摘の文にある participant の出力を読みます。 |

実行中の指摘のコードは [simulation-run.md](../simulation-run.md) の「出力」に、中断する条件は同じ文書の「中断する条件とタイムアウト」にあります。

## ECU を無効にして動かす（`enabled`、`--disable`）

まだ受け取っていない ECU や、原因を切り分けたい ECU を除いて、残りの ECU で照合と実行をします。`--disable` に participant 名を指定します。

```sh
vehicle_sil_launch check simulation.fixed.yaml --disable Dashboard
vehicle_sil_launch run simulation.fixed.yaml --disable Dashboard
```

### 確認

無効にした ECU は、表に `disabled` と表示され、`participant-disabled` の注記が出ます。

<!-- integrate ECUs: vehicle_sil_launch check simulation.fixed.yaml --disable Dashboard -->
```text
simulation.fixed.yaml: 2 participants (1 disabled), 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    -      disabled

network    kind  participants
J1939_CAN  can   Engine

warning: simulation.fixed.yaml:7: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it [single-participant]
note: simulation.fixed.yaml:13: participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked nor started, and the other findings are about the simulation without it [participant-disabled]

passed: 0 errors, 1 warning, 1 note
```

| 知っておくこと | 内容 |
|---|---|
| 無効にした ECU | 照合も起動もしないので、実行ファイルがまだなくても構いません。 |
| 設定ファイルに書く | participant に `enabled: false` と書いても同じです。 |
| 有効に戻す | `--enable <participant 名>` を付けます。コマンドラインの指定が、設定ファイルより優先されます。 |
| 注記 `participant-disabled` | 一部を無効にした結果を、全部の結果と読み違えないためのものです。 |
| `--strict` | 無効にしたために出る警告（上の `single-participant`）でも、失敗になります。 |
| `required: false` | `ecu_main()` で作った ECU には書けません。除くときは、`enabled: false` か `--disable` を使います。 |

ルールは、[simulation-config.md](../simulation-config.md) の「participant を無効にする」にあります。

## 止める

| 止め方 | 動作 | 終了コード |
|---|---|---|
| `stop_at_ms` を書く | その仮想時刻で止まります。 | 指摘に `error` がなければ 0 |
| Ctrl+C | シミュレーションを止め、ECU を終了させてから、結果を表示します。 | 130 |
| Ctrl+C をもう 1 回 | 待たずに強制終了します。 | 130 |
| `--timeout-ms <ms>` | 開始から実時間で `<ms>` たっても終わらなければ、中断して `error` にします。 | 1 |

プログラムから止めるときは、`run` に `--stdin-commands` を付けて、標準入力に `{"command":"stop"}` を書くか、標準入力を閉じます（[simulation-events.md](../simulation-events.md) の「標準入力のコマンド」）。

どの止め方でも、`vehicle_sil_launch` は、起動した ECU と、ECU が起動したプロセスを終了させてから終わります。シミュレーションが終わっても終了しない ECU は、強制終了して、`did-not-end` の `error` にします。順番と待つ時間は、[simulation-run.md](../simulation-run.md) の「止め方」「中断（Ctrl+C）」「participant を残さないこと」にあります。

## スクリプトから使う（`--json`、`--events`）

CI のスクリプトや自作のツールから使うときは、終了コードと、機械が読める出力を使います。

| したいこと | コマンド | 出力 |
|---|---|---|
| 成功か失敗かだけを知る | `check` か `run` の終了コード | 0 が成功、1 が失敗、130 が中断です。 |
| 指摘をプログラムで読む | `vehicle_sil_launch check --json simulation.yaml` | 結果を 1 つの JSON で標準出力に書きます。 |
| 実行の経過を読む | `vehicle_sil_launch run --events --stdin-commands simulation.yaml` | 1 行に 1 つの JSON で、起きたことを順に書きます。 |
| 使える機能を調べる | `vehicle_sil_launch version --json` | `features` に、機能の名前が並びます。 |

`check --json` では、指摘が `findings` に入ります。次は、手順 3 の `simulation.yaml` の出力の一部です。

<!-- integrate ECUs: vehicle_sil_launch check --json simulation.yaml, an excerpt -->
```json
{
  "result": "failed",
  "errors": 1,
  "warnings": 1,
  "findings": [
    {
      "severity": "error",
      "code": "unknown-network",
      "file": "simulation.yaml",
      "line": 12,
      "participant": "Dashboard",
      "network": "VehicleCAN"
    },
    {
      "severity": "warning",
      "code": "single-participant",
      "line": 6,
      "network": "J1939_CAN"
    }
  ]
}
```

| 知っておくこと | 内容 |
|---|---|
| `participants[].variables` | `check --json` の出力にあり、どの ECU にどの変数があるかを、実行の前に調べられます。 |
| `--events` の最初と最後 | 最初の行が `hello`、最後の行が `result` で、`result` の内容は `run --json` と同じです。 |
| `--json` と `--events` | 一緒には指定できません。 |
| `--stdin-commands` | 呼び出したプログラムが終わると標準入力が閉じ、`vehicle_sil_launch` も止まります。 |

リファレンスは次のとおりです。

| 知りたいこと | 場所 |
|---|---|
| 終了コード | [simulation-config.md](../simulation-config.md) の「引数と終了コード」 |
| `check --json` の形式 | [simulation-check.md](../simulation-check.md) の「出力」 |
| イベントの種類と順序 | [simulation-events.md](../simulation-events.md) |

Python からは、`Launch` クラスがこの形で `vehicle_sil_launch` を起動します（[control-api.md](../control-api.md) の「Python」）。

## 等速で動かす（`--speed`）と遅れの見方

何も指定しないと、シミュレーションは PC が計算できる最速で進みます。実時間で動く機器と一緒に動かすときや、画面を見ながら操作するときは、`--speed` で速度を指定します。`1` が実時間と同じ速度です。

```sh
vehicle_sil_launch run simulation.fixed.yaml --speed 1
```

### 確認

速度を指定すると、結果に `time:` の行が追加されます。次は `--speed 4` の行です（`<` と `>` で囲んだ所は、実行ごとに違う値になります）。

<!-- vehicle_sil_launch run simulation.yaml --speed 4: the line of the time -->
```text
time: speed 4, ran at <actual_speed> times real time; <late_steps> of 200 paced steps more than 10 ms late (the latest by <max_late_ms> ms), <slipped_ms> ms behind in all
```

| 行の部分 | 意味 |
|---|---|
| `ran at <actual_speed> times real time` | 全体の実際の速度です（一時停止していた時間を除きます）。 |
| `<late_steps> of 200 paced steps more than 10 ms late` | 速度を合わせたタイムステップのうち、許容（`late_tolerance_ms`、既定は 10 ms）を超えて遅れた数です。 |
| `the latest by <max_late_ms> ms` | いちばん遅れたタイムステップの遅れです。 |
| `<slipped_ms> ms behind in all` | 遅れのために後ろにずれた時間の合計です（シミュレーションは遅れを取り戻しません）。 |

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| 警告 `behind-real-time` が出る | 許容を超えて遅れたタイムステップが、1 つ以上あります。 | 速度を下げるか、ECU を 1 つずつ `--disable` で無効にして、遅い ECU を探します。 |
| 警告 `coarse-time-step` が出る | `time_step_ms` を書いていて、ECU のステップがタイムステップの境目で始まりません。 | 指摘の文にある値を、`time_step_ms` に書きます。 |

遅れの定義、許容の決め方、タイムステップと ECU のステップの関係は、[simulation-run.md](../simulation-run.md) の「速度を指定して動かす」にあります。

## 同じ ECU を 2 つ載せる

同じ実行ファイルを 2 つの participant として並べると、participant 名は `--name` で変わりますが、J1939 の NAME、アドレス、変数名は同じままです。同じネットワークにいれば、`check` が `name-conflict` を報告します。これも設定ファイルだけでは直せないので、ECU を作った人に、NAME とアドレスを引数で受け取れるようにしてもらいます。

| すること | 担当 | 内容 |
|---|---|---|
| ECU に独自の引数を追加する | ECU を作った人 | `main()` で独自の引数を `argv` から取り出し、残りを `ecu_main()` に渡します（[ecu-executable.md](../ecu-executable.md) の「`ecu_main`」）。 |
| 引数を設定ファイルに書く | 設定ファイルを書く人 | `command` の、実行ファイルの後ろに書きます。 |

設定ファイルは、たとえば次のようになります。`--address` と `--identity-number` は、ECU を作った人が決める引数の例です。例のエンジンの ECU には、この引数はありません。

```yaml
  - name: Engine2
    command: [engine/vehicle_sil_engine_ecu, --address, "0x01", --identity-number, "2"]
```

| 知っておくこと | 内容 |
|---|---|
| `check` での扱い | `check` も `run` も、`command` の引数を付けて ECU を起動するので、その引数で作ったノードのアドレスと NAME を照合します。 |
| 使えない引数の名前 | `ecu_main()` の引数（[ecu-executable.md](../ecu-executable.md) の「引数」）と同じ名前は使えません。 |
| 知らない引数 | `ecu_main()` は、知らない引数があると終了コード 4 で終わるので、独自の引数は必ず `main()` で取り出します。 |
| `--help` | 独自の引数は `ecu_main()` の `--help` に表示されないので、ECU の説明に書いてもらいます。 |

## `ecu_main()` で作っていないツールを並べる

ログを取るツールや自作のテスト用のプログラムなど、`ecu_main()` で作っていない実行ファイルは、`describe: false` を書いて並べます。`check` はその実行ファイルを照合せず、`not-checked` の注記を出します。

| 書くもの | 内容 |
|---|---|
| `describe: false` | `--describe` で起動しません。 |
| `command` の `{registry_uri}`、`{name}` | `run` が、registry の URI と participant 名に置き換えます。 |
| `required: false` | シミュレーションに参加しないツールに書きます。`run` は、そのツールの参加を待ちません。 |
| `manual: true` | `run` は起動せず、コマンドを表示します。自分で起動します。 |

> [!WARNING]
> SIL Kit の autonomous のライフサイクルで仮想時間に参加するツール（既定の設定の `Peer` など）が、ECU より先に仮想時間を進めると、ECU が始まった時点で、SIL Kit がシミュレーション全体を中断します。こういうツールは、coordinated にして `required: true` にするか、`manual: true` にして、シミュレーションが始まった後に起動します。

ルールは、[simulation-config.md](../simulation-config.md) の「ほかの実行ファイル（`describe: false`）」と「participant を自分で起動する」にあります。

## 補足

- 変数名は ECU ごとに独立なので、ECU の間で合わせる必要はありません。`ecu.` と `peer.` で始まる名前は使えません。`j1939.<ネットワーク名>.` で始まる名前は、監視の出力と重なることがあるので避けます。重なると、ECU は起動のときに終了コード 3 で終わります。例では、ECU の名前を先頭に付けています（`engine.throttle`、`dashboard.engine_speed`）。
- J1939 のアドレス 254 は、空いているアドレスを ECU が自分で選ぶ指定で、`check` は重なりとみなしません。アドレスが重なったときに `error` になるか `warning` になるかの条件は、[simulation-check.md](../simulation-check.md) の「照合」にあります。
- 例の Manufacturer Code の 0x7F0 は、例のための仮の値です。

## 次に読むもの

- 値の設定と確認を、自動のテストにする → [シナリオでテストを書く](scenario-tests.md)
- 実行中に変数を読み書きする、時間を止めて進める → [信号を外から操作する](operate-signals.md)
- ほかの PC で動く ECU を参加させる → [複数の PC で動かす](multi-pc.md)
