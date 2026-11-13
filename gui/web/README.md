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

| コマンド               | 内容                                                                                                                                                                 |
| ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `npm ci`               | `package-lock.json` のとおりに入れる                                                                                                                                 |
| `npm start`            | 開発サーバー（http://127.0.0.1:4200）。`/api/` と `/ws` を GUI サーバー（127.0.0.1:8765）へ転送する（`proxy.conf.mjs`。ポートは環境変数 `GUI_PORT`）                 |
| `npm run build`        | 本番のビルド。出力は `dist/browser/`、ライセンスの表示は `dist/3rdpartylicenses.txt`                                                                                 |
| `npm test`             | 単体試験（Vitest、jsdom）。1 回走って終わる                                                                                                                          |
| `npm run test:client`  | クライアントのライブラリ（`../client`）の型の検査と単体試験（Vitest、Node。Angular も DOM もなし）                                                                   |
| `npm run build:client` | クライアントのライブラリを 1 つの ESM のファイルにまとめる（`../../build/gui-client/vehicle-sil-gui-client.mjs`。esbuild は Angular のビルドの道具が持っているもの） |
| `npm run lint`         | ESLint（angular-eslint）。警告も失敗にする。`../client` も、その `eslint.config.js` で                                                                               |
| `npm run format:check` | Prettier の書式の検査（`../client` も。設定は `../.prettierrc`）                                                                                                     |
| `npm run e2e`          | Playwright。GUI サーバーが `dist/browser/` と API を同じオリジンで配信する。先に `npm run build` が要る                                                              |
| `npm run e2e:dev`      | Playwright。開発サーバーと、その転送（WebSocket も）を通す                                                                                                           |
| `npm run e2e:real`     | Playwright。本物のランチャー（環境変数 `VEHICLE_SIL_GUI_LAUNCHER`）と、その隣にビルドした ecu_main の例の ECU で通す（`e2e/real/`）。先に `npm run build` が要る     |
| `npm run i18n:extract` | 翻訳する文言を `build/i18n/messages.json`（リポジトリの直下の `build/`）に書き出す。翻訳ファイルとの比べと更新は `scripts/verify-web.py`（下の「国際化」）           |

リポジトリの直下から `scripts/verify-web.py` を実行すると、`npm ci`、書式、lint、単体試験、クライアントのライブラリの単体試験、翻訳ファイルの比べ、ビルド、e2e を順に回して要約だけを表示します。e2e を直している間は `--skip-checks`（書式、lint、単体試験、翻訳ファイルの比べを省く）、`--e2e-grep <名前の一部>`、`--e2e-repeat <回数>` で絞れます。

開発のときは、2 つの端末で次のように動かします。GUI サーバーは偽のランチャー（`gui/server/tests/fake_launcher.py`）を使い、`gui/server/tests/configs/` の構成ファイル（ファイルごとに筋書きが決まっている）を選べます。サーバーが表示する `dev_url`（トークンつき）を開きます。

```sh
uv run vehicle-sil-gui --no-browser --json --port 8765 --dev-origin http://127.0.0.1:4200 \
  --launcher ../server/tests/fake_launcher.py --root ../server/tests/configs   # GUI サーバー
npm start                                                                     # 画面（127.0.0.1:4200）
```

e2e の試験（`npm run e2e`、`npm run e2e:dev`）は、GUI サーバーを偽のランチャーと一緒に自分で起動し（`e2e/support/gui-server.ts`。ポートは 18000 から 18999 のどれか）、終わったら止めます。`npm run e2e:real` は、本物のランチャーで同じように起動し（`e2e/support/real-server.ts`。構成ファイルは `gui/server/tests/real_configs.py` が一時ディレクトリに書く）、環境変数 `VEHICLE_SIL_GUI_SHOTS` にディレクトリを書くと、4 つの状態を 3 つの幅で撮ります。`scripts/verify-gui.py --launcher <path>` が回します。偽のランチャーの e2e も、`VEHICLE_SIL_GUI_SHOTS` を書くと、実行の画面の 6 つの状態を 360 px と 1024 px で撮ります（`e2e/shots.spec.ts`。状態ごとに新しい GUI サーバーを起動するので、前の実行は写りません。`VEHICLE_SIL_GUI_SHOTS_LANGUAGE=en` で英語の画面を撮ります）。

