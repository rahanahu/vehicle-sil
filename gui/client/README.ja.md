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
| スライダーやランプを作る（`condition`、`block` の値の一覧） | [コンポーネントを作る（`bind`）](#コンポーネントを作るbind)      |
| `connect()` と `close()` の後に何が起きるか                 | [ライフサイクル](#ライフサイクル)                                |
| ライブラリを開発する                                        | [コーディングのルール](#コーディングのルール)、[テスト](#テスト) |

## 入手とインポート

ライブラリは TypeScript のソースで、依存はありません。npm のレジストリには公開していません。

| 使い方                                                | 入手                                                                                                              | import の書き方                                               |
| ----------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------- |
| ビルドなしの素の JavaScript                           | 1 つの ESM のファイルにまとめた `vehicle-sil-gui-client.mjs` を、画面のディレクトリに置く                         | `import { createClient } from './vehicle-sil-gui-client.mjs'` |
| TypeScript と好きなフレームワーク                     | パッケージ `vehicle-sil-gui-client-<version>.tgz` を入手し、自分のプロジェクトで `npm install <that .tgz>` をする | `import { createClient } from '@vehicle-sil/gui-client'`      |
| このリポジトリの中の TypeScript のアプリ（`gui/web`） | `tsconfig.json` の `paths` で、エントリーポイントの名前を `src/` に向ける                                         | `import { createClient } from '@vehicle-sil/gui-client'`      |

2 つのファイルは、次の場所にあります。

| ファイル                               | ビルド済みのツール                                    | リポジトリでビルドする（`gui/web` で実行）                                              |
| -------------------------------------- | ----------------------------------------------------- | --------------------------------------------------------------------------------------- |
| `vehicle-sil-gui-client.mjs`           | `<tools>/client/vehicle-sil-gui-client.mjs`           | `npm run build:client` が `build/gui-client/vehicle-sil-gui-client.mjs` を作る          |
| `vehicle-sil-gui-client-<version>.tgz` | `<tools>/client/vehicle-sil-gui-client-<version>.tgz` | `npm run pack:client` が `build/gui-client/vehicle-sil-gui-client-<version>.tgz` を作る |

同じ `.tgz` は、リリースのページにもあります。`build/` はリポジトリの直下のもので、Git では追跡しません。

- **1 つにまとめたファイル**: ブラウザでも Node（22 以上）でも読めます。`scripts/verify-gui.py` が、このファイルを Node から使う例を、記録を再生するランチャーを相手に動かして確認します。
- **パッケージ**: 中身は、1 つにまとめた ESM、`.d.ts`、`exports`、README、LICENSE です（作り方は `pack.mjs`）。バージョンは GUI サーバーと同じです。型はブラウザの型（`lib` の `DOM`）を使います。エントリーポイントは `.` と `./timeline` です（`testing` と `internal` は入りません）。

## エントリーポイント

| エントリーポイント                                                                                               | 名前                               | 中身                                                                                                                                                                    |
| ---------------------------------------------------------------------------------------------------------------- | ---------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/index.ts`                                                                                                   | `@vehicle-sil/gui-client`          | 公開 API（下の「公開 API」）。互換を保証するのはここだけ                                                                                                                |
| `src/testing/index.ts`                                                                                           | `@vehicle-sil/gui-client/testing`  | テスト用のヘルパー（自分のコンポーネントのテストに使える）                                                                                                              |
| `src/internal.ts`                                                                                                | `@vehicle-sil/gui-client/internal` | **互換の保証の外**。内部のクラスと関数で、アプリでは使わない                                                                                                            |
| `../timeline/vehicle-sil-timeline.js`（パッケージでは `export {}` を足した `timeline/vehicle-sil-timeline.mjs`） | `@vehicle-sil/gui-client/timeline` | 要素 `<vehicle-sil-timeline>` と型。import すると要素を定義します（[docs/ja/gui.md](../../docs/ja/gui.md#自分の画面に時間の軸を置く) の「自分の画面に時間の軸を置く」） |

- `testing` にあるのは、偽の WebSocket（`FakeWebSockets`）、偽の HTTP（`FakeHttp`）、`gui/protocol/examples/` の読み込み、要約から `SessionView` を作る `sessionViewFrom` です。
- `internal` にあるのは、内部のクラス、要約を反映する関数、読み取りの関数です。テストのために公開していて、予告なく変わり、消えます。

公開 API は狭く始めます（追加は互換を保ち、削除は互換を壊すため）。必要なものが公開のエントリーポイントにないときは、`internal` に頼らず、追加するかを決めます。

## 公開 API

`src/index.ts` が公開する名前を、グループごとに挙げます。括弧の中は、そのオブジェクトのメンバーです。

### クライアント

| 種類 | 名前                                                                                                                                                                                                                    |
| ---- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 関数 | `createClient(options)`                                                                                                                                                                                                 |
| 型   | `ClientOptions`（`baseUrl`、`token`、`origin`、`http`、`socket`、`answerTimeoutMs`、`onError`）                                                                                                                         |
| 型   | `GuiClient`（`connection`、`session`、`run`、`time`、`variables`、`app`、`limits`、`connect()`、`close()`、`loadApp()`、`configs()`、`check()`、`fetchSession()`、`dashboard(file)`、`scenarios(file)`、`recording()`） |
| 定数 | `DEFAULT_LIMITS`                                                                                                                                                                                                        |

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

| 種類 | 名前                                                                                                                                 |
| ---- | ------------------------------------------------------------------------------------------------------------------------------------ |
| 型   | `ClientSession`（`session`、`ready`、`lastEventAt`、`notices`、`rejected`、`incompatible`、`onMessages`、`report`、`dismissNotice`） |
| 型   | `Notice`、`SessionView`、`ParticipantView`、`LauncherView`、`LogLine`、`LogSource`、`TimeView`、`CommandView`、`RunOutcome`          |
| 関数 | `runOutcome`、`phaseIndex`                                                                                                           |

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
| 定数 | `ANSWER_TIMEOUT_MS`、`FEATURE_SPEED`、`FEATURE_START_PAUSED`、`FEATURE_TIME_COMMANDS`     |

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

| 種類     | 名前                                                                                                                  |
| -------- | --------------------------------------------------------------------------------------------------------------------- |
| メソッド | `client.variables.bind(ref, options)`                                                                                 |
| 型       | `BindOptions`（`periodMs`、`history`、`needsRange`、`needsValue`、`visible`）                                         |
| 型       | `VariableBinding`（`ref`、`state`、`history()`、`write(value)`、`reject(message)`、`setVisible(visible)`、`close()`） |
| 型       | `BoundVariable`（`ref`、`info`、`reading`、`condition`、`block`、`writeStatus`、`pending`）                           |
| 型       | `ValueCondition`、`WriteBlock`、`WriteStatus`、`WriteValue`                                                           |
| 関数     | `isWriteError`、`hasRange`                                                                                            |
| 関数     | `valueCondition`（`ConditionInput`）、`writeBlock`（`WriteBlockInput`）                                               |
| 定数     | `DROPPED_MESSAGE`                                                                                                     |

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

| 対象                                                       | 名前                                                                                                                                                                                                                            |
| ---------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `gui/protocol` の形の型                                    | `SessionSummary`、`ServerMessage`、`LauncherEvent`、`CheckResult`、`RunResult`、`AppInfo` など                                                                                                                                  |
| 定数と関数                                                 | `PROTOCOL_VERSION`、`PHASES`、`severityRank`                                                                                                                                                                                    |
| 読み取り関数                                               | `readServerMessage`、`readAppInfo`、`readConfigList`、`readCheckResponse`、`readSessionResponse`、`readTimeResponse`、`readWriteResponse`、`readVariablesSummary`、`readDashboardResponse`、`readScenarioList`、`readRecording` |
| 変数の API の形                                            | `VariablesSummary`、`VariableSummary`、`ValuesMessage`、`OperationReport`、`WatchMessage`、`WriteAnswer`                                                                                                                        |
| ダッシュボードのファイル（`dashboard(file)`）              | `DashboardResponse`、`DashboardFile`、`DashboardFileSection`、`DashboardFileItem`、`DashboardFileError`、`DashboardPanelModule`、`DashboardVariable`                                                                            |
| シナリオ（`--scenario`、`scenarios(file)`、`recording()`） | `ScenarioStep`、`ScenarioStepStatus`、`ScenarioStatus`、`CheckScenario`、`ScenarioResult`、`SummaryScenario`、`ScenarioList`、`ScenarioFile`、`Recording`、`FEATURE_SCENARIO`、`FEATURE_SCENARIO_CONTROL`                       |
| リクエストの形                                             | `CheckRequest`、`RunRequest`、`Speed`、`StopRequest`、`TimeCommand`                                                                                                                                                             |
| 設定ファイルの一覧                                         | `ConfigList`、`ConfigFile`                                                                                                                                                                                                      |
| エラーの形                                                 | `ServerProblem`                                                                                                                                                                                                                 |
| 指摘                                                       | `Finding`、`Severity`                                                                                                                                                                                                           |
| ランチャーのキーと通信量                                   | `FromLauncher`、`LauncherFields`、`RunTraffic`、`TrafficNetwork`、`TrafficParticipant`、`TrafficUnplaced`、`ParticipantRole`                                                                                                    |
| 自分で起動する participant（`manual`）                     | `FEATURE_MANUAL`                                                                                                                                                                                                                |
| セッションのコピーの `SessionView.scenario`                | `ScenarioView`                                                                                                                                                                                                                  |

## 基本の使い方

```ts
import { createClient, takeToken } from '@vehicle-sil/gui-client';

// サーバーが表示した URL（http://127.0.0.1:<port>/#token=<token>）から開いた画面: URL の #token= を受け取って消す。
// Node のツールでは、baseUrl とトークンを、その URL から自分で取り出して渡す（examples/gui_client）。
const client = createClient({
  baseUrl: new URL('/', location.href).href,
  token: takeToken(window),
});
await client.loadApp(); // ランチャーの features（時間のコマンドを持つか、など）
client.connect();
const stop = client.session.session.subscribe((session) => {
  if (session?.result) console.log(session.result.result);
});
await client.run.start({ file: 'simulation.yaml', disable: [], speed: null, start_paused: false });
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

ライブラリが肩代わりしないルール（ランチャーの `features` に合わせたリクエストのフィールド、変数を書けるか、値が古いか）は、[docs/gui.md](../../docs/ja/gui.md) の「画面を作るときのルール」にあります。

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

**再接続**: 結果を待つ間に接続が切断されても、再接続した後の要約（`hello`、`fetchSession()`）で続きがわかります。要約には、サーバーが覚えている未確定の操作の全部と、番号が `operations_from_seq` 以上の操作の全部が入っています。

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
- 最近の操作（ほかのクライアントの書き込みも）は `client.variables.operations` で読めます。

### 最初のステップの前に書く

止めて始めた実行（`start_paused`）は、変数の読み書きができるランチャーでは、最初のステップの前で待ちます。その間は `client.time.state` の `beforeStart` が真で、再開とステップ実行を押せます（書いた値は時刻 0 のステップで反映されます）。

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

| フィールド    | 中身                                                                            |
| ------------- | ------------------------------------------------------------------------------- |
| `info`        | 変数の定義（いまの実行になければ `undefined`）                                  |
| `reading`     | 最後の値（`VariableReading`。まだないとき、`visible` が偽のときは `undefined`） |
| `condition`   | 値の状態（下の「値の状態（`condition`）」）                                     |
| `block`       | いま書けない理由（下の「書けない理由（`block`）」）                             |
| `writeStatus` | 最後の書き込みの状態（下の「書き込みの状態（`writeStatus`、`pending`）」）      |
| `pending`     | 送っていて、結果がまだの値（なければ `null`）                                   |

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

| `block`             | 意味                                                                                                   |
| ------------------- | ------------------------------------------------------------------------------------------------------ |
| `read-only`         | 読むだけの変数（`role` が `output`）                                                                   |
| `disconnected`      | クライアントとサーバーの接続が切断されている                                                           |
| `ended`             | 実行が終わった                                                                                         |
| `not-ready`         | 変数の機能が使えない（実行していない、接続している途中、接続できない）                                 |
| `missing`           | いまの実行にない変数                                                                                   |
| `peer-disconnected` | その ECU が Control Server と接続していない                                                            |
| `parameter`         | `parameter` は、止めて始めた実行の最初のステップの前（`client.time.state` の `beforeStart`）だけ書ける |
| `scenario`          | シナリオの実行中（見るだけ）                                                                           |
| `no-range`          | `needsRange` を付けたのに、変数に範囲（`min` と `max`）がない                                          |
| `no-value`          | `needsValue` を付けていて、値（読んだ値か送った値）がまだない                                          |

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

| コマンド               | 内容                                                                                           |
| ---------------------- | ---------------------------------------------------------------------------------------------- |
| `npm run test:client`  | 型の検査（`tsc -p ../client`）と単体テスト（Vitest。Node の上で、Angular も DOM もなしに動く） |
| `npm run build:client` | 1 つの ESM のファイルにまとめる（esbuild。Angular のビルドのツールが持っているもの）           |
| `npm run lint`         | `gui/web` と、この `eslint.config.js`                                                          |
| `npm run format:check` | `gui/web` と、このディレクトリの書式（`gui/.prettierrc`）                                      |

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
| `variables.mjs`        | 同じくライブラリで、ECU の変数を読み書きする                           |
