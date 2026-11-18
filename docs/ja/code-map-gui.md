# ブラウザー GUI・プロトコル・クライアント・画面の例

[English](../code-map-gui.md)

この分野を変更するときの地図です。全体の入口は [code-map.md](code-map.md)、テストの共通ルールは [code-map-testing.md](code-map-testing.md) にあります。

関連する仕様と変更範囲: [gui.md](gui.md) / [howto/gui-screen.md](howto/gui-screen.md) / [Control API・gRPC・ランチャーの Control Server](code-map-control.md) / [ランチャー・プロセス・設定・実行](code-map-launcher.md)

## ディレクトリごとのコードとテスト

### `gui/server`、`gui/protocol`、`gui/client`、`gui/web`

案内は [gui.md](gui.md) にあります。

| 内容 | 場所 |
|---|---|
| GUI サーバー | `gui/server/src/vehicle_sil_gui/`（Control Server に接続するのは `control.py`） |
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

## 領域横断の変更と開発資料

- ランチャーのイベント・標準入力コマンド・features・形式の変更: [イベントとコマンド](code-map-launcher.md#イベント標準入力のコマンドversion---json)。録画とプロトコルの例は [実行の送受信](code-map-launcher.md#実行の送受信traffic) を参照。
- 速度や時間の上限: [ランチャーの上限](code-map-launcher.md#ランチャーの上限)（GUI のコピーと不一致を検出するテスト）。
- Control API の応答: [GetSnapshot](code-map-control.md#時刻と値をまとめて読む)、[control.proto](code-map-control.md#controlproto)、[ランチャーの Control Server](code-map-control.md#ランチャーの-control-server)。
- ダッシュボードのスキーマやテンプレートのコピー: [設定とスキーマ](code-map-launcher.md#設定ファイルかシナリオのキー)。
- GUI 固有の開発・テスト・録画の扱い: [GUI の開発](gui.md#開発)。HTTP/WebSocket の契約は [gui/protocol/README.md](../../gui/protocol/README.ja.md)、クライアント API は [gui/client/README.md](../../gui/client/README.ja.md)、画面の開発は [gui/web/README.md](../../gui/web/README.ja.md)、テンプレートは [examples/gui_starter/README.md](../../examples/gui_starter/README.ja.md) を参照。
