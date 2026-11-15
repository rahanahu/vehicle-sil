# サンプル

やりたいこと別のサンプルの一覧と、それぞれの内容と動かし方をまとめたドキュメントです。

## 一覧

上から順に読むと、使う層が少しずつ下がります。

| やりたいこと | サンプル | 実行ファイル、ファイル |
|---|---|---|
| 最初の ECU を動かす（送信する、受信する） | [getting_started](getting_started) | `vehicle_sil_getting_started` |
| ECU アプリのコンポーネントを使う（Request、監視、識別情報） | [j1939/ecu_pair](j1939/ecu_pair) | `vehicle_sil_j1939_ecu_pair` |
| メッセージを YAML で定義して使う | [typed_messages](typed_messages) | `vehicle_sil_typed_messages` |
| 独自のプロトコルを作る（問い合わせを受けて値を定期的に返す） | [pid_stream](pid_stream) | `vehicle_sil_pid_stream` |
| 独自のプロトコルを作る（セッション、書き込み、長いデータ） | [j1939/param_service](j1939/param_service) | `vehicle_sil_j1939_param_service` |
| 故障を入れて、通信を観測してテストする | [fault_injection](fault_injection) | `vehicle_sil_fault_injection` |
| 複数の ECU、複数のネットワーク、LIN を 1 つのシミュレーションにする | [vehicle](vehicle) | `vehicle_sil_vehicle` |
| ECU ごとに実行ファイルを作り、設定ファイルで 1 つのシミュレーションにする | [ecu_main](ecu_main) | `vehicle_sil_engine_ecu`、`vehicle_sil_dashboard_ecu` |
| 別々に作った ECU の不整合を見つけて直す | [integrate_ecus](integrate_ecus) | `vehicle_sil_dashboard_v1_ecu` |
| ECU のプロジェクトのテンプレートに ECU を 1 つ追加する | [first_ecu](first_ecu) | `cooler_ecu`、`read_cooler.py` |
| シナリオのテストを書く | [scenario_tests](scenario_tests) | `my_tests/*.yaml`、`simulation.variants.yaml` |
| 生のイーサネットのフレームを送受信する | [ethernet](ethernet) | `vehicle_sil_ethernet_example`、`vehicle_sil_eth_sensor_ecu`、`vehicle_sil_eth_display_ecu` |
| ランチャーの Control Server に Python から接続する | [launch_control](launch_control) | `write_before_start.py` |
| Python から値を書き込み、時間を進めて、読む（最短） | [launch_control](launch_control) | `write_and_read.py` |
| 動いているシミュレーションを止めて、時刻を指定して書き込む | [launch_control](launch_control) | `pause_and_step.py` |
| ブラウザの GUI のサーバーを、画面を使わずに呼び出す | [gui_client](gui_client) | `run.mjs`、`run-with-library.mjs`、`variables.mjs`（素の JavaScript） |
| ブラウザの GUI に、自分の画面を作る | [gui_starter](gui_starter) | 素の HTML と JavaScript（ビルドなし） |
| 層を自分で組み立てる（バス、ホスト、ドライバー、ネットワーク） | [low_level](low_level) | `vehicle_sil_low_level` |

## 実行のしかた

実行ファイルはビルドツリーの `examples/` 以下にあります（例: `build/linux-gcc-release/examples/getting_started/vehicle_sil_getting_started`）。

getting_started、typed_messages、pid_stream、fault_injection、vehicle、ethernet（`vehicle_sil_ethernet_example`）、low_level は、引数なしで動きます。結果を数行で表示し、期待どおりなら 0、違えば 1 で終了します。CTest にも入っています（ラベル `examples`）。ecu_pair は結果を検査しません。

## サンプルごとの説明

### getting_started

最初に読みます。`Ecu&` を受け取るクラスで ECU を書き、`Simulation` で仮想バスか SIL Kit に載せます（`--silkit`）。

