# ブラウザー GUI・プロトコル・クライアント・画面の例

[English](../code-map-gui.md)

<!-- 書き方: 項目は、同じ形の短いブロックにします（小見出し、1 行に 1 つの事実の箇条書き、セルが 1 文の表）。1 つのセルや 1 つの箇条書きに複数の事実を詰めません。1 行が長くなり、別々の変更が同じ行に当たるためです。 -->

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

- テスト: `gui/server/tests`（pytest）、`gui/client/src` と `gui/web/src/app` の単体テスト、`gui/web/e2e`（Playwright）
- `ctest -R`: CTest には入っていません。`scripts/verify-gui.py`（`-k` でサーバーのテストを絞る、`--skip-web`）と `scripts/verify-web.py` で実行します。

### `examples/gui_client`、`examples/gui_starter`

| 内容 | 場所 |
|---|---|
| 画面のないツールの例 | `examples/gui_client` の `run.mjs`、`run-with-library.mjs`、`variables.mjs` |
| 自分の画面のテンプレート | `examples/gui_starter` |

- テスト: `gui/server/tests/test_example_client.py`、`gui/web/e2e/starter.spec.ts`
- `ctest -R`: CTest には入っていません。`scripts/verify-gui.py` で実行します。
