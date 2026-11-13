# AGENTS.md

vehicle-sil で作業する開発者と AI エージェント向けの指示です。

## ドキュメントの管理

確定した仕様は Git で管理し、作業中の資料はローカルにだけ置きます。仕様の正本は Git で管理しているコード、テスト、ドキュメントです。

### Git で管理するもの

- `README.md`、`AGENTS.md`
- `docs/` に置く次の文書
  - ビルドと開発の手順
  - API リファレンス、利用者向けマニュアル
  - 確定したアーキテクチャ、外部に公開するインターフェースの仕様
  - 実装が守るべき設計上の契約と制約

確定仕様は簡潔に書きます。検討の過程は転載しません。

### ローカルにだけ置くもの

次の資料はリポジトリ直下の `.local/` に置き、コミットしません。`.local/` は `.git/info/exclude` で除外しています。

| ディレクトリ | 内容 |
|---|---|
| `.local/research/` | 技術調査 |
| `.local/decisions/` | 設計案、比較検討、判断に至った議論、却下した案 |
| `.local/reviews/` | コードレビューの記録 |
| `.local/experiments/` | 実験ログ、性能測定の作業記録 |
| `.local/plans/` | 一時的な実装計画、AI エージェントの作業記録 |

新しいクローンでは、次のコマンドで除外設定とディレクトリを用意します。

```sh
grep -qxF '/.local/' .git/info/exclude || echo '/.local/' >> .git/info/exclude
mkdir -p .local/research .local/decisions .local/reviews .local/experiments .local/plans
```

### 設計判断の扱い

検討の過程と確定事項を分けて記録します。

- 検討の過程は `.local/decisions/<topic>.md` に置きます。代替案、測定結果、却下した理由もここに書きます。
- 確定事項は `docs/` に反映します。対象は、採用した方式、公開 API の契約、不変条件、安全性とスレッド安全性の制約、重要な制限、将来の変更で注意すべき点です。
- 確定事項が既存の仕様と矛盾する場合は、仕様の文書も更新します。
- 設計の根拠をローカル資料への参照だけで説明しません。Git で管理している文書だけを読んで、現在の仕様がわかるようにします。

### AI エージェントへの指示

- 調査結果と設計の検討は `.local/` に保存します。新しい設計判断をしたら、必要に応じて経緯を `.local/decisions/` に残します。
- 実装が終わって仕様が確定したら、必要な情報だけを `docs/` に反映します。
- 次のことは禁止です。
  - 調査メモをそのまま `docs/` に保存する。
  - AI との議論の全文をコミットする。
  - 一時的な設計案を確定仕様として書く。
  - 整理していない検討資料を大量にコミットする。
  - ローカル資料を唯一の仕様書として扱う。

### コミットメッセージ

