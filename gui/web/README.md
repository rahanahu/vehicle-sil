# gui/web

ブラウザで開く画面（Angular、TypeScript）です。ビルドした画面は Python の GUI サーバーが配信します。画面を使う人に Node.js は要りません。Node.js が要るのは、画面を開発、ビルドする人だけです。

## 要るもの

- Node.js 24（`.nvmrc`）。`package.json` の `engines` は Node.js 24.15 以上の 24 と 26、npm 11.12.1 以上です。`.npmrc` の `engine-strict` により、範囲の外では `npm ci` が止まります。
- npm（Node.js に付いてくるもの）。ほかのパッケージマネージャーは使いません。
- [uv](https://docs.astral.sh/uv/)（代役のサーバーと e2e の試験で使う）。
- e2e の試験のブラウザ: `npx playwright install chromium --only-shell`（Playwright の版に合う Chromium をユーザーのキャッシュに入れる。管理者の権限は要らない）。

Windows でも同じコマンドで動かす想定です。Node.js は Windows 版を使います。`scripts/verify-web.py` は Windows の Python（python.org か MinGW のもの）で動かします。

## コマンド

`gui/web/` で実行します。どれも終了コードで成否を返します。

| コマンド               | 内容                                                                                                                                   |
| ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| `npm ci`               | `package-lock.json` のとおりに入れる                                                                                                   |
| `npm start`            | 開発サーバー（http://127.0.0.1:4200）。`/api/` と `/ws` を 127.0.0.1:8765 へ転送する（`proxy.conf.mjs`。ポートは環境変数 `STUB_PORT`） |
| `npm run build`        | 本番のビルド。出力は `dist/browser/`、ライセンスの表示は `dist/3rdpartylicenses.txt`                                                   |
| `npm test`             | 単体試験（Vitest、jsdom）。1 回走って終わる                                                                                            |
| `npm run lint`         | ESLint（angular-eslint）。警告も失敗にする                                                                                             |
| `npm run format:check` | Prettier の書式の検査                                                                                                                  |
| `npm run e2e`          | Playwright。代役のサーバーが `dist/browser/` と API を同じオリジンで配信する。先に `npm run build` が要る                              |
| `npm run e2e:dev`      | Playwright。開発サーバーと、その転送（WebSocket も）を通す                                                                             |
| `npm run i18n:extract` | 翻訳する文言を `src/locale/messages.xlf` に書き出す                                                                                    |

リポジトリの直下から `scripts/verify-web.py` を実行すると、`npm ci`、書式、lint、単体試験、ビルド、e2e を順に回して要約だけを表示します。

開発のときは、2 つの端末で次のように動かします。

```sh
uv run dev/stub_server.py   # 代役のサーバー（127.0.0.1:8765）
npm start                   # 画面（127.0.0.1:4200）
```

## ディレクトリ

| パス                                     | 内容                                                                                                                                           |
| ---------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/app/app.*`                          | シェル（ツールバー、ナビゲーション、中身の領域）                                                                                               |
| `src/app/app.routes.ts`、`navigation.ts` | 経路と、ナビゲーションの項目                                                                                                                   |
| `src/app/core/`                          | 画面をまたいで使うもの: サーバーとの間の型（`api.ts`）、URL（`urls.ts`）、幅の区分（`layout/`）、計測値の書式（`format/`）、流れ（`streams/`） |
| `src/app/not-found/`                     | 見つからない経路の画面                                                                                                                         |
| `src/app/testing/`                       | 試験用の画面（`/_test/layout`。レイアウトの e2e が使う。ナビゲーションには出さない）                                                           |
| `src/app/smoke/`                         | 仮の画面（後で捨てる）                                                                                                                         |
| `src/theme.scss`、`src/styles.css`       | Material のテーマ（明暗）、レイアウトの寸法（CSS のカスタムプロパティ）                                                                        |
| `public/`                                | そのまま配信するファイル（アイコン）                                                                                                           |
| `e2e/`                                   | Playwright の試験。`app.spec.ts`（配信と経路）、`layout.spec.ts` と `support/layout.ts`（レイアウト）                                          |
| `dev/stub_server.py`                     | 代役のサーバー（依存のロックは `stub_server.py.lock`）                                                                                         |

## 仮の画面（後で捨てる）

`src/app/smoke/` と `e2e/smoke/` は、道具の組み合わせを確かめるための仮の画面とその試験です。どのファイルも先頭に「通しの確認用（後で捨てる）」と書いてあります。本物の画面ができたら、次のように消します。

1. `src/app/smoke/` と `e2e/smoke/` を消す。
2. `src/app/app.routes.ts` の `SMOKE_ROUTES` と、`src/app/navigation.ts` の `SMOKE_NAV` を消し、最初の画面の経路（`''`）を足す。

## 代役のサーバー

`dev/stub_server.py` は、画面の開発と試験のための代役です。本物の GUI サーバーではありません。本物ができるまで、e2e の試験で画面を配信するのに使います。依存はファイルの先頭に書いてあり（PEP 723）、`uv run` で動きます。127.0.0.1 だけで待ちます。

- `GET /api/health`: 状態を JSON で返す
- `/ws`: WebSocket。決まった間隔で数値を JSON で流す（`--interval`）
- `--static <dir>`: ビルド済みの画面も配信する。ファイルがなく拡張子もないパスには `index.html` を返す（SPA の戻し先）。`api/` と `ws` の下と、拡張子のあるファイルがないときは 404。画面には厳しい CSP（inline の script を禁じる）を付ける
- `--mount /gui/`: 入り口を `/` 以外のパスに置く

開発サーバーの転送を通すと、サーバーに届く `Host` と `Origin` は開発サーバーのもの（`127.0.0.1:4200`）になります。本物のサーバーで `Host` と `Origin` を検査するときは、開発のときにこれを許す設定が要ります。

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
- 見本は `core/streams/live-connection.ts`（WebSocket の差し替え口、購読の寿命、再接続）と `smoke/telemetry.ts`、`smoke/smoke-page.ts`、試験は `live-connection.spec.ts`（偽の WebSocket）。
- 画面へ毎秒数十回を超えて送らない（間引きはサーバーの仕事）。グラフには固定長の履歴（`recentValues`）を使う。

```ts
// よい: 流れはサービスの中の RxJS、画面は signal で読む（画面が壊れると購読も止まる）
readonly samples = liveConnection<TelemetrySample>('ws').messages; // サービス
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

### 国際化

- 画面の言葉は日本語を元にする（`angular.json` の `i18n.sourceLocale` は `ja`。`LOCALE_ID` と `<html lang>` も `ja`）。
- テンプレートの文言には `i18n`（属性には `i18n-<属性>`）、TypeScript の文言には `$localize` を付ける。英語は、翻訳ファイルを足して後から出せる。印のない文言は lint で止まる。
- 計測値は `measure` パイプ（`core/format/measure-pipe.ts`）で出す。桁区切りを入れず、小数の桁を決める。locale の数値の書式に任せない。
- ECU やランチャーが出す英語の文（所見、ログ）は訳さず、そのまま出す。

### サーバーとの間の型

- サーバーとの間でやりとりする形は `core/api.ts` にだけ型で書く。本物のサーバーの契約ができたら、それに合わせて置き換える。

### 試験の書き方

- 単体試験は TestBed。`Layout` は signal を持つ偽物に差し替える（jsdom に `matchMedia` がない）。
- `httpResource` は `TestBed.tick()` で要求を出させ、`HttpTestingController` で `flush` してから `whenStable()`（応答していない要求があると `whenStable()` が終わらない）。
- WebSocket は `FakeWebSockets`（`core/streams/fake-web-socket.ts`）で差し替える。時間は Vitest の偽の時計。
- e2e は `getByRole` を先に使い、`data-testid` は補助。レイアウトは上の「レイアウト」。

## アイコン

`public/favicon.svg` が元です。`public/favicon.ico`（16、32、48 px）は SVG から ImageMagick で作り、SVG を変えたら作り直します。

```sh
cd public && magick -background none -density 384 favicon.svg -define icon:auto-resize=48,32,16 favicon.ico
```

## 依存の導入スクリプト

`.npmrc` の `ignore-scripts=true` で、依存の導入スクリプト（`preinstall`、`install`、`postinstall`）を走らせません。いま入っている依存は、どれもビルド済みのバイナリを別のパッケージ（`optionalDependencies`）で持つので、スクリプトがなくても動きます。`npm run build` のように明示したスクリプトは走りますが、`prebuild` のような前後のスクリプトは走りません。前後のスクリプトを書かずに、1 つのスクリプトにまとめます。依存を足したら、導入スクリプトがなくても動くことを確かめます。
