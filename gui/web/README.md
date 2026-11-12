# gui/web

ブラウザで開く画面（Angular、TypeScript）です。ビルドした画面は Python の GUI サーバーが配信します。画面を使う人に Node.js は要りません。Node.js が要るのは、画面を開発、ビルドする人だけです。

## 要るもの

- Node.js 24（`.nvmrc`）。`package.json` の `engines` は Node.js 24.15 以上の 24 と 26、npm 11.12.1 以上です。`.npmrc` の `engine-strict` により、範囲の外では `npm ci` が止まります。
- npm（Node.js に付いてくるもの）。ほかのパッケージマネージャーは使いません。
- [uv](https://docs.astral.sh/uv/)（GUI サーバー `gui/server` を動かす。開発と e2e の試験で使う）。
- e2e の試験のブラウザ: `npx playwright install chromium --only-shell`（Playwright の版に合う Chromium をユーザーのキャッシュに入れる。管理者の権限は要らない）。

Windows でも同じコマンドで動かす想定です。Node.js は Windows 版を使います。`scripts/verify-web.py` は Windows の Python（python.org か MinGW のもの）で動かします。

## コマンド

`gui/web/` で実行します。どれも終了コードで成否を返します。

| コマンド               | 内容                                                                                                                                                 |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| `npm ci`               | `package-lock.json` のとおりに入れる                                                                                                                 |
| `npm start`            | 開発サーバー（http://127.0.0.1:4200）。`/api/` と `/ws` を GUI サーバー（127.0.0.1:8765）へ転送する（`proxy.conf.mjs`。ポートは環境変数 `GUI_PORT`） |
| `npm run build`        | 本番のビルド。出力は `dist/browser/`、ライセンスの表示は `dist/3rdpartylicenses.txt`                                                                 |
| `npm test`             | 単体試験（Vitest、jsdom）。1 回走って終わる                                                                                                          |
| `npm run lint`         | ESLint（angular-eslint）。警告も失敗にする                                                                                                           |
| `npm run format:check` | Prettier の書式の検査                                                                                                                                |
| `npm run e2e`          | Playwright。GUI サーバーが `dist/browser/` と API を同じオリジンで配信する。先に `npm run build` が要る                                              |
| `npm run e2e:dev`      | Playwright。開発サーバーと、その転送（WebSocket も）を通す                                                                                           |
| `npm run i18n:extract` | 翻訳する文言を `src/locale/messages.xlf` に書き出す                                                                                                  |

リポジトリの直下から `scripts/verify-web.py` を実行すると、`npm ci`、書式、lint、単体試験、ビルド、e2e を順に回して要約だけを表示します。

開発のときは、2 つの端末で次のように動かします。GUI サーバーは偽のランチャー（`gui/server/tests/fake_launcher.py`）を使い、`gui/server/tests/configs/` の構成ファイル（ファイルごとに筋書きが決まっている）を選べます。サーバーが表示する `dev_url`（トークンつき）を開きます。

```sh
uv run vehicle-sil-gui --no-browser --json --port 8765 --dev-origin http://127.0.0.1:4200 \
  --launcher ../server/tests/fake_launcher.py --root ../server/tests/configs   # GUI サーバー
npm start                                                                     # 画面（127.0.0.1:4200）
```

e2e の試験（`npm run e2e`、`npm run e2e:dev`）は、GUI サーバーを偽のランチャーと一緒に自分で起動し（`e2e/support/gui-server.ts`。ポートは 18000 から 18999 のどれか）、終わったら止めます。

## ディレクトリ

| パス                                     | 内容                                                                                                                                                                                                                                  |
| ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/app/app.*`                          | シェル（ツールバー、ナビゲーション、中身の領域）                                                                                                                                                                                      |
| `src/app/app.routes.ts`、`navigation.ts` | 経路と、ナビゲーションの項目                                                                                                                                                                                                          |
| `src/app/core/`                          | 画面をまたいで使うもの: サーバーとの間の型と読み取り（`api.ts`）、トークン（`auth/`）、URL（`urls.ts`）、幅の区分（`layout/`）、計測値とパスの書式（`format/`）、流れ（`streams/`）、サーバーとの接続とセッションの状態（`session/`） |
| `src/app/core/session/`                  | `server-connection.ts`（アプリ全体で 1 つの WebSocket の接続。認証、再接続、要約と事象を当てたセッションの状態）と `session-state.ts`（要約と事象を当てる純粋な関数）                                                                 |
| `src/app/run/`                           | 実行の画面（`''`）。操作は `run-session.ts`（開始、停止、待たずに止める。画面と同じ寿命）、いつも見える状態の帯は `run-status.ts`                                                                                                     |
| `src/app/not-found/`                     | 見つからない経路の画面                                                                                                                                                                                                                |
| `src/app/testing/`                       | 試験用の画面（`/_test/layout`。レイアウトの e2e が使う。ナビゲーションには出さない）と、単体試験が `gui/protocol/examples/` を読む関数                                                                                                |
| `src/theme.scss`、`src/styles.css`       | Material のテーマ（明暗）、レイアウトの寸法（CSS のカスタムプロパティ）                                                                                                                                                               |
| `public/`                                | そのまま配信するファイル（アイコン）                                                                                                                                                                                                  |
| `e2e/`                                   | Playwright の試験。`app.spec.ts`（配信と経路）、`run.spec.ts`（実行の画面）、`layout.spec.ts` と `support/layout.ts`（レイアウト）、`support/gui-server.ts`（GUI サーバーの起動）                                                     |

## URL と CSP

API と WebSocket の URL は、先頭に `/` を付けずに書きます（`api/health`、`ws`）。`index.html` の `<base href>` からの相対になるので、`/` 以外のパスに置いても動きます。そのときは `ng build --base-href /gui/` でビルドします。WebSocket の URL は `document.baseURI` から作ります（`core/urls.ts` の `webSocketUrl`）。

本番のビルドは inline の script を出しません（`angular.json` で `inlineCritical: false`）。`script-src 'self'` の CSP で動きます。コンポーネントのスタイルは `<style>` で入るので、`style-src` には `'unsafe-inline'`（または nonce）が要ります。

## 書き方の決まり

いまの Angular（22）の書き方にそろえます。どこに書いても誤りと言えるものは `npm run lint` で止まります。判断が要るもの（RxJS をどこで使うかなど）はここに書き、レビューで見ます。

### 置き方と命名

- ファイル名と型の名前は `ng generate` の既定に合わせる（`ng g service telemetry` → `telemetry.ts` の `Telemetry`、`ng g component not-found` → `not-found/not-found.ts` の `NotFound`、`ng g pipe measure` → `measure-pipe.ts` の `MeasurePipe`）。
- 画面ごとにディレクトリを作る（`src/app/<画面>/`）。画面をまたいで使うものは `src/app/core/` に置く。
- 試験で探す要素には、役割（`getByRole`）で探せないときだけ `data-testid` を付ける。

### Angular の書き方（lint で強制）

- コンポーネントは standalone。依存は `inject()`。入出力と問い合わせは `input()`、`output()`、`model()`、`viewChild()`。`@HostBinding` の代わりに `host`。
- テンプレートは `@if`、`@for`、`@switch`、`@defer`。`async` パイプを使わない（テンプレートが読むのは signal）。
- zone.js は使わない（zoneless）。変更の検出は OnPush（既定）のまま。
- 実験的、開発者プレビューの API を土台にしない。安定した代わりを使う。
- guard、interceptor、resolver は関数で書く。経路の引数は `input()` で受け取る（`withComponentInputBinding`）。

### 状態の持ち場

- 1 つの画面だけの状態は、そのコンポーネントの signal。
- 機能ごとの状態とサーバーからの値は、その機能のサービス。サービスは読み取り専用の signal（`asReadonly()`）と、変えるためのメソッドだけを出す。
- アプリ全体の状態（幅の区分など）は `core/` のサービス。

### signal と RxJS

- 状態、そこから導く値、テンプレートが読むものは signal（`signal`、`computed`、`linkedSignal`）。
- 1 回の取得は `httpResource`（ほかの非同期の値は `resource`）。`HttpClient` と `toSignal` で書かない。
- RxJS は、時間とともに続く流れに演算子が要るときに使う（再接続と待ち時間、間引き、取り消し、いくつかの流れの組み合わせ）。サービスの中に置き、外へは signal を出す。流れそのものが要る利用者には、流れとわかる名前の Observable を出し、利用者が `toSignal`（`core/streams/stream-signals.ts` の `latestValue`、`recentValues`）で signal にする。
- 見本は `core/streams/live-connection.ts`（WebSocket の差し替え口、購読の寿命、再接続、つなぐたびに最初に送るもの）と `core/session/server-connection.ts`（流れをまとめて状態の signal に当てる。アプリ全体で 1 つで、画面を移っても切れない）、試験は `live-connection.spec.ts` と `server-connection.spec.ts`（偽の WebSocket）。
- サーバーとの接続は `ServerConnection` だけが持つ。画面はその signal を読む。自分で流れを当てたい画面（毎秒数十回の値など）は、`batches`（まとめた通知の Observable）を購読して自分の signal に当てる。接続や認証を画面ごとに持たない。
- 変更を伴う要求（開始、停止）は `HttpClient` で送り、応答（いまの要約）をその場で `ServerConnection.adopt` に当てる。
- 照合（`POST api/check`）は読み取りに近いので signal から自動で送るが、`httpResource` ではなく `resource` で書く。切り替えを少し待ってまとめ、前の照合の応答を待ってから次を送り、古い応答を捨てるため（`httpResource` は前の要求を取り消すだけで、サーバーでは照合が続く）。
- 画面へ毎秒数十回を超えて送らない（間引きはサーバーの仕事）。グラフには固定長の履歴（`recentValues`）を使う。

```ts
// よい: 流れはサービスの中の RxJS、画面は signal で読む（画面が壊れると購読も止まる）
readonly samples = liveConnection<Sample>('ws').messages; // サービス
protected readonly latest = latestValue(this.telemetry.samples); // コンポーネント
```

```ts
// しない: ただの状態や 1 回の取得に RxJS を使う
readonly selected$ = new BehaviorSubject<string | null>(null); // → signal<string | null>(null)
readonly health = toSignal(inject(HttpClient).get<Health>('api/health')); // → httpResource
```

### effect

- 状態の同期に使わない（「A が変わったら B に set する」は `computed` か `linkedSignal` で書く）。
- 使ってよいのは signal の外の世界への反映だけ: ログ、`localStorage` への保存、外部のライブラリ。DOM を測る、描くは `afterRenderEffect`（シェルの `updateContentMargins` が見本）。

```ts
effect(() => localStorage.setItem('selected', this.selected() ?? '')); // 外への反映だけ
```

### レイアウト

- 並びは CSS で決める（grid、flex、`repeat(auto-fill, minmax(...))`、container query、`clamp()`、論理プロパティ）。JavaScript で寸法を計算しない。
- 幅の区分は 3 つ: compact（600 px 未満、ナビゲーションは引き出し）、medium（1200 px 未満、アイコンの帯）、wide（文字つき）。境目は `core/layout/layout.ts` の `BREAKPOINTS` だけで決め、シェルが CSS の変数（`--app-breakpoint-*`）として渡す。寸法と間隔は `src/styles.css` のカスタムプロパティ。
- 幅の区分を読むのはシェルだけ。画面と部品は、中身の領域（container `content`）や自分の幅（container query、auto-fill）で並びを変える。CSS に窓の幅の `@media` を書かない。`:host-context` で区分を読まない。
- 区分が上がっても中身の領域は狭くならない（シェルが各区分の中身の幅を抑える）。
- 色は Material のシステムトークン（`--mat-sys-*`）。明暗は OS の設定に、動きは `prefers-reduced-motion` に従う。
- 画面を足したら `e2e/layout.spec.ts` の `ROUTES` に足す。8 つの幅（360、480、599、600、1024、1199、1200、1920）で、はみ出し（ページ、スクロールする要素、`main`、カード）、カードの重なり、ナビゲーションの形、キーボードを確かめる。

### アクセシビリティ

- 経路ごとに `title` を付ける（「画面名 - vehicle-sil」）。画面ごとに `h1` を 1 つ。
- キーボードだけで全部を操作できる。Esc で引き出しを閉じる。
- 色だけで状態を伝えない（文字や形も添える）。
- 部品は Material のもの（ボタン、リスト、カードなど）を使う。速く変わる値に `aria-live` を付けない。
- 状態の変わり目（実行の始まり、止めている、結果、異常終了、接続の切断）は `role="status"`、操作の誤りは `role="alert"` の領域で知らせる。領域は中身が変わる前から置いておく（中身がない間は見た目から消す）。
- 押すと役目が変わるボタン（開始と停止）は、同じ要素のまま文言と動きを変える（フォーカスが残る）。押せない間は `disabledInteractive` で、フォーカスを受けたまま理由を `aria-describedby` で読ませる。

### 国際化

- 画面の言葉は日本語を元にする（`angular.json` の `i18n.sourceLocale` は `ja`。`LOCALE_ID` と `<html lang>` も `ja`）。
- テンプレートの文言には `i18n`（属性には `i18n-<属性>`）、TypeScript の文言には `$localize` を付ける。英語は、翻訳ファイルを足して後から出せる。印のない文言は lint で止まる。
- 計測値は `measure` パイプ（`core/format/measure-pipe.ts`）で出す。桁区切りを入れず、小数の桁を決める。locale の数値の書式に任せない。
- ECU やランチャーが出す英語の文（所見、ログ）は訳さず、そのまま出す。
- 翻訳はビルドのときだけ。`run/labels.ts` の表のように、モジュールを読み込むときに `$localize` を評価するところがあるので、実行時に翻訳を読み込む形にはしない。
- 並べる語の区切りは `Intl.ListFormat`（`LOCALE_ID`）で作る。区切りや `:` を CSS の `content` で足さない（訳せない）。

### サーバーとの間の型

- サーバーとの間でやりとりする形は `core/api.ts` にだけ、型と読み取り関数（`read*`）で書く。形の正本は `gui/protocol/`（README と `examples/`）。届いた JSON は読み取り関数を通してから使う（`httpResource` の `parse`、WebSocket の通知）。知らない欄、知らない通知と事象は無視して通す。
- `api.spec.ts` が `gui/protocol/examples/` のすべての例を読ませる。例が変わったら、型と読み取り関数を合わせる。

### 試験の書き方

- 単体試験は TestBed。`Layout` は signal を持つ偽物に差し替える（jsdom に `matchMedia` がない）。
- `httpResource` は `TestBed.tick()` で要求を出させ、`HttpTestingController` で `flush` してから `whenStable()`（応答していない要求があると `whenStable()` が終わらない）。
- WebSocket は `FakeWebSockets`（`core/streams/fake-web-socket.ts`）で差し替える。時間は Vitest の偽の時計。サーバーの通知は、作らずに `gui/protocol/examples/` の例を流す（`testing/protocol-examples.ts`）。
- e2e は `getByRole` を先に使い、`data-testid` は補助。レイアウトは上の「レイアウト」。e2e は本物の GUI サーバーと偽のランチャーを相手にする（筋書きは構成ファイルで選ぶ）。筋書きにない状況（切断、読んでいる間に流れ続ける行）は、`page.routeWebSocket` で通知を差し込んで作る。
- 見た目の確かめとスクリーンショットは、窓を伸ばさずに利用者が見る大きさのままで行う（最初の画面に何が入っているかを見るため）。

## アイコン

`public/favicon.svg` が元です。`public/favicon.ico`（16、32、48 px）は SVG から ImageMagick で作り、SVG を変えたら作り直します。

```sh
cd public && magick -background none -density 384 favicon.svg -define icon:auto-resize=48,32,16 favicon.ico
```

## 依存の導入スクリプト

`.npmrc` の `ignore-scripts=true` で、依存の導入スクリプト（`preinstall`、`install`、`postinstall`）を走らせません。いま入っている依存は、どれもビルド済みのバイナリを別のパッケージ（`optionalDependencies`）で持つので、スクリプトがなくても動きます。`npm run build` のように明示したスクリプトは走りますが、`prebuild` のような前後のスクリプトは走りません。前後のスクリプトを書かずに、1 つのスクリプトにまとめます。依存を足したら、導入スクリプトがなくても動くことを確かめます。
