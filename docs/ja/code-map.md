# コードの地図

[English](../code-map.md)

<!-- 書き方: 項目は、同じ形の短いブロックにします（小見出し、1 行に 1 つの事実の箇条書き、セルが 1 文の表）。1 つのセルや 1 つの箇条書きに複数の事実を詰めません。1 行が長くなり、別々の変更が同じ行に当たるためです。 -->

vehicle-sil 自体を開発するときに、変える場所、そのテスト、テストだけを実行する方法、一緒に直すものを探すための地図です。各ディレクトリと各ターゲットの内容は [architecture.md](architecture.md) の「ディレクトリとターゲット」にあり、ここには書きません。

## 作業対象から探す

まず変更対象のページを読みます。そこで示す仕様と、必要なときだけ隣接する分野の地図を読みます。テストの共通規則はテストのページにあります。実際の登録名とリンク先は CMake と CTest を正とします。図や変更時の注意は設計上の制約を示すために残します。

| 作業対象 | 地図 |
|---|---|
| テストの開発 | [code-map-testing.md](code-map-testing.md) |
| 基盤・ステップ・信号・SIL Kit の既定 | [code-map-core.md](code-map-core.md) |
| CAN・イーサネット・LIN のバス | [code-map-buses.md](code-map-buses.md) |
| J1939 エンジン・コンポーネント・コーデック・spy | [code-map-j1939.md](code-map-j1939.md) |
| ECU・Peer・ローカル実行・Simulation | [code-map-execution.md](code-map-execution.md) |
| ランチャー・プロセス・設定・実行 | [code-map-launcher.md](code-map-launcher.md) |
| シナリオ・時間の軸・一括実行 | [code-map-scenario.md](code-map-scenario.md) |
| Control API・gRPC・ランチャーの Control Server | [code-map-control.md](code-map-control.md) |
| Vector XL とイーサネットのブリッジ | [code-map-bridges.md](code-map-bridges.md) |
| ブラウザー GUI・プロトコル・クライアント・画面の例 | [code-map-gui.md](code-map-gui.md) |
| ビルド・配布・依存ライブラリ・ライブラリの追加 | [code-map-build.md](code-map-build.md) |

## 以前の節への案内

以前の見出しへのリンクを保つために、ここから詳しい項目へ案内します。

