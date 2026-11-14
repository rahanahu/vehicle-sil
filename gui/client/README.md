# gui/client

GUI サーバー（`vehicle-sil-gui`）のクライアントのライブラリです（TypeScript）。接続（トークン、再接続）、セッションの写し（サーバーの要約と事象から作る実行の状態）、実行と時刻の操作、ECU の変数（一覧、値の watch、書き込みと結果）、サーバーの口の型と読み取りを持ちます。Angular にも RxJS にも、ほかのパッケージにも依りません。サンプルの画面（`gui/web`）も、Angular の薄い包み（`gui/web/src/app/core/session/`）を通して、これだけを使います。

サーバーの口そのもの（HTTP と WebSocket の形、版と互換の決まり、認証）は [gui/protocol](../protocol/README.md) にあります。例は [examples/gui_client](../../examples/gui_client/) にあります: ライブラリを使わずに口を叩く `run.mjs`、ライブラリを 1 つにまとめたファイルを素の JavaScript（Node）から使う `run-with-library.mjs`、同じくライブラリで ECU の変数を読み書きする `variables.mjs`。GUI サーバーに配らせる自分の画面のひな形は [examples/gui_starter](../../examples/gui_starter/) です（作り方は [docs/gui.md](../../docs/gui.md) の「自分の画面を作る」）。

## 取り込み方

ライブラリは TypeScript のソースで、依存はありません。npm のレジストリには公開していません。

- **パッケージ（TypeScript と好きな枠組み）**: `gui/web` で `npm run pack:client` を実行すると、`build/gui-client/vehicle-sil-gui-client-<版>.tgz` ができます（1 つにまとめた ESM、`.d.ts`、`exports`、README、LICENSE。版は GUI サーバーと同じ。作り方は `pack.mjs`）。自分のプロジェクトで `npm install <その .tgz>` をすると、`@vehicle-sil/gui-client` として import できます。型はブラウザの型（`lib` の `DOM`）を使います。入口は `.` だけです（`testing` と `internal` は入りません）。
- **このリポジトリの中の TypeScript のアプリ**: `tsconfig.json` の `paths` で、入口の名前を `src/` に向けます（`gui/web` がそうしています）。
- **素の JavaScript**: 1 つの ESM のファイルにまとめたものを使います。`gui/web` で `npm run build:client` を実行すると、`build/gui-client/vehicle-sil-gui-client.mjs`（リポジトリの直下の `build/`。追跡しない）ができます。ブラウザでも Node（22 以上）でも読めます。`scripts/verify-gui.py` が、このファイルを Node から使う例を、録ったものを流し直すランチャーを相手に動かして確かめます。

## 入口