### j1939/ecu_pair

コンポーネント（`PeriodicSender`、`Requester`、`Monitor` など）で ECU の振る舞いを組み立てるときに読みます。既定は SIL Kit で、`--local` を付けると仮想バスで動きます。

### typed_messages

ビット位置を書かずに信号を扱いたいときに読みます。スケール、符号、列挙、特別な値、9 バイト以上のメッセージを使います。

### pid_stream

コンポーネントを自分で書く最初の手本です。要求を受けて要求元へ周期的に返すコンポーネントと、その使い方を示します。バイト列の形式は差し替えられます。

同じ相手の新しい要求には送信のコンポーネントを作り直さずに周期を変え（`set_period()`）、長さの変わる返信は vector に追記して作ります。新しい相手のコンポーネントは、上限（`max_streams`）とタイマーの残り（`timers_free()`）を確認してからハンドラの中で作り、Stop や期限切れではハンドラやタイマーの中で破棄します。ツールの操作は `Ecu::at()` で時刻に書きます。

### j1939/param_service

上の pid_stream より大きいプロトコル（セッション、再試行、ブロックの転送）を作るときに読みます。

### fault_injection

通信断、アドレスの競合、転送の途中の欠落、生フレームの注入を、仮想バスで再現して確認するときに読みます。

### vehicle

2 つの CAN と 1 つの LIN にまたがる構成を作るときに読みます。LIN の Master と Slave も `Ecu&` のクラスの中にあります。エンジンの ECU は外から読み書きする変数を宣言し、`main()` が時刻を指定して書き込みます。既定は SIL Kit で、`--local` を付けると仮想バス（CAN と LIN）で同じ結果になります。

### ecu_main

ECU ごとに実行ファイルを作り、それらを SIL Kit の 1 つのシミュレーションで動かすときに読みます。`main()` は `ecu_main()` の 1 行で、どの ECU も同じ引数（`--describe`、`--registry-uri`、`--local --run-for-ms`）を持ちます。動かし方と `--describe` の JSON は [docs/ecu-executable.md](../docs/ja/ecu-executable.md) にあります。CTest の `ecu_main.*` が、2 つを別々のプロセスで動かして確認します。

| ファイル | 内容 |
|---|---|
| `simulation.yaml` | 2 つの ECU を 1 つのシミュレーションにする設定ファイルで、`vehicle_sil_launch check` で照合し、`vehicle_sil_launch run` で動かせます（[docs/simulation-config.md](../docs/ja/simulation-config.md)）。 |
| `simulation.live.yaml` | 同じ 2 つを等速で、止めるまで動かす設定ファイルで、GUI で値を見ながら操作するときに使います（リポジトリからは `scripts/run-gui.py` がこれで GUI を起動します）。 |
| `simulation.two-pcs.yaml` | 同じ 2 つを 2 台の PC に分けて動かす設定ファイルです（[docs/howto/multi-pc.md](../docs/ja/howto/multi-pc.md)）。 |
| `simulation.dashboard.yaml` | ブラウザの GUI で変数をどう並べるかです（[docs/gui.md](../docs/ja/gui.md) の「ダッシュボードのファイル」）。 |
| `panels/rpm-bar.js` | ダッシュボードに置く自作のパネル（素の JavaScript）です（[docs/gui.md](../docs/ja/gui.md) の「カスタムパネル」）。 |

### integrate_ecus

ガイド [複数の ECU をつないで動かす](../docs/ja/howto/integrate-ecus.md) の例を動かすときに読みます。ビルドツリーの `examples/integrate_ecus/` で実行します。