## ディレクトリ

| パス                                     | 内容                                                                                                                                                                                                                                                                                                                                                                                                                              |
| ---------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/app/app.*`                          | シェル（ツールバー、ナビゲーション、中身の領域）                                                                                                                                                                                                                                                                                                                                                                                  |
| `src/app/app.routes.ts`、`navigation.ts` | 経路と、ナビゲーションの項目                                                                                                                                                                                                                                                                                                                                                                                                      |
| `src/app/core/`                          | 画面をまたいで使うもの: トークン（`auth/`）、幅の区分（`layout/`）、計測値とパスの書式（`format/`）、流れ（`streams/`）、クライアントのライブラリの包み（`session/`）、画面の言語（`i18n/`）。サーバーとの間の型と読み取り、接続、セッションの状態はクライアントのライブラリ（`../client`、`@vehicle-sil/gui-client`）にある                                                                                                      |
| `src/app/core/session/`                  | クライアントのライブラリの Angular の包み（読める値を signal にするだけ）: `gui-client.ts`（アプリ全体で 1 つのクライアントを作る。HTTP は HttpClient、`SOCKET_FACTORY`、`readableSignal`）、`server-connection.ts`（接続とセッションの写し）、`server-info.ts`（`api/app` と 1 回の要求）、`simulation-time.ts`（時刻）、`run-commands.ts`（開始と停止）、`ecu-variables.ts`（ECU の変数）                                       |
| `src/app/run/`                           | 実行の画面（`''`）。操作は `run-session.ts`（開始、停止、待たずに止める、経過時間。画面と同じ寿命。中身は包みの `RunCommands`）、いつも見える状態の帯は `run-status.ts`                                                                                                                                                                                                                                                           |
| `src/app/widgets/`                       | 変数の部品（値の表示、数値の入力、スライダー、スイッチ、ランプ、選択肢、グラフ）、部品と変数をつなぐ `variable-binding.ts`、部品の枠 `variable-frame.ts`、ダッシュボードのデータと既定の部品、ダッシュボードのファイルからの組み立て `dashboard-spec.ts`、それを描く `dashboard-view.ts`、持ち込みのパネルを置く `panel-host.ts`。包みの公開の口だけを使う（自分で部品を足す人と同じ方法。docs/gui.md の「変数の画面と部品」）    |
| `src/app/variables/`                     | 変数の画面（`variables`）。自動のダッシュボードと絞り込み                                                                                                                                                                                                                                                                                                                                                                         |
| `src/app/not-found/`                     | 見つからない経路の画面                                                                                                                                                                                                                                                                                                                                                                                                            |
| `src/app/testing/`                       | 試験用の画面（`/_test/layout`。レイアウトの e2e が使う。ナビゲーションには出さない）。単体試験の部品（偽の WebSocket と HTTP、`gui/protocol/examples/` を読む関数）は `@vehicle-sil/gui-client/testing`                                                                                                                                                                                                                           |
| `src/theme.scss`、`src/styles.css`       | Material のテーマ（明暗）、レイアウトの寸法（CSS のカスタムプロパティ）                                                                                                                                                                                                                                                                                                                                                           |
| `src/main.ts`、`src/start.ts`            | 起動。`main.ts` が言語を決めて翻訳を読み込み、それから `start.ts`（アプリ）を読み込む                                                                                                                                                                                                                                                                                                                                             |
| `src/locale/`                            | 翻訳ファイル（`messages.<言語>.json`）。ビルドで `locale/` に置かれる                                                                                                                                                                                                                                                                                                                                                             |
| `public/`                                | そのまま配信するファイル（アイコン）                                                                                                                                                                                                                                                                                                                                                                                              |
| `e2e/`                                   | Playwright の試験。`app.spec.ts`（配信と経路）、`run.spec.ts`（実行の画面）、`i18n.spec.ts`（英語の画面と言語の切り替え）、`layout.spec.ts` と `support/layout.ts`（レイアウト）、`support/gui-server.ts`（GUI サーバーの起動）、`support/run-page.ts`（実行の画面の操作）、`variables.spec.ts` と `support/variables-page.ts`（変数の画面）、`shots.spec.ts`（写真）、`real/`（本物のランチャーで。`playwright.real.config.ts`） |

## URL と CSP

API と WebSocket の URL は、先頭に `/` を付けずに書きます（`api/health`、`ws`）。`index.html` の `<base href>` からの相対になるので、`/` 以外のパスに置いても動きます。そのときは `ng build --base-href /gui/` でビルドします。WebSocket の URL は `document.baseURI` から作ります（クライアントのライブラリの `webSocketUrl`。`core/session/gui-client.ts` が `baseUrl` に `document.baseURI` を渡す）。

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
- サーバーへの要求と接続は、クライアントのライブラリ（`@vehicle-sil/gui-client`）だけが持つ。画面は `core/session/` の包み（`ServerConnection`、`ServerInfo`、`SimulationTime`、`RunCommands`）の signal とメソッドを使い、`HttpClient` や WebSocket を自分で持たない（自分の画面を作る人と同じ部品だけを使う。docs/gui.md の「自分の画面や道具を作る」）。ライブラリの HTTP は、包みが `HttpClient` で送る（interceptor がトークンを付ける）。
- ライブラリの状態は「読める値」（`get()` と `subscribe()`）。包みは `readableSignal`（`core/session/gui-client.ts`）で signal にする。ライブラリが足りないときは、包みで近道を作らず、ライブラリの公開の入口（`@vehicle-sil/gui-client`）に足す。本番のコードは `@vehicle-sil/gui-client/internal`（互換の約束の外）と `/testing` を import しない（lint で止まる。試験は使ってよい）。購読が投げたものは、Angular の `ErrorHandler` へ渡る。
- 1 回の取得（`api/app`、`api/configs`）は、包みの Promise を返すメソッドを `resource` の loader で呼ぶ。照合（`POST api/check`）は読み取りに近いので signal から自動で送るが、切り替えを少し待ってまとめ、前の照合の応答を待ってから次を送り、古い応答を捨てる（`run-page.ts` の `check`）。
- RxJS は、時間とともに続く流れに演算子が要るときに使う（間引き、取り消し、いくつかの流れの組み合わせ）。サービスの中に置き、外へは signal を出す。流れそのものが要る利用者には、流れとわかる名前の Observable を出し、利用者が `toSignal`（`core/streams/stream-signals.ts` の `latestValue`、`recentValues`）で signal にする。再接続と待ち時間はライブラリの `Connection` が持つ（RxJS を使わない）。
- 自分で通知を当てたい画面（毎秒数十回の値など）は、`ServerConnection.batches`（まとめて読んだ通知の Observable）を購読して自分の signal に当てる。接続や認証を画面ごとに持たない。
- 変更を伴う要求（開始、停止、時刻の命令）は包みのメソッドで送る。応答（いまの要約）はライブラリがその場でセッションの写しに当てる。
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
- 押している最中に意味が変わってはいけない操作（開始と停止）は、別の要素にして、決まった位置にいつも置く。使えない方は消さずに `disabledInteractive` で押せなくし、フォーカスを受けたまま理由を `aria-describedby` で読ませる（同じ位置のボタンが、ダブルクリックや Enter の連打の途中で反対の操作に変わらない）。
- 押せるようになったばかりの操作は、`ACTION_GUARD_MS`（800 ms、`run/run-status.ts`）の間は押しても何もしない（`core/streams/stream-signals.ts` の `settledAfter`。キーボードも同じ）。その間は薄く出す。e2e は `data-armed="true"` を待ってから押す（`support/run-page.ts` の `press`）。
- フォーカスを持った要素が消えるとき（「待たずに止める…」を確かめた後など）は、反対の操作ではなく、いまの状態を言う要素（帯の結果の文）へフォーカスを移す。

### 国際化

画面の言葉は日本語を元にし（`angular.json` の `i18n.sourceLocale` は `ja`）、英語の訳を同じビルドに入れて出します。言語ごとにビルドを分けません（Python のパッケージに 1 つのディレクトリで入れて配信するため）。

- 言語の決め方（`core/i18n/language.ts` の `chooseLanguage`）: 利用者が言語のメニューで選んだ言語（`localStorage` の `vehicle-sil-gui-language`）があれば、それ。なければ、ブラウザの言語（`navigator.languages`）を好みの順に見て、主な部分（`ja-JP` なら `ja`）が対応する言語の最初のもの。どれも対応しなければ英語。
- 起動の順: `main.ts` が言語を決め、英語なら `locale/messages.en.json` を同じオリジンから読んで `loadTranslations` に渡し、`<html lang>` を合わせてから、`import()` で `start.ts`（アプリ）を読み込んで起動する。`$localize` は文を最初に評価したときに訳を決める（`run/labels.ts` の表のように、モジュールを読み込むときに評価するところがある）ので、アプリのモジュールを `main.ts` から静的に import しない。`LOCALE_ID` は選んだ言語（`Intl.ListFormat` などが従う）。日本語は元の言語なので、翻訳ファイルを読まない。
- 言語を変えると、アプリを読み込み直す（訳は起動の前に決まる）。実行はサーバーの中で続き、画面はつなぎ直して受け直す。サーバーにない画面の値（まだ送っていない選択）は、`LanguageChoice.keep` で渡して `take` で受け取る（実行の画面は、選んだファイル、外した participant、速度、ログの出どころ）。フォーカスは言語のメニューに戻す。
- 言語のメニューは、地球の絵と、いまの言語の名前（その言語自身の文字）を出す。名前には、どの言語でも「Language」を含める。メニューの項目は、その言語自身の文字で書き、`lang` を付ける。
- 訳がない文（翻訳ファイルにない、`target` が `null` か空、置き場所が元の文と合わない）は、元の日本語の文で出る。id や空の文にはならない。翻訳ファイルを読めなければ、画面全体を日本語で出す。
- テンプレートの文言には `i18n`（属性には `i18n-<属性>`）、TypeScript の文言には `$localize` を付ける。印のない文言は lint で止まる。id は抽出のときに作られるもの（元の文から決まる）を使う。
- 文を連結して作らない（語順は言語で違う）。値は 1 つの文の中に置き場所（テンプレートの `{{ }}`、`$localize` の `${値}:名前:`）として入れる。数で語の形が変わる文（「N 行」）は、テンプレートでは ICU の `plural` で書く（日本語は `other` だけ。英語の訳が `one` を足す）。TypeScript では、数に左右されない書き方にする（「誤り: N」）。
- 計測値は `measure` パイプ（`core/format/measure-pipe.ts`）で出す。桁区切りを入れず、小数の桁を決める。どの言語でも同じ書式で、locale の数値の書式に任せない。
- ECU やランチャーが出す英語の文（所見、ログ）は訳さず、そのまま出す（`lang="en"` を付ける）。
- 並べる語の区切りは `Intl.ListFormat`（`LOCALE_ID`）で作る。区切りや `:` を CSS の `content` で足さない（訳せない）。

翻訳ファイル `src/locale/messages.<言語>.json` は、抽出した文ごとに元の文と訳を並べた JSON です（読み比べやすく、差分で見やすい）。`target` が `null` の文は「まだ訳していない」印です。`note` は、そのファイルについての説明です。

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

`scripts/verify-web.py` の i18n の段は、文を抽出して各翻訳ファイルと比べます（`scripts/web_i18n.py`）。元の文が抽出と違うもの、空の訳、置き場所（`{$名前}`）や ICU の形が元の文と合わない訳があると失敗します。まだ訳していない文（ファイルにない文と、`target` が `null` の文）は失敗にせず、要約に数を出します。もう抽出されない古い文は、一覧に出します（失敗にはしない）。

文を足す、変える:

1. 文言に印を付ける（上の決まり）。
2. `scripts/verify-web.py --i18n-update` で翻訳ファイルを合わせる。新しい文は `"target": null` で入り、古い文は消える（消した文と訳を表示するので、言い回しを変えただけなら訳を使い回せる）。
3. 各ファイルの `null` を訳に書き換える。すぐに訳せなければ `null` のまま残してよい（画面には日本語で出て、i18n の段の要約に数が出る）。
4. `scripts/verify-web.py --i18n-only` で確かめる。

言語を足す:

1. `core/i18n/language.ts` の `LANGUAGES` に、コードとその言語自身の文字の名前を足す。
2. `src/locale/messages.<コード>.json` を `{"locale": "<コード>", "note": "…", "translations": {}}` で作り、`scripts/verify-web.py --i18n-update` で文を入れて訳す。
3. `en` 以外の言語なら、その locale のデータ（日付、複数形の規則）を `@angular/common/locales/<コード>` から `registerLocaleData` で登録する（日本語はビルドが、英語は Angular が持っている）。
4. `e2e/i18n.spec.ts` のように、その言語で画面を確かめる。

### サーバーとの間の型

- サーバーとの間でやりとりする形は、クライアントのライブラリの `gui/client/src/protocol.ts` にだけ、型と読み取り関数（`read*`）で書く（画面は `@vehicle-sil/gui-client` から import する）。形の正本は `gui/protocol/`（README と `examples/`）。届いた JSON は読み取り関数を通してから使う（ライブラリが HTTP の応答と WebSocket の通知を読む）。知らない欄、知らない通知と事象は無視して通す。
- セッションの要約とランチャーの事象は、部分ごとに読む。ランチャーから来る部分やサーバーが上限で縮めた部分が読めなくても、全体を捨てない。読めない部分は既定の値（`null`、空）にして、その名前を `unreadable` に入れ、帯に「一部を表示できません」と出す。全体を読めないとするのは、要約の芯（`id`、`state`、`launcher`、`stop`）か事象の芯（`seq`、`event`）が読めないときだけ（`hello` と `session` なら、つなぎ直して受け直す）。
- ライブラリの `protocol.spec.ts` が `gui/protocol/examples/` のすべての例を読ませる。例が変わったら、型と読み取り関数を合わせる。

### 試験の書き方

- 単体試験は TestBed。`Layout` は signal を持つ偽物に差し替える（jsdom に `matchMedia` がない）。
- `resource` は `TestBed.tick()` で要求を出させ、`HttpTestingController` で `flush` してから `whenStable()`（応答していない要求があると `whenStable()` が終わらない）。
- WebSocket は `FakeWebSockets`（`@vehicle-sil/gui-client/testing`）で差し替える（`{ provide: SOCKET_FACTORY, useValue: fake.factory }`）。HTTP は、包みが `HttpClient` で送るので `HttpTestingController` で答える。時間は Vitest の偽の時計。サーバーの通知は、作らずに `gui/protocol/examples/` の例を流す（`webSocketExamples`、`httpExamples`）。
- 接続、セッションの写し、時刻と実行の操作の決まりは、ライブラリの試験（`gui/client/src/*.spec.ts`。Angular なし）で確かめる。包みの試験は、signal に写ること、トークン、画面の言葉だけを見る。
- 「何かが起きないこと」を、決まった時間待ってから確かめない（遅いマシンで見落とす）。その時間が過ぎたとわかる後の出来事（後の事象、サーバーの要約の `last_seq` や `stop`）を待ってから確かめる。
- e2e は `getByRole` を先に使い、`data-testid` は補助。レイアウトは上の「レイアウト」。e2e は本物の GUI サーバーと偽のランチャーを相手にする（筋書きは構成ファイルで選ぶ）。偽のランチャーは本物と同じ事象を書く（`gui/server/tests/test_real_launcher.py` が比べる）。本物のランチャーでしか起きないこと（例の ECU の申告、本物の止まり方）は `e2e/real/` で確かめる。筋書きにない状況（切断、読んでいる間に流れ続ける行）は、`page.routeWebSocket` で通知を差し込んで作る。
- 見た目の確かめとスクリーンショットは、窓を伸ばさずに利用者が見る大きさのままで行う（最初の画面に何が入っているかを見るため）。

## アイコン

`public/favicon.svg` が元です。`public/favicon.ico`（16、32、48 px）は SVG から ImageMagick で作り、SVG を変えたら作り直します。

```sh
cd public && magick -background none -density 384 favicon.svg -define icon:auto-resize=48,32,16 favicon.ico
```

## 依存の導入スクリプト

`.npmrc` の `ignore-scripts=true` で、依存の導入スクリプト（`preinstall`、`install`、`postinstall`）を走らせません。いま入っている依存は、どれもビルド済みのバイナリを別のパッケージ（`optionalDependencies`）で持つので、スクリプトがなくても動きます。`npm run build` のように明示したスクリプトは走りますが、`prebuild` のような前後のスクリプトは走りません。前後のスクリプトを書かずに、1 つのスクリプトにまとめます。依存を足したら、導入スクリプトがなくても動くことを確かめます。
