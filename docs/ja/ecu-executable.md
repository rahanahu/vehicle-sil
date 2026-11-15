# ECU の実行ファイル（`ecu_main`）と `--describe` の出力

[English](../ecu-executable.md)

ECU のクラス（`Ecu&` を受け取るクラス）を 1 つの実行ファイルにする関数 `ecu_main()` のリファレンスです。実行ファイルの引数と終了コード、止まり方、ECU が自分の構成を JSON で出力する `--describe` の形式を載せています。API はまだ変わることがあります。

ECU ごとに別の人が実行ファイルをビルドし、それらを 1 つの SIL Kit のシミュレーションで動かすときに使います。どの ECU の実行ファイルも同じ引数を持つので、ECU の中身を知らなくても動かせます。

```cpp
#include "vehicle_sil/simulation/ecu_main.hpp"

int main(int argc, char** argv)
{
    return vehicle_sil::ecu_main<Engine>(argc, argv, {.name = "Engine", .step = 10ms});
}
```

完全な例は [examples/ecu_main](../../examples/ecu_main)（`vehicle_sil_engine_ecu` と `vehicle_sil_dashboard_ecu`）です。

| 調べること | 節 |
|---|---|
| `ecu_main()` の呼び出し方、引数、終了コード | [`ecu_main`](#ecu_main) |
| ECU を作る人に依頼すること、使えない変数名 | [ECU を作る人に依頼すること](#ecu-を作る人に依頼すること) |
| `Ecu::describe()` が返す内容 | [`Ecu::describe()`](#ecudescribe) |
| `--describe` の JSON のキー | [JSON の形式](#json-の形式) |
| ランチャーを使わずに複数の ECU を動かす方法 | [複数の ECU を 1 つのシミュレーションで動かす](#複数の-ecu-を-1-つのシミュレーションで動かす) |

## `ecu_main`

ライブラリは `vehicle_sil::simulation` です。ヘッダーは SIL Kit のヘッダーを含みません。

| 呼び出し方 | すること |
|---|---|
| `ecu_main<App>(argc, argv, EcuMainOptions, args...)` | ECU に `App{ecu, args...}` を持たせて（`Ecu::emplace()`）、引数のとおりに動かします。 |
| `ecu_main(argc, argv, EcuMainOptions, std::function<void(Ecu&)>)` | クラスの代わりに関数で ECU を組み立てて、引数のとおりに動かします。 |

`args` は `App` のコンストラクターへそのまま渡します。`App` は 1 回だけ作るので、ムーブしかできない値も渡せます。関数で組み立てる形では、関数は 1 回だけ、何かを動かす前に呼ばれます。

| `EcuMainOptions` のメンバー | 内容 |
|---|---|
| `name` | ECU の名前です（SIL Kit の participant 名で、`--describe` の `name`）。 |
| `step` | ステップ幅です（0 より大きいこと）。 |
| `last_word_wait` | `--registry-uri` で終わるときに、最後の通知へのランチャーのレスポンスを待つ上限です（既定 500 ms、0 で待たない）。 |

`last_word_wait` で待つ最後の通知は、取り消された書き込みなどをランチャーに伝えるものです。レスポンスはふつうすぐ来るので、変える必要があるのは、レスポンスに 500 ms 以上かかるほど遅い PC だけです。この値を変える引数はありません。

### 引数と終了コード

| 引数 | すること |
|---|---|
| `--describe` | ECU をプロセス内の仮想バスの上で組み立て、何も動かさず、どこにも接続せずに、`to_json(Ecu::describe())` を標準出力に書いて終了します。 |
| `--registry-uri <uri>` | `<uri>` の registry の SIL Kit のシミュレーションに、Coordinated で仮想時間の participant として参加し、シミュレーションが終わるまで動きます。 |
| `--start-timeout-ms <ms>` | `--registry-uri` のときだけ使えます。参加してからシミュレーションが始まるまで待つ時間（1 以上の整数。既定は 30000）です。 |
| `--no-control` | `--registry-uri` のとき、ECU の変数をシミュレーションに公開しません（[変数を公開する](#変数を公開する--registry-uri)）。 |
| `--silkit-config <file>` | `--registry-uri` のとき、この participant に SIL Kit の設定ファイル（YAML か JSON）を渡します（[SIL Kit の設定ファイルを渡す](#sil-kit-の設定ファイルを渡す--silkit-config)）。 |
| `--map-network <シミュレーションの名前>=<実行ファイルの名前>` | `--registry-uri` のときだけ使えます。ECU のコードが `<実行ファイルの名前>` で作ったネットワークを、SIL Kit のネットワーク `<シミュレーションの名前>` につなぎます（[ネットワークの名前を替える](#ネットワークの名前を替える--map-network)）。 |
| `--j1939-node <ネットワーク>=<設定>` | `--registry-uri` のときだけ使えます。この実行で、ECU の J1939 のノードのアドレスと NAME の欄を替えます（[J1939 のノードのアドレスと NAME を替える](#j1939-のノードのアドレスと-name-を替える--j1939-node)）。 |
| `--local --run-for-ms <ms>` | この ECU だけを、プロセス内の仮想バスの上で、仮想時間で `<ms>` ミリ秒（1 以上の整数）動かします。 |
| `--name <name>` | `EcuMainOptions::name` の代わりの participant 名です（ASCII か UTF-8）。 |
| `--help`（`-h`） | 使い方を標準出力に書いて、0 で終了します。 |

`--describe`、`--registry-uri`、`--local` のどれか 1 つが必要です。引数ごとの補足は次のとおりです。

| 引数 | 補足 |
|---|---|
| `--registry-uri` | シミュレーションを始めるのと止めるのは、ほかのプロセスの controller（`vehicle_sil_sim_controller`）です。 |
| `--registry-uri` | `<uri>` のホストがループバックのアドレス（`127.0.0.1`、`localhost` など）なら、ほかの participant からの接続をループバックのアドレスだけで待ち受けます。 |
| `--registry-uri` | 環境変数 `VEHICLE_SIL_LISTEN_ON_NETWORK=1` を設定すると、全部のアドレスで待ち受けます（[待ち受けるアドレス](simulation-run.md#待ち受けるアドレス)）。 |
| `--start-timeout-ms` | 時間内に始まらなければ、終了コード 3 で終わります。 |
| `--start-timeout-ms` | ECU をたくさん起動するときや、遅い PC で使います。 |
| `--no-control` | ECU の動きは変わりません。 |
| `--no-control` | `--describe` に付けると、`variables` はそのままで、`control` が `null` になります（`vehicle_sil_launch run` は、この ECU を待ちません）。 |
| `--no-control` | `--local` に付けても何も変わりません。 |
| `--silkit-config` | `--describe` に付けると、ファイルを読んで確認するだけです（エラーは終了コード 3）。 |
| `--silkit-config` | `--local` には付けられません（終了コード 4）。 |
| `--map-network` | ネットワークごとに 1 回ずつ、何回でも書けます。 |
| `--j1939-node` | ノードごとに 1 回ずつ、何回でも書けます。 |
| `--local` | ECU を単体で確認するときに使います。 |
| `--name` | `--describe` の `name` にも反映されます。 |
| `--name` | 変わるのは名前だけで、J1939 の NAME、アドレス、変数の名前は同じです。 |
| `--help` | ほかの引数にエラーがあっても使い方を書きます。 |
| `--help` | 値を取る引数の後ろの `-h` は値です（`--name -h` は名前が `-h`）。 |

同じ ECU を 2 つ載せるときは、`--name` で変わらない NAME とアドレスが重ならないようにします（`--j1939-node` で替えられます。[J1939 のノードのアドレスと NAME を替える](#j1939-のノードのアドレスと-name-を替える--j1939-node)）。変数の名前は participant ごとなので、同じままでかまいません。方法は [複数の ECU をつないで動かす](howto/integrate-ecus.md) の「同じ ECU を 2 つ載せる」にあります。

終了コードは、ほかのツールと同じです。

| 終了コード | 意味 |
|---|---|
| 0 | 終わった（`--describe` は JSON を書いた。`--registry-uri` はシミュレーションが終わった。`--local` は時間まで動いた） |
| 1 | 実行中に ECU が失敗した（ステップの例外。`SimulationError`） |
| 3 | 準備に失敗した（`App` のコンストラクターの例外、`EcuMainOptions` のエラー、registry に接続できない、シミュレーションが始まらない、標準出力に書けない、`--map-network` の実行ファイルの名前のネットワークを ECU が持たない、どのノードにも当たらない `--j1939-node`） |
| 4 | 引数のエラー |
| 130 | `--registry-uri` で、停止を求められた（[止まるよう求められたとき](#止まるよう求められたとき--registry-uri)） |

終了コード 4 になる引数は次のとおりです。何も始めずに、使い方を標準エラーに書きます。

| 引数のエラー | 補足 |
|---|---|
| `--describe`、`--registry-uri`、`--local` のどれもない | 引数がないときも同じです。 |
| 3 つのうち 2 つ以上がある | |
| 知らない引数 | `--name=X` の形も知らない引数です（値は空白で区切ります）。 |
| 値のない `--name`、`--registry-uri`、`--run-for-ms`、`--start-timeout-ms` | 次が `--` で始まる引数のときも、値がないとみなします。 |
| 空の値 | |
| ASCII でも UTF-8 でもない `--name` | Windows のコードページの名前などで、メッセージにバイト列を 16 進で表示します。 |
| 同じ引数の 2 回目 | `--map-network` と `--j1939-node` は別です（[ネットワークの名前を替える](#ネットワークの名前を替える--map-network)、[J1939 のノードのアドレスと NAME を替える](#j1939-のノードのアドレスと-name-を替える--j1939-node)）。 |
| `--map-network` の値のエラー | [ネットワークの名前を替える](#ネットワークの名前を替える--map-network) にあります。 |
| `--j1939-node` の値のエラー、ぶつかる 2 つの `--j1939-node` | [J1939 のノードのアドレスと NAME を替える](#j1939-のノードのアドレスと-name-を替える--j1939-node) にあります。 |
| `--registry-uri` のない `--map-network` か `--j1939-node` | |
| `--run-for-ms` のない `--local`、`--local` のない `--run-for-ms` | |
| `--registry-uri` のない `--start-timeout-ms` | |
| 整数でない、0 の、または 10^12 を超える `--run-for-ms` と `--start-timeout-ms` | |

失敗したときは、標準エラーに 1 行 `error: <ECU 名>: <理由と直し方>` を書きます。

| 項目 | ルール |
|---|---|
| 1 行にする方法 | SIL Kit のメッセージの改行は空白にし、`std::exception` でない例外も固定のメッセージで 1 行にします。 |
| ECU 名 | `--name` で変えた名前です（引数のエラーのときは `EcuMainOptions::name`）。 |
| 引数のエラー | 続けて使い方を書きます。 |
| シミュレーションが始まらないとき | 時間切れと、それ以外とで、メッセージを分けます。 |

時間切れは、controller がいないか、必要な participant がそろわないときで、メッセージに controller と `--start-timeout-ms` の確認方法を添えます。それ以外の例は、変数の名前が `Peer` の監視用の変数と重なったときです。

registry に接続できないときのメッセージ（`cannot join the simulation at "<URI>": ...`）は、確認することを挙げます。挙げるのは、`vehicle_sil_launch run` を先に起動して待たせること、URI のアドレスとポート、その PC のファイアウォール、同じ名前の participant です。

registry が「その名前は使用中」として拒否したときは、5 秒の間、participant を作り直してから失敗します（`--start-timeout-ms` が 5 秒より短ければ、その時間）。同じ名前ですぐに起動し直したとき、registry が前のプロセスの終了にまだ気づいていないことがあるためです。このときのメッセージは、待った時間と名前のことだけを言います。registry には接続できているので、確認することの一覧は付けません。

### ECU 独自の引数

`ecu_main()` は、知らない引数があると終了コード 4 で終わります。ECU に独自の引数（J1939 のアドレスなど）を持たせるときのルールは次のとおりです。

| 項目 | ルール |
|---|---|
| 取り出す場所 | `main()` で独自の引数を `argv` から取り出し、残りを `ecu_main()` に渡します。 |
| `App` への渡し方 | 取り出した値は、`ecu_main<App>()` の `args...` で `App` のコンストラクターに渡せます。 |
| 使えない名前 | `ecu_main()` の引数（上の表）と同じ名前は使えません。 |
| `--help` | 独自の引数は `ecu_main()` の `--help` に表示されません。 |

`vehicle_sil_launch` は、`check` でも `run` でも、設定ファイルの `command` に書いた引数を付けて ECU を起動します。そのため、`--describe` の出力は、独自の引数で作った構成（ノードのアドレスや NAME）になります。

### 標準出力と標準エラー

`ecu_main` が標準出力に書くのは、`--describe` の JSON と `--help` の使い方だけです。

`--describe` では、ECU を組み立てる前からプロセスが終わるまで、標準出力（fd 1）を標準エラーに向けます（元に戻しません）。JSON は、元の標準出力を複製した記述子に直接書きます。そのため、`App` が標準出力に書いたものが JSON に混ざりません。

| 場合 | 起きること |
|---|---|
| `App` のコンストラクターとデストラクター、`App` のスレッド、`atexit`、リンクしたライブラリが標準出力に書く（`printf`、`std::cout` など） | 標準エラーに出力されます。 |
| 標準エラーが閉じている | 先に null デバイスを開いて標準エラーにするので、`App` の出力はどこにも出ません。 |
| 標準出力が閉じている | JSON を書けないので、終了コード 3 です。 |

`ecu_main` が戻った後も fd 1 は標準エラーのままなので、`ecu_main` は `main()` の最後で呼びます。

### 変数を公開する（`--registry-uri`）

ECU は、`--registry-uri` で参加するとき、何も書かなくても、自分の変数を SIL Kit の publish/subscribe でシミュレーションに公開します（`vehicle_sil::control_silkit::AgentTransport`。[internals/control.md](internals/control.md) の「SIL Kit の上のトランスポート」）。変数を宣言していない ECU にも `ecu.connected` があるので、どの ECU も公開します。

`vehicle_sil_launch run` がそれを受信して、変数を読み書きします（[simulation-run.md](simulation-run.md) の「ECU の変数との接続」）。受信するプロセスがいなければ、送信したものは誰にも届かないだけです。

| 項目 | 内容 |
|---|---|
| 依存するライブラリ | gRPC も Protocol Buffers も必要ありません（ECU の実行ファイルは、どちらにも依存しません）。 |
| 送信する時点 | 値と書き込みの結果は、ステップの中で、値を読んだ直後に送信します。 |
| 送信する頻度 | Agent の `sample_period`（既定 10 ms。`Ecu::set_agent_options()`）で、間引きはしません。 |
| `--describe` の `control` | 公開することを `control` に出力します（`Ecu::describe()` 自体は出力せず、`ecu_main` が追加します）。 |
| ランチャーが待つ ECU | `vehicle_sil_launch run` は、`control` で公開すると出力した ECU だけを待ちます。 |
| `--no-control` | 公開しなくなり、`--describe` の `control` も `null` になります。 |
| 認証 | ECU とランチャーの間に認証はありません（registry に参加できるプロセスは、変数に書き込めます）。 |

アプリが Agent に自分のトランスポート（`control::TransportLease` を持つもの）を付けているときは、`ecu_main` はトランスポートを付けません。このとき、`--describe` の時点でトランスポートが付いていれば、`ecu_main` は `control` に公開すると出力しません。付いていなければ公開すると出力し、`vehicle_sil_launch run` が ECU からの最初のメッセージ（hello）を待ちます。そのため、アプリが Agent に自分のトランスポートを付けるときは、`--no-control` を付けてください。

### SIL Kit の設定ファイルを渡す（`--silkit-config`）

SIL Kit のミドルウェアやログの設定を変えるとき（決まったポートで待ち受ける、接続を試す回数を増やす、SIL Kit のログをファイルに書く、など）に使います。なくても動きます。

```yaml
# engine.silkit.yaml: 決まったポートで待ち受け、SIL Kit のログをファイルにも書く
Middleware:
  AcceptorUris: ["tcp://0.0.0.0:8601"]
  ConnectAttempts: 5
Logging:
  Sinks:
    - Type: File
      Level: Info
      LogName: engine_silkit
```

```sh
engine/vehicle_sil_engine_ecu --registry-uri silkit://192.0.2.10:8500 --silkit-config engine.silkit.yaml
```

ファイルの形式とキーは、SIL Kit の participant の設定（SIL Kit のドキュメントの「Configuration」）です。相対パスは、プロセスの作業ディレクトリを基準にします。

書いたものは、書いたとおりに使います。書いていないものには、vehicle-sil の既定を追加します。

| キー | 書いていないとき |
|---|---|
| `Middleware.AcceptorUris` | 設定ファイルなしのときと同じです（registry の URI がループバックのアドレスなら、ループバックだけで待ち受ける。[待ち受けるアドレス](simulation-run.md#待ち受けるアドレス)）。 |
| `Middleware.EnableDomainSockets` | Windows では `false` にします。 |
| `Experimental.TimeSynchronization.DynamicSimulationStep` | `false` にします。 |

Windows では、`Middleware.EnableDomainSockets` に `true` と書いても `false` になります。SIL Kit の既定が `true` なので、書いたかどうかを見分けられないためです。

エラーになる設定は次のとおりです。

| 設定 | 扱い | 理由と補足 |
|---|---|---|
| 知らないキー | SIL Kit が警告にします。 | |
| 型の違う値 | SIL Kit がエラーにします。 | メッセージは `error: <名前>: --silkit-config "<file>": SIL Kit does not accept the configuration: ...` です。 |
| YAML として読めないファイル | SIL Kit がエラーにします。 | 上のメッセージに「(at line <行>, column <列> of the configuration text)」が付きます。 |
| `ParticipantName` が participant 名（`--name`、なければ `EcuMainOptions::name`）と違う | 接続する前にエラーにします。 | SIL Kit は設定ファイルの名前で参加するので、participant 名で待つプロセスが待ち続けます。 |
| `Middleware.RegistryUri` が `--registry-uri` と違う | 接続する前にエラーにします。 | SIL Kit は設定ファイルの URI に接続します。 |
| `DynamicSimulationStep: true` | 接続する前にエラーにします。 | vehicle-sil の participant は、自分の固定のステップ幅で動きます。 |

接続する前のエラーは終了コード 3 で、メッセージ（`error: <名前>: --silkit-config "<file>": ...`）に直し方が入ります。`ParticipantName` と `Middleware.RegistryUri` は、同じ値なら通ります。`--describe` は URI を知らないので、`Middleware.RegistryUri` を比べません。

そのほかのルールは次のとおりです。

- `Includes` で分けたファイルは、設定ファイルのあるディレクトリからも探します。
- 上の検査と既定は、分けたファイルも合わせた結果を見ます（どのファイルに書いても同じです）。
- コントローラーの節（`CanControllers` の `Network` など）は書かないでください。ネットワークの名前が `--describe` の出力とずれます（確認する仕組みはありません）。
- `vehicle_sil_launch` では、`command` に書きます（[simulation-config.md](simulation-config.md) の「ecu_main の実行ファイル」）。照合の `--describe` にも渡るので、設定のエラーは `check` で分かります。

ライブラリから使うときは、`SilKitOptions::participant_configuration` か `ParticipantRuntimeOptions::participant_configuration` に、設定の文字列を渡します。`SilKitOptions` は `Simulation` のもので、そのプロセスの全部の participant に渡ります。ファイルから読むときは `participant_configuration_from_file()` を使います（ルールは [internals/core.md](internals/core.md) の「SIL Kit の設定の既定」）。

### 止まるよう求められたとき（`--registry-uri`）

`--registry-uri` のとき、`ecu_main` は SIGINT（Ctrl+C）と SIGTERM（Windows: Ctrl+C、Ctrl+Break、コンソールを閉じる）を受信して止まります。

| 受信した時点 | `ecu_main` がすること | 標準エラーのメッセージ |
|---|---|---|
| シミュレーションが始まった後 | participant を止め（`request_stop()`）、`run()` が戻って `App` のデストラクターなどが動いてから、終了コード 130 で終わります。 | `error: <ECU 名>: interrupted (SIGTERM): it stopped the simulation and left it` |
| 始まる前（registry への接続中、開始の待ち） | デストラクターを動かさずに、すぐに終了コード 130 で終わります。 | `error: <ECU 名>: interrupted (SIGTERM) before the simulation started: it ends at once` |
| 止めている間の 2 回目 | デストラクターを動かさずに、すぐに終了コード 130 で終わります。 | `error: <ECU 名>: interrupted (SIGTERM) again while it stopped: it ends at once` |

coordinated の participant が止まると、シミュレーション全体が順に止まります。始まる前に受信したときにデストラクターが動かないのは、シグナルの既定の動作と同じです。このときも、終わる前に C の標準入出力（`printf` など）のバッファーは書き出します。

| 項目 | ルール |
|---|---|
| 受信する期間 | `ecu_main` の中で `--registry-uri` のシミュレーションが動いている間だけです。 |
| `App` のデストラクター | 動くときには、シグナルの設定を前の設定に戻しています。 |
| `--describe` と `--local` | シグナルを扱いません（既定の動作ですぐに終わります）。 |
| `App` の `read()` など | `SA_RESTART` を使うので、途中で戻しません。 |
| 起動したプロセスが無視するように設定したシグナル（`nohup`） | 無視したままです。 |
| Windows でコンソールを閉じたとき | 順に止める処理が終わるのを、最大 4 秒待ちます（その後 Windows がプロセスを終わらせます）。 |

受信している間、これらのシグナルは `ecu_main` が使います。シグナルを自分で扱いたい ECU は、`ecu_main` を使わず、`Simulation` を直接使います。

`vehicle_sil_launch run` は、シミュレーションが終わっても終わらない participant や、始まらなかったときの participant に、この方法で停止を求めます（[simulation-run.md](simulation-run.md) の「止め方」）。

### 実行中にほかの参加者がいなくなったとき（`--registry-uri`）

`ecu_main` が自分で終わるのは、次の 3 つの場合だけです。

| 場合 | 終了コード |
|---|---|
| シミュレーションが終わった | 0 |
| この ECU のステップが失敗した | 1 |
| 停止を求められた | 130 |

ステップが失敗した ECU は、Error の状態のまま、controller がシミュレーションを中断するのを最大 5 秒待ってから終わります。controller が失敗を Error として見られるようにするためです。Error を見るとすぐ中断する controller の下では、待つのはその間だけです。

ほかのプロセスがいなくなったときに起きることは、次のとおりです。

| いなくなったもの | 起きること |
|---|---|
| controller のプロセス | シミュレーションは止まらずに、仮想時間が進み続け、ECU は自分では終わりません。 |
| registry のプロセス | participant の間の接続によっては、ほかの participant から見てこの ECU が Error になり、この ECU は仮想時間が進まないまま待ち続けることがあります。 |
| 失敗したほかの participant（controller がシミュレーションを中断する） | この ECU は終了コード 0 で終わります。 |

どの participant が失敗したかは、controller（`vehicle_sil_sim_controller` の出力と終了コード 1）が通知します。

どの場合も、ECU を起動したプロセスが、controller の終了を見て ECU のプロセスを止めてください。`vehicle_sil_launch run` はこれをします（[simulation-run.md](simulation-run.md)）。自分で起動するときのルールは次の 2 つです。

シミュレーションの結果は、controller の終了コードで決めます。ECU の終了コードだけでは決めません。ほかの participant が失敗しても、ECU は 0 で終わることがあるためです。ECU の終了コード 1 は、その ECU のステップが失敗したことだけを示します。

controller のプロセスが結果を出さずにいなくなったら、すべての ECU を止めます（ECU は自分では終わりません）。controller が終わった後もしばらく動いている ECU（registry がいなくなった場合など）も止めます。まず SIGTERM（Windows: Ctrl+Break）で停止を求め（上の「止まるよう求められたとき」）、それでも終わらなければ強制終了します。

### ネットワークの名前を替える（`--map-network`）

ECU のコードが付けたネットワークの名前が、シミュレーションでの名前と違うときに、ビルドし直さずに合わせます。

```sh
engine_ecu --registry-uri silkit://localhost:8500 --map-network VehicleCAN=J1939_CAN
```

値の向きは `<シミュレーションの名前>=<実行ファイルの名前>` です。上の例では、コードが `j1939("J1939_CAN")` で作ったネットワークが、SIL Kit のネットワーク `VehicleCAN` につながります。値は最初の `=` で分けます（実行ファイルの名前は `=` を含められます。シミュレーションの名前は含められません）。

| 項目 | ルール |
|---|---|
| 替わる単位 | 名前ごとに効きます。同じ名前の `j1939()` と `can()` は両方替わります。`lin_master()`、`lin_slave()`、`ethernet()` のネットワークも同じです。書いていないネットワークは、コードの名前のままです。2 つの名前が同じもの（`CAN=CAN`）は、何も替えません。 |
| 替わるもの | SIL Kit のネットワークの名前と、`Peer` の監視の出力の名前（`j1939.<シミュレーションの名前>.` で始まる変数と、ノードの状態のイベントの本文）だけです。 |
| コードの名前のままのもの | コードから見える名前（`ecu.network("J1939_CAN")` など）、SIL Kit のコントローラーの名前、`--describe` の `networks[].name` です。 |
| `--describe` と `--local` | どちらにも付けられません（終了コード 4）。`--describe` の出力は引数で変わりません。`--map-network` を受け取れる実行ファイルは、`features` に `"map-network"` を出します（[キー](#キー)）。`--local` での監視の出力は、コードの名前です。 |

終了コード 4 になる値: `=` がない、どちらかの名前が空、同じシミュレーションの名前を 2 回、同じ実行ファイルの名前を 2 回、ASCII でも UTF-8 でもない値、Windows では ASCII でない文字のある値。

終了コード 3 になる場合は次のとおりです。どれも、シミュレーションが始まる前に 1 行で伝えます。

| 場合 | 補足 |
|---|---|
| 実行ファイルの名前のネットワークを、ECU が持たない | メッセージに、ECU のネットワークを並べます。 |
| 替えた後で、ECU の 2 つのネットワークが同じ名前になる | たとえば、コードが `A` と `B` の両方を持つときの `--map-network B=A` です。入れ替え（`--map-network B=A --map-network A=B`）は書けます。 |
| `--silkit-config` の設定が、コントローラー（`CanControllers`、`LinControllers`、`EthernetControllers`、`FlexrayControllers`）に `Network` を書いている | SIL Kit は設定の `Network` を優先するので、どちらが効くか分からなくなります。名前を替えるネットワークと関係のないコントローラーでも拒否します。名前を替えない `--map-network` だけなら拒否しません。ソースからビルドしていない SIL Kit を使う vehicle-sil のビルドは、設定を読めないので、名前を替える `--map-network` と `--silkit-config` の併用を、設定の内容によらず拒否します（`PeerOptions::network_names` と participant の設定も同じです）。 |
| ECU が、替えた後の監視の出力と同じ名前の変数を宣言している | たとえば `j1939.VehicleCAN.bus_state` です。 |

Windows（MinGW）では、名前を替えるネットワークに ASCII の名前を使います。ASCII でない文字のある `--map-network` は終了コード 4 です。Windows の引数はコードページに変換されて届き、コードページにない文字は `?` や似た文字に置き換わるので、そのまま読むと、エラーなしに別の名前のネットワークに載ってしまいます。`ecu_main` は、この引数だけを変換前のコマンドラインで確認します（`main()` の `argc` と `argv` をそのまま渡したとき）。Linux では、ASCII か UTF-8 の名前を書けます（どちらでもないバイト列は終了コード 4）。

`vehicle_sil_launch` では、この引数を `command` に書かず、設定ファイルの participant の `network_names` に書きます（`network_names: { VehicleCAN: J1939_CAN }`。[simulation-config.md](simulation-config.md) の「ネットワークの名前を合わせる（`network_names`）」）。`run` が、ほかの引数の後ろに `--map-network` を追加して起動し、`check` は、自己申告のネットワークを対応の後の名前で照合します。ecu_main の実行ファイル（`describe: true`）の `command` に書くと、設定ファイルのエラー `launcher-argument` です。

`ecu_main` を使わないプログラムは、`SilKitOptions::network_names`（`Simulation`）か `PeerOptions::network_names`（`Peer`）に、同じ向き（シミュレーションの名前 → コードの名前）で渡します。1 つのプロセスに複数の ECU を載せた `Simulation` では、全部の ECU に同じ対応が効き、その名前のネットワークを持たない ECU には何もしません（持たない名前をエラーにするのは `ecu_main` だけです）。`Peer::j1939(name, port)`（呼び出し側が作ったポート）の名前には対応を書けません（`std::invalid_argument`。SIL Kit のネットワークにつながないので、替えるものがありません）。

### J1939 のノードのアドレスと NAME を替える（`--j1939-node`）

同じ ECU の実行ファイルを 1 つの J1939 のネットワークに 2 つ載せるときに、2 つ目に別のアドレスと NAME を、ビルドし直さずに与えます。与えないと、2 つが同じ NAME で同じアドレスを求め、どちらもアドレスを得られません。

```sh
engine_ecu --registry-uri silkit://localhost:8500 --name Engine2 --j1939-node J1939_CAN=address:0x01,identity_number:2
```

値は `<ネットワーク>=<設定>:<値>[,...]` です。ECU の J1939 のネットワークのシミュレーションでの名前（`--map-network` で名前を替えたネットワークは、替えた後の名前）、`=`、コンマで区切った設定の順に書きます。上の例では、`J1939_CAN` にある ECU のただ 1 つのノードが、コードのアドレスの代わりに 0x01 を求め、NAME の identity number が 2 になります。

| 設定 | 範囲 | 意味 |
|---|---|---|
| `from` | 0..254 | ECU のコードが `add_node()` で与えるアドレスで、ノードを指します。`from` がない行は、そのネットワークにある ECU のただ 1 つのノードに当たります。 |
| `address` | 0..254 | ノードが求めるアドレスです。254（空いているどのアドレスでも）は、NAME が arbitrary address capable のノードだけです。 |
| `identity_number` | 0..2097151 | ノードの NAME の identity number の欄です。 |
| `function_instance` | 0..31 | ノードの NAME の function instance の欄です。 |
| `ecu_instance` | 0..7 | ノードの NAME の ECU instance の欄です。 |

数は 10 進か、`0x` を付けた 16 進で書きます。同じ設定は 1 回までで、`from` のほかに 1 つ以上の設定が要ります。NAME の欄は、NAME を持つノードにだけ書けます。

| 項目 | ルール |
|---|---|
| 効く時点 | ECU を組み立てる間の `add_node()` の中です。ECU が始まった後に追加するノードは、コードの値のままです。 |
| 替わるもの | そのノードのアドレスと、行に書いた NAME の欄だけです。NAME のほかの欄はコードのままです。 |
| 替わらないもの | ほかの ECU がコードに持つ、このノードのこと（アドレス、NAME の全体）と、ECU 自身のコードのほかの定数です（[ECU を作る人に依頼すること](#ecu-を作る人に依頼すること)）。 |
| 複数のノード | ノードごとに 1 つの `--j1939-node` を書きます。ECU がそのネットワークに 2 つ以上のノードを持つときは、どの行にも `from` が要ります。 |
| `--describe` と `--local` | どちらにも付けられません（終了コード 4）。`--describe` は、コードのアドレスと NAME を出します。`--j1939-node` を受け取れる実行ファイルは、`features` に `"j1939-node"` を出します（[キー](#キー)）。 |

終了コード 4 になる値: `=` がない、ネットワークの名前が空か `-` で始まる、空の設定、値のない設定、知らない設定、同じ設定を 2 回、数でない値か範囲の外の値、`from` だけの行、ASCII でも UTF-8 でもない値（Windows では、`--map-network` と同じく、ASCII でない文字のある値）、同じノード（同じネットワークと `from`）への 2 つの行、`from` のない行と同じネットワークへのほかの行。

終了コード 3 になる場合は次のとおりです。どれも、ECU がシミュレーションに加わる前に 1 行で伝えます。

| 場合 | 補足 |
|---|---|
| その名前の J1939 のネットワークを、ECU が持たない | メッセージに、ECU の J1939 のネットワーク（名前を替えたものは、コードの名前も）と、シミュレーションがそのどれかを別の名前で呼ぶときに付ける `--map-network` を出します。 |
| ECU を組み立てたとき、そのネットワークにノードがない | |
| `from` のアドレスにノードがない | メッセージに、そこにある ECU のノードのアドレスを並べます。`from` は ECU のコードのアドレスで、新しいアドレスではありません。 |
| 行がノードを選べない | `from` のない行で、ECU がそのネットワークに 2 つ目のノードを追加する。または、コードで同じアドレスの 2 つのノード。 |
| 行をノードに当てられない | NAME のないノードへの NAME の欄、または NAME が arbitrary address capable でないノードへの 254。 |

`ecu_main` を使わないプログラムは、同じ行を `SilKitOptions::j1939_nodes`（`Simulation`。ECU の名前ごと）か `PeerOptions::j1939_nodes`（`Peer`）に渡します。[j1939-guide.md](j1939-guide.md#sil-kit-の設定silkitoptions) の「SIL Kit の設定（`SilKitOptions`）」にあります。

## ECU を作る人に依頼すること

`vehicle_sil_launch` で複数の ECU を動かすとき、ECU の実行ファイルに求めることです。守らないと起きることは、ランチャーの指摘か表示に現れます。

| 依頼すること | 守らないと起きること |
|---|---|
| `main()` を `ecu_main()` で作ります。 | `check` がその ECU を照合できません。 |
| 設定ファイルを書く人と決めた名前（ネットワーク名など）を使い、渡す前に `--describe` で確認します。 | `check` の指摘になります（[simulation-check.md](simulation-check.md)）。 |
| `vehicle_sil_launch` と同じバージョンの vehicle-sil でビルドします。 | 注記 `version-mismatch` が出て、変数を読み書きするメッセージの形式のバージョンも違えば、警告 `control-link-version` でその ECU の変数を読み書きできません。 |
| ECU が自分で起動したプロセスは、ECU が終わるときに終了させます。 | 警告 `left-processes` が出ます（[simulation-run.md](simulation-run.md) の「止め方」）。 |
| 標準出力と標準エラーは UTF-8 で出力します。 | `vehicle_sil_launch run` は、UTF-8 でないバイトを `?` と表示します。 |
| 変数の名前に、次の表の接頭辞を使いません。 | ECU が終了コード 3 で終わります。 |
| ノードの自分のアドレスは `node.address()` で読み、別の定数に持ちません。ほかの ECU のアドレスと NAME の全体を定数に持ちません。ほかの ECU は、`Network::name_at()` が伝える NAME を identity number と 2 つの instance の欄を除いて比べて探すか、アドレスか NAME を引数で受け取ります。 | シミュレーションが `--j1939-node` でアドレスと NAME を替えたとき、ECU が正しくないアドレスで送ったり、正しくないアドレスへ送ったりします。 |

### 使えない変数名の接頭辞

次の接頭辞で始まる名前は、vehicle-sil が追加する変数に使います。

| 接頭辞 | 使う変数 | 例 |
|---|---|---|
| `ecu.` | どの ECU にもある変数 | `ecu.connected` |
| `peer.` | `Peer` が追加する監視用の変数 | `peer.steps` |
| `j1939.<ネットワーク名>.` | `Peer` が J1939 のネットワークごとに追加する監視用の変数 | `j1939.J1939_CAN.frames_sent` |

監視用の変数は、SIL Kit のシミュレーションに参加する ECU（`Peer`）が、ECU の宣言した変数の後に追加します（`PeerOptions::monitor`、既定で有効）。`--describe` の `variables` には含まれませんが、実行中は ECU の変数と並んで見えます。変数の一覧は [internals/control.md](internals/control.md) の「Peer の監視」にあります。

重なる名前を宣言したときにエラーになる時点は、宣言のしかたと接頭辞で違います。

| 宣言 | エラーになる時点 |
|---|---|
| `Ecu` の `input()`、`output()` などで、`ecu.` か `peer.` で始まる名前を宣言する | 宣言のときに `std::invalid_argument` になるので、`--describe` と `--local` でも見つかります。 |
| `agent()` に直接登録した名前が、`Peer` の監視用の変数と重なる | 登録のときは検査せず、SIL Kit で始めるときに見つかります（`ecu_main` では終了コード 3）。 |
| `j1939.<ネットワーク名>.` で始まる監視用の変数と重なる名前を宣言する | SIL Kit で始めるときにだけ見つかります。 |

`input()` などの `std::invalid_argument` は、`Peer` でも `LocalBench` でも同じで、メッセージに変数の名前とルールが入ります。変数名は ECU ごとに独立なので、ECU の間でそろえる必要はありません。

## `Ecu::describe()`

`Ecu::describe()` は、ECU の構成を `EcuDescription`（`vehicle_sil/ecu/describe.hpp`）で返します。

| メンバー | 内容 |
|---|---|
| `name` | ECU の名前（SIL Kit では participant 名）。 |
| `step` | ステップ幅。実時間の `Peer` では空です。 |
| `networks` | ECU が接続するネットワーク。 |
| `networks` の `mac` | イーサネットだけ。`ethernet()` に渡した自分のアドレス（`eth::ChannelOptions::mac`）。 |
| `networks` の `nodes` | J1939 だけ。ノードを追加した順に、追加したときの値（`NodeOptions`）。 |
| `variables` | 変数の `control::VariableInfo`。 |
| `control`、`features` | `Ecu::describe()` は空のまま返します。`ecu_main` が `--describe` のときに入れます（`control` は [変数を公開する](#変数を公開する--registry-uri)、`features` は [キー](#キー)）。 |

**`networks`**: `j1939()`、`can()`、`lin_master()`、`lin_slave()`、`ethernet()` を呼んだ順に、種類をまたいで並びます。1 回の呼び出しが 1 つの要素です。同じ ECU が同じ名前で `j1939()` と `can()` を持つと、同じ CAN のネットワークに 2 つ並びます（コントローラーが 2 つあるため）。各要素は `name`、`use`（`NetworkUse`: `Raw`、`J1939`、`LinMaster`、`LinSlave`、`Ethernet`）、`kind()`（`use` から決まる `NetworkKind`: `Can`、`Lin`、`Ethernet`）、`nodes`、`mac` です。

**`mac`**: `ethernet()` にアドレスを渡さなければ空です（アドレスをアプリケーションが実行中に決める ECU）。チャネルはこの値をフレームに書き込みません。照合が、同じネットワークの ECU のアドレスの重なりを確認するのに使います。

**`nodes`**: `address` は、ノードが求めたアドレスです。Address Claim で別のアドレスになった後も変わりません（今のアドレスは `Node::address()`、求めたアドレスは `Node::requested_address()`）。`j1939::kNullAddress`（254）は、任意アドレスの範囲から空いているものを取るノードです。`name` は NAME です（NAME を持たないノードは空。JSON のキーは `j1939_name`）。`arbitrary_address_capable()` は、NAME の任意アドレスの bit です。

**`variables`**: 先頭は、どの ECU にもある入力 `ecu.connected` で、Agent を作る前でも並びます。その後に、ECU が宣言した変数（`input()`、`output()` などと、`agent()` に直接登録したもの）が宣言した順に並びます。`Peer` が追加する監視用の変数（`PeerOptions::monitor`）は含みません。

`ecu.connected` は bool の入力で、`false` の間、ECU はすべてのバスから切断されます。ステップは動き続けます（[internals/execution.md](internals/execution.md) の「`Ecu` クラス」の「バスからの切断」）。

`describe()` を呼べる時点と、返す内容のルールは次のとおりです。

| 項目 | ルール |
|---|---|
| 呼べる時点 | いつでも呼べます（組み立ての途中でも、実行の前でも後でも）。 |
| 実行中 | その ECU のステップの中からだけ呼びます（ほかの関数と同じです）。 |
| 宣言への影響 | 宣言は閉じません（Agent のスキーマを確定しないので、後から変数、ネットワーク、ノードを追加できます）。 |
| 実装による違い | 同じクラスで同じように組み立てた ECU は、`Peer`（仮想時間）と `LocalBench` で同じ内容を返します（`name` と `step` を含めて）。 |
| 実行の前後 | 実行の前と後でも同じ内容です。 |
| 比較 | `EcuDescription` とそのメンバーの型は `==` で比べられます。 |

## JSON の形式

`to_json(const EcuDescription&)`（`vehicle_sil/ecu/describe.hpp`）は、`EcuDescription` を JSON のオブジェクトにします。`--describe` が書くのはこの文字列です。同じ内容からは、同じバイト列ができます。

| 項目 | 書き方 |
|---|---|
| キーの順 | 下の表のとおりです。 |
| 値のないキー | 省きません（`null`、空の文字列、空の配列）。 |
| 字下げと行 | 字下げは空白 2 つで、1 行に 1 つのキーか要素を書き、最後は改行です。 |
| 改行 | LF です（`--describe` は Windows でも `\r\n` にしません）。 |
| 整数 | 10 進で、桁を落としません。 |
| 浮動小数 | 読み戻すと同じ `double` になる最も短い書き方です（`std::to_chars`。例: `0.1`、`100`、`1e+300`）。 |
| 有限でない値 | `null` です。 |
| 時間 | 整数のナノ秒です（`step_ns`）。 |
| NAME | `"0x"` と 16 桁の大文字の 16 進の文字列です（キー `j1939_name`。64 ビットの値を識別子として扱うため）。 |
| 文字列 | 正しい UTF-8 ならそのまま書き、`"`、`\`、0x20 未満の制御文字はエスケープします（`\b`、`\f`、`\n`、`\r`、`\t`、ほかは `\u00XX`）。 |
| 正しい UTF-8 の並びを始めないバイト | 1 バイトごとに U+FFFD にします（出力は、いつも正しい JSON です）。 |
| 範囲外の列挙値（キャストで作った `NetworkUse` など） | 名前を作らずに `std::logic_error` です。 |

列挙の値は 64 ビットの整数で、2^53 を超えることがあります。整数として読んでください。

### 形式のバージョン

先頭の `vehicle_sil_ecu` は形式のバージョンです。

| 変更 | `vehicle_sil_ecu` |
|---|---|
| キーの意味が変わる、キーがなくなる | 上がります。 |
| キーが増える | 上がりません。 |
| 決まった値をとるキー（`kind`、`use`、`type`、`causality`）に値が増える | 上がりません。 |
| `features` に名前が増える | 上がりません。 |

読む側は、知らないバージョンを拒否し、知らないキーは無視します。決まった値をとるキーの知らない値は、その ECU のエラーにします（`vehicle_sil_launch check` の指摘 `describe-unknown-value`。ほかの participant の照合は続けます）。新しい vehicle-sil の ECU を古い `vehicle_sil_launch` で読むと、そうなります。`features` は決まった値をとるキーではなく、開いた一覧です。読む側は、知らない名前を無視します。

`vehicle_sil` は、ECU をビルドした vehicle-sil のバージョン（CMake のプロジェクトのバージョン）です。別々にビルドした実行ファイルを 1 つのシミュレーションで動かすときに、バージョンの違いを見つけるのに使います。

### キー

| キー | 値 |
|---|---|
| `vehicle_sil_ecu` | 形式のバージョン（今は 1） |
| `vehicle_sil` | vehicle-sil のバージョンの文字列 |
| `name` | ECU の名前 |
| `step_ns` | ステップ幅（ナノ秒）。実時間の `Peer` では `null` |
| `networks` | ネットワークの配列（`describe()` の順） |
| `networks[].name` | ネットワークの名前 |
| `networks[].kind` | `"can"`、`"lin"`、`"ethernet"` |
| `networks[].use` | `"raw"`、`"j1939"`、`"lin_master"`、`"lin_slave"`、`"ethernet"` |
| `networks[].nodes` | J1939 のノードの配列（`use` が `"j1939"` のときだけ要素がある） |
| `networks[].nodes[].address` | 求めたアドレス（0〜253、254 は任意アドレスの範囲から取る） |
| `networks[].nodes[].j1939_name` | NAME（`"0x"` と 16 桁）。NAME のないノードは `null` |
| `networks[].nodes[].arbitrary_address_capable` | NAME の任意アドレスの bit（NAME がなければ `false`） |
| `networks[].mac` | イーサネットのネットワークでの ECU のアドレス（`"02:00:00:00:00:01"` の形、小文字）。出力していないときと、イーサネット以外のネットワークでは `null` |
| `variables` | 変数の配列（`ecu.connected`、続いて宣言の順） |
| `variables[].name` | 変数の名前 |
| `variables[].type` | `"bool"`、`"integer"`、`"real"`、`"enum"` |
| `variables[].causality` | `"input"`、`"output"`、`"parameter"` |
| `variables[].unit` | 単位（なければ `""`） |
| `variables[].min`、`max` | 範囲（両端を含む）。なければ `null` |
| `variables[].step` | 値を編集するときの増減の幅の目安。なければ `null` |
| `variables[].description` | 説明（なければ `""`） |
| `variables[].items` | `"enum"` の値の配列（`name` と、64 ビットの整数の `value`）。ほかの型では空 |
| `variables[].builtin` | `true` は vehicle-sil がどの ECU にも付ける変数（名前が `ecu.` で始まる。`ecu.connected`）、`false` は ECU が宣言した変数 |
| `control` | この実行ファイルが `--registry-uri` のときに変数をシミュレーションに公開するなら、その方法。公開しない（`--no-control`）なら `null` |
| `control.transport` | トランスポート。今は `"silkit"`（SIL Kit の publish/subscribe）だけ |
| `control.wire_version` | メッセージの形式のバージョン（整数） |
| `features` | この実行ファイルが、起動する側から受け取れる引数や動きの名前の配列（文字列）。`"map-network"` は `--map-network` を、`"j1939-node"` は `--j1939-node` を受け取ります。`ecu_main` の実行ファイルは、どの引数で呼んでも同じ一覧を出します |

キーのない JSON の読み方は次のとおりです。

| キー | ないとき |
|---|---|
| `networks[].mac` | 前のバージョンの ECU の JSON です。`null` として読みます。 |
| `control` | このキーができる前の vehicle-sil でビルドした実行ファイルです。`null` と同じに扱います。 |
| `features` | 前のバージョンの ECU の JSON です。空の配列として読みます。 |

`variables[].builtin` は、画面などが、アプリの変数と分けて表示するのに使います。`control.wire_version` が `vehicle_sil_launch` のバージョンと違えば、変数を読み書きできません。

例: `vehicle_sil_engine_ecu --describe` の出力です（CTest の `ecu_main.describe` が、実行ファイルの出力とこのブロックをバイト単位で照合します）。

<!-- vehicle_sil_engine_ecu --describe -->
```json
{
  "vehicle_sil_ecu": 1,
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
  ],
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
  "control": {
    "transport": "silkit",
    "wire_version": 1
  },
  "features": [
    "map-network",
    "j1939-node"
  ]
}
```

## 複数の ECU を 1 つのシミュレーションで動かす

2 つの ECU の実行ファイルを 1 つの SIL Kit のシミュレーションで動かすには、設定ファイルを書いて `vehicle_sil_launch run` に渡します（[simulation-config.md](simulation-config.md)、[simulation-run.md](simulation-run.md)）。`run` は、照合してから、registry、controller、各 ECU を起動し、終わりまで監視して、各 ECU がどう終わったかを表示します。

```sh
cd build/<preset>/examples/ecu_main
vehicle_sil_launch run simulation.yaml
```

実行する前に、ECU の名前、ステップ幅、ネットワーク、J1939 のアドレスと NAME に不整合がないかを、設定ファイルと `vehicle_sil_launch check` で確認できます（[simulation-check.md](simulation-check.md)）。手順は [複数の ECU をつないで動かす](howto/integrate-ecus.md) にあります。

### ランチャーを使わずに動かす

同じことを、ランチャーを使わずにコマンドで組み立てる例です。ECU は `--registry-uri` で参加し、`vehicle_sil_sim_controller` が全員の参加を待って始め、仮想時間 1 秒で止めます。

```sh
sil-kit-registry --listen-uri silkit://localhost:8500 &
vehicle_sil_engine_ecu --registry-uri silkit://localhost:8500 &
vehicle_sil_dashboard_ecu --registry-uri silkit://localhost:8500 &
vehicle_sil_sim_controller --registry-uri silkit://localhost:8500 --required Engine,Dashboard --stop-at-ms 1000
```

`--required` には、各 ECU の名前（`--describe` の `name`）を並べます。

## 関連するドキュメント

| 調べること | ドキュメント |
|---|---|
| ECU のクラスの書き方 | [j1939-guide.md](j1939-guide.md) |
| ECU を 1 つ作って動かす手順 | [最初の ECU を作る](howto/first-ecu.md) |
| 設定ファイルのキーと `vehicle_sil_launch` の引数 | [simulation-config.md](simulation-config.md) |
| 照合の順序と指摘のコード | [simulation-check.md](simulation-check.md) |
| 実行、止め方、結果 | [simulation-run.md](simulation-run.md) |
