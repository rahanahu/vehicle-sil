# gui/client（`@vehicle-sil/gui-client`）

[English](README.md)

`vehicle-sil-gui` サーバー向けの依存ゼロ TypeScript クライアントライブラリおよびカスタム要素集です。ブラウザと Node.js 22 以上で動作します。

このページでは、導入手順、`connect()` と `createFront()` の使い分け、高水準レシピ、および 5 フレームワーク（Vanilla JS/TS・Angular・React・Vue・Svelte）での 12 種の `vsil-*` 部品の使い方を扱います。低水準の `createClient()` API、`variables.bind()`、公開エクスポート全一覧、プロトコルに近い詳細、開発者向けテストは [docs/ja/gui-client-low-level.md](../../docs/ja/gui-client-low-level.md) を参照してください。

## 目次

- [入手とインポート](#入手とインポート)
- [`connect()`・`createFront()`・`createClient()` の使い分け](#connectcreatefrontcreateclient-の使い分け)
- [正面の口（`connect()`）](#正面の口connect)
- [レシピ](#レシピ)
- [部品（custom element）](#部品custom-element)
- [5 フレームワークでのセットアップ](#5-フレームワークでのセットアップ)
- [低水準 API と関連ドキュメント](#低水準-api-と関連ドキュメント)

## 入手とインポート

リリースのたびに npm レジストリへ `@vehicle-sil/gui-client` を公開し（`latest` がリリース版、`next` がプレリリース版）、同じ `vehicle-sil-gui-client-<version>.tgz` をリリースページとビルド済みツールアーカイブにも同梱しています。GUI サーバーと同じ版をインストールしてください（プロトコル版が違うサーバーは拒否されます）。

| 使い方                                         | 入手方法                                                                           | import の書き方                                              |
| ---------------------------------------------- | ---------------------------------------------------------------------------------- | ------------------------------------------------------------ |
| `vehicle-sil-gui --web` で配信する画面         | コピー不要（GUI サーバーが `/vsil/` から配信）                                     | `import { connect } from '/vsil/vehicle-sil-gui-client.mjs'` |
| `--web` 外の素の JavaScript（または Node 22+） | `vehicle-sil-gui-client.mjs` をスクリプトの隣に置く                                | `import { connect } from './vehicle-sil-gui-client.mjs'`     |
| TypeScript やフレームワークのビルド            | `npm install @vehicle-sil/gui-client@<version>`（または `npm install <path>.tgz`） | `import { connect } from '@vehicle-sil/gui-client'`          |

ビルド成果物の場所:

| ファイル                               | ビルド済みのツール                                    | リポジトリでのビルド（`gui/web`）                                                |
| -------------------------------------- | ----------------------------------------------------- | -------------------------------------------------------------------------------- |
| `vehicle-sil-gui-client.mjs`           | `<tools>/client/vehicle-sil-gui-client.mjs`           | `npm run build:client` -> `build/gui-client/vehicle-sil-gui-client.mjs`          |
| `vehicle-sil-gui-elements.mjs`         | `<tools>/client/vehicle-sil-gui-elements.mjs`         | `npm run build:client` -> `build/gui-client/vehicle-sil-gui-elements.mjs`        |
| `vehicle-sil-gui-client-<version>.tgz` | `<tools>/client/vehicle-sil-gui-client-<version>.tgz` | `npm run pack:client` -> `build/gui-client/vehicle-sil-gui-client-<version>.tgz` |

パッケージのサブパス（`package.json` の `exports`）:

| import パス                        | 用途                                                                                                                                                                                                        |
| ---------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `@vehicle-sil/gui-client`          | 高水準 `connect()` / `createFront()`、低水準 `createClient()`、`watch()`、`until()`、`ActionError`、`WriteError`、プロトコル型                                                                              |
| `@vehicle-sil/gui-client/elements` | 12 種の `vsil-*` カスタム要素を登録し、`pageIntent`、`setReasonText`、要素クラス、`VsilElementContracts`、書き込みイベント詳細型（`WriteQueuedInfo`、`WriteOutcome`、`WriteSuccess`、`WriteFailure`）を公開 |
| `@vehicle-sil/gui-client/timeline` | `<vehicle-sil-timeline>` を登録し、要素型・イベント型を公開                                                                                                                                                 |
| `@vehicle-sil/gui-client/react`    | React 18・19 向けの型専用 JSX 要素宣言（`react` への実行時依存なし）                                                                                                                                        |
| `@vehicle-sil/gui-client/vue`      | Vue 3 向けの型専用 `GlobalComponents` 宣言（`vue` への実行時依存なし）                                                                                                                                      |
| `@vehicle-sil/gui-client/svelte`   | Svelte 5 向けの型専用 `SvelteHTMLElements` 宣言（`svelte` への実行時依存なし）                                                                                                                              |

- **開発サーバー（Vite、`ng serve` など）**: `/api/`、`/vsil/`、`/ws` を `vehicle-sil-gui` へプロキシし、`--dev-origin http://127.0.0.1:<port>` 付きで起動します（ローカル開発では `--no-auth` も併用できます。[docs/ja/gui.md](../../docs/ja/gui.md#開発サーバーを使う) の「開発サーバーを使う」）。

## `connect()`・`createFront()`・`createClient()` の使い分け

| 入口                                        | すぐ接続するか                                                        | ページ共有接続か                         | 向いている用途                                                                                                                                         |
| ------------------------------------------- | --------------------------------------------------------------------- | ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `await connect()`（引数なし、ブラウザ）     | はい（`connect()` + `loadApp()`）                                     | はい（ページ上の `<vsil-*>` 部品と共有） | `--web` で配信する独自画面、`<vsil-*>` 部品と併用するスクリプト                                                                                        |
| `await connect(options)`（ブラウザ / Node） | はい（`connect()` + `loadApp()`）                                     | いいえ（専用接続。終了時に `close()`）   | Node.js スクリプト・自動化、2 本目の接続、`batchMs` / `watchPeriodMs` を明示したいとき                                                                 |
| `createFront(options)`                      | いいえ（`front.low.connect()` と `front.low.loadApp()` を自分で呼ぶ） | いいえ（専用接続。終了時に `close()`）   | サーバー応答前やオフライン中も画面を描画し続けたいアプリ（`gui/web` など）                                                                             |
| `createClient(options)`                     | いいえ（`client.connect()` と `client.loadApp()` を自分で呼ぶ）       | いいえ                                   | 生のセッション・イベント・ログ、複数変数の同時書き込み、`variables.bind()`（[docs/ja/gui-client-low-level.md](../../docs/ja/gui-client-low-level.md)） |

3 つのオプションはすべて camelCase に統一されています（`ConnectOptions` は `ClientOptions` の別名）:

| オプション                  | 型・範囲                                   | 既定値                                                      | 用途                                                                                                                                             |
| --------------------------- | ------------------------------------------ | ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| `baseUrl`                   | `string`（オプション指定時は必須）         | `connect()` ではサーバーのルート                            | GUI サーバーのルート URL（`http://127.0.0.1:<port>/`、`#token=...` なし）                                                                        |
| `token`                     | `string \| null \| (() => string \| null)` | `connect()` では `takeToken(window)`                        | `#token=...` のトークン。`--no-auth` サーバーでは省略または `null`                                                                               |
| `origin`                    | `string`                                   | Node では `baseUrl` の origin、ブラウザ / Worker では未指定 | `Origin` ヘッダーの上書き（ブラウザや Web Worker では渡さない）                                                                                  |
| `answerTimeoutMs`           | 有限 `number > 0`                          | `5000`（`ANSWER_TIMEOUT_MS`）                               | 時間コマンド応答および `run`/`time`/`write` の初回 WS 同期の待機上限                                                                             |
| `batchMs`                   | 有限 `number` `0..1000`                    | `100`（`BATCH_MS`）                                         | 受信 WebSocket 通知のバッチ間隔（`0` はマイクロタスクでフラッシュ。制御フレーム `synced`、`operation`、`error`、`command` は常に即時フラッシュ） |
| `watchPeriodMs`             | 有限 `number` `20..60_000`                 | `100`（`WATCH_PERIOD_MS`）                                  | 変数 watch の既定ポーリング間隔（`WATCH_PERIODS_MS` へ切り下げてサーバー上限内に丸める）                                                         |
| `http`, `socket`, `onError` | トランスポート / エラー受け                | `fetch`、`WebSocket`、`console.error`                       | 通信差し替え、購読者例外の受け口                                                                                                                 |

未知のキーや旧 snake_case キー（`base_url`、`timeout_ms` など）は実行時に `ActionError('unknown-field', ..., 'refused')` で即拒否されます。

- **起動時のリトライ挙動の違い**: `connect()` は初回の `loadApp()` と WebSocket 同期を待って解決し、初期接続に失敗した場合は無限リトライせず**即座に reject** します（自動化スクリプトで失敗をすぐ検知できます）。これに対し `createFront()` は即座に `Front` を返し、オンライン復帰までのリトライやオフライン描画を `front.low.connect()` 側で扱えます。

## 正面の口（`connect()`）

`connect()` は `Front` を返します。リアクティブな `Readable` 状態（`status`、`clock`、`controls`、`scenario`、`runtime`、`outside`）、Promise ベースの操作（`run`、`time`、`check`、`scenarios`、`connections`、`configs`、`recording`、`prepare`、`readConfig`、`describeAgain`、`saveConfig`）、および変数ハンドル（`variable(ref)`）を提供します。土台の低水準クライアントは常に `front.low` から参照できます。

```ts
import { ActionError, connect, watch } from '@vehicle-sil/gui-client';

// --web で配信するページでは引数なしで呼ぶと <vsil-*> 部品と接続を共有する。
// Node.js では { baseUrl, token } を渡す（--no-auth 時は token を省略可）。
const client = await connect();

const stop = watch([client.status, client.controls], (status, controls) => {
  startButton.disabled = !controls.start.ok;
  startButton.title = controls.start.reason?.message ?? '';
  banner.textContent = status.problem?.message ?? '';
});

try {
  await client.time.pause();
  await client.time.step(100); // 時刻が進み再び一時停止したときに解決する
} catch (error) {
  if (error instanceof ActionError) console.log(error.code, error.message, error.outcome);
}
```

データのフィールド名はプロトコルと同じ snake_case（`force_stop`、`sim_ms`、`can_write`）、メソッドと JS オプションは camelCase（`forceStop`、`runUntil`、`timeoutMs`）です。すべての `Readable` は変化がなければ同一参照を返し、`get` / `subscribe` は `this` に依存しません（React の `useSyncExternalStore` にそのまま渡せます）。

### `status`、`clock`、`controls`

- **`status`**: `{ link, run, file, result, problem }`。
  - `link`: `'connecting' | 'online' | 'offline' | 'rejected' | 'incompatible'`。
  - `run`: `'none' | 'starting' | 'waiting' | 'running' | 'paused' | 'stopping' | 'ended' | 'crashed'`。
  - `result`: `run` が `'ended'` または `'crashed'` のとき `{ end, scenario, ok, message }`。
  - `problem`: `{ code, message } | null` — 接続エラーを最優先し、それ以外はランチャー異常終了・実行失敗・直近の操作失敗のうち最新のもの（変数書き込み失敗は `variable.get().problem` に出ます）。
- **`clock`**: `{ sim_ms, speed, actual_speed, pacing }`（`pacing`: `'not-paced' | 'on-time' | 'late'`）。`sim_ms` は頻繁に更新されるため `status` から分離しています。
- **`controls`**: `{ start, stop, force_stop, pause, resume, step, run_until, speed, save }`。各項目は `{ ok, reason, caution }` で、`reason` と `caution` は `{ code, message } | null` です。英語の `message` ではなく `code` で分岐してください。全 `controls.*.reason.code` と `caution.code` の網羅表（および `--time-policy` 優先順位・`free` 既定速度）は [docs/ja/gui.md](../../docs/ja/gui.md#操作の理由コード注意コードと---time-policy) の「操作の理由コード・注意コードと `--time-policy`」を参照してください。

### `run`、`time`、`until()`

- **`run.start(intent?, { file?, onStarted? }?)`**: 実行を開始します。`intent` は `schema/run-request.schema.json` のキー（`variant`、`params`、`scenario`、`case`、`after_scenario`、`record`、`record_after_fail_ms`、`record_events`、`record_can`、`disable`、`enable`、`manual`、`strict`、`speed`、`start_paused`、`stop_at_ms`、`late_tolerance_ms`、`connect`、`time_policy`）を取り、未指定キーは `null` ではなく省略します。直近のセッションと同じ条件で再実行するときは、`session.intent` をそのまま渡せます: `await front.run.start(session.intent, { file: session.file })`（`session.intent.scenario` はルート相対パスのまま保持されています）。サーバーが開始を受け付けた時点で解決するため、新しい実行状態を読む前に `until()` で `status.run !== 'starting'` を待ってください。
- **`run.stop()` / `run.forceStop(target?)`**: 実行を停止します。`await connect()` 直後の `link === 'connecting'` 中に `run.stop()`、`run.forceStop()`、`time.*()`、`variable().enqueue()` / `write()` を呼んだ場合、初回 WebSocket 同期の完了（上限 `answerTimeoutMs`）を自動で待ってから送出します。
- **`time.pause()` / `resume()` / `step(ms)` / `runUntil(ms)` / `setSpeed(value)`**: `step` と `runUntil` は目標時刻へ到達して再び一時停止したときに解決します。到達前に別クライアントや標準入力から別のコマンドが入ると `ActionError('interrupted', ..., 'unknown')` で reject します。
- **`until(source, test, { timeoutMs?, signal? })`**: `test` を満たす最初の値で解決し（現在値も即検査）、タイムアウトや中断では `ActionError('timeout' | 'aborted', ..., 'unknown')` で reject します（タイムアウト時は `(last observed: ...)` で最後に観測した値を含みます）。オプションは camelCase（`timeoutMs`、`signal`）のみで、未知キーは `unknown-field` で拒否されます。
- **`scenario`（`timeline_url`）— 完了済み・`--speed max` 実行の調査**: `client.scenario.get()?.timeline_url`（またはセッションの `scenario.timelineUrl`）が `GET api/timeline/<run_id>` を指します（`--no-auth` 以外は `Authorization: Bearer <token>` を付けて取得）。ライブ変数ポーリングでは取りこぼす完了後のシナリオ実行や `--speed max` の高速実行を全ステップ精度で調べるには、このタイムライン JSON を取得してください（サーバーは `--keep-runs N` 件、既定 4 件を保持し、過去の成功 run より失敗 run を優先して残します）。

### 変数 1 つを扱う（`client.variable(ref)`）

`"ECU.variable"`（最初のドットで分割）または `{ peer, name }`（ECU 名にドットを含むとき）で指定します。

```ts
const throttle = client.variable('Engine.engine.throttle');

// 購読単位でポーリング周期を任意指定できる（20..60_000 ms）
const stop = throttle.subscribe(
  (view) => {
    slider.value = String(view.value ?? 0);
    slider.disabled = !view.can_write;
    slider.title = view.why?.message ?? view.note ?? '';
  },
  { periodMs: 20 },
);

// 1 回の呼び出しで段階 1〜4 を完了させる書き込み
const result = await throttle.write(40);
console.log(result.appliedAtMs, result.late);
stop();
```

**4 段階書き込み API（`enqueue()` + `applied()` / `write()`）**:

- **`enqueue(value, options?): Promise<QueuedWrite>`**: 段階 1 + 2 を完了します。GUI サーバーが検証・FIFO `seq` 発番し、ランチャー Control Server の `WAIT_FOR_QUEUED` で ECU キュー到達（または即時確定）を確認してから解決します。拒否または即時終端失敗（`invalid`、`busy`、`late`、`rejected`、`cancelled` 等）はその場で `ActionError` で reject します。
- **`queued.applied(waitOptions?): Promise<WriteResult>`**: 段階 3 + 4 を完了します。ECU がステップで反映すると `{ session, seq, peer, name, value, appliedAtMs, late }` で解決し、失敗時は `ActionError` で reject します。
- **`write(value, options?): Promise<WriteResult>`**: `enqueue(value, options).then((q) => q.applied({ timeoutMs, signal }))` の糖衣です。`applied()` が仮想時間ステップでの適用を待つため、`paused` 中に `await throttle.write(...)` を待つと仮想時間が進むまで完了しません。一時停止中は `enqueue()` → `step()` / `runUntil()` → `applied()` の順で使ってください（下のレシピ 3）。
- **`FrontWriteOptions`**: `{ atMs?, late?: 'reject' | 'apply', timeoutMs?, keepalive?, signal? }`。未知のキーは即座に `ActionError('unknown-field', ..., 'refused')` で拒否されます。
- **仮想時間連動ウォッチドッグ（`timeoutMs`）**: 既定 `WRITE_TIMEOUT_MS`（10,000 ms）。`0` より大きい有限の数値である必要があります（`Infinity` は `invalid-argument` で拒否されます）。接続が健全で対象 ECU が接続中かつ実行が `paused` または `holding` の間はウォッチドッグが**一時停止**するため、一時停止中の書き込みやマルチレート予約 `atMs` 書き込みが誤タイムアウトしません（一時停止中の実時間上限が必要なら `signal: AbortSignal.timeout(ms)` を渡します）。実行中は低速再生（`speed < 1`）に合わせて予算を拡大し `sim_ms` が進むたびにリセットされる一方、WS 切断・ECU 切断・仮想時間停止時は有限時間で確実に reject します。
- **ポーリング周期と責務境界**: 最初の `subscribe()` で watch が始まり、最後の解除で止まります（`close()` はありません）。`{ periodMs }` を渡すと購読中のみ最短周期へ昇格し、解除で元の周期へ戻ります。`watchPeriodMs` / `periodMs` を `20` ms（`batchMs: 16` または `0`）にするとライブ UI の応答性が上がりますが、最新値ポーリングは 20 ms 未満のパルスの捕捉を保証**しません**。ステップ精度の決定論的記録には `--timeline` または `Spy` を使ってください（[docs/ja/gui.md](../../docs/ja/gui.md#ライブポーリングと決定論的記録の使い分け) の「ライブポーリングと決定論的記録の使い分け」）。
- **保持入力と再入注意**: `pagehide` からの解除書き込みには `{ keepalive: true }` を渡し、`subscribe()` コールバック内で無防備な再入書き込みを行わないでください（[docs/ja/gui.md](../../docs/ja/gui.md#保持入力切断復帰バス観測) の「保持入力・切断復帰・バス観測」）。

### 外部接続と構成ファイルのテキスト編集

- **外部バス接続**: 選択 UI を開くときに `await client.connections({ network })` を呼び（サーバー側ホストでブリッジを起動して一覧取得）、`await client.check({ connect: { ETH_EXT: target }, time_policy: 'free' })` で事前照合し、`client.run.start(intent)` で開始、`client.outside` でブリッジ状態を監視します。
- **構成テキスト編集**: `await client.readConfig({ file })` で保存済みテキストと SHA-256 `stamp` を取得し、`await client.check({ variant }, { file, text })` で ECU を起動せずに未保存編集を検証、`await client.describeAgain({ file, variant })` で再ビルド後の `--describe` を更新し、`await client.saveConfig({ file, text, stamp })` で原子的に保存します。

## レシピ

### 1. 実行開始前に変数を発見する（`client.check()`）

`client.check()` は実行開始前に `vehicle_sil_launch check --json` を実行し、各 participant の `--describe` 変数メタデータ（`builtin` を含む: `ecu.connected`・`peer.*`・`j1939.*`・`can.*`・`lin.*`・`eth.*` 等の自動追加変数は `true`、ECU 宣言変数は `false`）を返します。

```ts
import { connect } from '@vehicle-sil/gui-client';

const client = await connect({ baseUrl: 'http://127.0.0.1:8765/', token });
try {
  const checked = await client.check();
  for (const participant of checked.participants) {
    const vars = participant.variables ?? [];
    const appVars = vars.filter((v) => !v.builtin);
    const builtinVars = vars.filter((v) => v.builtin);
    console.log(participant.name, {
      app: appVars.map((v) => `${v.name} (${v.type}, ${v.causality}, ${v.unit})`),
      builtin: builtinVars.map((v) => v.name),
    });
  }
} finally {
  client.close();
}
```

### 2. `run.start()` 後に `starting` 状態の完了を待つ

`client.run.start()` はサーバーが要求を受理した時点で解決するため、直後は `status.run === 'starting'` です。camelCase の `timeoutMs` を指定した `until()` で遷移を待ち、その後の `status.run` と `status.result` を確認します。

```ts
import { connect, until } from '@vehicle-sil/gui-client';

const client = await connect({ baseUrl: 'http://127.0.0.1:8765/', token });
try {
  await client.run.start({ start_paused: true });
  const settled = await until(client.status, (s) => s.run !== 'starting' && s.run !== 'waiting', {
    timeoutMs: 30_000,
  });
  if (settled.run !== 'paused') {
    throw new Error(`Run did not pause: ${settled.run} (${settled.result?.message ?? ''})`);
  }
} finally {
  client.close();
}
```

### 3. 一時停止中の決定論的書き込み（`enqueue()` -> `step()` / `runUntil()` -> `applied()`）

`enqueue()` は ECU キューへの到達（`WAIT_FOR_QUEUED`）まで待つため、`step()` や `runUntil()` の前に固定の `sleep()` や無限タイムアウトを書く必要はありません。

```ts
import { connect, until } from '@vehicle-sil/gui-client';

const client = await connect({ baseUrl: 'http://127.0.0.1:8765/', token });
try {
  await client.run.start({ start_paused: true });
  await until(client.status, (s) => s.run === 'paused', { timeoutMs: 30_000 });

  const throttle = client.variable('Engine.engine.throttle');

  // 一時停止中に立ち上がり・立ち下がりをキューへ積む（sleep 不要）
  const rise = await throttle.enqueue(80, { atMs: 100 });
  const fall = await throttle.enqueue(20, { atMs: 250 });

  // 仮想時間を決定論的に進めて両方反映させる
  await client.time.runUntil(300);

  const [r1, r2] = await Promise.all([rise.applied(), fall.applied()]);
  console.log('applied at:', r1.appliedAtMs, r2.appliedAtMs);
} finally {
  await client.run.stop().catch(() => undefined);
  client.close();
}
```

### 4. 手動操作の記録を再生して手動へ引き継ぐ（`recording()` + `end_ms` + `after_scenario: 'pause'`）

書き込みが反映された実行を終了した後に `client.recording()` を呼ぶと、終了仮想時刻のトップレベル `end_ms` が確定したシナリオ YAML（`vehicle_sil_scenario: 1`）が得られます（実行中に呼ぶと `end_ms` は省かれ、最後の `set` 手順で終了します）。その YAML を設定ファイルの隣の `scenarios/` に置いて `after_scenario: 'pause'` で再生すると、ECU を再起動せず同じ実行のまま手動操作へ引き継げます。

```ts
import { writeFileSync } from 'node:fs';
import { connect, until } from '@vehicle-sil/gui-client';

const client = await connect({ baseUrl: 'http://127.0.0.1:8765/', token });
try {
  // 1. 記録元の実行終了後、確定 end_ms 付きシナリオ YAML を取得する
  const rec = await client.recording();
  if (!rec?.yaml || rec.writes === 0) throw new Error('No applied writes recorded');
  console.log(`recorded ${rec.writes} writes, end_ms=${rec.end_ms}`);
  writeFileSync('scenarios/recorded.yaml', rec.yaml, 'utf8');

  // 2. サーバーの一覧から保存済みシナリオを取得し、一時停止引き継ぎ付きで再生する
  const list = await client.scenarios();
  const saved = list.find((s) => s.path.endsWith('recorded.yaml'));
  if (!saved) throw new Error('recorded.yaml not listed under scenarios/');

  await client.run.start({ scenario: saved.path, after_scenario: 'pause' });
  await until(client.scenario, (sc) => sc?.handed_over === true, { timeoutMs: 30_000 });
  console.log('handed over at ms:', client.scenario.get()?.over_at_ms);

  // 3. 同じ実行のまま書き込みと時間操作が解放される
  const q = await client.variable('Engine.engine.throttle').enqueue(0);
  await client.time.step(100);
  await q.applied();
} finally {
  await client.run.stop().catch(() => undefined);
  client.close();
}
```

### 5. ライブポーリング周期の調整（`watchPeriodMs`、`batchMs`、`subscribe({ periodMs })`）

既定の 100 ms ポーリング・100 ms バッチはバックグラウンド負荷を抑えます。高応答なゲージやアニメーション向けには、`connect()` の `watchPeriodMs` / `batchMs` を下げるか、特定変数の購読中だけ短縮します:

```ts
import { connect } from '@vehicle-sil/gui-client';

const client = await connect({
  baseUrl: 'http://127.0.0.1:8765/',
  token,
  watchPeriodMs: 100, // ほとんどの変数の既定周期
  batchMs: 16, // 受信 WebSocket フレームを約 60 Hz でフラッシュ
});

// このパネルを表示中だけ engine.speed を 20 ms 周期へ昇格する
const stop = client.variable('Engine.engine.speed').subscribe(
  (view) => {
    if (view.value !== null) console.log(view.sim_ms, view.text, view.unit);
  },
  { periodMs: 20 },
);
// stop() を呼ぶと自動的に元の遅い周期へ戻る
```

## 部品（custom element）

`@vehicle-sil/gui-client/elements` は、`connect()` の上に作られたスタイルなしのライト DOM カスタム要素を 12 個定義します。すべての HTML 属性に対応する camelCase JS プロパティ（真偽値 `noUnit`、falsy 値 `value={false}` / `value={0}`、数値 `stepMs` を含む）を備えているため、素の HTML でも Angular・React・Vue・Svelte のテンプレートでもそのままバインドできます。

| 要素                     | 表示・動作                                                                                                                               | 属性 / JS プロパティ                                                                                                                                                                 | カスタムイベント                         |
| ------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ---------------------------------------- |
| `<vsil-status>`          | 接続、実行状態、仮想時間（遅れ表示付き）、現在の問題 1 件                                                                                | `client`（prop）                                                                                                                                                                     | —                                        |
| `<vsil-run-controls>`    | 開始（ページの意図付き）、停止、強制停止                                                                                                 | `label-start`（`labelStart`）、`label-stop`（`labelStop`）、`label-force-stop`（`labelForceStop`）、`intent`（prop）、`client`（prop）                                               | —                                        |
| `<vsil-time-controls>`   | 一時停止、再開、ステップ、時刻まで実行、速度選択                                                                                         | `speeds`、`step-ms`（`stepMs`）、`label-pause`（`labelPause`）、`label-resume`（`labelResume`）、`label-step`（`labelStep`）、`label-run-until`（`labelRunUntil`）、`client`（prop） | —                                        |
| `<vsil-value>`           | 単位付きの値と非ライブ時の説明。bool は `on`/`off` 文字                                                                                  | `variable`、`on`、`off`、`no-unit`（`noUnit`）、`client`（prop）                                                                                                                     | —                                        |
| `<vsil-slider>`          | 範囲内の数値スライダー。離したときに単一スロット合体で書き込み                                                                           | `variable`、`client`（prop）                                                                                                                                                         | `vsil-write-queued`、`vsil-write-result` |
| `<vsil-write>`           | 1 つの値を書き込むボタン（すでに同値なら無効）                                                                                           | `variable`、`value`（`string \| number \| boolean \| null`）、`label`、`client`（prop）                                                                                              | `vsil-write-queued`、`vsil-write-result` |
| `<vsil-scenario-select>` | 次の実行のシナリオ選択                                                                                                                   | `client`（prop）                                                                                                                                                                     | —                                        |
| `<vsil-case-select>`     | 選んだシナリオに複数ケースがあるときのケース選択（現在の `variant` に合わない `only` 付きケースを無効化）                                | `client`（prop）                                                                                                                                                                     | —                                        |
| `<vsil-scenario-result>` | シナリオの状態、ケース、解決済みパラメーター、ステップ一覧（失敗手順の `file:line` とテンプレートの `call_stack` を含む）                | `client`（prop）                                                                                                                                                                     | —                                        |
| `<vsil-timeline>`        | 内の `<vehicle-sil-timeline>` を実行の `timeline_url`（シナリオ実行と `record`・`record_events`・`record_can` つき手動実行）へ結び付ける | `client`（prop）                                                                                                                                                                     | —                                        |
| `<vsil-connect>`         | 外部バス接続先選択（`SIL Kit only` + ブリッジ一覧）                                                                                      | `network`、`file`、`client`（prop）                                                                                                                                                  | —                                        |
| `<vsil-outside>`         | 外部接続中ネットワークの状態・フレーム数一覧                                                                                             | `client`（prop）                                                                                                                                                                     | —                                        |

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
<label>Case <vsil-case-select></vsil-case-select></label>
<vsil-scenario-result></vsil-scenario-result>
<script src="/timeline/vehicle-sil-timeline.js"></script>
<vsil-timeline>
  <vehicle-sil-timeline compact hide="help tables"></vehicle-sil-timeline>
</vsil-timeline>
```

`<vsil-timeline>` は内部の `<vehicle-sil-timeline>` の `src` と `hidden` だけを管理し（中身が空なら既定要素を追加）、自身に `data-timeline`（`shown` または `none`）を付けます。タイムラインのスクリプト自体はページ側で読み込みます（`<script src="/timeline/vehicle-sil-timeline.js">` または `import '@vehicle-sil/gui-client/timeline'`）。

```ts
import '@vehicle-sil/gui-client/elements'; // 12 個の部品を定義する
import { pageIntent, setReasonText } from '@vehicle-sil/gui-client/elements';

setReasonText((reason) => (reason.code === 'busy' ? 'Wait a moment.' : null));
pageIntent.update({ variant: 'winter' }); // <vsil-scenario-select> と合わせて次の Start に使われる
const controls = document.querySelector('vsil-run-controls'); // HTMLElementTagNameMap で型が付く
if (controls) {
  controls.intent = { speed: 'max' };
  controls.labelStart = 'Run';
}
```

### 部品が約束すること

- **ページ共有の接続**: 既定では各部品が引数なしの `connect()` を呼び、自分のスクリプトと 1 本の WebSocket を共有します。別の `Front` を使わせたいときは `element.client = front` を設定します。`/vsil/` のファイルとローカルコピーを混ぜないでください（ライブラリが二重になると接続が 2 本になります）。
- **ライト DOM と `data-*` スタイル属性**: shadow DOM は使わず `<span class="vsil">` 内に描画します。`[aria-disabled="true"]`、`[data-link]`、`[data-run]`、`[data-pacing]`、`[data-stale="true"]`、`[data-can-write]`、`[data-availability="inactive"]`、`[data-on]`、`[data-error]`、`[data-cases]`（`<vsil-case-select>`: `none` または `available`）、`[data-status]` と `[data-case]`（`<vsil-scenario-result>`、手順は `data-status`）、`[data-reason]`、`[data-caution]`、`[data-write-status]` で見た目を付けられます。
- **書き込みライフサイクルイベント（`vsil-write-queued` と `vsil-write-result`）**: `<vsil-slider>` と `<vsil-write>` は、バブリング・composed（`bubbles: true, composed: true`）の小文字カスタムイベントを 2 つ発火します（切断・再バインド後は発火しません）。
  - `vsil-write-queued`: `CustomEvent<WriteQueuedInfo>`（`{ session, seq, peer, name, value }`）— 書き込みが ECU キューへ到達したとき。
  - `vsil-write-result`: `CustomEvent<WriteOutcome>` — 成功時は `WriteSuccess`（`{ ok: true, session, seq, peer, name, value, appliedAtMs, late }`）、失敗時は `WriteFailure`（`{ ok: false, peer, name, value, error: ActionError, seq? }`）。
  - 両要素とも直近に開始した書き込み状態を `data-write-status`（`idle`、`sending`、`queued`、`applied`、`failed`）へ反映し、実行の切り替わりで自動リセットします。
- **アクセシブルで簡潔な理由表示（`.vsil-why`、`.vsil-note`、`QUIET`）**: 使えないボタンも `aria-disabled="true"` のままフォーカス可能で、`data-reason` と `aria-describedby` で `<small class="vsil-why">`（変数部品は `<small class="vsil-note">`）を指します。空の説明や自明な `QUIET` 理由（`not-running`、`offline`、`connecting`、`already-running`、`paused`、`not-paused`、`not-stopping`、`already-stopping`、`starting`、および `<vsil-write>` の `already`）は `hidden` が付き、スクリーンリーダーには読まれつつ画面のレイアウトを乱しません。CSS で `.vsil-why` や `.vsil-note` に `display` を当てるときは `:not([hidden])` を付けてください（[docs/ja/gui.md](../../docs/ja/gui.md#部品のスタイリングと書き込みイベントの購読) の「部品のスタイリングと書き込みイベントの購読」）。
- **接続失敗の重複抑止**: 共有接続が失敗したとき（トークン未指定など）、すべての部品に `data-error` が付き、エラー文はページ内で 1 か所だけ（`<vsil-status>`、なければ文書順で最初の部品）に表示されます。

### 文言と次の実行

`setReasonText(fn)` で英語の理由文を差し替えられます（`fn(reason)` が文字列を返すと置き換わり、`null`/`undefined` で英語のまま。`setReasonText(null)` で解除）。部品をページへ置く前に呼んでください。部品の自前の語（`ready`、`writing…`、`(no scenario)`、`(no cases)`、`(choose a case)`、`fastest`、`behind real time` 等）は英語固定です。`pageIntent`（`get()`、`subscribe(fn)`、`update(patch)`）は**次の**実行の意図を保持します（実行中のセッションで上書きされません）。`<vsil-scenario-select>` が `scenario` を書き（シナリオが変わると `case` を消去）、`<vsil-case-select>` は `pageIntent` の `scenario` と `variant` を読んでケース一覧を取得し `case` を書き込みます。`<vsil-run-controls>` は `{ ...pageIntent.get(), ...controls.intent }` で開始します。

### 部品でネットワークを外につなぐ

`<vsil-connect network="ETH_EXT">` は選択を開いたときにだけ `connections()` を呼び（ページ読込や Tab 移動では呼ばない）、選択を `pageIntent.connect` へ反映して約 200 ms デバウンスで `check()` し、一覧から消えた接続先は `none` へ戻して `data-reverted` を付けます。`time_policy` はスクリプトから `pageIntent.update({ time_policy: 'free' })` で設定します。`<vsil-outside>` は `client.outside` の各行を `data-network`、`data-condition`、`data-nothing-received` 付きで描画します。

## 5 フレームワークでのセットアップ

以下の 5 環境はすべて、CI で実 `.tgz` パッケージに対してコンパイルおよび実行スモーク検証されています（`npm --prefix gui/web run verify:consumers`）。

### Vanilla JS / TypeScript

```ts
import '@vehicle-sil/gui-client/elements';
import type {
  VsilSliderElement,
  VsilWriteElement,
  WriteOutcome,
  WriteQueuedInfo,
} from '@vehicle-sil/gui-client/elements';

const slider: VsilSliderElement | null = document.querySelector('vsil-slider');
slider?.addEventListener('vsil-write-queued', (e) => {
  const info: WriteQueuedInfo = e.detail;
  console.log(`queued seq=${info.seq} value=${info.value}`);
});
slider?.addEventListener('vsil-write-result', (e) => {
  const outcome: WriteOutcome = e.detail;
  if (outcome.ok) console.log(`applied at ${outcome.appliedAtMs} ms`);
  else console.error(`failed (${outcome.error.code}): ${outcome.error.message}`);
});

const cut: VsilWriteElement = document.createElement('vsil-write');
cut.variable = 'Engine.ecu.connected';
cut.value = false;
cut.label = 'Cut engine';
document.body.append(cut);
```

### Angular

コンポーネントに `CUSTOM_ELEMENTS_SCHEMA` を追加し、`@vehicle-sil/gui-client/elements`（`<vehicle-sil-timeline>` を使うなら `@vehicle-sil/gui-client/timeline` も）を import します:

```ts
import { CUSTOM_ELEMENTS_SCHEMA, Component } from '@angular/core';
import type { Front, Intent } from '@vehicle-sil/gui-client';
import '@vehicle-sil/gui-client/elements';
import type { WriteOutcome, WriteQueuedInfo } from '@vehicle-sil/gui-client/elements';

@Component({
  selector: 'app-bench',
  standalone: true,
  schemas: [CUSTOM_ELEMENTS_SCHEMA],
  template: `
    <vsil-status [client]="front"></vsil-status>
    <vsil-run-controls [client]="front" [intent]="intent" label-start="Run"></vsil-run-controls>
    <vsil-slider
      [client]="front"
      variable="Engine.engine.throttle"
      (vsil-write-queued)="onQueued($event)"
      (vsil-write-result)="onResult($event)"
    ></vsil-slider>
    <vsil-write
      [client]="front"
      variable="Engine.ecu.connected"
      [value]="false"
      label="Cut"
    ></vsil-write>
  `,
})
export class BenchComponent {
  front: Front | null = null;
  intent: Intent = { speed: 'max' };
  onQueued(event: Event): void {
    const detail = (event as CustomEvent<WriteQueuedInfo>).detail;
    console.log(detail.seq, detail.value);
  }
  onResult(event: Event): void {
    const detail = (event as CustomEvent<WriteOutcome>).detail;
    console.log(detail.ok ? detail.appliedAtMs : detail.error.code);
  }
}
```

### React 18・19

`@vehicle-sil/gui-client/react` を 1 回 import（`src/main.tsx` や `src/env.d.ts` など）すると、12 種の `vsil-*` 部品と `<vehicle-sil-timeline>` の JSX 型が登録されます。

```tsx
import { useEffect, useRef } from 'react';
import type { Front } from '@vehicle-sil/gui-client';
import '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/react';
import '@vehicle-sil/gui-client/timeline';
import type { VsilSliderElement, WriteOutcome } from '@vehicle-sil/gui-client/elements';

// React 19: 小文字の `on<event>` JSX props が addEventListener へ直接委譲される。
export function BenchReact19({ front }: { front: Front }) {
  return (
    <div>
      <vsil-status client={front} />
      <vsil-run-controls client={front} intent={{ speed: 'max' }} labelStart="Run" />
      <vsil-value client={front} variable="Engine.engine.speed" noUnit={false} />
      <vsil-slider
        client={front}
        variable="Engine.engine.throttle"
        onvsil-write-queued={(e) => console.log(e.detail.seq)}
        onvsil-write-result={(e) =>
          console.log(e.detail.ok ? e.detail.appliedAtMs : e.detail.error.code)
        }
      />
      <vsil-write client={front} variable="Engine.ecu.connected" value={false} label="Cut" />
      <vehicle-sil-timeline
        compact={true}
        variables={['Engine.*']}
        onload={(e) => console.log(e.detail.status)}
        onerror={(e) => console.error(e.detail.message)}
      />
    </div>
  );
}

// React 18（React 19 でも動作）: ref + useEffect でカスタムイベントとオブジェクト prop を設定する。
export function BenchSliderReact18({ front }: { front: Front }) {
  const ref = useRef<VsilSliderElement>(null);
  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    el.client = front;
    const onResult = (e: CustomEvent<WriteOutcome>) => console.log(e.detail);
    el.addEventListener('vsil-write-result', onResult);
    return () => el.removeEventListener('vsil-write-result', onResult);
  }, [front]);
  return <vsil-slider ref={ref} variable="Engine.engine.throttle" />;
}
```

> **React 19 の `onload` / `onerror` に関する注意**: React は PascalCase の `onLoad` / `onError` を合成イベントとして横取りするため、カスタム要素の `CustomEvent` に対して発火しません（`./react` 型はコンパイル時に検知できるよう `<vehicle-sil-timeline>` から `onLoad` / `onError` を除外しています）。React 19 では小文字の `onload` / `onerror` を使い、React 18・19 共通で動かす場合は `ref` + `addEventListener` を使ってください。

### Vue 3

Vue のテンプレートコンパイラで `vsil-*` と `vehicle-sil-timeline` をカスタム要素として扱う設定を追加し（例 `vite.config.ts`: `vue({ template: { compilerOptions: { isCustomElement: (tag) => tag.startsWith('vsil-') || tag === 'vehicle-sil-timeline' } } })`）、`vue-tsc` 向けに `@vehicle-sil/gui-client/vue` を import します:

```vue
<script setup lang="ts">
import type { Front, Intent } from '@vehicle-sil/gui-client';
import '@vehicle-sil/gui-client/elements';
import type { WriteOutcome, WriteQueuedInfo } from '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/vue';

const props = defineProps<{ front: Front | null }>();
const intent: Intent = { speed: 'max' };

function onWriteQueued(event: CustomEvent<WriteQueuedInfo>) {
  console.log(event.detail.seq, event.detail.value);
}
function onWriteResult(event: CustomEvent<WriteOutcome>) {
  console.log(event.detail.ok ? event.detail.appliedAtMs : event.detail.error.code);
}
</script>

<template>
  <vsil-status :client="props.front" />
  <vsil-run-controls :client="props.front" :intent="intent" label-start="Run" />
  <vsil-value :client="props.front" variable="Engine.engine.speed" :noUnit="true" />
  <vsil-slider
    :client="props.front"
    variable="Engine.engine.throttle"
    @vsil-write-queued="onWriteQueued"
    @vsil-write-result="onWriteResult"
  />
  <vsil-write
    :client="props.front"
    variable="Engine.ecu.connected"
    :value="false"
    label="Cut"
    @vsil-write-queued="onWriteQueued"
    @vsil-write-result="onWriteResult"
  />
</template>
```

### Svelte 5

`svelte-check` 向けに `@vehicle-sil/gui-client/svelte` を import します:

```svelte
<script lang="ts">
  import type { Front, Intent } from '@vehicle-sil/gui-client';
  import '@vehicle-sil/gui-client/elements';
  import type { WriteOutcome, WriteQueuedInfo } from '@vehicle-sil/gui-client/elements';
  import '@vehicle-sil/gui-client/svelte';

  let { front }: { front: Front | null } = $props();
  const intent: Intent = { speed: 'max' };

  function onWriteQueued(event: CustomEvent<WriteQueuedInfo>) {
    console.log(event.detail.seq, event.detail.value);
  }
  function onWriteResult(event: CustomEvent<WriteOutcome>) {
    console.log(event.detail.ok ? event.detail.appliedAtMs : event.detail.error.code);
  }
</script>

<vsil-status client={front}></vsil-status>
<vsil-run-controls client={front} {intent} label-start="Run"></vsil-run-controls>
<vsil-time-controls client={front} speeds="0.5 1 max" stepMs={25}></vsil-time-controls>
<vsil-value client={front} variable="Engine.engine.speed" noUnit={true}></vsil-value>
<vsil-slider
  client={front}
  variable="Engine.engine.throttle"
  onvsil-write-queued={onWriteQueued}
  onvsil-write-result={onWriteResult}
></vsil-slider>
<vsil-write
  client={front}
  variable="Engine.ecu.connected"
  value={false}
  label="Cut"
  onvsil-write-queued={onWriteQueued}
  onvsil-write-result={onWriteResult}
></vsil-write>
```

## 低水準 API と関連ドキュメント

| 知りたいこと                                                                                                                                        | ドキュメント                                                             |
| --------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------ |
| 低水準 `createClient()`、公開 API 全一覧、`variables.bind()`、`createWaveforms()`、`createVariableEditor()`、`raw` キー、ライフサイクル、開発テスト | [docs/ja/gui-client-low-level.md](../../docs/ja/gui-client-low-level.md) |
| 操作の `reason.code` / `caution.code` 一覧、CSP 制限、ライブポーリングと `--timeline`/`Spy` の境界、自作画面のルール                                | [docs/ja/gui.md](../../docs/ja/gui.md)                                   |
| 独自画面を作成して配信する手順ガイド                                                                                                                | [ブラウザの GUI に自分の画面を作る](../../docs/ja/howto/gui-screen.md)   |
| サーバーの HTTP・WebSocket プロトコル仕様                                                                                                           | [gui/protocol/README.ja.md](../protocol/README.ja.md)                    |
| ビルド不要の HTML/CSS/JS 独自画面スターター                                                                                                         | [examples/gui_starter](../../examples/gui_starter/)                      |
| Node.js ヘッドレスクライアント例（`run.mjs`、`front.mjs`、`variables.mjs`）                                                                         | [examples/gui_client](../../examples/gui_client/)                        |
