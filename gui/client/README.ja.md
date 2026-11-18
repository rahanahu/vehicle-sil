# gui/client

[English](README.md)

GUI サーバー（`vehicle-sil-gui`）のクライアントのライブラリ（TypeScript、`@vehicle-sil/gui-client`）のリファレンスです。入手とインポートの方法、公開 API の一覧、使い方、ECU の変数の読み書き、ライフサイクルが書いてあります。

ライブラリが持つのは、接続（トークン、再接続）、セッションのコピー（サーバーの要約とイベントから組み立てた、実行の状態のコピー）、実行と時間の操作、ECU の変数（一覧、値の watch、書き込みと結果）、サーバーの API の型と読み取りです。Angular にも RxJS にも、ほかのパッケージにも依存しません。サンプルの画面（`gui/web`）も、Angular の薄いラッパー（`gui/web/src/app/core/session/`）を通して、これだけを使います。

| 知りたいこと                                                | 節                                                               |
| ----------------------------------------------------------- | ---------------------------------------------------------------- |
| ライブラリをどこから入手し、どう import するか              | [入手とインポート](#入手とインポート)                            |
| どの名前が公開されているか                                  | [エントリーポイント](#エントリーポイント)、[公開 API](#公開-api) |
| 接続して実行を始めるまでのコード                            | [基本の使い方](#基本の使い方)                                    |
| 変数の値を読む、書き込む                                    | [ECU の変数](#ecu-の変数)                                        |
| 画面が見せるものと、押せるボタン: `connect()`               | [正面の口（`connect()`）](#正面の口connect)                      |
| HTML に並べる要素（スクリプトなし）                         | [部品（custom element）](#部品custom-element)                    |
| スライダーやランプを作る（`condition`、`block` の値の一覧） | [コンポーネントを作る（`bind`）](#コンポーネントを作るbind)      |
| `connect()` と `close()` の後に何が起きるか                 | [ライフサイクル](#ライフサイクル)                                |
| ライブラリを開発する                                        | [コーディングのルール](#コーディングのルール)、[テスト](#テスト) |

## 入手とインポート

ライブラリは TypeScript のソースで、依存はありません。npm のレジストリには公開していません。

| 使い方                                                | 入手                                                                                                              | import の書き方                                              |
| ----------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------ |
| GUI サーバーが配信する画面（`--web`）                 | コピーするものはありません。サーバーがファイルを配信します（下）                                                  | `import { connect } from '/vsil/vehicle-sil-gui-client.mjs'` |
| 上の画面の外の素の JavaScript                         | 1 つの ESM のファイルにまとめた `vehicle-sil-gui-client.mjs` を、スクリプトの隣に置く                             | `import { connect } from './vehicle-sil-gui-client.mjs'`     |
| TypeScript と好きなフレームワーク                     | パッケージ `vehicle-sil-gui-client-<version>.tgz` を入手し、自分のプロジェクトで `npm install <that .tgz>` をする | `import { connect } from '@vehicle-sil/gui-client'`          |
| このリポジトリの中の TypeScript のアプリ（`gui/web`） | `tsconfig.json` の `paths` で、エントリーポイントの名前を `src/` に向ける                                         | `import { createClient } from '@vehicle-sil/gui-client'`     |

3 つのファイルは、次の場所にあります。

| ファイル                               | ビルド済みのツール                                    | リポジトリでビルドする（`gui/web` で実行）                                              |
| -------------------------------------- | ----------------------------------------------------- | --------------------------------------------------------------------------------------- |
| `vehicle-sil-gui-client.mjs`           | `<tools>/client/vehicle-sil-gui-client.mjs`           | `npm run build:client` が `build/gui-client/vehicle-sil-gui-client.mjs` を作る          |
| `vehicle-sil-gui-elements.mjs`         | `<tools>/client/vehicle-sil-gui-elements.mjs`         | `npm run build:client` が `build/gui-client/vehicle-sil-gui-elements.mjs` を作る        |
| `vehicle-sil-gui-client-<version>.tgz` | `<tools>/client/vehicle-sil-gui-client-<version>.tgz` | `npm run pack:client` が `build/gui-client/vehicle-sil-gui-client-<version>.tgz` を作る |

同じ `.tgz` は、リリースのページにもあります。`build/` はリポジトリの直下のもので、Git では追跡しません。

GUI サーバーは、2 つの `.mjs` のファイルも、トークンなしで `/vsil/vehicle-sil-gui-client.mjs` と `/vsil/vehicle-sil-gui-elements.mjs` として配ります（リポジトリでは `build/gui-client/` から。ビルドしていないときは、実行するコマンドを言います）。ビルドなしの画面は、ここから読めば、何も写さずに済みます。

- **1 つにまとめたファイル**: ブラウザでも Node（22 以上）でも読めます。`scripts/verify-gui.py` が、このファイルを Node から使う例を、記録を再生するランチャーを相手に動かして確認します。
- **パッケージ**: 中身は、1 つにまとめた ESM、`.d.ts`、`exports`、README、LICENSE です（作り方は `pack.mjs`）。バージョンは GUI サーバーと同じです。型はブラウザの型（`lib` の `DOM`）を使います。エントリーポイントは `.`、`./elements`、`./timeline` です（`testing` と `internal` は入りません）。

## エントリーポイント

| エントリーポイント                                                                                               | 名前                               | 中身                                                                                                                                                                                               |
| ---------------------------------------------------------------------------------------------------------------- | ---------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/index.ts`                                                                                                   | `@vehicle-sil/gui-client`          | 公開 API（下の「公開 API」）。互換を保証するのはここと `./elements`                                                                                                                                |
| `src/testing/index.ts`                                                                                           | `@vehicle-sil/gui-client/testing`  | テスト用のヘルパー（自分のコンポーネントのテストに使える）                                                                                                                                         |
| `src/internal.ts`                                                                                                | `@vehicle-sil/gui-client/internal` | **互換の保証の外**。内部のクラスと関数で、アプリでは使わない                                                                                                                                       |
| `src/elements/index.ts`（パッケージでは `dist/index.js` を import する `dist/elements.js`）                      | `@vehicle-sil/gui-client/elements` | 部品（custom element の `<vsil-status>` ほか 10 個）、`pageIntent`、`setReasonText`。import すると定義します（下の「部品（custom element）」）。DOM を要します。本体の入口はこれを import しません |
| `../timeline/vehicle-sil-timeline.js`（パッケージでは `export {}` を足した `timeline/vehicle-sil-timeline.mjs`） | `@vehicle-sil/gui-client/timeline` | 要素 `<vehicle-sil-timeline>` と型。import すると要素を定義します（[docs/ja/gui.md](../../docs/ja/gui.md#自分の画面に時間の軸を置く) の「自分の画面に時間の軸を置く」）                            |

- `testing` にあるのは、偽の WebSocket（`FakeWebSockets`）、偽の HTTP（`FakeHttp`）、`gui/protocol/examples/` の読み込み、要約から `SessionView` を作る `sessionViewFrom` です。
- `internal` にあるのは、内部のクラス、要約を反映する関数、読み取りの関数です。テストのために公開していて、予告なく変わり、消えます。

公開 API は狭く始めます（追加は互換を保ち、削除は互換を壊すため）。必要なものが公開のエントリーポイントにないときは、`internal` に頼らず、追加するかを決めます。

## 公開 API

`src/index.ts` が公開する名前を、グループごとに挙げます。括弧の中は、そのオブジェクトのメンバーです。

### クライアント

| 種類 | 名前                                                                                                                                                                                                                                     |
| ---- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 関数 | `createClient(options)`                                                                                                                                                                                                                  |
| 型   | `ClientOptions`（`baseUrl`、`token`、`origin`、`http`、`socket`、`answerTimeoutMs`、`onError`）                                                                                                                                          |
| 型   | `GuiClient`（`connection`、`session`、`run`、`time`、`variables`、`app`、`limits`、`connect()`、`close()`、`loadApp()`、`configs()`、`check()`、`connections()`、`fetchSession()`、`dashboard(file)`、`scenarios(file)`、`recording()`） |
| 定数 | `DEFAULT_LIMITS`                                                                                                                                                                                                                         |

### 正面の口

| 種類   | 名前                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 関数   | `connect(options?)`、`createFront(options)`、`watch(source, fn)`、`until(source, test, options?)`                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| クラス | `ActionError`（`code`、`message`、`outcome`、`findings`）                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| 型     | `ConnectOptions`（`base_url`、`token`、`origin`、`http`、`socket`、`answer_timeout_ms`、`on_error`）、`Front`（`low`、`status`、`clock`、`controls`、`scenario`、`outside`、`run`、`time`、`variable()`、`connections()`、`check()`、`close()`）、`FrontStatus`、`FrontResult`、`FrontClock`、`FrontControls`、`Control`、`Reason`、`FrontScenario`、`FrontRun`、`FrontTime`、`FrontVariable`、`VariableView`、`Intent`、`Outside`、`ConnectEntry`、`ConnectTarget`、`ConnectListError`、`Link`、`RunState`、`EndKind`、`Pacing`、`OperationName` |

### トークン

| 種類 | 名前                |
| ---- | ------------------- |
| 関数 | `takeToken(place)`  |
| 型   | `TokenPlace`        |
| 定数 | `TOKEN_STORAGE_KEY` |

ブラウザでは `takeToken(window)` と呼びます。URL の `#token=` を読んで URL から消し、sessionStorage に置きます。URL になければ sessionStorage に置いたものを返し、どちらにもなければ `null` を返します。

### 接続（読むだけ）

| 種類 | 名前                                                                                                |
| ---- | --------------------------------------------------------------------------------------------------- |
| 型   | `ClientConnection`（`state`、`failures`、`lastClose`、`tryAgainReason`、`onBatch`、`onUnreadable`） |
| 型   | `ConnectionState`、`CloseInfo`                                                                      |
| 定数 | `BATCH_MS`、`TRY_AGAIN_LATER`                                                                       |

### セッションのコピー

| 種類 | 名前                                                                                                                                            |
| ---- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| 型   | `ClientSession`（`session`、`ready`、`allowed`、`lastEventAt`、`notices`、`rejected`、`incompatible`、`onMessages`、`report`、`dismissNotice`） |
| 型   | `Notice`、`SessionView`、`ParticipantView`、`LauncherView`、`LogLine`、`LogSource`、`TimeView`、`CommandView`、`RunOutcome`、`LauncherAnswer`   |
| 関数 | `runOutcome`、`phaseIndex`、`isHolding`、`launcherAnswer`                                                                                       |

`ClientSession.allowed` は、GUI サーバーがいま受けるものです（`start`、`stop`、`force`。それぞれ `ok`、理由のコード `reason`、文 `message`。`rev` の大きい方が勝ちます。サーバーが言うまでは `null`）。`SessionView.launcherState` は、ランチャーの最後の `state` イベントです（実行の状態と、いま受けるもの。`rev` の大きい方が勝ちます。届くまでは `null`）。`launcherAnswer(launcherState, name)` は、1 つの操作（`write`、`pause` など）について、それが言うことを読みます: `reason`（受けるなら `null`）と `caution`。まだ言っていないランチャー（`state` がまだない、その操作の項目がない）は、受けないものとします（`unsupported`）。正面の口の `controls` と `variables.bind` は、これでランチャーを読みます。

### 実行の操作

| 種類 | 名前                                                                                              |
| ---- | ------------------------------------------------------------------------------------------------- |
| 型   | `ClientRun`（`starting`、`stopping`、`stopWhileStarting`、`error`、`start`、`stop`、`forceStop`） |
| 型   | `StopAction`                                                                                      |

### 時間

| 種類 | 名前                                                                                      |
| ---- | ----------------------------------------------------------------------------------------- |
| 型   | `ClientTime`（`state`、`answerTimeoutMs`、`pause`、`resume`、`step`、`setSpeed`、`send`） |
| 型   | `TimeState`、`TimeFailure`、`Lateness`、`SendResult`、`NotSentReason`                     |
| 定数 | `ANSWER_TIMEOUT_MS`                                                                       |

### ECU の変数

| 種類   | 名前                                                                                                                                                                  |
| ------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 型     | `ClientVariables`（`state`、`operations`、`watchProblem`、`watch`、`write`、`bind`）                                                                                  |
| 型     | `VariableWatch`（`values`、`value`、`history`、`close`）                                                                                                              |
| 型     | `VariablesState`、`VariablesStatus`、`VariablePeer`、`VariableInfo`、`VariableRef`、`VariableValue`、`WatchOptions`、`WatchValues`、`VariableReading`、`HistoryPoint` |
| 型     | `WriteOptions`、`OperationView`、`OperationPeer`、`WatchProblem`                                                                                                      |
| クラス | `WriteError`                                                                                                                                                          |
| 定数   | `WATCH_PERIOD_MS`、`WATCH_PERIODS_MS`、`WRITE_TIMEOUT_MS`、`HISTORY_MAX`、`HISTORY_TOTAL_MAX`、`OPERATIONS_KEPT`                                                      |

### 変数とコンポーネントの接続（`bind`）

| 種類     | 名前                                                                                                                      |
| -------- | ------------------------------------------------------------------------------------------------------------------------- |
| メソッド | `client.variables.bind(ref, options)`                                                                                     |
| 型       | `BindOptions`（`periodMs`、`history`、`needsRange`、`needsValue`、`visible`）                                             |
| 型       | `VariableBinding`（`ref`、`state`、`history()`、`write(value)`、`reject(message)`、`setVisible(visible)`、`close()`）     |
| 型       | `BoundVariable`（`ref`、`info`、`reading`、`condition`、`block`、`launcherReason`（`Reason`）、`writeStatus`、`pending`） |
| 型       | `ValueCondition`、`WriteBlock`、`WriteStatus`、`WriteValue`                                                               |
| 関数     | `isWriteError`、`hasRange`                                                                                                |
| 関数     | `valueCondition`（`ConditionInput`）、`writeBlock`（`WriteBlockInput`）                                                   |
| 定数     | `DROPPED_MESSAGE`                                                                                                         |

`valueCondition`、`writeBlock`、`hasRange` は、`bind` が組み立てに使っている純粋な関数です。

### `Readable`（読み取り専用の値）

| 種類 | 名前                                      |
| ---- | ----------------------------------------- |
| 型   | `Readable<T>`（`get()`、`subscribe(fn)`） |

### 差し替え用の API

| 種類 | 名前                                                                   |
| ---- | ---------------------------------------------------------------------- |
| 型   | `SocketFactory`、`SocketHandlers`、`SocketLike`                        |
| 型   | `HttpTransport`、`HttpRequest`、`HttpOutcome`、`FetchTransportOptions` |
| 関数 | `webSocketFactory`、`fetchTransport`                                   |
| 定数 | `DEFAULT_TIMEOUT_MS`                                                   |

### エラー

| 種類   | 名前                                                         |
| ------ | ------------------------------------------------------------ |
| クラス | `HttpError`（`status`、`statusText`、`problem`、`timedOut`） |
| クラス | `ProtocolError`                                              |
| 関数   | `errorMessage`                                               |

### サーバーの API の型と読み取り

`gui/protocol` が決める形の型と、サーバーの API を自分で読むときに使う読み取り関数です。

| 対象                                                       | 名前                                                                                                                                                                                                                                               |
| ---------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `gui/protocol` の形の型                                    | `SessionSummary`、`ServerMessage`、`LauncherEvent`、`CheckResult`、`RunResult`、`AppInfo` など                                                                                                                                                     |
| 定数と関数                                                 | `PROTOCOL_VERSION`、`PHASES`、`severityRank`                                                                                                                                                                                                       |
| 読み取り関数                                               | `readServerMessage`、`readAppInfo`、`readConfigList`、`readCheckResponse`、`readConnections`、`readSessionResponse`、`readTimeResponse`、`readWriteResponse`、`readVariablesSummary`、`readDashboardResponse`、`readScenarioList`、`readRecording` |
| 変数の API の形                                            | `VariablesSummary`、`VariableSummary`、`ValuesMessage`、`OperationReport`、`WatchMessage`、`WriteAnswer`                                                                                                                                           |
| ダッシュボードのファイル（`dashboard(file)`）              | `DashboardResponse`、`DashboardFile`、`DashboardFileSection`、`DashboardFileItem`、`DashboardFileError`、`DashboardPanelModule`、`DashboardVariable`                                                                                               |
| シナリオ（`--scenario`、`scenarios(file)`、`recording()`） | `ScenarioStep`、`ScenarioStepStatus`、`ScenarioStatus`、`CheckScenario`、`ScenarioResult`、`SummaryScenario`、`ScenarioList`、`ScenarioFile`、`Recording`                                                                                          |
| リクエストの形                                             | `CheckRequest`、`RunRequest`、`ConnectionsRequest`、`Speed`、`StopRequest`、`TimeCommand`                                                                                                                                                          |
| 設定ファイルの一覧                                         | `ConfigList`、`ConfigFile`                                                                                                                                                                                                                         |
| エラーの形                                                 | `ServerProblem`                                                                                                                                                                                                                                    |
| 指摘                                                       | `Finding`、`Severity`                                                                                                                                                                                                                              |
| ランチャーのキーと通信量                                   | `FromLauncher`、`LauncherFields`、`RunTraffic`、`TrafficNetwork`、`TrafficParticipant`、`TrafficUnplaced`、`ParticipantRole`、`OutsideState`                                                                                                       |
| セッションのコピーの `SessionView.scenario`                | `ScenarioView`                                                                                                                                                                                                                                     |

## 基本の使い方

ここで使うのは、ライブラリの低い口の `createClient` です。正面の口はこの上に作ってあり、`client.low` がこれです。画面は `connect()` と部品から始め（下の「正面の口（`connect()`）」と「部品（custom element）」）、それらにないもの（事象、ログ、指摘、参加者、`bind`）にこの口を使います。

```ts
import { createClient, takeToken } from '@vehicle-sil/gui-client';

// サーバーが表示した URL（http://127.0.0.1:<port>/#token=<token>）から開いた画面: URL の #token= を受け取って消す。
// Node のツールでは、baseUrl とトークンを、その URL から自分で取り出して渡す（examples/gui_client）。
const client = createClient({
  baseUrl: new URL('/', location.href).href,
  token: takeToken(window),
});
await client.loadApp(); // サーバーの情報（起動時のファイル、範囲、変数を使えるか）
client.connect();
const stop = client.session.session.subscribe((session) => {
  if (session?.result) console.log(session.result.result);
});
await client.run.start({ file: 'simulation.yaml', disable: [] });
```

### 状態を読む（`Readable`）

状態はどれも `Readable`（読み取り専用の値）です。

| 呼び出し        | すること                                           |
| --------------- | -------------------------------------------------- |
| `get()`         | いまの値を返す                                     |
| `subscribe(fn)` | 後の変化を `fn` に通知する（いまの値では呼ばない） |
| 返された関数    | 購読をやめる                                       |

- 通知は同期で、値を変えた処理の中で届きます。
- 通知の途中で購読をやめても、その回の通知は届きます。
- 購読の中で同じ値を変えると、新しい値が全員に届き、古い値は後から届きません。
- フレームワークのラッパーは、`get()` で最初の値を取り、`subscribe()` で自分の状態に変換します（Angular は `readableSignal`）。
- 購読者（`subscribe`、`onBatch`、`onMessages`、`onUnreadable` に渡した関数）が例外を投げても、ほかの購読者には届き、ライブラリの処理（拒否されたら切断する、作り直す、など）も進みます。投げたものは `onError`（既定は `console.error`）に渡ります。
- セッションのコピーが揃ったか（接続していて、`hello` と再送を受け終えた）は `client.session.ready` です。機能ごとに持ちます。

### 拒否とタイムアウト

- トークンを拒否されたら（サーバーを起動し直した、など）、`client.session.rejected` が真になり（WebSocket）、HTTP のリクエストは状態 401 の `HttpError` で拒否されます。
- 拒否された操作は、Promise が `HttpError` で reject し、同じ理由が状態（`client.run.error`、`client.time.state` の `failure`）にも出ます。
- レスポンスを待つのは `DEFAULT_TIMEOUT_MS` までです。過ぎたら `timedOut` が真の `HttpError`（状態 0）です。
- 開始（`run.start()`）と照合（`check()`）は、照合の上限（`limits.check_timeout_s`）の 2 倍に `DEFAULT_TIMEOUT_MS` を加えた時間まで待ちます。サーバーが照合を 1 つずつ行い、順番待ちと照合にそれぞれ照合の上限までかけ、開始の中でも変わった設定ファイルを照合し直すためです。
- タイムアウトは、拒否されたのではなく、レスポンスが来なかっただけです。開始なら、サーバーは実行を始めたかもしれず、始まったかはセッションのコピー（WebSocket）でわかります（`client.run.error` もそう言います）。
- 転送（リダイレクト）には従いません。

### ランチャーのキー（`raw`）

チェックの結果、実行の結果、指摘、ランチャーのイベントはランチャーが書いたもので、ライブラリはそのキーをすべて渡します。型のあるフィールド（例: 実行の結果の `traffic`、`participants[].log`、チェックの参加者の `role`）は、このライブラリの版ではなく、ランチャーの形式の版（`vehicle_sil_check`、`vehicle_sil_run`、`vehicle_sil_events`）に従います。ランチャーから読んだオブジェクトには、どれにも `raw` があります。届いたままのランチャーのオブジェクトで、型にないキーもここにあります。`raw` は読み取り専用で、列挙されません（`JSON.stringify` とスプレッドには入りません）。値は型を確かめていない JSON です。型を確かめてから使い、HTML としてではなく文字列として出してください。

```ts
const result = client.session.session.get()?.result; // RunResult | null
for (const network of result?.traffic?.networks ?? []) console.log(network.name, network.delivered);
const strict = result?.raw?.['strict']; // 型のないキー: unknown
```

### 読めない通知の扱い

読み取りは部分ごとです。

- 要約とイベントの一部が読めなくても、読める部分を使い、読めない部分の名前を `SessionView.unreadable` に入れます。
- 知らない通知とイベントは、状態に反映せずに通し、`onMessages` には渡しません（`client.connection.onBatch` には、読む前のものがすべて届きます。ライブラリの知らないイベントは、ここで読みます）。
- JSON として読めない通知は、通知を出して `hello` から受け直します。

### Node から使う、通信を差し替える

- Node では、`Origin` をサーバー自身の origin にして送ります（`createClient` が `baseUrl` から作ります）。
- ブラウザと Web Worker では、ブラウザが付けるので `origin` を渡しません（Worker の WebSocket はハンドシェイクのヘッダーを付けられません）。
- HTTP の実装を変えるときは `http`（`HttpTransport`）を、WebSocket を変えるときは `socket`（`SocketFactory`）を渡します。

ライブラリが肩代わりしないルール（照合と開始のリクエストのフィールド、変数を書けるか、値が古いか）は、[docs/gui.md](../../docs/ja/gui.md) の「画面を作るときのルール」にあります。

## 正面の口（`connect()`）

`connect()` は、ライブラリの正面の口を返します。画面が見せるもの（`status`、`clock`、`controls`、`scenario`）、Promise で返す操作（`run`、`time`）、変数 1 つ（`variable()`）です。ここまでに説明したクライアントの上に作ってあり、そちらは何も変えません。`front.low` は同じ `GuiClient`（イベント、ログ、所見、参加者）で、`createClient` もこれまでどおり使えます。画面が決めなければならないこと（「いまはどの状態か」「このボタンはいま押せるか、押せないなら理由は何か」）には正面の口を使い、それ以外には `low` を使います。

サーバーが答える前から、また届かない間も画面を出す必要のあるアプリ（標準の画面）は、代わりに `createFront(options)` を使います。`createClient` と同じく、接続せずに正面の口を作るだけで、アプリが自分で `front.low.connect()` と `front.low.loadApp()` を呼びます。`loadApp()` が答えるまで、正面の口は起動時のファイルを知らず、`controls` の範囲は既定の値（`DEFAULT_LIMITS`）です。

```ts
import { ActionError, connect, until, watch } from '@vehicle-sil/gui-client';

// GUI サーバーが配信し、サーバーが表示した URL から開いたページ: 引数なし。最初の呼び出しがつなぎ、次からの呼び出し
// （ページの別の部品）は同じ接続を受け取る。Node か 2 本目の接続では、サーバーの URL とトークンを渡す。
const client = await connect();
// const client = await connect({ base_url: 'http://127.0.0.1:8765/', token });

const stop = watch([client.status, client.controls], (status, controls) => {
  startButton.disabled = !controls.start.ok;
  startButton.title = controls.start.reason?.message ?? '';
  banner.textContent = status.problem?.message ?? '';
});

try {
  await client.time.step(100); // 時刻が着いて、実行がまた一時停止したときに解決する
} catch (error) {
  if (error instanceof ActionError) console.log(error.code, error.message, error.outcome);
}
```

| 名前                                                               | 内容                                                                                                                                                                                                                                                           |
| ------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `status`                                                           | 見せる状態 1 つ: `link`、`run`、`file`、`result`、`problem`（下）。離散の値だけです                                                                                                                                                                            |
| `clock`                                                            | `sim_ms`、`speed`、`actual_speed`、`pacing`（`not-paced`、`on-time`、`late`）。よく変わるので `status` と分けています                                                                                                                                          |
| `controls`                                                         | 操作ごとの、いま使えるか: `start`、`stop`、`force_stop`、`pause`、`resume`、`step`（`min_ms` と `max_ms` つき）、`run_until`、`speed`（`min`、`max`、`fastest`（数のほかに `'max'` を受けるか）つき）                                                          |
| `scenario`                                                         | いまの（か最後の）実行のシナリオ: `file`、`name`、`status`、`steps`、`timeline_url`。シナリオのない実行では `null`                                                                                                                                             |
| `outside`                                                          | 実行の `connect` が実機のバスにつないだネットワーク（下の「ネットワークを外につなぐ」）                                                                                                                                                                        |
| `connections({ network }, { file }?)`、`check(intent?, { file }?)` | ネットワークをつなげる先と、始めずに意図を確かめた結果（下の「ネットワークを外につなぐ」）                                                                                                                                                                     |
| `run`                                                              | `start(intent?, { file }?)`、`stop()`、`forceStop()`                                                                                                                                                                                                           |
| `time`                                                             | `pause()`、`resume()`、`step(ms)`、`setSpeed(value)`、`runUntil(ms)`                                                                                                                                                                                           |
| `variable(ref)`                                                    | 変数 1 つ。`"ECU.変数"`（最初の点で切る）か `{ peer, name }`（名前に点を含む ECU）で指します                                                                                                                                                                   |
| `low`、`close()`                                                   | 同じ接続の `GuiClient`。`close()` は切ります（共有している接続は、次の `connect()` がつなぎ直します）                                                                                                                                                          |
| `watch(source, fn)`、`until(source, test, options)`                | `watch` は、いますぐ `fn` を呼び、変わるたびに呼びます（複数の元は `watch([a, b], (a, b) => ...)`）。戻り値で止めます。`until` は、`test` を満たす最初の値で解決します。時間切れと中止は `ActionError`（`timeout`、`aborted`。どちらも `unknown`）で拒否します |

データの欄は wire の名前（`force_stop`、`sim_ms`）、関数は JavaScript の慣習（`forceStop`、`runUntil`）です。状態はどれも `Readable` です。変わっていなければ `get()` は同じオブジェクトを返し、`get` と `subscribe` は `this` なしで動きます（`useSyncExternalStore` にそのまま渡せます）。

### `status`

| 欄        | 値                                                                                                                                                                                                                                                                                                             |
| --------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `link`    | `connecting`、`online`、`offline`、`rejected`（トークンが断られた）、`incompatible`（プロトコルの版が違う）。`online` はコピーがサーバーと揃っていることです                                                                                                                                                   |
| `run`     | `none`、`starting`（開始が向かっている。このページのでも別のページのでも）、`waiting`（参加者がそろうのを待っている）、`running`、`paused`、`stopping`、`ended`（結果がある）、`crashed`（ランチャーが結果なしで終わった）                                                                                     |
| `file`    | 実行の構成ファイル（走っていなければ最後のもの）。最初の実行の前は、サーバーの起動時のファイル                                                                                                                                                                                                                 |
| `result`  | `run` が `ended` か `crashed` の間: `end`（`completed`、`stopped`、`interrupted`、`failed`、`setup_failed`、`crashed`。何も起動しなかったとき（チェックの失敗、ファイルや引数の誤り）は `null`）、`scenario`、`ok`、`message`（英語の 1 文）。操作者の停止は `ok` です（シナリオが終わる前に止めたときを除く） |
| `problem` | いま利用者に知らせる問題 1 件（`{ code, message }`）か `null`                                                                                                                                                                                                                                                  |

`problem` は次の順で選びます。まず接続（トークンの拒否、プロトコルの版の違い、切断）。なければ、ランチャーの異常終了、直前の操作の失敗、失敗した実行（`result.ok` が偽）のうち、新しいものです。異常終了と失敗した実行は、次の実行が始まると消えます。操作の失敗は、次の操作か、`status.run` が変わったときに消えます。操作者の停止は問題にしません。ただし、シナリオが終わる前に止めたときは問題にします（`result.ok` が偽で、文はシナリオの判定がないことを伝えます）。変数の書き込みの失敗は、ここでなく、その変数の `problem` に出ます。サーバーの知らせ（`low.session.session` の `problems`）と `notices` は `low` のままです。

`result.end`、`result.ok`、`result.message` はランチャーのものです。ランチャーが結果に書き（`end`、`ok`、`message`。[simulation-run.md](../../docs/ja/simulation-run.md#実行の終わり方endokmessage) の「実行の終わり方」）、ライブラリはそのまま渡します。ライブラリは結果の名前の表を持ちません。`crashed` だけはライブラリのものです（`ok` は偽で、文もライブラリが作ります）。結果を書かずに終わったランチャーは、何も書いていないためです。

### `controls`

各項目は `{ ok, reason, caution }` です。`ok` が偽のとき `reason` は `{ code, message }`、`caution` は、受けるが先に知っておくことがある操作に付く同じ形です。コードは変わりません（増えることはありますが、意味は変わりません）。文は英語で、規則の持ち主が書いたものです。

| 項目                                            | 出どころ                                                                                                                                                                                                                                                                                                                                                                       |
| ----------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `start`                                         | GUI サーバーの `allowed.start`（`starting`、`already-running`、`launcher-unusable`、`shutting-down`）とその文                                                                                                                                                                                                                                                                  |
| `stop`、`force_stop`                            | GUI サーバーの `allowed.stop` と `allowed.force`。`stop` は、最後に知った実行（か、向かっている開始）があれば、切断中でも、結果の後もランチャーが動いているとサーバーが言う間も `ok`。`force_stop` は、1 回目の停止の後だけ `ok`（それまでは `not-stopping`）                                                                                                                  |
| `pause`、`resume`、`step`、`speed`、`run_until` | ランチャーの `state.allowed`（理由、文、`caution` はランチャーが書いたまま。`step` と `speed` の範囲も。`fastest` は、ランチャーの約束のとおり、`min` と `max` が等しいとき false）。コピーに実行がなければ `not-running`、ランチャーがまだ言っていなければ（実行の初めで `state` がまだ届いていない、またはその操作の項目がない）`unsupported`                                |
| `pause`、`resume`（何も変わらないとき）         | 一時停止中は `pause` が `ok` でなく（`paused`）、止まっていなければ `resume` が `ok` でありません（`not-paused`）。ランチャーはどちらも受けますが、ボタンにはすることがありません。ステップ送りか run_until が向かっている間（受け付けられて、まだ着いたと見えていない間。着いたとは、仮想時刻がその `at_ms` で、ランチャーの `state` が一時停止のこと）は、どちらも言いません |
| すべて                                          | 接続: `rejected`、`incompatible`、`offline`、`connecting` では不可になります（`stop` と `force_stop` は上のとおり）。このページの同じ操作が答えを待つ間は `busy`                                                                                                                                                                                                               |

### 操作

操作はすべて Promise で、うまくいかなければ `ActionError` で拒否します。`code`、文（`message`）、`findings`（`check-failed` のとき）、`outcome` を持ちます。`outcome` は `refused`（持ち主が断った。何も起きていない）か `unknown`（実行されたかもしれない。送り直す前に `status` と `clock` を見る）です。

`run.start(intent, { file })` の `intent` は、ランチャーの要求ファイルのキーです: `vehicle_sil_run_request`（1。省いてよい）、`variant`、`params`、`scenario`、`disable`、`enable`、`manual`、`strict`、`speed`、`start_paused`、`connect`、`time_policy`（ほかの欄は `unknown-field` で断ります。選ばないものはキーを書かず、`null` は書きません）。`file` の既定は、サーバーの起動時のファイルです。

| 操作                               | 解決する時                                                                                                                                                                                                                                                                                                            | 拒否の理由                                                                                                                                                                                                                                            |
| ---------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `run.start`                        | サーバーが受け付けた時。走り出したかは `status.run`（待つなら `until(client.status, s => s.run !== 'starting')` の後で `run` を見る）                                                                                                                                                                                 | 断り（`check-failed` と `findings`、`already-running` など）。時間内に答えがなければ `no-answer`（`unknown`: 始まったかもしれない）                                                                                                                   |
| `run.stop`、`run.forceStop`        | サーバーが受け付けた時。接続の状態に関係なく送る（WebSocket が切れていても HTTP が届くことがある）。向かっている間にもう一度頼んでも、同じ要求                                                                                                                                                                        | 同じ                                                                                                                                                                                                                                                  |
| `time.pause`、`resume`、`setSpeed` | ランチャーが答えた時。一時停止中の `pause` と、走行中の `resume` は、何も送らずに解決する（接続が `online` で、実行がその操作を受けるときだけ）                                                                                                                                                                       | ランチャーの `reason` と文（`refused`）。5 秒以内に答えがなければ `no-answer`（`unknown`）。答えの前に実行が終わったら `not-running`（`unknown`）                                                                                                     |
| `time.step`、`time.runUntil`       | 時刻が着いて、実行がまた一時停止した時。`await` の後で `clock` を読める。`runUntil(t)` は `t` に着いた時（一時停止中の実行がもう過ぎた `t` なら、すぐに解決し、時刻は動かない）、`step(ms)` は、ランチャーが答えに書く `at_ms` に着いた時（古いランチャーでは、受け付けた時刻にランチャーが丸めた `ms` を足した推定） | 上と同じ。着く前に、ほかの送り手（別のページ、stdin）の再開か一時停止が来たら `interrupted`（`unknown`）（このページの一時停止と再開、停止でも同じく拒否しますが、`status.problem` には出しません）。途中で送った別のステップは、これを終わらせません |

送る前に client が断るのは、client が持ち主であるものだけです。接続（`rejected`、`incompatible`）、同じ操作が答えを待っていること（`busy`。`start()` も含む）、引数の形（`invalid-argument`）です。それ以外は、`controls` が不可でも送り、持ち主の答えを返します（コピーが古いことがあるため）。5 秒は正面の口が持つ時間で、着くまでの待ちには適用しません（着くまでの待ちは、実行の終わりか、別の命令の割り込みで終わります）。

時刻の命令には、ページが選んだ要求の番号（`id`）を付け、答えは、同じ `id` を持つランチャーの `command` イベントです。ほかのページや stdin の、同じ名前の命令への答えは、自分のものとして取りません。

### ネットワークを外につなぐ

`can` と `ethernet` のネットワークは、ランチャーのブリッジを通して実機のバスにつなげます（[gui/protocol/README.ja.md](../protocol/README.ja.md#ネットワークを外につなぐconnecttime_policyapiconnections) の「ネットワークを外につなぐ」）。正面の口は、そのために 3 つを足します。どれも自前の規則を持ちません。

```ts
const entries = await client.connections({ network: 'ETH_EXT' }); // 利用者が選択を開いたときに
const intent = { connect: { ETH_EXT: 'fake0' }, time_policy: 'free' as const };
const result = await client.check(intent); // 何も始める前に、ランチャーの所見
await client.run.start(intent);
const stop = watch(client.outside, (rows) => console.log(rows.map((r) => r.condition)));
```

- **`connections({ network }, { file }?)`** は `POST api/connections` を呼び、答えを答えの順のまま 1 列に並べます。先ごとに `{ backend, value, label, detail, usable, reason }`、一覧を作れなかった backend と、ファイルの誤りは `{ backend, error }`（ランチャーの文）です。2 つは `'error' in entry` で見分けます。backend の一覧が欠けた答え（終了コード 1）は失敗にしません。GUI サーバーの PC でそのネットワークのブリッジを起こします（Windows では、そこで Npcap が権限の昇格を求めることがあります）。そのため、ページを開いたときではなく、利用者が選択を開いたときに呼びます。一覧はその PC のもので、ブラウザーの PC のものではありません。断られると `ActionError`（`check-busy`、`unknown-network` など）で reject します。
- **`intent.connect`** は、ネットワークの名前から、一覧が `usable: true` で出した `value`（書かれたとおり）への対応です。`'none'` は SIL Kit だけにつなぎ、キーを省くのと同じです。そのネットワークの最後の一覧が出していない値は、サーバーが `target-not-listed` で断ります。一覧を取り直してください。**`intent.time_policy`** は `realtime`（ランチャーの既定。外につないだ実行は実時間を保つ）か `free` です。
- **`check(intent?, { file, signal }?)`** は、`run.start(intent)` が始めるものをランチャーが確かめた結果を返します（`findings`。`time_lock` と `networks[].outside` は `raw` にあります）。断られると `ActionError` で reject します。その失敗は `status.problem` に入れません。照合は 1 つずつ送ります: 次は前の照合の答えの後に送ります（サーバーは照合を 1 つずつ実行し、待つものが多すぎると `check-busy` で断ります）。待っているか送っている途中の照合と同じ意図、同じファイルの照合は、それを分け合います。そのため、同じページの意図を照合するページの部品と画面は、1 回の照合で済みます。待っている照合は、呼んだすべての側の `signal` が中断していれば送らず、その signal の理由で reject します。送っている最中の照合も分け合い、その答えは送ったときのファイルを読んだものです。利用者が照合し直すよう頼んだときは `fresh: true` を渡します（待っている照合だけを分け合います）。`close()` の後、待っている照合は送らず、`closed` で reject します。
- **`outside`** は、実行の `connect` がつないだネットワークごとに 1 行です: `network`、`target`、`participant`、`link`（Ethernet）か `bus`（CAN）、`condition`（`ok`、`interrupted`、`degraded`）、`reasons`（code。ランチャーの順）、`nothing_received`（`condition` とは別）、`to_outside` と `from_outside`（このページに `outside` の事象が届くまで `null`）。実行がないときと、構成ファイルの `role: bridge` の参加者が外につなぐ実行では空です。外につながっているかどうかを、これから読まないでください（実行が実時間を保つ理由は、check の `time_lock.because` が言います）。実行の終わり方は、`connect-failed` と `connect-lost` も含めて `status` にあります。

### 変数 1 つ

```ts
const throttle = client.variable('Engine.engine.throttle');
const stop = throttle.subscribe((view) => {
  slider.value = String(view.value ?? 0);
  slider.disabled = !view.can_write;
  slider.title = view.why?.message ?? view.note ?? '';
});
await throttle.write(40); // ECU のステップで適用されたときに解決する
stop(); // 最後の購読を解除すると、サーバーに値を頼むのをやめる
```

見え方（view）は、`value`、`text`（enum は項目の名前）、`unit`、`min`、`max`、`step`、`sim_ms`、`stale`、`note`（値が生きたものでないときの 1 文）、`availability`（`unknown`、`active`、`inactive`。ECU がその変数をいま使っているか。表示のための情報で、`can_write`、`why`、`note` は見ません。接続が切れても変わりません。実行の要約にその変数が届くまでは `unknown`、届いたあと、送らない古い GUI サーバーでは `active`）、`can_write`（変数自身の条件と、ランチャーが受けるか（`state.allowed.write`））、`why`（書けない理由の `{ code, message }`。変数自身の理由が先、次にランチャーの理由）、`writing`、`problem`（このページの直前の書き込みが通らなかった理由）と、`client.low.variables.bind` の値（`condition`、`block`、`write_status`）を持ちます。

- 最初の購読がサーバーに値を頼み、最後の購読の解除がやめます。`close()` はありません。
- `write(value)` は購読がなくても書けます（その 1 回だけ送ります）。`ActionError` で拒否します。`no-answer` は `unknown` で、答えの来なかった書き込みは送り直しません。
- バインド（`client.low.variables.bind`）は、書き込みが向かっている間、次の値を保留してまとめます（スライダーのため）。正面の口の `write()` は、呼ぶたびに送ります。

## 部品（custom element）

`@vehicle-sil/gui-client/elements` は、`connect()` が返すものを描く custom element を 11 個定義します。スクリプトなしの HTML だけで画面を書けます。素のコントロールを light DOM に置き、見た目は付けません。スタイルは自分の CSS で付けます。素の HTML でも、Angular、React、Vue、Svelte のテンプレートでも使えます。import すると定義します（定義済みの名前は飛ばします）。

| 要素                     | 出すもの、すること                                                                                                     | 属性とプロパティ                                                                                                                                                                                                                                                                                                                                                   |
| ------------------------ | ---------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `<vsil-status>`          | 接続、実行の状態、仮想時刻（実時間に遅れていれば印）、知らせる問題 1 件                                                | なし                                                                                                                                                                                                                                                                                                                                                               |
| `<vsil-run-controls>`    | 開始（ページの選択つき）、停止、1 回目の停止の後の「待たずに停止」                                                     | プロパティ `intent`（次の実行を始めるときの内容）、`label-start`、`label-stop`、`label-force-stop`（ボタンの文字）                                                                                                                                                                                                                                                 |
| `<vsil-time-controls>`   | 一時停止、再開、ステップ、指定時刻まで、速さ                                                                           | `speeds`（選べる速さ。空白で区切る。既定 `0.5 1 5 max`。`controls.speed` の範囲の外の速さは選べず、`fastest` が false の間は `max` を出しません。部品は理由を言わないので、言いたい画面は範囲を `client.controls` から読みます）、`step-ms`（1 ステップの大きさ（ms）。既定 `10`）、`label-pause`、`label-resume`、`label-step`、`label-run-until`（ボタンの文字） |
| `<vsil-value>`           | 値と単位、生きた値でないときの 1 文。bool は `on` と `off` の文                                                        | `variable`、`on`、`off`（bool の true と false に出す文字）、`no-unit`（単位を出さない）                                                                                                                                                                                                                                                                           |
| `<vsil-slider>`          | 変数の範囲の数。つまみを離したときに書く                                                                               | `variable`                                                                                                                                                                                                                                                                                                                                                         |
| `<vsil-write>`           | 1 つの値を書くボタン（変数がもうその値なら押せない）                                                                   | `variable`、`value`（書く値。`false` や `3` のような JSON、それ以外は文字列）、`label`（ボタンの文字）                                                                                                                                                                                                                                                             |
| `<vsil-scenario-select>` | 次の実行のシナリオ。走らせる構成ファイルの隣にあるものから選ぶ（最初の選択肢は「なし」）                               | なし                                                                                                                                                                                                                                                                                                                                                               |
| `<vsil-scenario-result>` | 実行のシナリオの結果と手順                                                                                             | なし                                                                                                                                                                                                                                                                                                                                                               |
| `<vsil-timeline>`        | 実行のシナリオの時間の軸。中に置いた `<vehicle-sil-timeline>` に出す                                                   | なし。その要素の属性は自分で決める（下を参照）                                                                                                                                                                                                                                                                                                                     |
| `<vsil-connect>`         | 次の実行で 1 つのネットワークをつなぐ先。最初は SIL Kit だけ、続いて一覧の先（下の「部品でネットワークを外につなぐ」） | `network`（構成ファイルの `can` か `ethernet` のネットワーク）                                                                                                                                                                                                                                                                                                     |
| `<vsil-outside>`         | 実行がつないだネットワークごとに 1 行。状態と数                                                                        | なし                                                                                                                                                                                                                                                                                                                                                               |

変数は、`client.variable()` と同じ `ECU.variable`（最初の点で切る）で書きます。

```html
<script type="module" src="/vsil/vehicle-sil-gui-elements.mjs"></script>
<vsil-status></vsil-status> <vsil-run-controls></vsil-run-controls>
<label>Throttle <vsil-slider variable="Engine.engine.throttle"></vsil-slider></label>
<vsil-write
  variable="Engine.ecu.connected"
  value="false"
  label="Cut engine from the bus"
></vsil-write>
<p>
  Engine lost:
  <vsil-value variable="Dashboard.dashboard.engine_lost" on="LOST" off="ok"></vsil-value>
</p>
<vsil-time-controls speeds="0.5 1 5 max"></vsil-time-controls>
<label>Scenario of the next run <vsil-scenario-select></vsil-scenario-select></label>
<vsil-scenario-result></vsil-scenario-result>
<script src="/timeline/vehicle-sil-timeline.js"></script>
<vsil-timeline>
  <vehicle-sil-timeline compact hide="help tables"></vehicle-sil-timeline>
</vsil-timeline>
```

`<vsil-timeline>` は、要素を実行につなぐだけです。中の `<vehicle-sil-timeline>` の `src`（実行の `timeline_url`。サーバーの根から）と `hidden`（実行に時間の軸がないとき）だけを設定し、ほかには触れません。中に置かなければ既定の要素を 1 つ足し、外れるときは足したものだけ消します。要素のスクリプトは部品が読まないので、ページが読みます（`<script src="/timeline/vehicle-sil-timeline.js">`、または `import '@vehicle-sil/gui-client/timeline'`）。後で定義されても動きます。要素は、ページがタブに持つトークンを送ります。別の接続（`client`）を渡した部品では、要素の `token` のプロパティを渡してください。部品自身には `data-timeline`（`shown` か `none`）が付きます。要素の属性と事象は、[docs/gui.md](../../docs/ja/gui.md#自分の画面に時間の軸を置く) の「自分の画面に時間の軸を置く」にあります。

### 部品が守ること

- **ページの接続。** 部品は引数なしで `connect()` を呼びます。ページのスクリプトと部品は 1 つの接続（WebSocket 1 本）を共有し、自分のボタンの失敗も `<vsil-status>` に出ます。別の接続を使わせるには、部品の `client` プロパティに渡します（文書に入っていれば結び直します）。
- **ファイルの組は 1 つ。** パッケージ、1 ファイルの束の写し、GUI サーバーの `/vsil/` のファイルのどれか 1 つを使い、混ぜません。部品は本体の入口をファイル名で import し、写しを持ちません。プロジェクトでは（パッケージ）、部品も自分のスクリプトも `@vehicle-sil/gui-client` を import します。写しでは、`vehicle-sil-gui-elements.mjs` を `vehicle-sil-gui-client.mjs` の隣に置き、ライブラリーは同じファイルから import します。サーバーからでは、部品を `/vsil/vehicle-sil-gui-elements.mjs`、ライブラリーを `/vsil/vehicle-sil-gui-client.mjs` から読みます（部品の import は、そのファイルになります）。ライブラリーの写しが 2 つあると（たとえばページの隣に置いたライブラリーの写しと、`/vsil/` の部品）、接続も 2 本になります。
- **自分のマークアップと CSS。** 影の DOM はありません。部品は足した `<span class="vsil">` に描き、要素の中に置いた子は残します。CSS のために `data-*` を付けます: `data-link`、`data-run`、`data-pacing`（`<vsil-status>`）、`data-stale`、`data-can-write`、`data-availability`、`data-on`（変数の部品。`data-availability` で部品が無効になることはありません。使われていない変数を薄くするなら `[data-availability="inactive"] { opacity: .45; }`）、`data-error`（つなげなかった部品すべて。エラーにコードがあればそのコード、なければ `error`。結び直して成功すると外れます）、`data-status`（`<vsil-scenario-result>` とその手順）、`data-network`、`data-chosen`、`data-reverted`（`<vsil-connect>`。その `data-error` は、取れなかった一覧のコードでもあります）、`data-network`、`data-condition`、`data-nothing-received`（`<vsil-outside>` の各行）、`<vsil-run-controls>` と `<vsil-time-controls>` の各ボタンと速さの選択の `data-reason` と `data-caution`（あれば理由のコード）。
- **使えないボタン**は `disabled` にせず `aria-disabled="true"` にします（フォーカスを保ちます）。押しても何も起きず、理由は `aria-describedby` の先の要素に文として入ります（速さの選択も同じ）。何もすることがないと言うだけの理由（`not-running`、`offline`）もその要素に入りますが、要素は `hidden` です。読み上げは読み、画面には各ボタンの隣に繰り返しません（`<vsil-status>` が状態を出します）。変数がもうその値のときの `<vsil-write>` も同じです（"already false"）。書けない、または範囲のないスライダーは `disabled` で、理由はその隣に出ます。
- **寿命。** 部品は文書に入ると購読し、外れると全部止めます。接続を待つ間に外れたら、何も描きません。`variable` などの属性が変わると結び直します。`<vsil-connect>` は、外れるときと `network` か `file` が変わるときに、そのネットワークを `pageIntent.connect` から外します。画面にない選択で実機につないで始めないためです（描き直した部品は SIL Kit だけから始まります）。
- **つなげなかった部品**（トークンがない、サーバーがいない）は、ページの部品が同じ接続を使うので、まとめて失敗します。エラーの文はページで 1 回だけ出します。`<vsil-status>` があればそこに、なければ文書の順で最初の部品に出します（その部品が外れたら、ほかの部品が出します）。つなげなかった部品にはどれも `data-error` が付くので、CSS とスクリプトから失敗が分かります。部品が結び直して成功すると外れます。`client` で自分の接続を渡された部品と、描くときに失敗した部品（`variable` の属性がないなど）は、その部品が自分の文を出します。
- **操作の失敗**（`ActionError`）はページに投げません。`status.problem` が `<vsil-status>` に出し、書き込みの失敗はその変数の隣に出ます。
- **この 11 個から増やしません。** グラフ、表、配置は画面の仕事です。変数の時系列のグラフは、`client.low.variables.bind(ref, { history: 600 })` で履歴を持ち、点を自分で描きます（下の「[履歴](#履歴)」）。

### 文言と次の実行

文言は英語です。ボタンの名前は属性で替えられます（表の `label-start` など、`<vsil-write>` は `label`）。理由の文（使えないボタン、`caution`、問題、変数が書けない理由）は理由の持ち主の英語の文で、`setReasonText(fn)` で替えます。`fn(reason)` が文を返せばそれを出し、`null` か `undefined` なら英語のままにします。`setReasonText(null)` で外します。部品をページに置く前に呼びます。部品の自前の語（`ready`、`writing…`、`(no scenario)`、`fastest`、`behind real time`、`Step (ms)`、`Until (ms)`、`Speed`、`-`、既定の `on` と `off`、`SIL Kit only (not connected)`、SIL Kit だけに戻した選択の文、`<vsil-outside>` の `link`、`bus`、`to outside`、`from outside`、`nothing received`）は替えられません。

選ぶ部品と開始のボタンは別の要素なので、次の実行に選んだものはモジュールが `pageIntent` に持ちます（`get()`、`subscribe(fn)`、`update(patch)`。`''` か `undefined` の欄は消えます）。`<vsil-scenario-select>` が `scenario` を書き、`<vsil-run-controls>` は `{ ...pageIntent.get(), ...intent }` で開始するので、`intent` プロパティが勝ちます。開始するファイルは `status.file` のもので、`<vsil-scenario-select>` がシナリオを並べるファイルと同じです（実行をまだ始めていなければ、サーバーの起動時のファイル）。自分のコードで開始する画面は、意図を `client.run.start(intent)` に自分で渡します。照合が見る選択は `pageIntent` にも書きます。そうすると `<vsil-connect>` は、画面が始めるものを照合します。

GUI サーバーは、構成ファイルを付けて起動します（`vehicle-sil-gui --web my-screen simulation.yaml`）。付けないとサーバーに起動時のファイルがなく、部品だけの画面は実行を開始できません。断った理由（`no-file`）は `<vsil-status>` に出ます。そのときファイルを指定できるのは、自分のコードだけです: `client.run.start(intent, { file })`。

```ts
import '@vehicle-sil/gui-client/elements'; // defines the parts
import { pageIntent, setReasonText } from '@vehicle-sil/gui-client/elements';

setReasonText((reason) => (reason.code === 'running' ? 'A run is going.' : null));
pageIntent.update({ variant: 'winter' }); // what the next start uses, with the scenario that was chosen
const controls = document.querySelector('vsil-run-controls'); // typed: the parts are in HTMLElementTagNameMap
if (controls) controls.intent = { speed: 'max' };
```

部品は DOM を要するので、Node は読みません（本体の入口は部品を import しません）。

### 部品でネットワークを外につなぐ

`<vsil-connect network="ETH_EXT">` は素の `<select>` です。最初の選択肢は `none`（SIL Kit だけ）です。一覧（`connections()`）は、利用者が選択を開いたときにだけ取ります: 押したとき、または Tab、Shift、Escape と修飾キーだけの押下以外のキーのとき（Tab で通り過ぎても、ページを開いても取りません）。そのたびに取り直し、待つ間は `aria-busy` です。先は一覧の順に、backend の名前の下に並べます（`<optgroup>`）。使えない先は `disabled` の選択肢で、文は label、detail、ランチャーの理由です。一覧のない backend は、選択を説明する要素（`aria-describedby`）に 1 行で出ます。選んだものは `pageIntent.connect` に入ります（`none` はそのネットワークを外します）。ページが自分で `pageIntent` に入れた選択は、書かれたとおりに出します。部品が一覧を取り、照合する構成ファイルは、`file` 属性のファイルです（既定は `run.start` と同じく起動のときのファイル）。別のファイルを始めるページは、`file` にそのファイルを書きます。

ネットワークに選択がある間、部品は、ページの意図が最後に変わってから約 200 ms 後に check（`check()`）します。どの部品やスクリプトが変えても同じです。同じ意図を check するページの部品と画面は、1 つの check を分け合います（`check()` が分け合います）。出すのは、ランチャーの文のままで、自分のネットワークの `connect-*` と `outside-*` の所見、`connect` にあるほかのネットワークの `connect-*` の所見（名前を頭に付けます: `CAN1: ...`。2 つのネットワークにかかわる所見は、片方の名前だけを持つため）、このネットワークのために実行が実時間を保つと check が言うとき（`time_lock.because`）の実行全体の `outside-*` の所見です。各行に `data-code` と `data-severity` が付きます。これらの行は開始の前の案内で、実行がどうなるかは、開始のときに GUI サーバーとランチャーが決めます。断られたときは、サーバーの文のまま出します。`target-not-listed` の後、利用者が開いたことのある部品は一覧を 1 回だけ取り直し、選択がまだ一覧にあれば check し直します。新しい一覧に選択がない、または使えない先として出たときは、選択を `none` に戻します: 説明の要素に、どの値をなぜ戻したかの 1 行（`'<value>' is not in the list of this computer: back to SIL Kit only` か `'<value>' cannot be used (<reason>): back to SIL Kit only`）が出て、`data-reverted` に、利用者が次に選ぶまでその値が入ります。部品は、ページを開き直したときに前の選択を戻しません。`time_policy` を選ぶ口も持ちません。ページが `pageIntent.update({ time_policy: 'free' })` で決めます。

`<vsil-outside>` は `client.outside` の行を文字で描きます（色だけに頼りません。数は毎秒変わるので live region にしません）。行がなければ何も描きません。

## ECU の変数

```ts
const watch = client.variables.watch([{ peer: 'Engine', name: 'engine.speed' }], {
  periodMs: 100,
  history: 600,
});
watch.values.subscribe((values) => console.log(values['Engine']?.['engine.speed']));
try {
  const operation = await client.variables.write({ Engine: { 'engine.throttle': 50 } });
  console.log(operation.peers['Engine'].appliedAtMs);
} catch (error) {
  if (error instanceof WriteError) console.log(error.kind, error.message);
}
watch.close();
```

### 一覧と状態（`client.variables.state`）

| `status`     | 意味                             |
| ------------ | -------------------------------- |
| `unknown`    | まだ聞いていない                 |
| `off`        | この実行に変数はない             |
| `connecting` | 接続している途中                 |
| `ready`      | 読み書きできる                   |
| `failed`     | 接続できなかった、拒否された     |
| `ended`      | 実行が終わった（値は最後のもの） |

- `off`、`connecting`、`failed` の理由は `reason`（サーバーの英語の文）です。
- 実行を始める前に使えるかは、`client.app` の `variables`（`available`、`reason`）でわかります。
- 変数の定義は `peers[].variables[]`（`type`、`role`、`writable`、`unit`、`min`、`max`、`step`、`description`、`enum`）です。

### 値を読む（`watch`）

`watch` は、変数（`'all'` は全部）の最後の値を受け取ります。使い終わったら必ず `close()` します。

**値**: `values`（`Readable`）は、`values[peer]?.[name]` が `VariableReading`（`value`、`sampleMs`、`simMs`、`paused`）です。値ごとに「いつのものか」を持ちます。

| `paused` | 値の意味                                                                          |
| -------- | --------------------------------------------------------------------------------- |
| 真       | 止まっている時刻 `simMs` の値（その時刻より前に始まったステップの値は入っている） |
| 偽       | 動いている途中の最後の値                                                          |

**購読**:

- 同じ変数を見る watch がいくつあっても、サーバーへの購読は変数ごとに 1 つです。変数は、頼まれた中でいちばん短い間隔の購読に入ります（間隔は 20、50、100、200、500 ms … の段に丸めます）。
- `close()` すると、ほかに見ている watch がなければサーバーの購読もやめます。
- 再接続したら、自動で購読し直します。
- 実行が替わっても watch は続き、前の実行の値と履歴は捨てます。
- 値を覚えるのは、だれかが見ている変数だけです。

**履歴**（`history` を頼んだとき）:

- 変数ごとに、その数までの点（`sampleMs`、`value`）を環状バッファーに持ちます。変数ごとの上限は `HISTORY_MAX` で、古いものから捨てます。
- メモリーは点が届いてから確保します（頼んだだけでは確保しません）。
- 全部の変数の履歴を合わせて `HISTORY_TOTAL_MAX` 点（約 34 MB）までです。そこに届くと、どの履歴もそれ以上は伸びず、そのときの大きさのまま古い点を捨てます（`history(ref)` が返す点が、頼んだ数より少なくなります）。
- 同じステップの値は 1 点です。同じステップの値が変わって届いたら、その点を置き換えます。
- 実行が替わると、履歴は空から始まります。
- `watch.history(ref)` は、呼ぶたびに新しい配列を返します。
- 通知は約 100 ms ごとにまとめて届くので（`BATCH_MS`）、`values` の通知はその頻度ですが、履歴には間の値も入ります。

**上限を超えそうなとき**:

- 長い名前を多く挙げて、購読の 1 通がサーバーの上限（256 KiB）を超えそうなときは、その間隔の購読を「全部」に替えます（受け取った中から、見ている変数だけを使います）。
- サーバーが watch を拒否したとき（上限の違うサーバー）は、`client.variables.watchProblem` に出ます。

### 値を書き込む（`write`）

`write(writes, options)` は、呼んだ時点の実行（`client.variables.state` の `session`）に向けて送ります（本文の `session`）。

- サーバーは、その実行がもう動いていなければ拒否します（`session-changed`）。そのため、実行が替わる間に送った書き込みが、次の実行に書かれることはありません。
- 実行がまだわからないときは、送らずに `refused`（`not-running`）で拒否します。
- サーバーが型と範囲を確認し、ランチャーに渡します。
- Promise は、反映されたら（`applied`）その操作（`OperationView`）で解決し、ほかは `WriteError` で reject します。
- 同じ書き込みを自動で送り直すことはありません。

`WriteError` の `kind` は次のとおりです。

| `kind`      | 意味                                                     | 送り直してよいか                           |
| ----------- | -------------------------------------------------------- | ------------------------------------------ |
| `refused`   | サーバーが受け付けなかった（ランチャーには渡っていない） | 直してから、よい                           |
| `failed`    | 受け付けたが、反映されなかった                           | 理由を直してから、よい（反映されていない） |
| `no-answer` | **反映されたかどうかわからない**                         | **そのままは送り直さない**（下）           |
| `closed`    | 結果の前に `close()` した                                | 同上                                       |

- `refused` の `code` は、`unknown-variable`、`not-writable`、`invalid-field`、`not-running`、`session-changed`、`variables-not-ready`、`write-busy` などです。
- `failed` の `operation` には、participant ごとの状態と理由（`late`、`rejected`、`invalid` など）が入ります。
- `no-answer` になるのは、次のどれかのときです。
  - `timeoutMs` の間に結果が来なかった（`operation` に、わかっている最後の状態）。
  - HTTP のレスポンスがなかった。
  - サーバーが途中で失敗した（500）。
  - サーバーが Control Server のレスポンスを待ちきれなかった（`code` が `write-timeout`）か、途中で切断された（`write-failed`）。
  - レスポンスの前に実行が替わった。
  - 再接続している間に確定して、結果が失われた（下の「再接続」）。
  - サーバーが未確定の操作を覚えきれずに、追うのをやめた。

**二重の書き込みを避ける**: `no-answer` の後は、送り直す前に確認します。

- `error.operation` があれば、その番号の操作を `client.variables.operations` で追います。`queued` なら、シミュレーションがそのステップに進めば反映されます（待つ時間が短かっただけです）。
- 番号がなければ、`operations` に自分の書き込みらしい操作が増えていないかと、その変数の値（watch）を見ます。
- 反映されていないとわかってから送り直します。
- 値を足し込むような（同じ書き込みを 2 回すると結果が変わる）使い方では、特に守ります。同じ値を書くだけなら、2 回書いても結果は同じです。

**再接続**: 結果を待つ間に接続が切断されても、再接続した後の要約（`hello`、`fetchSession()`）で続きがわかります。要約には、サーバーが覚えている未確定の操作の全部と、番号が `operations_from_seq` 以上の操作の全部が入っています（サーバーが受け取るもののうち。シナリオ自身の書き込みの操作は届きません）。

| 自分の操作                                           | ライブラリの扱い                                            |
| ---------------------------------------------------- | ----------------------------------------------------------- |
| 一覧にある                                           | その状態を使う（未確定なら、そのまま待つ）                  |
| 一覧になく、番号が `operations_from_seq` より小さい  | 切断されている間に確定して結果が失われたとして、`no-answer` |
| 一覧になく、サーバーが未確定の操作を覚えきれなかった | 古い操作を `no-answer` にする                               |

- まだ反映されていない書き込み（止まっている間の書き込みなど）は、後からいくつ操作が続いても `no-answer` になりません。
- サーバーが覚えきれなかったとは、未確定の操作が 256 件を超えたときです（要約の `operations_open_omitted`）。サーバーがもう追っていないので `no-answer` にし、エラーの文でそうとわかります。

**オプションと、関係する状態**:

- `onAccepted`（`{session, seq}` を受け取る関数）は、サーバーが書き込みを受け取って操作の番号を返したときに 1 回呼ばれます。結果より前です。拒否されたとき、レスポンスがなかったときは呼ばれません。
- `onAccepted` は、「受け付けた、反映を待っている」を見せる、次の書き込みを送ってよいと知る、のに使います。`bind` の書き込みは、これを使って、受け取られる前の次の値を送らずに覚えておき、受け取られたら最後の値だけを送ります。
- `timeoutMs`（既定 `WRITE_TIMEOUT_MS`）は、HTTP のレスポンスが来てから数えます。`Infinity` は時間で切りません（実行が替わるか `close()` するまで待ちます）。2^31-1 ms（約 24.8 日）より長い値は、そこに丸めます。数でない値と 0 以下は既定の時間です。
- 書き込みは、指定した仮想時間（`atMs`。省くと次のステップ）のステップで反映されるので、止まっている間は確定しません。止めて書くときは、`client.variables.operations` で `queued` を見てから進めます（`examples/gui_client/variables.mjs`）。
- 最近の操作（ほかのクライアントの書き込みも。シナリオ自身の書き込みは除く）は `client.variables.operations` で読めます。

### 最初のステップの前に書く

止めて始めた実行（`start_paused`）は、変数の読み書きができるランチャーでは、最初のステップの前で待ちます。その間は `isHolding(session)` が真で（ランチャーの `state` の `holding`。セッションの写しの `launcherState`）、再開とステップ実行を押せます（書いた値は時刻 0 のステップで反映されます）。

## コンポーネントを作る（`bind`）

スライダー、スイッチ、ランプのように、変数 1 つを表示して書き込むコンポーネントは、`watch` と `write` を自分で組み合わせずに `client.variables.bind(ref, options)` を使います。値の状態、書けるかどうかと理由、書き込みの状態を、1 つの `Readable`（`state`）にまとめて出します。

```ts
const throttle = client.variables.bind(
  { peer: 'Engine', name: 'engine.throttle' },
  { needsRange: true, needsValue: true },
);
throttle.state.subscribe(({ info, reading, pending, block, writeStatus, condition }) => {
  slider.disabled = block !== null; // block: いま書けない理由。書けるなら null
  slider.value = String(pending ?? reading?.value ?? info?.min ?? 0); // pending: 送って結果を待っている値
  status.textContent = isWriteError(writeStatus) ? writeStatus.message : writeStatus.kind;
  stale.hidden = condition.kind === 'live';
});
slider.onchange = () => throttle.write(Number(slider.value));
// 使い終わったら
throttle.close();
```

使い終わったら必ず `close()` します（watch と購読を片づけます）。

### `state` のフィールド（`BoundVariable`）

| フィールド       | 中身                                                                                                                      |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------- |
| `info`           | 変数の定義（いまの実行になければ `undefined`）                                                                            |
| `reading`        | 最後の値（`VariableReading`。まだないとき、`visible` が偽のときは `undefined`）                                           |
| `condition`      | 値の状態（下の「値の状態（`condition`）」）                                                                               |
| `block`          | いま書けない理由（下の「書けない理由（`block`）」）                                                                       |
| `launcherReason` | `block` が `launcher` のとき、ランチャーの理由（`code` と英語の `message`。`launcherAnswer` が読んだまま）。ほかは `null` |
| `writeStatus`    | 最後の書き込みの状態（下の「書き込みの状態（`writeStatus`、`pending`）」）                                                |
| `pending`        | 送っていて、結果がまだの値（なければ `null`）                                                                             |

- `subscribe` は変わったときだけ呼ばれます（最初の状態は `state.get()`）。
- `get()` は、いつもいまの状態から作ります。そのため、ほかの `Readable`（`client.variables.state` など）の通知の途中で `get()` を呼ぶと、その場で、この `state` の購読者に新しい状態を通知することがあります。

### 値の状態（`condition`）

`condition.kind` で分けます。

| `kind`        | 意味                                                                |
| ------------- | ------------------------------------------------------------------- |
| `unavailable` | 変数の機能が使えない（`status` に `client.variables.state` の状態） |
| `missing`     | いまの実行にない変数                                                |
| `waiting`     | いまの実行にあるが、値がまだ届いていない                            |
| `live`        | 動いている途中の最後の値                                            |
| `paused`      | 止まっている時刻の値                                                |
| `stale`       | 古いかもしれない値（理由は `why`）                                  |
| `ended`       | 終わった実行の、最後に読めた値                                      |

`unavailable` の `status` は、実行していない、この実行に変数はない、接続している途中、接続できない、のどれかです。`stale` の `why` は次のとおりです。

| `why`          | 意味                                     |
| -------------- | ---------------------------------------- |
| `connection`   | クライアントとサーバーの接続が切断された |
| `reconnecting` | サーバーがランチャーに再接続している     |
| `peer`         | ECU が切断された                         |
| `reading`      | 値の読み取りが失敗し続けている           |

### 書けない理由（`block`）

書けるなら `null` です。複数に当てはまるときは、表で上にあるものになります。

変数と実行の事実が先、次にランチャーの答え（`launcher`）、次に client の予測（`parameter`）、最後に部品の要るものです。ランチャーから来るのは `launcher` だけで、ほかの値は client 自身のものです。ランチャーが何を受けるかについて、client は自分の規則を持ちません。シナリオの実行中でもランチャーが書き込みを受けるなら、`bind` は書かせます。ランチャーの答えは、正面の口の `controls` と同じく `launcherAnswer(launcherState, 'write')` で読みます。

| `block`             | 意味                                                                                                                                                                                                                                                   |
| ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `read-only`         | 読むだけの変数（`role` が `output`）                                                                                                                                                                                                                   |
| `disconnected`      | クライアントとサーバーの接続が切断されている                                                                                                                                                                                                           |
| `ended`             | 実行が終わった                                                                                                                                                                                                                                         |
| `not-ready`         | 変数の機能が使えない（実行していない、接続している途中、接続できない）                                                                                                                                                                                 |
| `missing`           | いまの実行にない変数                                                                                                                                                                                                                                   |
| `peer-disconnected` | その ECU が Control Server と接続していない                                                                                                                                                                                                            |
| `launcher`          | ランチャーがいまは書き込みを受けない（その `state` の `allowed.write`。`launcherReason` に code と文がある。たとえば、シナリオの実行中は `scenario-running`）か、受けるとまだ言っていない（`allowed.write` のある `state` が届くまでは `unsupported`） |
| `parameter`         | `parameter` は、止めて始めた実行の最初のステップの前（`isHolding(session)`: ランチャーの `state` の `holding`）だけ書ける。ECU の規則の client による予測（ランチャーは変数ごとには言わない）                                                          |
| `no-range`          | `needsRange` を付けたのに、変数に範囲（`min` と `max`）がない                                                                                                                                                                                          |
| `no-value`          | `needsValue` を付けていて、値（読んだ値か送った値）がまだない                                                                                                                                                                                          |

### 書き込みの状態（`writeStatus`、`pending`）

`writeStatus` は、最後の書き込みの状態です。

| `writeStatus`                              | 意味                                                        |
| ------------------------------------------ | ----------------------------------------------------------- |
| `idle`                                     | 空（最後の書き込みがない）                                  |
| `sending`                                  | 送った                                                      |
| `queued`                                   | サーバーが受け取った（`seq` は操作の番号）                  |
| `applied`                                  | 反映された（`atMs`）                                        |
| `invalid`                                  | `reject(message)` に渡した文（送っていない）                |
| `dropped`                                  | 覚えていた値を送らなかった（下の「`write(value)` の動き」） |
| `refused`、`failed`、`no-answer`、`closed` | `WriteError` の `kind`（`message` と `code` を持つ）        |

- `isWriteError(writeStatus)` は、`message` を持つもの（うまくいかなかったもの）を見分けます。
- `message` は英語です。画面の言葉にするときは `kind` から作ります。
- `pending` があれば、読んだ値の代わりに表示します（押したスイッチが、結果を待つ間に元へ戻って見えません）。

### `write(value)` の動き

- 書けないとき（`block`）は何もしません。
- Promise は返さず、結果は `writeStatus` に出ます。
- 時間では切らずに操作の結果を待ちます（止まっている間の書き込みは、進めるまで `queued` のままです）。
- サーバーが前の書き込みをまだ受け取っていない間（`sending`）の次の値は、送らずに最後の 1 つだけを覚えておき、受け取られたら送ります（スライダーやキーの連打で、書き込みが積み重なりません）。
- 覚えた値を送るときは、実行が同じで、まだ書けるかを確認し直し、だめなら送らずに `dropped` にします。
- 前の書き込みが、受け取られる前に拒否されたら、覚えた値も送りません。
- 結果が来なかった書き込み（`no-answer`）を、自動で送り直すことはしません。
- `reject(message)` は、送る前にコンポーネントが見つけたエラー（数でない文字、範囲の外）を `invalid` として出します。送っている書き込み、覚えている値、`pending` には触れません。

### 変数ごとに 1 つの状態

- 同じ変数を何か所で `bind` しても、`writeStatus` と `pending` は同じものが見え、書き込みは重なりません。
- `bind` を閉じて作り直しても残ります。
- 実行ごとなので、実行が替わると空（`idle`、`null`）から始まり、前の実行の書き込みの結果は出ません。

### 見えている間だけ watch する

`bind` は、作ったときから値を受け取ります（`watch` と同じく、サーバーへの購読は変数ごとに 1 つ）。コンポーネントが多い画面では、`{ visible: false }` で作り、見えたとき（IntersectionObserver など）に `setVisible(true)`、隠れたら `setVisible(false)` を呼びます。偽の間は、次のようになります。

| フィールド               | `visible` が偽の間                                                               |
| ------------------------ | -------------------------------------------------------------------------------- |
| `reading`                | `undefined`                                                                      |
| `condition`              | `waiting`                                                                        |
| `writeStatus`、`pending` | 変わらず出る                                                                     |
| `block`                  | 変わらず出る（`needsValue` を付けた `bind` は、`pending` もなければ `no-value`） |

### 履歴

`{ history: 600 }` で作ると、`history()` が点（古い順）を返します。点が増えるたびに `state` が通知するので、そこで読み直します。

### 確認しないこと、`close()` の後

- `write(value)` は、値の型と範囲を確認せずに送ります。合わない値は、サーバーが拒否して `writeStatus` が `refused` になります。先に確認して文を出したいときは、コンポーネントで確認して `reject(message)` を呼びます。
- `close()` の後の `write`、`reject`、`setVisible` は何もしません。
- `close()` しても、すでに送った書き込みと覚えた値は取り消しません。覚えた値は、前の書き込みが受け取られたら送られます（状態は変数ごとに持つので、同じ変数を次に `bind` すると見えます）。

## ライフサイクル

| 呼び出し                                       | すること                                                              |
| ---------------------------------------------- | --------------------------------------------------------------------- |
| `createClient()`                               | 作るだけで、接続しない                                                |
| `connect()`                                    | WebSocket で接続する（トークンがないときは何もしない）                |
| 接続している間の `connect()`                   | 何もしない（再接続を待っている間も）                                  |
| `close()`                                      | 切断し、再接続しない（片づけるものは下）                              |
| `close()` の後の `connect()`                   | また接続し、残っている watch を購読し直す                             |
| `close()` の後の HTTP のリクエスト             | 使える（`loadApp()`、`run.start()` などは、接続していなくても送れる） |
| 拒否された・バージョンが違った後の `connect()` | 何もしない（`session.rejected`、`session.incompatible` は戻らない）   |

- `close()` は、待っているタイマーを止め、時間のコマンドのレスポンスを待つのをやめ、結果を待っている書き込みを `WriteError`（`closed`）で reject します。まとめている途中の通知は捨てます。変数の watch は残ります。
- `close()` の後に `connect()` すると、セッションのコピーは残っていて、持っている分の続きから受け直します。
- 別のトークンやバージョンで接続するときは、クライアントを作り直します。

## コーディングのルール

- 自分のファイルのほかは import しない（`eslint.config.js` が止める。動的な `import()` も、テスト用のヘルパーのほかでは止める。`tsconfig.json` の `rootDir` で、`src/` の外も止まる）。
- ブラウザにしかないもの（`window`、`document`、`localStorage` など）を使わない。必要なものは設定（`ClientOptions`）で受け取る。
- 人に見せる文（`TimeFailure.message` など）は英語。画面の言葉にするのはラッパーの役目（`kind` などの機械が読むフィールドから作る）。
- 公開 API を追加したら、`src/index.ts` と上の「公開 API」の表に追加し、[自分の画面を作る](../../docs/ja/howto/gui-screen.md) を合わせる。

## テスト

ツール（TypeScript、ESLint、Prettier、Vitest、esbuild）は `gui/web` の `node_modules` のものを使います。`gui/web` で次を実行します（`scripts/verify-web.py` も実行します）。

| コマンド               | 内容                                                                                                                                                    |
| ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `npm run test:client`  | 型の検査（`tsc -p ../client`）と単体テスト（Vitest。ライブラリは Node の上で、Angular なしに動く。`src/elements` の部品はヘッドレスの Chromium で動く） |
| `npm run build:client` | 1 つの ESM のファイルにまとめ、部品はそれを import する 2 つ目のファイルにまとめる（esbuild。Angular のビルドのツールが持っているもの）                 |
| `npm run lint`         | `gui/web` と、この `eslint.config.js`                                                                                                                   |
| `npm run format:check` | `gui/web` と、このディレクトリの書式（`gui/.prettierrc`）                                                                                               |

部品の試験には Playwright の Chromium が要ります（`gui/web` で `npx playwright install --only-shell chromium`。起動できるかは `scripts/doctor.py` が答えます）。ヘッドレスで動きます。画面で見るには、`gui/web` で `npx vitest --config ../client/vitest.config.mjs --project browser --browser.headless=false` を実行します。

単体テストは、サーバーの通知を作らずに `gui/protocol/examples/` の例を流します（`testing/protocol-examples.ts`）。

## 関連するドキュメント

| 知りたいこと                                                               | ドキュメント                                                                   |
| -------------------------------------------------------------------------- | ------------------------------------------------------------------------------ |
| GUI に自分のコードを足す道（カスタムパネル、自分の画面、ツール）の使い分け | [docs/ja/gui.md](../../docs/ja/gui.md#自分の画面を作る) の「自分の画面を作る」 |
| 自分の画面を作り、GUI サーバーに配信させる手順                             | [自分の画面を作る](../../docs/ja/howto/gui-screen.md)                          |
| サーバーの API（HTTP と WebSocket の形、バージョンと互換のルール、認証）   | [gui/protocol](../protocol/README.ja.md)                                       |
| ライブラリが肩代わりしないルール                                           | [docs/gui.md](../../docs/ja/gui.md) の「画面を作るときのルール」               |
| 自分の画面の出発点にするファイル                                           | 画面のテンプレート [examples/gui_starter](../../examples/gui_starter/)         |
| 画面を使わずに Node から使う例                                             | [examples/gui_client](../../examples/gui_client/)                              |

`examples/gui_client` は、リポジトリにだけあります（ビルド済みのツールのアーカイブに入っているのは、`gui_starter/` と `client/` です）。

| ファイル               | 内容                                                                   |
| ---------------------- | ---------------------------------------------------------------------- |
| `run.mjs`              | ライブラリを使わずにサーバーの API を呼ぶ                              |
| `run-with-library.mjs` | ライブラリを 1 つにまとめたファイルを、素の JavaScript（Node）から使う |
| `front.mjs`            | 正面の口（`connect()`、`status`）を Node から使う                      |
| `variables.mjs`        | 同じくライブラリで、ECU の変数を読み書きする                           |
