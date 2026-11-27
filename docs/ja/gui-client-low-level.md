# GUI クライアント低水準リファレンス（`createClient()`）

[English](../gui-client-low-level.md)

`@vehicle-sil/gui-client` の低水準リファレンスです。`createClient()`、`GuiClient`、公開 API の全一覧、プロトコルに近い ECU 変数の watch・書き込み、コンポーネント向け `bind`、ライフサイクルの詳細、開発者向けテスト手順をまとめています。入門手順、`connect()` と `createFront()` の選び方、高水準レシピ、5 フレームワーク対応の 11 部品（`@vehicle-sil/gui-client/elements`）については、まず [gui/client/README.ja.md](../../gui/client/README.ja.md) を参照してください。

正面の口（`connect()` / `createFront()`）は `createClient()` の上に作られており、同じインスタンスを `front.low` として公開しています。生のイベント、ログ、指摘、participant の状態、複数変数の同時 `enqueue()` / `write()`、あるいは `variables.bind()` が必要なときに、この低水準 API を使います。

| 知りたいこと | 節 |
| --- | --- |
| パッケージのエントリーポイント（`src/index.ts`、`./elements`、`./timeline`、フレームワーク型アダプタ、`testing`、`internal`） | [エントリーポイント](#エントリーポイント) |
| 公開エクスポートのグループ別全一覧 | [公開 API](#公開-api) |
| `createClient()` での接続、`Readable`、タイムアウト、`raw` キー、通信差し替え | [基本の使い方](#基本の使い方) |
| 値の watch、4 段階 `enqueue()` / `write()`、仮想時間連動ウォッチドッグ | [ECU の変数](#ecu-の変数) |
| `bind()` で変数コンポーネントを作る（`condition`、`block`、`writeStatus`、`history`） | [コンポーネントを作る（`bind`）](#コンポーネントを作るbind) |
| 接続のライフサイクル（`connect()`、`close()`、再接続） | [ライフサイクル](#ライフサイクル) |
| `gui/client` の開発とテスト | [コーディングのルール](#コーディングのルール)、[テスト](#テスト) |

## エントリーポイント

| エントリーポイント | 名前 | 中身 |
| --- | --- | --- |
| `src/index.ts` | `@vehicle-sil/gui-client` | 公開 API（下の「[公開 API](#公開-api)」）。互換を保証するのはここと `./elements` |
| `src/elements/index.ts`（`dist/index.js` を import する `dist/elements.js`） | `@vehicle-sil/gui-client/elements` | 11 個の `vsil-*` カスタム要素、`pageIntent`、`setReasonText`、および要素・イベント型（`VsilElementContracts`、`WriteQueuedInfo`、`WriteOutcome`、`WriteSuccess`、`WriteFailure`）。DOM を要します。本体の入口はこれを import しません |
| `../timeline/vehicle-sil-timeline.js`（`timeline/vehicle-sil-timeline.mjs`） | `@vehicle-sil/gui-client/timeline` | `<vehicle-sil-timeline>` 要素と型（[docs/ja/gui.md](gui.md#自分の画面に時間の軸を置く) の「自分の画面に時間の軸を置く」） |
| `types/frameworks/react.d.ts`（`dist/empty.js`） | `@vehicle-sil/gui-client/react` | `AllElementContracts` から導出された React 18/19 向け型専用の JSX 要素宣言（`react` への実行時依存なし） |
| `types/frameworks/vue.d.ts`（`dist/empty.js`） | `@vehicle-sil/gui-client/vue` | `AllElementContracts` から導出された Vue 3 向け型専用の `GlobalComponents` 宣言（`vue` への実行時依存なし） |
| `types/frameworks/svelte.d.ts`（`dist/empty.js`） | `@vehicle-sil/gui-client/svelte` | `AllElementContracts` から導出された Svelte 5 向け型専用の `SvelteHTMLElements` 宣言（`svelte` への実行時依存なし） |
| `src/testing/index.ts`（リポジトリ内のみ） | `@vehicle-sil/gui-client/testing` | テスト用ヘルパー（`FakeWebSockets`、`FakeHttp`、`gui/protocol/examples/` の読み込み、`sessionViewFrom`） |
| `src/internal.ts`（リポジトリ内のみ） | `@vehicle-sil/gui-client/internal` | **互換の保証外**。内部のクラスとパーサー。アプリでは使わない |

- `testing` と `internal` は公開 npm パッケージに含まれません。
- 公開 API は狭く保ちます（追加は互換を保ち、削除は互換を壊すため）。公開エントリーポイントに足りないものがあるときは、`internal` に頼らず `src/index.ts` へ追加するかを決めます。

## 公開 API

`src/index.ts` が公開する名前をグループごとに挙げます。括弧の中はそのオブジェクトの主なメンバーです。

### クライアント

| 種類 | 名前 |
| --- | --- |
| 関数 | `createClient(options)` |
| 型 | `ClientOptions`（`baseUrl`、`token`、`origin`、`http`、`socket`、`answerTimeoutMs`、`batchMs`、`watchPeriodMs`、`onError`） |
| 型 | `GuiClient`（`connection`、`session`、`run`、`time`、`variables`、`app`、`limits`、`connect()`、`close()`、`loadApp()`、`configs()`、`config(file)`、`check()`、`describe()`、`saveConfig()`、`prepare()`、`connections()`、`dashboard(file)`、`scenarios(file)`、`recording()`） |
| 定数 | `DEFAULT_LIMITS` |

### 正面の口

| 種類 | 名前 |
| --- | --- |
| 関数 | `connect(options?)`、`createFront(options)`、`watch(source, fn)`、`until(source, test, options?)` |
| クラス | `ActionError`（`code`、`message`、`outcome`、`findings`） |
| 型 | `ConnectOptions`（`ClientOptions` の別名）、`UntilOptions`（`timeoutMs`、`signal`）、`Front`（`low`、`status`、`clock`、`controls`、`scenario`、`runtime`、`outside`、`prepare()`、`configs()`、`recording()`、`run`、`time`、`variable()`、`scenarios()`、`connections()`、`check()`、`readConfig()`、`describeAgain()`、`saveConfig()`、`close()`）、`FrontStatus`、`FrontResult`、`FrontClock`、`FrontControls`、`Control`、`Reason`、`FrontScenario`、`FrontRun`、`FrontTime`、`FrontVariable`、`FrontWriteOptions`、`QueuedWrite`、`WriteResult`、`VariableView`、`Intent`、`Outside`、`ConnectEntry`、`ConnectTarget`、`ConnectListError`、`Link`、`RunState`、`EndKind`、`Pacing`、`OperationName`、`RuntimeStatus` |

### トークン

| 種類 | 名前 |
| --- | --- |
| 関数 | `takeToken(place)` |
| 型 | `TokenPlace` |
| 定数 | `TOKEN_STORAGE_KEY` |

ブラウザでは `takeToken(window)` と呼びます。URL の `#token=` を読み、`history.replaceState` で URL から消し、`sessionStorage` に置きます。URL になければ `sessionStorage` に置いたものを返し、どちらにもなければ `null` を返します。`--no-auth` で起動したサーバーにつなぐときは、`token` を省略するか `null` にできます（トークン認証が有効なサーバーでは、HTTP は 401 で失敗し WebSocket は `rejected` になります）。

### 接続（読むだけ）

| 種類 | 名前 |
| --- | --- |
| 型 | `ClientConnection`（`state`、`failures`、`lastClose`、`tryAgainReason`、`onBatch`、`onUnreadable`） |
| 型 | `ConnectionState`、`CloseInfo` |
| 定数 | `BATCH_MS`（`100`）、`TRY_AGAIN_LATER` |

### セッションのコピー

| 種類 | 名前 |
| --- | --- |
| 型 | `ClientSession`（`session`、`currentTarget`、`currentInstance`、`ready`、`allowed`、`runtime`、`lastEventAt`、`notices`、`rejected`、`incompatible`、`onMessages`、`report`、`dismissNotice`） |
| 型 | `Notice`、`SessionView`、`ParticipantView`、`LauncherView`、`LogLine`、`LogSource`、`TimeView`、`CommandView`、`RunOutcome`、`LauncherAnswer` |
| 関数 | `runOutcome`、`phaseIndex`、`isHolding`、`isLauncherAlive`、`isLauncherFinishing`、`isRunActive`、`isSessionRunning`、`isScenarioHandedOver`、`isScenarioRunning`、`launcherAnswer` |

`ClientSession.allowed` は、GUI サーバーがいま受けるものです（`start`、`stop`、`force`、`save`。それぞれ `ok`、理由のコード `reason`、文 `message`。サーバーが言うまでは `null`）。`SessionView.launcherState` は、ランチャーの最後の `state` イベントです（実行の状態と、いま受けるもの。`rev` の大きい方が勝ちます。届くまでは `null`）。`launcherAnswer(launcherState, name)` は、1 つの操作（`write`、`pause` など）について、それが言うことを読みます: `reason`（受けるなら `null`）と `caution`。まだ言っていないランチャー（`state` がまだない、その操作の項目がない）は、受けないものとします（`unsupported`）。正面の口の `controls` と `variables.bind` は、これでランチャーを読みます。

順序付きの通知（`session`、`event`、`allowed`、`runtime`、`variables`、`availability`、`operation`、`error`、`synced`）は、`(instance, run_id, stream_seq)` で順序付けられた単一の WebSocket ストリームで届きます。制御フレーム（`synced`、`operation`、`error`、およびランチャーの `command` イベント）は `batchMs` を待たずマイクロタスクで即時フラッシュされます。HTTP の変更操作（`api/run`、`api/stop-session`、`api/prepare`、`api/time`、`api/write`）は受領応答を返し、状態の更新は WebSocket ストリームで届きます。

`currentTarget()` は現在の `{ run_id, instance }` を返し、不明なら `null` を返します。`currentInstance()` は実行がなくても直近に認証したサーバーのインスタンスを返し、切断中も保持します。同じインスタンスと実行への再接続では `{ instance, run_id, after_stream_seq }` を送ってそのカーソル以後のログイベントだけを再送してもらい、サーバーのインスタンスが変わったときは新しい `hello` から取り直します。

### 実行の操作

| 種類 | 名前 |
| --- | --- |
| 型 | `ClientRun`（`starting`、`stopping`、`currentStopping`、`stopWhileStarting`、`error`、`start`、`stop`、`forceStop`） |
| 型 | `StopAction` |

`currentStopping` は現在のセッションまたは開始を対象とする応答待ちの停止だけを示し、`stopping` は前の実行の停止も含みます。停止と強制停止は、把握しているセッションとインスタンスを対象にします。確認画面は確認時の `RunTarget` を `front.run.forceStop(target)` に渡せます。対象が変わった場合や新しい開始が応答待ちの場合は断ります。このクライアントの開始中に停止を頼んだ場合は開始の応答を待ちます。応答が失われた場合は停止を送らず、その旨をエラーで知らせます。再試行する前に現在の実行を確認してください。対象付き停止に対応しない旧サーバーではエラーになり、同じクライアントからの重複した開始も断ります。

### 時間

| 種類 | 名前 |
| --- | --- |
| 型 | `ClientTime`（`state`、`answerTimeoutMs`、`pause`、`resume`、`step`、`runUntil?`、`setSpeed`、`send`） |
| 型 | `TimeState`、`TimeFailure`、`Lateness`、`SendResult`、`NotSentReason` |
| 定数 | `ANSWER_TIMEOUT_MS`（`5000`） |

`front.time` と `client.time` は、把握している実行 ID とサーバーの識別子をコマンドに付けます。実行が切り替わった場合、サーバーはランチャーに送る前にコマンドを拒否します。画面は操作ボタンを描画した時点の `RunTarget` を `front.time` に渡せ、その対象が現在の実行と一致しなければクライアントが操作を拒否します。

### ECU の変数の API

| 種類 | 名前 |
| --- | --- |
| 型 | `ClientVariables`（`state`、`operations`、`watchProblem`、`watch`、`enqueue`、`write`、`bind`） |
| 型 | `VariableWatch`（`values`、`value`、`history`、`close`） |
| 型 | `VariablesState`、`VariablesStatus`、`VariablePeer`、`VariableInfo`（`builtin` を含む）、`VariableRef`、`VariableValue`、`WatchOptions`、`WatchValues`、`VariableReading`、`HistoryPoint` |
| 型 | `QueuedOperation`（`session`、`seq`、`applied(options?)`）、`WriteOptions`（`instance`、`atMs`、`late`、`timeoutMs`、`keepalive`、`signal`）、`WriteWaitOptions`（`timeoutMs`、`signal`）、`OperationView`、`OperationPeer`、`WatchProblem` |
| クラス | `WriteError` |
| 定数 | `WATCH_PERIOD_MS`（`100`）、`WATCH_PERIODS_MS`、`WRITE_TIMEOUT_MS`（`10_000`）、`HISTORY_MAX`、`HISTORY_TOTAL_MAX`、`OPERATIONS_KEPT` |

### 変数とコンポーネントの接続（`bind`）

| 種類 | 名前 |
| --- | --- |
| メソッド | `client.variables.bind(ref, options)` |
| 型 | `BindOptions`（`periodMs`、`history`、`needsRange`、`needsValue`、`visible`） |
| 型 | `VariableBinding`（`ref`、`state`、`history()`、`write(value)`、`reject(message)`、`setVisible(visible)`、`close()`） |
| 型 | `BoundVariable`（`ref`、`info`、`reading`、`availability`、`condition`、`block`、`launcherReason`（`Reason`）、`writeStatus`、`pending`） |
| 型 | `ValueCondition`、`WriteBlock`、`WriteStatus`、`WriteValue` |
| 関数 | `isWriteError`、`hasRange` |
| 関数 | `valueCondition`（`ConditionInput`）、`writeBlock`（`WriteBlockInput`） |
| 定数 | `DROPPED_MESSAGE` |

`valueCondition`、`writeBlock`、`hasRange` は、`bind` が組み立てに使っている純粋な関数です。

### `Readable`（読み取り専用の値）

| 種類 | 名前 |
| --- | --- |
| 型 | `Readable<T>`（`get()`、`subscribe(fn)`） |

### 差し替え用の API

| 種類 | 名前 |
| --- | --- |
| 型 | `SocketFactory`、`SocketHandlers`、`SocketLike` |
| 型 | `HttpTransport`、`HttpRequest`、`HttpOutcome`、`FetchTransportOptions` |
| 関数 | `webSocketFactory`、`fetchTransport` |
| 定数 | `DEFAULT_TIMEOUT_MS` |

### エラー

| 種類 | 名前 |
| --- | --- |
| クラス | `HttpError`（`status`、`statusText`、`problem`、`timedOut`） |
| クラス | `ProtocolError` |
| 関数 | `errorMessage` |

### サーバーの API の型と読み取り

`gui/protocol` が決める形の型と、サーバーの API を自分で読むときに使う読み取り関数です。

| 対象 | 名前 |
| --- | --- |
| `gui/protocol` の形の型 | `SessionSummary`、`ServerMessage`、`LauncherEvent`、`CheckResult`、`RunResult`、`AppInfo` など |
| 定数と関数 | `PROTOCOL_VERSION`、`PHASES`、`severityRank` |
| 読み取り関数 | `readServerMessage`、`readAppInfo`、`readConfigList`、`readConfigText`、`readSavedConfig`、`readCheckResponse`、`readConnections`、`readDashboardResponse`、`readPrepareResponse`、`readRecording`、`readRunResponse`、`readRuntimeStatus`、`readScenarioList`、`readSessionAllowed`、`readSessionResponse`、`readSessionRuntime`、`readTimeResponse`、`readVariablesSummary`、`readWriteResponse` |
| 変数の API の形 | `VariablesSummary`、`VariableSummary`、`ValuesMessage`、`OperationReport`、`VariableAvailability`、`WatchMessage`、`WriteAnswer` |
| ダッシュボードのファイル（`dashboard(file)`） | `DashboardResponse`、`DashboardFile`、`DashboardFileSection`、`DashboardFileItem`、`DashboardFileError`、`DashboardPanelModule`、`DashboardVariable` |
| シナリオ（`--scenario`、`scenarios(file)`、`recording()`） | `ScenarioStep`、`ScenarioStepStatus`、`ScenarioStatus`、`ScenarioCallFrame`、`CheckScenario`、`ScenarioResult`、`SummaryScenario`（任意の `handed_over` を含む）、`ScenarioList`、`ScenarioFile`、`Recording`（`end_ms: number \| null` を含む） |
| リクエストの形 | `AfterScenario`、`CheckRequest`、`CheckTextRequest`、`DescribeRequest`、`PrepareRequest`、`SaveConfigRequest`、`RunRequest`、`ConnectionsRequest`、`Speed`、`StopRequest`、`StopSessionRequestBody`、`TimeCommand` |
| 設定ファイルの一覧 | `ConfigList`、`ConfigFile`、`ConfigText`、`SavedConfig` |
| エラーの形 | `ServerProblem` |
| 指摘 | `Finding`、`Severity` |
| ランチャーのキーと通信量 | `FromLauncher`、`LauncherFields`、`RunTraffic`、`TrafficNetwork`、`TrafficParticipant`、`TrafficUnplaced`、`ParticipantRole`、`OutsideState` |
| セッションのコピーの `SessionView.scenario` | `ScenarioView` |

### 自作画面の波形と変数編集

`createWaveforms` は選択順に各信号の履歴を返します。選択を変更しても共通の信号の履歴を保持し、実行が替わると履歴を消します。既定は信号ごとに 600 点です。点の描画と別タブ間の同期は画面で行います。

`createVariableEditor` は変数の定義、表示値、書き込みの結果を `state` に公開します。数値・真偽値・列挙値を定義に照らして検査します。真偽値は適用の通知を受けてから、読み戻しが届くまで最長 500 ms、次の操作を待ちます。`setVisible` で値の購読を切り替え、使い終えたら両方とも `close()` を呼びます。

```ts
import { connect, createVariableEditor, createWaveforms } from '@vehicle-sil/gui-client';

const front = await connect();
const ref = { peer: 'Engine', name: 'engine.throttle' };
const waves = createWaveforms(front.low, { periodMs: 20 });
waves.setSignals([ref]);
const editor = createVariableEditor(front, ref);
editor.setVisible(true);
// Render waves.series and editor.state. Call editor.write(value) on commit.
```

## 基本の使い方

`createClient()` は、`connect()` や `createFront()` の土台となる低水準の入口です（`front.low` がこの同じオブジェクトです）。未知のキーや旧 snake_case キー（`base_url` など）は実行時に `ActionError('unknown-field', ..., 'refused')` で拒否されます。

```ts
import { createClient, takeToken } from '@vehicle-sil/gui-client';

// サーバーが表示した URL（http://127.0.0.1:<port>/#token=<token>）から開いた画面: URL の #token= を受け取って消す。
// Node のツールでは、baseUrl とトークンをその URL から自分で取り出して渡す（リポジトリの examples/gui_client）。
const client = createClient({
  baseUrl: new URL('/', location.href).href,
  token: takeToken(window),
  batchMs: 100, // 0..1000 ms（既定 100）。0 は各ターンをマイクロタスクで即時フラッシュ
  watchPeriodMs: 100, // 20..60_000 ms（既定 100）。変数 watch の既定間隔
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

| 呼び出し | すること |
| --- | --- |
| `get()` | いまの値を返す |
| `subscribe(fn)` | 次に値が変わったときに `fn` を呼ぶ（いまの値では呼ばない） |
| 返る関数 | 購読をやめる |

- 通知は同期です。値を変えたコードの中で呼ばれます。
- 通知の途中で購読をやめた購読者にも、その回の通知は届きます。
- 購読者が同じ値を変えたときは、どの購読者にも新しい値が届き、古い値は後から届きません。
- フレームワークのラッパーは、`get()` で最初の値を取り、`subscribe()` で後の値を自分の状態に変えます（Angular の signal は「[フレームワークのプロジェクト](../../examples/gui_starter/README.ja.md#フレームワークのプロジェクト)」にあります。パッケージにフレームワークのコードはありません）。
- 購読者（`subscribe`、`onBatch`、`onMessages`、`onUnreadable` に渡した関数）が例外を投げても、ほかの購読者には通知され、ライブラリは動き続けます（拒否されたときの切断、作り直しなども最後まで行われます）。投げられた値は `onError`（既定は `console.error`）に渡ります。
- `client.session.ready` は、セッションのコピーがそろったか（接続して、`hello` と再送が届いたか）を示します。ほかの機能（値など）は自分の状態を持ちます。

### 拒否とタイムアウト

- トークンが拒否されると（サーバーを再起動したときなど）、`client.session.rejected` が真になり（WebSocket）、HTTP のリクエストは状態 401 の `HttpError` で拒否されます。
- 操作が拒否されると、その Promise は `HttpError` で reject し、同じ理由が状態（`client.run.error`、`client.time.state` の `failure`）に入ります。
- リクエストは最大 `DEFAULT_TIMEOUT_MS` 待ち、応答がなければ `timedOut` が真の `HttpError`（状態 0）で失敗します。
- 開始（`run.start()`）と照合（`check()`）は、照合の上限（`limits.check_timeout_s`）の 2 倍と `DEFAULT_TIMEOUT_MS` まで待ちます。サーバーは照合を 1 つずつ実行するため、順番待ちと照合の両方が上限まで掛かりえます。開始も、設定ファイルが変わっていれば照合し直します。
- タイムアウトは拒否ではなく、応答がなかっただけです。開始では、サーバー側で実行が始まっていることがあります。始まったかはセッションのコピー（WebSocket）に現れ、`client.run.error` にもその旨が出ます。
- リダイレクトは辿りません。

### ランチャーのキー（`raw`）

チェックの結果、実行の結果、指摘、ランチャーのイベントはランチャーが書いたもので、ライブラリはそのキーをすべて渡します。型のあるフィールド（例: 実行の結果の `traffic`、`participants[].log`、チェックの参加者の `role`）は、このライブラリの版でなくランチャーの形式の版（`vehicle_sil_check`、`vehicle_sil_run`、`vehicle_sil_events`）に従います。ランチャーから読んだオブジェクトには、どれにも `raw`（届いたままのランチャーのオブジェクト。型にないキーも含む）があります。`raw` は読み取り専用で列挙されません（`JSON.stringify` やスプレッドには入りません）。値は検査していない JSON です。型を確かめ、HTML ではなく文字列として出してください。

```ts
const result = client.session.session.get()?.result; // RunResult | null
for (const network of result?.traffic?.networks ?? []) console.log(network.name, network.delivered);
const strict = result?.raw?.['strict']; // 型のないキー: unknown
```

### 読めない通知の扱い

ライブラリは各部分を分けて読みます。

- 要約やイベントの一部が読めないときは、読める部分を使い、読めなかった部分の名前を `SessionView.unreadable` に入れます。
- 知らない通知とイベントは、状態を変えずに素通りし、`onMessages` にも渡りません（`client.connection.onBatch` には読む前の全部が届くので、ライブラリの知らないイベントはそこで読みます）。
- JSON として読めない通知が来たときは、お知らせを出して `hello` から受け直します。

### Node から使う、通信を差し替える

- Node では、ライブラリがサーバー自身の origin を `Origin` として送ります（`createClient` が `baseUrl` から作ります）。
- ブラウザと Web Worker では、`origin` を渡しません。ブラウザが付けます（Worker の WebSocket はハンドシェイクのヘッダーを付けられません）。
- HTTP の実装を差し替えるには `http`（`HttpTransport`）、WebSocket を差し替えるには `socket`（`SocketFactory`）を渡します。

ライブラリが代行しないルール（照合と開始のリクエストのフィールド、変数に書き込めるか、値が古いか）は、[docs/ja/gui.md](gui.md#画面を作るときのルール) の「画面を作るときのルール」にあります。

## ECU の変数

```ts
const watch = client.variables.watch([{ peer: 'Engine', name: 'engine.speed' }], {
  periodMs: 100,
  history: 600,
});
watch.values.subscribe((values) => console.log(values['Engine']?.['engine.speed']));
try {
  // 2 段階書き込み: enqueue() で ECU キュー到達を確認し、applied() でステップ反映を待つ。
  const queued = await client.variables.enqueue({ Engine: { 'engine.throttle': 50 } });
  const operation = await queued.applied();
  console.log(operation.peers['Engine'].appliedAtMs);
} catch (error) {
  if (error instanceof WriteError) console.log(error.kind, error.code, error.message);
}
watch.close();
```

### 一覧と状態（`client.variables.state`）

| `status` | 意味 |
| --- | --- |
| `unknown` | まだ聞いていない |
| `off` | この実行に変数はない |
| `connecting` | 接続している途中 |
| `ready` | 読み書きできる |
| `failed` | 接続できなかった、拒否された |
| `ended` | 実行が終わった（値は最後のもの） |

- `off`、`connecting`、`failed` の理由は `reason`（サーバーの英語の文）です。
- 実行を始める前に使えるかは、`client.app` の `variables`（`available`、`reason`）でわかります。
- 変数の定義は `peers[].variables[]`（`type`、`role`、`writable`、`builtin`、`unit`、`min`、`max`、`step`、`description`、`enum`）です。`builtin: true` は vehicle-sil が自動追加した変数（`ecu.connected`、`peer.*`、`j1939.*`、`can.*`、`lin.*`、`eth.*`）を示し、`check().participants[].variables[].builtin` と一致します。

### 値を読む（`watch`）

`watch` は、変数（`'all'` は全部）の最後の値を受け取ります。使い終わったら必ず `close()` します。

**値**: `values`（`Readable`）は、`values[peer]?.[name]` が `VariableReading`（`value`、`sampleMs`、`simMs`、`paused`）です。値ごとに「いつのものか」を持ちます。

| `paused` | 値の意味 |
| --- | --- |
| 真 | 止まっている時刻 `simMs` の値（その時刻より前に始まったステップの値は入っている） |
| 偽 | 動いている途中の最後の値 |

- `watch.values` は、見ている変数の読み取り値が変わるたびに通知します（値自体が変わらなくても `simMs` や `paused` が更新されたときを含みます）。`peers: {}` のフレーム（`start_paused` でステップ 0 の前など）では `watch.values` は通知しません。
- 最初のサンプルより前に一時停止や仮想時間の状態を知るには `isHolding(session)` や `front.clock` を使い、`peers: {}` を含む生の `ValuesMessage` フレームを受け取るには `client.session.onMessages` を購読します。

**購読**:

- 同じ変数を見る watch がいくつあっても、サーバーへの購読は変数ごとに 1 つです。変数は、頼まれた中でいちばん短い間隔の購読に入ります（間隔は `WATCH_PERIODS_MS` の 20、50、100、200、500 ms … の段へ切り下げ、サーバーの `limits.watch_period_ms_min..watch_period_ms_max` に収めます）。
- `WatchOptions.periodMs` を省略すると、`ClientOptions.watchPeriodMs`（既定 `WATCH_PERIOD_MS` = 100 ms）が使われます。
- `close()` すると、ほかに見ている watch がなければサーバーの購読をやめ、残りの購読者がいればその中の最短間隔へ戻します。
- 再接続したら、自動で購読し直します。
- 実行が替わっても watch は続き、前の実行の値と履歴は捨てます。
- 値を覚えるのは、だれかが見ている変数だけです。
- 実時間の最新値ポーリング（20 ms を含む）は、ポーリング間隔より短いパルスの捕捉を保証しません。[docs/ja/gui.md](gui.md#ライブポーリングと決定論的記録の使い分け) の「ライブポーリングと決定論的記録の使い分け」を参照してください。

**履歴**（`history` を頼んだとき）:

- 変数ごとに、その数までの点（`sampleMs`、`value`）を環状バッファーに持ちます。変数ごとの上限は `HISTORY_MAX` で、古いものから捨てます。
- メモリーは点が届いてから確保します（頼んだだけでは確保しません）。
- 全部の変数の履歴を合わせて `HISTORY_TOTAL_MAX` 点（約 34 MB）までです。そこに届くと、どの履歴もそれ以上は伸びず、そのときの大きさのまま古い点を捨てます（`history(ref)` が返す点が、頼んだ数より少なくなります）。
- 同じステップの値は 1 点です。同じステップの値が変わって届いたら、その点を置き換えます。
- 実行が替わると、履歴は空から始まります。
- `watch.history(ref)` は、呼ぶたびに新しい配列を返します。
- 受信した `values` 通知は `ClientOptions.batchMs`（既定 `BATCH_MS` = 100 ms、`0` はマイクロタスク）ごとにまとめて届くため、`values` の通知はその頻度になりますが、履歴には間に届いた異なるステップの点もすべて保持されます。

**上限を超えそうなとき**:

- 長い名前を多く挙げて、購読の 1 通がサーバーの上限（256 KiB）を超えそうなときは、その間隔の購読を「全部」に替えます（受け取った中から、見ている変数だけを使います）。
- サーバーが watch を拒否したとき（上限の違うサーバー）は、`client.variables.watchProblem` に出ます。

### 値を書き込む（`enqueue` と `write`）

変数の書き込みは、ネイティブ Promise を返す 2 つのメソッドに分かれた 4 段階契約に従います。

1. **段階 1 + 2（`enqueue(writes, options?) -> Promise<QueuedOperation>`）**: 呼んだ時点の実行（`client.variables.state` の `session`、`run_id`）へ `POST api/write` を送ります。サーバーが変数名・型・範囲を確認して FIFO の `seq` 順で登録し、全対象 ECU が `sent` を抜ける（`queued` または即時確定）まで gRPC `WaitOperation(WAIT_FOR_QUEUED)` を待ちます。ECU キュー到達が確認されると `{ session, seq, applied(waitOptions?) }` で解決し、拒否や即時終端失敗（`invalid`、`busy`、`late`、`rejected`、`cancelled`、`unknown`、`mixed`）はその場で `WriteError` で reject します。
2. **段階 3 + 4（`queued.applied(waitOptions?) -> Promise<OperationView>`）**: 全対象 ECU が仮想時間のステップで反映（`applied`）すると解決し、それ以外は `WriteError` で reject します。
3. **一括の糖衣（`write(writes, options?) -> Promise<OperationView>`）**: `enqueue(writes, options).then((q) => q.applied({ timeoutMs, signal }))` と同じです。

- サーバーは、その実行がもう動いていなければ拒否します（`session-changed`）。そのため、実行が替わる間に送った書き込みが次の実行に書かれることはありません。
- 実行がまだわからないときは、送らずに `refused`（`not-running`）で拒否します。
- 未知のオプションキーは、その場で `WriteError('refused', ..., 'unknown-field')` で拒否されます。
- 同じ書き込みを自動で送り直すことはありません。

`WriteError` の `kind` は次のとおりです。

| `kind` | 意味 | 送り直してよいか |
| --- | --- | --- |
| `refused` | サーバーまたはクライアントがランチャーへ渡す前に拒否した | 原因を直してから、よい |
| `failed` | 受け付けたが反映されなかった（`invalid`、`busy`、`late`、`rejected`、`cancelled`、`unknown`、`mixed`） | どの対象にも反映していなければよい（`mixed` や `unknown` は `operation.peers` を確認） |
| `no-answer` | **反映されたかどうかわからない** | **そのままは送り直さない**（下） |
| `closed` | 結果の前に `close()` した | 同上 |

- `refused` の `code` は、`unknown-variable`、`not-writable`、`invalid-field`、`unknown-field`、`invalid-argument`、`not-running`、`session-changed`、`variables-not-ready`、`write-busy`、`aborted` などです。
- `failed` の `operation` には、participant ごとの状態と理由（`late`、`rejected`、`invalid` など）が入ります。
- `no-answer` になるのは、次のどれかのときです。
  - ウォッチドッグのタイマーが満了した（`operation` にわかっている最後の状態）。
  - `signal` で中断された（`code: 'aborted'`）。
  - HTTP のレスポンスがなかった、またはサーバーが途中で失敗した（500）。
  - `seq` 発番後に ECU キュー到達の待機がタイムアウトした（`504 write-timeout`、`detail: { operation, state: 'sent' }`）、または Control Server との接続が途中で切れた（`502 write-failed`）。
  - レスポンスの前に実行が替わった。
  - 再接続している間に確定して結果が失われた（下の「再接続」）。
  - サーバーが未確定の操作を覚えきれずに追うのをやめた。

**仮想時間連動ウォッチドッグ（`timeoutMs`）**:

`timeoutMs`（既定 `WRITE_TIMEOUT_MS` = 10,000 ms）は `0` より大きい有限の数値である必要があります（`Infinity` や `<= 0` は `invalid-argument` で拒否され、2^31-1 ms より大きい値はそこに丸めます）。ウォッチドッグは接続健全性・実行状態・仮想時間に応じて 4 モードで動作します。

| モード | 適用される状態 | 振る舞い |
| --- | --- | --- |
| `armed-unhealthy` | WebSocket 未同期・切断、`variables.status !== 'ready'`、または未完了の対象 ECU が切断 | 固定 `timeoutMs` で計測し `no-answer` で reject（回線断や ECU 異常終了での永久待機を防止） |
| `suspended` | 健全かつ全未完了対象 ECU が接続中で、`paused` または `holding` | タイマーを一時停止（マルチレート構成で次ステップ境界を待つ間も含む）。一時停止中の実時間上限が必要なら `signal: AbortSignal.timeout(ms)` を渡す |
| `liveness` | 健全・接続中・実行中（`!paused && !holding`）で適用予定時刻の通過前 | 低速再生（`speed < 1`）に合わせて実効予算を拡大し、`simMs` が厳密に進むたびにリセット。仮想時間が止まれば（ECU デッドロック等）確実に reject |
| `armed-due` | 健全・接続中・実行中で、仮想時間が予定ステップを通過したのに結果が来ない | リセットなしの固定実効タイマーで異常を検出して reject |

**二重の書き込みを避ける**: `no-answer` の後は、送り直す前に確認します。

- `error.operation` があれば、その番号の操作を `client.variables.operations` で追います。`queued` なら、シミュレーションがそのステップに進めば反映されます。
- 番号がなければ、`operations` に自分の書き込みらしい操作が増えていないかと、その変数の値（`watch`）を見ます。
- 反映されていないとわかってから送り直します。
- 値を足し込むような（同じ書き込みを 2 回すると結果が変わる）使い方では、特に守ります。同じ値を書くだけなら、2 回書いても結果は同じです。

**再接続**: 結果を待つ間に接続が切断されても、再接続した後の要約（`hello`）で続きがわかります。要約には、サーバーが覚えている未確定の操作の全部と、番号が `operations_from_seq` 以上の操作の全部が入っています（サーバーが受け取るもののうち。シナリオ自身の書き込みの操作は届きません）。

| 自分の操作 | ライブラリの扱い |
| --- | --- |
| 一覧にある | その状態を使う（未確定なら、そのまま待つ） |
| 一覧になく、番号が `operations_from_seq` より小さい | 切断されている間に確定して結果が失われたとして、`no-answer` |
| 一覧になく、サーバーが未確定の操作を覚えきれなかった | 古い操作を `no-answer` にする |

- まだ反映されていない書き込み（止まっている間の書き込みなど）は、後からいくつ操作が続いても `no-answer` になりません。
- サーバーが覚えきれなかったとは、未確定の操作が 256 件を超えたときです（要約の `operations_open_omitted`）。サーバーがもう追っていないので `no-answer` にし、エラーの文でそうとわかります。

**その他のオプションと状態**:

- 書き込みは、指定した仮想時間（`atMs`。省くと次のステップ）のステップで反映されるので、止まっている間は確定しません。`enqueue()` が ECU キュー到達を待つため、`await client.variables.enqueue(...)` → `step()` / `runUntil()` → `await queued.applied()` の順で `sleep()` なしに決定論的に書けます。
- `keepalive`（`WriteOptions.keepalive`、`HttpRequest.keepalive` を通じて `fetch(..., { keepalive })` へ転送）を使うと、ブラウザの `pagehide` ハンドラから送る解除書き込みに `Authorization` と `Origin` ヘッダーを付けたままページ離脱後も完了させられます。[docs/ja/gui.md](gui.md#保持入力切断復帰バス観測) の「保持入力・切断復帰・バス観測」を参照してください。
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

| フィールド | 中身 |
| --- | --- |
| `info` | 変数の定義（いまの実行になければ `undefined`） |
| `reading` | 最後の値（`VariableReading`。まだないとき、`visible` が偽のときは `undefined`） |
| `availability` | ECU がいまその変数を使っているか（`unknown`、`active`、`inactive`） |
| `condition` | 値の状態（下の「[値の状態（`condition`）](#値の状態condition)」） |
| `block` | いま書けない理由（下の「[書けない理由（`block`）](#書けない理由block)」） |
| `launcherReason` | `block` が `launcher` のとき、ランチャーの理由（`code` と英語の `message`。`launcherAnswer` が読んだまま）。ほかは `null` |
| `writeStatus` | 最後の書き込みの状態（下の「[書き込みの状態（`writeStatus`、`pending`）](#書き込みの状態writestatuspending)」） |
| `pending` | 送っていて、結果がまだの値（なければ `null`） |

- `subscribe` は変わったときだけ呼ばれます（最初の状態は `state.get()`）。
- `get()` は、いつもいまの状態から作ります。そのため、ほかの `Readable`（`client.variables.state` など）の通知の途中で `get()` を呼ぶと、その場で、この `state` の購読者に新しい状態を通知することがあります。

### 値の状態（`condition`）

`condition.kind` で分けます。

| `kind` | 意味 |
| --- | --- |
| `unavailable` | 変数の機能が使えない（`status` に `client.variables.state` の状態） |
| `missing` | いまの実行にない変数 |
| `waiting` | いまの実行にあるが、値がまだ届いていない |
| `live` | 動いている途中の最後の値 |
| `paused` | 止まっている時刻の値 |
| `stale` | 古いかもしれない値（理由は `why`） |
| `ended` | 終わった実行の、最後に読めた値 |

`unavailable` の `status` は、実行していない、この実行に変数はない、接続している途中、接続できない、のどれかです。`stale` の `why` は次のとおりです。

| `why` | 意味 |
| --- | --- |
| `connection` | クライアントとサーバーの接続が切断された、または同じ実行への再接続後、キャッシュされた値に最初の `values` サンプルがまだ届いていない |
| `reconnecting` | サーバーがランチャーに再接続している |
| `peer` | ECU が切断された |
| `reading` | 値の読み取りが失敗し続けている |

### 書けない理由（`block`）

書けるなら `null` です。複数に当てはまるときは、表で上にあるものになります。

変数と実行の事実が先、次にランチャーの答え（`launcher`）、次に client の予測（`parameter`）、最後に部品の要るものです。ランチャーから来るのは `launcher` だけで、ほかの値は client 自身のものです。ランチャーが何を受けるかについて、client は自分の規則を持ちません。シナリオの実行中でもランチャーが書き込みを受けるなら、`bind` は書かせます。ランチャーの答えは、正面の口の `controls` と同じく `launcherAnswer(launcherState, 'write')` で読みます。

| `block` | 意味 |
| --- | --- |
| `read-only` | 読むだけの変数（`role` が `output`） |
| `disconnected` | クライアントとサーバーの接続が切断されている |
| `ended` | 実行が終わった |
| `not-ready` | 変数の機能が使えない（実行していない、接続している途中、接続できない） |
| `missing` | いまの実行にない変数 |
| `peer-disconnected` | その ECU が Control Server と接続していない |
| `launcher` | ランチャーがいまは書き込みを受けない（その `state` の `allowed.write`。`launcherReason` に code と文がある。たとえば、シナリオの実行中は `scenario-running`）か、受けるとまだ言っていない（`allowed.write` のある `state` が届くまでは `unsupported`） |
| `parameter` | `parameter` は、止めて始めた実行の最初のステップの前（`isHolding(session)`: ランチャーの `state` の `holding`）だけ書ける。ECU の規則の client による予測（ランチャーは変数ごとには言わない） |
| `no-range` | `needsRange` を付けたのに、変数に範囲（`min` と `max`）がない |
| `no-value` | `needsValue` を付けていて、値（読んだ値か送った値）がまだない |

### 書き込みの状態（`writeStatus`、`pending`）

`writeStatus` は、最後の書き込みの状態です。

| `writeStatus` | 意味 |
| --- | --- |
| `idle` | 空（最後の書き込みがない） |
| `sending` | 送った（`enqueue()` / ECU キュー到達を待機中） |
| `queued` | ECU のキューに到達した（`seq` は操作の番号） |
| `applied` | 反映された（`atMs`） |
| `invalid` | `reject(message)` に渡した文（送っていない） |
| `dropped` | 覚えていた値を送らなかった（下の「[`write(value)` の動き](#writevalue-の動き)」） |
| `refused`、`failed`、`no-answer`、`closed` | `WriteError` の `kind`（`message` と `code` を持つ） |

- `isWriteError(writeStatus)` は、`message` を持つもの（うまくいかなかったもの）を見分けます。
- `message` は人向けの英語で文言が変わることがあります（ローカライズされていません）。分岐や画面の文言の組み立てには `kind` と `code` を使ってください。
- `pending` があれば、読んだ値の代わりに表示します（押したスイッチが、結果を待つ間に元へ戻って見えません）。

### `write(value)` の動き

- 書けないとき（`block`）は何もしません。
- Promise は返さず、結果は `writeStatus` に出ます。
- 内部では `enqueue()` に続けて仮想時間連動ウォッチドッグ付き `applied()` を使うため、一時停止中の書き込みは進めるまで `queued` のまま待機し、接続断や ECU 異常終了では有限時間でタイムアウトします。
- 前の書き込みがまだ `sending`（`enqueue()` 応答待ち）の間の次の値は、送らずに最後の 1 つだけを覚えておき、前の書き込みが `queued` に到達したら送ります（スライダーやキーの連打で書き込みが積み重なりません）。
- 覚えた値を送るときは、実行が同じで、まだ書けるかを確認し直し、だめなら送らずに `dropped` にします。
- 前の書き込みがキュー到達前に拒否されたら、覚えた値も送りません。
- 結果が来なかった書き込み（`no-answer`）を、自動で送り直すことはしません。
- `reject(message)` は、送る前にコンポーネントが見つけたエラー（数でない文字、範囲の外）を `invalid` として出します。送っている書き込み、覚えている値、`pending` には触れません。

### 変数ごとに 1 つの状態

- 同じ変数を何か所で `bind` しても、`writeStatus` と `pending` は同じものが見え、書き込みは重なりません。
- `bind` を閉じて作り直しても残ります。
- 実行ごとなので、実行が替わると空（`idle`、`null`）から始まり、前の実行の書き込みの結果は出ません。

### 見えている間だけ watch する

`bind` は、作ったときから値を受け取ります（`watch` と同じく、サーバーへの購読は変数ごとに 1 つ）。コンポーネントが多い画面では、`{ visible: false }` で作り、見えたとき（`IntersectionObserver` など）に `setVisible(true)`、隠れたら `setVisible(false)` を呼びます。偽の間は、次のようになります。

| フィールド | `visible` が偽の間 |
| --- | --- |
| `reading` | `undefined` |
| `condition` | `waiting` |
| `writeStatus`、`pending` | 変わらず出る |
| `block` | 変わらず出る（`needsValue` を付けた `bind` は、`pending` もなければ `no-value`） |

### 履歴

`{ history: 600 }` で作ると、`history()` が点（古い順）を返します。点が増えるたびに `state` が通知するので、そこで読み直します。

### 確認しないこと、`close()` の後

- `write(value)` は、値の型と範囲を確認せずに送ります。合わない値は、サーバーが拒否して `writeStatus` が `refused` になります。先に確認して文を出したいときは、コンポーネントで確認して `reject(message)` を呼びます。
- `close()` の後の `write`、`reject`、`setVisible` は何もしません。
- `close()` しても、すでに送った書き込みと覚えた値は取り消しません。覚えた値は、前の書き込みが `queued` に到達したら送られます（状態は変数ごとに持つので、同じ変数を次に `bind` すると見えます）。

## ライフサイクル

| 呼び出し | すること |
| --- | --- |
| `createClient()` | 作るだけで、接続しない |
| `connect()` | WebSocket で接続する（トークンがないときは `token: ''` を送る。`--no-auth` 用で、認証が有効なサーバーでは `unauthorized` で拒否される） |
| 接続している間の `connect()` | 何もしない（再接続を待っている間も） |
| `close()` | 切断し、再接続しない（片づけるものは下） |
| `close()` の後の `connect()` | また接続し、残っている watch を購読し直す |
| `close()` の後の HTTP のリクエスト | 使える（`loadApp()`、`run.start()` などは、接続していなくても送れる） |
| 拒否された・バージョンが違った後の `connect()` | 何もしない（`session.rejected`、`session.incompatible` は戻らない） |

- `close()` は、待っているタイマーを止め、時間のコマンドのレスポンスを待つのをやめ、結果を待っている書き込みを `WriteError`（`closed`）で reject します。まとめている途中の通知は捨てます。変数の watch は残ります。
- `close()` の後に `connect()` すると、セッションのコピーは残っていて、持っている分の続きから受け直します。
- 別のトークンやバージョンで接続するときは、クライアントを作り直します。

## コーディングのルール

- 自分のファイルのほかは import しない（`eslint.config.js` が止める。動的な `import()` も、テスト用のヘルパーのほかでは止める。`tsconfig.json` の `rootDir` で、`src/` の外も止まる）。
- ブラウザにしかないもの（`window`、`document`、`localStorage` など）を使わない。必要なものは設定（`ClientOptions`）で受け取る。
- 人に見せる文（`TimeFailure.message` など）は英語。画面の言葉にするのはラッパーの役目（`kind` などの機械が読むフィールドから作る）。
- 公開 API を追加したら、`src/index.ts` と上の「[公開 API](#公開-api)」の表に追加し、[gui/client/README.ja.md](../../gui/client/README.ja.md) と [ブラウザの GUI に自分の画面を作る](howto/gui-screen.md) を合わせる。

## テスト

ツール（TypeScript、ESLint、Prettier、Vitest、esbuild）は `gui/web` の `node_modules` のものを使います。`gui/web` で次を実行します（`scripts/verify-web.py` も実行します）。

| コマンド | 内容 |
| --- | --- |
| `npm run test:client` | 型の検査（`tsc -p ../client`）と単体テスト（Vitest。ライブラリは Node の上で Angular なしに動く。`src/elements` の部品はヘッドレスの Chromium で動く） |
| `npm run build:client` | 1 つの ESM のファイルにまとめ、部品はそれを import する 2 つ目のファイルにまとめる（esbuild） |
| `npm run pack:client` | `@vehicle-sil/gui-client` の `.tgz` を作成し（`gui/client/pack.mjs`）、Node からの import と `nodenext`/`bundler` の TypeScript 例を検査する |
| `npm run verify:consumers` | 実 `.tgz` を隔離展開し、Vanilla TS・Angular・React 18/19・Vue 3・Svelte 5 の型検査（`@ts-expect-error` 負例含む）と Chromium 実マウントスモークを実行する |
| `npm run test:timeline` | `<vehicle-sil-timeline>` バンドルの同期とビューアー単体テストを検証する |
| `npm run lint` | `gui/web`、`gui/client`、`gui/timeline` の lint |
| `npm run format:check` | `gui/web`、`gui/client`、`gui/timeline` の書式検査（`gui/.prettierrc`） |

部品の試験には Playwright の Chromium が要ります（`gui/web` で `npx playwright install --only-shell chromium`。起動できるかは `scripts/doctor.py` が答えます）。ヘッドレスで動きます。画面で見るには、`gui/web` で `npx vitest --config ../client/vitest.config.mjs --project browser --browser.headless=false` を実行します。

単体テストは、サーバーの通知を作らずに `gui/protocol/examples/` の例を流します（`testing/protocol-examples.ts`）。

## 関連するドキュメント

| 知りたいこと | ドキュメント |
| --- | --- |
| 入門、正面の口 `connect()`、レシピ、11 部品、5 フレームワーク設定 | [gui/client/README.ja.md](../../gui/client/README.ja.md) |
| GUI に自分のコードを足す道（カスタムパネル、自分の画面、ツール）の使い分け | [docs/ja/gui.md](gui.md#自分の画面を作る) の「自分の画面を作る」 |
| 自分の画面を作り、GUI サーバーに配信させる手順 | [ブラウザの GUI に自分の画面を作る](howto/gui-screen.md) |
| サーバーの API（HTTP と WebSocket の形、バージョンと互換のルール、認証） | [gui/protocol/README.ja.md](../../gui/protocol/README.ja.md) |
| ライブラリが肩代わりしないルール | [docs/ja/gui.md](gui.md#画面を作るときのルール) の「画面を作るときのルール」 |
| 自分の画面の出発点にするファイル | 画面のテンプレート [examples/gui_starter](../../examples/gui_starter/) |
| 画面を使わずに Node から使う例 | [examples/gui_client](../../examples/gui_client/) |
