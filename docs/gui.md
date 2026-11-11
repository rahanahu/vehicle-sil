# ブラウザの GUI（`vehicle-sil-gui`）

構成ファイルを選んで照合し、シミュレーションを始めて止め、participant の状態とログをブラウザで見る道具です。GUI サーバー（Python）が自分の PC の上で `vehicle_sil_launch` を動かし、その経過を画面に中継します。シミュレーションを動かすのはランチャーで、GUI はそれを映すだけです。試験や AI エージェントから使う正式な口は、今までどおり `vehicle_sil_launch` と Control API です。

## できること

- 構成ファイルを一覧から選ぶ（起動のときに渡したファイル、または `--root` の下の `.yaml` / `.yml`）。
- 照合の結果（所見、participant の表）を見る。participant を外して（`--disable`）照合し直す。
- 実行を始める、止める（2 回目は待たずに止める）。段階、participant ごとの状態と終了コード、ログ、所見、結果を見る。
- 画面を開き直したり、別のタブで開いたりしても、同じ実行の状態を見られる。

## 起動

リポジトリの直下で `uv sync` をしてから、次のように起動します（例は、ビルドした ECU の例の隣にコピーされた構成ファイル）。

```sh
uv run vehicle-sil-gui build/<preset>/examples/ecu_main/simulation.yaml
```

- 起動すると、`http://127.0.0.1:<port>/#token=<...>` を 1 行表示して、ブラウザで開きます。開かないときは、この URL をブラウザに貼ります。Ctrl+C でサーバーを止めます。実行中のシミュレーションは、先に止めます。
- ランチャーは `--launcher <path>`、環境変数 `VEHICLE_SIL_LAUNCH`、`PATH` の順に探します。ランチャーが GUI に要る機能（`version --json` の `features` の `events` と `stdin-commands`）を持たないときは、何が足りないかを表示して終了コード 2 で終わります。
- 画面のビルド（`gui/web` の `npm run build`）がないときは、ビルドの手順を書いたページを表示します。

| 引数 | 意味 |
|---|---|
| `<構成ファイル>` | 最初に選んでおくファイル（`--root` の下） |
| `--root <dir>` | 画面で選べるファイルの範囲。既定は、渡した構成ファイルのディレクトリ、なければカレントディレクトリ |
| `--port <n>` | 待ち受けるポート（127.0.0.1）。既定の 0 は空いているポート |
| `--no-browser` | ブラウザを開かない |
| `--launcher <path>` | `vehicle_sil_launch` のパス（`.py` で終わるパスは、この Python で動かす） |
| `--dev-origin <origin>` | 画面の開発サーバー（`http://127.0.0.1:4200` など）からの要求も受け付ける |
| `--json` | URL の代わりに `{"vehicle_sil_gui": 1, "url": ..., "port": ...}` を 1 行で表示する（道具から起動するとき） |
| `--stop-timeout <秒>` | 終わるとき、実行中のシミュレーションが止まるのを待つ時間（既定 10）。過ぎたらランチャーを止める |

終了コード: 0 は止めた（Ctrl+C、SIGTERM）、1 は動いている間の失敗、2 は引数の誤りか、ランチャーやポートを使えない。

## 安全の考え方

GUI サーバーはプロセスを起動できるので、自分の PC の上の自分だけが使う前提で、次のように守っています。

- 127.0.0.1 だけで待ちます。ほかの PC からは開けません。
- 起動のたびに新しいトークンを作ります。トークンは表示した URL の `#token=` の後ろにだけあり、画面はそれを読んだら URL から消します。API と WebSocket はトークンがないと答えません。URL を人に渡さないでください。
- ほかのウェブページからの要求を断ります（`Host` と `Origin` の検査。CORS は許しません）。
- 画面から使えるのは `--root` の下の構成ファイルだけです。外へ出るパスやシンボリックリンクは断ります。ランチャーに渡す引数はサーバーが決まった形で組み立て、任意のコマンドは受け取りません。
- サーバーが終わるとき（Ctrl+C、端末を閉じた、異常終了）は、ランチャーの標準入力を閉じて順に止めさせ、止まらなければランチャーと、それが起動したプロセスを止めます。サーバーが強制終了されたときも、ランチャーは標準入力の終わりを受けて自分で止まります。

## いまの制限

- 変数の値（Control）はまだ見られません。照合の結果にある変数の一覧だけを表示します。
- 速度と一時停止で始める（`speed`、`start_paused`）は、ランチャーが対応するまで使えません。
- いまの `vehicle_sil_launch` は GUI に要る機能（`run --events`、`--stdin-commands`、`version --json`）をまだ持たないので、GUI は起動しません。それまでは試験用の偽のランチャー（`gui/server/tests/fake_launcher.py`）で動かせます: `uv run vehicle-sil-gui --launcher gui/server/tests/fake_launcher.py gui/server/tests/configs/simulation.yaml`。
- Windows では確かめていません（子プロセスの止め方は書いてありますが、動かしていません）。

## 開発

- サーバーは `gui/server`（Python パッケージ `vehicle_sil_gui`、uv の workspace のメンバー）、画面は `gui/web`、両者の間の形の正本は `gui/protocol`（README と実例）です。
- 試験: `uv run pytest gui/server/tests`。画面まで含めて確かめるには `scripts/verify-gui.py`。
