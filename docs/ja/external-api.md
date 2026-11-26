# 外部 API とスキーマ

[English](../external-api.md)

GUI サーバー API、ランチャー CLI / イベント、Control API、および YAML / JSON スキーマを使って `vehicle-sil` と連携する外部ツール・スクリプト・独自画面向けの入口ページです。このページのリンクはすべてリポジトリ相対パスで書いてあるため、特定の Git タグやリリースアーカイブを開いたときも、その版に一致した仕様・スキーマ・サンプルを参照できます。詳細な仕様の正本は各リンク先です。

## GUI サーバー API（HTTP と WebSocket）

プロトコル v4 では `POST /api/time` で `run_id` と `instance` の両方を省略する呼び出しを互換性のため維持していますが、新規クライアント・AI エージェント・CI 自動化では常に `instance` と `run_id` の両方を指定してください。

| リソース | パス | 内容 |
|---|---|---|
| プロトコル仕様 | [gui/protocol/README.ja.md](../../gui/protocol/README.ja.md) | HTTP・WebSocket プロトコル v4（`vehicle_sil_gui: 4`）の正本仕様: 認証・Origin・実行の対象指定（`instance` + `run_id`）・`/api/time`・`/api/stop-session`・`/api/write`・再接続リプレイ・順序などの振る舞い不変条件・上限 |
| OpenAPI 3.1.0（全 17 エンドポイント） | [gui/protocol/openapi.yaml](../../gui/protocol/openapi.yaml) | 全 17 個の `/api/*` HTTP エンドポイントを網羅するオフライン生成の機械可読契約（`run_id` ↔ `instance` の `dependentRequired` を含む `TimeCommandRequest`、`TimeCommandAck`、`ErrorResponse`、ステータスコード）。ビルド済みツールアーカイブでは `protocol/openapi.yaml` として同梱 |
| WebSocket サーバーフレームスキーマ | [gui/protocol/schemas/ws-server.schema.json](../../gui/protocol/schemas/ws-server.schema.json) | サーバーからクライアントへの全 13 種類の WebSocket フレーム（`hello`、`synced`、`session`、`event`、`allowed`、`runtime`、`variables`、`values`、`operation`、`availability`、`dropped`、`resync`、`error`）の Draft 2020-12 JSON Schema |
| WebSocket クライアントフレームスキーマ | [gui/protocol/schemas/ws-client.schema.json](../../gui/protocol/schemas/ws-client.schema.json) | クライアントからサーバーへの全 3 種類の WebSocket フレーム（`auth`、`watch`、`unwatch`）の Draft 2020-12 JSON Schema |
| WebSocket `command` イベントスキーマ | [gui/protocol/schemas/ws-command-event.schema.json](../../gui/protocol/schemas/ws-command-event.schema.json) | 非同期 WebSocket `command` イベント（`accepted`、`reason`、`message`、`id`、`asked_run`）の Draft 2020-12 JSON Schema |
| CLI `--json` 起動通知スキーマ | [gui/protocol/schemas/cli-json.schema.json](../../gui/protocol/schemas/cli-json.schema.json) | `vehicle-sil-gui --json` が標準出力に書き出す起動完了 JSON 行の Draft 2020-12 JSON Schema |
| ダッシュボードファイルスキーマ | [gui/protocol/dashboard.schema.json](../../gui/protocol/dashboard.schema.json) | `<configuration>.dashboard.yaml` の Draft 2020-12 JSON Schema |
| 通信例の JSON | [gui/protocol/examples/](../../gui/protocol/examples/) | 全 17 個の HTTP エンドポイントと WebSocket 交換を網羅し、生成スキーマで検証される 60 件の JSON 例 |
| TypeScript クライアントライブラリ | [gui/client/README.ja.md](../../gui/client/README.ja.md) | `@vehicle-sil/gui-client`（正面入口 `connect()` および低レベル `createClient()`） |

GUI サーバー API の全スキーマ（`openapi.yaml` と `schemas/*.schema.json`）は、サーバーを起動せずにオフラインで決定論的に生成・検証でき（`python -m vehicle_sil_gui.openapi --write` / `--check`）、ビルド済みツールアーカイブの `protocol/` にも同梱されます。

## 構成・シナリオ・ランチャーのスキーマ

