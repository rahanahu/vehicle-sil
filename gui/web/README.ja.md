# gui/web

[English](README.md)

サンプルの画面（Angular、TypeScript）を開発するためのリファレンスです。必要なもの、コマンド、ディレクトリの構成、コーディングのルール（Angular の書き方、レイアウト、アクセシビリティ、国際化、テスト）が書いてあります。

サンプルの画面は、クライアントのライブラリ（`gui/client`）とその Angular のラッパーだけで作ったリファレンス実装です。ビルドした画面は、Python の GUI サーバーが配信します（`--web` を付けないとき）。画面を使うだけなら Node.js は不要で、Node.js が必要なのは、画面を開発、ビルドするときだけです。

| 知りたいこと                    | 節                                            |
| ------------------------------- | --------------------------------------------- |
| 開発に何をインストールするか    | [必要なもの](#必要なもの)                     |
| ビルド、テスト、lint のコマンド | [コマンド](#コマンド)                         |
| 開発サーバーで画面を動かす      | [開発サーバーで動かす](#開発サーバーで動かす) |
| e2e のテストが何を起動するか    | [e2e のテスト](#e2e-のテスト)                 |
| どのファイルがどこにあるか      | [ディレクトリ](#ディレクトリ)                 |
| コードをどう書くか              | [コーディングのルール](#コーディングのルール) |
| 文言を追加する、言語を追加する  | [国際化](#国際化)                             |

自分の画面を一から作る手順と、この画面のコンポーネントを別の Angular のアプリへコピーする手順は、末尾の「関連するドキュメント」にあります。

## 必要なもの

| 必要なもの                       | 条件と用途                                                                       |
| -------------------------------- | -------------------------------------------------------------------------------- |
| Node.js                          | 24（`.nvmrc`）。`package.json` の `engines` は Node.js 24.15 以上の 24 と 26     |
| npm                              | 11.12.1 以上（Node.js に付いてくるもの）。ほかのパッケージマネージャーは使わない |
| [uv](https://docs.astral.sh/uv/) | GUI サーバー `gui/server` を動かす（開発と e2e のテストで使う）                  |
| e2e のテストのブラウザ           | `npx playwright install chromium --only-shell` でインストールする                |

- `.npmrc` の `engine-strict` により、`engines` の範囲の外では `npm ci` が止まります。
- `npx playwright install chromium --only-shell` は、Playwright のバージョンに合う Chromium を、ユーザーのキャッシュにインストールします。管理者の権限は不要です。

Windows でも同じコマンドで動かす想定です。Node.js は Windows 版を使います。`scripts/verify-web.py` は Windows の Python（python.org か MinGW のもの）で動かします。

## コマンド

`gui/web/` で実行します。どれも終了コードで成否を返します。

| コマンド                | 内容                                                                                                 |
| ----------------------- | ---------------------------------------------------------------------------------------------------- |
| `npm ci`                | `package-lock.json` のとおりにインストールする                                                       |
| `npm start`             | 開発サーバー（http://127.0.0.1:4200）を起動する                                                      |
| `npm run build`         | 本番のビルド（出力は `dist/browser/`、ライセンスの表示は `dist/3rdpartylicenses.txt`）               |
| `npm test`              | 単体テスト（Vitest、jsdom）を 1 回実行して終わる                                                     |
| `npm run test:client`   | クライアントのライブラリ（`../client`）の型の検査と単体テスト（Vitest、Node。Angular も DOM もなし） |
| `npm run test:timeline` | 時間の軸のページ（`../timeline/viewer.js`）の単体テスト（Node のテストランナー。DOM なし）           |
| `npm run build:client`  | クライアントのライブラリを 1 つの ESM のファイルにまとめる                                           |
| `npm run pack:client`   | クライアントのライブラリを、型つきの npm のパッケージにする                                          |
| `npm run lint`          | ESLint（angular-eslint）。警告も失敗にする                                                           |
| `npm run format:check`  | Prettier の書式の検査（`../client` と `../timeline` も。設定は `../.prettierrc`）                    |
| `npm run e2e`           | Playwright。GUI サーバーが `dist/browser/` と API を同じオリジンで配信する                           |
| `npm run e2e:dev`       | Playwright。開発サーバーと、その転送（WebSocket も）を通す                                           |
| `npm run e2e:real`      | Playwright。本物のランチャーと、その隣にビルドした ecu_main の例の ECU で通す（`e2e/real/`）         |
| `npm run i18n:extract`  | 翻訳する文言を `build/i18n/messages.json`（リポジトリの直下の `build/`）に書き出す                   |

- `npm start` は、`/api/` と `/ws` を GUI サーバー（127.0.0.1:8765）へ転送します（`proxy.conf.mjs`。ポートは環境変数 `GUI_PORT`）。
- `npm run build:client` の出力は `../../build/gui-client/vehicle-sil-gui-client.mjs` です。esbuild は、Angular のビルドのツールが持っているものを使います。
- `npm run pack:client` の出力は `../../build/gui-client/vehicle-sil-gui-client-<version>.tgz` です。作った後に、インストールして確認します（`../client/pack.mjs`）。
- `npm run lint` は、`../client` も、その `eslint.config.js` で検査します。
- `npm run e2e` と `npm run e2e:real` の前に、`npm run build` が必要です。
- `npm run e2e:real` は、ランチャーを環境変数 `VEHICLE_SIL_GUI_LAUNCHER` で受け取ります。
- 翻訳ファイルとの比較と更新は、`scripts/verify-web.py` で行います（下の「国際化」）。

### まとめて実行する（`scripts/verify-web.py`）

リポジトリの直下から `scripts/verify-web.py` を実行すると、次を順に実行して、要約だけを表示します。

`npm ci`、書式、lint、単体テスト、クライアントのライブラリの単体テスト、翻訳ファイルの比較、ビルド、ライブラリを 1 つにまとめる（`bundle`）、パッケージ（`package`）、e2e。

e2e を直している間は、次の引数で絞れます。

| 引数                          | 内容                                             |
| ----------------------------- | ------------------------------------------------ |
| `--skip-checks`               | 書式、lint、単体テスト、翻訳ファイルの比較を省く |
| `--e2e-grep <part of a name>` | 名前の合う e2e のテストだけを実行する            |
| `--e2e-repeat <count>`        | e2e を繰り返す                                   |

### 開発サーバーで動かす

2 つの端末で、次のように動かします。

```sh
uv run vehicle-sil-gui --no-browser --json --port 8765 --dev-origin http://127.0.0.1:4200 \
  --launcher ../server/tests/replay_launcher.py --root ../server/tests/configs   # GUI サーバー
npm start                                                                     # 画面（127.0.0.1:4200）
```

- GUI サーバーは、本物のランチャーの出力の記録を再生するランチャー（`gui/server/tests/replay_launcher.py`。docs/gui.md の「開発」）を使います。
- 画面では、`gui/server/tests/configs/` の設定ファイル（記録のあるもの）を選べます。
- ブラウザでは、サーバーが表示する `dev_url`（トークンつき）を開きます。

### e2e のテスト

e2e のテストは、GUI サーバーを自分で起動し、終わったら止めます。

| コマンド                         | 起動するもの                             | 起動するコード               |
| -------------------------------- | ---------------------------------------- | ---------------------------- |
| `npm run e2e`、`npm run e2e:dev` | GUI サーバーと、記録を再生するランチャー | `e2e/support/gui-server.ts`  |
| `npm run e2e:real`               | GUI サーバーと、本物のランチャー         | `e2e/support/real-server.ts` |

- ポートは、実行ごとに OS が選ぶ空いているポートです（開発サーバーも同じ）。
- `npm run e2e:real` の設定ファイルは、`gui/server/tests/real_configs.py` が一時ディレクトリに書きます。`scripts/verify-gui.py --launcher <path>` が、この e2e を実行します。

環境変数 `VEHICLE_SIL_GUI_SHOTS` にディレクトリを書くと、スクリーンショットを撮ります。

| e2e                                                   | 撮るもの                                       |
| ----------------------------------------------------- | ---------------------------------------------- |
| `npm run e2e:real`                                    | 4 つの状態を 3 つの幅で                        |
| 記録を再生するランチャーの e2e（`e2e/shots.spec.ts`） | 実行の画面の 6 つの状態を 360 px と 1024 px で |

`e2e/shots.spec.ts` は、状態ごとに新しい GUI サーバーを起動するので、前の実行は写りません。`VEHICLE_SIL_GUI_SHOTS_LANGUAGE=en` で、英語の画面を撮ります。

## ディレクトリ

| パス                                     | 内容                                                                      |
| ---------------------------------------- | ------------------------------------------------------------------------- |
| `src/app/app.*`                          | シェル（ツールバー、ナビゲーション、中身の領域）                          |
| `src/app/app.routes.ts`、`navigation.ts` | ルートと、ナビゲーションの項目                                            |
| `src/app/core/`                          | 画面をまたいで使うもの（下の「`core/`」）                                 |
| `src/app/core/session/`                  | クライアントのライブラリの Angular のラッパー（下の「`core/session/`」）  |
| `src/app/run/`                           | 実行の画面（`''`）（下の「`run/`」）                                      |
| `src/app/widgets/`                       | 変数のコンポーネントとダッシュボード（下の「`widgets/`」）                |
| `src/app/variables/`                     | 変数の画面（`variables`）。自動のダッシュボードと絞り込み                 |
| `src/app/not-found/`                     | 見つからないルートの画面                                                  |
| `src/app/testing/`                       | テスト用の画面（`/_test/layout`）                                         |
| `src/theme.scss`、`src/styles.css`       | Material のテーマ（明暗）、レイアウトの寸法（CSS のカスタムプロパティ）   |
| `src/main.ts`、`src/start.ts`            | 起動                                                                      |
| `src/locale/`                            | 翻訳ファイル（`messages.<language>.json`）。ビルドで `locale/` に置かれる |
| `public/`                                | そのまま配信するファイル（アイコン）                                      |
| `e2e/`                                   | Playwright のテスト（下の「`e2e/`」）                                     |

- `src/main.ts` が言語を決めて翻訳を読み込み、それから `src/start.ts`（アプリ）を読み込みます。
- `src/app/testing/` の画面は、レイアウトの e2e が使います。ナビゲーションには表示しません。単体テスト用のヘルパー（偽の WebSocket と HTTP、`gui/protocol/examples/` を読む関数）は、`@vehicle-sil/gui-client/testing` にあります。

### `core/`

| ディレクトリ | 内容                               |
| ------------ | ---------------------------------- |
| `auth/`      | トークン                           |
| `layout/`    | 幅の区分                           |
| `format/`    | 計測値とパスの書式                 |
| `streams/`   | 流れ                               |
| `session/`   | クライアントのライブラリのラッパー |
| `i18n/`      | 画面の言語                         |

サーバーとの間の型と読み取り、接続、セッションの状態は、クライアントのライブラリ（`../client`、`@vehicle-sil/gui-client`）にあります。

### `core/session/`

ラッパーは、ライブラリの `Readable`（読み取り専用の値）を signal にするだけです。

| ファイル               | 内容                                                                                            |
| ---------------------- | ----------------------------------------------------------------------------------------------- |
| `gui-client.ts`        | アプリ全体で 1 つのクライアントを作る（HTTP は HttpClient、`SOCKET_FACTORY`、`readableSignal`） |
| `server-connection.ts` | 接続とセッションのコピー                                                                        |
| `server-info.ts`       | `api/app` と 1 回のリクエスト                                                                   |
| `simulation-time.ts`   | 時間                                                                                            |
| `run-commands.ts`      | 開始と停止                                                                                      |
| `ecu-variables.ts`     | ECU の変数                                                                                      |

### `run/`

| ファイル         | 内容                                                                                                     |
| ---------------- | -------------------------------------------------------------------------------------------------------- |
| `run-session.ts` | 操作（開始、停止、待たずに止める、経過時間）。画面と同じライフサイクルで、中身はラッパーの `RunCommands` |
| `run-status.ts`  | いつも見える状態の帯                                                                                     |

### `widgets/`

変数のコンポーネントは、値の表示、数値の入力、スライダー、スイッチ、ランプ、選択肢、グラフです。

| ファイル              | 内容                                                                                 |
| --------------------- | ------------------------------------------------------------------------------------ |
| `variable-binding.ts` | コンポーネントと変数を接続する（中身はライブラリの `client.variables.bind`）         |
| `variable-frame.ts`   | コンポーネントの枠                                                                   |
| `dashboard-spec.ts`   | ダッシュボードのデータと既定のコンポーネント、ダッシュボードのファイルからの組み立て |
| `dashboard-view.ts`   | ダッシュボードのデータを表示する                                                     |
| `panel-host.ts`       | カスタムパネルを置く                                                                 |

このディレクトリのコードは、ラッパーの公開 API だけを使います（自分でコンポーネントを追加するときと同じ方法。docs/gui.md の「変数の画面とコンポーネント」）。

### `e2e/`

| ファイル                                         | 内容                                                            |
| ------------------------------------------------ | --------------------------------------------------------------- |
| `app.spec.ts`                                    | 配信とルーティング                                              |
| `run.spec.ts`                                    | 実行の画面                                                      |
| `before-start.spec.ts`                           | 最初のステップの前                                              |
| `manual.spec.ts`                                 | 自分で起動する participant                                      |
| `scenario.spec.ts`                               | シナリオ                                                        |
| `starter.spec.ts`                                | 画面のテンプレート `examples/gui_starter` を `--web` で配信する |
| `i18n.spec.ts`                                   | 英語の画面と言語の切り替え                                      |
| `layout.spec.ts`、`support/layout.ts`            | レイアウト                                                      |
| `support/gui-server.ts`                          | GUI サーバーの起動                                              |
| `support/run-page.ts`                            | 実行の画面の操作                                                |
| `variables.spec.ts`、`support/variables-page.ts` | 変数の画面                                                      |
| `shots.spec.ts`                                  | スクリーンショット                                              |
| `real/`                                          | 本物のランチャーで実行するテスト（`playwright.real.config.ts`） |

## URL と CSP

API と WebSocket の URL は、先頭に `/` を付けずに書きます（`api/app`、`ws`）。`index.html` の `<base href>` からの相対になるので、`/` 以外のパスに置いても動きます。そのときは `ng build --base-href /gui/` でビルドします。WebSocket の URL は `document.baseURI` から作ります（クライアントのライブラリの `webSocketUrl`。`core/session/gui-client.ts` が `baseUrl` に `document.baseURI` を渡す）。

本番のビルドは inline の script を出力しません（`angular.json` で `inlineCritical: false`）。`script-src 'self'` の CSP で動きます。コンポーネントのスタイルは `<style>` で入るので、`style-src` には `'unsafe-inline'`（または nonce）が必要です。

## コーディングのルール

いまの Angular（22）の書き方にそろえます。どこに書いてもエラーと言えるものは `npm run lint` で止まります。判断が必要なもの（RxJS をどこで使うかなど）はここに書き、レビューで見ます。

### ファイルの配置と命名

- ファイル名と型の名前は `ng generate` の既定に合わせる（`ng g service telemetry` → `telemetry.ts` の `Telemetry`、`ng g component not-found` → `not-found/not-found.ts` の `NotFound`、`ng g pipe measure` → `measure-pipe.ts` の `MeasurePipe`）。
- 画面ごとにディレクトリを作る（`src/app/<screen>/`）。画面をまたいで使うものは `src/app/core/` に置く。
- テストで探す要素には、役割（`getByRole`）で探せないときだけ `data-testid` を付ける。

### Angular の書き方（lint で強制）

- コンポーネントは standalone。依存は `inject()`。入出力と問い合わせは `input()`、`output()`、`model()`、`viewChild()`。`@HostBinding` の代わりに `host`。
- テンプレートは `@if`、`@for`、`@switch`、`@defer`。`async` パイプを使わない（テンプレートが読むのは signal）。
- zone.js は使わない（zoneless）。変更の検出は OnPush（既定）のまま。
- 実験的、開発者プレビューの API を土台にしない。安定した代わりを使う。
- guard、interceptor、resolver は関数で書く。ルートの引数は `input()` で受け取る（`withComponentInputBinding`）。

### 状態を持つ場所

| 状態                             | 持つ場所                    |
| -------------------------------- | --------------------------- |
| 1 つの画面だけの状態             | そのコンポーネントの signal |
| 機能ごとの状態とサーバーからの値 | その機能のサービス          |
| アプリ全体の状態（幅の区分など） | `core/` のサービス          |

サービスは、読み取り専用の signal（`asReadonly()`）と、変えるためのメソッドだけを公開する。

### signal と RxJS

**signal を使う所**:

- 状態、そこから導く値、テンプレートが読むものは signal（`signal`、`computed`、`linkedSignal`）。

**サーバーとの通信はライブラリが持つ**:

- サーバーへのリクエストと接続は、クライアントのライブラリ（`@vehicle-sil/gui-client`）だけが持つ。
- 画面は、`core/session/` のラッパー（`ServerConnection`、`ServerInfo`、`SimulationTime`、`RunCommands`、`EcuVariables`）の signal とメソッドを使い、`HttpClient` や WebSocket を自分で持たない。自分の画面を作るときと同じものだけを使うためである（docs/howto/gui-screen.md）。
- ライブラリの HTTP は、ラッパーが `HttpClient` で送る（interceptor がトークンを付ける）。
- ライブラリの状態は `Readable`（`get()` と `subscribe()`）。ラッパーは `readableSignal`（`core/session/gui-client.ts`）で signal にする。
- ライブラリが足りないときは、ラッパーで近道を作らず、ライブラリの公開のエントリーポイント（`@vehicle-sil/gui-client`）に追加する。
- 本番のコードは、`@vehicle-sil/gui-client/internal`（互換の保証の外）と `/testing` を import しない（lint で止まる。テストは使ってよい）。
- 購読が投げたものは、Angular の `ErrorHandler` へ渡る。

**リクエストの送り方**:

- 1 回の取得（`api/app`、`api/configs`）は、ラッパーの Promise を返すメソッドを `resource` の loader で呼ぶ。
- 照合（`POST api/check`）は読み取りに近いので、signal から自動で送る。ただし、切り替えを少し待ってまとめ、前の照合のレスポンスを待ってから次を送り、古いレスポンスを捨てる（`run-page.ts` の `check`）。
- 変更を伴うリクエスト（開始、停止、時間のコマンド）は、ラッパーのメソッドで送る。レスポンス（いまの要約）は、ライブラリがその場でセッションのコピーに反映する。

**RxJS を使う所**:

- RxJS は、時間とともに続く流れに演算子が必要なときに使う（間引き、取り消し、いくつかの流れの組み合わせ）。サービスの中に置き、外へは signal を公開する。
- 流れそのものが必要な呼び出し側には、流れとわかる名前の Observable を公開し、呼び出し側が `toSignal`（`core/streams/stream-signals.ts` の `latestValue`、`recentValues`）で signal にする。
- 再接続と待ち時間は、ライブラリの `Connection` が持つ（RxJS を使わない）。
- 通知を自分で反映したい画面（毎秒数十回の値など）は、`ServerConnection.batches`（まとめて読んだ通知の Observable）を購読して、自分の signal に反映する。接続や認証を画面ごとに持たない。
- 画面へ毎秒数十回を超えて送らない（間引きはサーバーの仕事）。グラフには固定長の履歴（`recentValues`）を使う。

```ts
// よい: 流れはサービスの中の Observable、画面は signal で読む（画面が壊れると購読も止まる）
readonly samples = this.server.batches.pipe(map(toSamples)); // サービス
protected readonly latest = latestValue(this.telemetry.samples); // コンポーネント
```

```ts
// しない: ただの状態や 1 回の取得に RxJS を使う
readonly selected$ = new BehaviorSubject<string | null>(null); // → signal<string | null>(null)
readonly configs = toSignal(inject(HttpClient).get<ConfigList>('api/configs')); // → resource({ loader: () => this.info.configs() })
```

### effect

- 状態の同期に使わない（「A が変わったら B に set する」は `computed` か `linkedSignal` で書く）。
- 使ってよいのは signal の外の世界への反映だけ: ログ、`localStorage` への保存、外部のライブラリ。DOM を測る、描画するは `afterRenderEffect`（シェルの `updateContentMargins` が見本）。

```ts
effect(() => localStorage.setItem('selected', this.selected() ?? '')); // 外への反映だけ
```

### レイアウト

- 並びは CSS で決める（grid、flex、`repeat(auto-fill, minmax(...))`、container query、`clamp()`、論理プロパティ）。JavaScript で寸法を計算しない。
- 幅の区分は、下の表の 3 つ。境目は `core/layout/layout.ts` の `BREAKPOINTS` だけで決め、シェルが CSS の変数（`--app-breakpoint-*`）として渡す。寸法と間隔は `src/styles.css` のカスタムプロパティ。
- 幅の区分を読むのはシェルだけ。画面とコンポーネントは、中身の領域（container `content`）や自分の幅（container query、auto-fill）で並びを変える。CSS に窓の幅の `@media` を書かない。`:host-context` で区分を読まない。
- 区分が上がっても中身の領域は狭くならない（シェルが各区分の中身の幅を抑える）。
- 色は Material のシステムトークン（`--mat-sys-*`）。明暗は OS の設定に、動きは `prefers-reduced-motion` に従う。
- 画面を追加したら `e2e/layout.spec.ts` の `ROUTES` に追加する。8 つの幅（360、480、599、600、1024、1199、1200、1920）で、はみ出し（ページ、スクロールする要素、`main`、カード）、カードの重なり、ナビゲーションの形、キーボードを確認する。

| 区分    | 幅           | ナビゲーション |
| ------- | ------------ | -------------- |
| compact | 600 px 未満  | 引き出し       |
| medium  | 1200 px 未満 | アイコンの帯   |
| wide    | それ以上     | 文字つき       |

### アクセシビリティ

- ルートごとに `title` を付ける（「画面名 - vehicle-sil」）。画面ごとに `h1` を 1 つ。
- キーボードだけで全部を操作できる。Esc で引き出しを閉じる。
- 色だけで状態を伝えない（文字や形も添える）。
- コンポーネントは Material のもの（ボタン、リスト、カードなど）を使う。速く変わる値に `aria-live` を付けない。
- 状態の変わり目（実行の始まり、止めている、結果、異常終了、接続の切断）は `role="status"`、操作のエラーは `role="alert"` の領域で通知する。領域は中身が変わる前から置いておく（中身がない間は見た目から消す）。
- 押している最中に意味が変わってはいけない操作（開始と停止）は、別の要素にして、決まった位置にいつも置く。使えない方は消さずに `disabledInteractive` で押せなくし、フォーカスを受けたまま理由を `aria-describedby` で読ませる（同じ位置のボタンが、ダブルクリックや Enter の連打の途中で反対の操作に変わらない）。
- 押せるようになったばかりの操作は、`ACTION_GUARD_MS`（800 ms、`run/run-status.ts`）の間は押しても何もしない（`core/streams/stream-signals.ts` の `settledAfter`。キーボードも同じ）。その間は薄く表示する。e2e は `data-armed="true"` を待ってから押す（`support/run-page.ts` の `press`）。
- フォーカスを持った要素が消えるとき（「待たずに止める…」を確認した後など）は、反対の操作ではなく、いまの状態を言う要素（帯の結果の文）へフォーカスを移す。

### 国際化

画面の言葉は日本語を元にし（`angular.json` の `i18n.sourceLocale` は `ja`）、英語の訳を同じビルドに入れて配信します。言語ごとにビルドを分けません（Python のパッケージに 1 つのディレクトリで入れて配信するため）。

#### 言語の決め方

`core/i18n/language.ts` の `chooseLanguage` が、次の順に決めます。

| 順  | 条件                                                                             | 言語                                               |
| --- | -------------------------------------------------------------------------------- | -------------------------------------------------- |
| 1   | 言語のメニューで選んだ言語（`localStorage` の `vehicle-sil-gui-language`）がある | その言語                                           |
| 2   | ブラウザの言語（`navigator.languages`）に、対応する言語がある                    | 好みの順に見て、主な部分が対応する言語の最初のもの |
| 3   | どれも対応しない                                                                 | 英語                                               |

主な部分とは、`ja-JP` なら `ja` です。

#### 起動の順

1. `main.ts` が言語を決める。
2. 英語なら、`locale/messages.en.json` を同じオリジンから読んで `loadTranslations` に渡す。日本語は元の言語なので、翻訳ファイルを読まない。
3. `<html lang>` を合わせる。
4. `import()` で `start.ts`（アプリ）を読み込んで起動する。

アプリのモジュールを `main.ts` から静的に import しません。`$localize` は、文を最初に評価したときに訳を決め、モジュールを読み込むときに評価する所（`run/labels.ts` の表など）があるためです。`LOCALE_ID` は選んだ言語です（`Intl.ListFormat` などが従います）。

#### 言語の切り替え

- 言語を変えると、アプリを読み込み直す（訳は起動の前に決まる）。実行はサーバーの中で続き、画面は再接続して受け直す。
- サーバーにない画面の値（まだ送っていない選択）は、`LanguageChoice.keep` で渡して `take` で受け取る（実行の画面は、選んだファイル、無効にした participant、速度、ログのソース）。
- フォーカスは言語のメニューに戻す。
- 言語のメニューは、地球の絵と、いまの言語の名前（その言語自身の文字）を表示する。名前には、どの言語でも「Language」を含める。メニューの項目は、その言語自身の文字で書き、`lang` を付ける。

#### 文言の書き方

- テンプレートの文言には `i18n`（属性には `i18n-<attribute>`）、TypeScript の文言には `$localize` を付ける。印のない文言は lint で止まる。id は抽出のときに作られるもの（元の文から決まる）を使う。
- 文を連結して作らない（語順は言語で違う）。値は 1 つの文の中にプレースホルダー（テンプレートの `{{ }}`、`$localize` の `${value}:name:`）として入れる。
- 数で語の形が変わる文（「N 行」）は、テンプレートでは ICU の `plural` で書く（日本語は `other` だけ。英語の訳が `one` を追加する）。TypeScript では、数に左右されない書き方にする（「誤り: N」）。
- 計測値は `measure` パイプ（`core/format/measure-pipe.ts`）で表示する。桁区切りを入れず、小数の桁を決める。どの言語でも同じ書式で、locale の数値の書式に任せない。
- ECU やランチャーが出す英語の文（指摘、ログ）は訳さず、そのまま表示する（`lang="en"` を付ける）。
- 並べる語の区切りは `Intl.ListFormat`（`LOCALE_ID`）で作る。区切りや `:` を CSS の `content` で追加しない（訳せない）。
- 訳がない文（翻訳ファイルにない、`target` が `null` か空、プレースホルダーが元の文と合わない）は、元の日本語の文で表示される。id や空の文にはならない。翻訳ファイルを読めなければ、画面全体を日本語で表示する。

#### 翻訳ファイル

翻訳ファイル `src/locale/messages.<language>.json` は、抽出した文ごとに元の文と訳を並べた JSON です（読み比べやすく、差分で見やすい）。`target` が `null` の文は「まだ訳していない」印です。`note` は、そのファイルについての説明です。

```json
{
  "locale": "en",
  "note": "…",
  "translations": {
    "4418433101149864838": { "source": "停止", "target": "Stop" },
    "6952070187813782386": {
      "source": "{VAR_PLURAL, plural, other {{INTERPOLATION} 行}}",
      "target": "{VAR_PLURAL, plural, one {{INTERPOLATION} line} other {{INTERPOLATION} lines}}"
    }
  }
}
```

`scripts/verify-web.py` の i18n の段は、文を抽出して各翻訳ファイルと比較します（`scripts/web_i18n.py`）。

| 見つけたもの                                                 | 扱い                           |
| ------------------------------------------------------------ | ------------------------------ |
| 翻訳ファイルにない文                                         | 失敗                           |
| 元の文が抽出と違うもの                                       | 失敗                           |
| 空の訳                                                       | 失敗                           |
| プレースホルダー（`{$name}`）や ICU の形が元の文と合わない訳 | 失敗                           |
| `target` が `null` の文（まだ訳していないと印を付けた文）    | 失敗                           |
| もう抽出されない古い文                                       | 一覧に表示する（失敗にしない） |

訳しかけの間だけ手元で通すには、`--allow-untranslated` を付けます（CI では付けません）。

#### 文を追加する、変える

1. 文言に印を付ける（上の「文言の書き方」）。
2. `scripts/verify-web.py --i18n-update` で翻訳ファイルを合わせる。新しい文は `"target": null` で入り、古い文は消える（消した文と訳を表示するので、言い回しを変えただけなら訳を使い回せる）。
3. 各ファイルの `null` を訳に書き換える。`null` が残っている間は i18n の段が失敗する（訳しかけの間は、手元で `--allow-untranslated` を付けて実行できる）。
4. `scripts/verify-web.py --i18n-only` で確認する。

#### 言語を追加する

1. `core/i18n/language.ts` の `LANGUAGES` に、コードとその言語自身の文字の名前を追加する。
2. `src/locale/messages.<code>.json` を `{"locale": "<code>", "note": "…", "translations": {}}` で作り、`scripts/verify-web.py --i18n-update` で文を入れて訳す。
3. `en` 以外の言語なら、その locale のデータ（日付、複数形の規則）を `@angular/common/locales/<code>` から `registerLocaleData` で登録する（日本語はビルドが、英語は Angular が持っている）。
4. `e2e/i18n.spec.ts` のように、その言語で画面を確認する。

### サーバーとの間の型

- サーバーとの間でやりとりする形は、クライアントのライブラリの `gui/client/src/protocol.ts` にだけ、型と読み取り関数（`read*`）で書く（画面は `@vehicle-sil/gui-client` から import する）。形のリファレンスは `gui/protocol/`（README と `examples/`）。
- 届いた JSON は、読み取り関数を通してから使う（ライブラリが HTTP のレスポンスと WebSocket の通知を読む）。知らないフィールド、知らない通知とイベントは無視して通す。
- セッションの要約とランチャーのイベントは、部分ごとに読む。ランチャーから来る部分やサーバーが上限で縮めた部分が読めなくても、全体を捨てない。
- 読めない部分は既定の値（`null`、空）にして、その名前を `unreadable` に入れ、帯に「一部を表示できません」と表示する。
- 全体を読めないとするのは、要約の芯（`id`、`state`、`launcher`、`stop`）かイベントの芯（`seq`、`event`）が読めないときだけ（`hello` と `session` なら、再接続して受け直す）。
- ライブラリの `protocol.spec.ts` が、`gui/protocol/examples/` のすべての例を読ませる。例が変わったら、型と読み取り関数を合わせる。

### テストの書き方

**単体テスト**:

- 単体テストは TestBed。`Layout` は signal を持つ偽物に差し替える（jsdom に `matchMedia` がない）。
- `resource` は `TestBed.tick()` でリクエストを出させ、`HttpTestingController` で `flush` してから `whenStable()`（レスポンスを返していないリクエストがあると `whenStable()` が終わらない）。
- WebSocket は `FakeWebSockets`（`@vehicle-sil/gui-client/testing`）で差し替える（`{ provide: SOCKET_FACTORY, useValue: fake.factory }`）。HTTP は、ラッパーが `HttpClient` で送るので `HttpTestingController` で返す。時間は Vitest の偽の時計。
- サーバーの通知は、作らずに `gui/protocol/examples/` の例を流す（`webSocketExamples`、`httpExamples`）。
- 接続、セッションのコピー、時間と実行の操作のルールは、ライブラリのテスト（`gui/client/src/*.spec.ts`。Angular なし）で確認する。ラッパーのテストは、signal に変換されること、トークン、画面の言葉だけを見る。
- 「何かが起きないこと」を、決まった時間待ってから確認しない（遅いマシンで見落とす）。その時間が過ぎたとわかる後の出来事（後のイベント、サーバーの要約の `last_seq` や `stop`）を待ってから確認する。

**e2e**:

- 起きるはずのことは、`expect` の待ちか `expect.poll` で待つ。決まった時間だけ眠って「進んだはず」としない。
- 待ちの上限（設定ファイルの `timeout` と `expect.timeout`、`RUN_TIMEOUT`）には、環境変数 `VEHICLE_SIL_TEST_TIMEOUT_SCALE` の倍率が掛かる。上限を書くときは `scaled(ms)`（`e2e/support/timeouts.ts`）を通す。「起きないこと」を確認する時間と、画面に与える時間（ガードの長さなど）には掛けない。
- `getByRole` を先に使い、`data-testid` は補助。レイアウトは上の「レイアウト」。
- 相手は、本物の GUI サーバーと、本物のランチャーの出力の記録を再生するランチャー。設定ファイルと、送ったコマンドで記録が決まる。記録のない操作は、再生するランチャーが、何を記録すればよいかを言って終わる。
- 記録がいまの本物と合うかは、`gui/server/tests/test_real_launcher.py` が比較する。
- 本物のランチャーでしか起きないこと（例の ECU の `--describe` の出力、本物の止まり方）は、`e2e/real/` で確認する。
- 記録にない状況（切断、読んでいる間に流れ続ける行）は、`page.routeWebSocket` で通知を差し込んで作る。
- 見た目の確認とスクリーンショットは、窓を伸ばさずに、実際に見る大きさのままで行う（最初の画面に何が入っているかを見るため）。

## アイコン

`public/favicon.svg` が元です。`public/favicon.ico`（16、32、48 px）は SVG から ImageMagick で作り、SVG を変えたら作り直します。

```sh
cd public && magick -background none -density 384 favicon.svg -define icon:auto-resize=48,32,16 favicon.ico
```

## 依存のインストールスクリプト

`.npmrc` の `ignore-scripts=true` で、依存のインストールスクリプト（`preinstall`、`install`、`postinstall`）を実行しません。いまインストールしている依存は、どれもビルド済みのバイナリを別のパッケージ（`optionalDependencies`）で持つので、スクリプトがなくても動きます。`npm run build` のように明示したスクリプトは実行されますが、`prebuild` のような前後のスクリプトは実行されません。前後のスクリプトを書かずに、1 つのスクリプトにまとめます。依存を追加したら、インストールスクリプトがなくても動くことを確認します。

## 関連するドキュメント

| 知りたいこと                                                | ドキュメント                                                                       |
| ----------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| 自分の画面を一から作る                                      | [自分の画面を作る](../../docs/ja/howto/gui-screen.md)                              |
| この画面のコンポーネントを別の Angular のアプリへコピーする | [docs/gui.md](../../docs/ja/gui.md) の「サンプルのコンポーネントをコピーして使う」 |
| クライアントのライブラリの公開 API                          | [gui/client](../client/README.ja.md)                                               |
| サーバーの API の形                                         | [gui/protocol](../protocol/README.ja.md)                                           |
