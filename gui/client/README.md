# gui/client

[日本語](README.ja.md)

Reference for the client library of the GUI server (`vehicle-sil-gui`), written in TypeScript and published as `@vehicle-sil/gui-client`. It covers how to get and import the library, the public API, basic usage, reading and writing ECU variables, and the lifecycle.

The library handles the connection (token, reconnection), the session copy (a copy of the run state, built from the server's summary and events), run and time control, ECU variables (list, watching values, writes and their results), and the types and parsers of the server API. It depends on neither Angular nor RxJS, nor on any other package. The sample screens (`gui/web`) use only this library, through a thin Angular wrapper (`gui/web/src/app/core/session/`).

| To find                                                            | See                                                      |
| ------------------------------------------------------------------ | -------------------------------------------------------- |
| Where to get the library and how to import it                      | [Installing and importing](#installing-and-importing)    |
| Which names are public                                             | [Entry points](#entry-points), [Public API](#public-api) |
| The code to connect and start a run                                | [Basic usage](#basic-usage)                              |
| Reading and writing variable values                                | [ECU variables](#ecu-variables)                          |
| Building sliders and lamps (the values of `condition` and `block`) | [Build widgets (`bind`)](#build-widgets-bind)            |
| What happens after `connect()` and `close()`                       | [Lifecycle](#lifecycle)                                  |
| Developing the library                                             | [Coding rules](#coding-rules), [Tests](#tests)           |

## Installing and importing

The library is TypeScript source with no dependencies. It is not published to the npm registry.

| Use                                             | How to get it                                                                                            | Import                                                        |
| ----------------------------------------------- | -------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------- |
| Plain JavaScript, no build step                 | Put `vehicle-sil-gui-client.mjs`, the library bundled into one ESM file, in the screen's directory       | `import { createClient } from './vehicle-sil-gui-client.mjs'` |
| TypeScript and any framework                    | Get the package `vehicle-sil-gui-client-<version>.tgz` and run `npm install <that .tgz>` in your project | `import { createClient } from '@vehicle-sil/gui-client'`      |
| A TypeScript app in this repository (`gui/web`) | Point the entry point names to `src/` with `paths` in `tsconfig.json`                                    | `import { createClient } from '@vehicle-sil/gui-client'`      |

Where to find the 2 files:

| File                                   | Prebuilt tools                                        | Build in the repository (run in `gui/web`)                                            |
| -------------------------------------- | ----------------------------------------------------- | ------------------------------------------------------------------------------------- |
| `vehicle-sil-gui-client.mjs`           | `<tools>/client/vehicle-sil-gui-client.mjs`           | `npm run build:client` creates `build/gui-client/vehicle-sil-gui-client.mjs`          |
| `vehicle-sil-gui-client-<version>.tgz` | `<tools>/client/vehicle-sil-gui-client-<version>.tgz` | `npm run pack:client` creates `build/gui-client/vehicle-sil-gui-client-<version>.tgz` |

The releases page has the same `.tgz`. `build/` is the one at the top of the repository and is not tracked in Git.

- **Bundled file**: loads in the browser and in Node (22 or later). `scripts/verify-gui.py` runs an example that uses this file from Node against the replay launcher.
- **Package**: contains the bundled ESM, `.d.ts` files, `exports`, the README and the LICENSE (`pack.mjs` builds it). Its version is the same as the GUI server's. Its types use the browser types (`DOM` in `lib`). The only entry point is `.` (`testing` and `internal` are not included).

## Entry points

| Entry point            | Name                               | Contents                                                                                |
| ---------------------- | ---------------------------------- | --------------------------------------------------------------------------------------- |
| `src/index.ts`         | `@vehicle-sil/gui-client`          | The public API ("Public API" below). Only this entry point is kept compatible           |
| `src/testing/index.ts` | `@vehicle-sil/gui-client/testing`  | Test helpers (usable in tests of your own widgets)                                      |
| `src/internal.ts`      | `@vehicle-sil/gui-client/internal` | **No compatibility guarantee.** Internal classes and functions; do not use them in apps |

- `testing` contains a fake WebSocket (`FakeWebSockets`), a fake HTTP (`FakeHttp`), loaders for `gui/protocol/examples/`, and `sessionViewFrom`, which builds a `SessionView` from a summary.
- `internal` contains the internal classes, the functions that apply a summary, and the parsers. They are public for tests only, and change or disappear without notice.

The public API starts narrow: adding to it keeps compatibility, removing from it breaks compatibility. If the public entry point lacks something you need, do not reach into `internal`; decide whether to add it.

## Public API

The names that `src/index.ts` exports, by group. Parentheses list the members of the object.

### Client

| Kind     | Name                                                                                                                                                                                                                   |
| -------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Function | `createClient(options)`                                                                                                                                                                                                |
| Type     | `ClientOptions` (`baseUrl`, `token`, `origin`, `http`, `socket`, `answerTimeoutMs`, `onError`)                                                                                                                         |
| Type     | `GuiClient` (`connection`, `session`, `run`, `time`, `variables`, `app`, `limits`, `connect()`, `close()`, `loadApp()`, `configs()`, `check()`, `fetchSession()`, `dashboard(file)`, `scenarios(file)`, `recording()`) |
| Constant | `DEFAULT_LIMITS`                                                                                                                                                                                                       |

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

| Kind     | Name                                                                                                                                |
| -------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| Type     | `ClientSession` (`session`, `ready`, `lastEventAt`, `notices`, `rejected`, `incompatible`, `onMessages`, `report`, `dismissNotice`) |
| Type     | `Notice`, `SessionView`, `ParticipantView`, `LauncherView`, `LogLine`, `LogSource`, `TimeView`, `CommandView`, `RunOutcome`         |
| Function | `runOutcome`, `phaseIndex`                                                                                                          |

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

| Kind     | Name                                                                                                                 |
| -------- | -------------------------------------------------------------------------------------------------------------------- |
| Method   | `client.variables.bind(ref, options)`                                                                                |
| Type     | `BindOptions` (`periodMs`, `history`, `needsRange`, `needsValue`, `visible`)                                         |
| Type     | `VariableBinding` (`ref`, `state`, `history()`, `write(value)`, `reject(message)`, `setVisible(visible)`, `close()`) |
| Type     | `BoundVariable` (`ref`, `info`, `reading`, `condition`, `block`, `writeStatus`, `pending`)                           |
| Type     | `ValueCondition`, `WriteBlock`, `WriteStatus`, `WriteValue`                                                          |
| Function | `isWriteError`, `hasRange`                                                                                           |
| Function | `valueCondition` (`ConditionInput`), `writeBlock` (`WriteBlockInput`)                                                |
| Constant | `DROPPED_MESSAGE`                                                                                                    |

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

| Subject                                                    | Name                                                                                                                                                                                                                            |
| ---------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Types of the `gui/protocol` shapes                         | `SessionSummary`, `ServerMessage`, `LauncherEvent`, `CheckResult`, `RunResult`, `AppInfo` and others                                                                                                                            |
| Constants and functions                                    | `PROTOCOL_VERSION`, `PHASES`, `severityRank`                                                                                                                                                                                    |
| Parsers                                                    | `readServerMessage`, `readAppInfo`, `readConfigList`, `readCheckResponse`, `readSessionResponse`, `readTimeResponse`, `readWriteResponse`, `readVariablesSummary`, `readDashboardResponse`, `readScenarioList`, `readRecording` |
| Shapes of the variable API                                 | `VariablesSummary`, `VariableSummary`, `ValuesMessage`, `OperationReport`, `WatchMessage`, `WriteAnswer`                                                                                                                        |
| Dashboard files (`dashboard(file)`)                        | `DashboardResponse`, `DashboardFile`, `DashboardFileSection`, `DashboardFileItem`, `DashboardFileError`, `DashboardPanelModule`, `DashboardVariable`                                                                            |
| Scenarios (`--scenario`, `scenarios(file)`, `recording()`) | `ScenarioStep`, `ScenarioStepStatus`, `ScenarioStatus`, `CheckScenario`, `ScenarioResult`, `SummaryScenario`, `ScenarioList`, `ScenarioFile`, `Recording`, `FEATURE_SCENARIO`, `FEATURE_SCENARIO_CONTROL`                       |
| Request shapes                                             | `CheckRequest`, `RunRequest`, `Speed`, `StopRequest`, `TimeCommand`                                                                                                                                                             |
| List of configuration files                                | `ConfigList`, `ConfigFile`                                                                                                                                                                                                      |
| Error shape                                                | `ServerProblem`                                                                                                                                                                                                                 |
| Findings                                                   | `Finding`, `Severity`                                                                                                                                                                                                           |
| Participants you start yourself (`manual`)                 | `FEATURE_MANUAL`                                                                                                                                                                                                                |
| `SessionView.scenario` of the session copy                 | `ScenarioView`                                                                                                                                                                                                                  |

## Basic usage

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
await client.run.start({ file: 'simulation.yaml', disable: [], speed: null, start_paused: false });
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
- `client.session.ready` tells whether the session copy is complete (connected, and `hello` and the replay have arrived). Each feature has its own.

### Rejections and timeouts

- If the token is rejected (for example, because the server was restarted), `client.session.rejected` becomes true (WebSocket), and HTTP requests are rejected with an `HttpError` of status 401.
- When an operation is rejected, its Promise rejects with an `HttpError`, and the same reason appears in the state (`client.run.error`, `failure` in `client.time.state`).
- A request waits for its response for up to `DEFAULT_TIMEOUT_MS`. After that it fails with an `HttpError` whose `timedOut` is true (status 0).
- Start (`run.start()`) and check (`check()`) wait up to twice the check limit (`limits.check_timeout_s`) plus `DEFAULT_TIMEOUT_MS`. The server runs checks one at a time, and both the wait in the queue and the check itself can take up to the check limit; a start also checks the configuration file again if it changed.
- A timeout is not a rejection: no response came. For a start, the server may have started the run; the session copy (WebSocket) shows whether it did, and `client.run.error` says so.
- Redirects are not followed.

### Handling unreadable notifications

The library parses each part separately.

- If part of a summary or an event cannot be read, the library uses the readable parts and adds the names of the unreadable parts to `SessionView.unreadable`.
- Unknown notifications and events pass through without changing the state (`client.connection.onBatch` receives everything, before parsing).
- A notification that is not valid JSON raises a notice, and the library receives everything again from `hello`.

### Use from Node, or replace the transport

- In Node, the library sends the server's own origin as `Origin` (`createClient` builds it from `baseUrl`).
- In the browser and in Web Workers, do not pass `origin`: the browser sets it (a Worker's WebSocket cannot set handshake headers).
- To replace the HTTP implementation, pass `http` (`HttpTransport`); to replace the WebSocket, pass `socket` (`SocketFactory`).

For the rules the library does not enforce for you (request fields that match the launcher's `features`, whether a variable is writable, whether a value is stale), see "Rules for building a screen" in [docs/gui.md](../../docs/gui.md#rules-for-building-a-screen).

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

- However many watches look at a variable, the server has one subscription per variable. A variable joins the subscription with the shortest period requested (periods are rounded to the steps 20, 50, 100, 200, 500 ms …).
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

A run started paused (`start_paused`) waits before the first step if the launcher can read and write variables. Meanwhile `beforeStart` in `client.time.state` is true, and you can resume and step (written values are applied in the step at time 0).

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

| Field         | Contents                                                                                         |
| ------------- | ------------------------------------------------------------------------------------------------ |
| `info`        | The variable definition (`undefined` if the current run does not have the variable)              |
| `reading`     | The last value (`VariableReading`; `undefined` if there is none yet or while `visible` is false) |
| `condition`   | The value condition ("Value condition (`condition`)" below)                                      |
| `block`       | Why the variable cannot be written now ("Why a write is blocked (`block`)" below)                |
| `writeStatus` | The status of the last write ("Write status (`writeStatus`, `pending`)" below)                   |
| `pending`     | The value sent and still waiting for its result (`null` if none)                                 |

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

| `block`             | Meaning                                                                                                                |
| ------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| `read-only`         | A read-only variable (`role` is `output`)                                                                              |
| `disconnected`      | The connection between the client and the server is down                                                               |
| `ended`             | The run ended                                                                                                          |
| `not-ready`         | The variable feature is not available (not running, connecting, cannot connect)                                        |
| `missing`           | The current run does not have the variable                                                                             |
| `peer-disconnected` | The ECU is not connected to the Control Server                                                                         |
| `parameter`         | A `parameter` can be written only before the first step of a run started paused (`beforeStart` in `client.time.state`) |
| `scenario`          | A scenario is running (view only)                                                                                      |
| `no-range`          | `needsRange` is set, but the variable has no range (`min` and `max`)                                                   |
| `no-value`          | `needsValue` is set, and there is no value yet (neither read nor sent)                                                 |

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
- Text meant for people (such as `TimeFailure.message`) is in English. Turning it into the screen's language is the wrapper's job (built from machine-readable fields such as `kind`).
- When you add to the public API, add it to `src/index.ts` and to the "Public API" tables above, and update [Build your own screen for the browser GUI](../../docs/howto/gui-screen.md) to match.

## Tests

The tools (TypeScript, ESLint, Prettier, Vitest, esbuild) are the ones in `node_modules` of `gui/web`. Run these in `gui/web` (`scripts/verify-web.py` runs them too):

| Command                | What it does                                                                                    |
| ---------------------- | ----------------------------------------------------------------------------------------------- |
| `npm run test:client`  | Type check (`tsc -p ../client`) and unit tests (Vitest; runs on Node, without Angular or a DOM) |
| `npm run build:client` | Bundles into one ESM file (esbuild, the copy that comes with the Angular build tools)           |
| `npm run lint`         | `gui/web`, and this directory with its `eslint.config.js`                                       |
| `npm run format:check` | Formatting of `gui/web` and this directory (`gui/.prettierrc`)                                  |

The unit tests feed the examples in `gui/protocol/examples/` instead of building server notifications by hand (`testing/protocol-examples.ts`).

## See also

| To find                                                                                        | Document                                                                                      |
| ---------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------- |
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
| `variables.mjs`        | Also uses the library, to read and write ECU variables     |
