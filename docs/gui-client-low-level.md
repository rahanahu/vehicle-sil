# Low-level GUI client reference (`createClient()`)

[日本語](ja/gui-client-low-level.md)

Low-level reference for `@vehicle-sil/gui-client`: `createClient()`, `GuiClient`, the full public API surface, wire-adjacent ECU variable watches and writes, widget binding (`bind`), lifecycle details, and contributor tests. For onboarding, choosing between `connect()` and `createFront()`, high-level recipes, and the 12 custom elements (`@vehicle-sil/gui-client/elements`) across all 5 frameworks, start with [gui/client/README.md](../gui/client/README.md).

The front door (`connect()` / `createFront()`) is built directly on `createClient()` and exposes the same instance as `front.low`. Use this low-level API when you need direct access to raw events, logs, findings, participant state, multi-variable atomic `enqueue()` / `write()`, or `variables.bind()`.

| To find | See |
| --- | --- |
| Package entry points (`src/index.ts`, `./elements`, `./timeline`, framework adapters, `testing`, `internal`) | [Entry points](#entry-points) |
| Complete list of public exports by group | [Public API](#public-api) |
| Connecting with `createClient()`, `Readable`, timeouts, `raw` keys, and custom transports | [Basic usage](#basic-usage) |
| Watching values, four-stage `enqueue()` / `write()`, and the virtual-time watchdog | [ECU variables](#ecu-variables) |
| Building custom variable widgets with `bind()` (`condition`, `block`, `writeStatus`, `history`) | [Build widgets (`bind`)](#build-widgets-bind) |
| Connection lifecycle (`connect()`, `close()`, reconnect) | [Lifecycle](#lifecycle) |
| Developing and testing `gui/client` | [Coding rules](#coding-rules), [Tests](#tests) |

## Entry points

| Entry point | Name | Contents |
| --- | --- | --- |
| `src/index.ts` | `@vehicle-sil/gui-client` | The public API ([Public API](#public-api) below). This entry point and `./elements` are kept compatible |
| `src/elements/index.ts` (`dist/elements.js`, which imports `dist/index.js`) | `@vehicle-sil/gui-client/elements` | The 12 `vsil-*` custom elements, `pageIntent`, `setReasonText`, and element/event types (`VsilElementContracts`, `WriteQueuedInfo`, `WriteOutcome`, `WriteSuccess`, `WriteFailure`). Needs a DOM; the main entry does not import it |
| `../timeline/vehicle-sil-timeline.js` (`timeline/vehicle-sil-timeline.mjs`) | `@vehicle-sil/gui-client/timeline` | The `<vehicle-sil-timeline>` element and its types ("Putting a timeline on your own screen" in [docs/gui.md](gui.md#putting-a-timeline-on-your-own-screen)) |
| `types/frameworks/react.d.ts` (`dist/empty.js`) | `@vehicle-sil/gui-client/react` | Type-only React 18/19 JSX intrinsic element declarations derived from `AllElementContracts` (no runtime dependency on `react`) |
| `types/frameworks/vue.d.ts` (`dist/empty.js`) | `@vehicle-sil/gui-client/vue` | Type-only Vue 3 `GlobalComponents` declarations derived from `AllElementContracts` (no runtime dependency on `vue`) |
| `types/frameworks/svelte.d.ts` (`dist/empty.js`) | `@vehicle-sil/gui-client/svelte` | Type-only Svelte 5 `SvelteHTMLElements` declarations derived from `AllElementContracts` (no runtime dependency on `svelte`) |
| `src/testing/index.ts` (repository only) | `@vehicle-sil/gui-client/testing` | Test helpers (`FakeWebSockets`, `FakeHttp`, `gui/protocol/examples/` loaders, `sessionViewFrom`) |
| `src/internal.ts` (repository only) | `@vehicle-sil/gui-client/internal` | **No compatibility guarantee.** Internal classes and parsers; do not use them in apps |

- `testing` and `internal` are not included in the published npm package.
- The public API is kept small because adding to it preserves compatibility while removing breaks it. If the public entry point lacks something you need, decide whether to add it to `src/index.ts` instead of reaching into `internal`.

## Public API

The names that `src/index.ts` exports, by group. Parentheses list the main members of the object.

### Client

| Kind | Name |
| --- | --- |
| Function | `createClient(options)` |
| Type | `ClientOptions` (`baseUrl`, `token`, `origin`, `http`, `socket`, `answerTimeoutMs`, `batchMs`, `watchPeriodMs`, `onError`) |
| Type | `GuiClient` (`connection`, `session`, `run`, `time`, `variables`, `app`, `limits`, `connect()`, `close()`, `loadApp()`, `configs()`, `config(file)`, `check()`, `describe()`, `saveConfig()`, `prepare()`, `connections()`, `dashboard(file)`, `scenarios(file)`, `recording()`) |
| Constant | `DEFAULT_LIMITS` |

### Front door

| Kind | Name |
| --- | --- |
| Function | `connect(options?)`, `createFront(options)`, `watch(source, fn)`, `until(source, test, options?)` |
| Class | `ActionError` (`code`, `message`, `outcome`, `findings`) |
| Type | `ConnectOptions` (alias of `ClientOptions`), `UntilOptions` (`timeoutMs`, `signal`), `Front` (`low`, `status`, `clock`, `controls`, `scenario`, `runtime`, `outside`, `prepare()`, `configs()`, `recording()`, `run`, `time`, `variable()`, `scenarios()`, `connections()`, `check()`, `readConfig()`, `describeAgain()`, `saveConfig()`, `close()`), `FrontStatus`, `FrontResult`, `FrontClock`, `FrontControls`, `Control`, `Reason`, `FrontScenario`, `FrontRun`, `FrontTime`, `FrontVariable`, `FrontWriteOptions`, `QueuedWrite`, `WriteResult`, `VariableView`, `Intent`, `Outside`, `ConnectEntry`, `ConnectTarget`, `ConnectListError`, `Link`, `RunState`, `EndKind`, `Pacing`, `OperationName`, `RuntimeStatus` |

### Token

| Kind | Name |
| --- | --- |
| Function | `takeToken(place)` |
| Type | `TokenPlace` |
| Constant | `TOKEN_STORAGE_KEY` |

In the browser, call `takeToken(window)`. It reads `#token=` from the URL, removes it from the URL via `history.replaceState`, and stores it in `sessionStorage`. If the URL has no token, it returns the one in `sessionStorage`; if neither has one, it returns `null`. When connecting to a server started with `--no-auth`, `token` may be omitted or `null` (against a server with token authentication enabled, HTTP requests reject with 401 and WebSocket sets `rejected`).

### Connection (read-only)

| Kind | Name |
| --- | --- |
| Type | `ClientConnection` (`state`, `failures`, `lastClose`, `tryAgainReason`, `onBatch`, `onUnreadable`) |
| Type | `ConnectionState`, `CloseInfo` |
| Constant | `BATCH_MS` (`100`), `TRY_AGAIN_LATER` |

### Session copy

| Kind | Name |
| --- | --- |
| Type | `ClientSession` (`session`, `currentTarget`, `currentInstance`, `ready`, `allowed`, `runtime`, `lastEventAt`, `notices`, `rejected`, `incompatible`, `onMessages`, `report`, `dismissNotice`) |
| Type | `Notice`, `SessionView`, `ParticipantView`, `LauncherView`, `LogLine`, `LogSource`, `TimeView`, `CommandView`, `RunOutcome`, `LauncherAnswer` |
| Function | `runOutcome`, `phaseIndex`, `isHolding`, `isLauncherAlive`, `isLauncherFinishing`, `isRunActive`, `isSessionRunning`, `isScenarioHandedOver`, `isScenarioRunning`, `launcherAnswer` |

`ClientSession.allowed` is what the GUI server accepts now (`start`, `stop`, `force` and `save`, each with `ok`, a `reason` code and a `message`; `null` until the server says). `SessionView.launcherState` is the launcher's last `state` event (where the run stands and what it accepts; the larger `rev` wins; `null` until one arrives). `launcherAnswer(launcherState, name)` reads what it says of one operation (`write`, `pause`, ...): `reason` (`null` when the launcher accepts it) and `caution`. A launcher that has not said (no `state` yet, or no entry for the operation) does not accept it (`unsupported`). The front door's `controls` and `variables.bind` read the launcher with it.

Ordered notifications (`session`, `event`, `allowed`, `runtime`, `variables`, `availability`, `operation`, `error`, `synced`) arrive over the single WebSocket stream ordered by `(instance, run_id, stream_seq)`. Control frames (`synced`, `operation`, `error`, and launcher `command` events) flush immediately on a microtask without waiting for `batchMs`. HTTP mutation endpoints (`api/run`, `api/stop-session`, `api/prepare`, `api/time`, `api/write`) return acknowledgements while state updates arrive on the WebSocket stream.

`currentTarget()` returns the current `{ run_id, instance }` pair, or `null` when unknown. `currentInstance()` returns the last authenticated server instance even if no run exists, and retains it while disconnected. On reconnect to the same instance and run, the client sends `{ instance, run_id, after_stream_seq }` so the server resends only log events after that cursor; when the server instance changes, it starts fresh from the new `hello`.

### Run control

| Kind | Name |
| --- | --- |
| Type | `ClientRun` (`starting`, `stopping`, `currentStopping`, `stopWhileStarting`, `error`, `start`, `stop`, `forceStop`) |
| Type | `StopAction` |

`currentStopping` reports only pending stops of the current session or current start; `stopping` includes stops for earlier runs. Stop and force stop target the known session and instance. A confirmation dialog can pass the captured `RunTarget` to `front.run.forceStop(target)`; the action is refused if the target has changed or a new start is pending. A stop requested during this client's start waits for its answer; if that answer is lost, the stop is not sent and the error says so. Check the current session before retrying. An old server without targeted stop is rejected; overlapping starts on one client are rejected.

### Time

| Kind | Name |
| --- | --- |
| Type | `ClientTime` (`state`, `answerTimeoutMs`, `pause`, `resume`, `step`, `runUntil?`, `setSpeed`, `send`) |
| Type | `TimeState`, `TimeFailure`, `Lateness`, `SendResult`, `NotSentReason` |
| Constant | `ANSWER_TIMEOUT_MS` (`5000`) |

Both `front.time` and `client.time` send commands with the known run ID and server instance. If the run is replaced, the server rejects the command before sending it to a launcher. A screen can pass the `RunTarget` it captured when rendering a control to `front.time`; the client rejects the action if that target no longer matches the current run.

### ECU variable API

| Kind | Name |
| --- | --- |
| Type | `ClientVariables` (`state`, `operations`, `watchProblem`, `watch`, `enqueue`, `write`, `bind`) |
| Type | `VariableWatch` (`values`, `value`, `history`, `close`) |
| Type | `VariablesState`, `VariablesStatus`, `VariablePeer`, `VariableInfo` (includes `builtin`), `VariableRef`, `VariableValue`, `WatchOptions`, `WatchValues`, `VariableReading`, `HistoryPoint` |
| Type | `QueuedOperation` (`session`, `seq`, `applied(options?)`), `WriteOptions` (`instance`, `atMs`, `late`, `timeoutMs`, `keepalive`, `signal`), `WriteWaitOptions` (`timeoutMs`, `signal`), `OperationView`, `OperationPeer`, `WatchProblem` |
| Class | `WriteError` |
| Constant | `WATCH_PERIOD_MS` (`100`), `WATCH_PERIODS_MS`, `WRITE_TIMEOUT_MS` (`10_000`), `HISTORY_MAX`, `HISTORY_TOTAL_MAX`, `OPERATIONS_KEPT` |

### Binding variables to widgets (`bind`)

| Kind | Name |
| --- | --- |
| Method | `client.variables.bind(ref, options)` |
| Type | `BindOptions` (`periodMs`, `history`, `needsRange`, `needsValue`, `visible`) |
| Type | `VariableBinding` (`ref`, `state`, `history()`, `write(value)`, `reject(message)`, `setVisible(visible)`, `close()`) |
| Type | `BoundVariable` (`ref`, `info`, `reading`, `availability`, `condition`, `block`, `launcherReason` (a `Reason`), `writeStatus`, `pending`) |
| Type | `ValueCondition`, `WriteBlock`, `WriteStatus`, `WriteValue` |
| Function | `isWriteError`, `hasRange` |
| Function | `valueCondition` (`ConditionInput`), `writeBlock` (`WriteBlockInput`) |
| Constant | `DROPPED_MESSAGE` |

`valueCondition`, `writeBlock`, and `hasRange` are the pure functions that `bind` builds its state with.

### `Readable` (read-only value)

| Kind | Name |
| --- | --- |
| Type | `Readable<T>` (`get()`, `subscribe(fn)`) |

### APIs for substitution

| Kind | Name |
| --- | --- |
| Type | `SocketFactory`, `SocketHandlers`, `SocketLike` |
| Type | `HttpTransport`, `HttpRequest`, `HttpOutcome`, `FetchTransportOptions` |
| Function | `webSocketFactory`, `fetchTransport` |
| Constant | `DEFAULT_TIMEOUT_MS` |

### Errors

| Kind | Name |
| --- | --- |
| Class | `HttpError` (`status`, `statusText`, `problem`, `timedOut`) |
| Class | `ProtocolError` |
| Function | `errorMessage` |

### Server API types and parsing

Types for the shapes that `gui/protocol` defines, and the parsers to use when you read the server API yourself.

| Subject | Name |
| --- | --- |
| Types of the `gui/protocol` shapes | `SessionSummary`, `ServerMessage`, `LauncherEvent`, `CheckResult`, `RunResult`, `AppInfo` and others |
| Constants and functions | `PROTOCOL_VERSION`, `PHASES`, `severityRank` |
| Parsers | `readServerMessage`, `readAppInfo`, `readConfigList`, `readConfigText`, `readSavedConfig`, `readCheckResponse`, `readConnections`, `readDashboardResponse`, `readPrepareResponse`, `readRecording`, `readRunResponse`, `readRuntimeStatus`, `readScenarioList`, `readSessionAllowed`, `readSessionResponse`, `readSessionRuntime`, `readTimeResponse`, `readVariablesSummary`, `readWriteResponse` |
| Shapes of the variable API | `VariablesSummary`, `VariableSummary`, `ValuesMessage`, `OperationReport`, `VariableAvailability`, `WatchMessage`, `WriteAnswer` |
| Dashboard files (`dashboard(file)`) | `DashboardResponse`, `DashboardFile`, `DashboardFileSection`, `DashboardFileItem`, `DashboardFileError`, `DashboardPanelModule`, `DashboardVariable` |
| Scenarios (`--scenario`, `scenarios(file)`, `recording()`) | `ScenarioStep`, `ScenarioStepStatus`, `ScenarioStatus`, `ScenarioCallFrame`, `ScenarioParamType`, `CheckScenario`, `CheckScenarioCaseDetail`, `CheckScenarioParam`, `CheckScenarioParamMeta`, `ScenarioResult`, `SummaryScenario` (includes optional `handed_over`, `case`, `params`), `ScenarioList`, `ScenarioFile`, `Recording` (includes `end_ms: number \| null`) |
| Request shapes | `AfterScenario`, `CheckRequest`, `CheckTextRequest`, `DescribeRequest`, `PrepareRequest`, `SaveConfigRequest`, `RunRequest`, `SessionIntent`, `ConnectionsRequest`, `Speed`, `StopRequest`, `StopSessionRequestBody`, `TimeCommand` |
| List of configuration files | `ConfigList`, `ConfigFile`, `ConfigText`, `SavedConfig` |
| Error shape | `ServerProblem` |
| Findings | `Finding`, `Severity` |
| The launcher's keys and traffic | `FromLauncher`, `LauncherFields`, `RunTraffic`, `TrafficNetwork`, `TrafficParticipant`, `TrafficUnplaced`, `ParticipantRole`, `OutsideState` |
| `SessionView.scenario` of the session copy | `ScenarioView` |

### Live waveforms and variable editing

`createWaveforms` returns live histories in selection order. Signals shared by two selections keep their history, and a new run clears the histories. It keeps up to 600 points per signal by default. The screen draws the points and handles cross-tab synchronization.

`createVariableEditor` exposes the variable definition, display value, and write outcome in `state`. It validates numeric, Boolean, and enum edits against the definition. After a Boolean edit is applied, it waits for readback for up to 500 ms before accepting another edit. Set visibility to subscribe to values, and call `close()` on both parts when done.

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

## Basic usage

`createClient()` is the lower door of the library, which `connect()` and `createFront()` are built on (`front.low` is this same object). Unknown or legacy snake_case option keys (such as `base_url`) are rejected at runtime with `ActionError('unknown-field', ..., 'refused')`.

```ts
import { createClient, takeToken } from '@vehicle-sil/gui-client';

// A screen opened from the URL the server printed (http://127.0.0.1:<port>/#token=<token>): take #token= from the URL and remove it.
// A Node tool takes baseUrl and the token out of that URL itself and passes them (examples/gui_client, in a clone of the repository).
const client = createClient({
  baseUrl: new URL('/', location.href).href,
  token: takeToken(window),
  batchMs: 100, // 0..1000 ms (default 100); 0 flushes each turn on a microtask
  watchPeriodMs: 100, // 20..60_000 ms (default 100); default interval for variable watches
});
await client.loadApp(); // the server's information (the start-up file, the limits, whether variables are available)
client.connect();
const stop = client.session.session.subscribe((session) => {
  if (session?.result) console.log(session.result.result);
});
await client.run.start({ file: 'simulation.yaml', disable: [] });
```

### Read state (`Readable`)

Every state is a `Readable` (a read-only value).

| Call | What it does |
| --- | --- |
| `get()` | Returns the current value |
| `subscribe(fn)` | Notifies `fn` of later changes (not called with the current value) |
| The returned function | Ends the subscription |

- Notifications are synchronous: they arrive inside the code that changed the value.
- A subscriber that unsubscribes during a notification still receives that notification.
- If a subscriber changes the same value, every subscriber receives the new value, and the old value does not arrive afterward.
- A framework wrapper takes the first value with `get()` and converts later values into its own state with `subscribe()` (an Angular signal is shown in "[A project with a framework](../examples/gui_starter/README.md#a-project-with-a-framework)"; the package has no framework code).
- If a subscriber (a function passed to `subscribe`, `onBatch`, `onMessages` or `onUnreadable`) throws, the other subscribers are still notified and the library carries on (disconnecting when rejected, rebuilding and so on). The thrown value goes to `onError` (default `console.error`).
- `client.session.ready` tells whether the session copy is complete (connected, and `hello` and the replay have arrived). Other features (values, for example) have their own ready state.

### Rejections and timeouts

- If the token is rejected (for example, because the server was restarted), `client.session.rejected` becomes true (WebSocket), and HTTP requests are rejected with an `HttpError` of status 401.
- When an operation is rejected, its Promise rejects with an `HttpError`, and the same reason appears in the state (`client.run.error`, `failure` in `client.time.state`).
- A request waits for its response for up to `DEFAULT_TIMEOUT_MS`. After that it fails with an `HttpError` whose `timedOut` is true (status 0).
- Start (`run.start()`) and check (`check()`) wait up to twice the check limit (`limits.check_timeout_s`) plus `DEFAULT_TIMEOUT_MS`. The server runs checks one at a time, and both the wait in the queue and the check itself can take up to the check limit; a start also checks the configuration file again if it changed.
- A timeout is not a rejection: no response came. For a start, the server may have started the run; the session copy (WebSocket) shows whether it did, and `client.run.error` says so.
- Redirects are not followed.

### The launcher's keys (`raw`)

The check result, the run result, findings and launcher events come from the launcher, and the library passes on every key of them. The typed fields (for example `traffic` of the run result, `participants[].log`, and `role` of a check participant) follow the launcher's format versions (`vehicle_sil_check`, `vehicle_sil_run`, `vehicle_sil_events`), not the version of this library. Every object read from the launcher also has `raw`: the launcher's object as it arrived, with the keys the types do not name. `raw` is read-only and not enumerable (`JSON.stringify` and spreading leave it out). Its values are unchecked JSON: check their type, and show them as text, never as HTML.

```ts
const result = client.session.session.get()?.result; // RunResult | null
for (const network of result?.traffic?.networks ?? []) console.log(network.name, network.delivered);
const strict = result?.raw?.['strict']; // a key without a typed field: unknown
```

### Handling unreadable notifications

The library parses each part separately.

- If part of a summary or an event cannot be read, the library uses the readable parts and adds the names of the unreadable parts to `SessionView.unreadable`.
- Unknown notifications and events pass through without changing the state, and `onMessages` does not get them (`client.connection.onBatch` receives everything, before parsing: read an event that the library does not know there).
- A notification that is not valid JSON raises a notice, and the library receives everything again from `hello`.

### Use from Node, or replace the transport

- In Node, the library sends the server's own origin as `Origin` (`createClient` builds it from `baseUrl`).
- In the browser and in Web Workers, do not pass `origin`: the browser sets it (a Worker's WebSocket cannot set handshake headers).
- To replace the HTTP implementation, pass `http` (`HttpTransport`); to replace the WebSocket, pass `socket` (`SocketFactory`).

For the rules the library does not enforce for you (the fields of check and start requests, whether a variable is writable, whether a value is stale), see "Rules for building a screen" in [docs/gui.md](gui.md#rules-for-building-a-screen).

## ECU variables

```ts
const watch = client.variables.watch([{ peer: 'Engine', name: 'engine.speed' }], {
  periodMs: 100,
  history: 600,
});
watch.values.subscribe((values) => console.log(values['Engine']?.['engine.speed']));
try {
  // Two-stage write: enqueue() confirms ECU queue arrival, applied() waits for the ECU step.
  const queued = await client.variables.enqueue({ Engine: { 'engine.throttle': 50 } });
  const operation = await queued.applied();
  console.log(operation.peers['Engine'].appliedAtMs);
} catch (error) {
  if (error instanceof WriteError) console.log(error.kind, error.code, error.message);
}
watch.close();
```

### List and state (`client.variables.state`)

| `status` | Meaning |
| --- | --- |
| `unknown` | Not known yet |
| `off` | This run has no variables |
| `connecting` | Connecting |
| `ready` | Variables can be read and written |
| `failed` | Could not connect, or was refused |
| `ended` | The run ended (values are the last ones) |

- For `off`, `connecting` and `failed`, `reason` gives the reason, as text from the server (not localized).
- Before you start a run, `variables` in `client.app` (`available`, `reason`) tells whether variables are available.
- The variable definitions are `peers[].variables[]` (`type`, `role`, `writable`, `builtin`, `unit`, `min`, `max`, `step`, `description`, `enum`). `builtin: true` identifies automatic variables added by vehicle-sil (`ecu.connected`, `peer.*`, `j1939.*`, `can.*`, `lin.*`, `eth.*`), matching `check().participants[].variables[].builtin`.

### Read values (`watch`)

`watch` receives the last values of the given variables (`'all'` for every variable). Always call `close()` when you are done.

**Values**: in `values` (a `Readable`), `values[peer]?.[name]` is a `VariableReading` (`value`, `sampleMs`, `simMs`, `paused`). Each value carries the time it belongs to.

| `paused` | What the value is |
| --- | --- |
| True | The value at the paused time `simMs` (it includes the values of every step that started before that time) |
| False | The last value while the simulation runs |

- `watch.values` notifies whenever any watched variable's reading changes (including `simMs` or `paused` updates when the value itself did not change). Frames with `peers: {}` (such as a `start_paused` run before step 0) do not emit on `watch.values`.
- For pause or simulation-time state before the first sample, use `isHolding(session)` or `front.clock`; for raw `ValuesMessage` frames (including `peers: {}`), subscribe to `client.session.onMessages`.

**Subscriptions**:

- However many watches look at a variable, the server has one subscription per variable. A variable joins the subscription with the shortest period requested (periods are rounded down to `WATCH_PERIODS_MS`: 20, 50, 100, 200, 500 ms … and clamped to the server's `limits.watch_period_ms_min..watch_period_ms_max`).
- When `WatchOptions.periodMs` is omitted, the watch uses `ClientOptions.watchPeriodMs` (default `WATCH_PERIOD_MS` = 100 ms).
- `close()` also ends the server subscription if no other watch looks at the variable, or relaxes the period back to the shortest remaining subscriber.
- After a reconnection, the library subscribes again automatically.
- A watch continues across runs; the values and history of the previous run are discarded.
- Values are kept only for variables that someone is watching.
- Real-time latest-value polling (even at 20 ms) does not guarantee capturing pulses shorter than the polling interval; see "[Live polling vs deterministic capture](gui.md#live-polling-vs-deterministic-capture)" in `docs/gui.md`.

**History** (when you ask for `history`):

- For each variable, the library keeps up to that many points (`sampleMs`, `value`) in a ring buffer. The limit per variable is `HISTORY_MAX`; the oldest points are dropped first.
- Memory is allocated as points arrive, not when you ask.
- The history of all variables together holds up to `HISTORY_TOTAL_MAX` points (about 34 MB). Once that is reached, no history grows further: each keeps its current size and drops its oldest points (`history(ref)` returns fewer points than you asked for).
- Values of the same step are one point. If a changed value arrives for the same step, it replaces that point.
- When the run changes, the history starts empty.
- `watch.history(ref)` returns a new array on every call.
- Incoming `values` notifications are grouped by `ClientOptions.batchMs` (default `BATCH_MS` = 100 ms; `0` flushes on a microtask), so `values` notifies at that cadence while the history still holds every distinct sample step that arrived.

**When a limit is near**:

- If you list many long names and one subscription message would exceed the server's limit (256 KiB), the library switches the subscription for that period to "all" (and uses only the watched variables from what arrives).
- If the server rejects a watch (a server with different limits), the problem appears in `client.variables.watchProblem`.

### Write values (`enqueue` and `write`)

Variable writes follow a four-stage contract split across two native-Promise methods:

1. **Stages 1 + 2 (`enqueue(writes, options?) -> Promise<QueuedOperation>`)**: sends `POST api/write` targeting the current run (`session` in `client.variables.state`, sent as `run_id`). The server validates names/types/ranges, registers the write in FIFO `seq` order, and awaits gRPC `WaitOperation(WAIT_FOR_QUEUED)` until every target ECU leaves `sent` (either `queued` or immediately settled). `enqueue()` resolves with `{ session, seq, applied(waitOptions?) }` once the ECU queue barrier completes, or rejects immediately with `WriteError` if refused or already terminally failed (`invalid`, `busy`, `late`, `rejected`, `cancelled`, `unknown`, `mixed`).
2. **Stages 3 + 4 (`queued.applied(waitOptions?) -> Promise<OperationView>`)**: resolves once every target ECU applies the write in a virtual-time step (`applied`), or rejects with `WriteError`.
3. **Combined shorthand (`write(writes, options?) -> Promise<OperationView>`)**: equivalent to `enqueue(writes, options).then((q) => q.applied({ timeoutMs, signal }))`.

- The server rejects writes if that run is no longer running (`session-changed`). A write sent while one run ends and the next starts is never written to the next run.
- If the run is not known yet, the library does not send and rejects with `refused` (`not-running`).
- Unknown option keys reject immediately with `WriteError('refused', ..., 'unknown-field')`.
- The library never resends a write automatically.

The `kind` of a `WriteError`:

| `kind` | Meaning | Safe to resend? |
| --- | --- | --- |
| `refused` | The server or client rejected the write before it reached the launcher | Yes, after fixing the cause |
| `failed` | Accepted, but not applied (`invalid`, `busy`, `late`, `rejected`, `cancelled`, `unknown`, `mixed`) | Yes when no target applied it; check `operation.peers` if `mixed` or `unknown` |
| `no-answer` | **Unknown whether it was applied** | **Do not resend as is** (see below) |
| `closed` | `close()` was called before the result | Same as above |

- The `code` of `refused` is `unknown-variable`, `not-writable`, `invalid-field`, `unknown-field`, `invalid-argument`, `not-running`, `session-changed`, `variables-not-ready`, `write-busy`, `aborted`, or similar.
- The `operation` of `failed` holds the state and reason for each participant (`late`, `rejected`, `invalid`, and others).
- `no-answer` happens when:
  - The watchdog timer expires (`operation` holds the last known state).
  - The wait was aborted via `signal` (`code: 'aborted'`).
  - There was no HTTP response, or the server failed midway (500).
  - The server timed out waiting for the ECU queue barrier after assigning `seq` (`504 write-timeout`, `detail: { operation, state: 'sent' }`) or lost the Control Server connection midway (`502 write-failed`).
  - The run changed before the response.
  - The operation completed during a reconnection and the result was lost ("Reconnection" below).
  - The server could not keep track of all open operations and stopped following this one.

**Virtual-time-aware watchdog (`timeoutMs`)**:

`timeoutMs` (default `WRITE_TIMEOUT_MS` = 10,000 ms) must be a finite number greater than `0` (`Infinity` and `<= 0` are rejected with `invalid-argument`; values above 2^31-1 ms are capped there). The watchdog runs in four modes based on connection health, run state, and virtual time:

| Mode | When active | Behavior |
| --- | --- | --- |
| `armed-unhealthy` | WebSocket not synced/connected, `variables.status !== 'ready'`, or an incomplete target ECU is disconnected | Fixed `timeoutMs` timer; rejects with `no-answer` so a dropped link or dead ECU never hangs forever |
| `suspended` | Healthy, all incomplete target ECUs connected, and the run is `paused` or `holding` | Timer paused (including multi-rate ECUs waiting for their next step boundary). Pass `signal: AbortSignal.timeout(ms)` if you want a wall-clock cap while paused |
| `liveness` | Healthy, connected, running (`!paused && !holding`), before the target step is past due | Effective budget scales up for slow playback (`speed < 1`) and resets whenever `simMs` strictly advances; rejects if virtual time freezes (e.g., a deadlocked ECU) |
| `armed-due` | Healthy, connected, running, and virtual time has passed the expected due step without an operation result | Fixed effective timer without resets; rejects if the result never arrives |

**Avoid writing twice**: after `no-answer`, check before you resend.

- If `error.operation` is set, follow the operation with that number in `client.variables.operations`. If it is `queued`, it will still be applied once the simulation reaches that step.
- If there is no number, look in `operations` for a new operation that looks like your write, and check the variable's value (`watch`).
- Resend only once you know that the write was not applied.
- This matters most for writes that add to a value (writing twice gives a different result). Writing the same value twice gives the same result.

**Reconnection**: if the connection drops while you wait for a result, the summary after reconnecting (`hello`) tells how it went on. The summary holds every open operation the server keeps, and every operation whose number is `operations_from_seq` or higher (of those the server receives: the operations of a scenario's own writes never come).

| Your operation | What the library does |
| --- | --- |
| In the list | Uses its state (if still open, keeps waiting) |
| Not in the list, number below `operations_from_seq` | Treats it as completed while disconnected with its result lost: `no-answer` |
| Not in the list, and the server could not keep all open operations | Sets the old operations to `no-answer` |

- A write that has not been applied yet (for example, a write while paused) never becomes `no-answer` however many operations follow it.
- "The server could not keep all open operations" means more than 256 open operations (`operations_open_omitted` in the summary). The server no longer follows them, so the library reports `no-answer`, and the error message says why.

**Other options and state**:

- A write is applied in the step at the given virtual time (`atMs`; if omitted, the next step), so it does not complete while the simulation is paused. Because `enqueue()` awaits the ECU queue barrier, you can `await client.variables.enqueue(...)`, then call `step()` / `runUntil()`, then `await queued.applied()` with zero `sleep()` calls.
- `keepalive` (`WriteOptions.keepalive`, forwarded via `HttpRequest.keepalive` to `fetch(..., { keepalive })`) lets a release write sent from a browser `pagehide` handler outlive page unload while keeping `Authorization` and `Origin` headers. See "[Held inputs, reconnect recovery, and bus observation](gui.md#held-inputs-reconnect-recovery-and-bus-observation)" in `docs/gui.md`.
- `client.variables.operations` lists recent operations, including writes from other clients, but not a scenario's own writes.

### Write before the first step

A run started paused (`start_paused`) waits before the first step if the launcher can read and write variables. Meanwhile `isHolding(session)` is true (`holding` in the launcher's `state`, `launcherState` of the session copy), and you can resume and step (written values are applied in the step at time 0).

## Build widgets (`bind`)

For a widget that shows and writes one variable, such as a slider, a switch or a lamp, use `client.variables.bind(ref, options)` instead of combining `watch` and `write` yourself. It puts the value condition, whether the variable can be written and why not, and the write status into one `Readable` (`state`).

```ts
const throttle = client.variables.bind(
  { peer: 'Engine', name: 'engine.throttle' },
  { needsRange: true, needsValue: true },
);
throttle.state.subscribe(({ info, reading, pending, block, writeStatus, condition }) => {
  slider.disabled = block !== null; // block: why it cannot be written now; null if it can
  slider.value = String(pending ?? reading?.value ?? info?.min ?? 0); // pending: the value sent and waiting for its result
  status.textContent = isWriteError(writeStatus) ? writeStatus.message : writeStatus.kind;
  stale.hidden = condition.kind === 'live';
});
slider.onchange = () => throttle.write(Number(slider.value));
// when you are done
throttle.close();
```

Always call `close()` when you are done (it cleans up the watch and the subscriptions).

### Fields of `state` (`BoundVariable`)

| Field | Contents |
| --- | --- |
| `info` | The variable definition (`undefined` if the current run does not have the variable) |
| `reading` | The last value (`VariableReading`; `undefined` if there is none yet or while `visible` is false) |
| `availability` | Whether the ECU uses the variable now (`unknown`, `active`, `inactive`) |
| `condition` | The value condition ([Value condition (`condition`)](#value-condition-condition)) |
| `block` | Why the variable cannot be written now ([Why a write is blocked (`block`)](#why-a-write-is-blocked-block)) |
| `launcherReason` | When `block` is `launcher`: the launcher's reason (`code` and an English `message`, as `launcherAnswer` reads it); `null` otherwise |
| `writeStatus` | The status of the last write ([Write status (`writeStatus`, `pending`)](#write-status-writestatus-pending)) |
| `pending` | The value sent and still waiting for its result (`null` if none) |

- `subscribe` is called only on changes (get the first state with `state.get()`).
- `get()` always builds the state from the current values. So if you call `get()` during a notification of another `Readable` (such as `client.variables.state`), it may notify this `state`'s subscribers of a new state right there.

### Value condition (`condition`)

Switch on `condition.kind`.

| `kind` | Meaning |
| --- | --- |
| `unavailable` | The variable feature is not available (`status` holds the state of `client.variables.state`) |
| `missing` | The current run does not have the variable |
| `waiting` | The current run has the variable, but no value has arrived yet |
| `live` | The last value while the simulation runs |
| `paused` | The value at the paused time |
| `stale` | A value that may be stale (the reason is in `why`) |
| `ended` | The last value read in a run that ended |

The `status` of `unavailable` is one of: not running, this run has no variables, connecting, cannot connect. The values of `why` for `stale`:

| `why` | Meaning |
| --- | --- |
| `connection` | The connection between the client and the server dropped, or a cached reading after reconnecting to the same run is still awaiting its first post-reconnect `values` sample |
| `reconnecting` | The server is reconnecting to the launcher |
| `peer` | The ECU is disconnected |
| `reading` | Reading values keeps failing |

### Why a write is blocked (`block`)

`null` if the variable can be written. If several reasons apply, `block` is the one highest in the table.

The facts of the variable and the run come first, then the launcher's answer (`launcher`), then the client's prediction (`parameter`), then what the widget needs. Only `launcher` comes from the launcher; the other values are the client's own. The client has no rule of its own about what the launcher accepts: when the launcher takes a write during a scenario, `bind` lets it through. It reads the launcher's answer with `launcherAnswer(launcherState, 'write')`, as `controls` of the front door read theirs.

| `block` | Meaning |
| --- | --- |
| `read-only` | A read-only variable (`role` is `output`) |
| `disconnected` | The connection between the client and the server is down |
| `ended` | The run ended |
| `not-ready` | The variable feature is not available (not running, connecting, cannot connect) |
| `missing` | The current run does not have the variable |
| `peer-disconnected` | The ECU is not connected to the Control Server |
| `launcher` | The launcher does not take a write now (`allowed.write` of its `state`; `launcherReason` has its code and sentence, for example `scenario-running` while a scenario plays), or has not said yet that it does: until a `state` with `allowed.write` arrives, `unsupported` |
| `parameter` | A `parameter` can be written only before the first step of a run started paused (`isHolding(session)`: `holding` in the launcher's `state`). The client's prediction of the ECU's rule: the launcher does not say it for each variable |
| `no-range` | `needsRange` is set, but the variable has no range (`min` and `max`) |
| `no-value` | `needsValue` is set, and there is no value yet (neither read nor sent) |

### Write status (`writeStatus`, `pending`)

`writeStatus` is the status of the last write.

| `writeStatus` | Meaning |
| --- | --- |
| `idle` | Empty (no last write) |
| `sending` | Sent (awaiting `enqueue()` / ECU queue arrival) |
| `queued` | The ECU queued it (`seq` is the operation number) |
| `applied` | Applied (`atMs`) |
| `invalid` | The text passed to `reject(message)` (not sent) |
| `dropped` | A kept value was not sent ([How `write(value)` behaves](#how-writevalue-behaves)) |
| `refused`, `failed`, `no-answer`, `closed` | The `kind` of a `WriteError` (has `message` and `code`) |

- `isWriteError(writeStatus)` picks out the statuses that have a `message` (the ones that did not succeed).
- `message` is human-readable English whose wording may change (not localized). Branch on `kind` and `code`, and build localized text from them.
- If `pending` is set, show it instead of the read value (so a switch you clicked does not appear to flip back while it waits for the result).

### How `write(value)` behaves

- If the variable cannot be written (`block`), it does nothing.
- It returns no Promise; the result appears in `writeStatus`.
- Internally it uses `enqueue()` followed by `applied()` with the virtual-time-aware watchdog: a write while paused stays `queued` until the simulation advances, while a dropped connection or dead ECU still times out cleanly.
- While the previous write is still in `sending` (awaiting `enqueue()`), it keeps only the last of the next values instead of sending them, and sends it once the previous write reaches `queued` (so a dragged slider or repeated keys do not pile up writes).
- Before it sends a kept value, it checks again that the run is the same and the variable is still writable; if not, it does not send and sets `dropped`.
- If the previous write is rejected before it is queued, the kept value is not sent either.
- A write that got no result (`no-answer`) is never resent automatically.
- `reject(message)` reports an error that the widget found before sending (not a number, out of range) as `invalid`. It does not touch the write in flight, the kept value or `pending`.

### One state per variable

- However many places `bind` the same variable, they all see the same `writeStatus` and `pending`, and writes do not overlap.
- The state survives closing a `bind` and creating it again.
- The state belongs to the run: when the run changes, it starts empty (`idle`, `null`) and shows no results of writes from the previous run.

### Watch only while visible

`bind` receives values from the moment you create it (as with `watch`, the server has one subscription per variable). On a screen with many widgets, create it with `{ visible: false }`, call `setVisible(true)` when it becomes visible (`IntersectionObserver` or similar) and `setVisible(false)` when it is hidden. While `visible` is false:

| Field | While `visible` is false |
| --- | --- |
| `reading` | `undefined` |
| `condition` | `waiting` |
| `writeStatus`, `pending` | Reported as usual |
| `block` | Reported as usual (a `bind` with `needsValue` reports `no-value` unless there is a `pending` value) |

### History

If you create it with `{ history: 600 }`, `history()` returns the points, oldest first. `state` notifies every time a point is added; read the history again there.

### What is not checked, and after `close()`

- `write(value)` sends without checking the value's type and range. The server rejects a value that does not fit, and `writeStatus` becomes `refused`. To check first and show a message, check in the widget and call `reject(message)`.
- After `close()`, `write`, `reject` and `setVisible` do nothing.
- `close()` does not cancel writes already sent or the kept value. The kept value is sent once the previous write reaches `queued` (the state belongs to the variable, so the next `bind` of the same variable sees it).

## Lifecycle

| Call | What it does |
| --- | --- |
| `createClient()` | Only creates the client; does not connect |
| `connect()` | Connects over WebSocket (without a token, sends `token: ''` for `--no-auth`, or is rejected with `unauthorized` when auth is enabled) |
| `connect()` while connected | Does nothing (also while waiting to reconnect) |
| `close()` | Disconnects and does not reconnect (what it cleans up is below) |
| `connect()` after `close()` | Connects again and subscribes the remaining watches again |
| HTTP requests after `close()` | Work (`loadApp()`, `run.start()` and others can be sent without a connection) |
| `connect()` after a rejection or a version mismatch | Does nothing (`session.rejected` and `session.incompatible` do not reset) |

- `close()` stops pending timers, stops waiting for responses to time commands, and rejects writes waiting for results with a `WriteError` (`closed`). Notifications in a batch being collected are discarded. Variable watches remain.
- After `close()` and `connect()`, the session copy is still there, and the library receives again from where its copy ends.
- To connect with another token or version, create a new client.

## Coding rules

- Import nothing outside this library's own files (`eslint.config.js` blocks it; it also blocks dynamic `import()` except in the test helpers; `rootDir` in `tsconfig.json` blocks anything outside `src/`).
- Do not use anything that exists only in the browser (`window`, `document`, `localStorage` and so on). Take what you need through the options (`ClientOptions`).
- Human-readable text (such as `TimeFailure.message`) is in English. Turning it into the screen's language is the wrapper's job (built from machine-readable fields such as `kind`).
- When you add to the public API, add it to `src/index.ts` and to the [Public API](#public-api) tables above, and update [gui/client/README.md](../gui/client/README.md) and [Build your own screen for the browser GUI](howto/gui-screen.md) to match.

## Tests

The tools (TypeScript, ESLint, Prettier, Vitest, esbuild) are the ones in `node_modules` of `gui/web`. Run these in `gui/web` (`scripts/verify-web.py` runs them too):

| Command | What it does |
| --- | --- |
| `npm run test:client` | Type check (`tsc -p ../client`) and unit tests (Vitest; the library runs on Node, without Angular; the parts in `src/elements` run in a headless Chromium) |
| `npm run build:client` | Bundles into one ESM file, and the parts into a second one that imports it (esbuild) |
| `npm run pack:client` | Builds `@vehicle-sil/gui-client` `.tgz` (`gui/client/pack.mjs`), checks Node import and `nodenext`/`bundler` TypeScript examples |
| `npm run verify:consumers` | Unpacks the real `.tgz` into isolated consumer projects for Vanilla TS, Angular, React 18/19, Vue 3, and Svelte 5 (type checks + negative `@ts-expect-error` checks + Chromium runtime smoke) |
| `npm run test:timeline` | Verifies `<vehicle-sil-timeline>` bundle sync and viewer unit tests |
| `npm run lint` | `gui/web`, `gui/client`, and `gui/timeline` |
| `npm run format:check` | Formatting of `gui/web`, `gui/client`, and `gui/timeline` (`gui/.prettierrc`) |

The browser tests of the parts need Playwright's Chromium (`npx playwright install --only-shell chromium` in `gui/web`; `scripts/doctor.py` says whether it starts). They run headless; to watch them, run `npx vitest --config ../client/vitest.config.mjs --project browser --browser.headless=false` in `gui/web`.

The unit tests feed the examples in `gui/protocol/examples/` instead of building server notifications by hand (`testing/protocol-examples.ts`).

## See also

| To find | Document |
| --- | --- |
| Onboarding, `connect()` front door, recipes, 12 custom elements, and 5-framework setup | [gui/client/README.md](../gui/client/README.md) |
| Which way to add your own code to the GUI: a custom panel, your own screen or a tool | "Build your own screen" in [docs/gui.md](gui.md#build-your-own-screen) |
| How to build your own screen and have the GUI server serve it | [Build your own screen for the browser GUI](howto/gui-screen.md) |
| The server API (HTTP and WebSocket shapes, versioning and compatibility rules, authentication) | [gui/protocol/README.md](../gui/protocol/README.md) |
| The rules the library does not enforce for you | "Rules for building a screen" in [docs/gui.md](gui.md#rules-for-building-a-screen) |
| Files to start your own screen from | The screen template [examples/gui_starter](../examples/gui_starter/) |
| Examples that use the library from Node, without a screen | [examples/gui_client](../examples/gui_client/) |
