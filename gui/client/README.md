# gui/client

GUI サーバー（`vehicle-sil-gui`）のクライアントのライブラリです（TypeScript）。接続（トークン、再接続）、セッションの写し（サーバーの要約と事象から作る実行の状態）、実行と時刻の操作、サーバーの口の型と読み取りを持ちます。Angular にも RxJS にも、ほかのパッケージにも依りません。私たちの画面（`gui/web`）も、Angular の薄い包み（`gui/web/src/app/core/session/`）を通して、これだけを使います。

サーバーの口そのもの（HTTP と WebSocket の形、版と互換の決まり、認証）は [gui/protocol](../protocol/README.md) にあります。例は [examples/gui_client](../../examples/gui_client/) の 2 つです: ライブラリを使わずに口を叩く `run.mjs` と、ライブラリを 1 つにまとめたファイルを素の JavaScript（Node）から使う `run-with-library.mjs`。

## 取り込み方

ライブラリは TypeScript のソースです。npm のパッケージとしてはまだ配っていません。

- **TypeScript のアプリ**: `tsconfig.json` の `paths` で、入口の名前を `src/` に向けます（`gui/web` がそうしています）。
- **素の JavaScript**: 1 つの ESM のファイルにまとめたものを使います。`gui/web` で `npm run build:client` を実行すると、`build/gui-client/vehicle-sil-gui-client.mjs`（リポジトリの直下の `build/`。追跡しない）ができます。ブラウザでも Node（22 以上）でも読めます。`scripts/verify-gui.py` が、このファイルを Node から使う例を、偽のランチャーを相手に動かして確かめます。

## 入口

| 入口                   | 名前                               | 中身                                                                                                                                                                                |
| ---------------------- | ---------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/index.ts`         | `@vehicle-sil/gui-client`          | 公開するもの（下の表）。互換を約束するのはここだけ                                                                                                                                  |
| `src/testing/index.ts` | `@vehicle-sil/gui-client/testing`  | 試験の部品: 偽の WebSocket（`FakeWebSockets`）、偽の HTTP（`FakeHttp`）、`gui/protocol/examples/` の読み込み、要約から `SessionView` を作る `sessionViewFrom`（自分の部品の試験に） |
| `src/internal.ts`      | `@vehicle-sil/gui-client/internal` | **互換の約束の外**。中の部品（クラス、要約を当てる関数、読み取りの部品）。試験のために出している。予告なく変わり、消える。アプリでは使わない                                        |

公開する口は狭く始めます（足すのは互換、消すのは互換でないため）。要るものが公開の入口にないときは、`internal` に頼らず、足すかを決めます。

## 公開するもの

| まとまり         | 公開するもの                                                                                                                                                                                                                                                                                                           |
| ---------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| クライアント     | `createClient(options)`、`ClientOptions`（`baseUrl`、`token`、`origin`、`http`、`socket`、`answerTimeoutMs`、`onError`）、`GuiClient`（`connection`、`session`、`run`、`time`、`app`、`limits`、`connect()`、`close()`、`loadApp()`、`configs()`、`check()`、`fetchSession()`）、`DEFAULT_LIMITS`                      |
| 接続（読むだけ） | `ClientConnection`（`state`、`failures`、`lastClose`、`tryAgainReason`、`onBatch`、`onUnreadable`）、`ConnectionState`、`CloseInfo`、`BATCH_MS`、`TRY_AGAIN_LATER`                                                                                                                                                     |
| セッションの写し | `ClientSession`（`session`、`ready`、`lastEventAt`、`notices`、`rejected`、`incompatible`、`onMessages`、`report`、`dismissNotice`）、`Notice`、`SessionView`、`ParticipantView`、`LauncherView`、`LogLine`、`LogSource`、`TimeView`、`CommandView`、`RunOutcome`、`runOutcome`、`phaseIndex`                          |
| 実行の操作       | `ClientRun`（`starting`、`stopping`、`stopWhileStarting`、`error`、`start`、`stop`、`forceStop`）、`StopAction`                                                                                                                                                                                                        |
| 時刻             | `ClientTime`（`state`、`answerTimeoutMs`、`pause`、`resume`、`step`、`setSpeed`、`send`）、`TimeState`、`TimeFailure`、`Lateness`、`SendResult`、`NotSentReason`、`ANSWER_TIMEOUT_MS`、`FEATURE_SPEED`、`FEATURE_START_PAUSED`、`FEATURE_TIME_COMMANDS`                                                                |
| 読める値         | `Readable<T>`（`get()`、`subscribe(fn)`）                                                                                                                                                                                                                                                                              |
| 差し替えの口     | `SocketFactory`、`SocketHandlers`、`SocketLike`、`webSocketFactory`、`HttpTransport`、`HttpRequest`、`HttpOutcome`、`fetchTransport`、`FetchTransportOptions`、`DEFAULT_TIMEOUT_MS`                                                                                                                                    |
| 誤り             | `HttpError`（`status`、`statusText`、`problem`、`timedOut`）、`errorMessage`、`ProtocolError`                                                                                                                                                                                                                          |
| 口の形と読み取り | `gui/protocol` の形の型（`SessionSummary`、`ServerMessage`、`LauncherEvent`、`CheckResult`、`RunResult`、`AppInfo` など）、`PROTOCOL_VERSION`、`PHASES`、`severityRank`。口を自分で読む人のための `readServerMessage`、`readAppInfo`、`readConfigList`、`readCheckResponse`、`readSessionResponse`、`readTimeResponse` |

## 使い方

```ts
import { createClient } from '@vehicle-sil/gui-client';

