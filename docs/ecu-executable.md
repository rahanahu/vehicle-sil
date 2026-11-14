# ECU の実行ファイル（`ecu_main`）と自己申告（`--describe`）

ECU を書く人（自分の ECU を実行ファイルにする）と、まとめる人（実行ファイルの引数と `--describe` を知る）向けです。ECU を書く人は、先に [j1939-guide.md](j1939-guide.md) を読みます。次は、まとめる人なら [simulation-config.md](simulation-config.md)（構成ファイル）、[simulation-check.md](simulation-check.md)（照合）、[simulation-run.md](simulation-run.md)（実行）です。

ECU のクラス（`Ecu&` を受け取るクラス）を 1 つの実行ファイルにする `main()` のひな形と、ECU が自分の構成を JSON で報告する形式です。各自が自分の ECU を実行ファイルとしてビルドし、別の人がそれを 1 つの SIL Kit のシミュレーションにまとめる、という分担に使います。どの ECU の実行ファイルも同じ引数を持つので、まとめる人は中身を知らなくても動かせます。API はまだ変わることがあります。

```cpp
#include "vehicle_sil/simulation/ecu_main.hpp"

int main(int argc, char** argv)
{
    return vehicle_sil::ecu_main<Engine>(argc, argv, {.name = "Engine", .step = 10ms});
}
```

完全な例は [examples/ecu_main](../examples/ecu_main)（`vehicle_sil_engine_ecu` と `vehicle_sil_dashboard_ecu`）です。

## `ecu_main`

- `ecu_main<App>(argc, argv, EcuMainOptions, args...)` は、ECU に `App{ecu, args...}` を持たせて（`Ecu::emplace()`）、引数のとおりに動かします。`args` は `App` のコンストラクターへそのまま渡します（1 回だけ作るので、ムーブしかできない値も渡せます）。
- `ecu_main(argc, argv, EcuMainOptions, std::function<void(Ecu&)>)` は、クラスの代わりに関数で ECU を組み立てます。関数は 1 回だけ、何かを動かす前に呼ばれます。
- `EcuMainOptions`: `name`（ECU の名前。SIL Kit の participant 名で、`--describe` の `name`）、`step`（ステップ幅。0 より大きいこと）、`last_word_wait`（`--registry-uri` で終わるときに、取り消された書き込みなどを運ぶ最後の知らせへのランチャーの返事を待つ、いちばん長い時間。既定 500 ms、0 で待たない。返事はふつうすぐ来るので、変える必要があるのは、返事に 500 ms 以上かかるほど遅い機械だけです。引数にはありません）。
- ライブラリは `vehicle_sil::simulation` です。ヘッダーは SIL Kit のヘッダーを含みません。

### 引数