シナリオファイルについては、**JSON Schema で構文を検証し、対象構成に対する整合性はランチャーの `check` で検証します**（`vehicle_sil_launch check <config> --scenario <scenario>`、または `POST /api/check`）。JSON Schema が検証するのは構造と値の型のみであり、participant 名・変数名・型・範囲・タイムステップ制約などの対象 ECU との整合性はランチャーの `check` が検証します。

| スキーマ | パス | 検証対象 |
|---|---|---|
| シナリオ YAML | [schema/scenario.schema.json](../../schema/scenario.schema.json) | シナリオファイル（`vehicle_sil_scenario: 1`）— 構文検証用。対象構成への照合はランチャーの `check` を併用 |
| シミュレーション構成 YAML | [schema/simulation.schema.json](../../schema/simulation.schema.json) | シミュレーション構成ファイル（`vehicle_sil_simulation: 2`） |
| 実行要求 JSON | [schema/run-request.schema.json](../../schema/run-request.schema.json) | ランチャーの `--request` ファイルおよび `POST /api/check` / `POST /api/run` の intent フィールド（`vehicle_sil_run_request: 1`） |
| シナリオタイムライン JSON | [schema/timeline.schema.json](../../schema/timeline.schema.json) | `--timeline` および `GET /api/timeline/<run_id>` の出力 |

## マニュアル

| 分野 | ドキュメント |
|---|---|
| ブラウザ GUI ガイド・セキュリティ・独自画面 | [gui.md](gui.md)、[ブラウザの GUI に自分の画面を作る](howto/gui-screen.md) |
| シナリオリファレンス・テストガイド | [scenario.md](scenario.md)、[シナリオでテストを書く](howto/scenario-tests.md) |
| シミュレーション構成と `check` | [simulation-config.md](simulation-config.md)、[simulation-check.md](simulation-check.md) |
| 実行（`run`、`test`、`serve`） | [simulation-run.md](simulation-run.md) |
| Events v4（`serve [--once]`）、標準入力コマンド、`version --json` | [simulation-events.md](simulation-events.md) |
| Control API（gRPC）と Python SDK | [control-api.md](control-api.md) |

## 実行できるサンプル

| サンプル | パス | 内容 |
|---|---|---|
| 最小の素の HTTP + WebSocket クライアント | [examples/gui_client/run.mjs](../../examples/gui_client/run.mjs) | Node 22+・外部依存なしの `fetch` + `WebSocket`: `api/app`、`api/check`、`api/run`、イベント購読 |
| 対象指定つきの時間コマンド・再接続・停止 | [examples/gui_client/time-and-stop.mjs](../../examples/gui_client/time-and-stop.mjs) | Node 22+・外部依存なしで `POST api/time` / `POST api/stop-session` に `{instance, run_id}` を送り、数値 `id` で HTTP 200 と WS `accepted: true`/`false` を対応づけ、`after_stream_seq` で再接続 |
| クライアントライブラリのサンプル | [examples/gui_client/run-with-library.mjs](../../examples/gui_client/run-with-library.mjs)、[examples/gui_client/front.mjs](../../examples/gui_client/front.mjs)、[examples/gui_client/variables.mjs](../../examples/gui_client/variables.mjs) | `@vehicle-sil/gui-client` による実行ライフサイクル、正面入口 `connect()`、ECU 変数の watch / write / step |
| 独自画面スターターテンプレート | [examples/gui_starter/README.ja.md](../../examples/gui_starter/README.ja.md) | `vehicle-sil-gui --web` で配信する独自ブラウザ UI の作成 |
| Python 常駐ランチャー（`Serve` / `RunHandle`） | [examples/launch_control/serve_runs.py](../../examples/launch_control/serve_runs.py) | `vehicle_sil_launch serve --control` による連続実行 |
| シナリオ・実行要求のサンプル | [examples/ecu_main/scenarios/throttle_step.yaml](../../examples/ecu_main/scenarios/throttle_step.yaml)、[examples/ecu_main/requests/throttle_step.json](../../examples/ecu_main/requests/throttle_step.json) | `examples/ecu_main` に対応するシナリオ YAML と実行要求 JSON |
| サンプル一覧 | [examples/README.ja.md](../../examples/README.ja.md) | やりたいこと別の全サンプル一覧 |