| 入口                   | 名前                               | 中身                                                                                                                                                                                |
| ---------------------- | ---------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/index.ts`         | `@vehicle-sil/gui-client`          | 公開するもの（下の表）。互換を約束するのはここだけ                                                                                                                                  |
| `src/testing/index.ts` | `@vehicle-sil/gui-client/testing`  | 試験の部品: 偽の WebSocket（`FakeWebSockets`）、偽の HTTP（`FakeHttp`）、`gui/protocol/examples/` の読み込み、要約から `SessionView` を作る `sessionViewFrom`（自分の部品の試験に） |
| `src/internal.ts`      | `@vehicle-sil/gui-client/internal` | **互換の約束の外**。中の部品（クラス、要約を当てる関数、読み取りの部品）。試験のために出している。予告なく変わり、消える。アプリでは使わない                                        |

公開する口は狭く始めます（足すのは互換、消すのは互換でないため）。要るものが公開の入口にないときは、`internal` に頼らず、足すかを決めます。

## 公開するもの

| まとまり             | 公開するもの                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| -------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| クライアント         | `createClient(options)`、`ClientOptions`（`baseUrl`、`token`、`origin`、`http`、`socket`、`answerTimeoutMs`、`onError`）、`GuiClient`（`connection`、`session`、`run`、`time`、`variables`、`app`、`limits`、`connect()`、`close()`、`loadApp()`、`configs()`、`check()`、`fetchSession()`、`dashboard(file)`、`scenarios(file)`、`recording()`）、`DEFAULT_LIMITS`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| トークン             | `takeToken(place)`（ブラウザでは `takeToken(window)`。URL の `#token=` を読んで URL から消し、sessionStorage に置く。なければ置いたもの、どちらもなければ `null`）、`TokenPlace`、`TOKEN_STORAGE_KEY`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| 接続（読むだけ）     | `ClientConnection`（`state`、`failures`、`lastClose`、`tryAgainReason`、`onBatch`、`onUnreadable`）、`ConnectionState`、`CloseInfo`、`BATCH_MS`、`TRY_AGAIN_LATER`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| セッションの写し     | `ClientSession`（`session`、`ready`、`lastEventAt`、`notices`、`rejected`、`incompatible`、`onMessages`、`report`、`dismissNotice`）、`Notice`、`SessionView`、`ParticipantView`、`LauncherView`、`LogLine`、`LogSource`、`TimeView`、`CommandView`、`RunOutcome`、`runOutcome`、`phaseIndex`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| 実行の操作           | `ClientRun`（`starting`、`stopping`、`stopWhileStarting`、`error`、`start`、`stop`、`forceStop`）、`StopAction`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| 時刻                 | `ClientTime`（`state`、`answerTimeoutMs`、`pause`、`resume`、`step`、`setSpeed`、`send`）、`TimeState`、`TimeFailure`、`Lateness`、`SendResult`、`NotSentReason`、`ANSWER_TIMEOUT_MS`、`FEATURE_SPEED`、`FEATURE_START_PAUSED`、`FEATURE_TIME_COMMANDS`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| ECU の変数           | `ClientVariables`（`state`、`operations`、`watchProblem`、`watch`、`write`、`bind`）、`VariablesState`、`VariablesStatus`、`VariablePeer`、`VariableInfo`、`VariableRef`、`VariableValue`、`VariableWatch`（`values`、`value`、`history`、`close`）、`WatchOptions`、`WatchValues`、`VariableReading`、`HistoryPoint`、`WriteOptions`、`OperationView`、`OperationPeer`、`WriteError`、`WatchProblem`、`WATCH_PERIOD_MS`、`WATCH_PERIODS_MS`、`WRITE_TIMEOUT_MS`、`HISTORY_MAX`、`HISTORY_TOTAL_MAX`、`OPERATIONS_KEPT`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| 変数と部品のつながり | `client.variables.bind(ref, options)`、`BindOptions`（`periodMs`、`history`、`needsRange`、`needsValue`、`visible`）、`VariableBinding`（`ref`、`state`、`history()`、`write(value)`、`reject(message)`、`setVisible(visible)`、`close()`）、`BoundVariable`（`ref`、`info`、`reading`、`condition`、`block`、`writeStatus`、`pending`）、`ValueCondition`、`WriteBlock`、`WriteStatus`、`WriteValue`、`isWriteError`、`DROPPED_MESSAGE`。組み立てに使っている純粋な関数 `valueCondition`（`ConditionInput`）、`writeBlock`（`WriteBlockInput`）、`hasRange`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| 読める値             | `Readable<T>`（`get()`、`subscribe(fn)`）                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| 差し替えの口         | `SocketFactory`、`SocketHandlers`、`SocketLike`、`webSocketFactory`、`HttpTransport`、`HttpRequest`、`HttpOutcome`、`fetchTransport`、`FetchTransportOptions`、`DEFAULT_TIMEOUT_MS`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| 誤り                 | `HttpError`（`status`、`statusText`、`problem`、`timedOut`）、`errorMessage`、`ProtocolError`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| 口の形と読み取り     | `gui/protocol` の形の型（`SessionSummary`、`ServerMessage`、`LauncherEvent`、`CheckResult`、`RunResult`、`AppInfo` など）、`PROTOCOL_VERSION`、`PHASES`、`severityRank`。口を自分で読む人のための `readServerMessage`、`readAppInfo`、`readConfigList`、`readCheckResponse`、`readSessionResponse`、`readTimeResponse`、`readWriteResponse`、`readVariablesSummary`、`readDashboardResponse`、`readScenarioList`、`readRecording`。変数の口の形は `VariablesSummary`、`VariableSummary`、`ValuesMessage`、`OperationReport`、`WatchMessage`、`WriteAnswer`。ダッシュボードのファイル（`dashboard(file)`）の形は `DashboardResponse`、`DashboardFile`、`DashboardFileSection`、`DashboardFileItem`、`DashboardFileError`、`DashboardPanelModule`、`DashboardVariable`。シナリオ（`--scenario`）の形は `ScenarioStep`、`ScenarioStepStatus`、`ScenarioStatus`、`CheckScenario`、`ScenarioResult`、`SummaryScenario`、`ScenarioList`、`ScenarioFile`、`Recording`（`scenarios(file)`、`recording()`）と、`FEATURE_SCENARIO`、`FEATURE_SCENARIO_CONTROL`。要求の形は `CheckRequest`、`RunRequest`、`Speed`、`StopRequest`、`TimeCommand`、構成ファイルの一覧は `ConfigList`、`ConfigFile`、誤りの形は `ServerProblem`、所見は `Finding`、`Severity`。自分で起動する participant（`manual`）は `FEATURE_MANUAL`。セッションの写しの `SessionView.scenario` は `ScenarioView` |

