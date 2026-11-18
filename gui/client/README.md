# gui/client

[日本語](README.ja.md)

Reference for the client library of the GUI server (`vehicle-sil-gui`), written in TypeScript and published as `@vehicle-sil/gui-client`. It covers how to get and import the library, the public API, basic usage, reading and writing ECU variables, and the lifecycle.

The library handles the connection (token, reconnection), the session copy (a copy of the run state, built from the server's summary and events), run and time control, ECU variables (list, watching values, writes and their results), and the types and parsers of the server API. It depends on no other package, not even Angular or RxJS. The sample screens (`gui/web`) use only this library, through a thin Angular wrapper (`gui/web/src/app/core/session/`).

| To find                                                            | See                                                      |
| ------------------------------------------------------------------ | -------------------------------------------------------- |
| Where to get the library and how to import it                      | [Installing and importing](#installing-and-importing)    |
| Which names are public                                             | [Entry points](#entry-points), [Public API](#public-api) |
| The code to connect and start a run                                | [Basic usage](#basic-usage)                              |
| Reading and writing variable values                                | [ECU variables](#ecu-variables)                          |
| What a screen shows and which buttons work: `connect()`            | [The front door (`connect()`)](#the-front-door-connect)  |
| The elements to put in HTML, with no script                        | [Parts (custom elements)](#parts-custom-elements)        |
| Building sliders and lamps (the values of `condition` and `block`) | [Build widgets (`bind`)](#build-widgets-bind)            |
| What happens after `connect()` and `close()`                       | [Lifecycle](#lifecycle)                                  |
| Developing the library                                             | [Coding rules](#coding-rules), [Tests](#tests)           |

## Installing and importing

The library is TypeScript source with no dependencies. It is not published to the npm registry.

| Use                                             | How to get it                                                                                            | Import                                                       |
| ----------------------------------------------- | -------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------ |
| A page that the GUI server serves (`--web`)     | Nothing to copy: the server serves the files (below)                                                     | `import { connect } from '/vsil/vehicle-sil-gui-client.mjs'` |
| Plain JavaScript, outside such a page           | Put `vehicle-sil-gui-client.mjs`, the library bundled into one ESM file, next to your script             | `import { connect } from './vehicle-sil-gui-client.mjs'`     |
| TypeScript and any framework                    | Get the package `vehicle-sil-gui-client-<version>.tgz` and run `npm install <that .tgz>` in your project | `import { connect } from '@vehicle-sil/gui-client'`          |
| A TypeScript app in this repository (`gui/web`) | Point the entry point names to `src/` with `paths` in `tsconfig.json`                                    | `import { createClient } from '@vehicle-sil/gui-client'`     |

Where to find the 3 files:

| File                                   | Prebuilt tools                                        | Build in the repository (run in `gui/web`)                                            |
| -------------------------------------- | ----------------------------------------------------- | ------------------------------------------------------------------------------------- |
| `vehicle-sil-gui-client.mjs`           | `<tools>/client/vehicle-sil-gui-client.mjs`           | `npm run build:client` creates `build/gui-client/vehicle-sil-gui-client.mjs`          |
| `vehicle-sil-gui-elements.mjs`         | `<tools>/client/vehicle-sil-gui-elements.mjs`         | `npm run build:client` creates `build/gui-client/vehicle-sil-gui-elements.mjs`        |
| `vehicle-sil-gui-client-<version>.tgz` | `<tools>/client/vehicle-sil-gui-client-<version>.tgz` | `npm run pack:client` creates `build/gui-client/vehicle-sil-gui-client-<version>.tgz` |

The releases page has the same `.tgz`. `build/` is the one at the top of the repository and is not tracked in Git.

The GUI server also serves the two `.mjs` files, without the token, as `/vsil/vehicle-sil-gui-client.mjs` and `/vsil/vehicle-sil-gui-elements.mjs` (in the repository, from `build/gui-client/`; it says what to run when they are not built). A screen without a build can load them from there and copy nothing.

- **Bundled file**: loads in the browser and in Node (22 or later). `scripts/verify-gui.py` runs an example that uses this file from Node against the replay launcher.
- **Package**: contains the bundled ESM, `.d.ts` files, `exports`, the README and the LICENSE (`pack.mjs` builds it). Its version is the same as the GUI server's. Its types use the browser types (`DOM` in `lib`). Its entry points are `.`, `./elements` and `./timeline` (`testing` and `internal` are not included).

## Entry points

| Entry point                                                                                                     | Name                               | Contents                                                                                                                                                                                                   |
| --------------------------------------------------------------------------------------------------------------- | ---------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/index.ts`                                                                                                  | `@vehicle-sil/gui-client`          | The public API ("Public API" below). This entry point and `./elements` are kept compatible                                                                                                                 |
| `src/testing/index.ts`                                                                                          | `@vehicle-sil/gui-client/testing`  | Test helpers (usable in tests of your own widgets)                                                                                                                                                         |
| `src/internal.ts`                                                                                               | `@vehicle-sil/gui-client/internal` | **No compatibility guarantee.** Internal classes and functions; do not use them in apps                                                                                                                    |
| `src/elements/index.ts` (in the package as `dist/elements.js`, which imports `dist/index.js`)                   | `@vehicle-sil/gui-client/elements` | The parts (custom elements `<vsil-status>` and 10 more), `pageIntent` and `setReasonText`: importing it defines them ("Parts (custom elements)" below). It needs a DOM; the main entry does not import it  |
| `../timeline/vehicle-sil-timeline.js` (in the package as `timeline/vehicle-sil-timeline.mjs`, with `export {}`) | `@vehicle-sil/gui-client/timeline` | The element `<vehicle-sil-timeline>`, with its types: importing it defines the element ("Putting a timeline on your own screen" in [docs/gui.md](../../docs/gui.md#putting-a-timeline-on-your-own-screen)) |

- `testing` contains a fake WebSocket (`FakeWebSockets`), a fake HTTP (`FakeHttp`), loaders for `gui/protocol/examples/`, and `sessionViewFrom`, which builds a `SessionView` from a summary.
- `internal` contains the internal classes, the functions that apply a summary, and the parsers. They are public for tests only, and change or disappear without notice.

The public API is kept small, because adding to it keeps compatibility and removing from it breaks compatibility. If the public entry point lacks something you need, do not reach into `internal`; decide whether to add it to the public API.

## Public API

The names that `src/index.ts` exports, by group. Parentheses list the members of the object.

### Client

| Kind     | Name                                                                                                                                                                                                                                    |
| -------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Function | `createClient(options)`                                                                                                                                                                                                                 |
| Type     | `ClientOptions` (`baseUrl`, `token`, `origin`, `http`, `socket`, `answerTimeoutMs`, `onError`)                                                                                                                                          |
| Type     | `GuiClient` (`connection`, `session`, `run`, `time`, `variables`, `app`, `limits`, `connect()`, `close()`, `loadApp()`, `configs()`, `check()`, `connections()`, `fetchSession()`, `dashboard(file)`, `scenarios(file)`, `recording()`) |
| Constant | `DEFAULT_LIMITS`                                                                                                                                                                                                                        |

### Front door

| Kind     | Name                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| -------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Function | `connect(options?)`, `createFront(options)`, `watch(source, fn)`, `until(source, test, options?)`                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| Class    | `ActionError` (`code`, `message`, `outcome`, `findings`)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| Type     | `ConnectOptions` (`base_url`, `token`, `origin`, `http`, `socket`, `answer_timeout_ms`, `on_error`), `Front` (`low`, `status`, `clock`, `controls`, `scenario`, `outside`, `run`, `time`, `variable()`, `connections()`, `check()`, `close()`), `FrontStatus`, `FrontResult`, `FrontClock`, `FrontControls`, `Control`, `Reason`, `FrontScenario`, `FrontRun`, `FrontTime`, `FrontVariable`, `VariableView`, `Intent`, `Outside`, `ConnectEntry`, `ConnectTarget`, `ConnectListError`, `Link`, `RunState`, `EndKind`, `Pacing`, `OperationName` |

### Token

| Kind     | Name                |
| -------- | ------------------- |
| Function | `takeToken(place)`  |
| Type     | `TokenPlace`        |
| Constant | `TOKEN_STORAGE_KEY` |

In the browser, call `takeToken(window)`. It reads `#token=` from the URL, removes it from the URL and stores it in sessionStorage. If the URL has no token, it returns the one in sessionStorage; if neither has one, it returns `null`.

### Connection (read-only)

| Kind     | Name                                                                                               |
| -------- | -------------------------------------------------------------------------------------------------- |
| Type     | `ClientConnection` (`state`, `failures`, `lastClose`, `tryAgainReason`, `onBatch`, `onUnreadable`) |
| Type     | `ConnectionState`, `CloseInfo`                                                                     |
| Constant | `BATCH_MS`, `TRY_AGAIN_LATER`                                                                      |

### Session copy

| Kind     | Name                                                                                                                                           |
| -------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| Type     | `ClientSession` (`session`, `ready`, `allowed`, `lastEventAt`, `notices`, `rejected`, `incompatible`, `onMessages`, `report`, `dismissNotice`) |
| Type     | `Notice`, `SessionView`, `ParticipantView`, `LauncherView`, `LogLine`, `LogSource`, `TimeView`, `CommandView`, `RunOutcome`, `LauncherAnswer`  |
| Function | `runOutcome`, `phaseIndex`, `isHolding`, `launcherAnswer`                                                                                      |

`ClientSession.allowed` is what the GUI server accepts now (`start`, `stop` and `force`, each with `ok`, a `reason` code and a `message`; the larger `rev` wins; `null` until the server says). `SessionView.launcherState` is the launcher's last `state` event (where the run stands and what it accepts; the larger `rev` wins; `null` until one arrives). `launcherAnswer(launcherState, name)` reads what it says of one operation (`write`, `pause`, ...): `reason` (`null` when the launcher accepts it) and `caution`. A launcher that has not said (no `state` yet, or no entry for the operation) does not accept it (`unsupported`). The front door's `controls` and `variables.bind` read the launcher with it.

### Run control

| Kind | Name                                                                                             |
| ---- | ------------------------------------------------------------------------------------------------ |
| Type | `ClientRun` (`starting`, `stopping`, `stopWhileStarting`, `error`, `start`, `stop`, `forceStop`) |
| Type | `StopAction`                                                                                     |

### Time

| Kind     | Name                                                                                     |
| -------- | ---------------------------------------------------------------------------------------- |
| Type     | `ClientTime` (`state`, `answerTimeoutMs`, `pause`, `resume`, `step`, `setSpeed`, `send`) |
| Type     | `TimeState`, `TimeFailure`, `Lateness`, `SendResult`, `NotSentReason`                    |
| Constant | `ANSWER_TIMEOUT_MS`, `FEATURE_SPEED`, `FEATURE_START_PAUSED`, `FEATURE_TIME_COMMANDS`    |

### ECU variable API

| Kind     | Name                                                                                                                                                                  |
| -------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Type     | `ClientVariables` (`state`, `operations`, `watchProblem`, `watch`, `write`, `bind`)                                                                                   |
| Type     | `VariableWatch` (`values`, `value`, `history`, `close`)                                                                                                               |
| Type     | `VariablesState`, `VariablesStatus`, `VariablePeer`, `VariableInfo`, `VariableRef`, `VariableValue`, `WatchOptions`, `WatchValues`, `VariableReading`, `HistoryPoint` |
| Type     | `WriteOptions`, `OperationView`, `OperationPeer`, `WatchProblem`                                                                                                      |
| Class    | `WriteError`                                                                                                                                                          |
| Constant | `WATCH_PERIOD_MS`, `WATCH_PERIODS_MS`, `WRITE_TIMEOUT_MS`, `HISTORY_MAX`, `HISTORY_TOTAL_MAX`, `OPERATIONS_KEPT`                                                      |

### Binding variables to widgets (`bind`)

| Kind     | Name                                                                                                                      |
| -------- | ------------------------------------------------------------------------------------------------------------------------- |
| Method   | `client.variables.bind(ref, options)`                                                                                     |
| Type     | `BindOptions` (`periodMs`, `history`, `needsRange`, `needsValue`, `visible`)                                              |
| Type     | `VariableBinding` (`ref`, `state`, `history()`, `write(value)`, `reject(message)`, `setVisible(visible)`, `close()`)      |
| Type     | `BoundVariable` (`ref`, `info`, `reading`, `condition`, `block`, `launcherReason` (a `Reason`), `writeStatus`, `pending`) |
| Type     | `ValueCondition`, `WriteBlock`, `WriteStatus`, `WriteValue`                                                               |
| Function | `isWriteError`, `hasRange`                                                                                                |
| Function | `valueCondition` (`ConditionInput`), `writeBlock` (`WriteBlockInput`)                                                     |
| Constant | `DROPPED_MESSAGE`                                                                                                         |

`valueCondition`, `writeBlock` and `hasRange` are the pure functions that `bind` builds its state with.

### `Readable` (read-only value)

| Kind | Name                                     |
| ---- | ---------------------------------------- |
| Type | `Readable<T>` (`get()`, `subscribe(fn)`) |

### APIs for substitution

| Kind     | Name                                                                   |
| -------- | ---------------------------------------------------------------------- |
| Type     | `SocketFactory`, `SocketHandlers`, `SocketLike`                        |
| Type     | `HttpTransport`, `HttpRequest`, `HttpOutcome`, `FetchTransportOptions` |
| Function | `webSocketFactory`, `fetchTransport`                                   |
| Constant | `DEFAULT_TIMEOUT_MS`                                                   |

### Errors

| Kind     | Name                                                        |
| -------- | ----------------------------------------------------------- |
| Class    | `HttpError` (`status`, `statusText`, `problem`, `timedOut`) |
| Class    | `ProtocolError`                                             |
| Function | `errorMessage`                                              |

### Server API types and parsing

Types for the shapes that `gui/protocol` defines, and the parsers to use when you read the server API yourself.

| Subject                                                    | Name                                                                                                                                                                                                                                               |
| ---------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Types of the `gui/protocol` shapes                         | `SessionSummary`, `ServerMessage`, `LauncherEvent`, `CheckResult`, `RunResult`, `AppInfo` and others                                                                                                                                               |
| Constants and functions                                    | `PROTOCOL_VERSION`, `PHASES`, `severityRank`                                                                                                                                                                                                       |
| Parsers                                                    | `readServerMessage`, `readAppInfo`, `readConfigList`, `readCheckResponse`, `readConnections`, `readSessionResponse`, `readTimeResponse`, `readWriteResponse`, `readVariablesSummary`, `readDashboardResponse`, `readScenarioList`, `readRecording` |
| Shapes of the variable API                                 | `VariablesSummary`, `VariableSummary`, `ValuesMessage`, `OperationReport`, `WatchMessage`, `WriteAnswer`                                                                                                                                           |
| Dashboard files (`dashboard(file)`)                        | `DashboardResponse`, `DashboardFile`, `DashboardFileSection`, `DashboardFileItem`, `DashboardFileError`, `DashboardPanelModule`, `DashboardVariable`                                                                                               |
| Scenarios (`--scenario`, `scenarios(file)`, `recording()`) | `ScenarioStep`, `ScenarioStepStatus`, `ScenarioStatus`, `CheckScenario`, `ScenarioResult`, `SummaryScenario`, `ScenarioList`, `ScenarioFile`, `Recording`, `FEATURE_SCENARIO`, `FEATURE_SCENARIO_CONTROL`                                          |
| Request shapes                                             | `CheckRequest`, `RunRequest`, `ConnectionsRequest`, `Speed`, `StopRequest`, `TimeCommand`                                                                                                                                                          |
| List of configuration files                                | `ConfigList`, `ConfigFile`                                                                                                                                                                                                                         |
| Error shape                                                | `ServerProblem`                                                                                                                                                                                                                                    |
| Findings                                                   | `Finding`, `Severity`                                                                                                                                                                                                                              |
| The launcher's keys and traffic                            | `FromLauncher`, `LauncherFields`, `RunTraffic`, `TrafficNetwork`, `TrafficParticipant`, `TrafficUnplaced`, `ParticipantRole`, `OutsideState`                                                                                                       |
| Participants you start yourself (`manual`)                 | `FEATURE_MANUAL`                                                                                                                                                                                                                                   |
| `SessionView.scenario` of the session copy                 | `ScenarioView`                                                                                                                                                                                                                                     |

## Basic usage

This is the lower door of the library, `createClient`, which the front door is built on (it is `client.low` there). A screen starts with `connect()` and the parts ("The front door (`connect()`)" and "Parts (custom elements)" below) and uses this door for what they do not cover: events, logs, findings, participants and `bind`.

```ts
import { createClient, takeToken } from '@vehicle-sil/gui-client';

// A screen opened from the URL the server printed (http://127.0.0.1:<port>/#token=<token>): take #token= from the URL and remove it.
// A Node tool takes baseUrl and the token out of that URL itself and passes them (examples/gui_client).
const client = createClient({
  baseUrl: new URL('/', location.href).href,
  token: takeToken(window),
});
await client.loadApp(); // the launcher's features (whether it has time commands, and so on)
client.connect();
const stop = client.session.session.subscribe((session) => {
  if (session?.result) console.log(session.result.result);
});
await client.run.start({ file: 'simulation.yaml', disable: [] });
```

### Read state (`Readable`)

Every state is a `Readable` (a read-only value).

| Call                  | What it does                                                       |
| --------------------- | ------------------------------------------------------------------ |
| `get()`               | Returns the current value                                          |
| `subscribe(fn)`       | Notifies `fn` of later changes (not called with the current value) |
| The returned function | Ends the subscription                                              |

- Notifications are synchronous: they arrive inside the code that changed the value.
- A subscriber that unsubscribes during a notification still receives that notification.
- If a subscriber changes the same value, every subscriber receives the new value, and the old value does not arrive afterward.
- A framework wrapper takes the first value with `get()` and converts later values into its own state with `subscribe()` (Angular uses `readableSignal`).
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

For the rules the library does not enforce for you (request fields that match the launcher's `features`, whether a variable is writable, whether a value is stale), see "Rules for building a screen" in [docs/gui.md](../../docs/gui.md#rules-for-building-a-screen).

## The front door (`connect()`)

`connect()` returns the front door of the library: what a screen shows (`status`, `clock`, `controls`, `scenario`), the operations as Promises (`run`, `time`) and one variable at a time (`variable()`). It is built on the client described above and changes nothing in it: `front.low` is the same `GuiClient` (events, logs, findings, participants), and `createClient` still works as before. Use the front door for what a screen needs to decide ("which state is this", "may this button be pressed now, and if not, why"); use `low` for the rest.

An app that must show its screens before the server answers, and while it cannot be reached (the standard screens), uses `createFront(options)` instead: it makes the front door without connecting, as `createClient` does, and the app calls `front.low.connect()` and `front.low.loadApp()` itself. Until `loadApp()` has answered, the front knows no start-up file and no launcher features.

```ts
import { ActionError, connect, until, watch } from '@vehicle-sil/gui-client';

// A page that the GUI server served, opened from the URL it printed: no options. The first call connects; the next calls
// (another part of the page) get the same connection. In Node, or for a second connection, pass the server's URL and the token.
const client = await connect();
// const client = await connect({ base_url: 'http://127.0.0.1:8765/', token });

const stop = watch([client.status, client.controls], (status, controls) => {
  startButton.disabled = !controls.start.ok;
  startButton.title = controls.start.reason?.message ?? '';
  banner.textContent = status.problem?.message ?? '';
});

try {
  await client.time.step(100); // resolves when time has arrived and the run is paused again
} catch (error) {
  if (error instanceof ActionError) console.log(error.code, error.message, error.outcome);
}
```

| Name                                                               | What it is                                                                                                                                                                                                                                                                  |
| ------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `status`                                                           | The one state to show: `link`, `run`, `file`, `result` and `problem` (below). Discrete values only                                                                                                                                                                          |
| `clock`                                                            | `sim_ms`, `speed`, `actual_speed` and `pacing` (`not-paced`, `on-time` or `late`). It changes often, so it is apart from `status`                                                                                                                                           |
| `controls`                                                         | Whether each operation can be used now: `start`, `stop`, `force_stop`, `pause`, `resume`, `step` (with `min_ms` and `max_ms`), `run_until`, and `speed` (with `min`, `max` and `fastest`: whether `'max'` is accepted besides the numbers)                                  |
| `scenario`                                                         | The scenario of the current (or last) run: `file`, `name`, `status`, `steps`, `timeline_url`; `null` for a run without one                                                                                                                                                  |
| `outside`                                                          | The networks that the run's `connect` connected to a real bus ("Connecting a network outside" below)                                                                                                                                                                        |
| `connections({ network }, { file }?)`, `check(intent?, { file }?)` | Where a network can be connected, and the check of an intent without starting it ("Connecting a network outside" below)                                                                                                                                                     |
| `run`                                                              | `start(intent?, { file }?)`, `stop()` and `forceStop()`                                                                                                                                                                                                                     |
| `time`                                                             | `pause()`, `resume()`, `step(ms)`, `setSpeed(value)` and `runUntil(ms)`                                                                                                                                                                                                     |
| `variable(ref)`                                                    | One variable, by `"ECU.variable"` (cut at the first dot) or `{ peer, name }` (for an ECU whose name has a dot)                                                                                                                                                              |
| `low`, `close()`                                                   | The `GuiClient` of the same connection; `close()` disconnects (a shared connection is made again by the next `connect()`)                                                                                                                                                   |
| `watch(source, fn)`, `until(source, test, options)`                | `watch` calls `fn` now and on every change (several sources: `watch([a, b], (a, b) => ...)`) and returns the function that stops it. `until` resolves with the first value that satisfies `test`; it rejects with an `ActionError` (`timeout` or `aborted`, both `unknown`) |

Data fields use the wire's names (`force_stop`, `sim_ms`); functions use JavaScript's (`forceStop`, `runUntil`). Every state is a `Readable`; `get()` returns the same object while nothing changed, and `get` and `subscribe` work without `this` (they can be passed to `useSyncExternalStore` as they are).

### `status`

| Field     | Values                                                                                                                                                                                                                                                                                                                                                                   |
| --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `link`    | `connecting`, `online`, `offline`, `rejected` (the token was refused) or `incompatible` (another protocol version). `online` means the copy matches the server                                                                                                                                                                                                           |
| `run`     | `none`, `starting` (a start is on its way, from this page or another), `waiting` (the launcher waits for the participants to join), `running`, `paused`, `stopping`, `ended` (there is a result) or `crashed` (the launcher ended without one)                                                                                                                           |
| `file`    | The configuration file of the run (the last one if none is running); the server's start-up file before the first run                                                                                                                                                                                                                                                     |
| `result`  | While `run` is `ended` or `crashed`: `end` (`completed`, `stopped`, `interrupted`, `failed`, `setup_failed` or `crashed`; `null` when nothing was started: the check failed, or the file or the command line could not be used), `scenario`, `ok` and `message` (one English sentence). A stop by the operator is `ok`, except when it stopped a scenario before its end |
| `problem` | The one problem to tell the user now (`{ code, message }`), or `null`                                                                                                                                                                                                                                                                                                    |

`problem` is chosen in this order: the connection first (the token refused, another protocol version, offline); otherwise the newest of the launcher's crash, the failure of the last operation and a failed run (`result.ok` is false). A crash and a failed run go when the next run begins. The failure of an operation goes at the next operation or when `status.run` changes. A stop by the operator is not a problem, except when it stopped a scenario before its end (`result.ok` is false then, and the message says that the scenario has no verdict). The failure of a variable write is in the variable's own `problem`, not here. The server's notices (`low.session.session` `problems`) and `notices` stay in `low`.

`result.end`, `result.ok` and `result.message` are the launcher's: it writes them into its result (`end`, `ok` and `message`; see "How a run ended" in [simulation-run.md](../../docs/simulation-run.md#how-a-run-ended-end-ok-message)), and the library passes them on. The library has no table of result names. Only `crashed` is the library's own (`ok` is false, with a sentence of its own), because a launcher that ended without a result wrote nothing.

### `controls`

Each entry is `{ ok, reason, caution }`: `reason` is `{ code, message }` when `ok` is false, `caution` is the same shape for an operation that is accepted with something to know first. The codes stay the same (a new code may be added; the meaning of one does not change); the sentence is English, written by the owner of the rule.

| Entry                                           | Comes from                                                                                                                                                                                                                                                                                                                                  |
| ----------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `start`                                         | The GUI server's `allowed.start` (`starting`, `already-running`, `launcher-unusable`, `shutting-down`), with its sentence                                                                                                                                                                                                                   |
| `stop`, `force_stop`                            | The GUI server's `allowed.stop` and `allowed.force`. `stop` is `ok` for the last known run (or a start on its way), also offline, and while the server says the launcher still runs after its result. `force_stop` is `ok` only after a first stop (`not-stopping` before)                                                                  |
| `pause`, `resume`, `step`, `speed`, `run_until` | The launcher's `state.allowed` (reason, sentence and `caution` as the launcher wrote them; the ranges of `step` and `speed`; `fastest` is false when `min` equals `max`, as the launcher promises). No run in the copy: `not-running`; a launcher without `state`: `unsupported`                                                            |
| `pause`, `resume` (what would change nothing)   | `pause` is not `ok` while the run is paused (`paused`), `resume` is not `ok` while it is not (`not-paused`): the launcher accepts both, but a button has nothing to do. While a step or a run_until is on its way (accepted, and not yet seen to arrive: virtual time at its `at_ms` and the launcher's `state` paused), neither is claimed |
| `run_until`                                     | `unsupported` until the launcher has the feature `stdin-run-until`                                                                                                                                                                                                                                                                          |
| All of them                                     | The connection: `rejected`, `incompatible`, `offline` or `connecting` turn them off (`stop` and `force_stop` stay as above); `busy` while the same operation of this page has not been answered                                                                                                                                             |

### Operations

`run.start(intent, { file })` takes the intent: `vehicle_sil_run_request` (1, optional), `variant`, `params`, `scenario`, `disable`, `enable`, `manual`, `strict`, `speed`, `start_paused`, `connect` and `time_policy`, as the launcher's request file names them (any other field is refused with `unknown-field`; a choice that is not made is a key that is left out, never `null`); `file` defaults to the server's start-up file. Every operation is a Promise. It rejects with an `ActionError`: `code`, a sentence (`message`), `findings` (for `check-failed`) and `outcome`: `refused` (the owner said no; nothing happened) or `unknown` (it may have been done: look at `status` and `clock` before you send it again).

| Operation                          | Resolves when                                                                                                                                                                                                                                                                                                                                       | Rejects with                                                                                                                                                                                                                                                                              |
| ---------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `run.start`                        | The server took the request. Whether the run began is `status.run` (wait with `until(client.status, s => s.run !== 'starting')`, then read `run`)                                                                                                                                                                                                   | The refusal (`check-failed` with the `findings`, `already-running`, ...). No answer in time: `no-answer` (`unknown`: it may have started)                                                                                                                                                 |
| `run.stop`, `run.forceStop`        | The server took the request. Sent whatever the state of the connection (HTTP may reach the server when the WebSocket is down); asking again while one is on its way is the same request                                                                                                                                                             | The same                                                                                                                                                                                                                                                                                  |
| `time.pause`, `resume`, `setSpeed` | The launcher answered. `pause` while paused and `resume` while running send nothing and resolve, only when the connection is `online` and a run takes the operation                                                                                                                                                                                 | The launcher's `reason` and sentence (`refused`); no answer within 5 seconds: `no-answer` (`unknown`); the run ended before the answer: `not-running` (`unknown`)                                                                                                                         |
| `time.step`, `time.runUntil`       | Time has arrived and the run is paused again, so `clock` can be read after `await`. `runUntil(t)` arrives at `t` (a `t` the paused run has already passed resolves at once and time does not move); `step(ms)` at the `at_ms` the launcher writes in the answer (older launcher: the time it was accepted plus the `ms` it rounded to, an estimate) | As above; and `interrupted` (`unknown`) when a resume or a pause of another sender (another page or stdin) comes before it arrives (a pause or resume of this page, or a stop, rejects it the same way but is not shown in `status.problem`). Another step sent meanwhile does not end it |

The client refuses before sending only what it owns: the connection (`rejected`, `incompatible`), the same operation still waiting for its answer (`busy`; this includes `start()`) and the shape of an argument (`invalid-argument`). Anything else is sent even if `controls` says no, and the owner's answer comes back (the copy may be old). The 5 seconds are the front door's own (they do not apply to the wait for arrival, which ends when the run ends or another command interrupts it).

A time command carries a request number (`id`) that the page chose, and the answer is the launcher's `command` event with the same `id`; an answer to another page's or stdin's command with the same name is not taken for it. This needs a launcher that writes `state`: for an older one the GUI server refuses the command (`unsupported`).

### Connecting a network outside

A `can` or `ethernet` network can be connected to a real bus through the launcher's bridge ("Connecting a network outside" in [gui/protocol/README.md](../protocol/README.md#connecting-a-network-outside-connect-time_policy-apiconnections)). The front door adds three things for it, with no rule of its own.

```ts
const entries = await client.connections({ network: 'ETH_EXT' }); // when the user opens the choice
const intent = { connect: { ETH_EXT: 'fake0' }, time_policy: 'free' as const };
const result = await client.check(intent); // the launcher's findings, before anything starts
await client.run.start(intent);
const stop = watch(client.outside, (rows) => console.log(rows.map((r) => r.condition)));
```

- **`connections({ network }, { file }?)`** asks `POST api/connections` and lays the answer out as one list, in its order: `{ backend, value, label, detail, usable, reason }` for each target, and `{ backend, error }` (the launcher's sentence) for a backend whose bridge could not list, or for an error of the file. Tell the two apart with `'error' in entry`. A list that lacks a backend (exit code 1) is not a failure. It starts that network's bridge on the computer of the GUI server (on Windows, Npcap may ask to raise the rights there), so call it when the user opens the choice, not when the page opens. The list is that computer's, not the browser's. It rejects with an `ActionError` (`check-busy`, `unknown-network`, ...).
- **`intent.connect`** maps a network to a `value` that the list gave with `usable: true`, as written there; `'none'` keeps it on SIL Kit only, the same as leaving it out. The server refuses a value that its last list of that network did not give (`target-not-listed`): list the network again. **`intent.time_policy`** is `realtime` (the launcher's default: a connected run keeps real time) or `free`.
- **`check(intent?, { file, signal }?)`** returns the launcher's check result of what `run.start(intent)` would start (`findings`; `time_lock` and `networks[].outside` are in `raw`), or rejects with an `ActionError`. Its failure is not put into `status.problem`. It sends one check at a time: the next is sent after the answer to the one before (the server runs checks one at a time and refuses with `check-busy` when too many wait). A check of the same intent and file as one that waits or is on its way shares it, so the parts and the screen of a page that check the same page intent make one check. A check that waits is not sent when `signal` has aborted for every caller of it; it rejects with the signal's reason. A check on its way is shared too, and its answer reads the file as it was when it was sent: pass `fresh: true` when the user asks to check again (it shares only a check that waits). After `close()`, a check that waits is not sent and rejects with `closed`.
- **`outside`** has one row for each network that the run's `connect` connected: `network`, `target`, `participant`, `link` (Ethernet) or `bus` (CAN), `condition` (`ok`, `interrupted`, `degraded`), `reasons` (codes, in the launcher's order), `nothing_received` (apart from `condition`), and `to_outside` and `from_outside` (`null` until an `outside` event has arrived on this page). It is empty while no run is going, and in a run that a `role: bridge` participant of the file connects: do not read from it whether the run is connected outside (`time_lock.because` of the check says what makes the run keep real time). How the run ended, `connect-failed` and `connect-lost` included, is in `status`.

### One variable

```ts
const throttle = client.variable('Engine.engine.throttle');
const stop = throttle.subscribe((view) => {
  slider.value = String(view.value ?? 0);
  slider.disabled = !view.can_write;
  slider.title = view.why?.message ?? view.note ?? '';
});
await throttle.write(40); // resolves when it was applied in an ECU step
stop(); // the last unsubscribe stops asking the server for values
```

The view has `value`, `text` (an enum shows the item's name), `unit`, `min`, `max`, `step`, `sim_ms`, `stale`, `note` (a sentence when the value is not a live one), `availability` (`unknown`, `active` or `inactive`: whether the ECU uses the variable now; information only, so `can_write`, `why` and `note` do not look at it, and a lost connection does not change it; `unknown` until the run's summary has the variable, then `active` with a GUI server that does not send it), `can_write` (the variable's own conditions and what the launcher accepts: `state.allowed.write`), `why` (a `{ code, message }` for what cannot be written: the variable's own reason first, then the launcher's), `writing`, `problem` (why the last write of this page did not go through), and the values of `client.low.variables.bind` (`condition`, `block`, `write_status`).

- The first subscription asks the server for the value, and the last unsubscribe stops it. There is no `close()`.
- `write(value)` needs no subscription (it sends that one write). It rejects with an `ActionError`; `no-answer` is `unknown`, and a write whose answer did not come is not sent again.
- The binding (`client.low.variables.bind`) holds back and merges writes while one is on its way (for sliders); `write()` of the front door sends each call.

## Parts (custom elements)

`@vehicle-sil/gui-client/elements` defines 11 custom elements that draw what `connect()` returns, so that a screen can be written as HTML without a script. They use native controls in the light DOM and have no styles of their own: your CSS styles them, and they work in plain HTML and in the templates of Angular, React, Vue and Svelte. Importing the module defines them (a name that is defined already is skipped).

| Element                  | What it shows or does                                                                                                                                  | Attributes and properties                                                                                                                                                                                                                                                                                                                                                                                                                    |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `<vsil-status>`          | The connection, the run, the virtual time (marked when it falls behind real time) and the one problem                                                  | none                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| `<vsil-run-controls>`    | Start (with the page's selection), Stop and, after a first stop, Stop without waiting                                                                  | property `intent` (what the next run starts with); `label-start`, `label-stop`, `label-force-stop` (the button texts)                                                                                                                                                                                                                                                                                                                        |
| `<vsil-time-controls>`   | Pause, Resume, Step, Run until and the speed                                                                                                           | `speeds` (the speeds to choose from, separated by spaces; default `0.5 1 5 max`; a speed outside the range of `controls.speed` cannot be chosen, and `max` is not offered while `fastest` is false; the part does not say why, so a page that wants to say it reads the range from `client.controls`), `step-ms` (the size of a step in ms; default `10`); `label-pause`, `label-resume`, `label-step`, `label-run-until` (the button texts) |
| `<vsil-value>`           | The value and its unit, and a sentence when it is not live. A bool shows as the `on` and `off` text                                                    | `variable`; `on`, `off` (the text for a bool's true and false); `no-unit` (leave the unit out)                                                                                                                                                                                                                                                                                                                                               |
| `<vsil-slider>`          | A number in the variable's range; it is written when the knob is released                                                                              | `variable`                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `<vsil-write>`           | A button that writes one value (not usable when the variable already has it)                                                                           | `variable`; `value` (what to write: JSON such as `false` or `3`, otherwise text); `label` (the button text)                                                                                                                                                                                                                                                                                                                                  |
| `<vsil-scenario-select>` | The scenario of the next run, among those next to the run's configuration file (the first choice is none)                                              | none                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| `<vsil-scenario-result>` | The status and the steps of the run's scenario                                                                                                         | none                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| `<vsil-timeline>`        | The timeline of the run's scenario, in the `<vehicle-sil-timeline>` that you put inside it                                                             | none; the attributes of that element are yours (see below)                                                                                                                                                                                                                                                                                                                                                                                   |
| `<vsil-connect>`         | Where one network of the next run is connected: SIL Kit only first, then the targets of the list ("Connecting a network outside with the parts" below) | `network` (a `can` or `ethernet` network of the configuration file)                                                                                                                                                                                                                                                                                                                                                                          |
| `<vsil-outside>`         | One row for each network that the run connected, with its condition and counts                                                                         | none                                                                                                                                                                                                                                                                                                                                                                                                                                         |

A variable is written `ECU.variable` (cut at the first dot), as for `client.variable()`.

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

`<vsil-timeline>` only ties the element to the run: it sets `src` (the run's `timeline_url`, from the server's root) and `hidden` (when the run has no timeline) of the `<vehicle-sil-timeline>` inside it, and nothing else of it. It adds a default element when you put none, and removes only that one. The part does not load the element's script: the page does (`<script src="/timeline/vehicle-sil-timeline.js">`, or `import '@vehicle-sil/gui-client/timeline'`); an element that is defined later works too. The element sends the token that the page keeps in the tab; for a part with another connection (`client`), give the element its `token` property. It sets `data-timeline` (`shown` or `none`) on itself. The attributes and events of the element are in "Putting a timeline on your own screen" in [docs/gui.md](../../docs/gui.md#putting-a-timeline-on-your-own-screen).

### What the parts promise

- **The page's connection.** A part calls `connect()` without options, so the page's own script and all the parts share one connection (one WebSocket), and a refusal of your own button shows in `<vsil-status>`. To give a part another connection, set its `client` property (a part that is in the document binds again).
- **One set of files.** Use the package, the copies of the single files, or the GUI server's `/vsil/` files, not a mix. The parts import the main entry by its file name; they do not carry a copy of it. In a project (the package) the parts and your script both import `@vehicle-sil/gui-client`. With copies, put `vehicle-sil-gui-elements.mjs` next to `vehicle-sil-gui-client.mjs` and import the client from that same file. With the server, load the parts from `/vsil/vehicle-sil-gui-elements.mjs` and the client from `/vsil/vehicle-sil-gui-client.mjs` (the parts' own import resolves to it). A second copy of the library, such as a copy of the client next to the page and the parts from `/vsil/`, makes a second connection.
- **Your markup and your CSS.** There is no shadow DOM. A part draws into one `<span class="vsil">` that it adds, and the children you put in the element stay. It sets `data-*` attributes for CSS: `data-link`, `data-run`, `data-pacing` (`<vsil-status>`), `data-stale`, `data-can-write`, `data-availability` and `data-on` (the variable parts; `data-availability` never disables a part; to dim an unused variable, write `[data-availability="inactive"] { opacity: .45; }`), `data-error` (every part that could not connect, the code of the error when it has one, else `error`; removed when it binds again), `data-status` (`<vsil-scenario-result>` and its steps), `data-network`, `data-chosen` and `data-reverted` (`<vsil-connect>`; its `data-error` is also the code of a list that failed), `data-network`, `data-condition` and `data-nothing-received` (each row of `<vsil-outside>`), and on each button of `<vsil-run-controls>` and `<vsil-time-controls>`, and on the speed select, `data-reason` and `data-caution` (the code, when there is one).
- **A button that cannot be used** is not `disabled`: it has `aria-disabled="true"` (so it keeps the focus), pressing it does nothing, and the reason is a sentence in an element that `aria-describedby` points to (the speed select is described the same way). A reason that only says there is nothing to do (`not-running`, `offline`) is in that element too, but the element is `hidden`: a screen reader reads it, and the page does not repeat it beside every button (`<vsil-status>` shows the state). The same goes for `<vsil-write>` when the variable already has the value ("already false"). A slider that cannot be written, or has no range, is `disabled`, and the reason is next to it.
- **Life.** A part subscribes when it enters the document and stops everything when it leaves. When it leaves while it waits for the connection, it draws nothing. When `variable` or another attribute changes, it binds again. `<vsil-connect>` takes its network out of `pageIntent.connect` when it leaves or its `network` or `file` changes, so that no run is connected by a choice that is not on the page (a part drawn again starts at SIL Kit only).
- **A part that could not connect** (no token, the server is not there) fails together with the other parts of the page, because they share one connection. The page shows the error sentence once: in `<vsil-status>`, or in the first part in document order when the page has no `<vsil-status>` (if that part leaves, another one shows it). Every such part gets `data-error`, so CSS and scripts see the failure; the mark goes when the part binds again and succeeds. A part that was given its own connection in `client`, or that fails while it draws (a missing `variable` attribute), shows its own sentence.
- **A failed action** (an `ActionError`) is not thrown at the page: `status.problem` shows it in `<vsil-status>`, and a failed write shows next to its variable.
- **No more than these 11.** Graphs, tables and the layout are the screen's work. For a graph of a variable over time, keep its history with `client.low.variables.bind(ref, { history: 600 })` and draw the points yourself ("[History](#history)" below).

### Words and the next run

The words are English. Attributes rename the buttons (`label-start` and the others in the table, and `label` of `<vsil-write>`). The sentences of reasons (a button that cannot be used, `caution`, the problem, why a variable cannot be written) come from the owner of the reason, in English; `setReasonText(fn)` replaces them: `fn(reason)` returns the sentence, or `null` or `undefined` to keep the English one, and `setReasonText(null)` removes it. Call it before the parts are in the page. The other words of the parts (`ready`, `writing…`, `(no scenario)`, `fastest`, `behind real time`, `Step (ms)`, `Until (ms)`, `Speed`, `-`, the default `on` and `off`, `SIL Kit only (not connected)`, the sentence of a choice taken back to SIL Kit only, and `link`, `bus`, `to outside`, `from outside` and `nothing received` of `<vsil-outside>`) cannot be changed.

The choosing parts and the start button are separate elements, so the module keeps what the page selected for the next run in `pageIntent` (`get()`, `subscribe(fn)` and `update(patch)`; a field set to `''` or `undefined` is removed). `<vsil-scenario-select>` writes `scenario`; `<vsil-run-controls>` starts with `{ ...pageIntent.get(), ...intent }`, so its `intent` property wins, and it starts the file that `status.file` names, the same file whose scenarios `<vsil-scenario-select>` lists (the server's start-up file when no run has been started). A screen that starts a run with its own code passes the intent to `client.run.start(intent)` itself; it writes the choices that the check sees into `pageIntent` too, so that `<vsil-connect>` checks what the screen will start.

Start the GUI server with a configuration file (`vehicle-sil-gui --web my-screen simulation.yaml`). Without one the server has no start-up file, and a screen made of parts alone cannot start a run: `<vsil-status>` shows the refusal (`no-file`). Only your own code can name the file then: `client.run.start(intent, { file })`.

```ts
import '@vehicle-sil/gui-client/elements'; // defines the parts
import { pageIntent, setReasonText } from '@vehicle-sil/gui-client/elements';

setReasonText((reason) => (reason.code === 'running' ? 'A run is going.' : null));
pageIntent.update({ variant: 'winter' }); // what the next start uses, with the scenario that was chosen
const controls = document.querySelector('vsil-run-controls'); // typed: the parts are in HTMLElementTagNameMap
if (controls) controls.intent = { speed: 'max' };
```

The parts need a DOM, so Node does not load them (the main entry does not import them).

### Connecting a network outside with the parts

`<vsil-connect network="ETH_EXT">` is a plain `<select>`. Its first choice is `none` (SIL Kit only). It asks for the list (`connections()`) only when the user opens it: a press, or a key other than Tab, Shift, Escape and the modifiers alone (passing it by with Tab asks for nothing, and neither does opening the page). It asks again each time, with `aria-busy` while it waits. The targets come in the list's order, under the name of their backend (`<optgroup>`). A target that cannot be used is a `disabled` option whose text is its label, its detail and the launcher's reason; a backend that has no list is a line in the element that describes the select (`aria-describedby`). The choice goes into `pageIntent.connect` (`none` takes the network out). A choice that the page put into `pageIntent` itself is shown as written. The part lists and checks the configuration file that its `file` attribute names (default: the start-up file, as `run.start`): a page that starts another file sets `file` to it.

While the network has a choice, the part checks the page intent (`check()`) about 200 ms after it last changed, whichever part or script changed it; the parts and the screen of a page that check the same intent share one check (`check()` shares it). It shows, as the launcher wrote them, the findings `connect-*` and `outside-*` of its network, the `connect-*` findings of the other networks of `connect` after their name (`CAN1: ...`; a finding about two networks names one of them), and the `outside-*` findings of the run when the check says that this network makes the run keep real time (`time_lock.because`). Each line has `data-code` and `data-severity`. These lines are a guide before the start: what the run does is decided when it starts, by the GUI server and the launcher. A refusal is shown as the server wrote it. After `target-not-listed`, a part that the user has opened lists the network again, once, and checks again if the choice is still listed. When a new list does not offer the choice, or offers it as one that cannot be used, the choice goes back to `none`: a line in the description says which value and why (`'<value>' is not in the list of this computer: back to SIL Kit only`, or `'<value>' cannot be used (<reason>): back to SIL Kit only`), and `data-reverted` holds the value until the user chooses again. The part does not keep a choice from one page load to the next, and it has no choice of `time_policy`: the page sets it with `pageIntent.update({ time_policy: 'free' })`.

`<vsil-outside>` draws the rows of `client.outside` in words (not only in colour; the counts are not a live region, since they change every second), and nothing when there are none.

## ECU variables

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

### List and state (`client.variables.state`)

| `status`     | Meaning                                  |
| ------------ | ---------------------------------------- |
| `unknown`    | Not known yet                            |
| `off`        | This run has no variables                |
| `connecting` | Connecting                               |
| `ready`      | Variables can be read and written        |
| `failed`     | Could not connect, or was refused        |
| `ended`      | The run ended (values are the last ones) |

- For `off`, `connecting` and `failed`, `reason` gives the reason, as text from the server (not localized).
- Before you start a run, `variables` in `client.app` (`available`, `reason`) tells whether variables are available.
- The variable definitions are `peers[].variables[]` (`type`, `role`, `writable`, `unit`, `min`, `max`, `step`, `description`, `enum`).

### Read values (`watch`)

`watch` receives the last values of the given variables (`'all'` for every variable). Always call `close()` when you are done.

**Values**: in `values` (a `Readable`), `values[peer]?.[name]` is a `VariableReading` (`value`, `sampleMs`, `simMs`, `paused`). Each value carries the time it belongs to.

| `paused` | What the value is                                                                                         |
| -------- | --------------------------------------------------------------------------------------------------------- |
| True     | The value at the paused time `simMs` (it includes the values of every step that started before that time) |
| False    | The last value while the simulation runs                                                                  |

**Subscriptions**:

- However many watches look at a variable, the server has one subscription per variable. A variable joins the subscription with the shortest period requested (periods are rounded to one of 20, 50, 100, 200, 500 ms …).
- `close()` also ends the server subscription if no other watch looks at the variable.
- After a reconnection, the library subscribes again automatically.
- A watch continues across runs; the values and history of the previous run are discarded.
- Values are kept only for variables that someone is watching.

**History** (when you ask for `history`):

- For each variable, the library keeps up to that many points (`sampleMs`, `value`) in a ring buffer. The limit per variable is `HISTORY_MAX`; the oldest points are dropped first.
- Memory is allocated as points arrive, not when you ask.
- The history of all variables together holds up to `HISTORY_TOTAL_MAX` points (about 34 MB). Once that is reached, no history grows further: each keeps its current size and drops its oldest points (`history(ref)` returns fewer points than you asked for).
- Values of the same step are one point. If a changed value arrives for the same step, it replaces that point.
- When the run changes, the history starts empty.
- `watch.history(ref)` returns a new array on every call.
- Notifications arrive in batches about every 100 ms (`BATCH_MS`), so `values` notifies at that rate, but the history also holds the values in between.

**When a limit is near**:

- If you list many long names and one subscription message would exceed the server's limit (256 KiB), the library switches the subscription for that period to "all" (and uses only the watched variables from what arrives).
- If the server rejects a watch (a server with different limits), the problem appears in `client.variables.watchProblem`.

### Write values (`write`)

`write(writes, options)` sends the writes to the run that was current when you called it (`session` in `client.variables.state`, sent as `session` in the body).

- The server rejects the writes if that run is no longer running (`session-changed`). A write sent while one run ends and the next starts is therefore never written to the next run.
- If the run is not known yet, the library does not send and rejects with `refused` (`not-running`).
- The server checks type and range, then passes the writes to the launcher.
- The Promise resolves with the operation (`OperationView`) once the writes are applied (`applied`); otherwise it rejects with a `WriteError`.
- The library never resends a write automatically.

The `kind` of a `WriteError`:

| `kind`      | Meaning                                                             | Safe to resend?                                    |
| ----------- | ------------------------------------------------------------------- | -------------------------------------------------- |
| `refused`   | The server did not accept the write (it did not reach the launcher) | Yes, after you fix it                              |
| `failed`    | Accepted, but not applied                                           | Yes, after you fix the reason (it was not applied) |
| `no-answer` | **Unknown whether it was applied**                                  | **Do not resend as is** (see below)                |
| `closed`    | `close()` was called before the result                              | Same as above                                      |

- The `code` of `refused` is `unknown-variable`, `not-writable`, `invalid-field`, `not-running`, `session-changed`, `variables-not-ready`, `write-busy` or similar.
- The `operation` of `failed` holds the state and reason for each participant (`late`, `rejected`, `invalid` and others).
- `no-answer` happens in any of these cases:
  - No result came within `timeoutMs` (`operation` holds the last known state).
  - There was no HTTP response.
  - The server failed midway (500).
  - The server gave up waiting for the Control Server's response (`code` is `write-timeout`) or was disconnected midway (`write-failed`).
  - The run changed before the response.
  - The operation completed during a reconnection and the result was lost ("Reconnection" below).
  - The server could not keep track of all open operations and stopped following this one.

**Avoid writing twice**: after `no-answer`, check before you resend.

- If `error.operation` is set, follow the operation with that number in `client.variables.operations`. If it is `queued`, it is applied once the simulation reaches that step (the wait was only too short).
- If there is no number, look in `operations` for a new operation that looks like your write, and look at the variable's value (watch).
- Resend only once you know that the write was not applied.
- This matters most for writes that add to a value (writing twice gives a different result). Writing the same value twice gives the same result.

**Reconnection**: if the connection drops while you wait for a result, the summary after reconnecting (`hello`, `fetchSession()`) tells how it went on. The summary holds every open operation the server keeps, and every operation whose number is `operations_from_seq` or higher.

| Your operation                                                     | What the library does                                                       |
| ------------------------------------------------------------------ | --------------------------------------------------------------------------- |
| In the list                                                        | Uses its state (if still open, keeps waiting)                               |
| Not in the list, number below `operations_from_seq`                | Treats it as completed while disconnected with its result lost: `no-answer` |
| Not in the list, and the server could not keep all open operations | Sets the old operations to `no-answer`                                      |

- A write that has not been applied yet (for example, a write while paused) never becomes `no-answer` however many operations follow it.
- "The server could not keep all open operations" means more than 256 open operations (`operations_open_omitted` in the summary). The server no longer follows them, so the library reports `no-answer`, and the error message says why.

**Options and related state**:

- `onAccepted` (a function that receives `{session, seq}`) is called once, when the server has received the write and returned the operation number. This is before the result. It is not called when the write is rejected or no response comes.
- Use `onAccepted` to show "accepted, waiting to be applied", and to know when you may send the next write. Writes through `bind` use it: a next value that comes before the server has received the previous write is kept instead of sent, and only the last value is sent once the server has it.
- `timeoutMs` (default `WRITE_TIMEOUT_MS`) counts from the HTTP response. `Infinity` sets no time limit (the write waits until the run changes or `close()` is called). Values above 2^31-1 ms (about 24.8 days) are capped there. Non-numbers and values of 0 or less mean the default.
- A write is applied in the step at the given virtual time (`atMs`; if omitted, the next step), so it does not complete while the simulation is paused. To write while paused, wait for `queued` in `client.variables.operations` before you advance (`examples/gui_client/variables.mjs`).
- `client.variables.operations` lists recent operations, including writes from other clients.

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

| Field            | Contents                                                                                                                            |
| ---------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| `info`           | The variable definition (`undefined` if the current run does not have the variable)                                                 |
| `reading`        | The last value (`VariableReading`; `undefined` if there is none yet or while `visible` is false)                                    |
| `condition`      | The value condition ("Value condition (`condition`)" below)                                                                         |
| `block`          | Why the variable cannot be written now ("Why a write is blocked (`block`)" below)                                                   |
| `launcherReason` | When `block` is `launcher`: the launcher's reason (`code` and an English `message`, as `launcherAnswer` reads it); `null` otherwise |
| `writeStatus`    | The status of the last write ("Write status (`writeStatus`, `pending`)" below)                                                      |
| `pending`        | The value sent and still waiting for its result (`null` if none)                                                                    |

- `subscribe` is called only on changes (get the first state with `state.get()`).
- `get()` always builds the state from the current values. So if you call `get()` during a notification of another `Readable` (such as `client.variables.state`), it may notify this `state`'s subscribers of a new state right there.

### Value condition (`condition`)

Switch on `condition.kind`.

| `kind`        | Meaning                                                                                      |
| ------------- | -------------------------------------------------------------------------------------------- |
| `unavailable` | The variable feature is not available (`status` holds the state of `client.variables.state`) |
| `missing`     | The current run does not have the variable                                                   |
| `waiting`     | The current run has the variable, but no value has arrived yet                               |
| `live`        | The last value while the simulation runs                                                     |
| `paused`      | The value at the paused time                                                                 |
| `stale`       | A value that may be stale (the reason is in `why`)                                           |
| `ended`       | The last value read in a run that ended                                                      |

The `status` of `unavailable` is one of: not running, this run has no variables, connecting, cannot connect. The values of `why` for `stale`:

| `why`          | Meaning                                                  |
| -------------- | -------------------------------------------------------- |
| `connection`   | The connection between the client and the server dropped |
| `reconnecting` | The server is reconnecting to the launcher               |
| `peer`         | The ECU is disconnected                                  |
| `reading`      | Reading values keeps failing                             |

### Why a write is blocked (`block`)

`null` if the variable can be written. If several reasons apply, `block` is the one highest in the table.

The facts of the variable and the run come first, then the launcher's answer (`launcher`), then the client's prediction (`parameter`), then what the widget needs. Only `launcher` comes from the launcher; the other values are the client's own. The client has no rule of its own about what the launcher accepts: when the launcher takes a write during a scenario, `bind` lets it through. It reads the launcher's answer with `launcherAnswer(launcherState, 'write')`, as `controls` of the front door read theirs.

| `block`             | Meaning                                                                                                                                                                                                                                                                   |
| ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `read-only`         | A read-only variable (`role` is `output`)                                                                                                                                                                                                                                 |
| `disconnected`      | The connection between the client and the server is down                                                                                                                                                                                                                  |
| `ended`             | The run ended                                                                                                                                                                                                                                                             |
| `not-ready`         | The variable feature is not available (not running, connecting, cannot connect)                                                                                                                                                                                           |
| `missing`           | The current run does not have the variable                                                                                                                                                                                                                                |
| `peer-disconnected` | The ECU is not connected to the Control Server                                                                                                                                                                                                                            |
| `launcher`          | The launcher does not take a write now (`allowed.write` of its `state`; `launcherReason` has its code and sentence, for example `scenario-running` while a scenario plays), or has not said yet that it does: until a `state` with `allowed.write` arrives, `unsupported` |
| `parameter`         | A `parameter` can be written only before the first step of a run started paused (`isHolding(session)`: `holding` in the launcher's `state`). The client's prediction of the ECU's rule: the launcher does not say it for each variable                                    |
| `no-range`          | `needsRange` is set, but the variable has no range (`min` and `max`)                                                                                                                                                                                                      |
| `no-value`          | `needsValue` is set, and there is no value yet (neither read nor sent)                                                                                                                                                                                                    |

### Write status (`writeStatus`, `pending`)

`writeStatus` is the status of the last write.

| `writeStatus`                              | Meaning                                                        |
| ------------------------------------------ | -------------------------------------------------------------- |
| `idle`                                     | Empty (no last write)                                          |
| `sending`                                  | Sent                                                           |
| `queued`                                   | The server received it (`seq` is the operation number)         |
| `applied`                                  | Applied (`atMs`)                                               |
| `invalid`                                  | The text passed to `reject(message)` (not sent)                |
| `dropped`                                  | A kept value was not sent ("How `write(value)` behaves" below) |
| `refused`, `failed`, `no-answer`, `closed` | The `kind` of a `WriteError` (has `message` and `code`)        |

- `isWriteError(writeStatus)` picks out the statuses that have a `message` (the ones that did not succeed).
- `message` is not localized. To show it in the screen's language, build the text from `kind`.
- If `pending` is set, show it instead of the read value (so a switch you clicked does not appear to flip back while it waits for the result).

### How `write(value)` behaves

- If the variable cannot be written (`block`), it does nothing.
- It returns no Promise; the result appears in `writeStatus`.
- It waits for the operation's result with no time limit (a write while paused stays `queued` until the simulation advances).
- While the server has not yet received the previous write (`sending`), it keeps only the last of the next values instead of sending them, and sends it once the previous write is received (so a dragged slider or repeated keys do not pile up writes).
- Before it sends a kept value, it checks again that the run is the same and the variable is still writable; if not, it does not send and sets `dropped`.
- If the previous write is rejected before it is received, the kept value is not sent either.
- A write that got no result (`no-answer`) is never resent automatically.
- `reject(message)` reports an error that the widget found before sending (not a number, out of range) as `invalid`. It does not touch the write in flight, the kept value or `pending`.

### One state per variable

- However many places `bind` the same variable, they all see the same `writeStatus` and `pending`, and writes do not overlap.
- The state survives closing a `bind` and creating it again.
- The state belongs to the run: when the run changes, it starts empty (`idle`, `null`) and shows no results of writes from the previous run.

### Watch only while visible

`bind` receives values from the moment you create it (as with `watch`, the server has one subscription per variable). On a screen with many widgets, create it with `{ visible: false }`, call `setVisible(true)` when it becomes visible (IntersectionObserver or similar) and `setVisible(false)` when it is hidden. While `visible` is false:

| Field                    | While `visible` is false                                                                            |
| ------------------------ | --------------------------------------------------------------------------------------------------- |
| `reading`                | `undefined`                                                                                         |
| `condition`              | `waiting`                                                                                           |
| `writeStatus`, `pending` | Reported as usual                                                                                   |
| `block`                  | Reported as usual (a `bind` with `needsValue` reports `no-value` unless there is a `pending` value) |

### History

If you create it with `{ history: 600 }`, `history()` returns the points, oldest first. `state` notifies every time a point is added; read the history again there.

### What is not checked, and after `close()`

- `write(value)` sends without checking the value's type and range. The server rejects a value that does not fit, and `writeStatus` becomes `refused`. To check first and show a message, check in the widget and call `reject(message)`.
- After `close()`, `write`, `reject` and `setVisible` do nothing.
- `close()` does not cancel writes already sent or the kept value. The kept value is sent once the previous write is received (the state belongs to the variable, so the next `bind` of the same variable sees it).

## Lifecycle

| Call                                                | What it does                                                                  |
| --------------------------------------------------- | ----------------------------------------------------------------------------- |
| `createClient()`                                    | Only creates the client; does not connect                                     |
| `connect()`                                         | Connects over WebSocket (does nothing without a token)                        |
| `connect()` while connected                         | Does nothing (also while waiting to reconnect)                                |
| `close()`                                           | Disconnects and does not reconnect (what it cleans up is below)               |
| `connect()` after `close()`                         | Connects again and subscribes the remaining watches again                     |
| HTTP requests after `close()`                       | Work (`loadApp()`, `run.start()` and others can be sent without a connection) |
| `connect()` after a rejection or a version mismatch | Does nothing (`session.rejected` and `session.incompatible` do not reset)     |

- `close()` stops pending timers, stops waiting for responses to time commands, and rejects writes waiting for results with a `WriteError` (`closed`). Notifications in a batch being collected are discarded. Variable watches remain.
- After `close()` and `connect()`, the session copy is still there, and the library receives again from where its copy ends.
- To connect with another token or version, create a new client.

## Coding rules

- Import nothing outside this library's own files (`eslint.config.js` blocks it; it also blocks dynamic `import()` except in the test helpers; `rootDir` in `tsconfig.json` blocks anything outside `src/`).
- Do not use anything that exists only in the browser (`window`, `document`, `localStorage` and so on). Take what you need through the options (`ClientOptions`).
- Human-readable text (such as `TimeFailure.message`) is in English. Turning it into the screen's language is the wrapper's job (built from machine-readable fields such as `kind`).
- When you add to the public API, add it to `src/index.ts` and to the "Public API" tables above, and update [Build your own screen for the browser GUI](../../docs/howto/gui-screen.md) to match.

## Tests

The tools (TypeScript, ESLint, Prettier, Vitest, esbuild) are the ones in `node_modules` of `gui/web`. Run these in `gui/web` (`scripts/verify-web.py` runs them too):

| Command                | What it does                                                                                                                                               |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `npm run test:client`  | Type check (`tsc -p ../client`) and unit tests (Vitest; the library runs on Node, without Angular; the parts in `src/elements` run in a headless Chromium) |
| `npm run build:client` | Bundles into one ESM file, and the parts into a second one that imports it (esbuild, the copy that comes with the Angular build tools)                     |
| `npm run lint`         | `gui/web`, and this directory with its `eslint.config.js`                                                                                                  |
| `npm run format:check` | Formatting of `gui/web` and this directory (`gui/.prettierrc`)                                                                                             |

The tests of the parts need Playwright's Chromium (`npx playwright install --only-shell chromium` in `gui/web`; `scripts/doctor.py` says whether it starts). They run headless; to watch them, run `npx vitest --config ../client/vitest.config.mjs --project browser --browser.headless=false` in `gui/web`.

The unit tests feed the examples in `gui/protocol/examples/` instead of building server notifications by hand (`testing/protocol-examples.ts`).

## See also

| To find                                                                                        | Document                                                                                      |
| ---------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------- |
| Which way to add your own code to the GUI: a custom panel, your own screen or a tool           | "Build your own screen" in [docs/gui.md](../../docs/gui.md#build-your-own-screen)             |
| How to build your own screen and have the GUI server serve it                                  | [Build your own screen for the browser GUI](../../docs/howto/gui-screen.md)                   |
| The server API (HTTP and WebSocket shapes, versioning and compatibility rules, authentication) | [gui/protocol](../protocol/README.md)                                                         |
| The rules the library does not enforce for you                                                 | "Rules for building a screen" in [docs/gui.md](../../docs/gui.md#rules-for-building-a-screen) |
| Files to start your own screen from                                                            | The screen template [examples/gui_starter](../../examples/gui_starter/)                       |
| An example that uses the library from Node, without a screen                                   | [examples/gui_client](../../examples/gui_client/)                                             |

`examples/gui_client` exists only in the repository (the prebuilt tools archive contains `gui_starter/` and `client/`).

| File                   | What it does                                               |
| ---------------------- | ---------------------------------------------------------- |
| `run.mjs`              | Calls the server API without the library                   |
| `run-with-library.mjs` | Uses the bundled library file from plain JavaScript (Node) |
| `front.mjs`            | Uses the front door (`connect()`, `status`) from Node      |
| `variables.mjs`        | Also uses the library, to read and write ECU variables     |