| ファイル | 内容 |
|---|---|
| `simulation.yaml` | ecu_main のエンジンの ECU と、同じバスを別のネットワーク名（`VehicleCAN`）で呼ぶ表示の ECU（`vehicle_sil_dashboard_v1_ecu`）を並べた設定ファイルで、`check` が不整合を報告します。 |
| `simulation.two-buses.yaml` | `networks` に名前を追加しただけの設定ファイルで、`check` も `run` も成功しますが、表示の ECU は回転数を受信しません。 |
| `simulation.fixed.yaml` | 作り直した表示の ECU に差し替えた設定ファイルです。 |
| `scenarios/engine_speed.yaml` | 受信を確認するシナリオです。 |

### first_ecu

ガイド [最初の ECU を作る](../docs/ja/howto/first-ecu.md) の完成形を見るときに読みます。[template/](../template) の 2 つの ECU に、変数だけの ECU（`Cooler`）、CMake に追加する 4 行、3 つの ECU の `simulation.yaml`、Python から変数を読み書きするスクリプトを追加したものです。

単独ではビルドしません。テストのビルドが、テンプレートの 2 つの ECU の隣（`build/<preset>/tests/template/project/bin/`）に `cooler_ecu` を作ります。CTest の `template.first_ecu` が、ファイルと出力をガイドと照合します（Python のスクリプトは `control_server.example_first_ecu`）。

### scenario_tests

ガイド [シナリオでテストを書く](../docs/ja/howto/scenario-tests.md) で書くファイルの完成形を見るときに読みます。[ecu_main](ecu_main) の 2 つの ECU に対する 4 つのシナリオ（値の設定と確認、応答の時間、`ramp` と `wave` の間の確認、パラメーターと `only`）と、機種を 2 つ追加した設定ファイルです。

ビルドするものはありません。`build/<preset>/examples/ecu_main/` にコピーして、`vehicle_sil_launch test simulation.variants.yaml --scenarios my_tests/` で実行できます（シナリオの先頭のコメントの分、出力の行番号はガイドとずれます）。CTest の `launch_run.scenario_guide` が、ファイル、出力、終了コードをガイドと照合します。

### ethernet

ECU が自分でイーサネットのフレームを組み立てて送受信するときに読みます（`Ecu::ethernet()`。vehicle-sil は TCP/IP のスタックを持ちません）。センサーが 100 ms ごとに値を送信し、表示器が受信して確認を返します。

| 実行ファイル | 内容 |
|---|---|
| `vehicle_sil_ethernet_example` | 2 つの ECU を 1 つのプロセスで動かし（引数なしは仮想ネットワーク、`--silkit` は SIL Kit。結果は同じで、自分で検査します）、`--ethertype <数>` でフレームの EtherType を変えられます。 |
| `vehicle_sil_eth_sensor_ecu`、`vehicle_sil_eth_display_ecu` | 同じ ECU を `ecu_main()` で実行ファイルにしたもので、`vehicle_sil_launch run simulation.yaml --scenario scenarios/exchange.yaml` で動かします（ネットワークの `kind: ethernet`、`ecu.connected` でバスから切断する）。 |

EtherType の既定（0x88B5）と MAC アドレス（ローカル管理のアドレス）はこの例が決めた値で、規格では確認していません。

### launch_control

`vehicle_sil_launch run --control` で動かすシミュレーションを、Python から操作する例です。

| スクリプト | 内容 | CTest |
|---|---|---|
| `write_before_start.py` | `ecu_main` の 2 つの ECU を `--start-paused` で起動し、最初のステップの前にエンジンのスロットルを書き込み、500 ms 進めて回転数を読み返して確認します（違えば 1 で終了）。 | `control_server.example_write_before_start` |
| `write_and_read.py` | ガイド [信号を外から操作する](../docs/ja/howto/operate-signals.md) の最初のスクリプトで、`ecu_main` の 2 つの ECU を `--start-paused` で起動し、スロットルに 50 % を書き込み、500 ms 進めて、回転数を表示します（引数は `vehicle_sil_launch` のパスと `simulation.yaml`）。 | `control_server.guide_operate_signals`（ガイドに載せた全文と出力を照合） |
| `pause_and_step.py` | `ecu_main` の 2 つの ECU を実時間の 4 分の 1 の速度で動かし、`watch()` で回転数を見て 1000 rpm を超えたら一時停止し、止まっている時刻の 1 ms 後を `at` にしてスロットルを書き込み、100 ms 進め、書き込みが反映されたステップと、`snapshot()` で読んだ時刻と値を確認し、最速に変えて最後まで動かします（違えば 1 で終了）。 | `control_server.example_pause_and_step` |