## 使い方

```ts
import { createClient, takeToken } from '@vehicle-sil/gui-client';

// サーバーが表示した URL（http://127.0.0.1:<port>/#token=<token>）から開いた画面: URL の #token= を受け取って消す。
// Node の道具では、baseUrl とトークンを、その URL から自分で取り出して渡す（examples/gui_client）。
const client = createClient({
  baseUrl: new URL('/', location.href).href,
  token: takeToken(window),
});
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
- トークンを断られたら（サーバーを起動し直した、など）、`client.session.rejected` が真になり（WebSocket）、HTTP の要求は状態 401 の `HttpError` で断られます。
- ライブラリが肩代わりしない決まり（ランチャーの `features` に合わせた要求の欄、変数を書けるか、値が古いか）は、[docs/gui.md](../../docs/gui.md) の「ライブラリの外にある決まり」にあります。
- 断られた操作は、Promise が `HttpError` で reject し、同じ理由が状態（`client.run.error`、`client.time.state` の `failure`）にも出ます。応答を待つのは `DEFAULT_TIMEOUT_MS` までです。開始（`run.start()`）と照合（`check()`）は、サーバーが照合を 1 つずつ行い、順番待ちと照合にそれぞれ照合の上限（`limits.check_timeout_s`）までかけ、開始の中でも変わった構成ファイルを照合し直すので、その 2 倍に `DEFAULT_TIMEOUT_MS` を足した時間まで待ちます。過ぎたら `timedOut` が真の `HttpError`（状態 0）です。これは断られたのではなく、答えが来なかっただけです: 開始なら、サーバーは実行を始めたかもしれず、始まったかはセッションの写し（WebSocket）でわかります（`client.run.error` もそう言います）。転送（リダイレクト）には従いません。
- 読み取りは部分ごとです。要約と事象の一部が読めなくても、読める部分を使い、読めない部分の名前を `SessionView.unreadable` に入れます。知らない通知と事象は状態に当てずに通します（`client.connection.onBatch` には、読む前のものがすべて届きます）。JSON として読めない通知は、知らせを出して `hello` から受け直します。
- Node では、`Origin` をサーバー自身の origin にして送ります（`createClient` が `baseUrl` から作ります）。ブラウザと Web Worker では、ブラウザが付けるので `origin` を渡しません（Worker の WebSocket は握手のヘッダーを付けられません）。HTTP の道具を変えるときは `http`（`HttpTransport`）を、WebSocket を変えるときは `socket`（`SocketFactory`）を渡します。

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

- **一覧と状態**（`client.variables.state`）: `status` は `unknown`（まだ聞いていない）、`off`（この実行に変数はない）、`connecting`、`ready`、`failed`、`ended`（実行が終わった。値は最後のもの）。`off`、`connecting`、`failed` の理由は `reason`（サーバーの英語の文）。実行を始める前に使えるかは `client.app` の `variables`（`available`、`reason`）でわかります。変数の定義は `peers[].variables[]`（`type`、`role`、`writable`、`unit`、`min`、`max`、`step`、`description`、`enum`）。
- **watch**: 変数（`'all'` は全部）の最後の値を受け取ります。`values`（読める値）は `values[peer]?.[name]` が `VariableReading`（`value`、`sampleMs`、`simMs`、`paused`）です。値ごとに「いつのものか」を持ちます。`paused` が真の値は、止まっている時刻 `simMs` の値として見せられます（その時刻より前に始まったステップの値は入っています）。偽の値は、動いている途中の最後の値です。
  - 同じ変数を見る watch がいくつあっても、サーバーへの購読は変数ごとに 1 つです（変数は、頼まれた中でいちばん短い間隔の購読に入ります。間隔は 20、50、100、200、500 ms … の段に丸めます）。`close()` で、ほかに見ている watch がなければサーバーの購読もやめます。使い終わったら必ず `close()` します。
  - つなぎ直したら、自動で購読し直します。実行が替わっても watch は続き、前の実行の値と履歴は捨てます。
  - `history` を頼むと、変数ごとに、その数までの点（`sampleMs`、`value`）を環状バッファーに持ちます（変数ごとの上限 `HISTORY_MAX`。古いものから捨てます）。場所は点が届いてから取ります（頼んだだけでは取りません）。全部の変数の履歴を合わせて `HISTORY_TOTAL_MAX` 点（約 34 MB）までで、そこに届くと、どの履歴もそれ以上は伸びず、そのときの大きさのまま古い点を捨てます（`history(ref)` が返す点が、頼んだ数より少なくなります）。同じステップの値は 1 点で、同じステップの値が変わって届いたら、その点を置き換えます。実行が替わると、履歴は空から始まります。`watch.history(ref)` は、呼ぶたびに新しい配列を返します。通知は約 100 ms ごとにまとめて届くので（`BATCH_MS`）、`values` の知らせはその頻度ですが、履歴には間の値も入ります。
  - 長い名前を多く挙げて、購読の 1 通がサーバーの上限（256 KiB）を超えそうなときは、その間隔の購読を「全部」に替えます（受け取った中から、見ている変数だけを使います）。サーバーが watch を断ったとき（上限の違うサーバー）は、`client.variables.watchProblem` に出ます。
  - 値を覚えるのは、だれかが見ている変数だけです。
- **書き込み**（`write(writes, options)`）: 呼んだ時点の実行（`client.variables.state` の `session`）に向けて送ります（本文の `session`）。サーバーは、その実行がもう動いていなければ断るので（`session-changed`）、実行が替わる間に送った書き込みが次の実行に書かれることはありません。実行がまだわからないときは、送らずに `refused`（`not-running`）で断ります。サーバーが型と範囲を確かめ、ランチャーに渡します。Promise は、反映されたら（`applied`）その操作（`OperationView`）で解け、ほかは `WriteError` で断ります。同じ書き込みを自動で送り直すことはありません。`kind` は次のとおりです。

  | `kind`      | 意味                                                                                                                                                                                                                                                                                                                                                                                                                    | 送り直してよいか                           |
  | ----------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------ |
  | `refused`   | サーバーが受け付けなかった（`code` に `unknown-variable`、`not-writable`、`invalid-field`、`not-running`、`session-changed`、`variables-not-ready`、`write-busy` など）。ランチャーには渡っていない                                                                                                                                                                                                                     | 直してから、よい                           |
  | `failed`    | 受け付けたが、反映されなかった（`operation` に participant ごとの状態と理由: `late`、`rejected`、`invalid` など）                                                                                                                                                                                                                                                                                                       | 理由を直してから、よい（反映されていない） |
  | `no-answer` | **反映されたかどうかわからない**: `timeoutMs` の間に結果が来なかった（`operation` に、わかっている最後の状態）、HTTP の答えがなかった、サーバーが途中で失敗した（500）、サーバーが Control Server の答えを待ちきれなかった（`code` が `write-timeout`）か途中で切れた（`write-failed`）、答えの前に実行が替わった、つなぎ直している間に確定して結果が失われた（下）、サーバーが未確定の操作を覚えきれずに追うのをやめた | **そのままは送り直さない**（下）           |
  | `closed`    | 結果の前に `close()` した                                                                                                                                                                                                                                                                                                                                                                                               | 同上                                       |
  - **二重の書き込みを避ける**: `no-answer` の後は、先に確かめます。`error.operation` があれば、その番号の操作を `client.variables.operations` で追います（`queued` なら、シミュレーションがそのステップに進めば反映されます。待つ時間が短かっただけです）。番号がなければ、`operations` に自分の書き込みらしい操作が増えていないかと、その変数の値（watch）を見ます。反映されていないとわかってから送り直します。値を足し込むような（同じ書き込みを 2 回すると結果が変わる）使い方では、特に守ります。同じ値を書くだけなら、2 回書いても結果は同じです。
  - **つなぎ直し**: 結果を待つ間に接続が切れても、つなぎ直した後の要約（`hello`、`fetchSession()`）で続きがわかります。要約には、サーバーが覚えている未確定の操作の全部と、番号が `operations_from_seq` 以上の操作の全部が入っています。自分の操作が一覧にあれば、その状態を使います（未確定なら、そのまま待ちます）。一覧になく、番号が `operations_from_seq` より小さいときだけ、切れている間に確定して結果が失われたとして `no-answer` にします。まだ反映されていない書き込み（止まっている間の書き込みなど）は、後からいくつ操作が続いても `no-answer` になりません。サーバーが未確定の操作を覚えきれなかったとき（256 件を超えた。要約の `operations_open_omitted`）は、一覧にない古い操作を `no-answer` にします（サーバーがもう追っていないため。文でわかります）。
  - `onAccepted`（`{session, seq}` を受け取る関数）は、サーバーが書き込みを受け取って操作の番号を返したときに 1 回呼ばれます（結果より前。断られたとき、答えがなかったときは呼ばれません）。「受け付けた、反映を待っている」を見せる、次の書き込みを送ってよいと知る、のに使います（`bind` の書き込みは、これで、受け取られる前の次の値を送らずに覚えておき、受け取られたら最後の値だけを送ります）。
  - `timeoutMs`（既定 `WRITE_TIMEOUT_MS`）は、HTTP の答えが来てから数えます。`Infinity` は時間で切りません（実行が替わるか `close()` するまで待ちます）。2^31-1 ms（約 24.8 日）より長い値は、そこに丸めます。数でない値と 0 以下は既定の時間です。
  - 書き込みは、指定した仮想時刻（`atMs`。省くと次のステップ）のステップで反映されるので、止まっている間は確定しません。止めて書くときは、`client.variables.operations` で `queued` を見てから進めます（`examples/gui_client/variables.mjs`）。
  - 最近の操作（ほかのクライアントの書き込みも）は `client.variables.operations` で読めます。

- 止めて始めた実行（`start_paused`）は、変数の読み書きができるランチャーでは、最初のステップの前で待ちます。その間は `client.time.state` の `beforeStart` が真で、再開とステップ送りを押せます（書いた値は時刻 0 のステップで反映されます）。

### 部品を作る（`bind`）

スライダー、スイッチ、ランプのように、変数 1 つを描いて書く部品は、`watch` と `write` を自分で組み合わせずに `client.variables.bind(ref, options)` を使います。値の状態、書けるかどうかと理由、書き込みの様子を、1 つの読める値（`state`）にまとめて出します。

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

- **`state`**（`BoundVariable`）: `subscribe` は変わったときだけ呼ばれます（最初の様子は `state.get()`）。`get()` は、いつもいまの状態から作ります。そのため、ほかの読める値（`client.variables.state` など）の知らせの途中で `get()` を呼ぶと、その場で、この `state` の購読者に新しい様子を知らせることがあります。
  - `info`: 変数の定義（いまの実行になければ `undefined`）。`reading`: 最後の値（`VariableReading`。まだないとき、`visible` が偽のときは `undefined`）。
  - `condition`（値の状態）: `kind` で分けます。

    | `kind`        | 意味                                                                                                                                                                                                             |
    | ------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
    | `unavailable` | 変数の機能が使えない（`status` に `client.variables.state` の状態: 実行していない、この実行に変数はない、つないでいる、つながらない）                                                                            |
    | `missing`     | いまの実行にない変数                                                                                                                                                                                             |
    | `waiting`     | いまの実行にあるが、値がまだ届いていない                                                                                                                                                                         |
    | `live`        | 動いている途中の最後の値                                                                                                                                                                                         |
    | `paused`      | 止まっている時刻の値                                                                                                                                                                                             |
    | `stale`       | 古いかもしれない値。`why` が `connection`（クライアントとサーバーの接続が切れた）、`reconnecting`（サーバーがランチャーにつなぎ直している）、`peer`（ECU が切れた）、`reading`（値の読み取りが失敗し続けている） |
    | `ended`       | 終わった実行の、最後に読めた値                                                                                                                                                                                   |

  - `block`（いま書けない理由。書けるなら `null`）: 上にあるものが先です。

    | `block`             | 意味                                                                                                   |
    | ------------------- | ------------------------------------------------------------------------------------------------------ |
    | `read-only`         | 読むだけの変数（`role` が `output`）                                                                   |
    | `disconnected`      | クライアントとサーバーの接続が切れている                                                               |
    | `ended`             | 実行が終わった                                                                                         |
    | `not-ready`         | 変数の機能が使えない（実行していない、つないでいる、つながらない）                                     |
    | `missing`           | いまの実行にない変数                                                                                   |
    | `peer-disconnected` | その ECU が Control Server とつながっていない                                                          |
    | `parameter`         | `parameter` は、止めて始めた実行の最初のステップの前（`client.time.state` の `beforeStart`）だけ書ける |
    | `scenario`          | シナリオの実行中（見るだけ）                                                                           |
    | `no-range`          | `needsRange` を付けたのに、変数に範囲（`min` と `max`）がない                                          |
    | `no-value`          | `needsValue` を付けていて、値（読んだ値か送った値）がまだない                                          |

  - `writeStatus`（最後の書き込みの様子）: `idle`、`sending`（送った）、`queued`（サーバーが受け取った。`seq` は操作の番号）、`applied`（反映された。`atMs`）、`invalid`（`reject(message)` に渡した文。送っていない）、`dropped`（覚えていた値を送らなかった。下）、`refused`・`failed`・`no-answer`・`closed`（`WriteError` の `kind`。`message` と `code`）。`isWriteError(writeStatus)` は、`message` を持つもの（うまくいかなかったもの）を見分けます。`message` は英語です。画面の言葉にするときは `kind` から作ります。
  - `pending`: 送っていて、結果がまだの値（なければ `null`）。あれば、読んだ値の代わりに出します（押したスイッチが、結果を待つ間に元へ戻って見えません）。
- **`write(value)`**: 書けないとき（`block`）は何もしません。Promise は返さず、結果は `writeStatus` に出ます。時間では切らずに操作の結果を待ちます（止まっている間の書き込みは、進めるまで `queued` のままです）。
  - サーバーが前の書き込みをまだ受け取っていない間（`sending`）の次の値は、送らずに最後の 1 つだけを覚えておき、受け取られたら送ります（スライダーやキーの連打で、書き込みが積み重なりません）。そのとき、実行が同じで、まだ書けるかを確かめ直し、だめなら送らずに `dropped` にします。前の書き込みが受け取られる前に断られたら、覚えた値も送りません。
  - 結果が来なかった書き込み（`no-answer`）を、自分で送り直すことはしません。
  - `reject(message)` は、送る前に部品が見つけた誤り（数でない文字、範囲の外）を `invalid` として出します。送っている書き込み、覚えている値、`pending` には触れません。
- **変数ごとに 1 つ**: 同じ変数を何か所で `bind` しても、`writeStatus` と `pending` は同じものが見え、書き込みは重なりません。`bind` を閉じて作り直しても残ります。実行ごとなので、実行が替わると空（`idle`、`null`）から始まり、前の実行の書き込みの結果は出ません。
- **見えている間だけ watch する**: `bind` は、作ったときから値を受け取ります（`watch` と同じく、サーバーへの購読は変数ごとに 1 つ）。部品が多い画面では、`{ visible: false }` で作り、見えたとき（IntersectionObserver など）に `setVisible(true)`、隠れたら `setVisible(false)` を呼びます。偽の間、`reading` は `undefined` で、`condition` は `waiting` になります。`writeStatus` と `pending` は変わらず出ます。`block` も同じですが、`needsValue` を付けた `bind` は、送った値（`pending`）もなければ `no-value` になります。
- **履歴**: `{ history: 600 }` で作ると、`history()` が点（古い順）を返します。点が増えるたびに `state` が知らせるので、そこで読み直します。
- 使い終わったら必ず `close()` します（watch と購読を片づけます）。
- **決まりの外の使い方**:
  - `write(value)` は、値の型と範囲を確かめずに送ります。合わない値は、サーバーが断って `writeStatus` が `refused` になります。先に確かめて文を出したいときは、部品で確かめて `reject(message)` を呼びます。
  - `close()` の後の `write`、`reject`、`setVisible` は何もしません。
  - `close()` しても、すでに送った書き込みと覚えた値は取り消しません。覚えた値は、前の書き込みが受け取られたら送られます（様子は変数ごとに持つので、同じ変数を次に `bind` すると見えます）。

## 寿命

| 呼び出し                             | すること                                                                                                                                                                                   |
| ------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `createClient()`                     | 作るだけ。つながない                                                                                                                                                                       |
| `connect()`                          | WebSocket でつなぐ。トークンがないときは何もしない                                                                                                                                         |
| つながっている間の `connect()`       | 何もしない（つなぎ直しを待っている間も）                                                                                                                                                   |
| `close()`                            | 切る。つなぎ直さない。待っている時計を止め、時刻の命令の答えを待つのをやめ、結果を待っている書き込みを `WriteError`（`closed`）で断る。まとめている途中の通知は捨てる。変数の watch は残る |
| `close()` の後の `connect()`         | またつなぐ。セッションの写しは残り、持っている分の続きから受け直す。残っている watch を購読し直す                                                                                          |
| `close()` の後の HTTP の要求         | 使える（`loadApp()`、`run.start()` などは、つないでいなくても送れる）                                                                                                                      |
| 断られた・版が違った後の `connect()` | 何もしない（`session.rejected`、`session.incompatible` は戻らない）。別のトークンや版では、クライアントを作り直す                                                                          |

## 書き方の決まり

- 自分のファイルのほかは import しない（`eslint.config.js` が止める。動的な `import()` も、試験の部品のほかでは止める。`tsconfig.json` の `rootDir` で、`src/` の外も止まる）。ブラウザにしかないもの（`window`、`document`、`localStorage` など）を使わない。要るものは設定（`ClientOptions`）で受け取る。
- 人に見せる文（`TimeFailure.message` など）は英語。画面の言葉にするのは包みの役目（`kind` などの機械が読む欄から作る）。
- 公開するものを足したら `src/index.ts` と上の表に足し、[docs/gui.md](../../docs/gui.md) の「自分の画面を作る」を合わせる。

## 試験

道具（TypeScript、ESLint、Prettier、Vitest、esbuild）は `gui/web` の `node_modules` から借ります。`gui/web` で次を実行します（`scripts/verify-web.py` も回します）。

| コマンド               | 内容                                                                                         |
| ---------------------- | -------------------------------------------------------------------------------------------- |
| `npm run test:client`  | 型の検査（`tsc -p ../client`）と単体試験（Vitest。Node の上で、Angular も DOM もなしに動く） |
| `npm run build:client` | 1 つの ESM のファイルにまとめる（esbuild。Angular のビルドの道具が持っているもの）           |
| `npm run lint`         | `gui/web` と、この `eslint.config.js`                                                        |
| `npm run format:check` | `gui/web` と、このディレクトリの書式（`gui/.prettierrc`）                                    |

単体試験は、サーバーの通知を作らずに `gui/protocol/examples/` の例を流します（`testing/protocol-examples.ts`）。
