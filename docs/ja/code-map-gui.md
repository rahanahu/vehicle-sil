# ブラウザー GUI・プロトコル・クライアント・画面の例

[English](../code-map-gui.md)

この分野を変更するときの地図です。全体の入口は [code-map.md](code-map.md)、テストの共通ルールは [code-map-testing.md](code-map-testing.md) にあります。

関連する仕様と変更範囲: [gui.md](gui.md) / [howto/gui-screen.md](howto/gui-screen.md) / [Control API・gRPC・ランチャーの Control Server](code-map-control.md) / [ランチャー・プロセス・設定・実行](code-map-launcher.md)

## ディレクトリごとのコードとテスト

### `gui/server`、`gui/protocol`、`gui/client`、`gui/web`

案内は [gui.md](gui.md) にあります。

| 内容 | 場所 |
|---|---|
| GUI サーバー | `gui/server/src/vehicle_sil_gui/`（HTTP 層・セキュリティミドルウェア・`RouteSpec` は `server.py`、設定・照合・保存・TOCTOU は `config_service.py`、実行ライフサイクルと `allowed()` は `run_manager.py`、WebSocket と監視は `ws_gateway.py`、Pydantic v2 モデルは `contracts.py`、Control Server に接続するのは `control.py`） |
| API の仕様と例 | `gui/protocol` |
| クライアントのライブラリ | `gui/client/src` |
| サンプルの画面 | `gui/web/src/app` |
| 自分の画面に配るクライアントライブラリと部品（`vsil/`） | `gui/server/src/vehicle_sil_gui/client_files.py`、`server.py` の `get_client_file()`。ファイルの一覧は `scripts/package.py` の `CLIENT_BUNDLES` にもあります。試験: `gui/server/tests/test_client_files.py` |

- テスト: `gui/server/tests`（pytest）、`gui/client/src` と `gui/web/src/app` の単体テスト、`gui/web/e2e`（Playwright）
- `ctest -R`: CTest には入っていません。`scripts/verify-gui.py`（`-k` でサーバーのテストを絞る、`--skip-web`）と `scripts/verify-web.py` で実行します。

### `examples/gui_client`、`examples/gui_starter`

| 内容 | 場所 |
|---|---|
| 画面のないツールの例 | `examples/gui_client` の `run.mjs`、`run-with-library.mjs`、`variables.mjs` |
| 自分の画面のテンプレート | `examples/gui_starter` |

- テスト: `gui/server/tests/test_example_client.py`、`gui/web/e2e/starter.spec.ts`
- `ctest -R`: CTest には入っていません。`scripts/verify-gui.py` で実行します。

### ランチャーへの要求と接続先

GUI サーバーは利用者の選択のためにランチャーのオプションを組み立てません。画面の選択を要求ファイルに書き、`check --json` には `--request` で、`vehicle_sil_launch serve`（または `serve --once`）には `{"command":"run","request":"<file>"}` で渡します。選択のルールは、ランチャーの照合に任せます。