// サーバーが表示した URL: http://127.0.0.1:<port>/#token=<token>
const client = createClient({ baseUrl: 'http://127.0.0.1:8765/', token: '<token>' });
await client.loadApp(); // ランチャーの features（時刻の命令を持つか、など）
client.connect();
const stop = client.session.session.subscribe((session) => {
  if (session?.result) console.log(session.result.result);
});
await client.run.start({ file: 'simulation.yaml', disable: [], speed: null, start_paused: false });
```

- 状態はどれも読める値（`Readable`）です。`get()` でいまの値、`subscribe(fn)` で後の変化を受けます（いまの値では呼びません）。返す関数で購読をやめます。知らせは同期で、値を変えた処理の中で届きます。知らせの途中で購読をやめても、その回の知らせは届きます。購読の中で同じ値を変えると、新しい値が全員に届き、古い値は後から届きません。枠組みの包みは、`get()` で最初の値を取り、`subscribe()` で写します（Angular は `readableSignal`）。
- 購読者（`subscribe`、`onBatch`、`onMessages`、`onUnreadable` に渡した関数）が投げても、ほかの購読者に届き、ライブラリの処理（断られたら切る、作り直す、など）も進みます。投げたものは `onError`（既定は `console.error`）に渡ります。
- セッションの写しが揃ったか（つながっていて、`hello` と再送を受け終えた）は `client.session.ready` です。機能ごとに持ちます。
- 断られた操作は、Promise が `HttpError` で reject し、同じ理由が状態（`client.run.error`、`client.time.state` の `failure`）にも出ます。応答を待つのは `DEFAULT_TIMEOUT_MS` までです。開始（`run.start()`）と照合（`check()`）は、サーバーが照合を 1 つずつ行い、順番待ちと照合にそれぞれ照合の上限（`limits.check_timeout_s`）までかけ、開始の中でも変わった構成ファイルを照合し直すので、その 2 倍に `DEFAULT_TIMEOUT_MS` を足した時間まで待ちます。過ぎたら `timedOut` が真の `HttpError`（状態 0）です。これは断られたのではなく、答えが来なかっただけです: 開始なら、サーバーは実行を始めたかもしれず、始まったかはセッションの写し（WebSocket）でわかります（`client.run.error` もそう言います）。転送（リダイレクト）には従いません。
- 読み取りは部分ごとです。要約と事象の一部が読めなくても、読める部分を使い、読めない部分の名前を `SessionView.unreadable` に入れます。知らない通知と事象は状態に当てずに通します（`client.connection.onBatch` には、読む前のものがすべて届きます）。JSON として読めない通知は、知らせを出して `hello` から受け直します。
- Node では、`Origin` をサーバー自身の origin にして送ります（`createClient` が `baseUrl` から作ります）。ブラウザと Web Worker では、ブラウザが付けるので `origin` を渡しません（Worker の WebSocket は握手のヘッダーを付けられません）。HTTP の道具を変えるときは `http`（`HttpTransport`）を、WebSocket を変えるときは `socket`（`SocketFactory`）を渡します。

## 寿命

| 呼び出し                             | すること                                                                                                          |
| ------------------------------------ | ----------------------------------------------------------------------------------------------------------------- |
| `createClient()`                     | 作るだけ。つながない                                                                                              |
| `connect()`                          | WebSocket でつなぐ。トークンがないときは何もしない                                                                |
| つながっている間の `connect()`       | 何もしない（つなぎ直しを待っている間も）                                                                          |
| `close()`                            | 切る。つなぎ直さない。待っている時計を止め、時刻の命令の答えを待つのをやめる。まとめている途中の通知は捨てる      |
| `close()` の後の `connect()`         | またつなぐ。セッションの写しは残り、持っている分の続きから受け直す                                                |
| `close()` の後の HTTP の要求         | 使える（`loadApp()`、`run.start()` などは、つないでいなくても送れる）                                             |
| 断られた・版が違った後の `connect()` | 何もしない（`session.rejected`、`session.incompatible` は戻らない）。別のトークンや版では、クライアントを作り直す |

## 書き方の決まり

- 自分のファイルのほかは import しない（`eslint.config.js` が止める。動的な `import()` も、試験の部品のほかでは止める。`tsconfig.json` の `rootDir` で、`src/` の外も止まる）。ブラウザにしかないもの（`window`、`document`、`localStorage` など）を使わない。要るものは設定（`ClientOptions`）で受け取る。
- 人に見せる文（`TimeFailure.message` など）は英語。画面の言葉にするのは包みの役目（`kind` などの機械が読む欄から作る）。
- 公開するものを足したら `src/index.ts` と上の表に足し、[docs/gui.md](../../docs/gui.md) の「自分の画面や道具を作る」を合わせる。

## 試験

道具（TypeScript、ESLint、Prettier、Vitest、esbuild）は `gui/web` の `node_modules` から借ります。`gui/web` で次を実行します（`scripts/verify-web.py` も回します）。

| コマンド               | 内容                                                                                         |
| ---------------------- | -------------------------------------------------------------------------------------------- |
| `npm run test:client`  | 型の検査（`tsc -p ../client`）と単体試験（Vitest。Node の上で、Angular も DOM もなしに動く） |
| `npm run build:client` | 1 つの ESM のファイルにまとめる（esbuild。Angular のビルドの道具が持っているもの）           |
| `npm run lint`         | `gui/web` と、この `eslint.config.js`                                                        |
| `npm run format:check` | `gui/web` と、このディレクトリの書式（`gui/.prettierrc`）                                    |

単体試験は、サーバーの通知を作らずに `gui/protocol/examples/` の例を流します（`testing/protocol-examples.ts`）。