タイトルは [Conventional Commits](https://www.conventionalcommits.org/ja/v1.0.0/) の形式で書きます（例: `feat: add LIN signal encoding`、`fix(j1939): keep the frame order within a step`）。

- 主な型: `feat`（機能の追加）、`fix`（不具合の修正）、`refactor`、`test`、`docs`、`build`（CMake、依存ライブラリ、パッチ）、`ci`、`chore`。
- スコープ（括弧内）は任意です。`core`、`j1939`、`lin`、`vector-xl`、`deps` などを使います。
- 本文には、何を変えたかではなく、なぜ変えたかを書きます。
- `patches/` の上流向けパッチのコミットメッセージは、上流の書き方に合わせます。この形式にはしません。

### コミット前の確認

1. `.local/` のファイルがステージされていない。
2. 一時的な調査資料やレビュー資料が含まれていない。
3. 公開 API や確定仕様を変えた場合は、関係するマニュアルを更新した。
4. 設計上の重要な制約をドキュメントに反映した。
5. ドキュメントの内容が実際のコードと合っている。

既存のコミット履歴は書き換えません。追跡済みのファイルは ignore の設定だけでは追跡対象から外れないので、無断で削除、移動、追跡解除をしません。

## 検証とスクリプト

`scripts/` に、開発と CI で使うスクリプトがあります。Python 3.11 以上の標準ライブラリだけで書いてあります。

- 出力は要約だけです。詳細は `build/logs/` のログに書き、要約にその場所を表示します。
- 終了コードは、0 が成功、1 が失敗、2 が使い方の誤りです。CI からそのまま呼べます。
- 使い方は `--help` で表示します。

| スクリプト | 内容 |
|---|---|
| `scripts/doctor.py` | この PC に、ビルドと試験に要るものがそろっているかを 1 行ずつ出す（読むだけ）。どのビルドにも要るものが足りなければ終了コード 1。一部の機能だけに要るもの（GUI、サニタイザーのプリセット、静的リンク）は「absent」と出すだけで、終了コードを変えない。足りないものには、何に要るかと入れ方を出す。`--json` |
| `scripts/verify.py` | プリセットごとに構成、ビルド、テストを実行して要約する。失敗した段、落ちたテストと出力の先頭、ログの場所を表示する。JUnit 形式の結果も `build/logs/` に書く |
| `scripts/check-commits.py` | コミットメッセージの形式と、コミットしてはいけないファイル（ローカルにだけ置くもの、`CLAUDE.local.md`、`build/`）を確かめる |
| `scripts/check-docs.py` | 追跡している Markdown の相対リンクを確かめる: リンク先のファイルがあるか、`#` の見出しがあるか、リンクの直後に「…」で挙げた節の名前がリンク先にあるか。CI でも回す |
| `scripts/mutate.py` | TOML に書いたミューテーションを 1 件ずつ当て、テストが落ちることを確かめる。書き換えたファイルは、中断しても元に戻す |
| `scripts/verify-web.py` | 画面（`gui/web`）とクライアントのライブラリ（`gui/client`）の `npm ci`、書式、lint、単体試験、ビルド、e2e を順に回して要約する。ログは `build/logs/web-*.log`。`--skip-install`、`--skip-e2e`、`--e2e-dev`（開発サーバーを通した e2e も回す。同じ試験の繰り返しなので、既定では回さない）、`--tag`、`--no-wait` |
| `scripts/verify-gui.py` | GUI 全体: GUI サーバー（`gui/server`）の試験（pytest。本物のランチャーの出力を録ったものを流し直すランチャーで API、WebSocket、安全、止め方、`gui/protocol` の例の照合）、最小のクライアントの例（`examples/gui_client/run.mjs`）を同じランチャーで動かす試験の後に `scripts/verify-web.py` を回して要約する。ログは `build/logs/gui-*.log`。`--skip-web`、`-k`、`--skip-install`、`--skip-e2e`、`--tag`、`--no-wait` |
| `scripts/package.py` | ビルド済みの道具の圧縮ファイルを作る（`tools`）、使う人の手順で確かめる（`verify`: 空のディレクトリにひな形を写し、`get_tools.py` で展開し、ひな形をビルドして `check`、`run`、シナリオ、`run --control` に Python のクライアントでつなぐ、GUI サーバーの起動）、ひな形の圧縮ファイルと `SHA256SUMS` を作る（`template`）、版の番号が全部の場所で同じかを確かめる（`version`）。出力は `build/dist/`。タグ `v<版>` の GitHub Actions（`.github/workflows/release.yml`）も同じものを呼ぶ |
| `scripts/verify-python.py` | uv で、Python 3.11 と `.python-version` の版の環境を `uv.lock` から作り、C++ のビルドなしで回せる Python の確認（構文、生成器の試験、ひな形の `get_tools.py` と `package.py template` の試験、スクリプトの `--help`、Control API のスタブの生成と import）をする。続けて、`pyproject.toml` の依存の下限そのものを 3.11 に入れて同じ確認をする（`--skip-lowest` で省く）。uv がなければ終了コード 2 |

`verify-web.py` と `verify-gui.py` は、時間に敏感な試験を回す前に、この PC で `ninja`、`ctest`、Windows の VM（`qemu-system-x86*`）が動いている間は待ちます（`scripts/busy_machine.py`。何を待つかを 1 行で表示し、`--wait-limit` 分（既定 20 分）で諦めて警告を出して進む）。どちらの試験も、同時に回すと時間の条件で落ちるためです。GUI の試験は、pytest、ng、Playwright を直接呼ばずにこの 2 つから回します。

```sh
scripts/doctor.py                                    # 最初に: 要るものがそろっているか
scripts/verify.py --linux                            # コミット前: Linux の 5 つのプリセット
scripts/verify.py linux-gcc-debug --tests '^launch'  # 1 つのプリセットで一部のテストだけ
scripts/verify.py --summarize build/logs/verify-linux-gcc-debug-test.log
scripts/check-commits.py                             # ステージした内容
scripts/check-docs.py                                # docs を変えたら: リンクと節の名前
scripts/check-commits.py --range origin/main..HEAD   # 範囲の各コミット
scripts/mutate.py mutations.toml --check             # 書き換える箇所が見つかるかだけを確かめる
scripts/verify-python.py                             # Python のコード: 3.11、.python-version の版、3.11 と依存の下限
scripts/verify-gui.py                                # GUI: サーバーの試験と画面（--skip-web でサーバーだけ）
scripts/package.py tools && scripts/package.py verify build/dist/vehicle-sil-tools-*-linux-x86_64.tar.gz
```