| 引数 | すること |
|---|---|
| `--describe` | ECU をプロセス内の仮想バスの上で組み立て、何も動かさず、どこにも接続せずに、`to_json(Ecu::describe())` を標準出力に書いて終了します。 |
| `--registry-uri <uri>` | `<uri>` の registry の SIL Kit のシミュレーションに、Coordinated で仮想時間の participant として参加し、シミュレーションが終わるまで動きます。始めるのと止めるのは、ほかのプロセスの controller（`vehicle_sil_sim_controller`）です。`<uri>` のホストがループバックのアドレス（`127.0.0.1`、`localhost` など）なら、ほかの participant からの接続をループバックのアドレスだけで待ち受けます（環境変数 `VEHICLE_SIL_LISTEN_ON_NETWORK=1` で全部のアドレス。[simulation-run.md](simulation-run.md#待ち受けるアドレス)）。 |
| `--start-timeout-ms <ms>` | `--registry-uri` のときだけ。参加してからシミュレーションが始まるまで待つ時間（1 以上の整数。既定は 30000）です。それまでに始まらなければ、終了コード 3 です。ECU をたくさん起動するときや、遅いマシンで使います。 |
| `--no-control` | `--registry-uri` のとき、ECU の変数をシミュレーションへ出しません（下の「変数を出す」）。ECU の動きは変わりません。`--describe` に付けると、`variables` はそのままで、`control` が `null` になります（まとめる側は、この ECU を待ちません）。`--local` に付けても何も変わりません。 |
| `--local --run-for-ms <ms>` | この ECU だけを、プロセス内の仮想バスの上で、仮想時間で `<ms>` ミリ秒（1 以上の整数）動かします。ECU を単体で確かめるときに使います。 |
| `--name <name>` | `EcuMainOptions::name` の代わりの participant 名です（ASCII か UTF-8）。`--describe` の `name` にも効きます。変わるのは名前だけで、J1939 の NAME、アドレス、変数の名前は同じです。同じ ECU を 2 つ載せるときは、それらがぶつからないようにしてください（やり方は [ECU をまとめて動かす](howto/integrate-ecus.md) の「同じ ECU を 2 つ載せる」）。 |
| `--help`（`-h`） | 使い方を標準出力に書いて、0 で終了します。ほかの引数に誤りがあっても使い方を書きます。ただし、値を取る引数の後ろの `-h` は値です（`--name -h` は名前が `-h`）。 |

- `--describe`、`--registry-uri`、`--local` のどれか 1 つが要ります。どれもなければ（引数がないときも）、何も始めずに使い方を標準エラーに書き、終了コード 4 です。
- 次の場合も終了コード 4 です。知らない引数（`--name=X` の形も。値は空白で区切ります）、値のない `--name`、`--registry-uri`、`--run-for-ms`、`--start-timeout-ms`（次が `--` で始まる引数のときも）、空の値、ASCII でも UTF-8 でもない `--name`（Windows のコードページの名前など。文言にバイト列を 16 進で出します）、同じ引数の 2 回目、3 つのうち 2 つ以上、`--run-for-ms` のない `--local`、`--local` のない `--run-for-ms`、`--registry-uri` のない `--start-timeout-ms`、整数でない、0 の、または 10^12 を超える `--run-for-ms` と `--start-timeout-ms`。
- 終了コードは、ほかのツールと同じです。

| 終了コード | 意味 |
|---|---|
| 0 | 終わった（`--describe` は JSON を書いた。`--registry-uri` はシミュレーションが終わった。`--local` は時間まで動いた） |
| 1 | 実行中に ECU が失敗した（ステップの例外。`SimulationError`） |
| 3 | 準備に失敗した（`App` のコンストラクターの例外、`EcuMainOptions` の誤り、registry に接続できない、シミュレーションが始まらない、標準出力に書けない）。registry に接続できないときの文（`cannot join the simulation at "<URI>": ...`）は、確かめること（`vehicle_sil_launch run` を先に起動して待たせる、URI のアドレスとポート、そのコンピューターのファイアウォール、同じ名前の participant）を挙げます。registry が名前を使用中として断ったときは、5 秒（`--start-timeout-ms` がそれより短ければ、その時間）作り直してから失敗します（同じ名前ですぐに起動し直したとき、registry が前のプロセスの終わりにまだ気づいていないことがあるため）。そのときの文は、待った時間と名前のことだけを言います（registry には届いているので、確かめることの一覧は付けません） |
| 4 | 引数の誤り |
| 130 | `--registry-uri` で、止まるよう求められた（下の「止まるよう求められたとき」） |

- 失敗は、標準エラーに 1 行 `error: <ECU 名>: <理由と直し方>` を書きます（SIL Kit の文言の改行は空白にします。`std::exception` でない例外も、固定の文言で 1 行にします）。ECU 名は `--name` で変えた名前です（引数の誤りのときは `EcuMainOptions::name`）。引数の誤りでは、続けて使い方を書きます。
- シミュレーションが始まらないときの文言は、時間切れ（controller がいない、必要な participant がそろわない。controller と `--start-timeout-ms` の確かめ方を添えます）と、それ以外（例: 変数の名前が `Peer` の監視の出力とぶつかる）で分けます。
- `ecu_main` が標準出力に書くのは、`--describe` の JSON と `--help` の使い方だけです。`--describe` では、ECU を組み立てる前から、プロセスが終わるまで、標準出力（fd 1）を標準エラーに向けます（元に戻しません）。`App` のコンストラクターとデストラクター、`App` のスレッド、`atexit`、リンクしたライブラリが標準出力に書いたもの（`printf`、`std::cout` など）は、標準エラーに出ます。JSON は、元の標準出力を複製した記述子に直接書きます。標準エラーが閉じていれば、先に null デバイスを開いて標準エラーにするので、`App` の出力はどこにも出ません。標準出力が閉じていれば、JSON を書けないので終了コード 3 です。`ecu_main` が戻った後も fd 1 は標準エラーのままなので、`ecu_main` は `main()` の最後で呼びます。

### 変数を出す（`--registry-uri`）

ECU は（変数を宣言していなくても、`ecu.connected` があります）、`--registry-uri` で参加するとき、何も書かなくても、その変数を SIL Kit の publish/subscribe でシミュレーションへ出します（`vehicle_sil::control_silkit::AgentTransport`。[internals/control.md](internals/control.md) の「SIL Kit の上の運び手」）。`vehicle_sil_launch run` がそれを受けて、変数を読み書きする側になります（[simulation-run.md](simulation-run.md) の「変数を読み書きする口」）。受ける側がいなければ、送ったものは誰にも届かないだけです。

- gRPC も Protocol Buffers も要りません（ECU の実行ファイルは、どちらにも依存しません）。
- 値と書き込みの結果は、ステップの中で、値を読んだ直後に送ります。送る頻度は Agent の `sample_period`（既定 10 ms。`Ecu::set_agent_options()`）です。間引きはしません。
- 出すことは `--describe` の `control` で自己申告します（`Ecu::describe()` 自体は申告しません。`ecu_main` が足します）。まとめる側は、申告した ECU だけを待ちます。
- `--no-control` で出さなくなります（`--describe` の `control` も `null` になります）。アプリが Agent に自分の通信の側を付けるときは、`--no-control` を付けてください（`--describe` の時点で通信の側が付いていれば `ecu_main` は申告しませんが、付いていなければ「出す」と申告し、まとめる側が hello を待ちます）。アプリが自分で通信の側（`control::TransportLease` を持つもの）を Agent に付けているときも、`ecu_main` は付けません。
- この区間に認証はありません（registry に参加できる者は、変数に書けます）。

### 止まるよう求められたとき（`--registry-uri`）

`--registry-uri` のとき、`ecu_main` は SIGINT（Ctrl+C）と SIGTERM（Windows: Ctrl+C、Ctrl+Break、コンソールを閉じる）を受けます。

- シミュレーションが始まった後: participant を止めます（`request_stop()`）。coordinated の participant が止まると、シミュレーション全体が順に止まります。`run()` が戻り、`App` のデストラクターなどが動いてから、`error: <ECU 名>: interrupted (SIGTERM): it stopped the simulation and left it` を書いて、終了コード 130 で終わります。
- 始まる前（registry への接続中、開始の待ち）: `error: <ECU 名>: interrupted (SIGTERM) before the simulation started: it ends at once` を書いて、すぐに終了コード 130 で終わります。デストラクターは動きません（既定の動作と同じです）。終わる前に C の標準入出力（`printf` など）のバッファーは書き出します。
- 止めている間の 2 回目も、すぐに終わります。
- 受けるのは `ecu_main` の中で `--registry-uri` のシミュレーションが動いている間だけです。`App` のデストラクターが動くときには、前の設定に戻しています。`--describe` と `--local` では扱いません（既定の動作ですぐに終わります）。
- その間、これらのシグナルは `ecu_main` のものです（`SA_RESTART` で、`App` の `read()` などを途中で戻しません）。起動した側が無視するように設定したシグナル（`nohup`）は、無視したままです。シグナルを自分で扱いたい ECU は、`ecu_main` を使わず、`Simulation` を直接使います。
- Windows でコンソールを閉じたときは、順に止める処理が終わるのを、最大 4 秒待ちます（その後 Windows がプロセスを終わらせます）。
- `vehicle_sil_launch run` は、シミュレーションが終わっても終わらない participant や、始まらなかったときの participant に、この方法で止まるよう求めます（[simulation-run.md](simulation-run.md) の「止め方」）。

### 実行中にほかの参加者がいなくなったとき（`--registry-uri`）

`ecu_main` が自分で終わるのは、シミュレーションが終わったとき（終了コード 0）と、この ECU のステップが失敗したとき（1）だけです（ほかに、止まるよう求められたとき（130））。ステップが失敗した ECU は、Error の状態のまま、controller がシミュレーションを中断するのを最大 5 秒待ってから終わります（controller が失敗を Error として見られるようにするためです。Error を見るとすぐ中断する controller の下では、待ちはその間だけです）。

- controller のプロセスがなくなると、シミュレーションは止まらずに、仮想時間が進み続けます。ECU は自分では終わりません。
- registry のプロセスがなくなると、participant の間の接続によっては、ほかの participant から見てこの ECU が Error になり、この ECU は仮想時間が進まないまま待ち続けることがあります。
- ほかの participant が失敗し、controller がシミュレーションを中断すると、この ECU は終了コード 0 で終わります。どれが失敗したかは、controller（`vehicle_sil_sim_controller` の出力と終了コード 1）が知らせます。
- どの場合も、ECU を起動した側が、controller の終わりを見てプロセスを止めてください。`vehicle_sil_launch run` はこれをします（[simulation-run.md](simulation-run.md)）。自分で起動するときの決まりは次の 2 つです。
  - シミュレーションの結果は controller の終了コードです。ECU の終了コードだけで決めません（ほかの participant が失敗しても、ECU は 0 で終わることがあります）。ECU の終了コード 1 は、その ECU のステップが失敗したことだけを示します。
  - controller のプロセスが結果を出さずになくなったら、すべての ECU を止めます（自分では終わりません）。controller が終わった後もしばらく動いている ECU（registry がなくなった場合など）も止めます。まず SIGTERM（Windows: Ctrl+Break）で止まるよう求め（上の「止まるよう求められたとき」）、それでも終わらなければ強制終了します。

## `Ecu::describe()`

`Ecu::describe()` は、ECU の構成を `EcuDescription`（`vehicle_sil/ecu/describe.hpp`）で返します。

- `name`: ECU の名前（SIL Kit では participant 名）。
- `step`: ステップ幅。実時間の `Peer` では空です。
- `networks`: ECU がつながるネットワーク。`j1939()`、`can()`、`lin_master()`、`lin_slave()` を呼んだ順に、種類をまたいで並びます。1 回の呼び出しが 1 つです。同じ ECU が同じ名前で `j1939()` と `can()` を持つと、同じ CAN のネットワークに 2 つ並びます（コントローラーが 2 つあるため）。各要素は `name`、`use`（`NetworkUse`: `Raw`、`J1939`、`LinMaster`、`LinSlave`）、`kind()`（`use` から決まる `NetworkKind`: `Can` か `Lin`）、`nodes` です。
- `nodes`（J1939 だけ）: ノードを足した順に、足したときの値（`NodeOptions`）です。`address` は求めたアドレスで、Address Claim で別のアドレスになった後も変わりません（今のアドレスは `Node::address()`、求めたアドレスは `Node::requested_address()`）。`j1939::kNullAddress`（254）は、任意アドレスの範囲から空いているものを取るノードです。`name` は NAME（NAME を持たないノードは空。JSON のキーは `j1939_name`）、`arbitrary_address_capable()` は NAME のその bit です。
- `variables`: 変数の `control::VariableInfo` です。先頭は、どの ECU にもある入力 `ecu.connected`（bool。false の間、ECU はすべてのバスから外れます。ステップは動き続けます。[internals/execution.md](internals/execution.md) の「ECU の入口」の「バスからの切り離し」）で、Agent を作る前でも並びます。その後に、ECU が宣言した変数（`input()`、`output()` などと、`agent()` に直接登録したもの）が宣言した順に並びます。`Peer` が足す監視の出力（`PeerOptions::monitor`）は含みません。
- `ecu.` で始まる変数の名前は、どの ECU にもある変数のために予約しています。`peer.` で始まる変数の名前は、`Peer` の監視の出力のために予約しています。`Ecu` の `input()`、`output()` などは、どの実装でも宣言のときに `std::invalid_argument`（変数の名前と決まりを言う）にするので、`--describe` と `--local` でも見つかります。`agent()` に直接登録した名前は検査しません。それが `Peer` の監視の出力とぶつかると、SIL Kit で始めるときに見つかります（`ecu_main` では終了コード 3）。`Peer` のネットワークごとの監視の出力（`j1939.<network>.` で始まる名前）とぶつかる名前は、SIL Kit で始めるときにだけ見つかります（`PeerOptions::monitor`）。
- `EcuDescription` とその部品は `==` で比べられます。
- いつでも呼べます。組み立ての途中でも、実行の前でも後でもかまいません。宣言は閉じません（Agent のスキーマを確定しないので、後から変数、ネットワーク、ノードを足せます）。実行中は、その ECU のステップの中からだけ呼びます（ほかの関数と同じです）。
- 同じクラスで同じように組み立てた ECU は、`Peer`（仮想時間）と `LocalBench` で同じ内容を返します（`name` と `step` を含めて）。実行の前と後でも同じです。

## JSON の形式

`to_json(const EcuDescription&)`（`vehicle_sil/ecu/describe.hpp`）は、`EcuDescription` を JSON のオブジェクトにします。`--describe` が書くのはこの文字列です。

- 同じ内容からは、同じバイト列ができます。キーの順は下の表のとおりで、値がなくてもキーは省きません（`null`、空の文字列、空の配列）。字下げは空白 2 つで、1 行に 1 つのキーか要素を書き、最後は改行です。改行は LF です（`--describe` は Windows でも `\r\n` にしません）。
- 整数は 10 進で、桁を落としません（列挙の値は 64 ビットの整数で、2^53 を超えることがあります。整数として読んでください）。浮動小数は、読み戻すと同じ `double` になる最も短い書き方です（`std::to_chars`。例: `0.1`、`100`、`1e+300`）。有限でない値は `null` です。
- 時間は整数のナノ秒です（`step_ns`）。NAME は `"0x"` と 16 桁の大文字の 16 進の文字列です（キー `j1939_name`。64 ビットの値を識別子として扱うため）。
- 文字列は、正しい UTF-8 ならそのまま書き、`"`、`\`、0x20 未満の制御文字はエスケープします（`\b`、`\f`、`\n`、`\r`、`\t`、ほかは `\u00XX`）。正しい UTF-8 の並びを始めないバイトは、1 バイトごとに U+FFFD にします。出力は、いつも正しい JSON です。
- 範囲外の列挙値（キャストで作った `NetworkUse` など）は、名前を作らずに `std::logic_error` です。
- 先頭の `vehicle_sil_ecu` は形式の版です。キーの意味が変わるときと、キーがなくなるときに上がります。キーが増えるときと、決まった値をとるキー（`kind`、`use`、`type`、`causality`）に値が増えるときは上がりません。読む側は、知らない版を断り、知らないキーは無視し、決まった値をとるキーの知らない値は、その ECU の誤りにします（`vehicle_sil_launch check` の所見 `describe-unknown-value`。ほかの participant の照合は続けます）。新しい vehicle-sil の ECU を古い `vehicle_sil_launch` で読むと、そうなります。
- `vehicle_sil` は、ECU をビルドした vehicle-sil の版（CMake のプロジェクトの版）です。別々にビルドした実行ファイルをまとめるときに、版の違いを見つけるのに使います。

| キー | 値 |
|---|---|
| `vehicle_sil_ecu` | 形式の版（今は 1） |
| `vehicle_sil` | vehicle-sil の版の文字列 |
| `name` | ECU の名前 |
| `step_ns` | ステップ幅（ナノ秒）。実時間の `Peer` では `null` |
| `networks` | ネットワークの配列（`describe()` の順） |
| `networks[].name` | ネットワークの名前 |
| `networks[].kind` | `"can"` か `"lin"` |
| `networks[].use` | `"raw"`、`"j1939"`、`"lin_master"`、`"lin_slave"` |
| `networks[].nodes` | J1939 のノードの配列（`use` が `"j1939"` のときだけ要素がある） |
| `networks[].nodes[].address` | 求めたアドレス（0〜253、254 は任意アドレスの範囲から取る） |
| `networks[].nodes[].j1939_name` | NAME（`"0x"` と 16 桁）。NAME のないノードは `null` |
| `networks[].nodes[].arbitrary_address_capable` | NAME の任意アドレスの bit（NAME がなければ `false`） |
| `variables` | 変数の配列（`ecu.connected`、続いて宣言の順） |
| `variables[].name` | 変数の名前 |
| `variables[].type` | `"bool"`、`"integer"`、`"real"`、`"enum"` |
| `variables[].causality` | `"input"`、`"output"`、`"parameter"` |
| `variables[].unit` | 単位（なければ `""`） |
| `variables[].min`、`max` | 範囲（両端を含む）。なければ `null` |
| `variables[].step` | 編集の刻みの目安。なければ `null` |
| `variables[].description` | 説明（なければ `""`） |
| `variables[].items` | `"enum"` の値の配列（`name` と、64 ビットの整数の `value`）。ほかの型では空 |
| `variables[].builtin` | `true`: vehicle-sil がどの ECU にも付ける変数（名前が `ecu.` で始まる。`ecu.connected`）。`false`: ECU が宣言した変数。画面などが、アプリの変数と分けて見せるのに使います |
| `control` | この実行ファイルが `--registry-uri` のときに変数をシミュレーションへ出すなら、その出し方。出さない（`--no-control`）なら `null`。このキーができる前の vehicle-sil でビルドした実行ファイルには、このキーがありません（読む側は `null` と同じに扱います） |
| `control.transport` | 運び手。いまは `"silkit"`（SIL Kit の publish/subscribe）だけ |
| `control.wire_version` | メッセージの形式の版（整数）。まとめる側の版と違えば、変数を読み書きできません |

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
      ]
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
```

## まとめて動かす

2 つの ECU の実行ファイルを 1 つの SIL Kit のシミュレーションで動かすには、構成ファイルを書いて `vehicle_sil_launch run` に渡します（[simulation-config.md](simulation-config.md)、[simulation-run.md](simulation-run.md)）。照合してから、registry、controller、各 ECU を起動し、終わりまで見て、各 ECU がどう終わったかを出します。

```sh
cd build/<preset>/examples/ecu_main
vehicle_sil_launch run simulation.yaml
```

同じことを手で組み立てる例です。ECU は `--registry-uri` で参加し、`vehicle_sil_sim_controller` が全員の参加を待って始め、仮想時間 1 秒で止めます。

```sh
sil-kit-registry --listen-uri silkit://localhost:8500 &
vehicle_sil_engine_ecu --registry-uri silkit://localhost:8500 &
vehicle_sil_dashboard_ecu --registry-uri silkit://localhost:8500 &
vehicle_sil_sim_controller --registry-uri silkit://localhost:8500 --required Engine,Dashboard --stop-at-ms 1000
```

`--required` には、各 ECU の名前（`--describe` の `name`）を並べます。

動かす前に、まとめる ECU の名前、ステップ幅、ネットワーク、J1939 のアドレスと NAME が食い違っていないかを、構成ファイルと `vehicle_sil_launch check` で確かめられます（[simulation-check.md](simulation-check.md)）。