| 内容 | 場所 |
|---|---|
| 照合や開始の欄、その形と大きさ、書き出す要求ファイル（新しい一時ファイル。POSIX ではモード 0600。照合や実行が終わったら削除） | `python/vehicle_sil_control/protocol.py`（`gui/server/src/vehicle_sil_gui/launcher.py` が再公開）の `INTENT_FIELDS`、`check_*()`、`write_request_file()`、`serve_prepare_command()`、`serve_run_command()`、`serve_status_command()`、`stop_command_line()`、および `launcher.py` の `check_command()` と `serve_command()` |
| file-only メタデータと常駐可否の検査（`check --file-only --json` と `serve prepare`）、および単一パースの Events v4（`vehicle_sil_events: 4`）イベント配送 | `launcher.py` の `file_only_check_command()`、`serve_prepare_command()`、`hide_request_in_event()`、`runtime.py` の `ServeRuntime.inspect_config()`、`parse_file_only_check()`、`parse_prepare_event()`、`session.py` の `Session.feed_event()`、`run_manager.py` の `RunManager.relay_event()` |
| Protocol v4（`vehicle_sil_gui: 4`）の単一ストリーム WebSocket 順序付け `(instance, run_id, stream_seq)`（`run_id` は `"<instance>-<n>"`）、再接続の差分ログ再生（`after_stream_seq`）、`stream_seq` 外の遅延 `values` フレーム `{id, watch_id, run_id}`、純粋な HTTP コマンド Ack エンドポイント（`POST /api/run`、`/api/stop-session`、`/api/prepare`、`/api/time`、`/api/write` と静止読み取り `GET /api/session`）、C++ `ExecutionStatus` の採用、および純粋な UI 操作可否の射影（`allowed.py`） | `runtime.py` の `ServeRunHandle.send_stop()`、`ServeRuntime.status()`、`ServeRuntime.execution_status()`、`allowed.py` の `Facts`、`RunFacts`、`stop_of()`、`project_run_facts()`、`allowed()`、`session.py` の `Session`、`Session.replay()`、`Session.run_facts()`、`hub.py` の `Hub`、`run_manager.py` の `RunManager.run_facts()`、`RunManager.allowed()`、`RunManager._write_stop()`、`ws_gateway.py` の `WsGateway.snapshot()`、`contracts.py` |
| サーバーが `connect` について持つ唯一のルール: `none` 以外の対象は、そのファイルのそのネットワークの直近の `POST api/connections` が使えると挙げたときだけ渡す（そうでなければ `target-not-listed`、ファイルスタンプと `file_sha256` の TOCTOU 検証つき） | `gui/server/src/vehicle_sil_gui/config_service.py` の `ConfigService.validate_connect_targets()`、`file_sha256()`、`ConfigService.connections()`、`server.py` の `post_connections()`、`launcher.py` の `connections_command()` |
| 構成の保存していない文の照合（`POST api/check` の `text`。プロセスを起動しない）、それが使う説明（保存したファイルの照合が、ファイルと variant ごとに一時ディレクトリに作る）、`POST api/describe` と `GET api/config` | `config_service.py` の `ConfigService._check_text()`、`ConfigService._text_check_locked()`、`ConfigService._describe_locked()`、`ConfigService.describe()`、`ConfigService.get_config()`、`descriptions.py` の `Descriptions`、`launcher.py` の `text_check_command()` と `describe_command()` |
| 文の保存（`POST api/config`）: 本文、新しいファイルを作れる場所、ランチャーの `check --compare-commands` の `commands` による判断（サーバーは YAML を比べない）、隣のファイルからの置き換え。`--no-save`、`--allow-command-changes`、`allowed.save`。`--no-auth`（`127.0.0.1`、`Host`、`Origin`、CSP、`--root` 検査を保ったまま HTTP/WS のトークン認証のみ無効化） | `config_save.py`（純粋な関数と置き換え）、`config_service.py` の `ConfigService.save_config()`、`ConfigService._save_locked()`、`server.py` の `post_config()`、`_guard()`、`websocket()`、`ws_gateway.py` の `WsGateway.handle_ws()`、`WsGateway._talk()`、`launcher.py` の `save_check_command()`、`allowed.py` の `allowed_save()`、`cli.py` の選択肢 |
| テスト | `gui/server/tests/test_connect.py`、`test_config_text.py`、`test_config_save.py`、`test_units.py`、`test_services.py`、`test_runs.py`、`test_runtime_inprocess.py`、`test_serve_runtime_inprocess.py`、`test_real_variables.py`、`test_protocol_examples.py`（`gui/server/tests/fake_runtime.py` と `gui/server/tests/stub_launcher.py`） |

ランチャーのルール（どの名前があるか、どの欄が一緒に成り立つか、時間の方針が何を意味するか）をサーバーに写さないでください。ランチャーに足し、照合に報告させます。欄を足すときは「[要求の欄と要求ファイル](code-map-launcher.md#要求の欄と要求ファイル)」に従います。

## 領域横断の変更と開発資料

- ランチャーのイベント・標準入力コマンド・features・形式の変更: [イベントとコマンド](code-map-launcher.md#イベント標準入力のコマンドversion---json)。スタブランチャーとプロトコルの例は [実行の送受信](code-map-launcher.md#実行の送受信traffic) を参照。
- 速度や時間の上限: [ランチャーの上限](code-map-launcher.md#ランチャーの上限)（GUI のコピーと不一致を検出するテスト）。
- Control API の応答: [GetSnapshot](code-map-control.md#時刻と値をまとめて読む)、[control.proto](code-map-control.md#controlproto)、[ランチャーの Control Server](code-map-control.md#ランチャーの-control-server)。
- ダッシュボードのスキーマやテンプレートのコピー: [設定とスキーマ](code-map-launcher.md#設定ファイルかシナリオのキー)。
- GUI 固有の開発・テスト・スタブランチャーとランタイムハーネスの扱い: [GUI の開発](gui.md#開発)。HTTP/WebSocket の契約は [gui/protocol/README.md](../../gui/protocol/README.ja.md)、クライアント API は [gui/client/README.md](../../gui/client/README.ja.md)、画面の開発は [gui/web/README.md](../../gui/web/README.ja.md)、テンプレートは [examples/gui_starter/README.md](../../examples/gui_starter/README.ja.md) を参照。