どれも、`VEHICLE_SIL_BUILD_CONTROL_SERVER=ON` のビルドと、gRPC のある Python が必要です（[docs/control-api.md](../docs/ja/control-api.md) の「Python」）。

ECU を自分で起動する（デバッガーから動かすなど）ときも、その participant を `manual` にして同じランチャーに接続します（[docs/simulation-config.md](../docs/ja/simulation-config.md) の「participant を自分で起動する」）。

### gui_client

GUI と同じ API（[gui/protocol](../gui/protocol/README.ja.md)）で、自分のツールを作るときに読みます。照合して実行を始め、イベントを読み、終わりを待ちます。実行は `VEHICLE_SIL_GUI_URL='<URL>' node run.mjs <設定ファイル>` です。

| ファイル | 内容 |
|---|---|
| `run.mjs` | Node 22 以上の標準の `fetch` と `WebSocket` だけを使います。 |
| `run-with-library.mjs` | クライアントのライブラリ（[gui/client](../gui/client/README.ja.md)）を 1 つにまとめたファイルを使います。 |
| `variables.mjs` | 同じライブラリで ECU の変数を読み書きします（止めて始め、最初のステップの前に 1 つ書き込み、進めて止めて、値を確認する。Control Server を持つランチャーが必要です）。 |

CMake には入っていません（`scripts/verify-gui.py` が、流し直すランチャーを相手に動かして確認します）。使い方は [docs/gui.md](../docs/ja/gui.md) の「画面のないツールから使う」にあります。

### gui_starter

GUI サーバーに配信させる自分の画面（`vehicle-sil-gui --web <dir>`）を、好きなフレームワークで一から作るときに読みます。クライアントのライブラリだけで、接続、照合、開始と停止、変数を 1 つ見て書き込む、`client.variables.bind` で作ったスライダーとランプ、までの最小の形です。TypeScript と好きなフレームワークで作るときの、ライブラリのパッケージ（`.tgz`）の使い方も [README](gui_starter/README.ja.md) にあります。

CMake には入っていません（`scripts/verify-gui.py` が、流し直すランチャーを相手に `--web` で配信してブラウザで確認します）。使い方は [docs/gui.md](../docs/ja/gui.md) の「自分の画面を作る」にあります。

### low_level

新しいバックエンドやテストの土台を作るときに読みます。通常は `Simulation` を使います。

## 結合テストが起動する実行ファイル

次の実行ファイルは、結合テストが別プロセスとして起動する相手で、入門用ではありません。

| サンプル | 内容 |
|---|---|
| [j1939/sender](j1939/sender)、[j1939/receiver](j1939/receiver) | J1939 スタックを実時間の `Peer` で動かす送信側と、SIL Kit の CAN で直接受信して照合する受信側です。 |
| [lin](lin) | LIN の Master と Slave のピアです（`Peer` を使わず、`ParticipantRuntime` と `SilKitLinPort` で組み立てる）。 |
| [virtual_time](virtual_time) | 仮想時間のピア（物理モデル、J1939 ECU、LIN センサー、ボディコントローラー）です。 |

## 外部の CMake プロジェクトから使う

[fetchcontent_consumer](fetchcontent_consumer) は、FetchContent で取り込む外部プロジェクトの例で、別のビルドとして configure してテストします（ラベル `fetchcontent`）。手順は [docs/build.md](../docs/ja/build.md) の「外部プロジェクトから使う（FetchContent）」にあります。