## テストの名前と実行方法
[詳細](code-map-testing.md#テストの名前と実行方法)

### 名前と登録
[詳細](code-map-testing.md#名前と登録)

### 一部のテストだけを実行する
[詳細](code-map-testing.md#一部のテストだけを実行する)

### ポートとロック
[詳細](code-map-testing.md#ポートとロック)

### 待ち時間と倍率
[詳細](code-map-testing.md#待ち時間と倍率)

## テストのラベル
[詳細](code-map-testing.md#テストのラベル)

### `unit`
[詳細](code-map-testing.md#unit)

### `integration`
[詳細](code-map-testing.md#integration)

### `vector_xl`
[詳細](code-map-bridges.md#vector_xl)

### `step`
[詳細](code-map-core.md#step)

### `signal`
[詳細](code-map-core.md#signal)

### `can`
[詳細](code-map-buses.md#can)

### `eth`
[詳細](code-map-buses.md#eth)

### `j1939`
[詳細](code-map-j1939.md#j1939)

### `peer` / `testkit`
[詳細](code-map-execution.md#peer--testkit)

### `ecu`
[詳細](code-map-execution.md#ecu)

### `simulation` / `examples`
[詳細](code-map-execution.md#simulation--examples)

### `core` / `lin`
[詳細](code-map-core.md#core--lin)

### `virtual_time`
[詳細](code-map-execution.md#virtual_time)

### `lin`（`lin_cluster.*`）
[詳細](code-map-core.md#linlin_cluster)

### `launch` / `process`
[詳細](code-map-launcher.md#launch--process)

### `control`
[詳細](code-map-control.md#control)

### `control_server`
[詳細](code-map-control.md#control_server)

### `eth_link` / `ethernet_bridge`
[詳細](code-map-bridges.md#eth_link--ethernet_bridge)

### `bridge_runtime`
[詳細](code-map-bridges.md#bridge_runtime)

### `fetchcontent`
[詳細](code-map-build.md#fetchcontent)

## ディレクトリごとのコードとテスト

どの項目も、主なクラスと関数の表（ヘッダーは `include/` の下のパス）、テストのディレクトリ、`ctest -R` に渡す正規表現の順に書いてあります。

### `libs/core`
[詳細](code-map-core.md#libscore)

### `libs/step`
[詳細](code-map-core.md#libsstep)

### `libs/signal`
[詳細](code-map-core.md#libssignal)

### `libs/can`
[詳細](code-map-buses.md#libscan)

### `libs/can_silkit`
[詳細](code-map-buses.md#libscan_silkit)

### `libs/can_netsim`、`tools/can_bus_simulator`
[詳細](code-map-buses.md#libscan_netsimtoolscan_bus_simulator)

### `libs/eth`、`libs/eth_silkit`
[詳細](code-map-buses.md#libsethlibseth_silkit)

### `libs/lin`、`libs/lin_silkit`
[詳細](code-map-buses.md#libslinlibslin_silkit)

### `libs/j1939`（エンジン）
[詳細](code-map-j1939.md#libsj1939エンジン)

### `libs/j1939/parts/<part>`
[詳細](code-map-j1939.md#libsj1939partspart)

### `libs/j1939/codec`、`tools/j1939_codegen`
[詳細](code-map-j1939.md#libsj1939codectoolsj1939_codegen)

### `libs/ecu`
[詳細](code-map-execution.md#libsecu)

### `libs/json`
[詳細](code-map-core.md#libsjson)

### `libs/spy`
[詳細](code-map-j1939.md#libsspy)

### `libs/local`
[詳細](code-map-execution.md#libslocal)

### `libs/testkit`
[詳細](code-map-execution.md#libstestkit)

### `libs/peer`
[詳細](code-map-execution.md#libspeer)

### `libs/simulation`
[詳細](code-map-execution.md#libssimulation)

### `libs/control`
[詳細](code-map-control.md#libscontrol)

### `libs/control_silkit`
[詳細](code-map-control.md#libscontrol_silkit)

### `libs/control_grpc`、`proto/`、`python/`
[詳細](code-map-control.md#libscontrol_grpcprotopython)

### `libs/process`
[詳細](code-map-launcher.md#libsprocess)

### `libs/launch`、`tools/launch`
[詳細](code-map-launcher.md#libslaunchtoolslaunch)

### `tools/common`
[詳細](code-map-launcher.md#toolscommon)

### `tools/sim_controller`
[詳細](code-map-execution.md#toolssim_controller)

### `bridges/vector-xl`
[詳細](code-map-bridges.md#bridgesvector-xl)

### `bridges/common`
[詳細](code-map-bridges.md#bridgescommon)

### `libs/eth_link`、`bridges/ethernet`
[詳細](code-map-bridges.md#libseth_linkbridgesethernet)

### `examples/j1939`（sender と receiver）
[詳細](code-map-j1939.md#examplesj1939sender-と-receiver)

### `examples/virtual_time`
[詳細](code-map-execution.md#examplesvirtual_time)

### `examples/fetchcontent_consumer`（FetchContent での利用）
[詳細](code-map-build.md#examplesfetchcontent_consumerfetchcontent-での利用)

### `template`（ECU のプロジェクトのテンプレート）、`scripts/package.py`（ビルド済みのツールとテンプレートのアーカイブ）
[詳細](code-map-build.md#templateecu-のプロジェクトのテンプレートscriptspackagepyビルド済みのツールとテンプレートのアーカイブ)

### `examples/first_ecu`（ガイド「最初の ECU を作る」の例）
[詳細](code-map-execution.md#examplesfirst_ecuガイド最初の-ecu-を作るの例)

### `examples/scenario_tests`（ガイド「シナリオでテストを書く」の例）
[詳細](code-map-scenario.md#examplesscenario_testsガイドシナリオでテストを書くの例)

### `examples/integrate_ecus`（ガイド「複数の ECU をつないで動かす」の例）
[詳細](code-map-launcher.md#examplesintegrate_ecusガイド複数の-ecu-をつないで動かすの例)

### `examples/ecu_main` の `simulation.two-pcs.yaml`（ガイド「複数の PC で動かす」の例）
[詳細](code-map-launcher.md#examplesecu_main-の-simulationtwo-pcsyamlガイド複数の-pc-で動かすの例)

### `examples/launch_control`（ガイド「信号を外から操作する」の例）
[詳細](code-map-control.md#exampleslaunch_controlガイド信号を外から操作するの例)

### `schema/`（設定ファイルとシナリオの JSON Schema）
[詳細](code-map-launcher.md#schema設定ファイルとシナリオの-json-schema)

### `gui/server`、`gui/protocol`、`gui/client`、`gui/web`
[詳細](code-map-gui.md#guiserverguiprotocolguiclientguiweb)

### `examples/gui_client`、`examples/gui_starter`
[詳細](code-map-gui.md#examplesgui_clientexamplesgui_starter)

### `scripts/`
[詳細](code-map-build.md#scripts)

### `patches/sil-kit`
[詳細](code-map-build.md#patchessil-kit)

### `cmake/`
[詳細](code-map-build.md#cmake)

## ドキュメントの例とテスト
[詳細](code-map-testing.md#ドキュメントの例とテスト)

### リファレンスの例
[詳細](code-map-testing.md#リファレンスの例)

### ガイドの例
[詳細](code-map-testing.md#ガイドの例)

### 照合していないもの
[詳細](code-map-testing.md#照合していないもの)

## 依存の向き

どの図でも、矢印は「リンクする」（`target_link_libraries`）です。逆向きの依存は作りません。

| 図の書き方 | 意味 |
|---|---|
| 実線の矢印 | `PUBLIC` か `INTERFACE` のリンク |
| 点線の矢印 | 非公開（`PRIVATE`）のリンク |
| ノードの名前 | ターゲットの名前（`vehicle_sil::` を省く） |

### 全体の層
[詳細](code-map-core.md#全体の層)

### 基盤と SIL Kit の実装
[詳細](code-map-core.md#基盤と-sil-kit-の実装)

### J1939 のエンジンとコンポーネント
[詳細](code-map-j1939.md#j1939-のエンジンとコンポーネント)

### ECU とローカルの実行（SIL Kit なし）
[詳細](code-map-execution.md#ecu-とローカルの実行sil-kit-なし)

### Peer と Simulation
[詳細](code-map-execution.md#peer-と-simulation)

### launch と Control API
[詳細](code-map-control.md#launch-と-control-api)

### Vector XL CAN Bridge
[詳細](code-map-bridges.md#vector-xl-can-bridge)

### Ethernet Bridge
[詳細](code-map-bridges.md#ethernet-bridge)

### ほかの実行ファイル
[詳細](code-map-build.md#ほかの実行ファイル)

## 境界を検査するテスト

次のテストは、ソースの `#include` などを読んで、層の境界を検査します。内部のヘッダーや SIL Kit のヘッダーを追加すると失敗します。

| テスト | 検査すること |
|---|---|
| `j1939_parts.boundary` | `libs/j1939/parts` が、エンジンの公開 API とツールキットだけを使い、`vehicle_sil::j1939` とほかのコンポーネントだけをリンクする |
| `local.boundary`、`testkit.boundary`、`peer.boundary`、`ecu.boundary`、`simulation.boundary`、`j1939_codec.boundary`、`param_service.boundary`、`j1939_spy.boundary` | そのディレクトリのソースが、エンジンの内部を使わない（`cmake/VehicleSilCheckJ1939Parts.cmake`） |
| `ecu.no_silkit_boundary`、`ecu.simulation_headers_boundary`、`lin.boundary`、`step.boundary`、`eth.boundary`、`signal.boundary` | `libs/ecu`、`libs/simulation/include`、`libs/lin`、`libs/step`、`libs/eth`、`libs/signal` が、SIL Kit のヘッダーを直接にも間接にも含まない（`cmake/VehicleSilCheckNoSilKit.cmake`） |
| `peer.controllers_are_made_on_the_simulation_name` | `libs/` で SIL Kit のバスのコントローラーを作るのは `libs/peer/src/peer.cpp` だけで、どの呼び出しも `simulation_name()` を通した名前のネットワークに作る（`cmake/VehicleSilCheckControllerNetworks.cmake`） |
| `core.silkit_configuration_defaults` | `libs/`、`tools/`、`bridges/`、`tests/`（`tests/core/configuration_fixtures` を除く）が文字列から作る SIL Kit の設定は、vehicle-sil の既定（`default_participant_configuration()` など）を通る（`cmake/VehicleSilCheckSilKitConfiguration.cmake`） |
| `launch.described_networks_are_read_in_one_place` | `libs/launch` と `tools/launch` で自己申告のネットワーク（`description.networks`。メンバーへのポインター `::networks` も）を読むのは `src/check_networks.cpp`（対応を当てる `seen_networks()` と、JSON の `reported` を書く所）と `src/describe_read.cpp` だけ（`cmake/VehicleSilCheckDescribedNetworks.cmake`。設定の `config.networks` は数えない） |
| `j1939_codec.independent_of_the_engine` | `codec.hpp` が含む vehicle-sil のヘッダーは `j1939/types.hpp`（値の型）だけで、`Network` と `Node` を含まない |
| `eth_link.boundary` | `libs/eth_link` が、SIL Kit のヘッダーを直接にも間接にも含まない（`cmake/VehicleSilCheckNoSilKit.cmake`） |

## 変更するときに一緒に直すもの

どの項目も、変える場所、合わせて直すドキュメント、合わせて直すコード、守ること、テストの順に書いてあります（ない見出しは省いています）。「手で合わせる」と書いたものは、テストが照合しません。

| 分野 | 変えるもの | 項目 |
|---|---|---|
| ランチャー | SIL Kit の設定の既定、待ち受けるアドレス | [SIL Kit の設定の既定](code-map-core.md#sil-kit-の設定の既定) |
| ランチャー | `check` と `run` の出力、`examples/ecu_main/simulation.yaml` | [`check` と `run` の出力](code-map-launcher.md#check-と-run-の出力) |
| ランチャー | 例のコードと、ガイドにあるコピー | [ガイドにある出力とコードのコピー](code-map-launcher.md#ガイドにある出力とコードのコピー) |
| ランチャー | 照合のルールと `code` | [照合のルールと `code`](code-map-launcher.md#照合のルールと-code) |
| ランチャー | 設定ファイルかシナリオのキー、スキーマ | [設定ファイルかシナリオのキー](code-map-launcher.md#設定ファイルかシナリオのキー) |
| ランチャー | `network_names` | [設定ファイルの `network_names`](code-map-launcher.md#設定ファイルの-network_names) |
| ランチャー | `j1939_nodes` | [設定ファイルの `j1939_nodes`](code-map-launcher.md#設定ファイルの-j1939_nodes) |
| ランチャー | `run --events`、`--stdin-commands`、`version --json` | [イベント、標準入力のコマンド、`version --json`](code-map-launcher.md#イベント標準入力のコマンドversion---json) |
| ランチャー | 速度、一時停止、ステップ実行、遅れ | [`run` の時間](code-map-launcher.md#run-の時間) |
| ランチャー | `manual`、`--manual` | [自分で起動する participant](code-map-launcher.md#自分で起動する-participant) |
| ランチャー | `variants`、`params`、`--variant`、`--param`、`only` | [機種とシナリオのパラメーター](code-map-scenario.md#機種とシナリオのパラメーター) |
| ランチャー | `run --scenario`、`check --scenario`、波形 | [シナリオ](code-map-scenario.md#シナリオ) |
| ランチャー | `run --scenario --timeline` | [時間の軸のファイル](code-map-scenario.md#時間の軸のファイル) |
| ランチャー | `vehicle_sil_launch test` | [シナリオをまとめて実行する](code-map-scenario.md#シナリオをまとめて実行する) |
| ランチャー | 実行の結果の `traffic` | [実行の送受信](code-map-launcher.md#実行の送受信traffic) |
| ランチャー | `kMaxMillis`、`kMinSpeed`、`kMaxSpeed` | [ランチャーの上限](code-map-launcher.md#ランチャーの上限) |
| ECU | `--describe` の JSON | [`--describe` の JSON](code-map-execution.md#--describe-の-json) |
| ECU | SIL Kit のコントローラーを作る所、`--map-network` | [ネットワークの名前の対応](code-map-execution.md#ネットワークの名前の対応) |
| ECU | `ecu_main` の引数と終了コード | [`ecu_main` の引数と終了コード](code-map-execution.md#ecu_main-の引数と終了コード) |
| ECU | `ecu.connected` | [ECU のバスからの切断](code-map-execution.md#ecu-のバスからの切断) |
| ECU | `NetworkKind`、`NetworkUse` | [ネットワークの種類か使い方を追加する](code-map-execution.md#ネットワークの種類か使い方を追加する) |
| ECU | `template/`、ECU の API | [テンプレートと ECU の API](code-map-execution.md#テンプレートと-ecu-の-api) |
| Control API | `participants[].variables` | [変数の対応表](code-map-control.md#変数の対応表) |
| Control API | `control.proto` | [`control.proto`](code-map-control.md#controlproto) |
| Control API | `run --control` | [ランチャーの Control Server](code-map-control.md#ランチャーの-control-server) |
| Control API | シナリオの実行中の読み取り専用 | [シナリオの実行中の Control Server](code-map-control.md#シナリオの実行中の-control-server) |
| Control API | `GetSnapshot` | [時刻と値をまとめて読む](code-map-control.md#時刻と値をまとめて読む) |
| Control API | `wire.cpp`、`kVersion`、`kRevision` | [Agent と Hub の間のメッセージ](code-map-control.md#agent-と-hub-の間のメッセージ) |
| Control API | `control_silkit`、開始前の窓 | [SIL Kit の上のトランスポートと開始前の窓](code-map-control.md#sil-kit-の上のトランスポートと開始前の窓) |
| ブリッジ | 止め方、終了コード、監視の行 | [ブリッジの止め方と終了コード](code-map-bridges.md#ブリッジの止め方と終了コード) |
| ブリッジ | イーサネットのブリッジの出力とエラーの文 | [イーサネットのブリッジの出力](code-map-bridges.md#イーサネットのブリッジの出力) |
| ビルドと配布 | `project(... VERSION)` | [バージョンの番号](code-map-build.md#バージョンの番号) |
| ビルドと配布 | `VEHICLE_SIL_STATIC` | [`VEHICLE_SIL_STATIC`](code-map-build.md#vehicle_sil_static) |
| ビルドと配布 | `scripts/package.py tools` | [ツールのアーカイブの中身](code-map-build.md#ツールのアーカイブの中身) |
| ビルドと配布 | 使う SIL Kit の API | [SIL Kit の API と上流の挙動への前提](code-map-build.md#sil-kit-の-api-と上流の挙動への前提) |
| 追加 | J1939 のコンポーネント | [J1939 のコンポーネントを追加する](code-map-j1939.md#j1939-のコンポーネントを追加する) |
| 追加 | `libs/` のライブラリ | [`libs/` にライブラリを追加する](code-map-build.md#libs-にライブラリを追加する) |

### SIL Kit の設定の既定
[詳細](code-map-core.md#sil-kit-の設定の既定)

### `check` と `run` の出力
[詳細](code-map-launcher.md#check-と-run-の出力)

### ガイドにある出力とコードのコピー
[詳細](code-map-launcher.md#ガイドにある出力とコードのコピー)

### 照合のルールと `code`
[詳細](code-map-launcher.md#照合のルールと-code)

### 設定ファイルかシナリオのキー
[詳細](code-map-launcher.md#設定ファイルかシナリオのキー)

### 設定ファイルの `network_names`
[詳細](code-map-launcher.md#設定ファイルの-network_names)

### 設定ファイルの `j1939_nodes`
[詳細](code-map-launcher.md#設定ファイルの-j1939_nodes)

### イベント、標準入力のコマンド、`version --json`
[詳細](code-map-launcher.md#イベント標準入力のコマンドversion---json)

### `run` の時間
[詳細](code-map-launcher.md#run-の時間)

### 自分で起動する participant
[詳細](code-map-launcher.md#自分で起動する-participant)

### 機種とシナリオのパラメーター
[詳細](code-map-scenario.md#機種とシナリオのパラメーター)

### シナリオ
[詳細](code-map-scenario.md#シナリオ)

### 時間の軸のファイル
[詳細](code-map-scenario.md#時間の軸のファイル)

### シナリオをまとめて実行する
[詳細](code-map-scenario.md#シナリオをまとめて実行する)

### 実行の送受信（`traffic`）
[詳細](code-map-launcher.md#実行の送受信traffic)

### ランチャーの上限
[詳細](code-map-launcher.md#ランチャーの上限)

### `--describe` の JSON
[詳細](code-map-execution.md#--describe-の-json)

### ネットワークの名前の対応
[詳細](code-map-execution.md#ネットワークの名前の対応)

### `ecu_main` の引数と終了コード
[詳細](code-map-execution.md#ecu_main-の引数と終了コード)

### ECU のバスからの切断
[詳細](code-map-execution.md#ecu-のバスからの切断)

### ネットワークの種類か使い方を追加する
[詳細](code-map-execution.md#ネットワークの種類か使い方を追加する)

### テンプレートと ECU の API
[詳細](code-map-execution.md#テンプレートと-ecu-の-api)

### 変数の対応表
[詳細](code-map-control.md#変数の対応表)

### `control.proto`
[詳細](code-map-control.md#controlproto)

### ランチャーの Control Server
[詳細](code-map-control.md#ランチャーの-control-server)

### シナリオの実行中の Control Server
[詳細](code-map-control.md#シナリオの実行中の-control-server)

### 時刻と値をまとめて読む
[詳細](code-map-control.md#時刻と値をまとめて読む)

### Agent と Hub の間のメッセージ
[詳細](code-map-control.md#agent-と-hub-の間のメッセージ)

### SIL Kit の上のトランスポートと開始前の窓
[詳細](code-map-control.md#sil-kit-の上のトランスポートと開始前の窓)

### ブリッジの止め方と終了コード
[詳細](code-map-bridges.md#ブリッジの止め方と終了コード)

### イーサネットのブリッジの出力
[詳細](code-map-bridges.md#イーサネットのブリッジの出力)

### バージョンの番号
[詳細](code-map-build.md#バージョンの番号)

### `VEHICLE_SIL_STATIC`
[詳細](code-map-build.md#vehicle_sil_static)

### ツールのアーカイブの中身
[詳細](code-map-build.md#ツールのアーカイブの中身)

### SIL Kit の API と上流の挙動への前提
[詳細](code-map-build.md#sil-kit-の-api-と上流の挙動への前提)

### J1939 のコンポーネントを追加する
[詳細](code-map-j1939.md#j1939-のコンポーネントを追加する)

### `libs/` にライブラリを追加する
[詳細](code-map-build.md#libs-にライブラリを追加する)

## 関連するドキュメント

| ドキュメント | 内容 |
|---|---|
| [AGENTS.md](../../AGENTS.md) | ドキュメントの管理、コミット、検証のスクリプト（先に読みます） |
| [architecture.md](architecture.md) | 全体の構成と、各ディレクトリと各ターゲットの内容 |
| [internals/](architecture.md#内部の仕組み) のドキュメント | 変える場所の内部の仕組み |
